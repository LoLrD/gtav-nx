/**
 * @file        input/pad_remap.h
 * @brief       Remapping of real controllers' buttons and triggers (title settings).
 */
#pragma once

#include <array>
#include <cstdint>

#include <rex/input/input.h>

namespace rex::input {

// The remappable controls of a pad, in this order.
enum PadControl : uint8_t {
  kPadA, kPadB, kPadX, kPadY, kPadLB, kPadRB, kPadL3, kPadR3, kPadBack, kPadStart,
  kPadDUp, kPadDDown, kPadDLeft, kPadDRight, kPadLT, kPadRT, kPadControlCount
};

// map[physical] = what the game sees; identity = no remap. Any thread.
void SetPadRemap(const std::array<uint8_t, kPadControlCount>& map);

// Applies the remap to one real pad's state (InputSystem, synthetic devices excluded).
void ApplyPadRemap(X_INPUT_STATE& state);

}  // namespace rex::input
