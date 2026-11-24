#include <pthread.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>

#if defined(__SWITCH__)
#include <reent.h>
#else
struct _reent;
#endif

#define REX_NO_PROFILE __attribute__((no_profile_instrument_function, no_instrument_function))

extern "C" {
void* __real__malloc_r(struct _reent* r, size_t n);
void __real__free_r(struct _reent* r, void* p);
void* __real__memalign_r(struct _reent* r, size_t align, size_t n);
void* __real__realloc_r(struct _reent* r, void* p, size_t n);
void __malloc_lock(struct _reent* r);
void __malloc_unlock(struct _reent* r);
void* __wrap__malloc_r(struct _reent* r, size_t n);
void __wrap__free_r(struct _reent* r, void* p);
void* __wrap__memalign_r(struct _reent* r, size_t align, size_t n);
void* __wrap__realloc_r(struct _reent* r, void* p, size_t n);
void RexSwitchMallocCacheStats(uint64_t out[6]);
}

namespace {

constexpr size_t kGranule = 16;
constexpr size_t kMinChunk = 32;
constexpr size_t kSmallMax = 1024;
constexpr unsigned kSmallClasses = kSmallMax / kGranule + 1;
constexpr unsigned kOctaves = 6;
constexpr unsigned kSteps = 8;
constexpr unsigned kClasses = kSmallClasses + kOctaves * kSteps;
constexpr size_t kLargeMax = kSmallMax << kOctaves;
constexpr unsigned kLists = kClasses * 2;
constexpr size_t kListBytes = 64 * 1024;
constexpr size_t kThreadBytes = 256 * 1024;
constexpr int64_t kDepotBytes = 8 * 1024 * 1024;
constexpr uint32_t kFlushEvery = 256;
constexpr size_t kMmapped = 2;
constexpr size_t kLargeBlock = 256 * 1024;

struct ThreadCache {
  void* head[kLists];
  uint16_t count[kLists];
  size_t bytes;
  uint32_t hits;
  uint32_t misses;
  int64_t bytes_delta;
};

struct Depot {
  std::mutex lock;
  std::atomic<void*> batches{nullptr};
};

thread_local ThreadCache* t_cache;
thread_local int t_bypass;
thread_local bool t_exited;

std::atomic<bool> g_ready{false};
pthread_key_t g_key;
Depot g_depot[kLists];
std::atomic<int64_t> g_depot_bytes{0};
std::atomic<int64_t> g_thread_bytes{0};
std::atomic<uint64_t> g_hits{0};
std::atomic<uint64_t> g_misses{0};
std::atomic<uint64_t> g_depot_moves{0};
std::atomic<unsigned> g_evict_cursor{0};
std::atomic<int64_t> g_large_bytes{0};
std::atomic<int64_t> g_large_peak{0};

REX_NO_PROFILE size_t ChunkFor(size_t n) {
  const size_t c = (n + sizeof(size_t) + kGranule - 1) & ~(kGranule - 1);
  return c < kMinChunk ? kMinChunk : c;
}

REX_NO_PROFILE size_t ClassSize(unsigned cls) {
  if (cls < kSmallClasses) return size_t(cls) * kGranule;
  const unsigned k = cls - kSmallClasses;
  const size_t base = kSmallMax << (k / kSteps);
  return base + (k % kSteps + 1) * (base / kSteps);
}

REX_NO_PROFILE bool CeilClass(size_t c, unsigned* cls) {
  if (c <= kSmallMax) {
    *cls = unsigned(c / kGranule);
    return true;
  }
  if (c > kLargeMax) return false;
  for (unsigned o = 0; o < kOctaves; ++o) {
    const size_t base = kSmallMax << o;
    if (c <= base * 2) {
      const size_t step = base / kSteps;
      *cls = kSmallClasses + o * kSteps + unsigned((c - base + step - 1) / step) - 1;
      return true;
    }
  }
  return false;
}

REX_NO_PROFILE bool ExactClass(size_t c, unsigned* cls) {
  return c >= kMinChunk && CeilClass(c, cls) && ClassSize(*cls) == c;
}

REX_NO_PROFILE unsigned ListMax(size_t c) {
  const size_t m = kListBytes / c;
  return m < 4 ? 4 : (m > 256 ? 256 : unsigned(m));
}

REX_NO_PROFILE size_t ChunkSize(const void* p) {
  return reinterpret_cast<const size_t*>(p)[-1] & ~size_t(7);
}

REX_NO_PROFILE void FlushStats(ThreadCache& t) {
  g_hits.fetch_add(t.hits, std::memory_order_relaxed);
  g_misses.fetch_add(t.misses, std::memory_order_relaxed);
  g_thread_bytes.fetch_add(t.bytes_delta, std::memory_order_relaxed);
  t.hits = 0;
  t.misses = 0;
  t.bytes_delta = 0;
}

REX_NO_PROFILE void FreeList(struct _reent* r, void* p) {
  if (!p) return;
  __malloc_lock(r);
  while (p) {
    void* next = *static_cast<void**>(p);
    __real__free_r(r, p);
    p = next;
  }
  __malloc_unlock(r);
}

REX_NO_PROFILE void* TakeBatch(unsigned list) {
  Depot& d = g_depot[list];
  if (!d.batches.load(std::memory_order_relaxed)) return nullptr;
  void* batch;
  {
    std::lock_guard<std::mutex> guard(d.lock);
    batch = d.batches.load(std::memory_order_relaxed);
    if (batch) d.batches.store(static_cast<void**>(batch)[1], std::memory_order_relaxed);
  }
  if (batch) {
    const auto n = static_cast<int64_t>(static_cast<uintptr_t*>(batch)[2]);
    g_depot_bytes.fetch_sub(n * int64_t(ClassSize(list / 2)), std::memory_order_relaxed);
    g_depot_moves.fetch_add(1, std::memory_order_relaxed);
  }
  return batch;
}

REX_NO_PROFILE void Evict(struct _reent* r, int64_t need, unsigned keep) {
  for (unsigned tries = 0;
       tries < kLists && g_depot_bytes.load(std::memory_order_relaxed) + need > kDepotBytes; ++tries) {
    const unsigned list = g_evict_cursor.fetch_add(1, std::memory_order_relaxed) % kLists;
    if (list == keep) continue;
    FreeList(r, TakeBatch(list));
  }
}

REX_NO_PROFILE void Spill(struct _reent* r, ThreadCache& t, unsigned list, size_t c, unsigned take) {
  void* first = t.head[list];
  void* last = first;
  for (unsigned i = 1; i < take; ++i) last = *static_cast<void**>(last);
  t.head[list] = *static_cast<void**>(last);
  *static_cast<void**>(last) = nullptr;
  t.count[list] = uint16_t(t.count[list] - take);
  t.bytes -= take * c;
  t.bytes_delta -= int64_t(take * c);
  const int64_t add = int64_t(take * c);
  if (g_depot_bytes.load(std::memory_order_relaxed) + add > kDepotBytes) Evict(r, add, list);
  if (g_depot_bytes.fetch_add(add, std::memory_order_relaxed) + add > kDepotBytes) {
    g_depot_bytes.fetch_sub(add, std::memory_order_relaxed);
    FreeList(r, first);
    return;
  }
  static_cast<uintptr_t*>(first)[2] = take;
  Depot& d = g_depot[list];
  {
    std::lock_guard<std::mutex> guard(d.lock);
    static_cast<void**>(first)[1] = d.batches.load(std::memory_order_relaxed);
    d.batches.store(first, std::memory_order_relaxed);
  }
  g_depot_moves.fetch_add(1, std::memory_order_relaxed);
}

REX_NO_PROFILE void* Pop(ThreadCache& t, unsigned list, size_t c) {
  void* p = t.head[list];
  if (!p) {
    p = TakeBatch(list);
    if (!p) return nullptr;
    const auto n = unsigned(static_cast<uintptr_t*>(p)[2]);
    t.head[list] = p;
    t.count[list] = uint16_t(n);
    t.bytes += n * c;
    t.bytes_delta += int64_t(n * c);
  }
  t.head[list] = *static_cast<void**>(p);
  t.count[list]--;
  t.bytes -= c;
  t.bytes_delta -= int64_t(c);
  return p;
}

REX_NO_PROFILE ThreadCache* Cache() {
  if (ThreadCache* t = t_cache) return t;
  if (t_exited) return nullptr;
  auto* t = static_cast<ThreadCache*>(__real__malloc_r(_REENT, sizeof(ThreadCache)));
  if (!t) return nullptr;
  std::memset(t, 0, sizeof(ThreadCache));
  t_cache = t;
  pthread_setspecific(g_key, t);
  return t;
}

REX_NO_PROFILE void ThreadExit(void* arg) {
  t_cache = nullptr;
  t_exited = true;
  auto* t = static_cast<ThreadCache*>(arg);
  for (unsigned list = 0; list < kLists; ++list) FreeList(_REENT, t->head[list]);
  t->bytes_delta -= int64_t(t->bytes);
  FlushStats(*t);
  __real__free_r(_REENT, t);
}

REX_NO_PROFILE void CountLarge(int64_t delta) {
  const int64_t now = g_large_bytes.fetch_add(delta, std::memory_order_relaxed) + delta;
  int64_t peak = g_large_peak.load(std::memory_order_relaxed);
  while (now > peak && !g_large_peak.compare_exchange_weak(peak, now, std::memory_order_relaxed)) {
  }
}

REX_NO_PROFILE int64_t LargeSize(const void* p) {
  if (!p) return 0;
  const size_t c = ChunkSize(p);
  return c >= kLargeBlock ? int64_t(c) : 0;
}

REX_NO_PROFILE void* Counted(void* p) {
  if (const int64_t c = LargeSize(p)) CountLarge(c);
  return p;
}

REX_NO_PROFILE void* RealMemalign(struct _reent* r, size_t align, size_t n) {
  ++t_bypass;
  void* p = __real__memalign_r(r, align, n);
  --t_bypass;
  return p;
}

REX_NO_PROFILE void* RealAllocate(struct _reent* r, size_t n, bool aligned32) {
  return Counted(aligned32 ? RealMemalign(r, 32, n) : __real__malloc_r(r, n));
}

REX_NO_PROFILE void* Allocate(struct _reent* r, size_t n, bool aligned32) {
  unsigned cls;
  if (!CeilClass(ChunkFor(n), &cls)) return RealAllocate(r, n, aligned32);
  ThreadCache* cache = Cache();
  if (!cache) return RealAllocate(r, n, aligned32);
  ThreadCache& t = *cache;
  const size_t c = ClassSize(cls);
  void* p = aligned32 ? Pop(t, cls * 2 + 1, c) : Pop(t, cls * 2, c);
  if (!p && !aligned32) p = Pop(t, cls * 2 + 1, c);
  if (p) {
    if (++t.hits >= kFlushEvery) FlushStats(t);
    return p;
  }
  if (++t.misses >= kFlushEvery) FlushStats(t);
  return RealAllocate(r, c - sizeof(size_t), aligned32);
}

REX_NO_PROFILE bool Release(struct _reent* r, void* p) {
  const size_t header = reinterpret_cast<const size_t*>(p)[-1];
  unsigned cls;
  if ((header & kMmapped) || !ExactClass(header & ~size_t(7), &cls)) return false;
  ThreadCache* cache = Cache();
  if (!cache) return false;
  ThreadCache& t = *cache;
  const size_t c = ClassSize(cls);
  const unsigned list = cls * 2 + ((reinterpret_cast<uintptr_t>(p) & 31) == 0);
  const unsigned max = ListMax(c);
  if (t.count[list] >= max) {
    Spill(r, t, list, c, max / 2);
  } else if (t.bytes + c > kThreadBytes && t.count[list] >= 2) {
    Spill(r, t, list, c, t.count[list] / 2);
  }
  if (t.bytes + c > kThreadBytes) return false;
  *static_cast<void**>(p) = t.head[list];
  t.head[list] = p;
  t.count[list]++;
  t.bytes += c;
  t.bytes_delta += int64_t(c);
  return true;
}

REX_NO_PROFILE bool LayoutMatches(struct _reent* r) {
  bool ok = true;
  for (size_t n : {size_t(1), size_t(24), size_t(100), size_t(1000), size_t(5000)}) {
    void* p = __real__malloc_r(r, n);
    void* q = __real__memalign_r(r, 32, n);
    ok = ok && p && q && ChunkSize(p) >= ChunkFor(n) && ChunkSize(p) < ChunkFor(n) + kMinChunk &&
         ChunkSize(q) >= ChunkFor(n) && ChunkSize(q) < ChunkFor(n) + kMinChunk &&
         (reinterpret_cast<uintptr_t>(q) & 31) == 0;
    __real__free_r(r, p);
    __real__free_r(r, q);
  }
  return ok;
}

REX_NO_PROFILE bool Active() { return g_ready.load(std::memory_order_relaxed) && !t_bypass; }

__attribute__((constructor(101))) REX_NO_PROFILE void Init() {
  if (!LayoutMatches(_REENT)) return;
  if (pthread_key_create(&g_key, ThreadExit) != 0) return;
  g_ready.store(true, std::memory_order_release);
}

}  // namespace

