#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>

REXCVAR_DEFINE_BOOL(gtav_audio_skip_silent, true, "GTAV",
                    "Skip synth, resampling and mixing of voices whose gains are all zero");
REXCVAR_DEFINE_BOOL(gtav_audio_kill_reverb_buses, false, "GTAV", "Do not feed, process or return the reverb and delay buses");
REXCVAR_DECLARE(bool, gtav_audio_diag);

namespace {
constexpr uint32_t kMixerPtr = 0x83D04410;
constexpr uint32_t kPoolPtr = 0x83D04450;
constexpr uint32_t kRouteTablePtr = 0x8398C2C0;
constexpr uint32_t kReverbVtable = 0x820AC8FC;
constexpr uint32_t kDelayVtable = 0x820AC91C;
constexpr uint32_t kSynthFxVtable = 0x820AC10C;
constexpr uint32_t kBuffers = 4096;
constexpr uint32_t kVoices = 128;

std::array<uint8_t, kBuffers> g_state{};
std::array<uint16_t, kVoices> g_marked{};
uint32_t g_marked_n = 0;
std::array<uint32_t, 8> g_killed{};
uint32_t g_killed_n = 0;
bool g_wave_silent = false;

std::atomic<uint64_t> g_synth_run{0}, g_synth_skip{0}, g_mix_run{0}, g_mix_skip{0}, g_resample_skip{0}, g_feed_skip{0};
std::chrono::steady_clock::time_point g_report = std::chrono::steady_clock::now();

uint32_t L32(const uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a, 4);
  return __builtin_bswap32(v);
}

uint16_t L16(const uint8_t* base, uint32_t a) {
  uint16_t v;
  std::memcpy(&v, base + a, 2);
  return __builtin_bswap16(v);
}

void S32(uint8_t* base, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(base + a, &v, 4);
}

uint32_t Pool(const uint8_t* base) { return L32(base, L32(base, kPoolPtr) + 1040); }

void Count(std::atomic<uint64_t>& counter) {
  if (REXCVAR_GET(gtav_audio_diag)) counter.fetch_add(1, std::memory_order_relaxed);
}

void Report() {
  if (!REXCVAR_GET(gtav_audio_diag)) return;
  const auto now = std::chrono::steady_clock::now();
  const double s = std::chrono::duration<double>(now - g_report).count();
  if (s < 10.0) return;
  g_report = now;
  REXLOG_INFO("[audio-cuts] per second: synth {:.0f} run {:.0f} skipped, voices {:.0f} mixed {:.0f} skipped, "
              "resamples skipped {:.0f}, bus feeds skipped {:.0f}",
              g_synth_run.exchange(0) / s, g_synth_skip.exchange(0) / s, g_mix_run.exchange(0) / s,
              g_mix_skip.exchange(0) / s, g_resample_skip.exchange(0) / s, g_feed_skip.exchange(0) / s);
}

bool VoiceSilent(const uint8_t* base, uint32_t v) {
  for (uint32_t i = 0; i < 8; ++i) {
    const uint16_t id = L16(base, v + 308 + 2 * i);
    if (id == 0xFFFF) continue;
    if (id >= 512) return false;
    uint64_t g[4];
    std::memcpy(g, base + v + 48 + 32 * i, sizeof(g));
    if (g[0] | g[1] | g[2] | g[3]) return false;
  }
  return true;
}

bool Killed(uint32_t submix) {
  for (uint32_t i = 0; i < g_killed_n; ++i)
    if (g_killed[i] == submix) return true;
  return false;
}

bool EmptySubmix(const uint8_t* base, uint32_t sm) {
  if (L32(base, sm + 180) != 0 || L32(base, sm + 164) != 0) return false;
  for (uint32_t k = 0; k < 8; ++k) {
    const uint32_t fx = L32(base, sm + 84 + 4 * k);
    if (!fx) continue;
    if (L32(base, fx) != kSynthFxVtable || (base[fx + 303] && !base[fx + 304])) return false;
  }
  return true;
}

void MarkBuffer(uint32_t buf, bool silent) {
  if (g_state[buf] == 0) {
    g_marked[g_marked_n++] = uint16_t(buf);
    g_state[buf] = silent ? 1 : 2;
    return;
  }
  if (!silent) g_state[buf] = 2;
}

void BuildFrameState(uint8_t* base, uint32_t mixer) {
  for (uint32_t i = 0; i < g_marked_n; ++i) g_state[g_marked[i]] = 0;
  g_marked_n = 0;
  const uint32_t voices = std::min<uint32_t>(L32(base, mixer + 46376), kVoices);
  for (uint32_t i = 0; i < voices; ++i) {
    const uint32_t v = mixer + 6688 + 336 * i;
    const uint32_t buf = L32(base, v + 304);
    if (L32(base, v + 324) != 1 || buf >= kBuffers) continue;
    MarkBuffer(buf, VoiceSilent(base, v));
  }
  g_killed_n = 0;
  if (!REXCVAR_GET(gtav_audio_kill_reverb_buses)) return;
  const uint32_t submixes = L32(base, mixer + 6672);
  for (uint32_t s = 0; s < submixes && g_killed_n < g_killed.size(); ++s) {
    const uint32_t sm = mixer + 16 + 208 * s;
    for (uint32_t k = 0; k < 8; ++k) {
      const uint32_t fx = L32(base, sm + 84 + 4 * k);
      if (fx && (L32(base, fx) == kReverbVtable || L32(base, fx) == kDelayVtable)) {
        g_killed[g_killed_n++] = sm;
        break;
      }
    }
  }
}
}  // namespace

