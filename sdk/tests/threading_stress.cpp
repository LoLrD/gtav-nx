#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/thread.h>

namespace {

using namespace std::chrono_literals;
using rex::thread::WaitResult;

std::atomic<int> g_failures{0};
std::atomic<const char*> g_current{"start"};

void Check(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL [%s]: %s\n", g_current.load(), what);
    g_failures.fetch_add(1);
  }
}

void Watchdog(std::chrono::seconds limit) {
  std::thread([limit] {
    std::this_thread::sleep_for(limit);
    std::fprintf(stderr, "HANG in [%s] after %lld s\n", g_current.load(), static_cast<long long>(limit.count()));
    std::fflush(stderr);
    std::_Exit(2);
  }).detach();
}

std::chrono::milliseconds RandomTimeout(std::mt19937& rng) {
  switch (rng() % 5) {
    case 0:
      return 0ms;
    case 1:
      return 1ms;
    case 2:
      return 3ms;
    case 3:
      return 20ms;
    default:
      return std::chrono::milliseconds::max();
  }
}

std::chrono::milliseconds BoundedTimeout(std::mt19937& rng, std::chrono::milliseconds cap) {
  return std::min(RandomTimeout(rng), cap);
}

void SemaphoreProducersConsumers(int producers, int consumers, int tokens_per_producer) {
  g_current = "semaphore producers/consumers";
  auto sem = rex::thread::Semaphore::Create(0, 1 << 20);
  const int total = producers * tokens_per_producer;
  std::atomic<int> consumed{0};
  std::atomic<int> released{0};
  std::vector<std::thread> threads;
  for (int p = 0; p < producers; ++p) {
    threads.emplace_back([&, p] {
      std::mt19937 rng(p * 7919u + 1);
      int left = tokens_per_producer;
      while (left > 0) {
        const int n = std::min(left, int(rng() % 3) + 1);
        int previous = -1;
        Check(sem->Release(n, &previous), "release within the maximum");
        Check(previous >= 0, "previous count reported");
        released.fetch_add(n);
        left -= n;
        if (rng() % 8 == 0) {
          std::this_thread::yield();
        }
      }
    });
  }
  for (int c = 0; c < consumers; ++c) {
    threads.emplace_back([&, c] {
      std::mt19937 rng(c * 104729u + 3);
      while (consumed.load() < total) {
        const auto timeout = BoundedTimeout(rng, 50ms);
        const WaitResult r = rex::thread::Wait(sem.get(), false, timeout);
        if (r == WaitResult::kSuccess) {
          consumed.fetch_add(1);
        } else {
          Check(r == WaitResult::kTimeout, "semaphore wait times out or succeeds");
        }
      }
    });
  }
  for (auto& t : threads) {
    t.join();
  }
  Check(consumed.load() == total, "every released token consumed exactly once");
  Check(rex::thread::Wait(sem.get(), false, 0ms) == WaitResult::kTimeout, "semaphore empty at the end");
  int previous = -1;
  Check(!sem->Release(1 << 20, &previous) || previous == 0, "count never went negative");
}

void SemaphoreLimit() {
  g_current = "semaphore limit";
  auto sem = rex::thread::Semaphore::Create(2, 3);
  int previous = -1;
  Check(sem->Release(1, &previous) && previous == 2, "release to the maximum");
  Check(!sem->Release(1, &previous), "release above the maximum fails");
  for (int i = 0; i < 3; ++i) {
    Check(rex::thread::Wait(sem.get(), false, 0ms) == WaitResult::kSuccess, "take a token");
  }
  Check(rex::thread::Wait(sem.get(), false, 0ms) == WaitResult::kTimeout, "no fourth token");
}

