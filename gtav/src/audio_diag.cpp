#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <atomic>
#include <chrono>

REXCVAR_DEFINE_BOOL(gtav_audio_diag, false, "GTAV", "Log how often the game's audio mixer and filter run");

namespace {
std::atomic<uint64_t> g_mix{0}, g_filter{0}, g_mix_silent{0};
std::chrono::steady_clock::time_point g_last = std::chrono::steady_clock::now();

void Report() {
  const auto now = std::chrono::steady_clock::now();
  const double s = std::chrono::duration<double>(now - g_last).count();
  if (s < 10.0) return;
  g_last = now;
  REXLOG_INFO("[audio-dsp] per second: mix {:.0f} ({:.0f} with zero gain), filter {:.0f}", g_mix.exchange(0) / s,
              g_mix_silent.exchange(0) / s, g_filter.exchange(0) / s);
}
}  // namespace

REX_EXTERN(__imp__sub_831B5F98);
REX_HOOK_RAW(sub_831B5F98) {
  bool silent = true;
  for (int i = 0; i < 4; ++i) silent &= ctx.v1.u32[i] == 0 && ctx.v2.u32[i] == 0;
  if (REXCVAR_GET(gtav_audio_diag)) {
    g_mix.fetch_add(1, std::memory_order_relaxed);
    if (silent) g_mix_silent.fetch_add(1, std::memory_order_relaxed);
    Report();
  }
  if (!silent) {
    __imp__sub_831B5F98(ctx, base);
    return;
  }
  ctx.v0 = ctx.v1;
  ctx.r9.u64 = 16;
  ctx.r10.u64 = ctx.r3.u64 + 256;
  ctx.r11.u64 = ctx.r4.u64 + 1024;
  ctx.ctr.u64 = 0;
}

REX_EXTERN(__imp__sub_8320CF78);
REX_HOOK_RAW(sub_8320CF78) {
  if (REXCVAR_GET(gtav_audio_diag)) g_filter.fetch_add(1, std::memory_order_relaxed);
  __imp__sub_8320CF78(ctx, base);
}