REX_EXTERN(__imp__sub_831C4708);
REX_HOOK_RAW(sub_831C4708) {
  BuildFrameState(base, ctx.r3.u32);
  Report();
  __imp__sub_831C4708(ctx, base);
}

REX_EXTERN(__imp__sub_83215AE8);
REX_HOOK_RAW(sub_83215AE8) {
  const uint32_t src = ctx.r3.u32;
  const uint32_t core = src + 24;
  const uint8_t outs = base[core + 282];
  bool skip = REXCVAR_GET(gtav_audio_skip_silent) && outs && !base[core + 283] && !base[core + 284] &&
              !base[core + 285];
  for (uint32_t i = 0; skip && i < outs; ++i) {
    const uint16_t id = L16(base, src + 4 + 2 * i);
    skip = id < kBuffers && g_state[id] == 1;
  }
  if (!skip) {
    Count(g_synth_run);
    __imp__sub_83215AE8(ctx, base);
    return;
  }
  Count(g_synth_skip);
  const uint32_t pool = Pool(base);
  for (uint32_t i = 0; i < outs; ++i) std::memset(base + pool + (uint32_t(L16(base, src + 4 + 2 * i)) << 10), 0, 1024);
  S32(base, core, L32(base, core) + 1);
}

REX_EXTERN(__imp__sub_831CAF88);
REX_HOOK_RAW(sub_831CAF88) {
  const uint32_t v = ctx.r3.u32;
  if (!REXCVAR_GET(gtav_audio_skip_silent) || L32(base, v + 304) >= 0xFFFF || !VoiceSilent(base, v)) {
    Count(g_mix_run);
    __imp__sub_831CAF88(ctx, base);
    return;
  }
  Count(g_mix_skip);
  const uint32_t mixer = L32(base, kMixerPtr);
  const uint32_t table = L32(base, kRouteTablePtr);
  for (uint32_t i = 0; i < 8; ++i) {
    const uint16_t id = L16(base, v + 308 + 2 * i);
    if (id == 0xFFFF) continue;
    const uint32_t dest = mixer + 16 + 208 * ((L32(base, table + (id + 388) * 8 + 4) >> 6) & 0x3FF);
    if (int32_t(L32(base, dest + 184)) < 0) continue;
    S32(base, dest + 180, L32(base, dest + 180) + 1);
  }
  std::memset(base + v + 16, 0, 32);
}

REX_EXTERN(__imp__sub_831C1108);
REX_HOOK_RAW(sub_831C1108) {
  if (Killed(ctx.r3.u32) || Killed(ctx.r4.u32) ||
      (REXCVAR_GET(gtav_audio_skip_silent) && EmptySubmix(base, ctx.r3.u32))) {
    Count(g_feed_skip);
    return;
  }
  __imp__sub_831C1108(ctx, base);
}

REX_EXTERN(__imp__sub_831BC8B0);
REX_HOOK_RAW(sub_831BC8B0) {
  if (g_killed_n) {
    const uint32_t pool = Pool(base);
    for (uint32_t i = 0; i < g_killed_n; ++i) {
      const uint32_t sm = g_killed[i];
      if (base[sm + 200] && ctx.r4.u32 == pool + (uint32_t(L16(base, sm + 148)) << 10)) {
        Count(g_feed_skip);
        return;
      }
    }
  }
  __imp__sub_831BC8B0(ctx, base);
}

REX_EXTERN(__imp__sub_831C1018);
REX_HOOK_RAW(sub_831C1018) {
  if (Killed(ctx.r3.u32)) {
    base[ctx.r3.u32 + 202] = 1;
    return;
  }
  __imp__sub_831C1018(ctx, base);
}

REX_EXTERN(__imp__sub_831C9008);
REX_HOOK_RAW(sub_831C9008) {
  const uint16_t id = L16(base, ctx.r3.u32 + 4);
  g_wave_silent = REXCVAR_GET(gtav_audio_skip_silent) && id < kBuffers && g_state[id] == 1;
  __imp__sub_831C9008(ctx, base);
  g_wave_silent = false;
}

REX_EXTERN(__imp__sub_831BB3D8);
REX_HOOK_RAW(sub_831BB3D8) {
  if (g_wave_silent) {
    Count(g_resample_skip);
    return;
  }
  __imp__sub_831BB3D8(ctx, base);
}

REX_EXTERN(__imp__sub_831BB508);
REX_HOOK_RAW(sub_831BB508) {
  if (g_wave_silent) {
    Count(g_resample_skip);
    return;
  }
  __imp__sub_831BB508(ctx, base);
}
