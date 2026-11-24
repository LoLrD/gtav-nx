#include <rex/filesystem.h>
#include <rex/logging.h>

#include <sys/stat.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

extern "C" void __gcov_dump(void);
extern "C" void __gcov_reset(void);
extern "C" [[noreturn]] void __real___libnx_exit(int rc);

namespace {

constexpr int kPeriodSeconds = 300;
constexpr int kStartDelaySeconds = 30;

std::once_flag g_prepared;
std::mutex g_dump_mutex;
int g_dumps = 0;

const std::string& Dir() {
  static const std::string dir = (rex::filesystem::GetExecutableFolder() / "pgo").string();
  return dir;
}

void Prepare() {
  if (mkdir(Dir().c_str(), 0777) != 0 && errno != EEXIST) {
    REXLOG_ERROR("[pgo] cannot create {}: {}", Dir(), std::strerror(errno));
  }
  setenv("GCOV_PREFIX", Dir().c_str(), 1);
  setenv("GCOV_PREFIX_STRIP", "1", 1);
  REXLOG_INFO("[pgo] instrumented build: profile goes to {} every {} s, on {}/dump.flag and on exit", Dir(),
              kPeriodSeconds, Dir());
}

void Dump(const char* reason) {
  std::call_once(g_prepared, Prepare);
  std::lock_guard lock(g_dump_mutex);
  const auto start = std::chrono::steady_clock::now();
  __gcov_dump();
  __gcov_reset();
  const std::chrono::duration<double> took = std::chrono::steady_clock::now() - start;
  REXLOG_INFO("[pgo] profile #{} written to {} ({}, {:.1f} s)", ++g_dumps, Dir(), reason, took.count());
  rex::FlushLogging();
}

void Run() {
  std::this_thread::sleep_for(std::chrono::seconds(kStartDelaySeconds));
  std::call_once(g_prepared, Prepare);
  const std::string flag = Dir() + "/dump.flag";
  bool answered = false;
  int idle = 0;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    struct stat st;
    if (stat(flag.c_str(), &st) == 0) {
      if (!answered) {
        Dump("requested");
        answered = true;
        idle = 0;
      }
      if (std::remove(flag.c_str()) == 0) {
        answered = false;
      }
      continue;
    }
    if (++idle >= kPeriodSeconds) {
      Dump("periodic");
      idle = 0;
    }
  }
}

const bool g_started = [] {
  std::thread(Run).detach();
  return true;
}();

}  // namespace

extern "C" [[noreturn]] void __wrap___libnx_exit(int rc) {
  Dump("exit");
  __real___libnx_exit(rc);
}
