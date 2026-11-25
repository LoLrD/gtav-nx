#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/ppc/intrinsics.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

REXCVAR_DEFINE_BOOL(gtav_native_rewrites, true, "GTAV",
                    "Native replacements of hot guest functions; off turns every gtav_nr_* one off");
REXCVAR_DEFINE_BOOL(gtav_nr_memcpy, true, "GTAV", "Native guest memcpy (sub_8378DEF8, sub_83773B60)");
REXCVAR_DEFINE_BOOL(gtav_nr_memset, true, "GTAV", "Native guest memset (sub_8378D940, sub_8378DAE0)");
REXCVAR_DEFINE_BOOL(gtav_nr_anim, true, "GTAV",
                    "Native animation curve evaluation (sub_8341DAA0)");
REXCVAR_DEFINE_BOOL(gtav_nr_unpack, true, "GTAV", "Native animation track unpacking (sub_8329BDA0)");
REXCVAR_DEFINE_BOOL(gtav_nr_volume, true, "GTAV",
                    "Native volume rasterization of boxes into a 4x4x6 cell grid (sub_8315B8D0)");
REXCVAR_DEFINE_BOOL(gtav_nr_quat, true, "GTAV", "Native animation Euler-to-quaternion curve evaluation (sub_83420D18)");
REXCVAR_DEFINE_BOOL(gtav_nr_cull, true, "GTAV", "Native triangle binning into the 12 occlusion tiles (sub_831665D8)");
REXCVAR_DEFINE_BOOL(gtav_nr_blend, true, "GTAV", "Native animation frame blending (sub_83441C48)");
REXCVAR_DEFINE_BOOL(gtav_nr_bound, true, "GTAV", "Native sphere against box overlap test (sub_83585DE8)");
REXCVAR_DEFINE_BOOL(gtav_nr_vm, true, "GTAV", "Native animation expression VM (sub_834260A0)");
REXCVAR_DEFINE_BOOL(gtav_nr_pack, true, "GTAV", "Native packed-element sphere culling into a bitmask (sub_8358AE80)");
REXCVAR_DEFINE_BOOL(gtav_nr_gather, true, "GTAV",
                    "Native skeleton local-pose gather and hierarchy transform (sub_83442200)");
REXCVAR_DEFINE_BOOL(gtav_nr_pow, true, "GTAV", "Native powf that evaluates only the lane it returns (sub_822F4E48)");
REXCVAR_DEFINE_INT32(gtav_nr_check, 0, "GTAV",
                     "Differential check: every Nth call of a replaced function runs both the native and the "
                     "original version and compares them, with timings, logged every 10 s (0 = off)");

REXCVAR_DEFINE_INT32(gtav_nr_check_delay, 0, "GTAV", "Seconds after start before gtav_nr_check begins");
REXCVAR_DEFINE_BOOL(gtav_nr_lzx, false, "GTAV", "Native LZX aligned-offset block decoder of XCompress (sub_837A0D88)");
REXCVAR_DEFINE_INT32(gtav_nr_lzx_check, 0, "GTAV",
                     "Every Nth sub_837A0D88 call runs both the native and the original decoder on the same input "
                     "and compares the window bytes, the decoder state and the result (0 = off)");
REXCVAR_DEFINE_BOOL(gtav_nr_lzx_stats, false, "GTAV",
                    "Time every sub_837A0D88 call and log calls, bytes and ns per byte every 10 s");

REX_EXTERN(__imp__sub_8378DEF8);
REX_EXTERN(__imp__sub_83773B60);
REX_EXTERN(__imp__sub_8378D940);
REX_EXTERN(__imp__sub_8378DAE0);
REX_EXTERN(__imp__sub_8341DAA0);
REX_EXTERN(__imp__sub_8329BDA0);
REX_EXTERN(__imp__sub_8315B8D0);
REX_EXTERN(__imp__sub_83420D18);
REX_EXTERN(__imp__sub_831665D8);
REX_EXTERN(__imp__sub_83441C48);
REX_EXTERN(__imp__sub_83585DE8);
REX_EXTERN(sub_826B9248);
REX_EXTERN(__imp__sub_834260A0);
REX_EXTERN(__imp__sub_8358AE80);
REX_EXTERN(__imp__sub_83442200);
REX_EXTERN(__imp__sub_822F4E48);
REX_EXTERN(__imp__sub_837A0D88);
REX_EXTERN(sub_822174F8);
REX_EXTERN(sub_82303FC0);
REX_EXTERN(sub_824FEFF0);
REX_EXTERN(sub_824FF578);
REX_EXTERN(sub_825176D8);
REX_EXTERN(sub_8341D708);
REX_EXTERN(sub_8341D9D8);
REX_EXTERN(sub_8341DAA0);
REX_EXTERN(sub_834206A8);
REX_EXTERN(sub_83420D18);

namespace {

#if REX_PLATFORM_WIN32 || (REX_PLATFORM_MAC && REX_ARCH_ARM64)
constexpr uint32_t kPhysOffset = 0x1000;
#else
constexpr uint32_t kPhysOffset = 0;
#endif

constexpr uint32_t kGuard = 64;
constexpr uint32_t kMaxChecked = 1u << 20;

inline void MarkCards(uint8_t* base, uint32_t a, uint32_t n) {
#if defined(REX_STORE_CARDS)
  if (n) std::memset(base + (a >> 12), 0, ((uint64_t(a) + n - 1) >> 12) - (a >> 12) + 1);
#endif
}

inline uint8_t* Host(uint8_t* base, uint32_t a) {
  return base + a + (kPhysOffset && a >= 0xE0000000u ? kPhysOffset : 0u);
}

inline bool Flat(uint32_t a, uint32_t n) {
  const uint64_t end = uint64_t(a) + n;
  if (end > 0x100000000ull) return false;
  return !kPhysOffset || n == 0 || (a >= 0xE0000000u) == (uint32_t(end - 1) >= 0xE0000000u);
}

inline bool Overlap(uint32_t a, uint32_t b, uint32_t n) {
  return uint64_t(a) < uint64_t(b) + n && uint64_t(b) < uint64_t(a) + n;
}

inline uint32_t Be32(const uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a, 4);
  return __builtin_bswap32(v);
}

using Clock = std::chrono::steady_clock;

struct Stat {
  const char* name;
  std::atomic<uint64_t> checks{0};
  std::atomic<uint64_t> mismatches{0};
  std::atomic<uint64_t> guard_hits{0};
  std::atomic<uint64_t> nan_only{0};
  std::atomic<uint64_t> native_ns{0};
  std::atomic<uint64_t> original_ns{0};
  std::atomic<uint32_t> logged{0};
};

Stat g_memcpy{"memcpy sub_8378DEF8"};
Stat g_memcpy2{"memcpy sub_83773B60"};
Stat g_memset{"memset sub_8378D940"};
Stat g_memset128{"memset128 sub_8378DAE0"};
Stat g_curve3{"curve3 sub_8341DAA0"};
Stat g_unpack{"unpack sub_8329BDA0"};
Stat g_volume{"volume sub_8315B8D0"};
Stat g_quat{"quat sub_83420D18"};
Stat g_cull{"cull sub_831665D8"};
Stat g_blend{"blend sub_83441C48"};
Stat g_bound{"bound sub_83585DE8"};
Stat g_vm{"vm sub_834260A0"};
Stat g_pack{"pack sub_8358AE80"};
Stat g_gather{"gather sub_83442200"};
Stat g_pow{"pow sub_822F4E48"};
Stat g_lzx{"lzx sub_837A0D88"};
std::atomic<uint64_t> g_vm_unchecked{0};

std::array<Stat*, 16> g_stats{&g_memcpy, &g_memcpy2, &g_memset, &g_memset128, &g_curve3, &g_unpack,
                              &g_volume, &g_quat,    &g_cull,   &g_blend,     &g_bound,  &g_vm,
                              &g_pack,   &g_gather,  &g_pow,    &g_lzx};

std::mutex g_report_mutex;
Clock::time_point g_report_at = Clock::now();
thread_local uint32_t t_tick = 0;

inline bool Enabled(bool flag) { return flag && REXCVAR_GET(gtav_native_rewrites); }

const Clock::time_point g_started = Clock::now();

inline bool CheckNow() {
  const int32_t every = REXCVAR_GET(gtav_nr_check);
  return every > 0 && ++t_tick % uint32_t(every) == 0 &&
         Clock::now() - g_started >= std::chrono::seconds(REXCVAR_GET(gtav_nr_check_delay));
}

uint64_t Ns(Clock::time_point a, Clock::time_point b) {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
}

void Report() {
  std::unique_lock lock(g_report_mutex, std::try_to_lock);
  if (!lock.owns_lock()) return;
  const auto now = Clock::now();
  if (now - g_report_at < std::chrono::seconds(10)) return;
  g_report_at = now;
  for (Stat* s : g_stats) {
    const uint64_t n = s->checks.load(std::memory_order_relaxed);
    if (!n) continue;
    const double native = double(s->native_ns.load(std::memory_order_relaxed)) / n;
    const double original = double(s->original_ns.load(std::memory_order_relaxed)) / n;
    REXLOG_INFO("[native-rewrites] {}: {} checked, {} mismatches, {} guard changes, {} NaN-payload-only, native "
                "{:.1f} ns, original {:.1f} ns, x{:.2f}",
                s->name, n, s->mismatches.load(std::memory_order_relaxed),
                s->guard_hits.load(std::memory_order_relaxed), s->nan_only.load(std::memory_order_relaxed), native,
                original, native > 0 ? original / native : 0.0);
  }
  if (const uint64_t n = g_vm_unchecked.load(std::memory_order_relaxed))
    REXLOG_INFO("[native-rewrites] vm sub_834260A0: {} calls not checkable", n);
}

void Record(Stat& s, bool ok, bool guard_ok, uint64_t native_ns, uint64_t original_ns, const std::string& what) {
  s.checks.fetch_add(1, std::memory_order_relaxed);
  s.native_ns.fetch_add(native_ns, std::memory_order_relaxed);
  s.original_ns.fetch_add(original_ns, std::memory_order_relaxed);
  if (!guard_ok) s.guard_hits.fetch_add(1, std::memory_order_relaxed);
  if (!ok) s.mismatches.fetch_add(1, std::memory_order_relaxed);
  if ((!ok || !guard_ok) && s.logged.fetch_add(1, std::memory_order_relaxed) < 32)
    REXLOG_WARN("[native-rewrites] {} {}: {}", s.name, ok ? "guard bytes changed" : "MISMATCH", what);
  Report();
}

uint32_t Writable(uint32_t a, uint32_t n);

struct Guards {
  uint32_t lo_addr = 0, lo_n = 0, hi_addr = 0, hi_n = 0;
  std::array<uint8_t, kGuard> lo{}, hi{};

  void Take(uint8_t* base, uint32_t a, uint32_t n) {
    lo_n = std::min(a, kGuard);
    lo_addr = a - lo_n;
    if (!Flat(lo_addr, lo_n) || Writable(lo_addr, lo_n) != lo_n) lo_n = 0;
    hi_addr = a + n;
    hi_n = uint64_t(hi_addr) + kGuard <= 0x100000000ull && hi_addr >= a ? kGuard : 0;
    if (!Flat(hi_addr, hi_n)) hi_n = 0;
    hi_n = Writable(hi_addr, hi_n);
    std::memcpy(lo.data(), Host(base, lo_addr), lo_n);
    std::memcpy(hi.data(), Host(base, hi_addr), hi_n);
  }

  bool Same(uint8_t* base) const {
    return std::memcmp(lo.data(), Host(base, lo_addr), lo_n) == 0 &&
           std::memcmp(hi.data(), Host(base, hi_addr), hi_n) == 0;
  }

  std::string Describe(uint8_t* base) const {
    std::string out;
    for (uint32_t i = 0; i < lo_n; ++i)
      if (lo[i] != Host(base, lo_addr)[i]) {
        out += fmt::format(" below: {:08X} {:02X}->{:02X}", lo_addr + i, lo[i], Host(base, lo_addr)[i]);
        break;
      }
    for (uint32_t i = 0; i < hi_n; ++i)
      if (hi[i] != Host(base, hi_addr)[i]) {
        out += fmt::format(" above: +{} {:02X}->{:02X}", i, hi[i], Host(base, hi_addr)[i]);
        break;
      }
    return out;
  }
};

std::vector<uint8_t>& Scratch() {
  thread_local std::vector<uint8_t> buffer;
  return buffer;
}

void CheckCopy(Stat& s, PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  const uint64_t r3 = ctx.r3.u64;
  const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32, n = ctx.r5.u32;
  if (n > kMaxChecked) {
    original(ctx, base);
    return;
  }
  auto& want = Scratch();
  want.resize(n);
  Guards g;
  g.Take(base, dst, n);
  const auto t0 = Clock::now();
  std::memcpy(want.data(), Host(base, src), n);
  const auto t1 = Clock::now();
  original(ctx, base);
  const auto t2 = Clock::now();
  const bool ok = ctx.r3.u64 == r3 && std::memcmp(Host(base, dst), want.data(), n) == 0;
  const bool guard_ok = g.Same(base);
  Record(s, ok, guard_ok, Ns(t0, t1), Ns(t1, t2),
         ok && guard_ok ? std::string()
                        : fmt::format("dst {:08X} src {:08X} n {} r3 {:X}{} src beyond: {:02X}", dst, src, n,
                                      ctx.r3.u64, g.Describe(base), Host(base, src + n)[0]));
}

void CheckFill(Stat& s, PPCContext& ctx, uint8_t* base, PPCFunc* original, uint32_t n) {
  const uint64_t r3 = ctx.r3.u64;
  const uint32_t dst = ctx.r3.u32;
  const uint8_t c = uint8_t(ctx.r4.u32);
  if (n > kMaxChecked) {
    original(ctx, base);
    return;
  }
  auto& want = Scratch();
  want.resize(n);
  Guards g;
  g.Take(base, dst, n);
  const auto t0 = Clock::now();
  std::memset(want.data(), c, n);
  const auto t1 = Clock::now();
  original(ctx, base);
  const auto t2 = Clock::now();
  const bool ok = ctx.r3.u64 == r3 && std::memcmp(Host(base, dst), want.data(), n) == 0;
  const bool guard_ok = g.Same(base);
  Record(s, ok, guard_ok, Ns(t0, t1), Ns(t1, t2),
         ok && guard_ok ? std::string()
                        : fmt::format("dst {:08X} c {:02X} n {} r5 {} r3 {:X}{}", dst, c, n, ctx.r5.u32, ctx.r3.u64,
                                      g.Describe(base)));
}

std::atomic<int> g_crt_state{0};

bool CrtValid(uint8_t* base) {
  int state = g_crt_state.load(std::memory_order_relaxed);
  if (state == 0) [[unlikely]] {
    const uint32_t step = Be32(base, 0x839D5DAC), stride = Be32(base, 0x839D5DA8);
    state = step == 1 && stride == 16 ? 1 : 2;
    g_crt_state.store(state, std::memory_order_relaxed);
    if (state == 2)
      REXLOG_ERROR("[native-rewrites] CRT constants are {} and {} instead of 1 and 16: native memcpy/memset off", step,
                   stride);
  }
  return state == 1;
}

template <PPCFunc* Original>
inline void Copy(Stat& s, PPCContext& ctx, uint8_t* base) {
  const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32, n = ctx.r5.u32;
  if (!Enabled(REXCVAR_GET(gtav_nr_memcpy)) || Overlap(dst, src, n) || !Flat(dst, n) || !Flat(src, n) ||
      !CrtValid(base)) {
    Original(ctx, base);
    return;
  }
  if (CheckNow()) [[unlikely]] {
    CheckCopy(s, ctx, base, Original);
    return;
  }
  std::memcpy(Host(base, dst), Host(base, src), n);
  MarkCards(base, dst, n);
}

}  // namespace

REX_HOOK_RAW(sub_8378DEF8) { Copy<__imp__sub_8378DEF8>(g_memcpy, ctx, base); }

REX_HOOK_RAW(sub_83773B60) { Copy<__imp__sub_83773B60>(g_memcpy2, ctx, base); }

REX_HOOK_RAW(sub_8378D940) {
  const uint32_t dst = ctx.r3.u32, n = ctx.r5.u32;
  if (!Enabled(REXCVAR_GET(gtav_nr_memset)) || !Flat(dst, n) || !CrtValid(base)) {
    __imp__sub_8378D940(ctx, base);
    return;
  }
  if (CheckNow()) [[unlikely]] {
    CheckFill(g_memset, ctx, base, __imp__sub_8378D940, n);
    return;
  }
  std::memset(Host(base, dst), int(ctx.r4.u32 & 0xFF), n);
  MarkCards(base, dst, n);
}

REX_HOOK_RAW(sub_8378DAE0) {
  const uint32_t dst = ctx.r3.u32, n = ctx.r5.u32 & ~127u;
  if (!Enabled(REXCVAR_GET(gtav_nr_memset)) || (dst & 127) || !Flat(dst, n)) {
    __imp__sub_8378DAE0(ctx, base);
    return;
  }
  if (CheckNow()) [[unlikely]] {
    CheckFill(g_memset128, ctx, base, __imp__sub_8378DAE0, n);
    return;
  }
  std::memset(Host(base, dst), int(ctx.r4.u32 & 0xFF), n);
  MarkCards(base, dst, n);
}

namespace {

struct Region {
  uint32_t addr;
  uint32_t size;
};

using RegsEqual = bool (*)(const PPCContext&, const PPCContext&);

inline bool IsNan(uint32_t bits) { return (bits & 0x7F800000u) == 0x7F800000u && (bits & 0x007FFFFFu); }

bool SameResult(const PPCContext& a, const PPCContext& b) { return a.r3.u64 == b.r3.u64 && a.r1.u64 == b.r1.u64; }

uint32_t Writable(uint32_t a, uint32_t n) {
  if (!a || !n) return 0;
  auto* heap = rex::system::kernel_memory()->LookupHeap(a);
  if (!heap) return 0;
  uint32_t ok = 0;
  while (ok < n) {
    const uint32_t p = a + ok;
    const uint32_t end = uint32_t(std::min<uint64_t>(uint64_t(a) + n, (uint64_t(p) | 0xFFF) + 1));
    const auto access = heap->QueryRangeAccess(p, end - 1);
    if (access != rex::memory::PageAccess::kReadWrite && access != rex::memory::PageAccess::kExecuteReadWrite) break;
    ok = end - a;
  }
  return ok;
}

template <class Native>
void Differential(Stat& s, PPCContext& ctx, uint8_t* base, PPCFunc* original, std::initializer_list<Region> asked,
                  RegsEqual same, Native native) {
  thread_local std::vector<uint8_t> before, after_native;
  thread_local std::vector<Region> regions;
  regions.clear();
  for (const Region& r : asked) regions.push_back({r.addr, Writable(r.addr, r.size)});
  size_t total = 0;
  for (const Region& r : regions) total += r.size;
  before.resize(total);
  after_native.resize(total);
  size_t o = 0;
  for (const Region& r : regions) {
    std::memcpy(before.data() + o, Host(base, r.addr), r.size);
    o += r.size;
  }
  const PPCContext input = ctx;
  PPCContext mine = ctx;
  const auto t0 = Clock::now();
  native(mine, base);
  const auto t1 = Clock::now();
  o = 0;
  for (const Region& r : regions) {
    std::memcpy(after_native.data() + o, Host(base, r.addr), r.size);
    o += r.size;
  }
  o = 0;
  for (const Region& r : regions) {
    std::memcpy(Host(base, r.addr), before.data() + o, r.size);
    o += r.size;
  }
  const auto t2 = Clock::now();
  original(ctx, base);
  const auto t3 = Clock::now();
  bool ok = same(mine, ctx), nan_only = false;
  std::string what;
  o = 0;
  for (const Region& r : regions) {
    const uint8_t* got = Host(base, r.addr);
    for (uint32_t i = 0; i < r.size && ok; ++i) {
      if (got[i] == after_native[o + i]) continue;
      const uint32_t w = i & ~3u;
      uint32_t want = 0, have = 0;
      for (uint32_t k = 0; k < 4 && w + k < r.size; ++k) {
        want = want << 8 | got[w + k];
        have = have << 8 | after_native[o + w + k];
      }
      if (IsNan(want) && IsNan(have)) {
        nan_only = true;
        i = w + 3;
        continue;
      }
      ok = false;
      what = fmt::format("byte {:08X}: original {:08X} native {:08X}", r.addr + i, want, have);
    }
    o += r.size;
  }
  if (nan_only) s.nan_only.fetch_add(1, std::memory_order_relaxed);
  if (!ok && what.empty()) what = fmt::format("registers: r3 {:X}/{:X}", ctx.r3.u64, mine.r3.u64);
  if (!ok)
    what += fmt::format(" | in r3 {:08X} r4 {:08X} r5 {:08X} r6 {:08X} r7 {:08X}", input.r3.u32, input.r4.u32,
                        input.r5.u32, input.r6.u32, input.r7.u32);
  Record(s, ok, true, Ns(t0, t1), Ns(t2, t3), what);
}

#if defined(__aarch64__)

inline uint32_t Load32(uint8_t* base, uint32_t a) { return Be32(Host(base, a), 0); }

inline float32x4_t LoadV(uint8_t* base, uint32_t a) {
  return vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(Host(base, a & ~15u))));
}

inline void StoreV(uint8_t* base, uint32_t a, float32x4_t v) {
  vst1q_u8(Host(base, a & ~15u), vrev32q_u8(vreinterpretq_u8_f32(v)));
}

inline uint32_t LvlxWord0(uint8_t* base, uint32_t ea) {
  const uint32_t sh = ea & 15;
  if (sh <= 12) return Be32(Host(base, ea), 0);
  uint32_t v = 0;
  for (uint32_t i = 0; i < 4; ++i) v = v << 8 | (i < 16 - sh ? Host(base, ea + i)[0] : 0u);
  return v;
}

inline uint16_t Load16(uint8_t* base, uint32_t a) {
  uint16_t v;
  std::memcpy(&v, Host(base, a), 2);
  return __builtin_bswap16(v);
}

inline float32x4_t MulAdd(float32x4_t a, float32x4_t b, float32x4_t c) { return vaddq_f32(vmulq_f32(a, b), c); }

void Curve3(PPCContext& ctx, uint8_t* base) {
  const uint32_t out = ctx.r3.u32, curves = ctx.r4.u32, src = ctx.r5.u32;
  const uint32_t n = Load32(base, curves + 4), knots = Load32(base, curves + 8);
  const uint32_t values = Load32(base, src), table = Load32(base, src + 4);
  const uint32_t groups = n >> 2, segments = knots - 1;
  uint32_t keys = curves + 16, data = curves + 16 + n * 4;
  float32x4_t ax = vdupq_n_f32(0.0f), ay = ax, az = ax;
  for (uint32_t g = 0; g < groups; ++g, keys += 16) {
    uint32_t e[4];
    for (uint32_t i = 0; i < 4; ++i) {
      const uint32_t at = Load16(base, table + 2u * Load16(base, keys + 4 * i)) + Load16(base, keys + 4 * i + 2);
      e[i] = LvlxWord0(base, values + at);
    }
    uint32x4_t raw = vld1q_u32(e);
    const float32x4_t t = vreinterpretq_f32_u32(vbicq_u32(raw, vceqq_u32(raw, vdupq_n_u32(0xFFFFFFFFu))));
    float32x4_t x = MulAdd(LoadV(base, data), t, LoadV(base, data + 48));
    float32x4_t y = MulAdd(LoadV(base, data + 16), t, LoadV(base, data + 64));
    float32x4_t z = MulAdd(LoadV(base, data + 32), t, LoadV(base, data + 80));
    data += 96;
    for (uint32_t k = 0; k < segments; ++k, data += 144) {
      const float32x4_t nx = MulAdd(LoadV(base, data + 48), t, LoadV(base, data + 96));
      const float32x4_t ny = MulAdd(LoadV(base, data + 64), t, LoadV(base, data + 112));
      const float32x4_t nz = MulAdd(LoadV(base, data + 80), t, LoadV(base, data + 128));
      x = vbslq_f32(vcgtq_f32(t, LoadV(base, data)), nx, x);
      y = vbslq_f32(vcgtq_f32(t, LoadV(base, data + 16)), ny, y);
      z = vbslq_f32(vcgtq_f32(t, LoadV(base, data + 32)), nz, z);
    }
    ax = vaddq_f32(ax, x);
    ay = vaddq_f32(ay, y);
    az = vaddq_f32(az, z);
  }
  StoreV(base, out, vpaddq_f32(vpaddq_f32(ax, ay), vpaddq_f32(az, vdupq_n_f32(0.0f))));
}

