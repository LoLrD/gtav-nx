#include <pthread.h>

#include <rex/cvar.h>
#include <rex/ui/switch_sysclk.h>

#include "switch_compat.h"

REXCVAR_DEFINE_INT32(gtav_switch_cpu_mhz, 1785, "GTAV", "CPU clock requested from sys-clk while running (0 = untouched)");
REXCVAR_DEFINE_INT32(gtav_switch_gpu_mhz, 614, "GTAV", "GPU clock requested from sys-clk while running (0 = untouched)");
REXCVAR_DEFINE_INT32(gtav_switch_mem_mhz, 1600, "GTAV", "Memory clock requested from sys-clk while running (0 = untouched)");

extern "C" int __real_pthread_detach(pthread_t thread);

extern "C" int __wrap_pthread_detach(pthread_t thread) {
  __real_pthread_detach(thread);
  return 0;
}

namespace gtav::switch_compat {

void RaiseClocks() {
  rex::ui::switch_sysclk::Raise(REXCVAR_GET(gtav_switch_cpu_mhz), REXCVAR_GET(gtav_switch_gpu_mhz),
                                REXCVAR_GET(gtav_switch_mem_mhz));
}

void ReleaseClocks() { rex::ui::switch_sysclk::Release(); }

}  // namespace gtav::switch_compat
