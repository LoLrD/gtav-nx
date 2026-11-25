#include <rex/audio/xma/decoder.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

#if defined(__APPLE__)
#include <pthread.h>
#include <time.h>
#endif

REXCVAR_DEFINE_BOOL(gtav_audio_skip_silent, true, "GTAV",
                    "Skip synth, resampling and mixing of voices whose gains are all zero");
REXCVAR_DEFINE_BOOL(gtav_audio_kill_reverb_buses, false, "GTAV", "Do not feed, process or return the reverb and delay buses");
REXCVAR_DEFINE_INT32(gtav_audio_quiet_xma, 0, "GTAV",
                     "A wave player whose voices have had zero gain for this many mixer frames (5.3 ms each) "
                     "has its XMA parsed but not decoded (silence, the stream keeps its position); 0 = always decode");
REXCVAR_DEFINE_INT32(gtav_audio_quiet_loop_every, 0, "GTAV",
                     "A wave player kept quiet by gtav_audio_quiet_xma whose XMA loops forever runs only once in this "
                     "many mixer frames (its output stays silent, its loop position advances slower); 0 = every frame");
REXCVAR_DEFINE_INT32(gtav_audio_controller_ms, 0, "GTAV",
                     "Minimum time between two updates of the audio controller thread (sounds, categories, voice "
                     "requests); 0 = the game's own period");
REXCVAR_DEFINE_DOUBLE(gtav_audio_silence_db, 0, "GTAV",
                      "Sources whose loudest voice route gain is below this level in dB (e.g. -48) are treated as "
                      "silent: not generated or mixed, their XMA parsed without decoding; 6 dB lower to stay audible; "
                      "0 = only zero gains");
REXCVAR_DEFINE_STRING(gtav_audio_ab_file, "", "GTAV",
                      "Diagnostic: while this file exists, the settings it names (quiet, loop, controller, filters, "
                      "silence; all when empty) are ignored (A/B runs)");
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
std::array<uint16_t, kBuffers> g_loud{};
std::array<uint32_t, kBuffers> g_kept{};
std::array<uint32_t, kBuffers> g_culled{};
std::array<uint16_t, kVoices> g_marked{};
uint32_t g_marked_n = 0;
std::array<uint32_t, 8> g_killed{};
uint32_t g_killed_n = 0;
bool g_wave_silent = false;
uint32_t g_frame = 0;
std::array<uint32_t, kBuffers> g_wave_owner{};
std::array<uint32_t, kBuffers> g_wave_seen{};
std::array<uint16_t, kBuffers> g_wave_run{};
std::array<uint32_t, kBuffers> g_wave_called{};
std::array<bool, kBuffers> g_wave_looping{};

std::atomic<uint64_t> g_synth_run{0}, g_synth_skip{0}, g_mix_run{0}, g_mix_skip{0}, g_resample_skip{0}, g_feed_skip{0},
    g_wave_loud{0}, g_wave_quiet{0}, g_wave_skip{0}, g_controller{0}, g_ns_sources{0}, g_ns_loud{0}, g_ns_quiet{0},
    g_ns_mix{0}, g_ns_synth{0};
std::array<std::atomic<uint64_t>, 9> g_route_channels{};
std::atomic<uint64_t> g_route_bus{0}, g_route_unbuffered{0}, g_cull{0}, g_audible{0}, g_audible_max{0};
std::array<std::atomic<uint64_t>, 6> g_loud_db{};
std::array<std::atomic<uint64_t>, 64> g_route_dest{};
std::chrono::steady_clock::time_point g_report = std::chrono::steady_clock::now();
std::chrono::steady_clock::time_point g_ab_check = std::chrono::steady_clock::now();
constexpr uint32_t kAbQuiet = 1, kAbLoop = 2, kAbController = 4, kAbFilters = 8, kAbSilence = 16;
std::atomic<uint32_t> g_ab_off{0};

bool AbOff(uint32_t what) { return (g_ab_off.load(std::memory_order_relaxed) & what) != 0; }

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

std::chrono::steady_clock::time_point Now() {
  return REXCVAR_GET(gtav_audio_diag) ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
}

void AddTime(std::atomic<uint64_t>& counter, std::chrono::steady_clock::time_point since) {
  if (!REXCVAR_GET(gtav_audio_diag)) return;
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - since);
  counter.fetch_add(uint64_t(ns.count()), std::memory_order_relaxed);
}

double ThreadCpuMs() {
#if defined(__APPLE__)
  timespec t{};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
  return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
#else
  return 0;
#endif
}

void NameThread(const char* name) {
#if defined(__APPLE__)
  static thread_local bool named = false;
  if (named || !REXCVAR_GET(gtav_audio_diag)) return;
  named = true;
  pthread_setname_np(name);
#endif
}

