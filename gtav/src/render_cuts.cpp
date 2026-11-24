#include "render_cuts.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#if defined(__APPLE__) || (defined(__linux__) && !defined(__ANDROID__))
#include <execinfo.h>
#include <pthread.h>
#include <time.h>
#define GTAV_RENDER_TRACE_STACKS 1
#endif

#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "native/capture.h"
#include "native/d3d_map.h"
#include "native/frame.h"
#include "native/paths.h"
#include "native/shaders.h"
#include "native/xenos_regs.h"

REXCVAR_DEFINE_INT32(gtav_reflection_every, 1, "GTAV",
                     "Render the paraboloid reflection map (cars, glass, wet surfaces) every Nth frame (1 = game)");
REXCVAR_DEFINE_INT32(gtav_light_shadow_every, 1, "GTAV",
                     "Render a local light's shadow map at most every Nth frame, the lights in turn (1 = game, "
                     "0 = never)");
REXCVAR_DEFINE_BOOL(gtav_light_shadow_draws, true, "GTAV",
                    "Draw objects into the local light shadow maps (off: the maps are still cleared, lights unshadowed)");
REXCVAR_DEFINE_BOOL(gtav_reflection_draws, true, "GTAV",
                    "Draw objects into the paraboloid reflection map (off: the map is still cleared, no environment reflections)");
REXCVAR_DEFINE_BOOL(gtav_minimap, true, "GTAV", "Draw the minimap (off: no radar at all)");
REXCVAR_DEFINE_BOOL(gtav_scene_fog_meshes, true, "GTAV",
                    "Draw the fog-lit forward meshes of the scene pass (callback 8270E2A8, one shader, ~65 draws "
                    "in the prologue bank with no visible change when off)");
REXCVAR_DEFINE_INT32(gtav_render_trace, 0, "GTAV",
                     "Diagnostic: write every draw, clear, resolve and draw list of guest frames N..N+1 with their "
                     "host call chains to logs/render_trace.txt");
REXCVAR_DEFINE_BOOL(gtav_render_dl_times, false, "GTAV",
                    "Diagnostic: log the game render thread's time and draws per draw list type every 300 frames");
REXCVAR_DEFINE_BOOL(gtav_render_cuts_ab, false, "GTAV",
                    "Diagnostic, with --gtav_render_dl_times: the cuts above are on and off for alternate 300-frame "
                    "reports (an A/B under the same load)");
REXCVAR_DEFINE_STRING(gtav_render_skip, "", "GTAV",
                      "Diagnostic: draw lists the render thread skips, comma-separated TT[.I][/N] (type TT in hex, "
                      "only its Ith one of the frame, run every Nth frame)");

REX_EXTERN(__imp__sub_83811030);
REX_EXTERN(__imp__sub_835F7E68);
REX_EXTERN(__imp__sub_82740D58);
REX_EXTERN(__imp__sub_82297D20);
REX_EXTERN(__imp__sub_8270E2A8);

