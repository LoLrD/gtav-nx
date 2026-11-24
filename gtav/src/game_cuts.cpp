#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#if defined(__APPLE__) || (defined(__linux__) && !defined(__ANDROID__))
#include <sys/resource.h>
#include <time.h>
#define GTAV_GAME_CPU_CLOCK 1
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

REXCVAR_DEFINE_INT32(gtav_anim_every, 1, "GTAV",
                     "Animate peds and objects other than the player every Nth frame, with an N-frame step "
                     "(1 = game)");
REXCVAR_DEFINE_DOUBLE(gtav_physics_max_step, 0.0, "GTAV",
                      "Run the physics once per frame instead of twice while the frame step is at most this many "
                      "seconds (0 = game: two half steps a frame below 60 fps)");
REXCVAR_DEFINE_INT32(gtav_script_every, 1, "GTAV",
                     "Run the scripts named in gtav_script_throttle every Nth frame (1 = game)");
REXCVAR_DEFINE_STRING(gtav_script_throttle, "", "GTAV",
                      "Scripts that gtav_script_every slows down, comma-separated names");
REXCVAR_DEFINE_BOOL(gtav_game_cpu_times, false, "GTAV",
                    "Diagnostic: log the CPU time per frame of the game's main thread systems, worker jobs and "
                    "scripts every 300 frames");
REXCVAR_DEFINE_BOOL(gtav_game_cuts_ab, false, "GTAV",
                    "Diagnostic, with --gtav_game_cpu_times: the game cuts are on and off for alternate 300-frame "
                    "reports (an A/B under the same load)");