#endif

}  // namespace

REX_HOOK_RAW(sub_8341DAA0) {
#if defined(__aarch64__)
  if (Enabled(REXCVAR_GET(gtav_nr_anim)) && Load32(base, ctx.r4.u32 + 8) != 0) {
    if (CheckNow()) [[unlikely]] {
      Differential(g_curve3, ctx, base, __imp__sub_8341DAA0, {{ctx.r3.u32 & ~15u, 16}}, SameResult, Curve3);
      return;
    }
    Curve3(ctx, base);
    return;
  }
#endif
  __imp__sub_8341DAA0(ctx, base);
}

namespace {

#if defined(__aarch64__)

constexpr uint32_t kUnpackMasks = 0x820B48A0u;
constexpr uint64_t kUnpackMaxSteps = 1u << 20;
constexpr uint32_t kUnpackMaxPairs = 1u << 16;

struct UnpackSetup {
  uint32_t k;
  uint32_t q;
  uint32_t blocks;
  uint32_t rem32;
  uint64_t rem64;
  uint32_t pairs;
  uint32_t odd;
};

struct UnpackTrack {
  uint32_t obj;
  uint32_t pos;
  uint32_t cur;
  uint32_t next;
  uint32_t c;
  uint32_t mask;
  uint32_t high;
};

inline UnpackSetup UnpackPrepare(const PPCContext& ctx, uint8_t* base) {
  UnpackSetup s;
  s.k = Load32(base, ctx.r1.u32 + 84);
  const uint32_t idx = ctx.r6.u32;
  s.q = s.k ? idx / s.k : 0;
  s.blocks = s.k ? (s.k - 1 + ctx.r10.u32) / s.k : 0;
  s.rem64 = ctx.r6.u64 - uint64_t(int64_t(int32_t(s.q)) * int64_t(int32_t(s.k)));
  s.rem32 = uint32_t(s.rem64);
  s.pairs = ctx.r3.u32 >> 1;
  s.odd = ctx.r3.u32 & 1;
  return s;
}

inline bool UnpackRunnable(const UnpackSetup& s) {
  const uint64_t pair_steps = s.rem64 + 1;
  const uint64_t single_steps = (s.pairs ? uint64_t(s.rem32) : s.rem64) + 1;
  if (s.pairs > kUnpackMaxPairs) return false;
  if (s.pairs && (pair_steps == 0 || pair_steps > kUnpackMaxSteps)) return false;
  if (s.odd && (single_steps == 0 || single_steps > kUnpackMaxSteps)) return false;
  return true;
}

inline uint32_t UnpackBits(uint8_t* base, uint32_t obj, uint32_t pos) {
  uint64_t v;
  std::memcpy(&v, Host(base, obj + ((pos >> 3) & 0x1FFFFFFCu)), 8);
  v = __builtin_bswap64(v);
  return uint32_t(((v << 32) | (v >> 32)) >> (pos & 31));
}

inline uint32_t UnpackRice(uint8_t* base, UnpackTrack& t) {
  const uint32_t w1 = UnpackBits(base, t.obj, t.pos + t.c);
  const uint32_t w0 = UnpackBits(base, t.obj, t.pos);
  const uint32_t n = w1 ? uint32_t(__builtin_ctz(w1)) : 0xFFFFFFFFu;
  const uint32_t mag = (w0 & t.mask) | ((n << (t.c & 0x1F)) & t.high);
  const uint32_t sign = uint32_t(uint64_t(w1) >> ((n + 1) & 63)) & 1u;
  t.pos += n + uint32_t(mag != 0) + t.c + 1;
  return (mag ^ (0u - sign)) + sign;
}

inline void UnpackPair(uint8_t* base, UnpackTrack& x, UnpackTrack& y, uint64_t steps, uint32_t& px, uint32_t& py) {
  uint32_t vx = 0, vy = 0;
  for (uint64_t k = 0; k < steps; ++k) {
    vx += UnpackRice(base, x);
    vy += UnpackRice(base, y);
    px = x.cur;
    py = y.cur;
    x.cur += vx;
    y.cur += vy;
  }
}

inline void UnpackSingle(uint8_t* base, UnpackTrack& x, uint64_t steps, uint32_t& px) {
  uint32_t vx = 0;
  for (uint64_t k = 0; k < steps; ++k) {
    vx += UnpackRice(base, x);
    px = x.cur;
    x.cur += vx;
  }
}

inline UnpackTrack UnpackOpen(uint8_t* base, uint32_t obj, const UnpackSetup& s) {
  const uint32_t desc = Load32(base, obj + 4);
  const uint32_t a = desc & 0xFF, b = (desc >> 8) & 0xFF, c = (desc >> 16) & 0xFF;
  const uint32_t section_b = a * s.blocks + 128;
  const uint32_t pb = b * s.q + section_b;
  const uint32_t mask_b = Load32(base, kUnpackMasks + 4 * b);
  UnpackTrack t;
  t.obj = obj;
  t.pos = (UnpackBits(base, obj, a * s.q + 128) & Load32(base, kUnpackMasks + 4 * a)) + b * s.blocks + section_b;
  t.cur = UnpackBits(base, obj, pb) & mask_b;
  t.next = UnpackBits(base, obj, pb + b) & mask_b;
  t.c = c;
  t.mask = Load32(base, kUnpackMasks + 4 * c);
  t.high = (c & 0x20) ? 0u : 0xFFFFFFFFu;
  return t;
}

inline double UnpackFloat(uint8_t* base, uint32_t a) {
  const uint32_t bits = Load32(base, a);
  float f;
  std::memcpy(&f, &bits, 4);
  return double(f);
}

inline void UnpackStoreFloat(uint8_t* base, uint32_t a, float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, 4);
  bits = __builtin_bswap32(bits);
  std::memcpy(Host(base, a), &bits, 4);
}

inline void UnpackStore32(uint8_t* base, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(Host(base, a), &v, 4);
}

inline uint32_t UnpackSlot(uint8_t* base, uint32_t table, uint32_t h) {
  return uint32_t(Load16(base, table + ((h >> 1) & 0x7FFFFFFEu))) + ((h << 2) & 0xCu);
}

inline float UnpackValue(uint32_t prev, uint32_t cur, double t, double scale, double offset) {
  const double c = double(float(double(int64_t(int32_t(cur)))));
  const double p = double(float(double(int64_t(int32_t(prev)))));
  const double d = double(float(c - p));
  const double l = double(float(std::fma(d, t, p)));
  return float(std::fma(l, scale, offset));
}

void Unpack8329BDA0(PPCContext& ctx, uint8_t* base) {
  const UnpackSetup s = UnpackPrepare(ctx, base);
  const uint32_t sp = ctx.r1.u32, out = ctx.r8.u32, table = ctx.r9.u32;
  const double t = ctx.f1.f64;
  const bool last = s.rem32 == s.k - 1;
  uint32_t obj = ctx.r4.u32, list = ctx.r5.u32;
  UnpackStore32(base, sp + 20, ctx.r3.u32);
  UnpackStore32(base, sp + 36, list);
  UnpackStore32(base, sp + 60, out);
  UnpackStore32(base, sp + 68, table);
  const uint64_t pair_steps = s.rem64 + 1;
  for (uint32_t i = 0; i < s.pairs; ++i) {
    const uint32_t obj2 = obj + (Load32(base, obj) << 2);
    UnpackTrack x = UnpackOpen(base, obj, s);
    UnpackTrack y = UnpackOpen(base, obj2, s);
    uint32_t px = 0, py = 0;
    UnpackPair(base, x, y, pair_steps, px, py);
    if (last) {
      x.cur = x.next;
      y.cur = y.next;
    }
    const double sx = UnpackFloat(base, obj + 8), ox = UnpackFloat(base, obj + 12);
    const double sy = UnpackFloat(base, obj2 + 8), oy = UnpackFloat(base, obj2 + 12);
    const uint32_t hx = Load16(base, list), hy = Load16(base, list + 2);
    list += 4;
    UnpackStore32(base, sp + 36, list);
    const uint32_t at_x = UnpackSlot(base, table, hx), at_y = UnpackSlot(base, table, hy);
    obj = obj2 + (Load32(base, obj2) << 2);
    UnpackStoreFloat(base, out + at_x, UnpackValue(px, x.cur, t, sx, ox));
    UnpackStoreFloat(base, out + at_y, UnpackValue(py, y.cur, t, sy, oy));
  }
  if (!s.odd) return;
  const uint64_t single_steps = (s.pairs ? uint64_t(s.rem32) : s.rem64) + 1;
  UnpackTrack x = UnpackOpen(base, obj, s);
  uint32_t px = 0;
  UnpackSingle(base, x, single_steps, px);
  if (last) x.cur = x.next;
  const uint32_t h = Load16(base, list);
  UnpackStoreFloat(base, out + UnpackSlot(base, table, h),
                   UnpackValue(px, x.cur, t, UnpackFloat(base, obj + 8), UnpackFloat(base, obj + 12)));
}

bool UnpackSpan(const PPCContext& ctx, uint8_t* base, const UnpackSetup& s, uint32_t& lo, uint32_t& size) {
  const uint32_t n = 2 * s.pairs + s.odd;
  if (!n) {
    lo = ctx.r8.u32;
    size = 0;
    return true;
  }
  uint32_t min_at = 0xFFFFFFFFu, max_at = 0;
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t at = UnpackSlot(base, ctx.r9.u32, Load16(base, ctx.r5.u32 + 2 * i));
    min_at = std::min(min_at, at);
    max_at = std::max(max_at, at);
  }
  lo = ctx.r8.u32 + min_at;
  size = max_at - min_at + 4;
  return size <= (1u << 20) && Flat(lo, size);
}

bool UnpackSameRegs(const PPCContext& a, const PPCContext& b) {
  return a.r1.u64 == b.r1.u64 && a.r8.u32 == b.r8.u32 && a.r9.u32 == b.r9.u32 && a.f1.u64 == b.f1.u64;
}

#endif

}  // namespace

REX_HOOK_RAW(sub_8329BDA0) {
#if defined(__aarch64__)
  if (Enabled(REXCVAR_GET(gtav_nr_unpack))) {
    const UnpackSetup s = UnpackPrepare(ctx, base);
    if (UnpackRunnable(s)) {
      if (!CheckNow()) [[likely]] {
        Unpack8329BDA0(ctx, base);
        return;
      }
      uint32_t lo = 0, size = 0;
      if (UnpackSpan(ctx, base, s, lo, size)) {
        Differential(g_unpack, ctx, base, __imp__sub_8329BDA0, {{ctx.r1.u32 + 20, 52}, {lo, size}}, UnpackSameRegs,
                     Unpack8329BDA0);
        return;
      }
    }
  }
#endif
  __imp__sub_8329BDA0(ctx, base);
}

namespace {

#if defined(__aarch64__)

inline void NrStore32(uint8_t* base, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(Host(base, a), &v, 4);
}

inline float32x4_t NrBits(uint32_t v) { return vreinterpretq_f32_u32(vdupq_n_u32(v)); }

inline float32x4_t NrNmsub(float32x4_t a, float32x4_t b, float32x4_t c) {
  return vreinterpretq_f32_u32(
      veorq_u32(vreinterpretq_u32_f32(vsubq_f32(vmulq_f32(a, b), c)), vdupq_n_u32(0x80000000u)));
}

inline uint32x4_t NrGt(float32x4_t a, float32x4_t b) { return vcgtq_f32(a, b); }

inline uint32x4_t NrCell(float32x4_t q, float32x4_t x, float32x4_t y, float32x4_t z) {
  return vandq_u32(vandq_u32(NrGt(x, vdupq_laneq_f32(q, 0)), NrGt(y, vdupq_laneq_f32(q, 1))),
                   NrGt(z, vdupq_laneq_f32(q, 2)));
}

inline void NrVolumeCell(uint8_t* base, uint32_t at, float32x4_t x, float32x4_t y, float32x4_t z, float32x4_t w,
                         float32x4_t a0, float32x4_t a1, float32x4_t a2, float32x4_t a3) {
  uint8_t* p = Host(base, at);
  const float32x4_t c = vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(p)));
  const float32x4_t q = vsubq_f32(w, a3);
  const uint32x4_t m = vandq_u32(vandq_u32(NrGt(x, a0), NrGt(y, a1)), vandq_u32(NrGt(z, a2), NrGt(q, c)));
  vst1q_u8(p, vrev32q_u8(vreinterpretq_u8_f32(vbslq_f32(m, q, c))));
}

__attribute__((noinline)) void NrVolumeRows(uint8_t* base, uint32_t cell, const float32x4_t* rows, float32x4_t a0, float32x4_t a1,
                         float32x4_t a2, float32x4_t a3, float32x4x4_t d) {
  const float32x4_t x0 = vdupq_laneq_f32(rows[0], 0), y0 = vdupq_laneq_f32(rows[0], 1), z0 = vdupq_laneq_f32(rows[0], 2),
                    w0 = vdupq_laneq_f32(rows[0], 3);
  const float32x4_t x1 = vdupq_laneq_f32(rows[1], 0), y1 = vdupq_laneq_f32(rows[1], 1), z1 = vdupq_laneq_f32(rows[1], 2),
                    w1 = vdupq_laneq_f32(rows[1], 3);
  const float32x4_t x2 = vdupq_laneq_f32(rows[2], 0), y2 = vdupq_laneq_f32(rows[2], 1), z2 = vdupq_laneq_f32(rows[2], 2),
                    w2 = vdupq_laneq_f32(rows[2], 3);
  const float32x4_t x3 = vdupq_laneq_f32(rows[3], 0), y3 = vdupq_laneq_f32(rows[3], 1), z3 = vdupq_laneq_f32(rows[3], 2),
                    w3 = vdupq_laneq_f32(rows[3], 3);
  for (int t = 0; t < 6; ++t, cell += 512) {
    NrVolumeCell(base, cell, x0, y0, z0, w0, a0, a1, a2, a3);
    NrVolumeCell(base, cell + 16, x1, y1, z1, w1, a0, a1, a2, a3);
    NrVolumeCell(base, cell + 32, x2, y2, z2, w2, a0, a1, a2, a3);
    NrVolumeCell(base, cell + 48, x3, y3, z3, w3, a0, a1, a2, a3);
    a0 = vaddq_f32(a0, d.val[0]);
    a1 = vaddq_f32(a1, d.val[1]);
    a2 = vaddq_f32(a2, d.val[2]);
    a3 = vaddq_f32(a3, d.val[3]);
  }
}

void Native8315B8D0(PPCContext& ctx, uint8_t* base) {
  const uint32_t keys = ctx.r3.u32, entries = ctx.r5.u32, grid = ctx.r6.u32;
  const int32_t count = ctx.r4.s32;
  NrStore32(base, ctx.r1.u32 + 36, entries);
  uint32_t guard = Load32(base, 0x83CC0540u);
  float32x4_t s17, s31;
  if (!(guard & 1)) {
    guard |= 1;
    NrStore32(base, 0x83CC0540u, guard);
    s17 = NrBits(LvlxWord0(base, 0x82023AD4u));
    StoreV(base, 0x83CC0530u, s17);
  } else {
    s17 = LoadV(base, 0x83CC0530u);
  }
  if (!(guard & 2)) {
    guard |= 2;
    NrStore32(base, 0x83CC0540u, guard);
    s31 = NrBits(LvlxWord0(base, 0x8206FC18u));
    StoreV(base, 0x83CC0520u, s31);
  } else {
    s31 = LoadV(base, 0x83CC0520u);
  }
  const uint32_t cbits[4] = {LvlxWord0(base, 0x82000E44u), LvlxWord0(base, 0x82023AD4u),
                             LvlxWord0(base, 0x82025208u), LvlxWord0(base, 0x82080E40u)};
  uint32_t step_bits = Load32(base, 0x8206FC18u);
  float step;
  std::memcpy(&step, &step_bits, 4);
  float fcol[4];
  for (int i = 0; i < 4; ++i)
    fcol[i] = float(double(float(double(float(double(int64_t(i)))) * double(step))));
  const float32x4_t vx = NrBits(ctx.v1.u32[3]);
  const float32x4_t vy = NrBits(ctx.v1.u32[2]);
  const float32x4_t c0 = LoadV(base, 0x820E6500u), c1 = LoadV(base, 0x820A8EB0u), c2 = LoadV(base, 0x820A8EC0u),
                    c3 = LoadV(base, 0x820A8F20u);
  const float32x4_t v120 = vmulq_f32(c0, s17);
  const float32x4_t v115 = vaddq_f32(vmulq_f32(c0, s31), vy);
  const float32x4_t v122 = vaddq_f32(c1, vx);
  const float32x4_t v121 = vaddq_f32(c2, vy);
  const float32x4_t v117 = vaddq_f32(vmulq_f32(c3, s17), vx);
  const float32x4_t v116 = vaddq_f32(vmulq_f32(c3, s31), vy);
  const float32x4_t v114 = vaddq_f32(vx, v120);
  if (count <= 0) return;
  const float32x4_t zero = vreinterpretq_f32_u32(vdupq_n_u32(0));
  const float32x4_t two = vdupq_n_f32(2.0f), three = vdupq_n_f32(3.0f), four = vdupq_n_f32(4.0f),
                    five = vdupq_n_f32(5.0f), six = vdupq_n_f32(6.0f), seven = vdupq_n_f32(7.0f);
  for (int32_t k = 0; k < count; ++k) {
    const uint32_t e = entries + (uint32_t(Load16(base, keys + 2u * uint32_t(k))) << 6);
    const float32x4_t e0 = LoadV(base, e), e1 = LoadV(base, e + 16), e2 = LoadV(base, e + 32),
                      e3 = LoadV(base, e + 48);
    const float32x4_t v4 = vsubq_f32(zero, e2);
    const float32x4_t v30 = vsubq_f32(zero, e1);
    const float32x4_t v38 = vmulq_f32(e1, s31);
    const float32x4_t v27 = vbslq_f32(NrGt(zero, e2), s17, zero);
    const float32x4_t v26 = vbslq_f32(NrGt(zero, e1), zero, s31);
    const float32x4_t v127 = NrNmsub(v121, vdupq_laneq_f32(e1, 0), vmulq_f32(vdupq_laneq_f32(e2, 0), v122));
    const float32x4_t v126 = NrNmsub(v121, vdupq_laneq_f32(e1, 1), vmulq_f32(vdupq_laneq_f32(e2, 1), v122));
    const float32x4_t v125 = NrNmsub(v121, vdupq_laneq_f32(e1, 2), vmulq_f32(vdupq_laneq_f32(e2, 2), v122));
    const float32x4_t v124 = NrNmsub(v121, vdupq_laneq_f32(e1, 3), vmulq_f32(vdupq_laneq_f32(e2, 3), v122));
    const float32x4_t px = vaddq_f32(vmulq_f32(v120, vdupq_laneq_f32(v4, 0)), vdupq_laneq_f32(e0, 0));
    const float32x4_t py = vaddq_f32(vmulq_f32(v120, vdupq_laneq_f32(v4, 1)), vdupq_laneq_f32(e0, 1));
    const float32x4_t pz = vaddq_f32(vmulq_f32(v120, vdupq_laneq_f32(v4, 2)), vdupq_laneq_f32(e0, 2));
    const float32x4_t v12 = vaddq_f32(vmulq_f32(vy, e1), vmulq_f32(vx, v4));
    float32x4_t q1 = vaddq_f32(vmulq_f32(v30, v26), vmulq_f32(e2, v27));
    q1 = vsubq_f32(q1, v12);
    const float32x4_t q0 = vsubq_f32(q1, v38);
    const float32x4_t v13 = vsubq_f32(zero, v38);
    const float32x4_t q2 = vaddq_f32(vmulq_f32(v13, two), q1);
    const float32x4_t q3 = vaddq_f32(vmulq_f32(v13, three), q1);
    const uint32x4_t lanes =
        vandq_u32(NrGt(v117, vdupq_laneq_f32(e3, 0)), NrGt(vdupq_laneq_f32(e3, 1), v114));
    const uint32x4_t slabs =
        vandq_u32(NrGt(v116, vdupq_laneq_f32(e3, 2)), NrGt(vdupq_laneq_f32(e3, 3), v115));
    uint32_t hits[16];
    vst1q_u32(hits, vandq_u32(vandq_u32(NrCell(q1, px, py, pz), lanes), vdupq_laneq_u32(slabs, 0)));
    vst1q_u32(hits + 4, vandq_u32(vandq_u32(NrCell(q0, px, py, pz), lanes), vdupq_laneq_u32(slabs, 1)));
    vst1q_u32(hits + 8, vandq_u32(vandq_u32(NrCell(q2, px, py, pz), lanes), vdupq_laneq_u32(slabs, 2)));
    vst1q_u32(hits + 12, vandq_u32(vandq_u32(NrCell(q3, px, py, pz), lanes), vdupq_laneq_u32(slabs, 3)));
    const float32x4_t neg = vsubq_f32(zero, e1);
    const float32x4x4_t steps = {{vdupq_laneq_f32(neg, 0), vdupq_laneq_f32(neg, 1), vdupq_laneq_f32(neg, 2),
                                  vdupq_laneq_f32(neg, 3)}};
    for (uint32_t j = 0; j < 16; ++j) {
      if (!hits[j]) continue;
      const float32x4_t r0 =
          NrNmsub(e2, NrBits(cbits[j & 3]), vaddq_f32(vmulq_f32(e1, vdupq_n_f32(fcol[j >> 2])), e0));
      const float32x4_t rows[8] = {r0,
                                   vaddq_f32(r0, v4),
                                   vaddq_f32(vmulq_f32(v4, two), r0),
                                   vaddq_f32(vmulq_f32(v4, three), r0),
                                   vaddq_f32(vmulq_f32(v4, four), r0),
                                   vaddq_f32(vmulq_f32(v4, five), r0),
                                   vaddq_f32(vmulq_f32(v4, six), r0),
                                   vaddq_f32(vmulq_f32(v4, seven), r0)};
      const uint32_t cell = (grid & ~15u) + 3072u * (j >> 2) + 128u * (j & 3);
      NrVolumeRows(base, cell, rows, v127, v126, v125, v124, steps);
      NrVolumeRows(base, cell + 64, rows + 4, v127, v126, v125, v124, steps);
    }
  }
}

#endif

}  // namespace

namespace {

bool SameStack(const PPCContext& a, const PPCContext& b) { return a.r1.u64 == b.r1.u64; }

}  // namespace

