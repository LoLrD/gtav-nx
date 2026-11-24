// gtav - per-second frame and draw statistics.
//
// Milestone 0/1 of the native renderer: confirms the D3D addresses in d3d_map.h (plausible
// per-frame call counts, game behaves the same) and gives a frame-time baseline. Swap time is
// the guest thread blocked in D3D Swap (VdSwap + GPU backpressure), so a high swap share
// means the GPU side is the bottleneck.

#include "stats.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "paths.h"

REXCVAR_DEFINE_BOOL(gtav_native_stats, false, "GTAV", "Log per-second frame time and D3D call counts");
REXCVAR_DEFINE_INT32(gtav_native_timeline, 0, "GTAV",
                     "Write logs/timeline.csv: timestamps of the game's render thread, the renderer, the command "
                     "processor and the GPU fence waits over this many guest frames (0: off)");
REXCVAR_DEFINE_INT32(gtav_native_timeline_from, 1500, "GTAV", "--gtav_native_timeline: the first guest frame");

namespace gtav::native {
namespace {

using Clock = std::chrono::steady_clock;

constexpr const char* kNames[kStatCount] = {"drawidx", "draw", "beginverts", "drawother", "vs",
                                            "ps",      "alu",  "rs",         "ib",        "vb"};

std::atomic<uint64_t> g_counts[kStatCount];

struct FrameStats {
  Clock::time_point window_start{}, last_swap{};
  uint32_t frames = 0;
  double max_ms = 0, swap_ms = 0;
  std::vector<double> intervals;  // frame-to-frame ms this window, for the 1% / 5% lows
};
FrameStats g_frame;  // touched only by the swapping thread

constexpr const char* kTimelineNames[kTlEventCount] = {
    "game_draw", "game_swap", "game_submitted", "record_begin", "record_end",
    "presented", "cp_swap",   "cp_swap_done",   "fence_wait",   "fence_wait_done"};

struct TimelineRecord {
  int64_t ns;
  uint64_t frame;
  uint32_t thread;
  TimelineEvent event;
};
constexpr size_t kTimelineMax = 1 << 16;
std::unique_ptr<TimelineRecord[]> g_timeline;
std::atomic<size_t> g_timeline_next{0};
std::atomic<int> g_timeline_state{0};  // 0 before the first frame, 1 collecting, 2 written
std::atomic<uint32_t> g_timeline_threads{0};

void TimelineWrite(size_t count) {
  const std::string path = LogPath("timeline.csv");
  std::filesystem::create_directories(std::filesystem::path(path).parent_path());
  FILE* f = std::fopen(path.c_str(), "w");
  if (!f) return;
  std::fprintf(f, "ns,event,frame,thread\n");
  for (size_t i = 0; i < count; i++) {
    const TimelineRecord& r = g_timeline[i];
    if (!r.ns) continue;
    std::fprintf(f, "%lld,%s,%llu,%u\n", (long long)r.ns, kTimelineNames[r.event], (unsigned long long)r.frame,
                 r.thread);
  }
  std::fclose(f);
  REXLOG_INFO("[native] timeline: {} events written to {}", count, path);
}

}  // namespace

void TimelineMark(TimelineEvent event, uint64_t frame) {
  static const int32_t frames = REXCVAR_GET(gtav_native_timeline);
  if (frames <= 0) return;
  static const uint64_t from = uint64_t(std::max(0, REXCVAR_GET(gtav_native_timeline_from)));
  int state = g_timeline_state.load(std::memory_order_acquire);
  if (state == 0) {
    if (event != kTlGameSwap || frame + 1 < from) return;
    g_timeline.reset(new TimelineRecord[kTimelineMax]());
    g_timeline_state.store(1, std::memory_order_release);
  } else if (state != 1) {
    return;
  }
  if (event == kTlGameSwap && frame >= from + uint64_t(frames)) {
    if (g_timeline_state.exchange(2) == 1) TimelineWrite(std::min(g_timeline_next.load(), kTimelineMax));
    return;
  }
  thread_local const uint32_t thread = g_timeline_threads.fetch_add(1) + 1;
  const size_t at = g_timeline_next.fetch_add(1, std::memory_order_relaxed);
  if (at >= kTimelineMax) return;
  TimelineRecord& r = g_timeline[at];
  r.frame = frame;
  r.thread = thread;
  r.event = event;
  r.ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

bool StatsEnabled() {
  static const bool enabled = REXCVAR_GET(gtav_native_stats);
  return enabled;
}

void StatCount(StatCounter c) {
  if (StatsEnabled()) g_counts[c].store(g_counts[c].load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

void StatsSwap(Clock::time_point begin, Clock::time_point end, uint32_t device) {
  auto& f = g_frame;
  if (f.frames == 0 && f.window_start == Clock::time_point{}) f.window_start = begin;
  if (f.last_swap != Clock::time_point{}) {
    double ms = std::chrono::duration<double, std::milli>(end - f.last_swap).count();
    f.max_ms = std::max(f.max_ms, ms);
    f.intervals.push_back(ms);
  }
  f.last_swap = end;
  f.swap_ms += std::chrono::duration<double, std::milli>(end - begin).count();
  f.frames++;
  double window = std::chrono::duration<double>(end - f.window_start).count();
  if (window < 1.0) return;
  uint64_t c[kStatCount];
  for (int i = 0; i < kStatCount; i++) c[i] = g_counts[i].exchange(0, std::memory_order_relaxed);
  std::string per_frame;
  for (int i = 0; i < kStatCount; i++) per_frame += fmt::format(" {}={}", kNames[i], c[i] / f.frames);
  // p99 / p95 frame intervals: the 1% and 5% lows of this window (1000 / p99 = "1% low fps").
  double p99 = 0, p95 = 0;
  if (!f.intervals.empty()) {
    std::sort(f.intervals.begin(), f.intervals.end());
    p99 = f.intervals[std::min(f.intervals.size() - 1, size_t(f.intervals.size() * 99 / 100))];
    p95 = f.intervals[std::min(f.intervals.size() - 1, size_t(f.intervals.size() * 95 / 100))];
  }
  REXLOG_INFO("[native] fps={:.1f} avg={:.2f}ms max={:.2f}ms p99={:.2f}ms p95={:.2f}ms swap={:.2f}ms/frame dev=0x{:08X}{}",
              f.frames / window, window * 1000.0 / f.frames, f.max_ms, p99, p95, f.swap_ms / f.frames, device,
              per_frame);
  f = FrameStats{};
  f.window_start = end;
  f.last_swap = end;
}

}  // namespace gtav::native