namespace gtav {
namespace {

enum Slot {
  kFrame,
  kWorld,
  kEntities,
  kPhysics,
  kScripts,
  kPreRender,
  kCamera,
  kAnimRoot,
  kAnimBlend,
  kAnimClip,
  kAnimMove,
  kAnimCopy,
  kAnimComponents,
  kAnimPhysicsTask,
  kScan,
  kScanEntities,
  kScanSetup,
  kScanJobs,
  kPhysicsTask,
  kPhysicsTask2,
  kGlassTask,
  kGlassTask2,
  kSlots
};

constexpr const char* kSlotNames[kSlots] = {
    "frame",     "world",     "entities",  "physics",   "scripts",   "prerender",  "camera",     "anim.root",
    "anim.blend", "anim.clip", "anim.move", "anim.copy", "anim.comp", "anim.phys",  "scan",       "scan.ents",
    "scan.setup", "scan.jobs", "phys.task", "phys.task2", "glass.task", "glass.task2"};

constexpr uint32_t kReportFrames = 300;
constexpr uint32_t kPlayerInfo = 0x83AAC7E4;
constexpr uint32_t kGameTimeStep = 0x83DDDBF4;
constexpr uint32_t kPhysicsSlices = 0x8390639C;
constexpr uint32_t kScriptHash = 8;
constexpr uint32_t kScriptState = 12;
constexpr uint32_t kScriptName = 104;

std::array<std::atomic<uint64_t>, kSlots> g_ns{};
std::array<std::atomic<uint64_t>, kSlots> g_calls{};
std::atomic<bool> g_cuts_paused{false};
uint32_t g_frames = 0;
bool g_single_slice = false;
std::map<uint32_t, bool> g_script_throttled;
std::map<uint32_t, uint64_t> g_script_ns;
std::map<uint32_t, std::string> g_script_names;

uint32_t L32(const uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a, 4);
  return __builtin_bswap32(v);
}

void S32(uint8_t* base, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(base + a, &v, 4);
}

bool Cut() { return !g_cuts_paused.load(std::memory_order_relaxed); }

uint32_t PlayerPed(const uint8_t* base) {
  const uint32_t info = L32(base, kPlayerInfo);
  return info ? L32(base, info + 4) : 0;
}

void AnimateTimesliced(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  const int32_t every = REXCVAR_GET(gtav_anim_every);
  const uint32_t entity = ctx.r3.u32;
  if (every <= 1 || !Cut() || entity == PlayerPed(base)) {
    original(ctx, base);
    return;
  }
  if ((g_frames + ((entity * 0x9E3779B1u) >> 24)) % uint32_t(every)) {
    ctx.r3.u64 = 0;
    return;
  }
  const uint32_t step = L32(base, kGameTimeStep);
  S32(base, kGameTimeStep, std::bit_cast<uint32_t>(std::bit_cast<float>(step) * float(every)));
  original(ctx, base);
  S32(base, kGameTimeStep, step);
}

bool ScriptThrottled(const uint8_t* base, uint32_t thread) {
  const uint32_t hash = L32(base, thread + kScriptHash);
  if (auto it = g_script_throttled.find(hash); it != g_script_throttled.end()) return it->second;
  const std::string name(reinterpret_cast<const char*>(base + thread + kScriptName));
  const bool throttled =
      !name.empty() && ("," + REXCVAR_GET(gtav_script_throttle) + ",").find("," + name + ",") != std::string::npos;
  if (throttled) REXLOG_INFO("[game-cuts] script {} runs every {} frames", name, REXCVAR_GET(gtav_script_every));
  return g_script_throttled[hash] = throttled;
}

uint64_t ThreadNs() {
#ifdef GTAV_GAME_CPU_CLOCK
  timespec t{};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
  return uint64_t(t.tv_sec) * 1000000000ull + uint64_t(t.tv_nsec);
#else
  return 0;
#endif
}

uint64_t ProcessNs() {
#ifdef GTAV_GAME_CPU_CLOCK
  rusage u{};
  getrusage(RUSAGE_SELF, &u);
  return (uint64_t(u.ru_utime.tv_sec) + uint64_t(u.ru_stime.tv_sec)) * 1000000000ull +
         (uint64_t(u.ru_utime.tv_usec) + uint64_t(u.ru_stime.tv_usec)) * 1000ull;
#else
  return 0;
#endif
}

struct Timed {
  Slot slot;
  uint64_t at;
  explicit Timed(Slot s) : slot(s), at(REXCVAR_GET(gtav_game_cpu_times) ? ThreadNs() : 0) {}
  ~Timed() {
    if (!at) return;
    g_ns[slot].fetch_add(ThreadNs() - at, std::memory_order_relaxed);
    g_calls[slot].fetch_add(1, std::memory_order_relaxed);
  }
};

std::string ScriptReport() {
  std::vector<std::pair<uint64_t, uint32_t>> order;
  for (const auto& [thread, ns] : g_script_ns) order.emplace_back(ns, thread);
  std::sort(order.rbegin(), order.rend());
  std::string line;
  for (size_t i = 0; i < order.size() && i < 16; i++)
    line += fmt::format(" {} {:.2f}", g_script_names[order[i].second], order[i].first / 1e6 / kReportFrames);
  g_script_ns.clear();
  return line;
}

void Report() {
  static uint64_t process_at = ProcessNs();
  static auto wall_at = std::chrono::steady_clock::now();
  const uint64_t process = ProcessNs();
  const auto wall = std::chrono::steady_clock::now();
  std::string line;
  for (int s = 0; s < kSlots; s++) {
    const uint64_t ns = g_ns[s].exchange(0, std::memory_order_relaxed);
    const uint64_t calls = g_calls[s].exchange(0, std::memory_order_relaxed);
    if (calls)
      line += fmt::format(" {} {:.2f}/{:.0f}", kSlotNames[s], ns / 1e6 / kReportFrames, double(calls) / kReportFrames);
  }
  REXLOG_INFO("[game-cpu]{} per frame: {:.1f} fps, process CPU {:.2f} ms | ms/calls:{} | scripts ms:{}",
              REXCVAR_GET(gtav_game_cuts_ab) ? (Cut() ? " (cuts on)" : " (cuts off)") : "",
              kReportFrames / std::chrono::duration<double>(wall - wall_at).count(),
              (process - process_at) / 1e6 / kReportFrames, line, ScriptReport());
  if (REXCVAR_GET(gtav_game_cuts_ab)) g_cuts_paused.store(Cut(), std::memory_order_relaxed);
  process_at = process;
  wall_at = wall;
}

}  // namespace
}  // namespace gtav