REX_HOOK_RAW(sub_8315B8D0) {
#if defined(__aarch64__)
  if (Enabled(REXCVAR_GET(gtav_nr_volume))) {
    if (CheckNow()) [[unlikely]] {
      Differential(g_volume, ctx, base, __imp__sub_8315B8D0,
                   {{ctx.r6.u32 & ~15u, 12288}, {0x83CC0520u, 36}, {ctx.r1.u32 + 36, 4}}, SameStack,
                   Native8315B8D0);
      return;
    }
    Native8315B8D0(ctx, base);
    return;
  }
#endif
  __imp__sub_8315B8D0(ctx, base);
}

namespace {

#if defined(__aarch64__)

inline uint32x4_t QuatLoad(uint8_t* base, uint32_t a) {
  return vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(Host(base, a & ~15u))));
}

inline uint32x4_t QuatAt(const uint8_t* p, uint32_t off) {
  return vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p + off)));
}

inline float32x4_t QuatF(uint32x4_t v) { return vreinterpretq_f32_u32(v); }

inline uint32x4_t QuatU(float32x4_t v) { return vreinterpretq_u32_f32(v); }

inline uint32x4_t QuatMulAdd(uint32x4_t a, uint32x4_t b, uint32x4_t c) {
  return QuatU(vaddq_f32(vmulq_f32(QuatF(a), QuatF(b)), QuatF(c)));
}

inline uint32x4_t QuatRev(uint32x4_t v) {
  const uint32x4_t r = vrev64q_u32(v);
  return vextq_u32(r, r, 2);
}

inline uint32x4_t QuatDot(uint32x4_t a, uint32x4_t b) {
  const float32x4_t p = vmulq_f32(QuatF(a), QuatF(b));
  const float32x2_t s = vpadd_f32(vrev64_f32(vget_high_f32(p)), vrev64_f32(vget_low_f32(p)));
  return QuatU(vdupq_n_f32(vpadds_f32(s)));
}

inline uint32x4_t QuatRlimi11(uint32x4_t d, uint32x4_t b) { return vcopyq_laneq_u32(b, 1, d, 1); }

inline uint32x4_t QuatRlimi1(uint32x4_t d, uint32x4_t b) { return vcopyq_laneq_u32(d, 3, b, 3); }

inline uint32x4_t QuatRlimi13(uint32x4_t d, uint32x4_t b) { return vcopyq_laneq_u32(b, 2, d, 2); }

inline uint32x4_t QuatRlimi7(uint32x4_t d, uint32x4_t b) { return vcopyq_laneq_u32(b, 0, d, 0); }

inline uint64_t QuatSpan(uint32_t n, uint32_t knots) { return uint64_t(n >> 2) * (96u + 144u * uint64_t(knots - 1)); }

inline bool QuatRunnable(uint8_t* base, const PPCContext& ctx) {
  const uint32_t curves = ctx.r4.u32;
  const uint32_t n = Load32(base, curves + 4), knots = Load32(base, curves + 8);
  if (knots == 0) return false;
  const uint64_t span = QuatSpan(n, knots);
  return span <= (64u << 20) && Flat((curves + 16 + n * 4) & ~15u, uint32_t(span));
}

void Quat83420D18(PPCContext& ctx, uint8_t* base) {
  const uint32_t out = ctx.r3.u32, curves = ctx.r4.u32, src = ctx.r5.u32;
  const uint32_t n = Load32(base, curves + 4), knots = Load32(base, curves + 8);
  const uint32_t values = Load32(base, src), table = Load32(base, src + 4);
  const uint32_t groups = n >> 2, segments = knots - 1;
  uint32_t keys = curves + 16;
  const uint8_t* d = Host(base, (curves + 16 + n * 4) & ~15u);
  const uint32x4_t sign = vdupq_n_u32(0x80000000u);
  uint32x4_t v63 = vdupq_n_u32(0);
  uint32x4_t v9 = v63, v8 = v63, v7 = v63;
  uint32x4_t v60 = QuatU(vdupq_n_f32(1.0f));
  if (groups) {
    const uint32x4_t v0 = vdupq_n_u32(1), v59 = vdupq_n_u32(0xFFFFFFFFu), v62 = vdupq_n_u32(2),
                     v55 = vdupq_n_u32(3), v19 = sign, v20 = v60;
    const uint32x4_t v61 = QuatU(vdupq_n_f32(0.5f));
    const uint32x4_t v21 = QuatLoad(base, 0x82001540u), v22 = QuatLoad(base, 0x82001530u),
                     v23 = QuatLoad(base, 0x82001510u), v57 = QuatLoad(base, 0x820014D0u),
                     v58 = QuatLoad(base, 0x820014B0u), v24 = QuatLoad(base, 0x82001520u),
                     v25 = QuatLoad(base, 0x820014F0u), v26 = QuatLoad(base, 0x82001500u),
                     v56 = QuatLoad(base, 0x820014E0u);
    uint32x4_t v1, v2, v3, v4, v5, v6, v10, v11, v12, v13, v15, v16, v17, v18, v27, v28, v29, v30, v31, v32, v33,
        v34, v35, v36, v37, v38, v39, v40, v41, v42, v43, v44, v45, v46, v47, v48, v49, v50, v51, v52, v53, v54;
    for (uint32_t g = 0; g < groups; ++g, keys += 16) {
      uint32_t e[4];
      for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t at = Load16(base, table + 2u * Load16(base, keys + 4 * i)) + Load16(base, keys + 4 * i + 2);
        e[i] = LvlxWord0(base, values + at);
      }
      const uint32x4_t raw = vld1q_u32(e);
      const uint32x4_t t = vbicq_u32(raw, vceqq_u32(raw, v59));
      v53 = QuatMulAdd(QuatAt(d, 0), t, QuatAt(d, 48));
      v52 = QuatMulAdd(QuatAt(d, 16), t, QuatAt(d, 64));
      v54 = QuatMulAdd(QuatAt(d, 32), t, QuatAt(d, 80));
      d += 96;
      for (uint32_t k = 0; k < segments; ++k, d += 144) {
        const uint32x4_t nx = QuatMulAdd(QuatAt(d, 48), t, QuatAt(d, 96));
        const uint32x4_t ny = QuatMulAdd(QuatAt(d, 64), t, QuatAt(d, 112));
        const uint32x4_t nz = QuatMulAdd(QuatAt(d, 80), t, QuatAt(d, 128));
        v53 = vbslq_u32(vcgtq_f32(QuatF(t), QuatF(QuatAt(d, 0))), nx, v53);
        v52 = vbslq_u32(vcgtq_f32(QuatF(t), QuatF(QuatAt(d, 16))), ny, v52);
        v54 = vbslq_u32(vcgtq_f32(QuatF(t), QuatF(QuatAt(d, 32))), nz, v54);
      }
      v29 = vreinterpretq_u32_f32(vmulq_f32(vreinterpretq_f32_u32(v61), vreinterpretq_f32_u32(v53)));
      v11 = vshlq_u32(v59, vreinterpretq_s32_u32(vandq_u32(v59, vdupq_n_u32(31))));
      v13 = v61;
      v28 = vreinterpretq_u32_f32(vmulq_f32(vreinterpretq_f32_u32(v61), vreinterpretq_f32_u32(v52)));
      v6 = v61;
      v27 = vreinterpretq_u32_f32(vmulq_f32(vreinterpretq_f32_u32(v61), vreinterpretq_f32_u32(v54)));
      v10 = vshlq_u32(v59, vreinterpretq_s32_u32(vandq_u32(v59, vdupq_n_u32(31))));
      v4 = v57;
      v2 = v56;
      v1 = v61;
      v31 = v57;
      v30 = v56;
      v3 = v57;
      v39 = vreinterpretq_u32_f32(vmulq_f32(vreinterpretq_f32_u32(v29), vreinterpretq_f32_u32(v58)));
      v38 = vreinterpretq_u32_f32(vmulq_f32(vreinterpretq_f32_u32(v28), vreinterpretq_f32_u32(v58)));
      v37 = vreinterpretq_u32_f32(vmulq_f32(vreinterpretq_f32_u32(v27), vreinterpretq_f32_u32(v58)));
      v12 = v39;
      v5 = v38;
      v18 = vbslq_u32(v11, v12, v13);
      v13 = vbslq_u32(v10, v5, v6);
      v36 = vreinterpretq_u32_f32(vaddq_f32(vreinterpretq_f32_u32(v39), vreinterpretq_f32_u32(v18)));
      v35 = vreinterpretq_u32_f32(vaddq_f32(vreinterpretq_f32_u32(v38), vreinterpretq_f32_u32(v13)));
      v34 = vreinterpretq_u32_s32(vcvtq_s32_f32(vreinterpretq_f32_u32(v36)));
      v33 = vreinterpretq_u32_s32(vcvtq_s32_f32(vreinterpretq_f32_u32(v35)));
      v5 = vreinterpretq_u32_f32(vcvtq_f32_s32(vreinterpretq_s32_u32(v34)));
      v13 = vandq_u32(v34, v55);
      v6 = vandq_u32(v33, v55);
      v32 = vreinterpretq_u32_f32(vcvtq_f32_s32(vreinterpretq_s32_u32(v33)));
      v12 = vaddq_u32(v0, v13);
      v54 = vandq_u32(v13, v0);
      v53 = vandq_u32(v13, v62);
      v16 = vaddq_u32(v0, v6);
      v51 = vandq_u32(v12, v0);
      v50 = vandq_u32(v12, v62);
      v18 = vceqq_u32(v54, v63);
      v52 = v16;
      v12 = veorq_u32(vreinterpretq_u32_f32(vsubq_f32(vmulq_f32(vreinterpretq_f32_u32(v4), vreinterpretq_f32_u32(v5)), vreinterpretq_f32_u32(v29))), sign);
      v29 = vceqq_u32(v51, v63);
      v17 = vceqq_u32(v53, v63);
      v16 = vceqq_u32(v50, v63);
      v49 = vandq_u32(v6, v0);
      v48 = vandq_u32(v6, v62);
      v41 = vandq_u32(v52, v0);
      v40 = vandq_u32(v52, v62);
      v12 = veorq_u32(vreinterpretq_u32_f32(vsubq_f32(vmulq_f32(vreinterpretq_f32_u32(v2), vreinterpretq_f32_u32(v5)), vreinterpretq_f32_u32(v12))), sign);
      v13 = vreinterpretq_u32_f32(vmulq_f32(vreinterpretq_f32_u32(v12), vreinterpretq_f32_u32(v12)));
      v5 = vreinterpretq_u32_f32(vaddq_f32(vmulq_f32(vreinterpretq_f32_u32(v26), vreinterpretq_f32_u32(v13)), vreinterpretq_f32_u32(v25)));
      v4 = vreinterpretq_u32_f32(vaddq_f32(vmulq_f32(vreinterpretq_f32_u32(v24), vreinterpretq_f32_u32(v13)), vreinterpretq_f32_u32(v23)));
      v2 = vreinterpretq_u32_f32(vmulq_f32(vreinterpretq_f32_u32(v13), vreinterpretq_f32_u32(v12)));
      v5 = vreinterpretq_u32_f32(vaddq_f32(vmulq_f32(vreinterpretq_f32_u32(v5), vreinterpretq_f32_u32(v13)), vreinterpretq_f32_u32(v22)));
      v4 = vreinterpretq_u32_f32(vaddq_f32(vmulq_f32(vreinterpretq_f32_u32(v4), vreinterpretq_f32_u32(v13)), vreinterpretq_f32_u32(v21)));
      v13 = vreinterpretq_u32_f32(vaddq_f32(vmulq_f32(vreinterpretq_f32_u32(v5), vreinterpretq_f32_u32(v13)), vreinterpretq_f32_u32(v20)));
      v12 = vreinterpretq_u32_f32(vaddq_f32(vmulq_f32(vreinterpretq_f32_u32(v4), vreinterpretq_f32_u32(v2)), vreinterpretq_f32_u32(v12)));
      v47 = v13;
      v46 = v12;
      v2 = vbslq_u32(v18, v12, v13);
      v12 = v37;
      v5 = v47;
      v4 = v46;
      v13 = vbslq_u32(v29, v4, v5);
      v29 = vceqq_u32(v41, v63);
      v45 = v13;
      v13 = veorq_u32(v11, v2);
      v11 = veorq_u32(v11, v45);
      v5 = v45;
      v4 = vbslq_u32(v17, v2, v13);
      v13 = vbslq_u32(v16, v5, v11);
      v5 = vbslq_u32(v19, v12, v1);
      v11 = v32;
      v44 = v4;
      v43 = v13;
      v42 = vreinterpretq_u32_f32(vaddq_f32(vreinterpretq_f32_u32(v37), vreinterpretq_f32_u32(v5)));
      v13 = v32;
      v12 = veorq_u32(vreinterpretq_u32_f32(vsubq_f32(vmulq_f32(vreinterpretq_f32_u32(v31), vreinterpretq_f32_u32(v11)), vreinterpretq_f32_u32(v28))), sign);
      v4 = v56;
      v28 = vceqq_u32(v49, v63);
      v31 = vceqq_u32(v48, v63);
      v1 = vceqq_u32(v40, v63);
      v39 = vreinterpretq_u32_s32(vcvtq_s32_f32(vreinterpretq_f32_u32(v42)));
      v11 = veorq_u32(vreinterpretq_u32_f32(vsubq_f32(vmulq_f32(vreinterpretq_f32_u32(v30), vreinterpretq_f32_u32(v13)), vreinterpretq_f32_u32(v12))), sign);
      v13 = vreinterpretq_u32_f32(vmulq_f32(vreinterpretq_f32_u32(v11), vreinterpretq_f32_u32(v11)));
      v6 = vandq_u32(v39, v55);
      v5 = vreinterpretq_u32_f32(vcvtq_f32_s32(vreinterpretq_s32_u32(v39)));
      v15 = vaddq_u32(v0, v6);
      v38 = vandq_u32(v6, v0);
      v37 = vandq_u32(v6, v62);
      v36 = vandq_u32(v15, v0);
      v35 = vandq_u32(v15, v62);
      v30 = vreinterpretq_u32_f32(vaddq_f32(vmulq_f32(vreinterpretq_f32_u32(v26), vreinterpretq_f32_u32(v13)), vreinterpretq_f32_u32(v25)));
      v27 = veorq_u32(vreinterpretq_u32_f32(vsubq_f32(vmulq_f32(vreinterpretq_f32_u32(v3), vreinterpretq_f32_u32(v5)), vreinterpretq_f32_u32(v27))), sign);
      v18 = vreinterpretq_u32_f32(vaddq_f32(vmulq_f32(vreinterpretq_f32_u32(v24), vreinterpretq_f32_u32(v13)), vreinterpretq_f32_u32(v23)));
      v2 = vreinterpretq_u32_f32(vmulq_f32(vreinterpretq_f32_u32(v13), vreinterpretq_f32_u32(v11)));
      v30 = vreinterpretq_u32_f32(vaddq_f32(vmulq_f32(vreinterpretq_f32_u32(v30), vreinterpretq_f32_u32(v13)), vreinterpretq_f32_u32(v22)));
      v12 = veorq_u32(vreinterpretq_u32_f32(vsubq_f32(vmulq_f32(vreinterpretq_f32_u32(v4), vreinterpretq_f32_u32(v5)), vreinterpretq_f32_u32(v27))), sign);
      v3 = vreinterpretq_u32_f32(vaddq_f32(vmulq_f32(vreinterpretq_f32_u32(v18), vreinterpretq_f32_u32(v13)), vreinterpretq_f32_u32(v21)));
      v5 = vreinterpretq_u32_f32(vaddq_f32(vmulq_f32(vreinterpretq_f32_u32(v30), vreinterpretq_f32_u32(v13)), vreinterpretq_f32_u32(v20)));
      v13 = vreinterpretq_u32_f32(vmulq_f32(vreinterpretq_f32_u32(v12), vreinterpretq_f32_u32(v12)));
      v11 = vreinterpretq_u32_f32(vaddq_f32(vmulq_f32(vreinterpretq_f32_u32(v3), vreinterpretq_f32_u32(v2)), vreinterpretq_f32_u32(v11)));
      v18 = vreinterpretq_u32_f32(vaddq_f32(vmulq_f32(vreinterpretq_f32_u32(v26), vreinterpretq_f32_u32(v13)), vreinterpretq_f32_u32(v25)));
      v27 = vreinterpretq_u32_f32(vaddq_f32(vmulq_f32(vreinterpretq_f32_u32(v24), vreinterpretq_f32_u32(v13)), vreinterpretq_f32_u32(v23)));
      v4 = vbslq_u32(v29, v11, v5);
      v30 = vbslq_u32(v28, v11, v5);
      v2 = vreinterpretq_u32_f32(vmulq_f32(vreinterpretq_f32_u32(v13), vreinterpretq_f32_u32(v12)));
      v34 = v4;
      v4 = veorq_u32(v10, v30);
      v10 = veorq_u32(v10, v34);
      v3 = vbslq_u32(v31, v30, v4);
      v11 = v34;
      v29 = vreinterpretq_u32_f32(vaddq_f32(vmulq_f32(vreinterpretq_f32_u32(v18), vreinterpretq_f32_u32(v13)), vreinterpretq_f32_u32(v22)));
      v4 = vceqq_u32(v36, v63);
      v30 = vreinterpretq_u32_f32(vaddq_f32(vmulq_f32(vreinterpretq_f32_u32(v27), vreinterpretq_f32_u32(v13)), vreinterpretq_f32_u32(v21)));
      v32 = vreinterpretq_u32_f32(vmulq_f32(vreinterpretq_f32_u32(v43), vreinterpretq_f32_u32(v3)));
      v5 = vbslq_u32(v1, v11, v10);
      v33 = v5;
      v5 = vceqq_u32(v38, v63);
      v1 = vreinterpretq_u32_f32(vmulq_f32(vreinterpretq_f32_u32(v44), vreinterpretq_f32_u32(v33)));
      v11 = vreinterpretq_u32_f32(vaddq_f32(vmulq_f32(vreinterpretq_f32_u32(v29), vreinterpretq_f32_u32(v13)), vreinterpretq_f32_u32(v20)));
      v13 = vreinterpretq_u32_f32(vaddq_f32(vmulq_f32(vreinterpretq_f32_u32(v30), vreinterpretq_f32_u32(v2)), vreinterpretq_f32_u32(v12)));
      v54 = vreinterpretq_u32_f32(vmulq_f32(vreinterpretq_f32_u32(v43), vreinterpretq_f32_u32(v33)));
      v2 = vreinterpretq_u32_f32(vmulq_f32(vreinterpretq_f32_u32(v44), vreinterpretq_f32_u32(v3)));
      v3 = v9;
      v53 = v11;
      v52 = v13;
      v10 = v53;
      v11 = v52;
      v12 = v53;
      v6 = vbslq_u32(v4, v11, v10);
      v11 = vbslq_u32(v5, v13, v12);
      v4 = vceqq_u32(v35, v63);
      v5 = vceqq_u32(v37, v63);
      v13 = v6;
      v6 = veorq_u32(v19, v6);
      v51 = v11;
      v11 = v32;
      v4 = vbslq_u32(v4, v13, v6);
      v12 = v51;
      v10 = veorq_u32(v19, v51);
      v6 = v7;
      v50 = v4;
      v13 = vbslq_u32(v5, v12, v10);
      v5 = v8;
      v10 = vreinterpretq_u32_f32(vmulq_f32(vreinterpretq_f32_u32(v1), vreinterpretq_f32_u32(v50)));
      v29 = vreinterpretq_u32_f32(vmulq_f32(vreinterpretq_f32_u32(v54), vreinterpretq_f32_u32(v50)));
      v28 = vreinterpretq_u32_f32(vmulq_f32(vreinterpretq_f32_u32(v32), vreinterpretq_f32_u32(v50)));
      v30 = vreinterpretq_u32_f32(vmulq_f32(vreinterpretq_f32_u32(v54), vreinterpretq_f32_u32(v13)));
      v10 = veorq_u32(vreinterpretq_u32_f32(vsubq_f32(vmulq_f32(vreinterpretq_f32_u32(v11), vreinterpretq_f32_u32(v13)), vreinterpretq_f32_u32(v10))), sign);
      v12 = vreinterpretq_u32_f32(vaddq_f32(vmulq_f32(vreinterpretq_f32_u32(v2), vreinterpretq_f32_u32(v13)), vreinterpretq_f32_u32(v29)));
      v11 = vreinterpretq_u32_f32(vaddq_f32(vmulq_f32(vreinterpretq_f32_u32(v1), vreinterpretq_f32_u32(v13)), vreinterpretq_f32_u32(v28)));
      v13 = veorq_u32(vreinterpretq_u32_f32(vsubq_f32(vmulq_f32(vreinterpretq_f32_u32(v2), vreinterpretq_f32_u32(v4)), vreinterpretq_f32_u32(v30))), sign);
      v27 = vreinterpretq_u32_f32(vmulq_f32(vreinterpretq_f32_u32(v60), vreinterpretq_f32_u32(v10)));
      v29 = vreinterpretq_u32_f32(vmulq_f32(vreinterpretq_f32_u32(v60), vreinterpretq_f32_u32(v12)));
      v28 = vreinterpretq_u32_f32(vmulq_f32(vreinterpretq_f32_u32(v60), vreinterpretq_f32_u32(v11)));
      v2 = v11;
      v30 = vreinterpretq_u32_f32(vmulq_f32(vreinterpretq_f32_u32(v60), vreinterpretq_f32_u32(v13)));
      v4 = v13;
      v27 = vreinterpretq_u32_f32(vaddq_f32(vmulq_f32(vreinterpretq_f32_u32(v12), vreinterpretq_f32_u32(v9)), vreinterpretq_f32_u32(v27)));
      v29 = veorq_u32(vreinterpretq_u32_f32(vsubq_f32(vmulq_f32(vreinterpretq_f32_u32(v9), vreinterpretq_f32_u32(v10)), vreinterpretq_f32_u32(v29))), sign);
      v28 = vreinterpretq_u32_f32(vaddq_f32(vmulq_f32(vreinterpretq_f32_u32(v12), vreinterpretq_f32_u32(v8)), vreinterpretq_f32_u32(v28)));
      v12 = vreinterpretq_u32_f32(vaddq_f32(vmulq_f32(vreinterpretq_f32_u32(v12), vreinterpretq_f32_u32(v7)), vreinterpretq_f32_u32(v30)));
      v30 = vreinterpretq_u32_f32(vaddq_f32(vmulq_f32(vreinterpretq_f32_u32(v8), vreinterpretq_f32_u32(v13)), vreinterpretq_f32_u32(v27)));
      v1 = veorq_u32(vreinterpretq_u32_f32(vsubq_f32(vmulq_f32(vreinterpretq_f32_u32(v8), vreinterpretq_f32_u32(v2)), vreinterpretq_f32_u32(v29))), sign);
      v31 = vreinterpretq_u32_f32(vaddq_f32(vmulq_f32(vreinterpretq_f32_u32(v7), vreinterpretq_f32_u32(v10)), vreinterpretq_f32_u32(v28)));
      v60 = v1;
      v12 = vreinterpretq_u32_f32(vaddq_f32(vmulq_f32(vreinterpretq_f32_u32(v9), vreinterpretq_f32_u32(v11)), vreinterpretq_f32_u32(v12)));
      v9 = veorq_u32(vreinterpretq_u32_f32(vsubq_f32(vmulq_f32(vreinterpretq_f32_u32(v7), vreinterpretq_f32_u32(v11)), vreinterpretq_f32_u32(v30))), sign);
      v60 = veorq_u32(vreinterpretq_u32_f32(vsubq_f32(vmulq_f32(vreinterpretq_f32_u32(v6), vreinterpretq_f32_u32(v13)), vreinterpretq_f32_u32(v60))), sign);
      v8 = veorq_u32(vreinterpretq_u32_f32(vsubq_f32(vmulq_f32(vreinterpretq_f32_u32(v3), vreinterpretq_f32_u32(v4)), vreinterpretq_f32_u32(v31))), sign);
      v7 = veorq_u32(vreinterpretq_u32_f32(vsubq_f32(vmulq_f32(vreinterpretq_f32_u32(v5), vreinterpretq_f32_u32(v10)), vreinterpretq_f32_u32(v12))), sign);
    }
  }
  const uint32x4_t m48 = vzip2q_u32(v8, v60), m49 = vzip2q_u32(v9, v7), m46 = vzip1q_u32(v8, v60),
                   m47 = vzip1q_u32(v9, v7);
  const uint32x4_t a45 = vzip2q_u32(m49, m48), a44 = vzip1q_u32(m49, m48), a43 = vzip2q_u32(m47, m46),
                   a42 = vzip1q_u32(m47, m46);
  const uint32x4_t n41 = QuatU(vsubq_f32(QuatF(v63), QuatF(a45)));
  const uint32x4_t n39 = QuatU(vsubq_f32(QuatF(v63), QuatF(a43)));
  const uint32x4_t s40 = vextq_u32(a44, a44, 2), p38 = vrev64q_u32(a44), r37 = QuatRev(a44);
  const uint32x4_t s36 = vextq_u32(a42, a42, 2), p35 = vrev64q_u32(a42), r34 = QuatRev(a42);
  const uint32x4_t d55 = QuatDot(QuatRlimi11(n41, a45), r37);
  const uint32x4_t d61 = QuatDot(QuatRlimi1(n41, a45), a44);
  const uint32x4_t d59 = QuatDot(QuatRlimi13(n41, a45), s40);
  const uint32x4_t d57 = QuatDot(QuatRlimi7(n41, a45), p38);
  const uint32x4_t d51 = QuatDot(QuatRlimi11(n39, a43), r34);
  const uint32x4_t d54 = QuatDot(QuatRlimi1(n39, a43), a42);
  const uint32x4_t d53 = QuatDot(QuatRlimi13(n39, a43), s36);
  const uint32x4_t d52 = QuatDot(QuatRlimi7(n39, a43), p35);
  const uint32x4_t b50 = vzip1q_u32(d59, d61), b49 = vzip1q_u32(d55, d57);
  const uint32x4_t b48 = vzip1q_u32(b49, b50);
  const uint32x4_t b47 = vzip1q_u32(d53, d54), b45 = vzip1q_u32(d51, d52);
  const uint32x4_t n46 = QuatU(vsubq_f32(QuatF(v63), QuatF(b48)));
  const uint32x4_t b44 = vzip1q_u32(b45, b47);
  const uint32x4_t s43 = vextq_u32(b44, b44, 2), p42 = vrev64q_u32(b44), r41 = QuatRev(b44);
  const uint32x4_t d34 = QuatDot(QuatRlimi11(n46, b48), r41);
  const uint32x4_t d37 = QuatDot(QuatRlimi1(n46, b48), b44);
  const uint32x4_t d36 = QuatDot(QuatRlimi13(n46, b48), s43);
  const uint32x4_t d35 = QuatDot(QuatRlimi7(n46, b48), p42);
  const uint32x4_t r = vzip1q_u32(vzip1q_u32(d34, d35), vzip1q_u32(d36, d37));
  vst1q_u8(Host(base, out & ~15u), vrev32q_u8(vreinterpretq_u8_u32(r)));
}

