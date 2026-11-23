#include <vulkan/vulkan.h>

PFN_vkGetInstanceProcAddr plume_switch_get_instance_proc_addr(void) {
    return vkGetInstanceProcAddr;
}
