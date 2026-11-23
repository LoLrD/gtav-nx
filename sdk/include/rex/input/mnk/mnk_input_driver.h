/**
 * @file        rex/input/mnk/mnk_input_driver.h
 * @brief       Keyboard/mouse input driver - maps MnK to Xbox 360 controller.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <rex/input/input_driver.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string>
#include <utility>
#include <vector>

namespace rex::input::mnk {

void SetMouseLookActive(bool active);
bool IsMouseLookActive();
// The title reports whether the player drives: the keybind_veh_* layout replaces the on-foot one
// (GTA V on PC: W/S drive, mouse buttons aim and shoot from the vehicle). Any thread.
void SetVehicleContext(bool in_vehicle);

// What the player is doing, as the title sees it: picks the PC layout's key table (mnk_pc_layout).
enum class InputContext : int {
  kOnFoot, kCar, kBike, kBicycle, kAircraft, kBoat, kSub, kParachute, kFrontend
};
void SetInputContext(InputContext context);
InputContext GetInputContext();
// The same without the pause menu: what the player is doing in the world.
InputContext GetGameplayContext();
// Pause menu / frontend open (scripts stop while paused, so the title reports it separately).
void SetFrontendActive(bool active);

// What a key binding drives: a pad control, or (past kRSRight) an action the driver or the title
// performs. Order is part of the API (the title's saved bindings use the names, not the values).
enum class PadOutput : int {
  kA, kB, kX, kY, kLB, kRB, kL3, kR3, kBack, kStart, kDUp, kDDown, kDLeft, kDRight,
  kLT, kRT, kLSLeft, kLSRight, kLSUp, kLSDown, kRSUp, kRSDown, kRSLeft, kRSRight, kBothSticks,
  kWeaponNext, kWeaponPrevious,
  kWeaponUnarmed, kWeaponMelee, kWeaponShotgun, kWeaponHeavy, kWeaponSpecial, kWeaponPistol,
  kWeaponSmg, kWeaponRifle, kWeaponSniper,
  kSwitchMichael, kSwitchFranklin, kSwitchTrevor,
  kVehicleWeaponNext, kAircraftWeaponNext, kScrollUp, kScrollDown,
  kCount
};
// Bindings of one InputContext (as int), or of everything but the pause menu (table 9). Keys: the
// keybind syntax ("Shift", "LMB", "WheelUp", "Ctrl+X", alternatives comma-separated). Replaces the
// built-in PC defaults for that table. Any thread.
void SetContextBindings(int table, std::vector<std::pair<std::string, PadOutput>> binds);
// Mouse look per context: sensitivity factor (0 = unchanged) and inverted vertical axis.
void SetMouseOptions(InputContext context, float scale, bool invert);

// Input from a window the driver is not attached to (the title's own game window, e.g. a Win32
// window on Windows): virtual-key codes (mouse buttons are VK_LBUTTON/RBUTTON/MBUTTON), relative
// mouse motion in pixels, wheel in WHEEL_DELTA units (120 a detent), focus. Any thread.
void InjectKey(uint16_t virtual_key, bool down);
void InjectMouseMotion(float dx, float dy);
void InjectMouseWheel(int32_t delta);
void InjectFocus(bool focused);

// PC actions the console pad has no button for, carried out by the title: the next request, or 0.
// 1-9 = weapon category key, kWeaponNext / kWeaponPrevious = mouse wheel, kSwitch* = F5-F7.
constexpr int kWeaponNext = 100, kWeaponPrevious = 101;
int TakeWeaponRequest();

class MnkInputDriver final : public InputDriver,
                             public rex::ui::WindowInputListener,
                             public rex::ui::WindowListener {
 public:
  explicit MnkInputDriver(rex::ui::Window* window, size_t window_z_order);
  ~MnkInputDriver() override;

  X_STATUS Setup() override;

  void EnumerateDevices(std::vector<DeviceInfo>& out) override;
  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) override;
  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t flags,
                                 X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t flags,
                              X_INPUT_KEYSTROKE* out_keystroke) override;

  void OnWindowAvailable(rex::ui::Window* window) override;

  // WindowInputListener
  void OnKeyDown(rex::ui::KeyEvent& e) override;
  void OnKeyUp(rex::ui::KeyEvent& e) override;
  void OnMouseDown(rex::ui::MouseEvent& e) override;
  void OnMouseUp(rex::ui::MouseEvent& e) override;
  void OnMouseMove(rex::ui::MouseEvent& e) override;
  void OnMouseWheel(rex::ui::MouseEvent& e) override;

  // WindowListener
  void OnClosing(rex::ui::UIEvent& e) override;
  void OnLostFocus(rex::ui::UISetupEvent& e) override;
  void OnGotFocus(rex::ui::UISetupEvent& e) override;

 private:
  bool IsEnabled() const;
  void SetKeyState(uint16_t vk, bool down);
  void EnqueueKeystroke(uint16_t vk_pad, bool down);
  void MapPcLayout(uint16_t* buttons, uint8_t* lt, uint8_t* rt, int32_t* lx, int32_t* ly);

 public:
  // InjectKey & co. (see above).
  void Inject(uint16_t vk, bool down);
  void InjectMotion(float dx, float dy);
  void InjectWheel(int32_t delta);
  void InjectFocusState(bool focused);

 private:

  // Called from the guest thread. The rest of the capture path stays on the UI
  // thread, since every Window call in it reaches SDL.
  void QueueMouseCaptureUpdate(bool should_capture);
  void ApplyMouseCaptureFromUIThread();
  void ReleaseMouseCaptureFromUIThread(rex::ui::Window* window);
  void RecenterCursorFromUIThread(int32_t x, int32_t y);
  // Safe to call from any thread.
  void DetachFromWindow();

  // Only the UI thread writes it, so only guest thread access needs the lock.
  rex::ui::Window* attached_window_ = nullptr;

  std::mutex state_mutex_;
  bool key_down_[256] = {};
  // Preserve a press until the guest has sampled it at least once. UI key
  // down/up pairs may both arrive between two XInputGetState calls.
  bool key_pressed_[256] = {};
  // PC layout: mouse wheel (scroll units), short presses it turns into, edge state of 1-9 / F5-F7.
  int32_t wheel_accum_ = 0;
  int32_t rs_pulse_y_ = 0;
  std::chrono::steady_clock::time_point pulse_until_[32] = {};
  bool special_down_[64] = {};
  int32_t rs_pulse_x_ = 0;
  std::chrono::steady_clock::time_point switch_start_{};  // F5-F7 character switch in progress
  int switch_target_ = 0;
  int32_t switch_stick_x_ = 0, switch_stick_y_ = 0;

  // Mouse delta tracking. Fractional because relative motion arrives in
  // fractions of a pixel, and truncating each event drops slow movement.
  // Filled on the UI thread, drained on the guest thread, hence the lock.
  float mouse_dx_ = 0.0f;
  float mouse_dy_ = 0.0f;

  // UI thread only.
  int32_t prev_mouse_x_ = 0;
  int32_t prev_mouse_y_ = 0;
  bool mouse_captured_ = false;
  // Whether the window has the pointer locked and is reporting relative motion.
  bool relative_mouse_mode_ = false;
  // Cursor visibility to restore on capture release - the window owner may run
  // an auto-hide policy that capture must not permanently override.
  rex::ui::Window::CursorVisibility precapture_cursor_visibility_ =
      rex::ui::Window::CursorVisibility::kVisible;

  // Guest thread to UI thread. The queued flag coalesces the posts.
  std::atomic<bool> mouse_capture_requested_{false};
  std::atomic<bool> mouse_capture_update_queued_{false};

  std::atomic<bool> has_focus_{true};

  // Keystroke queue
  std::queue<X_INPUT_KEYSTROKE> keystroke_queue_;

  // Packet number incremented on state change
  uint32_t packet_number_ = 0;
};

}  // namespace rex::input::mnk