#endif

}  // namespace

REX_HOOK_RAW(sub_83420D18) {
#if defined(__aarch64__)
  if (Enabled(REXCVAR_GET(gtav_nr_quat)) && QuatRunnable(base, ctx)) {
    if (CheckNow()) [[unlikely]] {
      Differential(g_quat, ctx, base, __imp__sub_83420D18, {{ctx.r3.u32 & ~15u, 16}}, SameResult, Quat83420D18);
      return;
    }
    Quat83420D18(ctx, base);
    return;
  }
#endif
  __imp__sub_83420D18(ctx, base);
}

namespace {

#if defined(__aarch64__)

inline float32x4_t CullArg(const PPCVRegister& r) {
  const uint32x4_t v = vld1q_u32(r.u32);
  const uint32x4_t s = vrev64q_u32(v);
  return vreinterpretq_f32_u32(vextq_u32(s, s, 2));
}

inline float32x4_t CullMin(float32x4_t a, float32x4_t b) {
  const uint32x4_t n = vmvnq_u32(vandq_u32(vceqq_f32(a, a), vceqq_f32(b, b)));
  return vbslq_f32(n, vaddq_f32(a, b), vbslq_f32(vcltq_f32(a, b), a, b));
}

inline float32x4_t CullMax(float32x4_t a, float32x4_t b) {
  const uint32x4_t n = vmvnq_u32(vandq_u32(vceqq_f32(a, a), vceqq_f32(b, b)));
  return vbslq_f32(n, vaddq_f32(a, b), vbslq_f32(vcgtq_f32(a, b), a, b));
}

inline float32x4_t CullMadd(float32x4_t a, float32x4_t b, float32x4_t c) { return vaddq_f32(vmulq_f32(a, b), c); }

inline float32x4_t CullNmsub(float32x4_t a, float32x4_t b, float32x4_t c) {
  return vreinterpretq_f32_u32(
      veorq_u32(vreinterpretq_u32_f32(vsubq_f32(vmulq_f32(a, b), c)), vdupq_n_u32(0x80000000u)));
}

inline float32x4_t CullRcp(float32x4_t a) { return vdivq_f32(vdupq_n_f32(1.0f), a); }

inline float32x4_t CullPerm99(float32x4_t b) {
  const float32x4_t t = vextq_f32(b, b, 1);
  return vcopyq_laneq_f32(vcopyq_laneq_f32(t, 2, b, 0), 3, b, 3);
}

inline float32x4_t CullSel(float32x4_t a, float32x4_t b, uint32x4_t m) { return vbslq_f32(m, b, a); }

inline uint32x4_t CullAnd(uint32x4_t a, uint32x4_t b) { return vandq_u32(a, b); }


inline uint32_t CullAll(uint32x4_t m) { return vminvq_u32(m) >> 31; }

inline void CullStore16(uint8_t* base, uint32_t a, uint16_t v) {
  v = __builtin_bswap16(v);
  std::memcpy(Host(base, a), &v, 2);
}

inline void CullStoreU(uint8_t* base, uint32_t a, uint32x4_t v) {
  vst1q_u8(Host(base, a & ~15u), vrev32q_u8(vreinterpretq_u8_u32(v)));
}

inline uint32x4_t CullLoadU(uint8_t* base, uint32_t a) { return vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(Host(base, a & ~15u)))); }

inline void CullStoreRow(uint8_t* p, float32x4_t v) { vst1q_u8(p, vrev32q_u8(vreinterpretq_u8_f32(v))); }

void Cull831665D8(PPCContext& ctx, uint8_t* base) {
  const uint32_t s = ctx.r3.u32;
  const float32x4_t a = CullArg(ctx.v1), b = CullArg(ctx.v2), c = CullArg(ctx.v3);
  const float32x4_t zero = vreinterpretq_f32_u32(vdupq_n_u32(0));
  const float32x4_t one = vdupq_n_f32(1.0f);
  const float32x4_t v59a = CullMin(a, b);
  const float32x4_t v58a = vzip1q_f32(a, c);
  const float32x4_t v57a = CullMax(a, b);
  const float32x4_t v56a = vzip2q_f32(a, c);
  const float32x4_t v55a = vzip1q_f32(b, zero);
  const float32x4_t v54a = vzip2q_f32(b, zero);
  const float32x4_t v52 = LoadV(base, s + 224);
  const float32x4_t v51a = vzip1q_f32(v58a, v55a);
  const float32x4_t v9a = vzip1q_f32(v56a, v54a);
  const float32x4_t v50a = vzip2q_f32(v58a, v55a);
  const float32x4_t v49a = CullPerm99(v51a);
  const float32x4_t v48a = CullMin(v59a, c);
  const float32x4_t v13a = CullRcp(v9a);
  const float32x4_t v12a = vdupq_laneq_f32(v51a, 0);
  const float32x4_t v47a = CullPerm99(v50a);
  const float32x4_t v46a = CullMax(v57a, c);
  const float32x4_t v63a = vsubq_f32(v51a, v49a);
  const float32x4_t v62a = vsubq_f32(v50a, v47a);
  const float32x4_t v45a = vrndmq_f32(v48a);
  const float32x4_t v7a = CullNmsub(v9a, v13a, one);
  const float32x4_t v5a = vdupq_laneq_f32(v63a, 2);
  const float32x4_t v8a = vdupq_laneq_f32(v63a, 0);
  const float32x4_t v4a = vdupq_laneq_f32(v62a, 0);
  const float32x4_t v3a = vdupq_laneq_f32(v62a, 2);
  const float32x4_t v44a = vsubq_f32(zero, v5a);
  const float32x4_t v43a = vsubq_f32(zero, v8a);
  const float32x4_t v42a = vsubq_f32(v46a, v45a);
  const float32x4_t v11a = CullMadd(v13a, v7a, v13a);
  const float32x4_t v13b = vmulq_f32(v44a, v4a);
  const float32x4_t v61a = vdupq_laneq_f32(v42a, 1);
  const float32x4_t v59b = vdupq_laneq_f32(v42a, 0);
  const float32x4_t v57b = CullMax(v59b, v61a);
  const float32x4_t v10a = CullMadd(v8a, v3a, v13b);
  if (CullAll(vcgeq_f32(zero, v10a))) {
    ctx.r3.u64 = 0;
    return;
  }
  const float32x4_t v8b = CullNmsub(v9a, v11a, one);
  const uint32_t lt_a = CullAll(vcgeq_f32(v52, v57b));
  const float32x4_t v41 = v10a;
  const float32x4_t v8c = CullMadd(v11a, v8b, v11a);
  const float32x4_t v13c = CullRcp(v41);
  const float32x4_t v40 = v8c;
  const float32x4_t v2a = CullNmsub(v10a, v13c, one);
  const float32x4_t v39a = CullPerm99(v40);
  const float32x4_t v10b = CullNmsub(v41, v13c, one);
  const float32x4_t v1a = vdupq_laneq_f32(v40, 0);
  const float32x4_t v38a = vsubq_f32(v40, v39a);
  const float32x4_t v11b = CullMadd(v13c, v2a, v13c);
  const float32x4_t v13d = CullMadd(v13c, v10b, v13c);
  const float32x4_t v8d = vdupq_laneq_f32(v38a, 0);
  const float32x4_t v2b = vdupq_laneq_f32(v38a, 2);
  const float32x4_t v37a = vsubq_f32(zero, v8d);
  const float32x4_t v30 = vmulq_f32(v43a, v2b);
  const float32x4_t v31 = CullNmsub(v41, v11b, one);
  const float32x4_t v0b = CullNmsub(v41, v13d, one);
  const float32x4_t v10c = vmulq_f32(v37a, v3a);
  const float32x4_t v8e = CullMadd(v8d, v5a, v30);
  const float32x4_t v9b = CullMadd(v11b, v31, v11b);
  const float32x4_t v0c = CullMadd(v13d, v0b, v13d);
  const float32x4_t v11c = CullMadd(v4a, v2b, v10c);
  const float32x4_t v36a = vmulq_f32(v8e, v0c);
  const float32x4_t v0d = vdupq_laneq_f32(v50a, 0);
  const float32x4_t v35a = vmulq_f32(v11c, v9b);
  const float32x4_t v34a = vsubq_f32(zero, v36a);
  const float32x4_t v33a = vsubq_f32(zero, v35a);
  const float32x4_t v10d = CullNmsub(v33a, v12a, v1a);
  const float32x4_t v12b = CullNmsub(v34a, v0d, v10d);
  const float32x4_t v32a = vzip1q_f32(v33a, v12b);
  const float32x4_t v58b = vzip1q_f32(v32a, v34a);
  const float32x4_t v0e = vmulq_f32(v62a, v51a);
  const float32x4_t v55b = vdupq_laneq_f32(v58b, 0);
  const float32x4_t v54b = vdupq_laneq_f32(v58b, 1);
  const float32x4_t v53b = vdupq_laneq_f32(v58b, 2);
  const float32x4_t v61b = vzip1q_f32(v45a, v46a);
  const float32x4_t v51b = vsubq_f32(zero, v55b);
  const float32x4_t v0f = CullNmsub(v63a, v50a, v0e);
  const float32x4_t v63b = vcopyq_laneq_f32(v63a, 3, v54b, 0);
  const float32x4_t v62b = vcopyq_laneq_f32(v62a, 3, v51b, 0);
  const float32x4_t v0g = vcopyq_laneq_f32(v0f, 3, v53b, 0);
  const uint32x4_t v7m = vcgeq_f32(v52, v57b);
  const uint32x4_t v8m = vcgtq_f32(zero, v62b);
  const float32x4_t v45b = vsubq_f32(zero, v62b);
  const float32x4_t v47b = vdupq_laneq_f32(v61b, 3);
  const float32x4_t v13e = LoadV(base, s + 11056);
  const float32x4_t v46b = vdupq_laneq_f32(v61b, 2);
  const float32x4_t v6a = vdupq_laneq_f32(v0g, 1);
  const float32x4_t v12c = LoadV(base, 0x820A8FB0u);
  const float32x4_t v9c = LoadV(base, 0x820A8FA0u);
  const float32x4_t v11d = LoadV(base, s + 11008);
  const float32x4_t v49b = vdupq_laneq_f32(v61b, 1);
  const float32x4_t v43b = LoadV(base, s + 11088);
  const float32x4_t v48b = vdupq_laneq_f32(v61b, 0);
  const float32x4_t v3b = CullSel(v9c, v12c, v7m);
  const float32x4_t v7b = vdupq_laneq_f32(v0g, 0);
  const float32x4_t v9d = CullSel(zero, v11d, v8m);
  const uint32x4_t v8n = vcgtq_f32(zero, v63b);
  const float32x4_t v4b = vdupq_laneq_f32(v45b, 0);
  const float32x4_t v35b = LoadV(base, s + 11024);
  const float32x4_t v40b = LoadV(base, s + 11072);
  const float32x4_t v5b = vdupq_laneq_f32(v45b, 1);
  const float32x4_t v0h = vdupq_laneq_f32(v0g, 2);
  const uint32x4_t v38m = vcgtq_f32(v47b, v43b);
  const float32x4_t v10e = vmulq_f32(v62b, v9d);
  const float32x4_t v9e = vsubq_f32(zero, v63b);
  const float32x4_t v4c = CullMadd(v4b, v13e, v7b);
  const float32x4_t v7c = vdupq_laneq_f32(v45b, 2);
  const uint32x4_t v37m = vcgtq_f32(v40b, v46b);
  const float32x4_t v44b = LoadV(base, s + 11040);
  const uint32x4_t v42m = vcgtq_f32(v49b, v13e);
  const float32x4_t v6b = CullMadd(v5b, v13e, v6a);
  const float32x4_t v13f = CullMadd(v7c, v13e, v0h);
  const uint32x4_t v39m = vcgtq_f32(v44b, v48b);
  const float32x4_t v0i = CullSel(v35b, zero, v8n);
  const float32x4_t v34b = vmulq_f32(v63b, v35b);
  const uint32x4_t v36m = vcgtq_f32(v41, zero);
  const float32x4_t v0j = CullMadd(v9e, v0i, v10e);
  const uint32x4_t v32m = CullAnd(v37m, v38m);
  const uint32x4_t v33m = CullAnd(v39m, v42m);
  const uint32x4_t v63m = vdupq_laneq_u32(v32m, 0);
  const uint32x4_t v62m = vdupq_laneq_u32(v32m, 1);
  const uint32x4_t v59m = vdupq_laneq_u32(v32m, 2);
  const uint32x4_t v61m = CullAnd(vreinterpretq_u32_f32(v3b), v36m);
  const uint32x4_t v58m = CullAnd(v33m, v63m);
  const uint32x4_t v57m = CullAnd(v33m, v62m);
  const uint32x4_t v56m = CullAnd(v33m, v59m);
  const float32x4_t v55c = vsubq_f32(v0j, v34b);
  const float32x4_t v48c = vsubq_f32(v55c, v34b);
  const float32x4_t v54c = vdupq_laneq_f32(v0j, 1);
  const float32x4_t v53c = vdupq_laneq_f32(v0j, 0);
  const float32x4_t v47c = vdupq_laneq_f32(v55c, 1);
  const float32x4_t v46c = vdupq_laneq_f32(v55c, 0);
  const float32x4_t v52c = vdupq_laneq_f32(v0j, 2);
  const uint32x4_t v51m = vcgtq_f32(v6b, v54c);
  const uint32x4_t v50m = vcgtq_f32(v4c, v53c);
  const float32x4_t v45c = vdupq_laneq_f32(v55c, 2);
  const uint32x4_t v44m = vcgtq_f32(v6b, v47c);
  const uint32x4_t v43m = vcgtq_f32(v4c, v46c);
  const uint32x4_t v49m = vcgtq_f32(v13f, v52c);
  const uint32x4_t v41m = vcgtq_f32(v13f, v45c);
  const float32x4_t v39c = vdupq_laneq_f32(v48c, 1);
  const float32x4_t v38c = vdupq_laneq_f32(v48c, 0);
  const float32x4_t v36c = vdupq_laneq_f32(v48c, 2);
  const uint32x4_t v35m = vcgtq_f32(v6b, v39c);
  const uint32x4_t v34m = vcgtq_f32(v4c, v38c);
  const uint32x4_t v42n = CullAnd(v50m, v51m);
  const uint32x4_t v32n = vcgtq_f32(v13f, v36c);
  const uint32x4_t v33n = CullAnd(v43m, v44m);
  const uint32x4_t v40m = CullAnd(v42n, v49m);
  const uint32x4_t v63n = CullAnd(v33n, v41m);
  const uint32x4_t v37n = CullAnd(v40m, v58m);
  const uint32x4_t v62n = CullAnd(v63n, v57m);
  const uint32x4_t v59n = CullAnd(v34m, v35m);
  const uint32x4_t v0m = CullAnd(v37n, v61m);
  const uint32x4_t v13m = CullAnd(v62n, v61m);
  const uint32x4_t v58n = CullAnd(v59n, v32n);
  const uint32x4_t v57n = vorrq_u32(v0m, v13m);
  const uint32x4_t v56n = CullAnd(v58n, v56m);
  const uint32x4_t v12m = CullAnd(v56n, v61m);
  const uint32x4_t v55m = vorrq_u32(v57n, v12m);
  const uint32_t lt_c = CullAll(vceqq_u32(v55m, vdupq_n_u32(0)));
  ctx.r3.u64 = lt_c ? 0 : 1;
  if (lt_c) return;
  const uint32_t sum = ctx.r5.u32 + ctx.r6.u32;
  if (int32_t(sum) >= 448) return;
  uint8_t* row = Host(base, (ctx.r5.u32 << 6) + ctx.r4.u32);
  CullStoreRow(row, v0g);
  CullStoreRow(row + 16, v63b);
  CullStoreRow(row + 32, v62b);
  CullStoreRow(row + 48, v61b);
  const uint16_t value = uint16_t(sum);
  const uint32_t counters = s + 48 * lt_a;
  for (uint32_t i = 0; i < 12; ++i)
    CullStore16(base, s + ((Load32(base, counters + 4 * i) + 122 + 448 * i) << 1), value);
  const uint32x4_t c0 = CullLoadU(base, counters), c1 = CullLoadU(base, counters + 16),
                   c2 = CullLoadU(base, counters + 32);
  CullStoreU(base, counters, vreinterpretq_u32_s32(vqaddq_s32(vreinterpretq_s32_u32(c0), vreinterpretq_s32_u32(v0m))));
  CullStoreU(base, counters + 32,
             vreinterpretq_u32_s32(vqaddq_s32(vreinterpretq_s32_u32(c2), vreinterpretq_s32_u32(v12m))));
  CullStoreU(base, counters + 16,
             vreinterpretq_u32_s32(vqaddq_s32(vreinterpretq_s32_u32(c1), vreinterpretq_s32_u32(v13m))));
}

#endif

#if defined(__aarch64__)

bool CullCheckable(const PPCContext& ctx, uint8_t* base) {
  const uint32_t lo = ctx.r3.u32 & ~15u;
  if (!Flat(lo, 11120)) return false;
  for (uint32_t blk = 0; blk < 2; ++blk)
    for (uint32_t i = 0; i < 12; ++i) {
      const uint32_t at = ctx.r3.u32 + ((Load32(base, ctx.r3.u32 + 48 * blk + 4 * i) + 122 + 448 * i) << 1);
      if (at - lo > 11118) return false;
    }
  return true;
}

bool CullSameRegs(const PPCContext& a, const PPCContext& b) { return a.r3.u64 == b.r3.u64 && a.r1.u64 == b.r1.u64; }

#endif

}  // namespace

REX_HOOK_RAW(sub_831665D8) {
#if defined(__aarch64__)
  if (Enabled(REXCVAR_GET(gtav_nr_cull)) && !(ctx.r1.u32 & 15) && Flat((ctx.r5.u32 << 6) + ctx.r4.u32, 64)) {
    if (CheckNow()) [[unlikely]] {
      if (CullCheckable(ctx, base)) {
        Differential(g_cull, ctx, base, __imp__sub_831665D8,
                     {{ctx.r3.u32 & ~15u, 11120}, {(ctx.r5.u32 << 6) + ctx.r4.u32, 64}}, CullSameRegs, Cull831665D8);
        return;
      }
      __imp__sub_831665D8(ctx, base);
      return;
    }
    Cull831665D8(ctx, base);
    return;
  }
#endif
  __imp__sub_831665D8(ctx, base);
}

namespace {

#if defined(__aarch64__)

constexpr uint32_t kBlendThreshold = 0x820CAE18u;
constexpr uint32_t kBlendMaxGroups = 1u << 20;

struct BlendCounts {
  uint32_t vecs;
  uint32_t quats;
  uint32_t floats;
};

inline uint32_t BlendGroups(uint8_t* base, uint32_t a) { return uint32_t(int32_t(Load32(base, a) + 3) >> 2); }

inline BlendCounts BlendCount(const PPCContext& ctx, uint8_t* base) {
  const uint32_t s = ctx.r3.u32;
  return {BlendGroups(base, s + 16), BlendGroups(base, s + 20), BlendGroups(base, s + 24)};
}

inline bool BlendRunnable(const PPCContext& ctx, uint8_t* base, const BlendCounts& n) {
  if (n.vecs > kBlendMaxGroups || n.quats > kBlendMaxGroups || n.floats > kBlendMaxGroups) return false;
  const uint32_t s = ctx.r3.u32;
  const uint32_t a = Load32(base, s), b = Load32(base, s + 4), w = Load32(base, s + 8);
  const uint32_t data = 64u * (n.vecs + n.quats) + 16u * n.floats, weights = 16u * (n.vecs + n.quats + n.floats);
  return Flat(a & ~15u, data) && Flat(b & ~15u, data) && Flat((w ? w : b) & ~15u, weights);
}

inline float32x4_t BlendLoad(const uint8_t* p) { return vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(p))); }

inline void BlendStore(uint8_t* p, float32x4_t v) { vst1q_u8(p, vrev32q_u8(vreinterpretq_u8_f32(v))); }

inline uint32x4_t BlendBits(float32x4_t v) { return vreinterpretq_u32_f32(v); }

inline float32x4_t BlendFloat(uint32x4_t v) { return vreinterpretq_f32_u32(v); }

inline uint32x4_t BlendInvalid(float32x4_t v) { return vceqq_u32(BlendBits(v), vdupq_n_u32(0xFFFFFFFFu)); }