void EventPingPong(int pairs, int rounds) {
  g_current = "auto-reset event ping-pong";
  std::vector<std::thread> threads;
  for (int p = 0; p < pairs; ++p) {
    auto ping = std::shared_ptr<rex::thread::Event>(rex::thread::Event::CreateAutoResetEvent(false));
    auto pong = std::shared_ptr<rex::thread::Event>(rex::thread::Event::CreateAutoResetEvent(false));
    threads.emplace_back([=] {
      for (int i = 0; i < rounds; ++i) {
        ping->Set();
        Check(rex::thread::Wait(pong.get(), false) == WaitResult::kSuccess, "pong");
      }
    });
    threads.emplace_back([=] {
      for (int i = 0; i < rounds; ++i) {
        Check(rex::thread::Wait(ping.get(), false) == WaitResult::kSuccess, "ping");
        pong->Set();
      }
    });
  }
  for (auto& t : threads) {
    t.join();
  }
}

void AutoResetSingleConsumption(int waiters, int rounds, bool with_multi) {
  g_current = with_multi ? "auto-reset event, single and WaitAny waiters" : "auto-reset event, single waiters";
  auto event = rex::thread::Event::CreateAutoResetEvent(false);
  auto never = rex::thread::Event::CreateManualResetEvent(false);
  auto ack = rex::thread::Semaphore::Create(0, 1 << 20);
  std::atomic<bool> stop{false};
  std::atomic<int> got{0};
  std::vector<std::thread> threads;
  for (int w = 0; w < waiters; ++w) {
    threads.emplace_back([&, w] {
      std::mt19937 rng(w * 31u + 11);
      while (!stop.load()) {
        const auto timeout = BoundedTimeout(rng, 50ms);
        bool ok;
        if (with_multi && (w % 2 == 1)) {
          rex::thread::WaitHandle* handles[] = {never.get(), event.get()};
          auto r = rex::thread::WaitAny(handles, 2, false, timeout);
          ok = r.first == WaitResult::kSuccess;
          if (ok) {
            Check(r.second == 1, "WaitAny reports the signalled handle");
          }
        } else {
          ok = rex::thread::Wait(event.get(), false, timeout) == WaitResult::kSuccess;
        }
        if (ok) {
          got.fetch_add(1);
          ack->Release(1, nullptr);
        }
      }
    });
  }
  for (int i = 0; i < rounds; ++i) {
    event->Set();
    Check(rex::thread::Wait(ack.get(), false, 10s) == WaitResult::kSuccess, "a waiter took the event");
  }
  stop = true;
  for (auto& t : threads) {
    t.join();
  }
  Check(got.load() == rounds, "one consumption per signalled state");
  Check(rex::thread::Wait(event.get(), false, 0ms) == WaitResult::kTimeout, "event reset after the last wait");
}

void AutoResetMerge() {
  g_current = "auto-reset event without waiters";
  auto event = rex::thread::Event::CreateAutoResetEvent(false);
  event->Set();
  event->Set();
  Check(rex::thread::Wait(event.get(), false, 0ms) == WaitResult::kSuccess, "set event satisfies one wait");
  Check(rex::thread::Wait(event.get(), false, 0ms) == WaitResult::kTimeout, "two Sets without waiters are one");
  event->Set();
  event->Reset();
  Check(rex::thread::Wait(event.get(), false, 0ms) == WaitResult::kTimeout, "reset clears the event");
}

void AutoResetEachSetReleasesWaiter(int rounds, bool required) {
  g_current = "auto-reset event, Set per blocked waiter";
  auto event = rex::thread::Event::CreateAutoResetEvent(false);
  int both = 0;
  for (int i = 0; i < rounds; ++i) {
    std::atomic<int> passed{0};
    std::atomic<int> started{0};
    std::thread a([&] {
      started.fetch_add(1);
      passed.fetch_add(rex::thread::Wait(event.get(), false, 2s) == WaitResult::kSuccess);
    });
    std::thread b([&] {
      started.fetch_add(1);
      passed.fetch_add(rex::thread::Wait(event.get(), false, 2s) == WaitResult::kSuccess);
    });
    while (started.load() < 2) {
      std::this_thread::yield();
    }
    std::this_thread::sleep_for(5ms);
    event->Set();
    event->Set();
    a.join();
    b.join();
    both += passed.load() == 2;
  }
  std::fprintf(stderr, "  two Sets released both blocked waiters %d of %d times\n", both, rounds);
  if (required) {
    Check(both == rounds, "two Sets release two blocked waiters");
  }
}

