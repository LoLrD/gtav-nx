#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/ppc.h>

#include <cstdint>
#include <mutex>

REXCVAR_DEFINE_BOOL(gtav_pool_list_lock, false, "GTAV",
                    "Serialize the reservation pair 83829CC0/83829CD8 of the pool free lists (pop 83570218, "
                    "push 835702C0): the generated stdcx. is a compare-and-swap and accepts a recycled list "
                    "head (ABA) the console's reservation would reject");

namespace {
std::recursive_mutex g_pool_list_mutex;
thread_local int32_t t_pool_list_locked = 0;
}  // namespace

REX_EXTERN(__imp__sub_83829CC0);
REX_HOOK_RAW(sub_83829CC0) {
  if (REXCVAR_GET(gtav_pool_list_lock)) {
    g_pool_list_mutex.lock();
    t_pool_list_locked++;
  }
  __imp__sub_83829CC0(ctx, base);
}

REX_EXTERN(__imp__sub_83829CD8);
REX_HOOK_RAW(sub_83829CD8) {
  __imp__sub_83829CD8(ctx, base);
  if (t_pool_list_locked) {
    t_pool_list_locked--;
    g_pool_list_mutex.unlock();
  }
}