inline float32x4_t BlendSel(float32x4_t a, float32x4_t b, uint32x4_t m) { return vbslq_f32(m, b, a); }

inline float32x4_t BlendOr(uint32x4_t m, float32x4_t v) { return BlendFloat(vorrq_u32(m, BlendBits(v))); }

inline float32x4_t BlendSums(float32x4_t p0, float32x4_t p1, float32x4_t p2, float32x4_t p3) {
  const float64x2_t l01 = vreinterpretq_f64_f32(vtrn1q_f32(p0, p1)), h01 = vreinterpretq_f64_f32(vtrn2q_f32(p0, p1));
  const float64x2_t l23 = vreinterpretq_f64_f32(vtrn1q_f32(p2, p3)), h23 = vreinterpretq_f64_f32(vtrn2q_f32(p2, p3));
  const float32x4_t c0 = vreinterpretq_f32_f64(vtrn1q_f64(l01, l23)), c1 = vreinterpretq_f32_f64(vtrn1q_f64(h01, h23));
  const float32x4_t c2 = vreinterpretq_f32_f64(vtrn2q_f64(l01, l23)), c3 = vreinterpretq_f32_f64(vtrn2q_f64(h01, h23));
  return vaddq_f32(vaddq_f32(c3, c2), vaddq_f32(c1, c0));
}

struct BlendState {
  float32x4_t t;
  float32x4_t zero;
  float32x4_t one;
  float32x4_t low;
  float32x4_t high;
  uint32x4_t fixed;
  uint32x4_t unweighted;
};

inline float32x4_t BlendWeight(const BlendState& s, const uint8_t* w) {
  return BlendSel(vminq_f32(vmaxq_f32(vmulq_f32(s.t, BlendLoad(w)), s.zero), s.one), s.t, s.unweighted);
}

inline float32x4_t BlendPick(float32x4_t r, float32x4_t a, float32x4_t b, uint32x4_t keep_a, uint32x4_t keep_b) {
  return BlendSel(BlendSel(r, BlendOr(keep_a, a), BlendInvalid(b)), BlendOr(keep_b, b), BlendInvalid(a));
}

template <int I>
inline float32x4_t BlendVec(float32x4_t a, float32x4_t b, float32x4_t w, uint32x4_t keep_a, uint32x4_t keep_b) {
  const float32x4_t r = MulAdd(vsubq_f32(b, a), vdupq_laneq_f32(w, I), a);
  return BlendPick(r, a, b, vdupq_laneq_u32(keep_a, I), vdupq_laneq_u32(keep_b, I));
}

template <int I>
inline float32x4_t BlendNear(const BlendState& s, float32x4_t a, float32x4_t b, float32x4_t w, uint32x4_t flip) {
  const float32x4_t near = BlendSel(a, vsubq_f32(s.zero, a), vdupq_laneq_u32(flip, I));
  return MulAdd(vsubq_f32(b, near), vdupq_laneq_f32(w, I), near);
}

template <int I>
inline float32x4_t BlendNorm(float32x4_t q, float32x4_t a, float32x4_t b, float32x4_t f, uint32x4_t keep_a,
                             uint32x4_t keep_b) {
  return BlendPick(vmulq_f32(q, vdupq_laneq_f32(f, I)), a, b, vdupq_laneq_u32(keep_a, I), vdupq_laneq_u32(keep_b, I));
}

void Blend83441C48(PPCContext& ctx, uint8_t* base) {
  const uint32_t st = ctx.r3.u32;
  const BlendCounts n = BlendCount(ctx, base);
  const uint32_t w_at = Load32(base, st + 8), b_at = Load32(base, st + 4);
  uint8_t* a = Host(base, Load32(base, st) & ~15u);
  const uint8_t* b = Host(base, b_at & ~15u);
  const uint8_t* w = Host(base, (w_at ? w_at : b_at) & ~15u);
  const uint32_t t_bits = Load32(base, st + 12);
  float t_in;
  std::memcpy(&t_in, &t_bits, 4);
  const float t = float(double(t_in));
  BlendState s;
  s.unweighted = vdupq_n_u32(w_at == 0 ? 0xFFFFFFFFu : 0u);
  s.t = vdupq_n_f32(t);
  s.zero = BlendFloat(vdupq_n_u32(0));
  s.one = vdupq_n_f32(1.0f);
  s.low = BlendFloat(vdupq_n_u32(LvlxWord0(base, kBlendThreshold)));
  s.high = vsubq_f32(s.one, s.low);
  s.fixed = vdupq_n_u32(Host(base, st + 28)[0] == 0 ? 0xFFFFFFFFu : 0u);
  for (uint32_t g = 0; g < n.vecs; ++g, a += 64, b += 64, w += 16) {
    const float32x4_t wt = BlendWeight(s, w);
    const uint32x4_t keep_a = vandq_u32(s.fixed, vcgtq_f32(wt, s.high));
    const uint32x4_t keep_b = vcgtq_f32(s.low, wt);
    const float32x4_t a0 = BlendLoad(a), a1 = BlendLoad(a + 16), a2 = BlendLoad(a + 32), a3 = BlendLoad(a + 48);
    const float32x4_t b0 = BlendLoad(b), b1 = BlendLoad(b + 16), b2 = BlendLoad(b + 32), b3 = BlendLoad(b + 48);
    BlendStore(a, BlendVec<0>(a0, b0, wt, keep_a, keep_b));
    BlendStore(a + 16, BlendVec<1>(a1, b1, wt, keep_a, keep_b));
    BlendStore(a + 32, BlendVec<2>(a2, b2, wt, keep_a, keep_b));
    BlendStore(a + 48, BlendVec<3>(a3, b3, wt, keep_a, keep_b));
  }
  const float32x4_t half = vdupq_n_f32(0.5f);
  for (uint32_t g = 0; g < n.quats; ++g, a += 64, b += 64, w += 16) {
    const float32x4_t wt = BlendWeight(s, w);
    const uint32x4_t keep_a = vandq_u32(s.fixed, vcgtq_f32(wt, s.high));
    const uint32x4_t keep_b = vcgtq_f32(s.low, wt);
    const float32x4_t a0 = BlendLoad(a), a1 = BlendLoad(a + 16), a2 = BlendLoad(a + 32), a3 = BlendLoad(a + 48);
    const float32x4_t b0 = BlendLoad(b), b1 = BlendLoad(b + 16), b2 = BlendLoad(b + 32), b3 = BlendLoad(b + 48);
    const uint32x4_t flip =
        vcgtq_f32(s.zero, BlendSums(vmulq_f32(b0, a0), vmulq_f32(b1, a1), vmulq_f32(b2, a2), vmulq_f32(b3, a3)));
    const float32x4_t q0 = BlendNear<0>(s, a0, b0, wt, flip), q1 = BlendNear<1>(s, a1, b1, wt, flip);
    const float32x4_t q2 = BlendNear<2>(s, a2, b2, wt, flip), q3 = BlendNear<3>(s, a3, b3, wt, flip);
    const float32x4_t len = BlendSums(vmulq_f32(q0, q0), vmulq_f32(q1, q1), vmulq_f32(q2, q2), vmulq_f32(q3, q3));
    const float32x4_t r = vdivq_f32(s.one, vsqrtq_f32(len));
    const float32x4_t e = BlendFloat(
        veorq_u32(BlendBits(vsubq_f32(vmulq_f32(half, vmulq_f32(len, vmulq_f32(r, r))), half)), vdupq_n_u32(0x80000000u)));
    const float32x4_t f = MulAdd(r, e, r);
    BlendStore(a, BlendNorm<0>(q0, a0, b0, f, keep_a, keep_b));
    BlendStore(a + 16, BlendNorm<1>(q1, a1, b1, f, keep_a, keep_b));
    BlendStore(a + 32, BlendNorm<2>(q2, a2, b2, f, keep_a, keep_b));
    BlendStore(a + 48, BlendNorm<3>(q3, a3, b3, f, keep_a, keep_b));
  }
  for (uint32_t g = 0; g < n.floats; ++g, a += 16, b += 16, w += 16) {
    const float32x4_t wt = BlendWeight(s, w);
    const float32x4_t va = BlendLoad(a), vb = BlendLoad(b);
    const float32x4_t r = MulAdd(vsubq_f32(vb, va), wt, va);
    BlendStore(a, BlendPick(r, va, vb, vandq_u32(s.fixed, vcgtq_f32(wt, s.high)), vcgtq_f32(s.low, wt)));
  }
  ctx.r3.u64 = 1;
}

bool BlendSameRegs(const PPCContext& a, const PPCContext& b) { return a.r3.u64 == b.r3.u64 && a.r1.u64 == b.r1.u64; }

#endif

}  // namespace

REX_HOOK_RAW(sub_83441C48) {
#if defined(__aarch64__)
  if (Enabled(REXCVAR_GET(gtav_nr_blend))) {
    const BlendCounts n = BlendCount(ctx, base);
    if (BlendRunnable(ctx, base, n)) {
      if (!CheckNow()) [[likely]] {
        Blend83441C48(ctx, base);
        return;
      }
      const uint32_t at = Load32(base, ctx.r3.u32) & ~15u;
      const uint64_t size = 64ull * (uint64_t(n.vecs) + n.quats) + 16ull * n.floats;
      if (size <= (1u << 20) && Flat(at, uint32_t(size))) {
        Differential(g_blend, ctx, base, __imp__sub_83441C48, {{at, uint32_t(size)}}, BlendSameRegs, Blend83441C48);
        return;
      }
    }
  }
#endif
  __imp__sub_83441C48(ctx, base);
}

namespace {

#if defined(__aarch64__)

inline void BoundStore32(uint8_t* base, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(Host(base, a), &v, 4);
}

bool BoundOverlap(uint8_t* base, uint32_t self, uint32_t box) {
  const float32x4_t lo = LoadV(base, box), hi = LoadV(base, box + 16), sphere = LoadV(base, self + 128);
  const float32x4_t half = vdupq_n_f32(0.5f);
  const float32x4_t center = vmulq_f32(vaddq_f32(hi, lo), half);
  const float32x4_t extent = vmulq_f32(vsubq_f32(hi, lo), half);
  const float32x4_t gap = vmaxq_f32(vdupq_n_f32(0.0f), vsubq_f32(vabsq_f32(vsubq_f32(sphere, center)), extent));
  const float dist = vaddvq_f32(vsetq_lane_f32(0.0f, vmulq_f32(gap, gap), 3));
  const float radius = vgetq_lane_f32(sphere, 3);
  return radius * radius >= dist;
}

void Bound83585DE8(PPCContext& ctx, uint8_t* base) {
  const uint32_t self = ctx.r3.u32, box = ctx.r4.u32;
  uint32_t flags = Load32(base, self + 268);
  bool hit = false;
  const uint64_t lr = ctx.lr;
  if (flags & 1) {
    const PPCRegister sp = ctx.r1;
    BoundStore32(base, sp.u32 - 8, uint32_t(lr));
    BoundStore32(base, sp.u32 - 24, 0);
    BoundStore32(base, sp.u32 - 20, 0);
    BoundStore32(base, sp.u32 - 16, 0);
    BoundStore32(base, sp.u32 - 12, 0);
    BoundStore32(base, sp.u32 - 112, sp.u32);
    ctx.r1.u32 = sp.u32 - 112;
    ctx.lr = 0x83585E1C;
    sub_826B9248(ctx, base);
    ctx.r1 = sp;
    hit = (ctx.r3.u32 & 0xFF) != 0;
    flags = Load32(base, self + 268);
  }
  if ((flags & 2) && !hit) hit = BoundOverlap(base, self, box);
  ctx.lr = uint32_t(lr);
  ctx.r3.u64 = hit ? 0 : 1;
}

#endif

}  // namespace

REX_HOOK_RAW(sub_83585DE8) {
#if defined(__aarch64__)
  if (Enabled(REXCVAR_GET(gtav_nr_bound))) {
    if (CheckNow() && !(Load32(base, ctx.r3.u32 + 268) & 1)) [[unlikely]] {
      Differential(g_bound, ctx, base, __imp__sub_83585DE8, {}, SameResult, Bound83585DE8);
      return;
    }
    Bound83585DE8(ctx, base);
    return;
  }
#endif
  __imp__sub_83585DE8(ctx, base);
}

namespace {

#if defined(__aarch64__)

constexpr int kVmStackBias = 16;
constexpr int kVmStackMask = 511;

struct VmRegs {
  PPCRegister r7, r8, r9, r10;
  PPCVRegister v0, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13;
};

inline void VmPull(const PPCContext& ctx, VmRegs& c) {
  c.r7 = ctx.r7;
  c.r8 = ctx.r8;
  c.r9 = ctx.r9;
  c.r10 = ctx.r10;
  c.v0 = ctx.v0;
  c.v4 = ctx.v4;
  c.v5 = ctx.v5;
  c.v6 = ctx.v6;
  c.v7 = ctx.v7;
  c.v8 = ctx.v8;
  c.v9 = ctx.v9;
  c.v10 = ctx.v10;
  c.v11 = ctx.v11;
  c.v12 = ctx.v12;
  c.v13 = ctx.v13;
}

inline void VmPush(PPCContext& ctx, const VmRegs& c) {
  ctx.r7 = c.r7;
  ctx.r8 = c.r8;
  ctx.r9 = c.r9;
  ctx.r10 = c.r10;
  ctx.v0 = c.v0;
  ctx.v4 = c.v4;
  ctx.v5 = c.v5;
  ctx.v6 = c.v6;
  ctx.v7 = c.v7;
  ctx.v8 = c.v8;
  ctx.v9 = c.v9;
  ctx.v10 = c.v10;
  ctx.v11 = c.v11;
  ctx.v12 = c.v12;
  ctx.v13 = c.v13;
}

inline uint8_t VmL8(uint8_t* base, uint32_t a) { return *Host(base, a); }

inline void VmS32(uint8_t* base, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(Host(base, a), &v, 4);
}

inline uint8_t* VmRaw(uint8_t* base, uint32_t a) { return Host(base, a); }

#define VM_NEXT                                 \
  do {                                          \
    r11.u64 = VmL8(base, r25.u32);              \
    r25.s64 = r25.s64 + 1;                      \
    if (r11.u32 > 73) __builtin_trap();         \
    goto* kVmTable[r11.u32];                    \
  } while (0)
#undef VM_NEXT

bool VmSameRegs(const PPCContext& a, const PPCContext& b) { return a.r1.u64 == b.r1.u64 && a.r3.u64 == b.r3.u64; }

constexpr uint32_t kVmObjBytes = 64;
constexpr uint32_t kVmTrackBytes = 65535u + 1024u + 32u;
constexpr uint32_t kVmMaxSlots = 4096;
constexpr uint32_t kVmTrackOffset = 128;
constexpr uint32_t kVmSlotOffset = kVmTrackOffset + ((kVmTrackBytes + 15) & ~15u) + 16;
constexpr uint32_t kVmShadowBytes = kVmSlotOffset + 16 * kVmMaxSlots + 32;
constexpr uint32_t kVmStackScratch = 32768;

struct VmShadow {
  uint32_t at = 0;
  uint32_t obj = 0, tracks_from = 0, tracks_n = 0, tracks_to = 0, slots_from = 0, slots_n = 0, slots_to = 0;

  uint32_t Map(uint32_t v) const {
    if (tracks_n && v - tracks_from < tracks_n) return v - tracks_from + tracks_to;
    if (slots_n && v - slots_from < slots_n) return v - slots_from + slots_to;
    return v;
  }