void ManualResetBroadcast(int waiters, int rounds) {
  g_current = "manual-reset event broadcast";
  auto gate = rex::thread::Event::CreateManualResetEvent(false);
  auto done = rex::thread::Semaphore::Create(0, 1 << 20);
  std::vector<std::unique_ptr<rex::thread::Event>> next;
  for (int w = 0; w < waiters; ++w) {
    next.push_back(rex::thread::Event::CreateAutoResetEvent(false));
  }
  std::vector<std::thread> threads;
  for (int w = 0; w < waiters; ++w) {
    threads.emplace_back([&, w] {
      for (int i = 0; i < rounds; ++i) {
        Check(rex::thread::Wait(gate.get(), false) == WaitResult::kSuccess, "gate opens");
        Check(rex::thread::Wait(gate.get(), false, 0ms) == WaitResult::kSuccess, "manual event stays set");
        done->Release(1, nullptr);
        Check(rex::thread::Wait(next[w].get(), false) == WaitResult::kSuccess, "next round");
      }
    });
  }
  for (int i = 0; i < rounds; ++i) {
    gate->Set();
    for (int w = 0; w < waiters; ++w) {
      Check(rex::thread::Wait(done.get(), false, 10s) == WaitResult::kSuccess, "every waiter passed the gate");
    }
    gate->Reset();
    Check(rex::thread::Wait(gate.get(), false, 0ms) == WaitResult::kTimeout, "reset closes the gate");
    for (auto& event : next) {
      event->Set();
    }
  }
  for (auto& t : threads) {
    t.join();
  }
}

void WaitAllPairs(int consumers, int pairs) {
  g_current = "WaitAll on two semaphores";
  auto a = rex::thread::Semaphore::Create(0, 1 << 20);
  auto b = rex::thread::Semaphore::Create(0, 1 << 20);
  std::atomic<int> taken{0};
  std::vector<std::thread> threads;
  for (int c = 0; c < consumers; ++c) {
    threads.emplace_back([&, c] {
      std::mt19937 rng(c * 17u + 5);
      while (taken.load() < pairs) {
        rex::thread::WaitHandle* handles[] = {a.get(), b.get()};
        const auto timeout = BoundedTimeout(rng, 20ms);
        if (rex::thread::WaitAll(handles, 2, false, timeout) == WaitResult::kSuccess) {
          taken.fetch_add(1);
        }
      }
    });
  }
  std::thread producer_a([&] {
    for (int i = 0; i < pairs; ++i) {
      a->Release(1, nullptr);
    }
  });
  std::thread producer_b([&] {
    for (int i = 0; i < pairs; ++i) {
      b->Release(1, nullptr);
      if (i % 64 == 0) {
        std::this_thread::sleep_for(100us);
      }
    }
  });
  producer_a.join();
  producer_b.join();
  for (auto& t : threads) {
    t.join();
  }
  Check(taken.load() == pairs, "one WaitAll per released pair");
  Check(rex::thread::Wait(a.get(), false, 0ms) == WaitResult::kTimeout, "first semaphore empty");
  Check(rex::thread::Wait(b.get(), false, 0ms) == WaitResult::kTimeout, "second semaphore empty");
}

void MutantExclusion(int threads_count, int iterations) {
  g_current = "mutant mutual exclusion";
  auto mutant = rex::thread::Mutant::Create(false);
  int counter = 0;
  std::atomic<int> inside{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < threads_count; ++t) {
    threads.emplace_back([&] {
      for (int i = 0; i < iterations; ++i) {
        Check(rex::thread::Wait(mutant.get(), false) == WaitResult::kSuccess, "acquire mutant");
        Check(rex::thread::Wait(mutant.get(), false, 0ms) == WaitResult::kSuccess, "recursive acquire");
        Check(inside.fetch_add(1) == 0, "one owner at a time");
        ++counter;
        inside.fetch_sub(1);
        Check(mutant->Release(), "release recursion");
        Check(mutant->Release(), "release mutant");
      }
    });
  }
  for (auto& t : threads) {
    t.join();
  }
  Check(counter == threads_count * iterations, "every increment under the mutant");
}