namespace gtav {
namespace {

constexpr uint32_t kDlFrameBegin = 0x1A;
constexpr uint32_t kDlFrameEnd = 0x1B;
constexpr uint32_t kDlParaboloidReflection = 0x0A;
constexpr uint32_t kDlShadows = 0x0C;
constexpr float kLightShadowMaxWidth = 256.0f;
constexpr uint32_t kTlsDrawList = 344;
constexpr uint32_t kParaboloidShadowIndex = 1072;

uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

std::string g_trace;
uint32_t g_trace_events = 0;
bool g_trace_done = false;
uint64_t g_draw_calls = 0;

struct SkipRule {
  uint32_t type;
  int32_t instance;
  uint32_t every;
};
std::vector<SkipRule> g_skip_rules;
bool g_skip_parsed = false;
uint64_t g_dl_frame = 0;
uint32_t g_dl_instances[256] = {};
double g_dl_ms[256] = {};
uint64_t g_dl_draws[256] = {};
uint32_t g_dl_report_frames = 0;
std::atomic<bool> g_cuts_paused{false};
uint32_t g_current_dl = 0xFFFFFFFF;

bool Cut() { return !g_cuts_paused.load(std::memory_order_relaxed); }

bool Tracing() {
  const int32_t at = REXCVAR_GET(gtav_render_trace);
  const uint64_t frame = native::GuestFrame();
  return at > 0 && !g_trace_done && frame >= uint64_t(at) && frame <= uint64_t(at) + 1;
}

void TraceBegin() {
  if (!g_trace.empty()) return;
  g_trace = fmt::format("anchor {:x} __imp__sub_83811030\n", uintptr_t(&__imp__sub_83811030));
#ifdef GTAV_RENDER_TRACE_STACKS
  char name[64] = {};
  pthread_getname_np(pthread_self(), name, sizeof(name));
  g_trace += fmt::format("thread {}\n", name);
#endif
}

void TraceDrawList(PPCContext& ctx, uint8_t* base, uint32_t dl) {
  TraceBegin();
  g_trace += fmt::format("L f{} dl {:08X} r4 {:08X} r5 {:08X} r6 {:08X} words", native::GuestFrame(), dl, ctx.r4.u32,
                         ctx.r5.u32, ctx.r6.u32);
  for (int i = 0; i < 12; i++) g_trace += fmt::format(" {:08X}", Be32(base + dl + 4 * i));
  g_trace += '\n';
  static bool table = false;
  if (table) return;
  table = true;
  g_trace += fmt::format("T mgr {:08X}", ctx.r3.u32);
  for (int i = 0; i < 256; i++) g_trace += fmt::format(" {:08X}", Be32(base + ctx.r3.u32 + 8 + 4 * i));
  g_trace += '\n';
}

void ParseSkipRules() {
  g_skip_parsed = true;
  std::stringstream in(REXCVAR_GET(gtav_render_skip));
  std::string item;
  while (std::getline(in, item, ',')) {
    if (item.size() < 2) continue;
    SkipRule r{uint32_t(std::stoul(item.substr(0, 2), nullptr, 16)), -1, 0};
    if (size_t dot = item.find('.'); dot != std::string::npos) r.instance = std::stoi(item.substr(dot + 1));
    if (size_t slash = item.find('/'); slash != std::string::npos) r.every = std::stoul(item.substr(slash + 1));
    g_skip_rules.push_back(r);
    REXLOG_INFO("[render-cuts] skipping draw list {:02X}, instance {}, run every {}", r.type, r.instance, r.every);
  }
}

bool SkipDrawList(uint32_t type) {
  if (!g_skip_parsed) ParseSkipRules();
  if (type == kDlFrameBegin) {
    g_dl_frame++;
    std::memset(g_dl_instances, 0, sizeof(g_dl_instances));
  }
  const uint32_t instance = g_dl_instances[type]++;
  if (type == kDlParaboloidReflection) {
    const int32_t every = REXCVAR_GET(gtav_reflection_every);
    if (every > 1 && Cut() && g_dl_frame % uint32_t(every)) return true;
  }
  for (const auto& r : g_skip_rules)
    if (r.type == type && (r.instance < 0 || uint32_t(r.instance) == instance) && (!r.every || g_dl_frame % r.every))
      return true;
  return false;
}

double ThreadCpuMs() {
#ifdef GTAV_RENDER_TRACE_STACKS
  timespec t{};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
  return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
#else
  return 0;
#endif
}

void ReportDrawListTimes() {
  static double cpu_at = ThreadCpuMs();
  const double cpu = ThreadCpuMs();
  std::string line;
  double total = 0;
  for (int t = 0; t < 256; t++) {
    if (g_dl_ms[t] == 0) continue;
    total += g_dl_ms[t];
    line += fmt::format(" {:02X}:{:.2f}ms/{:.0f}", t, g_dl_ms[t] / 300, g_dl_draws[t] / 300.0);
  }
  REXLOG_INFO("[render-cuts] render thread per frame{}: CPU {:.2f} ms, by draw list (ms/draws) {:.2f} ms:{}",
              REXCVAR_GET(gtav_render_cuts_ab) ? (Cut() ? " (cuts on)" : " (cuts off)") : "", (cpu - cpu_at) / 300,
              total / 300, line);
  if (REXCVAR_GET(gtav_render_cuts_ab)) g_cuts_paused.store(Cut(), std::memory_order_relaxed);
  cpu_at = cpu;
  std::memset(g_dl_ms, 0, sizeof(g_dl_ms));
  std::memset(g_dl_draws, 0, sizeof(g_dl_draws));
  g_dl_report_frames = 0;
}

}  // namespace

bool SkipDraw(const uint8_t* base, uint32_t device) {
  if (!Cut()) return false;
  if (g_current_dl == kDlShadows && !REXCVAR_GET(gtav_light_shadow_draws))
    return std::bit_cast<float>(Be32(base + device + native::d3d::kDevViewport + 8)) <= kLightShadowMaxWidth;
  return g_current_dl == kDlParaboloidReflection && !REXCVAR_GET(gtav_reflection_draws);
}

bool LightShadowPass(const uint8_t* device) {
  return Cut() && g_current_dl == kDlShadows && !REXCVAR_GET(gtav_light_shadow_draws) &&
         std::bit_cast<float>(Be32(device + native::d3d::kDevViewport + 8)) <= kLightShadowMaxWidth;
}

void RenderTrace(char kind, PPCContext& ctx, uint8_t* base) {
  if (kind != 'C' && kind != 'R') g_draw_calls++;
  const int32_t at = REXCVAR_GET(gtav_render_trace);
  if (at <= 0 || g_trace_done) return;
  const uint64_t frame = native::GuestFrame();
  if (frame < uint64_t(at)) return;
  if (frame > uint64_t(at) + 1) {
    g_trace_done = true;
    std::ofstream(native::LogPath("render_trace.txt")) << g_trace;
    REXLOG_INFO("[render-cuts] trace: wrote {} events", g_trace_events);
    g_trace.clear();
    return;
  }
  TraceBegin();
  const uint8_t* dev = base + ctx.r3.u32;
  const uint8_t* shadow = dev + native::kRegShadowBegin;
  const uint8_t* vp = dev + native::d3d::kDevViewport;
  g_trace += fmt::format(
      "{} f{} {} rt {:08X} {:08X} ds {:08X} si {:08X} ci {:08X} di {:08X} vp {}x{} vs {:016X} ps {:016X} n {} |",
      kind, frame, g_trace_events++, Be32(dev + native::d3d::kDevRenderTargets),
      Be32(dev + native::d3d::kDevRenderTargets + 4), Be32(dev + native::d3d::kDevDepthStencil),
      native::xenos_regs::Read(shadow, native::xenos_regs::RB_SURFACE_INFO),
      native::xenos_regs::Read(shadow, native::xenos_regs::RB_COLOR_INFO),
      native::xenos_regs::Read(shadow, native::xenos_regs::RB_DEPTH_INFO), Be32(vp + 8), Be32(vp + 12),
      native::ShaderHash(Be32(dev + native::d3d::kDevVertexShader)),
      native::ShaderHash(Be32(dev + native::d3d::kDevPixelShader)), ctx.r7.u32);
  if (kind == 'R') {
    const uint8_t* tex = base + ctx.r6.u32;
    g_trace += fmt::format(" dest {:08X} w {:08X} {:08X} {:08X} pt {:08X} |", ctx.r6.u32,
                           ctx.r6.u32 ? Be32(tex + 32) : 0, ctx.r6.u32 ? Be32(tex + 36) : 0,
                           ctx.r6.u32 ? Be32(tex + 40) : 0, ctx.r7.u32);
  } else {
    g_trace += fmt::format(" rt23 {:08X} {:08X} |", Be32(dev + native::d3d::kDevRenderTargets + 8),
                           Be32(dev + native::d3d::kDevRenderTargets + 12));
  }
#ifdef GTAV_RENDER_TRACE_STACKS
  void* frames[48];
  const int n = backtrace(frames, 48);
  for (int i = 1; i < n; i++) g_trace += fmt::format(" {:x}", uintptr_t(frames[i]));
#endif
  g_trace += '\n';
}

}  // namespace gtav

