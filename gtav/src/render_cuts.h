#pragma once

#include <rex/ppc/context.h>

#include <cstdint>

namespace gtav {

void RenderTrace(char kind, PPCContext& ctx, uint8_t* base);

}  // namespace gtav
