#include <pthread.h>

#include <algorithm>

#include <rex/cvar.h>

#include "switch_compat.h"

REXCVAR_DEFINE_INT32(gtav_switch_present_interval, 0, "GTAV",
                     "Vblanks each presented frame stays on screen on Switch (0 = the window's default)");

extern "C" int __real_pthread_detach(pthread_t thread);
extern "C" void RexSwitchPerfIntervalSwap(unsigned vblanks);

extern "C" int __wrap_pthread_detach(pthread_t thread) {
  __real_pthread_detach(thread);
  return 0;
}

namespace gtav::switch_compat {

void ApplyPresentInterval() {
  RexSwitchPerfIntervalSwap(unsigned(std::max(0, REXCVAR_GET(gtav_switch_present_interval))));
}

}  // namespace gtav::switch_compat