REX_HOOK_RAW(sub_835F7E68) {
  const uint32_t dl = gtav::Be32(base + gtav::Be32(base + ctx.r13.u32) + gtav::kTlsDrawList);
  const uint32_t type = gtav::Be32(base + dl + 8) & 0xFF;
  if (gtav::Tracing()) gtav::TraceDrawList(ctx, base, dl);
  if (gtav::SkipDrawList(type)) {
    ctx.r3.u64 = 1;
    return;
  }
  gtav::g_current_dl = type;
  if (!REXCVAR_GET(gtav_render_dl_times)) {
    __imp__sub_835F7E68(ctx, base);
    gtav::g_current_dl = 0xFFFFFFFF;
    return;
  }
  const auto start = std::chrono::steady_clock::now();
  const uint64_t draws = gtav::g_draw_calls;
  __imp__sub_835F7E68(ctx, base);
  gtav::g_current_dl = 0xFFFFFFFF;
  gtav::g_dl_ms[type] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  gtav::g_dl_draws[type] += gtav::g_draw_calls - draws;
  if (type == gtav::kDlFrameEnd && ++gtav::g_dl_report_frames == 300) gtav::ReportDrawListTimes();
}

REX_HOOK_RAW(sub_82740D58) {
  const int32_t every = REXCVAR_GET(gtav_light_shadow_every);
  if (every != 1 && gtav::Cut()) {
    const uint32_t light = gtav::Be32(base + ctx.r3.u32 + gtav::kParaboloidShadowIndex);
    if (every <= 0 || (gtav::native::GuestFrame() + light) % uint32_t(every)) return;
  }
  __imp__sub_82740D58(ctx, base);
}

REX_HOOK_RAW(sub_82297D20) {
  if (!REXCVAR_GET(gtav_minimap) && gtav::Cut()) return;
  __imp__sub_82297D20(ctx, base);
}

REX_HOOK_RAW(sub_8270E2A8) {
  if (!REXCVAR_GET(gtav_scene_fog_meshes) && gtav::Cut()) return;
  __imp__sub_8270E2A8(ctx, base);
}