extern "C" {

REX_NO_PROFILE void* __wrap__malloc_r(struct _reent* r, size_t n) {
  return Active() ? Allocate(r, n, false) : __real__malloc_r(r, n);
}

REX_NO_PROFILE void* __wrap__memalign_r(struct _reent* r, size_t align, size_t n) {
  if (align <= kGranule) return __wrap__malloc_r(r, n);
  if (!Active()) return RealMemalign(r, align, n);
  if (align == 32) return Allocate(r, n, true);
  return Counted(RealMemalign(r, align, n));
}

REX_NO_PROFILE void __wrap__free_r(struct _reent* r, void* p) {
  if (!p) return;
  if (Active()) {
    if (const int64_t c = LargeSize(p)) {
      CountLarge(-c);
    } else if (Release(r, p)) {
      return;
    }
  }
  __real__free_r(r, p);
}

REX_NO_PROFILE void* __wrap__realloc_r(struct _reent* r, void* p, size_t n) {
  const bool active = Active();
  const int64_t before = active ? LargeSize(p) : 0;
  ++t_bypass;
  void* q = __real__realloc_r(r, p, n);
  --t_bypass;
  if (active && (q || n == 0)) {
    const int64_t after = LargeSize(q);
    if (after != before) CountLarge(after - before);
  }
  return q;
}

REX_NO_PROFILE void RexSwitchMallocCacheStats(uint64_t out[6]) {
  out[0] = g_hits.exchange(0, std::memory_order_relaxed);
  out[1] = g_misses.exchange(0, std::memory_order_relaxed);
  out[2] = g_depot_moves.exchange(0, std::memory_order_relaxed);
  const int64_t held = g_thread_bytes.load(std::memory_order_relaxed) + g_depot_bytes.load(std::memory_order_relaxed);
  out[3] = held > 0 ? uint64_t(held) : 0;
  const int64_t large = g_large_bytes.load(std::memory_order_relaxed);
  out[4] = large > 0 ? uint64_t(large) : 0;
  out[5] = uint64_t(g_large_peak.load(std::memory_order_relaxed));
}

}