void Timeouts() {
  g_current = "timeouts";
  auto event = rex::thread::Event::CreateAutoResetEvent(false);
  auto other = rex::thread::Event::CreateAutoResetEvent(false);
  for (auto timeout : {1ms, 5ms, 20ms}) {
    const auto start = std::chrono::steady_clock::now();
    Check(rex::thread::Wait(event.get(), false, timeout) == WaitResult::kTimeout, "single wait times out");
    const auto elapsed = std::chrono::steady_clock::now() - start;
    Check(elapsed >= timeout, "single wait lasts its timeout");
    Check(elapsed < timeout + 500ms, "single wait ends soon after its timeout");
    rex::thread::WaitHandle* handles[] = {event.get(), other.get()};
    const auto start_multi = std::chrono::steady_clock::now();
    Check(rex::thread::WaitAny(handles, 2, false, timeout).first == WaitResult::kTimeout, "WaitAny times out");
    Check(std::chrono::steady_clock::now() - start_multi >= timeout, "WaitAny lasts its timeout");
  }
  std::thread setter([&] {
    std::this_thread::sleep_for(30ms);
    event->Set();
  });
  const auto start = std::chrono::steady_clock::now();
  Check(rex::thread::Wait(event.get(), false, 5s) == WaitResult::kSuccess, "late Set ends a long wait");
  Check(std::chrono::steady_clock::now() - start < 2s, "late Set wakes the waiter promptly");
  setter.join();
}

void PulseReleasesWaiter(int rounds) {
  g_current = "pulse";
  auto event = rex::thread::Event::CreateManualResetEvent(false);
  int woken = 0;
  for (int i = 0; i < rounds; ++i) {
    std::atomic<bool> waiting{false};
    std::atomic<bool> passed{false};
    std::thread waiter([&] {
      waiting = true;
      passed = rex::thread::Wait(event.get(), false, 2s) == WaitResult::kSuccess;
    });
    while (!waiting.load()) {
      std::this_thread::yield();
    }
    std::this_thread::sleep_for(2ms);
    event->Pulse();
    waiter.join();
    woken += passed.load() ? 1 : 0;
    Check(rex::thread::Wait(event.get(), false, 0ms) == WaitResult::kTimeout, "pulse leaves the event reset");
  }
  std::fprintf(stderr, "  pulse released the waiter %d of %d times\n", woken, rounds);
}

void AlertableWaits(int rounds) {
  g_current = "alertable waits and user callbacks";
  auto never = rex::thread::Event::CreateManualResetEvent(false);
  auto never2 = rex::thread::Event::CreateManualResetEvent(false);
  auto ready = rex::thread::Semaphore::Create(0, 1 << 20);
  std::atomic<int> callbacks{0};
  std::atomic<int> results[3] = {};
  rex::thread::Thread::CreationParameters params;
  auto target = rex::thread::Thread::Create(params, [&] {
    for (int i = 0; i < rounds; ++i) {
      ready->Release(1, nullptr);
      WaitResult r;
      if (i % 3 == 0) {
        r = rex::thread::Wait(never.get(), true);
      } else if (i % 3 == 1) {
        rex::thread::WaitHandle* handles[] = {never.get(), never2.get()};
        r = rex::thread::WaitAny(handles, 2, true).first;
      } else {
        r = rex::thread::AlertableSleep(10s) == rex::thread::SleepResult::kAlerted ? WaitResult::kUserCallback
                                                                                   : WaitResult::kTimeout;
      }
      Check(r == WaitResult::kUserCallback, "a queued callback ends the alertable wait");
      results[i % 3].fetch_add(1);
    }
  });
  Check(target != nullptr, "thread created");
  for (int i = 0; i < rounds; ++i) {
    Check(rex::thread::Wait(ready.get(), false, 10s) == WaitResult::kSuccess, "target ready");
    std::this_thread::sleep_for(std::chrono::microseconds(200 + (i % 5) * 300));
    target->QueueUserCallback([&] { callbacks.fetch_add(1); });
  }
  Check(rex::thread::Wait(target.get(), false, 30s) == WaitResult::kSuccess, "thread exit signals its handle");
  Check(callbacks.load() == rounds, "every callback ran");
}

