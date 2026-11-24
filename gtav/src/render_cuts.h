#pragma once

#include <rex/ppc/context.h>

#include <cstdint>

namespace gtav {

void RenderTrace(char kind, PPCContext& ctx, uint8_t* base);
bool SkipDraw(const uint8_t* base, uint32_t device);
bool LightShadowPass(const uint8_t* device);

}  // namespace gtav
