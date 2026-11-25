#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

REXCVAR_DEFINE_BOOL(gtav_audio_native_filters, false, "GTAV",
                    "Run the audio biquads (sub_83210BD0 scalar path, cascaded sub_8320CF78) as native code with the "
                    "recompiled code's arithmetic (same samples)");
REXCVAR_DEFINE_BOOL(gtav_audio_float_filters, false, "GTAV",
                    "With gtav_audio_native_filters, run the biquads in single precision with the guest's operation "
                    "order instead of emulating each single-precision rounding in double");
REXCVAR_DEFINE_BOOL(gtav_audio_verify_filters, false, "GTAV",
                    "Diagnostic: run the native audio biquads on a copy next to the recompiled ones and log how many "
                    "calls gave different bytes");
REXCVAR_DECLARE(bool, gtav_audio_diag);

bool GtavAudioFiltersAbOff();
bool GtavAudioFloatAbOff();

namespace {
constexpr uint32_t kVectorPathFlag = 0x83D12ED8;

std::atomic<uint64_t> g_same{0}, g_different{0}, g_native_calls{0}, g_native_ns{0}, g_original_calls{0},
    g_original_ns{0};
std::chrono::steady_clock::time_point g_report = std::chrono::steady_clock::now();

double F(double v) { return double(float(v)); }

double Load(const uint8_t* p) {
  uint32_t u;
  std::memcpy(&u, p, 4);
  u = __builtin_bswap32(u);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

void Store(uint8_t* p, double v) {
  const float f = float(v);
  uint32_t u;
  std::memcpy(&u, &f, 4);
  u = __builtin_bswap32(u);
  std::memcpy(p, &u, 4);
}

float LoadF(const uint8_t* p) {
  uint32_t u;
  std::memcpy(&u, p, 4);
  u = __builtin_bswap32(u);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

void StoreF(uint8_t* p, float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  u = __builtin_bswap32(u);
  std::memcpy(p, &u, 4);
}

struct Coefs {
  double b0, b1, b2, a1, a2;
};

Coefs Read(const PPCContext& ctx) { return {ctx.f1.f64, ctx.f2.f64, ctx.f3.f64, ctx.f4.f64, ctx.f5.f64}; }

void Biquad(uint8_t* samples, uint8_t* state, uint32_t n, const Coefs& c) {
  double z1 = Load(state), z2 = Load(state + 4);
  for (uint32_t i = 0; i < n; ++i) {
    uint8_t* p = samples + 4 * i;
    const double x = Load(p);
    const double y = F(std::fma(x, c.b0, z1));
    Store(p, y);
    const double t1 = F(y * c.a1), t2 = F(y * c.a2);
    z1 = F(F(std::fma(x, c.b1, -t1)) + z2);
    z2 = F(std::fma(x, c.b2, -t2));
  }
  Store(state, z1);
  Store(state + 4, z2);
}

void Cascade(uint8_t* samples, uint8_t* state, uint32_t n, const Coefs& c) {
  double s1a = Load(state), s2a = Load(state + 4), s1b = Load(state + 8), s2b = Load(state + 12);
  for (uint32_t i = 0; i < n; ++i) {
    uint8_t* p = samples + 4 * i;
    const double x = Load(p);
    const double y1 = F(std::fma(x, c.b0, s1a));
    const double y2 = F(std::fma(y1, c.b0, s1b));
    Store(p, y2);
    const double t1 = F(y1 * c.a1), t2 = F(y1 * c.a2), t3 = F(y2 * c.a1), t4 = F(y2 * c.a2);
    s1a = F(F(std::fma(x, c.b1, -t1)) + s2a);
    s2a = F(std::fma(x, c.b2, -t2));
    s1b = F(F(std::fma(y1, c.b1, -t3)) + s2b);
    s2b = F(std::fma(y1, c.b2, -t4));
  }
  Store(state, s1a);
  Store(state + 4, s2a);
  Store(state + 8, s1b);
  Store(state + 12, s2b);
}

void BiquadF(uint8_t* samples, uint8_t* state, uint32_t n, const Coefs& c) {
  const float b0 = float(c.b0), b1 = float(c.b1), b2 = float(c.b2), a1 = float(c.a1), a2 = float(c.a2);
  float z1 = LoadF(state), z2 = LoadF(state + 4);
  for (uint32_t i = 0; i < n; ++i) {
    uint8_t* p = samples + 4 * i;
    const float x = LoadF(p);
    const float y = std::fma(x, b0, z1);
    StoreF(p, y);
    const float t1 = y * a1, t2 = y * a2;
    z1 = std::fma(x, b1, -t1) + z2;
    z2 = std::fma(x, b2, -t2);
  }
  StoreF(state, z1);
  StoreF(state + 4, z2);
}

void CascadeF(uint8_t* samples, uint8_t* state, uint32_t n, const Coefs& c) {
  const float b0 = float(c.b0), b1 = float(c.b1), b2 = float(c.b2), a1 = float(c.a1), a2 = float(c.a2);
  float s1a = LoadF(state), s2a = LoadF(state + 4), s1b = LoadF(state + 8), s2b = LoadF(state + 12);
  for (uint32_t i = 0; i < n; ++i) {
    uint8_t* p = samples + 4 * i;
    const float x = LoadF(p);
    const float y1 = std::fma(x, b0, s1a);
    const float y2 = std::fma(y1, b0, s1b);
    StoreF(p, y2);
    const float t1 = y1 * a1, t2 = y1 * a2, t3 = y2 * a1, t4 = y2 * a2;
    s1a = std::fma(x, b1, -t1) + s2a;
    s2a = std::fma(x, b2, -t2);
    s1b = std::fma(y1, b1, -t3) + s2b;
    s2b = std::fma(y1, b2, -t4);
  }
  StoreF(state, s1a);
  StoreF(state + 4, s2a);
  StoreF(state + 8, s1b);
  StoreF(state + 12, s2b);
}

bool FloatFilters() { return REXCVAR_GET(gtav_audio_float_filters) && !GtavAudioFloatAbOff(); }

bool NativeFilters() {
  return REXCVAR_GET(gtav_audio_verify_filters) || (REXCVAR_GET(gtav_audio_native_filters) && !GtavAudioFiltersAbOff());
}

void Report() {
  if (!REXCVAR_GET(gtav_audio_diag)) return;
  const auto now = std::chrono::steady_clock::now();
  if (now - g_report < std::chrono::seconds(10)) return;
  g_report = now;
  const uint64_t native = g_native_calls.exchange(0), original = g_original_calls.exchange(0);
  const double native_ms = g_native_ns.exchange(0) / 1e6, original_ms = g_original_ns.exchange(0) / 1e6;
  REXLOG_INFO("[audio-dsp] biquads per 10 s: native {} calls {:.2f} ms, recompiled {} calls {:.2f} ms; checked {} "
              "calls same, {} different",
              native, native_ms, original, original_ms, g_same.exchange(0), g_different.exchange(0));
}

template <typename Body>
void Timed(std::atomic<uint64_t>& calls, std::atomic<uint64_t>& ns, Body body) {
  if (!REXCVAR_GET(gtav_audio_diag)) {
    body();
    return;
  }
  const auto start = std::chrono::steady_clock::now();
  body();
  ns.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start)
                            .count()),
               std::memory_order_relaxed);
  calls.fetch_add(1, std::memory_order_relaxed);
  Report();
}