void CheckAb() {
  const std::string& path = REXCVAR_GET(gtav_audio_ab_file);
  if (path.empty()) return;
  const auto now = std::chrono::steady_clock::now();
  if (now < g_ab_check) return;
  g_ab_check = now + std::chrono::seconds(1);
  std::ifstream in(path);
  uint32_t off = 0;
  if (in) {
    std::string word;
    while (in >> word)
      off |= word == "quiet"        ? kAbQuiet
             : word == "loop"       ? kAbLoop
             : word == "controller" ? kAbController
             : word == "filters"    ? kAbFilters
             : word == "silence"    ? kAbSilence
                                    : 0;
    if (!off) off = kAbQuiet | kAbLoop | kAbController | kAbFilters | kAbSilence;
  }
  if (off != g_ab_off.exchange(off)) REXLOG_INFO("[audio-ab] off {}", off);
}

void ReportRoutes(double s) {
  std::string dests;
  for (uint32_t i = 0; i < g_route_dest.size(); ++i)
    if (const uint64_t n = g_route_dest[i].exchange(0)) dests += fmt::format(" {}:{:.0f}", i, n / s);
  std::string channels;
  for (uint32_t i = 1; i < g_route_channels.size(); ++i)
    if (const uint64_t n = g_route_channels[i].exchange(0)) channels += fmt::format(" {}ch:{:.0f}", i, n / s);
  REXLOG_INFO("[audio-cuts] routes per second:{}, 4-buffer bus {:.0f}, no buffer {:.0f}; by submix:{}", channels,
              g_route_bus.exchange(0) / s, g_route_unbuffered.exchange(0) / s, dests);
  const double frames = s * 187.5;
  REXLOG_INFO("[audio-cuts] audible sources per frame {:.1f} (max {}), culled {:.1f}; by loudest gain: >-6 dB {:.1f}, "
              "-6..-12 {:.1f}, -12..-24 {:.1f}, -24..-36 {:.1f}, -36..-48 {:.1f}, <-48 {:.1f}",
              g_audible.exchange(0) / frames, g_audible_max.exchange(0), g_cull.exchange(0) / frames,
              g_loud_db[0].exchange(0) / frames, g_loud_db[1].exchange(0) / frames, g_loud_db[2].exchange(0) / frames,
              g_loud_db[3].exchange(0) / frames, g_loud_db[4].exchange(0) / frames, g_loud_db[5].exchange(0) / frames);
}

void Report() {
  if (!REXCVAR_GET(gtav_audio_diag)) return;
  static double cpu_at = ThreadCpuMs();
  const auto now = std::chrono::steady_clock::now();
  const double s = std::chrono::duration<double>(now - g_report).count();
  if (s < 10.0) return;
  g_report = now;
  const double cpu = ThreadCpuMs();
  REXLOG_INFO("[audio-cuts] engine thread cpu {:.1f} %; ms per second: voice mix {:.2f}, synth {:.2f}",
              (cpu - cpu_at) / s / 10.0, g_ns_mix.exchange(0) / s / 1e6, g_ns_synth.exchange(0) / s / 1e6);
  cpu_at = cpu;
  ReportRoutes(s);
  REXLOG_INFO("[audio-cuts] per second: synth {:.0f} run {:.0f} skipped, voices {:.0f} mixed {:.0f} skipped, "
              "resamples skipped {:.0f}, bus feeds skipped {:.0f}, wave players {:.0f} decoded {:.0f} quiet "
              "{:.0f} skipped, controller updates {:.1f}; ms per second: sources {:.2f}, loud wave players {:.2f}, "
              "quiet {:.2f}",
              g_synth_run.exchange(0) / s, g_synth_skip.exchange(0) / s, g_mix_run.exchange(0) / s,
              g_mix_skip.exchange(0) / s, g_resample_skip.exchange(0) / s, g_feed_skip.exchange(0) / s,
              g_wave_loud.exchange(0) / s, g_wave_quiet.exchange(0) / s, g_wave_skip.exchange(0) / s,
              g_controller.exchange(0) / s, g_ns_sources.exchange(0) / s / 1e6, g_ns_loud.exchange(0) / s / 1e6,
              g_ns_quiet.exchange(0) / s / 1e6);
}

