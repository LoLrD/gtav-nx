#if defined(__SWITCH__)

#include <dirent.h>
#include <malloc.h>

#include <algorithm>
#include <cinttypes>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#include <switch.h>

extern "C" {
extern char _start[];
extern char rex_code_blocks_swap_end[];
extern char rex_code_blocks_pad_end[];
extern char rex_code_blocks_hot_end[];
extern char** __system_argv;
extern int __system_argc;
u64 RexCodeBlocksSwap(void* ops, u64 count, u64 process);
const char* RexSwitchCodeBlocksReport(void);
}

asm(R"(
  .pushsection .text.rex_code_blocks_head,"ax",%progbits
  .balign 16
  .global RexCodeBlocksSwap
  .type RexCodeBlocksSwap, %function
RexCodeBlocksSwap:
  stp x19, x20, [sp, #-32]!
  stp x21, x30, [sp, #16]
  mov x19, x0
  mov x20, x1
  mov x21, x2
1:
  cbz x20, 9f
  mov x0, x21
  ldr x1, [x19, #0]
  ldr x2, [x19, #8]
  ldr x3, [x19, #24]
  svc #0x78
  cbnz w0, 8f
  mov x0, x21
  ldr x1, [x19, #0]
  ldr x2, [x19, #16]
  ldr x3, [x19, #24]
  svc #0x77
  cbnz w0, 6f
  mov x0, x21
  ldr x1, [x19, #0]
  ldr x2, [x19, #24]
  ldr x3, [x19, #32]
  svc #0x73
  cbnz w0, 5f
  str xzr, [x19, #40]
  add x19, x19, #48
  sub x20, x20, #1
  b 1b
5:
  str x0, [x19, #40]
  mov x0, x21
  ldr x1, [x19, #0]
  ldr x2, [x19, #16]
  ldr x3, [x19, #24]
  svc #0x78
  cbnz w0, 7f
  b 4f
6:
  str x0, [x19, #40]
4:
  mov x0, x21
  ldr x1, [x19, #0]
  ldr x2, [x19, #8]
  ldr x3, [x19, #24]
  svc #0x77
  cbnz w0, 7f
  mov x0, x21
  ldr x1, [x19, #0]
  ldr x2, [x19, #24]
  ldr x3, [x19, #32]
  svc #0x73
  cbnz w0, 7f
  b 9f
7:
  mov x0, #0
  mov x1, #0
  mov x2, #0
  svc #0x26
  b 7b
8:
  str x0, [x19, #40]
9:
  dsb ish
  isb
  mov x0, x20
  ldp x21, x30, [sp, #16]
  ldp x19, x20, [sp], #32
  ret
  .size RexCodeBlocksSwap, . - RexCodeBlocksSwap
  .global rex_code_blocks_swap_end
rex_code_blocks_swap_end:
  .space 0x200000
  .global rex_code_blocks_pad_end
rex_code_blocks_pad_end:
  .popsection
  .pushsection .text.rex_code_blocks_hot_end,"ax",%progbits
  .global rex_code_blocks_hot_end
rex_code_blocks_hot_end:
  ret
  .popsection
)");

namespace {

constexpr u64 kBlock = 0x200000;
constexpr u64 kPage = 0x1000;
constexpr u32 kRetInsn = 0xd65f03c0;
constexpr size_t kMaxRetPages = 2048;

enum class Mode { kOff, kHot, kAll };

struct SwapOp {
  u64 dst;
  u64 old_src;
  u64 new_src;
  u64 size;
  u64 perm;
  u64 result;
};
static_assert(sizeof(SwapOp) == 48);

struct Range {
  u64 lo;
  u64 hi;
};

char g_report[1024] = "code blocks: not run";
size_t g_report_len = 0;

u64 AlignUp(u64 v, u64 a) { return (v + a - 1) & ~(a - 1); }
u64 AlignDown(u64 v, u64 a) { return v & ~(a - 1); }

void Say(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Say(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  const int n = std::vsnprintf(g_report + g_report_len, sizeof(g_report) - g_report_len, fmt, ap);
  va_end(ap);
  if (n > 0) {
    g_report_len = std::min(sizeof(g_report) - 1, g_report_len + static_cast<size_t>(n));
  }
}

Mode ParseValue(const char* v) {
  while (*v == ' ' || *v == '\t' || *v == '"' || *v == '\'') ++v;
  if (!std::strncmp(v, "off", 3) || !std::strncmp(v, "false", 5) || *v == '0') return Mode::kOff;
  if (!std::strncmp(v, "all", 3) || !std::strncmp(v, "true", 4)) return Mode::kAll;
  return Mode::kHot;
}

bool FindKey(const char* path, Mode* mode) {
  FILE* f = std::fopen(path, "r");
  if (!f) return false;
  char line[256];
  bool found = false;
  while (!found && std::fgets(line, sizeof(line), f)) {
    const char* p = line;
    while (*p == ' ' || *p == '\t') ++p;
    constexpr char kKey[] = "switch_code_blocks";
    if (std::strncmp(p, kKey, sizeof(kKey) - 1)) continue;
    p += sizeof(kKey) - 1;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p != '=') continue;
    *mode = ParseValue(p + 1);
    found = true;
  }
  std::fclose(f);
  return found;
}

Mode ReadMode() {
  Mode mode = Mode::kHot;
  if (__system_argc < 1 || !__system_argv || !__system_argv[0]) return mode;
  char dir[512];
  std::snprintf(dir, sizeof(dir), "%s", __system_argv[0]);
  char* slash = std::strrchr(dir, '/');
  if (!slash) return mode;
  *slash = '\0';
  DIR* d = opendir(dir);
  if (!d) return mode;
  while (dirent* e = readdir(d)) {
    const size_t len = std::strlen(e->d_name);
    if (len < 6 || std::strcmp(e->d_name + len - 5, ".toml")) continue;
    char path[768];
    std::snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
    if (FindKey(path, &mode)) break;
  }
  closedir(d);
  return mode;
}

bool Query(u64 addr, MemoryInfo* mi) {
  u32 page_info = 0;
  return R_SUCCEEDED(svcQueryMemory(mi, &page_info, addr));
}

bool IsAliasCode(u64 addr, u64 size, u32 perm) {
  MemoryInfo mi{};
  return Query(addr, &mi) && (mi.type & 0xFF) == MemType_ModuleCodeStatic && mi.perm == perm &&
         mi.addr <= addr && addr + size <= mi.addr + mi.size;
}

bool IsLoaderCopy(u64 addr, u64 size) {
  MemoryInfo mi{};
  return Query(addr, &mi) && (mi.type & 0xFF) == MemType_Heap && (mi.attr & MemAttr_IsBorrowed) &&
         mi.perm == Perm_None && mi.addr <= addr && addr + size <= mi.addr + mi.size;
}

void CleanForExecution(u64 addr, u64 size) {
  u64 ctr = 0;
  asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
  const u64 dline = 4ull << ((ctr >> 16) & 0xF);
  const u64 iline = 4ull << (ctr & 0xF);
  for (u64 p = AlignDown(addr, dline); p < addr + size; p += dline) {
    asm volatile("dc cvau, %0" ::"r"(p) : "memory");
  }
  asm volatile("dsb ish" ::: "memory");
  for (u64 p = AlignDown(addr, iline); p < addr + size; p += iline) {
    asm volatile("ic ivau, %0" ::"r"(p) : "memory");
  }
  asm volatile("dsb ish\n\tisb" ::: "memory");
}

size_t CollectRets(Range r, u64* out) {
  size_t n = 0;
  for (u64 page = r.lo; page < r.hi && n < kMaxRetPages; page += kPage) {
    const u32* w = reinterpret_cast<const u32*>(page);
    for (size_t i = 0; i < kPage / 4; ++i) {
      if (w[i] == kRetInsn) {
        out[n++] = page + i * 4;
        break;
      }
    }
  }
  u64 seed = 0x9E3779B97F4A7C15ull;
  for (size_t i = n; i > 1; --i) {
    seed = seed * 6364136223846793005ull + 1442695040888963407ull;
    std::swap(out[i - 1], out[(seed >> 33) % i]);
  }
  return n;
}

double TimeRets(const u64* rets, size_t n) {
  using Fn = void (*)();
  for (size_t i = 0; i < n; ++i) reinterpret_cast<Fn>(rets[i])();
  constexpr int kRounds = 8;
  const u64 t0 = armGetSystemTick();
  for (int r = 0; r < kRounds; ++r) {
    for (size_t i = 0; i < n; ++i) reinterpret_cast<Fn>(rets[i])();
  }
  const u64 t1 = armGetSystemTick();
  return static_cast<double>(armTicksToNs(t1 - t0)) / static_cast<double>(kRounds * n);
}

void AddOp(SwapOp* ops, size_t* n, Range r, u64 base, u64 src_base, u32 perm) {
  if (r.hi <= r.lo) return;
  ops[*n] = SwapOp{r.lo, src_base + (r.lo - base), 0, r.hi - r.lo, perm, 0};
  ++*n;
}

bool Allocate(SwapOp* ops, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    void* copy = memalign(kBlock, ops[i].size);
    if (!copy) {
      Say(" | memalign of %" PRIu64 " MB failed", ops[i].size >> 20);
      for (size_t j = 0; j < i; ++j) std::free(reinterpret_cast<void*>(ops[j].new_src));
      return false;
    }
    std::memcpy(copy, reinterpret_cast<const void*>(ops[i].dst), ops[i].size);
    CleanForExecution(reinterpret_cast<u64>(copy), ops[i].size);
    ops[i].new_src = reinterpret_cast<u64>(copy);
  }
  return true;
}

void Run() {
  const Mode mode = ReadMode();
  if (mode == Mode::kOff) {
    Say("code blocks: off");
    return;
  }
  Say("code blocks (%s):", mode == Mode::kAll ? "all" : "hot");
  const u64 base = reinterpret_cast<u64>(_start);
  const auto* hdr = reinterpret_cast<const NroHeader*>(base + sizeof(NroStart));
  if (hdr->magic != NROHEADER_MAGIC) {
    Say(" no NRO header at 0x%" PRIx64, base);
    return;
  }
  if (!envHasHeapOverride()) {
    Say(" not loaded by hbloader");
    return;
  }
  const u64 text_end = base + hdr->segments[0].file_off + hdr->segments[0].size;
  const u64 ro_start = base + hdr->segments[1].file_off;
  const u64 ro_end = ro_start + hdr->segments[1].size;
  const u64 nro_size =
      hdr->segments[2].file_off + AlignUp(u64(hdr->segments[2].size) + hdr->bss_size, kPage);
  const u64 src_base = reinterpret_cast<u64>(envGetHeapOverrideAddr()) - nro_size;
  u64 heap_region = 0;
  svcGetInfo(&heap_region, InfoType_HeapRegionAddress, CUR_PROCESS_HANDLE, 0);
  Say(" image 0x%" PRIx64 " loader copy 0x%" PRIx64 " heap region 0x%" PRIx64, base, src_base,
      heap_region);
  if (hdr->segments[0].file_off != 0 || src_base != heap_region ||
      !IsAliasCode(base, text_end - base, Perm_Rx) ||
      !IsAliasCode(ro_start, ro_end - ro_start, Perm_R) || !IsLoaderCopy(src_base, ro_end - base)) {
    Say(" | layout not as expected, left alone");
    return;
  }
  const u64 swap_end = reinterpret_cast<u64>(rex_code_blocks_swap_end);
  const u64 pad_end = reinterpret_cast<u64>(rex_code_blocks_pad_end);
  const u64 hot_end = reinterpret_cast<u64>(rex_code_blocks_hot_end);
  Range text{AlignUp(swap_end, kBlock), AlignDown(text_end, kBlock)};
  Range ro{AlignUp(ro_start, kBlock), AlignDown(ro_end, kBlock)};
  Say(" | head +0x%" PRIx64 " hot +0x%" PRIx64 "..+0x%" PRIx64, swap_end - base, pad_end - base,
      hot_end - base);
  if (mode == Mode::kHot) {
    if (hot_end <= pad_end) {
      Say(" | no hot section after the pad (ordering file missing?), left alone");
      return;
    }
    text.lo = std::max(text.lo, AlignDown(pad_end, kBlock));
    text.hi = std::min(text.hi, AlignUp(hot_end, kBlock));
    ro.hi = ro.lo;
  }
  SwapOp ops[2]{};
  size_t n = 0;
  AddOp(ops, &n, text, base, src_base, Perm_Rx);
  AddOp(ops, &n, ro, base, src_base, Perm_R);
  if (!n) {
    Say(" | no whole 2 MB block to move");
    return;
  }
  static u64 rets[kMaxRetPages];
  const size_t ret_count = text.hi > text.lo ? CollectRets(text, rets) : 0;
  const double before_ns = ret_count ? TimeRets(rets, ret_count) : 0.0;
  if (!Allocate(ops, n)) return;
  const Handle own = envGetOwnProcessHandle();
  const u64 t0 = armGetSystemTick();
  const u64 left = RexCodeBlocksSwap(ops, n, own != INVALID_HANDLE ? own : CUR_PROCESS_HANDLE);
  const u64 t1 = armGetSystemTick();
  const size_t done = n - left;
  for (size_t i = 0; i < n; ++i) {
    Say(" | %s 0x%" PRIx64 " %" PRIu64 " MB %s", ops[i].perm == Perm_Rx ? "text" : "rodata",
        ops[i].dst - base, ops[i].size >> 20, i < done ? "moved" : "kept");
    if (i >= done) {
      if (ops[i].result) Say(" (0x%" PRIx64 ")", ops[i].result);
      std::free(reinterpret_cast<void*>(ops[i].new_src));
    }
  }
  Say(" | swap %" PRIu64 " us", armTicksToNs(t1 - t0) / 1000);
  if (ret_count && done && ops[0].perm == Perm_Rx) {
    Say(" | ret test %zu pages: %.1f ns before, %.1f ns after", ret_count, before_ns,
        TimeRets(rets, ret_count));
  }
}

__attribute__((constructor(101))) void RexSwitchCodeBlocks() { Run(); }

}  // namespace

extern "C" const char* RexSwitchCodeBlocksReport(void) { return g_report; }

#endif