  void Fill(uint8_t* base, uint32_t real_obj, uint32_t slots_need, const VmShadow* from) {
    obj = at + (real_obj & 15);
    std::memcpy(Host(base, obj), Host(base, real_obj), kVmObjBytes);
    std::memcpy(Host(base, tracks_to), Host(base, from ? from->tracks_to : tracks_from), tracks_n);
    std::memset(Host(base, tracks_to + tracks_n), 0, kVmTrackBytes - tracks_n);
    std::memcpy(Host(base, slots_to), Host(base, from ? from->slots_to : slots_from), slots_n);
    std::memset(Host(base, slots_to + slots_n), 0, slots_need - slots_n);
    for (uint32_t o : {0u, 4u, 8u, 16u, 24u}) VmS32(base, obj + o, Map(Load32(base, obj + o)));
  }
};

uint32_t VmShadowBlock(int i) {
  thread_local uint32_t blocks[2] = {0, 0};
  if (!blocks[i]) blocks[i] = rex::system::kernel_memory()->SystemHeapAlloc(kVmShadowBytes, 4096);
  return blocks[i];
}

bool VmShadows(const PPCContext& ctx, uint8_t* base, VmShadow& a, VmShadow& b) {
  const uint32_t obj = ctx.r4.u32;
  if (!Flat(obj, kVmObjBytes) || Writable(obj, kVmObjBytes) != kVmObjBytes) return false;
  const uint32_t tracks = Load32(base, obj), slots = Load32(base, obj + 24), count = Load32(base, obj + 48);
  if (count > kVmMaxSlots) return false;
  const uint32_t t0 = tracks & ~15u, s0 = slots & ~15u;
  const uint32_t tn = Flat(t0, kVmTrackBytes) ? Writable(t0, kVmTrackBytes) : 0;
  const uint32_t slots_need = 16 * count + 16;
  const uint32_t sn = count && Flat(s0, slots_need) ? Writable(s0, slots_need) : 0;
  auto apart = [](uint32_t a, uint32_t an, uint32_t b, uint32_t bn) {
    return !an || !bn || uint64_t(a) + an <= b || uint64_t(b) + bn <= a;
  };
  if (!tn || sn < 16 * count || !apart(t0, tn, s0, sn) || !apart(obj, kVmObjBytes, t0, tn) ||
      !apart(obj, kVmObjBytes, s0, sn))
    return false;
  for (VmShadow* s : {&a, &b}) {
    s->at = VmShadowBlock(s == &a ? 0 : 1);
    if (!s->at) return false;
    s->tracks_from = t0;
    s->tracks_n = tn;
    s->tracks_to = s->at + kVmTrackOffset;
    s->slots_from = s0;
    s->slots_n = sn;
    s->slots_to = s->at + kVmSlotOffset;
  }
  a.Fill(base, obj, slots_need, nullptr);
  b.Fill(base, obj, slots_need, &a);
  return true;
}

bool VmSameBytes(const uint8_t* want, const uint8_t* have, uint32_t n, uint32_t addr, std::string& what,
                 bool& nan_only) {
  for (uint32_t w = 0; w < n; w += 4) {
    if (std::memcmp(want + w, have + w, std::min(4u, n - w)) == 0) continue;
    const uint32_t x = n - w >= 4 ? Be32(want, w) : 0, y = n - w >= 4 ? Be32(have, w) : 1;
    if (IsNan(x) && IsNan(y)) {
      nan_only = true;
      continue;
    }
    what = fmt::format("byte {:08X}: original {:08X} native {:08X}", addr + w, x, y);
    return false;
  }
  return true;
}

void VmCheck(PPCContext& ctx, uint8_t* base) {
  VmShadow sa, sb;
  if (!VmShadows(ctx, base, sa, sb)) {
    g_vm_unchecked.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  PPCContext mine = ctx, theirs = ctx;
  mine.r4.u64 = sa.obj;
  theirs.r4.u64 = sb.obj;
  const uint32_t below = ctx.r1.u32 - kVmStackScratch;
  const uint32_t scratch = Flat(below, kVmStackScratch) ? Writable(below, kVmStackScratch) : 0;
  thread_local std::vector<uint8_t> stack;
  stack.resize(scratch);
  std::memcpy(stack.data(), Host(base, below), scratch);
  thread_local uint32_t flip = 0;
  Clock::time_point t[4];
  if (++flip & 1) {
    t[0] = Clock::now();
    Vm834260A0(mine, base);
    t[1] = Clock::now();
    std::memcpy(Host(base, below), stack.data(), scratch);
    t[2] = Clock::now();
    __imp__sub_834260A0(theirs, base);
    t[3] = Clock::now();
  } else {
    t[2] = Clock::now();
    __imp__sub_834260A0(theirs, base);
    t[3] = Clock::now();
    std::memcpy(Host(base, below), stack.data(), scratch);
    t[0] = Clock::now();
    Vm834260A0(mine, base);
    t[1] = Clock::now();
  }
  std::string what;
  bool nan_only = false;
  bool ok = VmSameBytes(Host(base, sb.tracks_to), Host(base, sa.tracks_to), sa.tracks_n, sa.tracks_from, what,
                        nan_only) &&
            VmSameBytes(Host(base, sb.slots_to), Host(base, sa.slots_to), sa.slots_n, sa.slots_from, what,
                        nan_only);
  auto canon = [](const VmShadow& s, uint64_t v) -> uint64_t {
    const uint32_t lo = uint32_t(v);
    if (s.tracks_n && lo - s.tracks_to < s.tracks_n) return (v & ~0xFFFFFFFFull) | (lo - s.tracks_to + s.tracks_from);
    if (s.slots_n && lo - s.slots_to < s.slots_n) return (v & ~0xFFFFFFFFull) | (lo - s.slots_to + s.slots_from);
    if (lo - s.at < kVmShadowBytes) return (v & ~0xFFFFFFFFull) | (lo - s.at + 0x10000000u);
    return v;
  };
  if (ok) {
    PPCContext x = mine, y = theirs;
    x.r3.u64 = canon(sa, x.r3.u64);
    y.r3.u64 = canon(sb, y.r3.u64);
    ok = VmSameRegs(x, y);
    if (!ok) what = fmt::format("registers: r1 {:X}/{:X} r3 {:X}/{:X}", y.r1.u64, x.r1.u64, y.r3.u64, x.r3.u64);
  }
  if (nan_only) g_vm.nan_only.fetch_add(1, std::memory_order_relaxed);
  if (!ok)
    what += fmt::format(" | in r3 {:08X} r4 {:08X} tracks {:08X} slots {:08X} count {}", ctx.r3.u32, ctx.r4.u32,
                        sa.tracks_from, sa.slots_from, sa.slots_n / 16);
  Record(g_vm, ok, true, Ns(t[0], t[1]), Ns(t[2], t[3]), what);
}

#endif

}  // namespace

REX_HOOK_RAW(sub_834260A0) {
#if defined(__aarch64__)
  if (Enabled(REXCVAR_GET(gtav_nr_vm))) {
    if (CheckNow()) [[unlikely]]
      VmCheck(ctx, base);
    Vm834260A0(ctx, base);
    return;
  }
#endif
  __imp__sub_834260A0(ctx, base);
}

namespace {

#if defined(__aarch64__)

constexpr uint32_t kPackMaskTable = 0x83E2BBD0u;
constexpr uint32_t kPackConstA = 0x8206F5F4u;
constexpr uint32_t kPackConstB = 0x8206F5F0u;
constexpr uint32_t kPackDefaultBias = 0x82000E44u;

inline float PackBits(uint32_t bits) {
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

inline uint32_t PackRound(uint32_t bits) {
  const double d = double(PackBits(bits));
  const float f = float(d);
  uint32_t out;
  std::memcpy(&out, &f, 4);
  return out;
}

inline uint32x4_t PackHalf(uint16x4_t h16) {
  const uint32x4_t h = vmovl_u16(h16);
  const uint32x4_t e = vandq_u32(h, vdupq_n_u32(0x7C00u));
  const uint32x4_t sign = vshlq_n_u32(vandq_u32(h, vdupq_n_u32(0x8000u)), 16);
  const uint32x4_t full = vorrq_u32(vorrq_u32(sign, vshlq_n_u32(vaddq_u32(e, vdupq_n_u32(0x1C000u)), 13)),
                                    vshlq_n_u32(vandq_u32(h, vdupq_n_u32(0x3FFu)), 13));
  return vbslq_u32(vceqq_u32(e, vdupq_n_u32(0)), sign, full);
}

inline uint16x8_t PackHalves(uint8_t* base, uint32_t a) {
  return vreinterpretq_u16_u8(vrev16q_u8(vld1q_u8(Host(base, a & ~15u))));
}

inline uint16x4_t PackLvlxHalves(uint8_t* base, uint32_t ea) {
  static const uint8_t kIota[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
  const uint8x16_t block = vld1q_u8(Host(base, ea & ~15u));
  const uint8x16_t shifted = vqtbl1q_u8(block, vaddq_u8(vld1q_u8(kIota), vdupq_n_u8(uint8_t(ea & 15))));
  return vget_low_u16(vreinterpretq_u16_u8(vrev16q_u8(shifted)));
}

inline float32x4_t PackRaw(uint8_t* base, uint32_t a) {
  return vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(Host(base, a & ~15u))));
}

struct PackLoop {
  uint32_t us, gs, qs, ps, rs, hs, ks;
  uint8x16x2_t table;
  float32x4_t vx, vy, vz, ca, cb, bias, near2, far2;
};

__attribute__((always_inline)) inline uint32x4_t PackCull(uint8_t* base, const PackLoop& l, uint32_t step) {
  const uint16x8_t h = PackHalves(base, l.hs + 16 * step);
  const uint16x4_t u = PackLvlxHalves(base, l.us + 8 * step);
  const float32x4_t p = PackRaw(base, l.ps + 16 * step);
  const uint16x4_t g = PackLvlxHalves(base, l.gs + 8 * step);
  const uint16x8_t k = PackHalves(base, l.ks + 16 * step);
  const float32x4_t q = PackRaw(base, l.qs + 16 * step);
  const float32x4_t r = PackRaw(base, l.rs + 16 * step);
  const float32x4_t x = vreinterpretq_f32_u32(PackHalf(vget_low_u16(vuzp1q_u16(h, h))));
  const float32x4_t y = vreinterpretq_f32_u32(PackHalf(vget_low_u16(vuzp2q_u16(h, h))));
  const float32x4_t z = vreinterpretq_f32_u32(PackHalf(vget_low_u16(vuzp1q_u16(k, k))));
  const float32x4_t dx = vsubq_f32(vaddq_f32(q, x), l.vx);
  const float32x4_t dy = vsubq_f32(vaddq_f32(p, y), l.vy);
  const float32x4_t dz = vsubq_f32(vaddq_f32(r, z), l.vz);
  const float32x4_t dist = vaddq_f32(vmulq_f32(dz, dz), vaddq_f32(vmulq_f32(dx, dx), vmulq_f32(dy, dy)));
  const float32x4_t uf = vcvtq_f32_u32(vmovl_u16(u));
  const uint32x4_t kind = vmovl_u16(vshr_n_u16(vand_u16(g, vdup_n_u16(0x70)), 4));
  const uint32x4_t pick = vmlaq_u32(vdupq_n_u32(0x03020100u), kind, vdupq_n_u32(0x04040404u));
  const float32x4_t scale = vreinterpretq_f32_u8(vqtbl2q_u8(l.table, vreinterpretq_u8_u32(pick)));
  const uint32x4_t special = vandq_u32(vceqq_u32(kind, vdupq_n_u32(5)), vcgeq_f32(l.ca, uf));
  const float32x4_t offset = vbslq_f32(special, l.cb, l.ca);
  const float32x4_t radius = vaddq_f32(vaddq_f32(vmulq_f32(uf, scale), offset), l.bias);
  return vorrq_u32(vcgtq_f32(dist, vmulq_f32(radius, radius)),
                   vorrq_u32(vcgtq_f32(dist, l.far2), vcgtq_f32(l.near2, dist)));
}

inline bool PackDisjoint(uint64_t a0, uint64_t a1, uint64_t b0, uint64_t b1) { return a1 <= b0 || b1 <= a0; }

inline bool PackApart(const PackLoop& l, uint32_t out, uint32_t n) {
  const uint64_t o0 = out & ~15u;
  const uint64_t o1 = o0 + (n > 128 ? uint64_t(((n - 1) >> 5) + 1) * 16 : 64);
  const uint64_t narrow = 8ull * n + 16, wide = 16ull * n + 16;
  const uint32_t small[2] = {l.us, l.gs};
  for (uint32_t a : small)
    if (!PackDisjoint(o0, o1, uint64_t(a & ~15u), uint64_t(a) + narrow)) return false;
  const uint32_t big[5] = {l.qs, l.ps, l.rs, l.hs, l.ks};
  for (uint32_t a : big)
    if (!PackDisjoint(o0, o1, uint64_t(a & ~15u), uint64_t(a) + wide)) return false;
  return PackDisjoint(o0, o1, kPackMaskTable, uint64_t(kPackMaskTable) + 512);
}

inline bool PackRunnable(const PPCContext& ctx, uint8_t* base) {
  if (ctx.r1.u32 & 15) return false;
  const uint32_t out = ctx.r3.u32 & ~15u, list = ctx.r5.u32;
  const int32_t n = int32_t(Load32(base, list + 4));
  const uint32_t size = n > 128 ? uint32_t(((n - 1) >> 5) + 1) * 16u : 64u;
  if (n > (1 << 24) || !Flat(out, size)) return false;
  return uint64_t(out) + size <= list || uint64_t(list) + 56 <= out;
}

inline uint32_t PackRegionSize(const PPCContext& ctx, uint8_t* base) {
  const int32_t n = int32_t(Load32(base, ctx.r5.u32 + 4));
  return n > 128 ? uint32_t(((n - 1) >> 5) + 1) * 16u : 64u;
}

void Pack8358AE80(PPCContext& ctx, uint8_t* base) {
  const uint32_t out = ctx.r3.u32, holder = ctx.r4.u32, list = ctx.r5.u32;
  const bool own = (ctx.r7.u32 & 0xFF) != 0;
  const uint32_t obj = Load32(base, holder);
  const uint32_t fill = PackRound(Load32(base, obj + 9704));
  uint32x4_t lo, hi;
  if (own) {
    uint32_t w[8];
    for (uint32_t k = 0; k < 8; ++k) w[k] = Load32(base, obj + 9616 + 4 * k);
    lo = vld1q_u32(w);
    hi = vld1q_u32(w + 4);
  } else {
    lo = vdupq_n_u32(fill);
    hi = lo;
  }
  const uint8x16x2_t table = {{vreinterpretq_u8_u32(lo), vreinterpretq_u8_u32(hi)}};
  std::memset(Host(base, out & ~15u), 0, 16);
  std::memset(Host(base, (out + 16) & ~15u), 0, 16);
  std::memset(Host(base, (out + 32) & ~15u), 0, 16);
  std::memset(Host(base, (out + 48) & ~15u), 0, 16);
  const float32x4_t ca = vdupq_n_f32(PackBits(LvlxWord0(base, kPackConstA)));
  const float32x4_t cb = vdupq_n_f32(PackBits(LvlxWord0(base, kPackConstB)));
  const uint32_t bias_src = own ? Load32(base, Load32(base, holder) + 9568) : Load32(base, kPackDefaultBias);
  const float32x4_t bias = vdupq_n_f32(PackBits(PackRound(bias_src)));
  const uint32_t obj2 = Load32(base, holder);
  const int32_t first = int32_t(Load32(base, list + 4));
  const float32x4_t vx = vdupq_n_f32(PackBits(ctx.v1.u32[3]));
  const float32x4_t vy = vdupq_n_f32(PackBits(ctx.v1.u32[2]));
  const float32x4_t vz = vdupq_n_f32(PackBits(ctx.v1.u32[1]));
  const float32x4_t near_v = vdupq_n_f32(PackBits(LvlxWord0(base, obj2 + 9604)));
  const float32x4_t far_v = vdupq_n_f32(PackBits(LvlxWord0(base, obj2 + 9608)));
  const float32x4_t near2 = vmulq_f32(near_v, near_v);
  const float32x4_t far2 = vmulq_f32(far_v, far_v);
  ctx.lr = 0x8358AE88;
  if (first <= 0) return;
  PackLoop loop;
  loop.us = Load32(base, list + 20);
  loop.gs = Load32(base, list + 24);
  loop.qs = Load32(base, list + 28);
  loop.ps = Load32(base, list + 32);
  loop.rs = Load32(base, list + 36);
  loop.hs = Load32(base, list + 48);
  loop.ks = Load32(base, list + 52);
  loop.table = table;
  loop.vx = vx;
  loop.vy = vy;
  loop.vz = vz;
  loop.ca = ca;
  loop.cb = cb;
  loop.bias = bias;
  loop.near2 = near2;
  loop.far2 = far2;
  const int32_t n = first;
  if (PackApart(loop, out, uint32_t(n))) {
    uint32_t group = 0;
    uint8_t* dst = Host(base, out & ~15u);
    uint8x16_t acc = vld1q_u8(dst);
    for (int32_t i = 0; i < n; ++i) {
      const uint32_t step = uint32_t(i);
      if ((step >> 5) != group) {
        vst1q_u8(dst, acc);
        group = step >> 5;
        dst = Host(base, (out + (group << 4)) & ~15u);
        acc = vld1q_u8(dst);
      }
      const uint8x16_t mask = vld1q_u8(Host(base, (kPackMaskTable + ((step & 31) << 4)) & ~15u));
      acc = vbslq_u8(mask, vreinterpretq_u8_u32(PackCull(base, loop, step)), acc);
    }
    vst1q_u8(dst, acc);
    return;
  }
  for (int32_t i = 0; i < n; ++i) {
    const uint32_t step = uint32_t(i);
    const uint32x4_t cull = PackCull(base, loop, step);
    const uint8x16_t mask = vld1q_u8(Host(base, (kPackMaskTable + ((step & 31) << 4)) & ~15u));
    uint8_t* dst = Host(base, (out + ((step >> 5) << 4)) & ~15u);
    vst1q_u8(dst, vbslq_u8(mask, vreinterpretq_u8_u32(cull), vld1q_u8(dst)));
  }
}

bool PackSameRegs(const PPCContext& a, const PPCContext& b) { return a.r3.u64 == b.r3.u64 && a.r1.u64 == b.r1.u64; }

#endif

}  // namespace

REX_HOOK_RAW(sub_8358AE80) {
#if defined(__aarch64__)
  if (Enabled(REXCVAR_GET(gtav_nr_pack)) && PackRunnable(ctx, base)) {
    if (CheckNow()) [[unlikely]] {
      const uint32_t size = PackRegionSize(ctx, base);
      if (size <= (1u << 20)) {
        Differential(g_pack, ctx, base, __imp__sub_8358AE80, {{ctx.r3.u32 & ~15u, size}}, PackSameRegs,
                     Pack8358AE80);
        return;
      }
    }
    Pack8358AE80(ctx, base);
    return;
  }
#endif
  __imp__sub_8358AE80(ctx, base);
}

namespace {

#if defined(__aarch64__)

inline uint32x4_t GatherLd(uint8_t* base, uint32_t a) {
  return vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(Host(base, a & ~15u))));
}

inline void GatherSt(uint8_t* base, uint32_t a, uint32x4_t v) {
  vst1q_u8(Host(base, a & ~15u), vrev32q_u8(vreinterpretq_u8_u32(v)));
}

inline uint32_t GatherRotl(uint32_t v, int n) { return (v << n) | (v >> ((32 - n) & 31)); }

inline float32x4_t GatherF(uint32x4_t v) { return vreinterpretq_f32_u32(v); }

inline uint32x4_t GatherU(float32x4_t v) { return vreinterpretq_u32_f32(v); }

inline uint32x4_t GatherMul(uint32x4_t a, uint32x4_t b) { return GatherU(vmulq_f32(GatherF(a), GatherF(b))); }

inline uint32x4_t GatherAdd(uint32x4_t a, uint32x4_t b) { return GatherU(vaddq_f32(GatherF(a), GatherF(b))); }

inline uint32x4_t GatherMadd(uint32x4_t a, uint32x4_t b, uint32x4_t c) {
  return GatherU(vaddq_f32(vmulq_f32(GatherF(a), GatherF(b)), GatherF(c)));
}

inline uint32x4_t GatherNmsub(uint32x4_t a, uint32x4_t b, uint32x4_t c) {
  return veorq_u32(GatherU(vsubq_f32(vmulq_f32(GatherF(a), GatherF(b)), GatherF(c))), vdupq_n_u32(0x80000000u));
}

template <int L>
inline uint32x4_t GatherMulLane(uint32x4_t a, uint32x4_t s) {
  return GatherU(vmulq_laneq_f32(GatherF(a), GatherF(s), L));
}

template <int L>
inline uint32x4_t GatherMaddLane(uint32x4_t a, uint32x4_t s, uint32x4_t c) {
  return GatherU(vaddq_f32(vmulq_laneq_f32(GatherF(a), GatherF(s), L), GatherF(c)));
}

template <int A, int B, int C, int D>
inline uint32x4_t GatherPerm(uint32x4_t v) {
  return __builtin_shufflevector(v, v, A, B, C, D);
}

inline void GatherStore32(uint8_t* base, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(Host(base, a), &v, 4);
}

struct GatherHeader {
  uint32_t poses, lists, parents, source, out, world, bytes, pairs;
  uint8_t skip, copied;
};

inline GatherHeader GatherRead(const PPCContext& ctx, uint8_t* base) {
  const uint32_t h = ctx.r3.u32;
  GatherHeader g;
  g.out = Load32(base, h + 16);
  g.bytes = Load32(base, h + 20) << 6;
  g.poses = Load32(base, h + 0);
  g.lists = Load32(base, h + 4);
  g.world = g.bytes + g.out;
  g.parents = Load32(base, h + 8);
  g.source = Load32(base, h + 12);
  g.pairs = Load32(base, h + 24);
  g.skip = *Host(base, h + 28);
  g.copied = *Host(base, h + 32);
  return g;
}

__attribute__((noinline)) uint32_t GatherTranslations(uint8_t* base, uint32_t r11, uint32_t n, uint32_t r29,
                                                      uint32_t r31) {
  r31 &= ~15u;
  const uint32x4_t kGatherOnes = vdupq_n_u32(0xFFFFFFFFu);
  for (uint32_t k = 0; k < n; ++k) {
    const uint32_t gP1_1 = uint32_t(Load16(base, r11 + 2u));
    const uint32_t gP1_2 = uint32_t(Load16(base, r11 + 6u));
    const uint32_t gP1_3 = uint32_t(Load16(base, r11 + 10u));
    const uint32_t gP1_4 = GatherRotl(gP1_1, 6);
    const uint32_t gP1_5 = uint32_t(Load16(base, r11 + 14u));
    const uint32_t gP1_6 = GatherRotl(gP1_2, 6);
    const uint32_t gP1_7 = uint32_t(Load16(base, r11 + 0u));
    const uint32_t gP1_8 = gP1_4 + r31;
    const uint32_t gP1_9 = uint32_t(Load16(base, r11 + 4u));
    const uint32_t gP1_10 = GatherRotl(gP1_3, 6);
    const uint32_t gP1_11 = uint32_t(Load16(base, r11 + 8u));
    const uint32_t gP1_12 = GatherRotl(gP1_5, 6);
    const uint32_t gP1_13 = uint32_t(Load16(base, r11 + 12u));
    const uint32_t gP1_14 = gP1_6 + r31;
    const uint32_t gP1_15 = gP1_8 + uint32_t(48);
    const uint32_t gP1_16 = gP1_10 + r31;
    const uint32x4_t vP1_17 = GatherLd(base, gP1_7 + r29);
    const uint32_t gP1_18 = gP1_12 + r31;
    const uint32x4_t vP1_19 = GatherLd(base, gP1_9 + r29);
    const uint32_t gP1_20 = gP1_14 + uint32_t(48);
    const uint32x4_t vP1_21 = GatherLd(base, gP1_11 + r29);
    const uint32_t gP1_22 = gP1_16 + uint32_t(48);
    const uint32x4_t vP1_23 = vceqq_u32(vP1_17, kGatherOnes);
    const uint32_t gP1_24 = gP1_18 + uint32_t(48);
    const uint32x4_t vP1_25 = GatherLd(base, gP1_13 + r29);
    const uint32x4_t vP1_26 = vceqq_u32(vP1_19, kGatherOnes);
    const uint32x4_t vP1_27 = GatherLd(base, 0u + gP1_15);
    const uint32x4_t vP1_28 = vceqq_u32(vP1_21, kGatherOnes);
    const uint32_t gP1_29 = r11 + uint32_t(16);
    const uint32x4_t vP1_30 = vceqq_u32(vP1_25, kGatherOnes);
    const uint32x4_t vP1_31 = GatherLd(base, 0u + gP1_20);
    const uint32x4_t vP1_32 = GatherLd(base, 0u + gP1_22);
    const uint32x4_t vP1_33 = vbslq_u32(vP1_23, vP1_27, vP1_17);
    const uint32x4_t vP1_34 = GatherLd(base, 0u + gP1_24);
    const uint32x4_t vP1_35 = vbslq_u32(vP1_26, vP1_31, vP1_19);
    const uint32x4_t vP1_36 = vbslq_u32(vP1_28, vP1_32, vP1_21);
    const uint32x4_t vP1_37 = vbslq_u32(vP1_30, vP1_34, vP1_25);
    GatherSt(base, 0u + gP1_15, vP1_33);
    GatherSt(base, 0u + gP1_20, vP1_35);
    GatherSt(base, 0u + gP1_22, vP1_36);
    GatherSt(base, 0u + gP1_24, vP1_37);
    r11 = gP1_29;
  }
  return r11;
}