bool LongSilent(uint32_t player, uint16_t id, bool silent) {
  const bool same = g_wave_owner[id] == player;
  if (!same || g_wave_seen[id] != g_frame) {
    if (!same) g_wave_looping[id] = false;
    const uint32_t run = same && g_wave_seen[id] + 1 == g_frame ? g_wave_run[id] : 0u;
    g_wave_run[id] = silent ? uint16_t(std::min<uint32_t>(run + 1u, 0xFFFF)) : 0;
    g_wave_owner[id] = player;
    g_wave_seen[id] = g_frame;
  }
  const int32_t after = REXCVAR_GET(gtav_audio_quiet_xma);
  return after > 0 && !AbOff(kAbQuiet) && g_wave_run[id] >= uint32_t(after);
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

uint16_t VoiceLoudness(const uint8_t* base, uint32_t v) {
  uint16_t loud = 0;
  for (uint32_t i = 0; i < 8; ++i) {
    const uint16_t id = L16(base, v + 308 + 2 * i);
    if (id == 0xFFFF) continue;
    if (id >= 512) return 0x8000;
    for (uint32_t j = 0; j < 16; ++j) {
      const int32_t g = int16_t(L16(base, v + 48 + 32 * i + 2 * j));
      loud = std::max<uint16_t>(loud, uint16_t(g < 0 ? -g : g));
    }
  }
  return loud;
}

void MarkBuffer(uint32_t buf, uint16_t loud) {
  if (g_state[buf] == 0) {
    g_marked[g_marked_n++] = uint16_t(buf);
    g_state[buf] = loud ? 2 : 1;
    g_loud[buf] = loud;
    return;
  }
  if (loud) g_state[buf] = 2;
  g_loud[buf] = std::max(g_loud[buf], loud);
}

void CountLoudness() {
  uint64_t audible = 0;
  for (uint32_t i = 0; i < g_marked_n; ++i) {
    const uint16_t buf = g_marked[i];
    if (g_state[buf] != 2) continue;
    ++audible;
    const uint16_t loud = g_loud[buf];
    const uint32_t bin = loud >= 16423 ? 0 : loud >= 8231 ? 1 : loud >= 2067 ? 2 : loud >= 519 ? 3 : loud >= 130 ? 4 : 5;
    g_loud_db[bin].fetch_add(1, std::memory_order_relaxed);
  }
  g_audible.fetch_add(audible, std::memory_order_relaxed);
  if (audible > g_audible_max.load(std::memory_order_relaxed)) g_audible_max.store(audible, std::memory_order_relaxed);
}

void CullQuiet() {
  const double db = REXCVAR_GET(gtav_audio_silence_db);
  if (db >= 0 || AbOff(kAbSilence)) return;
  const uint32_t below = uint32_t(32768.0 * std::pow(10.0, db / 20.0));
  for (uint32_t i = 0; i < g_marked_n; ++i) {
    const uint16_t buf = g_marked[i];
    if (g_state[buf] != 2) continue;
    if (g_loud[buf] >= (g_kept[buf] + 1 == g_frame ? below / 2 : below)) {
      g_kept[buf] = g_frame;
      continue;
    }
    g_state[buf] = 1;
    g_culled[buf] = g_frame;
    Count(g_cull);
  }
}

bool Culled(uint32_t buf) { return buf < kBuffers && g_culled[buf] == g_frame; }

void BuildFrameState(uint8_t* base, uint32_t mixer) {
  for (uint32_t i = 0; i < g_marked_n; ++i) g_state[g_marked[i]] = 0;
  g_marked_n = 0;
  const uint32_t voices = std::min<uint32_t>(L32(base, mixer + 46376), kVoices);
  for (uint32_t i = 0; i < voices; ++i) {
    const uint32_t v = mixer + 6688 + 336 * i;
    const uint32_t buf = L32(base, v + 304);
    if (L32(base, v + 324) != 1 || buf >= kBuffers) continue;
    MarkBuffer(buf, VoiceLoudness(base, v));
  }
  if (REXCVAR_GET(gtav_audio_diag)) CountLoudness();
  CullQuiet();
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

bool GtavAudioFiltersAbOff() { return AbOff(kAbFilters); }

REX_EXTERN(__imp__sub_831C4708);
REX_HOOK_RAW(sub_831C4708) {
  ++g_frame;
  NameThread("audio engine");
  CheckAb();
  BuildFrameState(base, ctx.r3.u32);
  Report();
  const auto start = Now();
  __imp__sub_831C4708(ctx, base);
  AddTime(g_ns_sources, start);
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
    const auto start = Now();
    __imp__sub_83215AE8(ctx, base);
    AddTime(g_ns_synth, start);
    return;
  }
  Count(g_synth_skip);
  const uint32_t pool = Pool(base);
  for (uint32_t i = 0; i < outs; ++i) std::memset(base + pool + (uint32_t(L16(base, src + 4 + 2 * i)) << 10), 0, 1024);
  S32(base, core, L32(base, core) + 1);
}

void CountRoutes(const uint8_t* base, uint32_t v) {
  if (!REXCVAR_GET(gtav_audio_diag) || L32(base, v + 304) >= 0xFFFF) return;
  const uint32_t mixer = L32(base, kMixerPtr);
  const uint32_t table = L32(base, kRouteTablePtr);
  for (uint32_t i = 0; i < 8; ++i) {
    const uint16_t id = L16(base, v + 308 + 2 * i);
    if (id >= 512) continue;
    const uint32_t info = L32(base, table + (id + 388) * 8 + 4);
    const uint32_t index = (info >> 6) & 0x3FF;
    const uint32_t dest = mixer + 16 + 208 * index;
    if (int32_t(L32(base, dest + 184)) < 0) continue;
    g_route_dest[std::min<uint32_t>(index, 63)].fetch_add(1, std::memory_order_relaxed);
    if (!base[dest + 200])
      g_route_unbuffered.fetch_add(1, std::memory_order_relaxed);
    else if (L32(base, dest + 192) != 1)
      g_route_bus.fetch_add(1, std::memory_order_relaxed);
    else
      g_route_channels[((info >> 3) & 7) + 1].fetch_add(1, std::memory_order_relaxed);
  }
}

REX_EXTERN(__imp__sub_831CAF88);
REX_HOOK_RAW(sub_831CAF88) {
  const uint32_t v = ctx.r3.u32;
  if (!REXCVAR_GET(gtav_audio_skip_silent) || L32(base, v + 304) >= 0xFFFF ||
      (!VoiceSilent(base, v) && !Culled(L32(base, v + 304)))) {
    Count(g_mix_run);
    CountRoutes(base, v);
    const auto start = Now();
    __imp__sub_831CAF88(ctx, base);
    AddTime(g_ns_mix, start);
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
  const bool silent = id < kBuffers && g_state[id] == 1;
  const bool quiet = id < kBuffers && (LongSilent(ctx.r3.u32, id, silent) || Culled(id));
  const int32_t every = REXCVAR_GET(gtav_audio_quiet_loop_every);
  if (quiet && every > 1 && !AbOff(kAbLoop) && g_wave_looping[id] && g_frame - g_wave_called[id] < uint32_t(every)) {
    Count(g_wave_skip);
    std::memset(base + Pool(base) + (uint32_t(id) << 10), 0, 1024);
    return;
  }
  g_wave_silent = REXCVAR_GET(gtav_audio_skip_silent) && silent;
  Count(quiet ? g_wave_quiet : g_wave_loud);
  rex::audio::XmaDecoder::SetQuietKicks(quiet);
  rex::audio::XmaDecoder::ResetKickCount();
  const auto start = Now();
  __imp__sub_831C9008(ctx, base);
  AddTime(quiet ? g_ns_quiet : g_ns_loud, start);
  rex::audio::XmaDecoder::SetQuietKicks(false);
  g_wave_silent = false;
  if (id < kBuffers) {
    g_wave_called[id] = g_frame;
    if (rex::audio::XmaDecoder::kicks())
      g_wave_looping[id] = rex::audio::XmaDecoder::kicks() == rex::audio::XmaDecoder::kicks_looping();
  }
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

REX_EXTERN(__imp__sub_831E7C10);
REX_HOOK_RAW(sub_831E7C10) {
  const auto start = std::chrono::steady_clock::now();
  NameThread("audio control");
  Count(g_controller);
  static bool logged = false;
  if (!logged && REXCVAR_GET(gtav_audio_diag)) {
    logged = true;
    uint32_t period;
    std::memcpy(&period, base + 0x8204C198, 4);
    period = __builtin_bswap32(period);
    float ms;
    std::memcpy(&ms, &period, 4);
    REXLOG_INFO("[audio-cuts] controller period {} ms, sleep flag {}", ms, base[0x8398C665]);
  }
  __imp__sub_831E7C10(ctx, base);
  if (REXCVAR_GET(gtav_audio_diag)) {
    static double cpu_at = ThreadCpuMs();
    static auto report_at = start;
    const double s = std::chrono::duration<double>(start - report_at).count();
    if (s >= 10.0) {
      const double cpu = ThreadCpuMs();
      REXLOG_INFO("[audio-cuts] controller thread cpu {:.1f} %", (cpu - cpu_at) / s / 10.0);
      cpu_at = cpu;
      report_at = start;
    }
  }
  const int32_t ms = REXCVAR_GET(gtav_audio_controller_ms);
  if (ms > 0 && !AbOff(kAbController))
    std::this_thread::sleep_until(start + std::chrono::milliseconds(ms));
}