void ThreadHandles(int count) {
  g_current = "thread handles";
  std::vector<std::unique_ptr<rex::thread::Thread>> threads;
  std::atomic<int> ran{0};
  rex::thread::Thread::CreationParameters params;
  for (int i = 0; i < count; ++i) {
    threads.push_back(rex::thread::Thread::Create(params, [&, i] {
      std::this_thread::sleep_for(std::chrono::microseconds(100 * (i % 7)));
      ran.fetch_add(1);
    }));
  }
  std::vector<rex::thread::WaitHandle*> handles;
  for (auto& t : threads) {
    handles.push_back(t.get());
  }
  Check(rex::thread::WaitAll(handles, false, 30s) == WaitResult::kSuccess, "WaitAll on thread handles");
  Check(ran.load() == count, "every thread ran");
}

std::string g_filter;

template <typename F>
void Step(const char* name, F&& f) {
  if (!g_filter.empty() && std::string(name).find(g_filter) == std::string::npos) {
    return;
  }
  const auto start = std::chrono::steady_clock::now();
  f();
  std::fprintf(stderr, "  %-48s %6.2f s\n", g_current.load(),
               std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
}

void RunAll(bool address_waits, int scale) {
  rex::cvar::SetFlagByName("thread_address_waits", address_waits ? "true" : "false");
  std::fprintf(stderr, "== thread_address_waits = %s\n", address_waits ? "true" : "false");
  Step("semaphore limit", [] { SemaphoreLimit(); });
  Step("semaphore producers", [=] { SemaphoreProducersConsumers(4, 8, 5000 * scale); });
  Step("semaphore producers", [=] { SemaphoreProducersConsumers(1, 3, 20000 * scale); });
  Step("ping-pong", [=] { EventPingPong(4, 5000 * scale); });
  Step("auto-reset merge", [] { AutoResetMerge(); });
  Step("auto-reset per waiter", [=] { AutoResetEachSetReleasesWaiter(20 * scale, address_waits); });
  Step("auto-reset consumption", [=] { AutoResetSingleConsumption(6, 3000 * scale, false); });
  Step("auto-reset consumption", [=] { AutoResetSingleConsumption(6, 3000 * scale, true); });
  Step("manual broadcast", [=] { ManualResetBroadcast(8, 300 * scale); });
  Step("wait all", [=] { WaitAllPairs(4, 3000 * scale); });
  Step("mutant", [=] { MutantExclusion(6, 3000 * scale); });
  Step("timeouts", [] { Timeouts(); });
  Step("pulse", [] { PulseReleasesWaiter(50); });
  Step("alertable", [=] { AlertableWaits(150 * scale); });
  Step("thread handles", [] { ThreadHandles(32); });
}

}  // namespace

int main(int argc, char** argv) {
  rex::cvar::Init(argc, argv);
  const int scale = argc > 1 ? std::max(1, std::atoi(argv[1])) : 1;
  const int passes = argc > 2 ? std::max(1, std::atoi(argv[2])) : 1;
  g_filter = argc > 3 ? argv[3] : "";
  Watchdog(std::chrono::seconds(600 * scale * passes));
  for (int i = 0; i < passes; ++i) {
    RunAll(false, scale);
    RunAll(true, scale);
  }
  std::fprintf(stderr, "%s: %d failures\n", g_failures.load() ? "FAILED" : "OK", g_failures.load());
  return g_failures.load() ? 1 : 0;
}