__attribute__((noinline)) uint32_t GatherRotations(uint8_t* base, uint32_t r11, uint32_t n, uint32_t r29,
                                                   uint32_t r31) {
  r31 &= ~15u;
  const uint32x4_t kGatherOnes = vdupq_n_u32(0xFFFFFFFFu);
  const uint32x4_t kGatherOne = vdupq_n_u32(0x3F800000u);
  const uint32x4_t kGatherPacked = vcombine_u32(vdup_n_u32(0), vdup_n_u32(0xFFFFFFFFu));
  const uint32x4_t kGatherLane0 = vsetq_lane_u32(0xFFFFFFFFu, vdupq_n_u32(0), 0);
  for (uint32_t k = 0; k < n; ++k) {
    const uint32_t gP2_1 = uint32_t(Load16(base, r11 + 0u));
    const uint32_t gP2_2 = uint32_t(Load16(base, r11 + 4u));
    const uint32_t gP2_3 = uint32_t(Load16(base, r11 + 2u));
    const uint32_t gP2_4 = uint32_t(Load16(base, r11 + 6u));
    const uint32_t gP2_5 = uint32_t(Load16(base, r11 + 10u));
    const uint32_t gP2_6 = GatherRotl(gP2_3, 6);
    const uint32x4_t vP2_7 = GatherLd(base, gP2_1 + r29);
    const uint32_t gP2_8 = uint32_t(Load16(base, r11 + 14u));
    const uint32x4_t vP2_9 = GatherAdd(vP2_7, vP2_7);
    const uint32x4_t vP2_10 = GatherLd(base, gP2_2 + r29);
    const uint32x4_t vP2_11 = GatherPerm<1, 2, 0, 3>(vP2_7);
    const uint32x4_t vP2_12 = GatherAdd(vP2_10, vP2_10);
    const uint32x4_t vP2_13 = vdupq_laneq_u32(vP2_7, 3);
    const uint32_t gP2_14 = uint32_t(Load16(base, r11 + 8u));
    const uint32x4_t vP2_15 = GatherPerm<2, 0, 1, 3>(vP2_7);
    const uint32_t gP2_16 = GatherRotl(gP2_4, 6);
    const uint32_t gP2_17 = GatherRotl(gP2_5, 6);
    const uint32_t gP2_18 = GatherRotl(gP2_8, 6);
    const uint32_t gP2_19 = gP2_6 + r31;
    const uint32_t gP2_20 = gP2_16 + r31;
    const uint32x4_t vP2_21 = GatherLd(base, gP2_14 + r29);
    const uint32_t gP2_22 = gP2_17 + r31;
    const uint32_t gP2_23 = uint32_t(Load16(base, r11 + 12u));
    const uint32_t gP2_24 = gP2_18 + r31;
    const uint32_t gP2_25 = gP2_19 + uint32_t(16);
    const uint32x4_t vP2_26 = GatherPerm<1, 2, 0, 3>(vP2_10);
    const uint32_t gP2_27 = gP2_19 + uint32_t(32);
    const uint32x4_t vP2_28 = vdupq_laneq_u32(vP2_10, 3);
    const uint32x4_t vP2_29 = GatherPerm<1, 2, 0, 3>(vP2_9);
    const uint32x4_t vP2_30 = GatherMul(vP2_11, vP2_9);
    const uint32x4_t vP2_31 = GatherPerm<2, 0, 1, 3>(vP2_9);
    const uint32_t gP2_32 = gP2_20 + uint32_t(16);
    const uint32_t gP2_33 = gP2_20 + uint32_t(32);
    const uint32x4_t vP2_34 = GatherPerm<1, 2, 0, 3>(vP2_12);
    const uint32_t gP2_35 = gP2_22 + uint32_t(16);
    const uint32x4_t vP2_36 = GatherLd(base, 0u + gP2_19);
    const uint32x4_t vP2_37 = GatherMulLane<3>(vP2_29, vP2_7);
    const uint32_t gP2_38 = gP2_22 + uint32_t(32);
    const uint32_t gP2_39 = gP2_24 + uint32_t(16);
    const uint32x4_t vP2_40 = GatherLd(base, 0u + gP2_20);
    const uint32_t gP2_41 = gP2_24 + uint32_t(32);
    const uint32x4_t vP2_42 = GatherLd(base, gP2_23 + r29);
    const uint32x4_t vP2_43 = GatherNmsub(vP2_29, vP2_11, kGatherOne);
    const uint32x4_t vP2_44 = GatherLd(base, 0u + gP2_25);
    const uint32x4_t vP2_45 = GatherLd(base, 0u + gP2_27);
    const uint32x4_t vP2_46 = GatherMul(vP2_26, vP2_12);
    const uint32x4_t vP2_47 = GatherLd(base, 0u + gP2_32);
    const uint32x4_t vP2_48 = GatherPerm<2, 0, 1, 3>(vP2_12);
    const uint32x4_t vP2_49 = GatherLd(base, 0u + gP2_33);
    const uint32x4_t vP2_50 = GatherMulLane<3>(vP2_34, vP2_10);
    const uint32x4_t vP2_51 = GatherNmsub(vP2_13, vP2_31, vP2_30);
    const uint32x4_t vP2_52 = GatherLd(base, 0u + gP2_22);
    const uint32x4_t vP2_53 = GatherLd(base, 0u + gP2_35);
    const uint32x4_t vP2_54 = GatherLd(base, 0u + gP2_38);
    const uint32x4_t vP2_55 = GatherLd(base, 0u + gP2_24);
    const uint32x4_t vP2_56 = GatherMadd(vP2_15, vP2_9, vP2_37);
    const uint32x4_t vP2_57 = GatherLd(base, 0u + gP2_39);
    const uint32x4_t vP2_58 = GatherLd(base, 0u + gP2_41);
    const uint32x4_t vP2_59 = GatherNmsub(vP2_31, vP2_15, vP2_43);
    const uint32x4_t vP2_60 = vbslq_u32(kGatherLane0, vP2_59, vP2_56);
    const uint32x4_t vP2_61 = vbslq_u32(kGatherPacked, vP2_51, vP2_60);
    const uint32x4_t vP2_62 = GatherAdd(vP2_21, vP2_21);
    const uint32x4_t vP2_63 = GatherNmsub(vP2_34, vP2_26, kGatherOne);
    const uint32x4_t vP2_64 = GatherPerm<2, 0, 1, 3>(vP2_10);
    const uint32x4_t vP2_65 = vbslq_u32(kGatherLane0, vP2_51, vP2_59);
    const uint32x4_t vP2_66 = GatherNmsub(vP2_28, vP2_48, vP2_46);
    const uint32x4_t vP2_67 = GatherPerm<1, 2, 0, 3>(vP2_62);
    const uint32x4_t vP2_68 = vdupq_laneq_u32(vP2_21, 3);
    const uint32x4_t vP2_69 = vbslq_u32(kGatherLane0, vP2_56, vP2_51);
    const uint32x4_t vP2_70 = GatherPerm<1, 2, 0, 3>(vP2_21);
    const uint32x4_t vP2_71 = GatherPerm<2, 0, 1, 3>(vP2_21);
    const uint32x4_t vP2_72 = GatherPerm<2, 0, 1, 3>(vP2_62);
    const uint32x4_t vP2_73 = GatherMulLane<3>(vP2_67, vP2_21);
    const uint32x4_t vP2_74 = vbslq_u32(kGatherPacked, vP2_59, vP2_69);
    const uint32x4_t vP2_75 = GatherMul(vP2_70, vP2_62);
    const uint32x4_t vP2_76 = GatherMadd(vP2_64, vP2_12, vP2_50);
    const uint32x4_t vP2_77 = GatherAdd(vP2_42, vP2_42);
    const uint32x4_t vP2_78 = GatherNmsub(vP2_48, vP2_64, vP2_63);
    const uint32x4_t vP2_79 = GatherPerm<1, 2, 0, 3>(vP2_42);
    const uint32x4_t vP2_80 = GatherNmsub(vP2_67, vP2_70, kGatherOne);
    const uint32x4_t vP2_81 = vdupq_laneq_u32(vP2_42, 3);
    const uint32x4_t vP2_82 = vbslq_u32(kGatherPacked, vP2_56, vP2_65);
    const uint32x4_t vP2_83 = GatherPerm<2, 0, 1, 3>(vP2_42);
    const uint32_t gP2_84 = r11 + uint32_t(16);
    const uint32x4_t vP2_85 = GatherMadd(vP2_71, vP2_62, vP2_73);
    const uint32x4_t vP2_86 = GatherMul(vP2_79, vP2_77);
    const uint32x4_t vP2_87 = GatherPerm<1, 2, 0, 3>(vP2_77);
    const uint32x4_t vP2_88 = GatherPerm<2, 0, 1, 3>(vP2_77);
    const uint32x4_t vP2_89 = vbslq_u32(kGatherLane0, vP2_76, vP2_66);
    const uint32x4_t vP2_90 = vbslq_u32(kGatherLane0, vP2_66, vP2_78);
    const uint32x4_t vP2_91 = vbslq_u32(kGatherLane0, vP2_78, vP2_76);
    const uint32x4_t vP2_92 = vbslq_u32(kGatherPacked, vP2_76, vP2_90);
    const uint32x4_t vP2_93 = GatherMulLane<3>(vP2_87, vP2_42);
    const uint32x4_t vP2_94 = vbslq_u32(kGatherPacked, vP2_66, vP2_91);
    const uint32x4_t vP2_95 = vbslq_u32(kGatherPacked, vP2_78, vP2_89);
    const uint32x4_t vP2_96 = GatherNmsub(vP2_72, vP2_71, vP2_80);
    const uint32x4_t vP2_97 = GatherNmsub(vP2_68, vP2_72, vP2_75);
    const uint32x4_t vP2_98 = GatherNmsub(vP2_81, vP2_88, vP2_86);
    const uint32x4_t vP2_99 = GatherNmsub(vP2_87, vP2_79, kGatherOne);
    const uint32x4_t vP2_100 = GatherMadd(vP2_83, vP2_77, vP2_93);
    const uint32x4_t vP2_101 = GatherNmsub(vP2_88, vP2_83, vP2_99);
    const uint32x4_t vP2_102 = vbslq_u32(kGatherLane0, vP2_96, vP2_85);
    const uint32x4_t vP2_103 = vbslq_u32(kGatherLane0, vP2_97, vP2_96);
    const uint32x4_t vP2_104 = vbslq_u32(kGatherLane0, vP2_85, vP2_97);
    const uint32x4_t vP2_105 = vbslq_u32(kGatherLane0, vP2_100, vP2_98);
    const uint32x4_t vP2_106 = vceqq_u32(vP2_7, kGatherOnes);
    const uint32x4_t vP2_107 = vbslq_u32(kGatherPacked, vP2_85, vP2_103);
    const uint32x4_t vP2_108 = vbslq_u32(kGatherLane0, vP2_101, vP2_100);
    const uint32x4_t vP2_109 = vbslq_u32(kGatherPacked, vP2_97, vP2_102);
    const uint32x4_t vP2_110 = vbslq_u32(kGatherLane0, vP2_98, vP2_101);
    const uint32x4_t vP2_111 = vbslq_u32(kGatherPacked, vP2_101, vP2_105);
    const uint32x4_t vP2_112 = vceqq_u32(vP2_10, kGatherOnes);
    const uint32x4_t vP2_113 = vbslq_u32(kGatherPacked, vP2_96, vP2_104);
    const uint32x4_t vP2_114 = vbslq_u32(kGatherPacked, vP2_100, vP2_110);
    const uint32x4_t vP2_115 = vceqq_u32(vP2_21, kGatherOnes);
    const uint32x4_t vP2_116 = vbslq_u32(vP2_106, vP2_44, vP2_82);
    const uint32x4_t vP2_117 = vbslq_u32(vP2_106, vP2_45, vP2_74);
    const uint32x4_t vP2_118 = vbslq_u32(vP2_106, vP2_36, vP2_61);
    const uint32x4_t vP2_119 = vbslq_u32(vP2_112, vP2_49, vP2_95);
    const uint32x4_t vP2_120 = vceqq_u32(vP2_42, kGatherOnes);
    GatherSt(base, 0u + gP2_25, vP2_116);
    const uint32x4_t vP2_121 = vbslq_u32(kGatherPacked, vP2_98, vP2_108);
    GatherSt(base, 0u + gP2_27, vP2_117);
    const uint32x4_t vP2_122 = vbslq_u32(vP2_112, vP2_40, vP2_94);
    GatherSt(base, 0u + gP2_19, vP2_118);
    const uint32x4_t vP2_123 = vbslq_u32(vP2_115, vP2_52, vP2_109);
    GatherSt(base, 0u + gP2_33, vP2_119);
    const uint32x4_t vP2_124 = vbslq_u32(vP2_115, vP2_53, vP2_107);
    const uint32x4_t vP2_125 = vbslq_u32(vP2_112, vP2_47, vP2_92);
    const uint32x4_t vP2_126 = vbslq_u32(vP2_115, vP2_54, vP2_113);
    GatherSt(base, 0u + gP2_20, vP2_122);
    const uint32x4_t vP2_127 = vbslq_u32(vP2_120, vP2_55, vP2_121);
    const uint32x4_t vP2_128 = vbslq_u32(vP2_120, vP2_57, vP2_114);
    const uint32x4_t vP2_129 = vbslq_u32(vP2_120, vP2_58, vP2_111);
    GatherSt(base, 0u + gP2_32, vP2_125);
    GatherSt(base, 0u + gP2_22, vP2_123);
    GatherSt(base, 0u + gP2_35, vP2_124);
    GatherSt(base, 0u + gP2_38, vP2_126);
    GatherSt(base, 0u + gP2_24, vP2_127);
    GatherSt(base, 0u + gP2_39, vP2_128);
    GatherSt(base, 0u + gP2_41, vP2_129);
    r11 = gP2_84;
  }
  return r11;
}

__attribute__((noinline)) void GatherScales(uint8_t* base, uint32_t r11, uint32_t n, uint32_t r29, uint32_t r31) {
  r31 &= ~15u;
  const uint32x4_t kGatherOnes = vdupq_n_u32(0xFFFFFFFFu);
  for (uint32_t k = 0; k < n; ++k) {
    const uint32_t gP3_1 = uint32_t(Load16(base, r11 + 4u));
    const uint32_t gP3_2 = uint32_t(Load16(base, r11 + 2u));
    const uint32_t gP3_3 = GatherRotl(gP3_1, 6);
    const uint32_t gP3_4 = uint32_t(Load16(base, r11 + 8u));
    const uint32_t gP3_5 = uint32_t(Load16(base, r11 + 6u));
    const uint32_t gP3_6 = uint32_t(Load16(base, r11 + 10u));
    const uint32_t gP3_7 = gP3_3 + r31;
    const uint32_t gP3_8 = uint32_t(Load16(base, r11 + 14u));
    const uint32_t gP3_9 = GatherRotl(gP3_4, 6);
    const uint32_t gP3_10 = uint32_t(Load16(base, r11 + 12u));
    const uint32x4_t vP3_11 = GatherLd(base, gP3_2 + r29);
    const uint32_t gP3_12 = r11 + 16u;
    const uint32_t gP3_13 = uint32_t(Load16(base, gP3_12));
    const uint32_t gP3_14 = gP3_7 + uint32_t(16);
    const uint32_t gP3_15 = GatherRotl(gP3_10, 6);
    const uint32_t gP3_17 = GatherRotl(gP3_13, 6);
    const uint32_t gP3_19 = gP3_9 + r31;
    const uint32x4_t vP3_20 = GatherLd(base, 0u + gP3_7);
    const uint32_t gP3_21 = gP3_15 + r31;
    const uint32x4_t vP3_22 = GatherLd(base, gP3_5 + r29);
    const uint32_t gP3_23 = gP3_17 + r31;
    const uint32x4_t vP3_24 = GatherLd(base, 0u + gP3_14);
    const uint32_t gP3_25 = gP3_7 + uint32_t(32);
    const uint32x4_t vP3_26 = GatherLd(base, gP3_6 + r29);
    const uint32x4_t vP3_27 = GatherLd(base, gP3_8 + r29);
    const uint32x4_t vP3_28 = GatherMulLane<0>(vP3_20, vP3_11);
    const uint32_t gP3_29 = gP3_19 + uint32_t(16);
    const uint32x4_t vP3_30 = GatherMulLane<1>(vP3_24, vP3_11);
    const uint32_t gP3_31 = gP3_19 + uint32_t(32);
    const uint32x4_t vP3_34 = GatherLd(base, 0u + gP3_19);
    const uint32x4_t vP3_36 = GatherLd(base, 0u + gP3_25);
    const uint32x4_t vP3_38 = vceqq_u32(vP3_11, kGatherOnes);
    const uint32x4_t vP3_39 = GatherLd(base, 0u + gP3_29);
    const uint32x4_t vP3_40 = GatherMulLane<2>(vP3_36, vP3_11);
    const uint32x4_t vP3_41 = GatherLd(base, 0u + gP3_31);
    const uint32x4_t vP3_42 = GatherMulLane<0>(vP3_34, vP3_22);
    const uint32x4_t vP3_43 = GatherMulLane<1>(vP3_39, vP3_22);
    const uint32x4_t vP3_45 = GatherMulLane<2>(vP3_41, vP3_22);
    const uint32x4_t vP3_46 = GatherLd(base, 0u + gP3_21);
    const uint32_t gP3_47 = gP3_21 + uint32_t(16);
    const uint32_t gP3_49 = gP3_21 + uint32_t(32);
    const uint32x4_t vP3_51 = vbslq_u32(vP3_38, vP3_20, vP3_28);
    const uint32x4_t vP3_52 = GatherMulLane<0>(vP3_46, vP3_26);
    const uint32x4_t vP3_53 = vbslq_u32(vP3_38, vP3_24, vP3_30);
    const uint32_t gP3_54 = gP3_23 + uint32_t(16);
    const uint32x4_t vP3_55 = vceqq_u32(vP3_22, kGatherOnes);
    const uint32_t gP3_56 = gP3_23 + uint32_t(32);
    const uint32x4_t vP3_58 = vceqq_u32(vP3_26, kGatherOnes);
    const uint32x4_t vP3_60 = GatherLd(base, 0u + gP3_23);
    const uint32x4_t vP3_62 = GatherLd(base, 0u + gP3_47);
    const uint32x4_t vP3_63 = vbslq_u32(vP3_38, vP3_36, vP3_40);
    const uint32x4_t vP3_64 = GatherLd(base, 0u + gP3_49);
    const uint32x4_t vP3_65 = vbslq_u32(vP3_55, vP3_34, vP3_42);
    const uint32x4_t vP3_66 = GatherLd(base, 0u + gP3_54);
    const uint32x4_t vP3_67 = vbslq_u32(vP3_55, vP3_39, vP3_43);
    const uint32x4_t vP3_68 = GatherLd(base, 0u + gP3_56);
    const uint32x4_t vP3_69 = vbslq_u32(vP3_55, vP3_41, vP3_45);
    GatherSt(base, 0u + gP3_14, vP3_53);
    GatherSt(base, 0u + gP3_7, vP3_51);
    const uint32x4_t vP3_70 = GatherMulLane<0>(vP3_60, vP3_27);
    GatherSt(base, 0u + gP3_25, vP3_63);
    const uint32x4_t vP3_71 = GatherMulLane<1>(vP3_62, vP3_26);
    const uint32x4_t vP3_72 = GatherMulLane<2>(vP3_64, vP3_26);
    const uint32x4_t vP3_73 = vbslq_u32(vP3_58, vP3_46, vP3_52);
    const uint32x4_t vP3_74 = GatherMulLane<1>(vP3_66, vP3_27);
    GatherSt(base, 0u + gP3_19, vP3_65);
    const uint32x4_t vP3_75 = GatherMulLane<2>(vP3_68, vP3_27);
    GatherSt(base, 0u + gP3_29, vP3_67);
    const uint32x4_t vP3_76 = vceqq_u32(vP3_27, kGatherOnes);
    GatherSt(base, 0u + gP3_31, vP3_69);
    GatherSt(base, 0u + gP3_21, vP3_73);
    const uint32x4_t vP3_77 = vbslq_u32(vP3_58, vP3_62, vP3_71);
    const uint32x4_t vP3_78 = vbslq_u32(vP3_58, vP3_64, vP3_72);
    const uint32x4_t vP3_79 = vbslq_u32(vP3_76, vP3_60, vP3_70);
    const uint32x4_t vP3_80 = vbslq_u32(vP3_76, vP3_66, vP3_74);
    const uint32x4_t vP3_81 = vbslq_u32(vP3_76, vP3_68, vP3_75);
    GatherSt(base, 0u + gP3_47, vP3_77);
    GatherSt(base, 0u + gP3_49, vP3_78);
    GatherSt(base, 0u + gP3_23, vP3_79);
    GatherSt(base, 0u + gP3_54, vP3_80);
    GatherSt(base, 0u + gP3_56, vP3_81);
    r11 = gP3_12;
  }
}

__attribute__((noinline)) void GatherHierarchy(uint8_t* base, uint32_t r8, uint32_t n, uint32_t r31, uint32_t r30) {
  r31 &= ~15u;
  r30 &= ~15u;
  const uint32_t r9 = 16, r10 = 32, r11 = 48;
  for (uint32_t k = 0; k < n; ++k) {
    const uint32_t gP4_1 = uint32_t(Load16(base, r8 + 6u));
    const uint32_t gP4_2 = uint32_t(Load16(base, r8 + 2u));
    const uint32_t gP4_3 = uint32_t(Load16(base, r8 + 4u));
    const uint32_t gP4_4 = GatherRotl(gP4_1, 6);
    const uint32_t gP4_5 = GatherRotl(gP4_2, 6);
    const uint32_t gP4_6 = uint32_t(Load16(base, r8 + 10u));
    const uint32_t gP4_7 = uint32_t(Load16(base, r8 + 12u));
    const uint32_t gP4_8 = GatherRotl(gP4_3, 6);
    const uint32_t gP4_9 = uint32_t(Load16(base, r8 + 14u));
    const uint32_t gP4_10 = gP4_5 + r31;
    const uint32_t gP4_11 = uint32_t(Load16(base, r8 + 8u));
    const uint32_t gP4_12 = gP4_4 + r31;
    const uint32_t gP4_13 = r8 + 16u;
    const uint32_t gP4_14 = uint32_t(Load16(base, gP4_13));
    const uint32_t gP4_15 = GatherRotl(gP4_6, 6);
    const uint32x4_t vP4_16 = GatherLd(base, gP4_5 + r31);
    const uint32_t gP4_17 = GatherRotl(gP4_11, 6);
    const uint32_t gP4_18 = GatherRotl(gP4_14, 6);
    const uint32x4_t vP4_19 = GatherLd(base, gP4_4 + r31);
    const uint32_t gP4_20 = gP4_8 + r30;
    const uint32_t gP4_22 = GatherRotl(gP4_7, 6);
    const uint32x4_t vP4_23 = GatherLd(base, gP4_10 + r9);
    const uint32x4_t vP4_24 = GatherLd(base, gP4_10 + r10);
    const uint32_t gP4_25 = gP4_17 + r30;
    const uint32x4_t vP4_26 = GatherLd(base, gP4_10 + r11);
    const uint32_t gP4_27 = GatherRotl(gP4_9, 6);
    const uint32x4_t vP4_28 = GatherLd(base, gP4_12 + r9);
    const uint32_t gP4_29 = gP4_22 + r30;
    const uint32x4_t vP4_30 = GatherLd(base, 0u + gP4_20);
    const uint32x4_t vP4_32 = GatherMulLane<0>(vP4_30, vP4_16);
    const uint32x4_t vP4_33 = GatherLd(base, gP4_12 + r10);
    const uint32x4_t vP4_34 = GatherLd(base, gP4_12 + r11);
    const uint32x4_t vP4_36 = GatherLd(base, gP4_15 + r31);
    const uint32_t gP4_38 = gP4_15 + r31;
    const uint32_t gP4_40 = gP4_27 + r31;
    const uint32_t gP4_42 = gP4_18 + r30;
    const uint32x4_t vP4_45 = GatherLd(base, gP4_20 + r11);
    const uint32x4_t vP4_47 = GatherLd(base, 0u + gP4_25);
    const uint32x4_t vP4_49 = GatherLd(base, gP4_25 + r11);
    const uint32x4_t vP4_50 = GatherLd(base, gP4_20 + r9);
    const uint32x4_t vP4_51 = GatherMaddLane<0>(vP4_30, vP4_26, vP4_45);
    const uint32x4_t vP4_52 = GatherLd(base, 0u + gP4_29);
    const uint32x4_t vP4_53 = GatherMulLane<0>(vP4_30, vP4_23);
    const uint32x4_t vP4_54 = GatherMulLane<0>(vP4_30, vP4_24);
    const uint32x4_t vP4_55 = GatherLd(base, gP4_20 + r10);
    const uint32x4_t vP4_56 = GatherMaddLane<0>(vP4_47, vP4_34, vP4_49);
    const uint32x4_t vP4_57 = GatherLd(base, gP4_27 + r31);
    const uint32x4_t vP4_58 = GatherMulLane<0>(vP4_47, vP4_19);
    const uint32x4_t vP4_59 = GatherLd(base, gP4_18 + r30);
    const uint32x4_t vP4_60 = GatherMulLane<0>(vP4_47, vP4_28);
    const uint32x4_t vP4_61 = GatherLd(base, gP4_38 + r9);
    const uint32x4_t vP4_62 = GatherMulLane<0>(vP4_47, vP4_33);
    const uint32x4_t vP4_63 = GatherLd(base, gP4_38 + r10);
    const uint32x4_t vP4_64 = GatherLd(base, gP4_38 + r11);
    const uint32_t gP4_65 = gP4_5 + r30;
    const uint32x4_t vP4_66 = GatherLd(base, gP4_40 + r9);
    const uint32x4_t vP4_68 = GatherLd(base, gP4_40 + r10);
    const uint32x4_t vP4_70 = GatherLd(base, gP4_40 + r11);
    const uint32x4_t vP4_71 = GatherMulLane<0>(vP4_52, vP4_36);
    const uint32x4_t vP4_72 = GatherLd(base, gP4_25 + r9);
    const uint32x4_t vP4_74 = GatherLd(base, gP4_25 + r10);
    const uint32x4_t vP4_76 = GatherLd(base, gP4_29 + r9);
    const uint32x4_t vP4_78 = GatherLd(base, gP4_29 + r10);
    const uint32x4_t vP4_79 = GatherMaddLane<1>(vP4_50, vP4_16, vP4_32);
    const uint32x4_t vP4_80 = GatherLd(base, gP4_29 + r11);
    const uint32x4_t vP4_82 = GatherLd(base, gP4_42 + r9);
    const uint32x4_t vP4_83 = GatherMaddLane<1>(vP4_50, vP4_23, vP4_53);
    const uint32x4_t vP4_86 = GatherMaddLane<1>(vP4_50, vP4_26, vP4_51);
    const uint32x4_t vP4_87 = GatherMaddLane<1>(vP4_72, vP4_28, vP4_60);
    const uint32x4_t vP4_89 = GatherMaddLane<1>(vP4_72, vP4_33, vP4_62);
    const uint32x4_t vP4_91 = GatherMaddLane<1>(vP4_72, vP4_19, vP4_58);
    const uint32x4_t vP4_93 = GatherMaddLane<1>(vP4_72, vP4_34, vP4_56);
    const uint32x4_t vP4_95 = GatherMulLane<0>(vP4_52, vP4_61);
    const uint32x4_t vP4_97 = GatherMaddLane<1>(vP4_50, vP4_24, vP4_54);
    const uint32x4_t vP4_99 = GatherMaddLane<2>(vP4_55, vP4_16, vP4_79);
    const uint32x4_t vP4_101 = GatherMaddLane<1>(vP4_76, vP4_36, vP4_71);
    const uint32_t gP4_104 = gP4_4 + r30;
    const uint32_t gP4_106 = gP4_15 + r30;
    const uint32x4_t vP4_107 = GatherMaddLane<2>(vP4_55, vP4_23, vP4_83);
    const uint32x4_t vP4_109 = GatherMaddLane<2>(vP4_55, vP4_26, vP4_86);
    const uint32x4_t vP4_111 = GatherLd(base, gP4_42 + r11);
    const uint32x4_t vP4_112 = GatherMaddLane<2>(vP4_74, vP4_28, vP4_87);
    const uint32x4_t vP4_113 = GatherMaddLane<2>(vP4_74, vP4_33, vP4_89);
    const uint32x4_t vP4_115 = GatherMaddLane<2>(vP4_74, vP4_19, vP4_91);
    const uint32x4_t vP4_117 = GatherMaddLane<2>(vP4_74, vP4_34, vP4_93);
    const uint32x4_t vP4_119 = GatherMaddLane<1>(vP4_76, vP4_61, vP4_95);
    const uint32x4_t vP4_121 = GatherMaddLane<2>(vP4_55, vP4_24, vP4_97);
    const uint32x4_t vP4_123 = GatherLd(base, gP4_42 + r10);
    const uint32_t gP4_124 = gP4_27 + r30;
    GatherSt(base, gP4_5 + r30, vP4_99);
    const uint32x4_t vP4_125 = GatherMaddLane<2>(vP4_78, vP4_36, vP4_101);
    const uint32x4_t vP4_126 = GatherMulLane<0>(vP4_59, vP4_68);
    GatherSt(base, gP4_65 + r9, vP4_107);
    const uint32x4_t vP4_128 = GatherMaddLane<0>(vP4_52, vP4_64, vP4_80);
    GatherSt(base, gP4_65 + r11, vP4_109);
    const uint32x4_t vP4_130 = GatherMulLane<0>(vP4_59, vP4_66);
    const uint32x4_t vP4_131 = GatherMaddLane<2>(vP4_78, vP4_61, vP4_119);
    const uint32x4_t vP4_133 = GatherMulLane<0>(vP4_52, vP4_63);
    GatherSt(base, gP4_65 + r10, vP4_121);
    GatherSt(base, gP4_4 + r30, vP4_115);
    GatherSt(base, gP4_104 + r10, vP4_113);
    GatherSt(base, gP4_104 + r9, vP4_112);
    const uint32x4_t vP4_136 = GatherMaddLane<0>(vP4_59, vP4_70, vP4_111);
    GatherSt(base, gP4_104 + r11, vP4_117);
    const uint32x4_t vP4_137 = GatherMulLane<0>(vP4_59, vP4_57);
    GatherSt(base, gP4_15 + r30, vP4_125);
    const uint32x4_t vP4_144 = GatherMaddLane<1>(vP4_82, vP4_66, vP4_130);
    GatherSt(base, gP4_106 + r9, vP4_131);
    const uint32x4_t vP4_145 = GatherMaddLane<1>(vP4_76, vP4_63, vP4_133);
    const uint32x4_t vP4_147 = GatherMaddLane<1>(vP4_76, vP4_64, vP4_128);
    const uint32x4_t vP4_149 = GatherMaddLane<1>(vP4_82, vP4_68, vP4_126);
    const uint32x4_t vP4_150 = GatherMaddLane<1>(vP4_82, vP4_70, vP4_136);
    const uint32x4_t vP4_151 = GatherMaddLane<1>(vP4_82, vP4_57, vP4_137);
    const uint32x4_t vP4_152 = GatherMaddLane<2>(vP4_78, vP4_63, vP4_145);
    const uint32x4_t vP4_153 = GatherMaddLane<2>(vP4_78, vP4_64, vP4_147);
    const uint32x4_t vP4_154 = GatherMaddLane<2>(vP4_123, vP4_57, vP4_151);
    GatherSt(base, gP4_106 + r10, vP4_152);
    const uint32x4_t vP4_155 = GatherMaddLane<2>(vP4_123, vP4_66, vP4_144);
    GatherSt(base, gP4_106 + r11, vP4_153);
    const uint32x4_t vP4_156 = GatherMaddLane<2>(vP4_123, vP4_68, vP4_149);
    const uint32x4_t vP4_157 = GatherMaddLane<2>(vP4_123, vP4_70, vP4_150);
    GatherSt(base, gP4_27 + r30, vP4_154);
    GatherSt(base, gP4_124 + r9, vP4_155);
    GatherSt(base, gP4_124 + r10, vP4_156);
    GatherSt(base, gP4_124 + r11, vP4_157);
    r8 = gP4_13;
  }
}