#define GTAV_TIMED_HOOK(address, slot) \
  REX_EXTERN(__imp__sub_##address);    \
  REX_HOOK_RAW(sub_##address) {        \
    gtav::Timed timed(gtav::slot);     \
    __imp__sub_##address(ctx, base);   \
  }

#define GTAV_ANIM_HOOK(address)                                \
  REX_EXTERN(__imp__sub_##address);                            \
  REX_HOOK_RAW(sub_##address) { gtav::AnimateTimesliced(ctx, base, __imp__sub_##address); }

GTAV_TIMED_HOOK(8283E9A0, kWorld)
GTAV_TIMED_HOOK(8282C548, kEntities)
GTAV_TIMED_HOOK(8293B258, kScripts)
GTAV_TIMED_HOOK(8231EE68, kPreRender)
GTAV_TIMED_HOOK(82376EB8, kCamera)
GTAV_TIMED_HOOK(834515C8, kAnimRoot)
GTAV_TIMED_HOOK(83446330, kAnimBlend)
GTAV_TIMED_HOOK(8343DE80, kAnimClip)
GTAV_TIMED_HOOK(8344AFF8, kAnimMove)
GTAV_TIMED_HOOK(83449860, kAnimCopy)
GTAV_TIMED_HOOK(834445B0, kAnimComponents)
GTAV_TIMED_HOOK(83412060, kAnimPhysicsTask)
GTAV_TIMED_HOOK(835A0950, kScan)
GTAV_TIMED_HOOK(8359B3F8, kScanEntities)
GTAV_TIMED_HOOK(835A20E0, kScanSetup)
GTAV_TIMED_HOOK(8359C820, kScanJobs)
GTAV_TIMED_HOOK(833CE9D0, kPhysicsTask)
GTAV_TIMED_HOOK(833CB750, kPhysicsTask2)
GTAV_TIMED_HOOK(8316BFA8, kGlassTask)
GTAV_TIMED_HOOK(831618C0, kGlassTask2)

GTAV_ANIM_HOOK(8280FDA0)
GTAV_ANIM_HOOK(827DDE80)
GTAV_ANIM_HOOK(827DDEE8)
GTAV_ANIM_HOOK(82791F00)

REX_EXTERN(__imp__sub_825A2860);
REX_HOOK_RAW(sub_825A2860) {
  gtav::Timed timed(gtav::kPhysics);
  const double max_step = REXCVAR_GET(gtav_physics_max_step);
  gtav::g_single_slice =
      max_step > 0 && gtav::Cut() && std::bit_cast<float>(gtav::L32(base, gtav::kGameTimeStep)) <= max_step;
  if (gtav::g_single_slice) gtav::S32(base, gtav::kPhysicsSlices, 1);
  __imp__sub_825A2860(ctx, base);
  gtav::g_single_slice = false;
}

REX_EXTERN(__imp__sub_835F98E0);
REX_HOOK_RAW(sub_835F98E0) {
  if (gtav::g_single_slice) gtav::S32(base, gtav::kPhysicsSlices, 1);
  __imp__sub_835F98E0(ctx, base);
}

REX_EXTERN(__imp__sub_835273D8);
REX_HOOK_RAW(sub_835273D8) {
  const int32_t every = REXCVAR_GET(gtav_script_every);
  const uint32_t thread = ctx.r3.u32;
  if (every > 1 && gtav::Cut() && (gtav::g_frames + (thread >> 4)) % uint32_t(every) &&
      gtav::ScriptThrottled(base, thread)) {
    ctx.r3.u64 = gtav::L32(base, thread + gtav::kScriptState);
    return;
  }
  __imp__sub_835273D8(ctx, base);
}

REX_EXTERN(__imp__sub_82927BE0);
REX_HOOK_RAW(sub_82927BE0) {
  if (!REXCVAR_GET(gtav_game_cpu_times)) {
    __imp__sub_82927BE0(ctx, base);
    return;
  }
  const uint32_t thread = ctx.r3.u32;
  const uint64_t at = gtav::ThreadNs();
  __imp__sub_82927BE0(ctx, base);
  gtav::g_script_ns[thread] += gtav::ThreadNs() - at;
  gtav::g_script_names[thread] = reinterpret_cast<const char*>(base + thread + gtav::kScriptName);
}

REX_EXTERN(__imp__sub_82235A80);
REX_HOOK_RAW(sub_82235A80) {
  {
    gtav::Timed timed(gtav::kFrame);
    __imp__sub_82235A80(ctx, base);
  }
  if (++gtav::g_frames % gtav::kReportFrames == 0 && REXCVAR_GET(gtav_game_cpu_times)) gtav::Report();
}