template <typename Native, typename Original>
void Run(PPCContext& ctx, uint8_t* base, uint32_t state_bytes, Native native, Original original) {
  uint8_t* samples = base + ctx.r3.u32;
  uint8_t* state = base + ctx.r9.u32;
  const uint32_t n = ctx.r10.u32;
  const Coefs c = Read(ctx);
  if (REXCVAR_GET(gtav_audio_verify_filters)) {
    thread_local std::vector<uint8_t> copy;
    copy.assign(samples, samples + 4 * n);
    uint8_t state_copy[16];
    std::memcpy(state_copy, state, 16);
    ctx.fpscr.disableFlushMode();
    native(copy.data(), state_copy, n, c);
    original(ctx, base);
    const bool same = !std::memcmp(copy.data(), samples, 4 * n) && !std::memcmp(state_copy, state, state_bytes);
    (same ? g_same : g_different).fetch_add(1, std::memory_order_relaxed);
    Report();
    return;
  }
  Timed(g_native_calls, g_native_ns, [&] {
    ctx.fpscr.disableFlushMode();
    native(samples, state, n, c);
  });
}
}  // namespace

REX_EXTERN(__imp__sub_83210BD0);
REX_HOOK_RAW(sub_83210BD0) {
  if (base[kVectorPathFlag] || !NativeFilters()) {
    Timed(g_original_calls, g_original_ns, [&] { __imp__sub_83210BD0(ctx, base); });
    return;
  }
  Run(ctx, base, 8, FloatFilters() ? BiquadF : Biquad, __imp__sub_83210BD0);
}

REX_EXTERN(__imp__sub_8320CF78);
REX_HOOK_RAW(sub_8320CF78) {
  if (!NativeFilters()) {
    Timed(g_original_calls, g_original_ns, [&] { __imp__sub_8320CF78(ctx, base); });
    return;
  }
  const uint32_t n = ctx.r10.u32;
  const uint32_t end = ctx.r3.u32 + (n >= 4 ? 16 * (n / 4) : 0);
  Run(ctx, base, 16, FloatFilters() ? CascadeF : Cascade, __imp__sub_8320CF78);
  if (REXCVAR_GET(gtav_audio_verify_filters) && ctx.r3.u32 != end) g_different.fetch_add(1, std::memory_order_relaxed);
  ctx.r3.u64 = end;
}