void Gather83442200(PPCContext& ctx, uint8_t* base) {
  const GatherHeader g = GatherRead(ctx, base);
  if (!g.copied) {
    const PPCRegister sp = ctx.r1;
    GatherStore32(base, sp.u32 - 192, sp.u32);
    ctx.r1.u32 = sp.u32 - 192;
    ctx.r3.u64 = g.out;
    ctx.r4.u64 = g.source;
    ctx.r5.u64 = g.bytes;
    ctx.lr = 0x8344224C;
    sub_8378DEF8(ctx, base);
    ctx.r1 = sp;
  }
  const uint32_t n0 = uint32_t(int32_t(Load16(base, g.lists) + 3u) >> 2);
  const uint32_t n1 = uint32_t(int32_t(Load16(base, g.lists + 2) + 3u) >> 2);
  const uint32_t n2 = uint32_t(int32_t(Load16(base, g.lists + 4) + 3u) >> 2);
  uint32_t r11 = GatherTranslations(base, g.lists + 16, n0, g.poses, g.out);
  r11 = GatherRotations(base, r11, n1, g.poses, g.out);
  GatherScales(base, r11 - 2, n2, g.poses, g.out);
  if (!g.skip) {
    GatherSt(base, g.world, GatherLd(base, g.out));
    const uint32x4_t a = GatherLd(base, g.out + 16), b = GatherLd(base, g.out + 32), c = GatherLd(base, g.out + 48);
    GatherSt(base, g.world + 16, a);
    GatherSt(base, g.world + 32, b);
    GatherSt(base, g.world + 48, c);
    const uint32_t n4 = (g.pairs + 7u) >> 3;
    GatherHierarchy(base, g.parents - 2, n4, g.out, g.world);
  }
  ctx.r3.u64 = 1;
}

#endif

#if defined(__aarch64__)

bool GatherRegions(const PPCContext& ctx, uint8_t* base, uint32_t& a0, uint32_t& n0, uint32_t& a1, uint32_t& n1,
                   uint32_t& a2, uint32_t& n2) {
  const GatherHeader g = GatherRead(ctx, base);
  const uint32_t entries = 4u * ((uint32_t(Load16(base, g.lists)) + 3u) >> 2) +
                           4u * ((uint32_t(Load16(base, g.lists + 2)) + 3u) >> 2) +
                           4u * ((uint32_t(Load16(base, g.lists + 4)) + 3u) >> 2);
  const uint32_t pairs = 4u * ((g.pairs + 7u) >> 3);
  if (pairs > 65536u || g.bytes > (1u << 20)) return false;
  if (!g.copied) {
    const uint64_t lo = g.out, hi = uint64_t(g.out) + g.bytes;
    if ((g.lists < hi && uint64_t(g.lists) + 16u + 4u * entries > lo) ||
        (g.parents < hi && uint64_t(g.parents) + 4u * pairs > lo))
      return false;
  }
  uint32_t top = 0;
  for (uint32_t i = 0; i < entries; ++i) top = std::max<uint32_t>(top, Load16(base, g.lists + 16 + 4 * i + 2));
  uint32_t child = 0;
  for (uint32_t i = 0; i < (g.skip ? 0u : pairs); ++i) child = std::max<uint32_t>(child, Load16(base, g.parents + 4 * i));
  a0 = g.out;
  n0 = g.copied ? 0u : g.bytes;
  a1 = g.out & ~15u;
  n1 = entries ? 64u * top + 80u : 0u;
  a2 = g.world & ~15u;
  n2 = g.skip ? 0u : 64u * child + 80u;
  return n1 <= (1u << 20) && n2 <= (1u << 20) && Flat(a0, n0) && Flat(a1, n1) && Flat(a2, n2);
}

#endif

}  // namespace

REX_HOOK_RAW(sub_83442200) {
#if defined(__aarch64__)
  if (Enabled(REXCVAR_GET(gtav_nr_gather))) {
    if (CheckNow()) [[unlikely]] {
      uint32_t a0 = 0, n0 = 0, a1 = 0, n1 = 0, a2 = 0, n2 = 0;
      if (GatherRegions(ctx, base, a0, n0, a1, n1, a2, n2)) {
        Differential(g_gather, ctx, base, __imp__sub_83442200, {{a0, n0}, {a1, n1}, {a2, n2}}, SameResult,
                     Gather83442200);
        return;
      }
    }
    Gather83442200(ctx, base);
    return;
  }
#endif
  __imp__sub_83442200(ctx, base);
}

namespace {

#if defined(__aarch64__)

inline simde__m128 PowSel(simde__m128 a, simde__m128 b, simde__m128 m) {
  return simde_mm_castsi128_ps(simde_mm_or_si128(simde_mm_andnot_si128(simde_mm_castps_si128(m), simde_mm_castps_si128(a)),
                                                 simde_mm_and_si128(simde_mm_castps_si128(m), simde_mm_castps_si128(b))));
}

inline simde__m128 PowExp2(simde__m128 x) {
  simde__m128 n = simde_mm_round_ps(x, SIMDE_MM_FROUND_TO_NEG_INF | SIMDE_MM_FROUND_NO_EXC);
  simde__m128 f = simde_mm_sub_ps(x, n);
  simde__m128 p = simde_mm_set1_ps(1.8775767e-3f);
  p = simde_mm_add_ps(simde_mm_mul_ps(p, f), simde_mm_set1_ps(8.9893397e-3f));
  p = simde_mm_add_ps(simde_mm_mul_ps(p, f), simde_mm_set1_ps(5.5826318e-2f));
  p = simde_mm_add_ps(simde_mm_mul_ps(p, f), simde_mm_set1_ps(2.4015361e-1f));
  p = simde_mm_add_ps(simde_mm_mul_ps(p, f), simde_mm_set1_ps(6.9315308e-1f));
  p = simde_mm_add_ps(simde_mm_mul_ps(p, f), simde_mm_set1_ps(1.0f));
  simde__m128i exp_bits = simde_mm_slli_epi32(simde_mm_add_epi32(simde_mm_cvttps_epi32(n), simde_mm_set1_epi32(127)), 23);
  simde__m128 r = simde_mm_mul_ps(p, simde_mm_castsi128_ps(exp_bits));
  r = simde_mm_andnot_ps(simde_mm_cmplt_ps(x, simde_mm_set1_ps(-126.0f)), r);
  r = simde_mm_blendv_ps(r, simde_mm_castsi128_ps(simde_mm_set1_epi32(0x7F800000)),
                         simde_mm_cmpge_ps(x, simde_mm_set1_ps(128.0f)));
  return simde_mm_blendv_ps(r, x, simde_mm_cmpunord_ps(x, x));
}

void Pow822F4E48(PPCContext& ctx, uint8_t* base) {
  const float fx = float(ctx.f1.f64), fy = float(ctx.f2.f64);
  uint32_t xb, yb;
  std::memcpy(&xb, &fx, 4);
  std::memcpy(&yb, &fy, 4);
  VmS32(base, ctx.r1.u32 + 28, yb);
  VmS32(base, ctx.r1.u32 + 20, xb);
  const simde__m128 x = simde_mm_castsi128_ps(simde_mm_set1_epi32(int(xb)));
  const simde__m128 y = simde_mm_castsi128_ps(simde_mm_set1_epi32(int(yb)));
  const simde__m128 zero = simde_mm_setzero_ps();
  const simde__m128 sign = simde_mm_castsi128_ps(simde_mm_set1_epi32(int(0x80000000u)));
  const simde__m128 x_zero = simde_mm_cmpeq_ps(x, zero);
  const simde__m128 ax = simde_mm_andnot_ps(sign, x);
  const simde__m128i yi = rex::ppc::simde_mm_vctsxs(y);
  const simde__m128 yt = simde_mm_round_ps(y, SIMDE_MM_FROUND_TO_ZERO | SIMDE_MM_FROUND_NO_EXC);
  const simde__m128 xs = simde_mm_and_ps(x, sign);
  const simde__m128 y_neg = simde_mm_cmpgt_ps(zero, y);
  const simde__m128 y_zero = simde_mm_cmpeq_ps(y, zero);
  const simde__m128 l = simde_mm_set1_ps(log2f(simde_mm_cvtss_f32(ax)));
  const simde__m128 x_neg = simde_mm_cmpgt_ps(zero, x);
  const simde__m128 qnan = simde_mm_castsi128_ps(simde_mm_set1_epi32(0x7FC00000));
  const simde__m128i odd = simde_mm_and_si128(yi, simde_mm_set1_epi32(1));
  const simde__m128 y_int = simde_mm_cmpeq_ps(y, yt);
  const simde__m128 zero_case = simde_mm_andnot_ps(y_neg, x_zero);
  const simde__m128 trivial = simde_mm_or_ps(x_zero, y_zero);
  const simde__m128 e = simde_mm_mul_ps(l, y);
  const simde__m128 odd_sign = simde_mm_and_ps(xs, simde_mm_castsi128_ps(simde_mm_slli_epi32(odd, 31)));
  const simde__m128 bad = simde_mm_andnot_ps(y_int, x_neg);
  const simde__m128 p = PowExp2(e);
  const simde__m128 special0 = PowSel(qnan, odd_sign, zero_case);
  const simde__m128 special = PowSel(special0, simde_mm_set1_ps(1.0f), y_zero);
  const simde__m128 r = PowSel(simde_mm_or_ps(p, odd_sign), special, simde_mm_or_ps(bad, trivial));
  ctx.f1.f64 = double(simde_mm_cvtss_f32(r));
}

bool PowSame(const PPCContext& a, const PPCContext& b) {
  const uint64_t x = a.f1.u64, y = b.f1.u64;
  const bool nan = (x & 0x7FF0000000000000ull) == 0x7FF0000000000000ull && (x & 0xFFFFFFFFFFFFFull) &&
                   (y & 0x7FF0000000000000ull) == 0x7FF0000000000000ull && (y & 0xFFFFFFFFFFFFFull);
  return a.r1.u64 == b.r1.u64 && (x == y || nan);
}

#endif

}  // namespace

REX_HOOK_RAW(sub_822F4E48) {
#if defined(__aarch64__)
  if (Enabled(REXCVAR_GET(gtav_nr_pow)) && !(ctx.r1.u32 & 15)) {
    if (CheckNow()) [[unlikely]] {
      Differential(g_pow, ctx, base, __imp__sub_822F4E48, {{ctx.r1.u32 + 20, 12}}, PowSame, Pow822F4E48);
      return;
    }
    Pow822F4E48(ctx, base);
    return;
  }
#endif
  __imp__sub_822F4E48(ctx, base);
}

namespace {

constexpr uint32_t kLzxStateSize = 12288;
constexpr uint32_t kLzxSlack = 512;
constexpr uint32_t kLzxWrittenState = 11964;
constexpr uint32_t kLzxWrittenTail = 288;

struct LzxBits {
  uint32_t buf;
  int8_t count;
  const uint8_t* in;

  void Drop(uint32_t n) {
    count = int8_t(count - int32_t(n));
    buf = n & 0x20 ? 0 : buf << (n & 0x1F);
    if (count > 0) return;
    const uint32_t word = uint32_t(in[0]) | uint32_t(in[1]) << 8;
    const uint32_t shift = uint32_t(-int32_t(count)) & 0xFF;
    in += 2;
    buf |= shift & 0x20 ? 0 : word << (shift & 0x1F);
    count = int8_t(count + 16);
  }
};

inline uint32_t LzxTop(uint32_t buf, uint32_t n) {
  const uint32_t shift = (32 - n) & 0xFF;
  return shift & 0x20 ? 0 : buf >> (shift & 0x1F);
}

inline int32_t LzxEntry(const uint8_t* s, uint32_t off) {
  uint16_t v;
  std::memcpy(&v, s + off, 2);
  return int16_t(__builtin_bswap16(v));
}

inline int32_t LzxDecode(const uint8_t* s, uint32_t buf, uint32_t table, uint32_t bits, uint32_t tree) {
  int32_t sym = LzxEntry(s, table + ((buf >> (32 - bits)) << 1));
  for (uint32_t m = 1u << (31 - bits); sym < 0; m >>= 1)
    sym = LzxEntry(s, tree + (uint32_t((buf & m ? 1 : 0) - 2 * sym) << 1));
  return sym;
}

inline void LzxStore32(uint8_t* s, uint32_t off, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(s + off, &v, 4);
}

template <uint32_t N>
inline void LzxMove(uint8_t* dst, const uint8_t* src) {
  uint8_t chunk[N];
  std::memcpy(chunk, src, N);
  std::memcpy(dst, chunk, N);
}

inline void LzxCopyBehind(uint8_t* dst, const uint8_t* src, uint32_t len) {
  if (len >= 16) {
    for (uint32_t i = 0; i + 16 <= len; i += 16) LzxMove<16>(dst + i, src + i);
    LzxMove<16>(dst + len - 16, src + len - 16);
  } else if (len >= 8) {
    LzxMove<8>(dst, src);
    LzxMove<8>(dst + len - 8, src + len - 8);
  } else if (len >= 4) {
    LzxMove<4>(dst, src);
    LzxMove<4>(dst + len - 4, src + len - 4);
  } else {
    LzxMove<2>(dst, src);
    dst[len - 1] = src[len - 1];
  }
}

inline void LzxCopy(uint8_t* dst, const uint8_t* src, uint32_t len, uint32_t dist, bool behind) {
  if (behind && dist >= 16) {
    LzxCopyBehind(dst, src, len);
    return;
  }
  if (dist >= len) {
    std::memmove(dst, src, len);
    return;
  }
  for (uint32_t i = 0; i < len; ++i) dst[i] = src[i];
}

bool LzxRunnable(const PPCContext& ctx, uint8_t* base) {
  const uint32_t st = ctx.r3.u32;
  if (!Flat(st, kLzxStateSize)) return false;
  const uint32_t win = Be32(Host(base, st), 0), mask = Be32(Host(base, st), 8);
  const uint32_t in = Be32(Host(base, st), 11012), in_end = Be32(Host(base, st), 11016);
  const uint64_t reach = std::max<uint64_t>(uint64_t(mask) + 1, uint64_t(ctx.r4.u32) + ctx.r5.u32) + kLzxSlack;
  return reach <= 0x10000000u && Flat(win, uint32_t(reach)) && in_end - in < 0x10000000u &&
         Flat(in, in_end - in + kLzxSlack);
}

void Lzx837A0D88(PPCContext& ctx, uint8_t* base) {
  const uint32_t st = ctx.r3.u32;
  uint8_t* s = Host(base, st);
  const uint32_t win = Be32(s, 0), mask = Be32(s, 8);
  uint8_t* w = Host(base, win);
  const uint32_t in0 = Be32(s, 11012), in_end = Be32(s, 11016);
  const uint8_t* in_base = Host(base, in0);
  LzxBits bits{Be32(s, 11952), int8_t(s[11956]), in_base};
  uint32_t r0 = Be32(s, 12), r1 = Be32(s, 16), r2 = Be32(s, 20);
  uint64_t pos = ctx.r4.u64;
  const uint64_t end = ctx.r4.u64 + ctx.r5.u64;
  bool failed = false;
  while (int32_t(uint32_t(pos)) < int32_t(uint32_t(end))) {
    const int32_t sym = LzxDecode(s, bits.buf, 24, 10, 3644);
    if (in0 + uint32_t(bits.in - in_base) >= in_end) {
      failed = true;
      break;
    }
    bits.Drop(s[2584 + uint32_t(sym)]);
    const int32_t code = sym - 256;
    if (code < 0) {
      w[uint32_t(pos)] = uint8_t(code);
      ++pos;
      continue;
    }
    uint32_t header = uint32_t(code) & 7;
    if (header == 7) {
      const int32_t extra = LzxDecode(s, bits.buf, 2072, 8, 9020);
      bits.Drop(s[3256 + uint32_t(extra)]);
      header = uint32_t(extra) + 7;
    }
    const int32_t slot = int8_t(code >> 3);
    uint32_t offset;
    if (slot > 2) {
      const uint32_t footer = s[12004 + uint32_t(slot)];
      const uint32_t slot_base = Be32(s, 12056 + 4 * uint32_t(slot));
      if (footer >= 3) {
        const uint32_t verbatim_bits = (footer - 3) & 0xFF;
        uint32_t verbatim = 0;
        if (verbatim_bits) {
          verbatim = LzxTop(bits.buf, verbatim_bits);
          bits.Drop(verbatim_bits);
        }
        const int32_t aligned = int8_t(s[3508 + (bits.buf >> 25)]);
        bits.Drop(s[3636 + uint32_t(aligned)]);
        offset = slot_base + (verbatim << 3) + uint32_t(aligned);
      } else if (footer) {
        const uint32_t verbatim = LzxTop(bits.buf, footer);
        bits.Drop(footer);
        offset = slot_base + verbatim;
      } else {
        offset = slot_base;
      }
      r2 = r1;
      r1 = r0;
    } else if (slot == 0) {
      offset = r0;
    } else if (slot == 1) {
      offset = r1;
      r1 = r0;
    } else {
      offset = r2;
      r2 = r0;
    }
    r0 = offset;
    const uint32_t len = header + 2;
    const uint32_t src = (uint32_t(pos) - offset) & mask;
    LzxCopy(w + uint32_t(pos), w + src, len, uint32_t(pos) - src,
            src < uint32_t(pos));
    pos += len;
  }
  LzxStore32(s, 12, r0);
  LzxStore32(s, 16, r1);
  LzxStore32(s, 20, r2);
  if (failed) {
    ctx.r3.s64 = -1;
    return;
  }
  s[11956] = uint8_t(bits.count);
  LzxStore32(s, 11952, bits.buf);
  LzxStore32(s, 11012, in0 + uint32_t(bits.in - in_base));
  LzxStore32(s, 11960, uint32_t(pos) & mask);
  ctx.r3.u64 = pos - end;
}

struct LzxTiming {
  std::atomic<uint64_t> calls{0}, bytes{0}, ns{0};
};

LzxTiming g_lzx_native, g_lzx_original;
std::mutex g_lzx_report_mutex;
Clock::time_point g_lzx_report_at = Clock::now();

void LzxReport() {
  std::unique_lock lock(g_lzx_report_mutex, std::try_to_lock);
  if (!lock.owns_lock()) return;
  const auto now = Clock::now();
  const uint64_t wall = Ns(g_lzx_report_at, now);
  if (wall < 10'000'000'000ull) return;
  g_lzx_report_at = now;
  for (auto [name, t] : {std::pair{"native", &g_lzx_native}, std::pair{"original", &g_lzx_original}}) {
    const uint64_t calls = t->calls.exchange(0, std::memory_order_relaxed);
    const uint64_t bytes = t->bytes.exchange(0, std::memory_order_relaxed);
    const uint64_t ns = t->ns.exchange(0, std::memory_order_relaxed);
    if (!calls) continue;
    REXLOG_INFO("[lzx] {}: {} calls, {} bytes, {:.0f} ns/call, {:.3f} ns/byte, {:.2f}% of one core", name, calls,
                bytes, double(ns) / calls, bytes ? double(ns) / bytes : 0.0, 100.0 * double(ns) / double(wall));
  }
}

thread_local uint32_t t_lzx_tick = 0;

inline bool LzxCheckNow() {
  const int32_t every = REXCVAR_GET(gtav_nr_lzx_check);
  return every > 0 && ++t_lzx_tick % uint32_t(every) == 0 &&
         Clock::now() - g_started >= std::chrono::seconds(REXCVAR_GET(gtav_nr_check_delay));
}

bool LzxRun(PPCContext& ctx, uint8_t* base) {
  if (Enabled(REXCVAR_GET(gtav_nr_lzx)) && LzxRunnable(ctx, base)) {
    if (LzxCheckNow()) [[unlikely]] {
      const uint32_t st = ctx.r3.u32, win = Be32(Host(base, st), 0);
      Differential(g_lzx, ctx, base, __imp__sub_837A0D88,
                   {{st, kLzxWrittenState}, {win + ctx.r4.u32, ctx.r5.u32 + kLzxWrittenTail}}, SameResult, Lzx837A0D88);
      return false;
    }
    Lzx837A0D88(ctx, base);
    return true;
  }
  __imp__sub_837A0D88(ctx, base);
  return true;
}

}  // namespace

REX_HOOK_RAW(sub_837A0D88) {
  if (!REXCVAR_GET(gtav_nr_lzx_stats)) {
    LzxRun(ctx, base);
    return;
  }
  const uint32_t bytes = ctx.r5.u32;
  const bool native = Enabled(REXCVAR_GET(gtav_nr_lzx));
  const auto t0 = Clock::now();
  if (!LzxRun(ctx, base)) return;
  LzxTiming& t = native ? g_lzx_native : g_lzx_original;
  t.ns.fetch_add(Ns(t0, Clock::now()), std::memory_order_relaxed);
  t.calls.fetch_add(1, std::memory_order_relaxed);
  t.bytes.fetch_add(bytes, std::memory_order_relaxed);
  LzxReport();
}
