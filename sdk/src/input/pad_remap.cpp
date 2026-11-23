/**
 * @file        input/pad_remap.cpp
 * @brief       Remapping of real controllers' buttons and triggers.
 */
#include <rex/input/pad_remap.h>

#include <algorithm>
#include <atomic>
#include <mutex>

namespace rex::input {
namespace {
std::mutex g_mutex;
std::array<uint8_t, kPadControlCount> g_map = {};
std::atomic<bool> g_active{false};

constexpr uint16_t kBits[] = {X_INPUT_GAMEPAD_A, X_INPUT_GAMEPAD_B, X_INPUT_GAMEPAD_X, X_INPUT_GAMEPAD_Y,
                              X_INPUT_GAMEPAD_LEFT_SHOULDER, X_INPUT_GAMEPAD_RIGHT_SHOULDER,
                              X_INPUT_GAMEPAD_LEFT_THUMB, X_INPUT_GAMEPAD_RIGHT_THUMB, X_INPUT_GAMEPAD_BACK,
                              X_INPUT_GAMEPAD_START, X_INPUT_GAMEPAD_DPAD_UP, X_INPUT_GAMEPAD_DPAD_DOWN,
                              X_INPUT_GAMEPAD_DPAD_LEFT, X_INPUT_GAMEPAD_DPAD_RIGHT};
}  // namespace

void SetPadRemap(const std::array<uint8_t, kPadControlCount>& map) {
  std::lock_guard lock(g_mutex);
  g_map = map;
  bool identity = true;
  for (int i = 0; i < kPadControlCount; ++i)
    if (map[i] != i) identity = false;
  g_active = !identity;
}

void ApplyPadRemap(X_INPUT_STATE& state) {
  if (!g_active.load(std::memory_order_relaxed)) return;
  std::array<uint8_t, kPadControlCount> map;
  {
    std::lock_guard lock(g_mutex);
    map = g_map;
  }
  X_INPUT_GAMEPAD& in = state.gamepad;
  uint8_t value[kPadControlCount] = {};
  for (int i = 0; i < kPadDRight + 1; ++i) value[i] = (in.buttons & kBits[i]) ? 0xFF : 0;
  value[kPadLT] = in.left_trigger;
  value[kPadRT] = in.right_trigger;
  uint16_t buttons = in.buttons;  // the guide button is not remappable
  for (uint16_t bit : kBits) buttons &= uint16_t(~bit);
  uint8_t lt = 0, rt = 0;
  for (int i = 0; i < kPadControlCount; ++i) {
    const uint8_t v = value[i];
    if (!v) continue;
    const uint8_t to = map[i] < kPadControlCount ? map[i] : uint8_t(i);
    if (to == kPadLT) lt = std::max(lt, v);
    else if (to == kPadRT) rt = std::max(rt, v);
    else if (v > 0x40) buttons |= kBits[to];  // a trigger as a button: past a third of its travel
  }
  in.buttons = buttons;
  in.left_trigger = lt;
  in.right_trigger = rt;
}

}  // namespace rex::input
