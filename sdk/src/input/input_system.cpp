/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <chrono>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <cmath>
#include <vector>

#include <rex/dbg.h>
#include <rex/input/device_assignment.h>
#include <rex/input/flags.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/input/pad_remap.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/input/nop/nop_input_driver.h>
#if REX_PLATFORM_SWITCH
#include <rex/input/switch/switch_input_driver.h>
#else
#include <rex/input/sdl/sdl_input_driver.h>
#endif
#include <rex/input/state_merge.h>
#include <rex/input/xinput/xinput_input_driver.h>
#include <rex/logging.h>

#if REX_PLATFORM_SWITCH
REXCVAR_DEFINE_STRING(input_backend, "switch", "Input", "Input backend: switch")
    .allowed({"switch"});
#else
REXCVAR_DEFINE_STRING(input_backend, "sdl", "Input", "Input backend: sdl, xinput")
    .allowed({"sdl", "xinput"});
#endif

REXCVAR_DEFINE_BOOL(guide_button, false, "Input", "Enable guide button pass-through");
REXCVAR_DEFINE_BOOL(input_rumble, false, "Input",
                    "Controller vibration (off by default: controllers only get "
                    "zero vibration). true = the game's vibration");
REXCVAR_DEFINE_STRING(input_script, "", "Input",
                      "Scripted pad for unattended runs: once this file exists, user 0 plays its lines "
                      "'start_s dur_s buttons_hex lt rt lx ly rx ry' in a loop instead of the real pads");
namespace rex::input {

namespace {

// --input_script: lets an unattended (hidden) run play the game, e.g. to reproduce audio glitches
// that only happen during gameplay. The clock starts when the file first appears, so a capture
// script can write it once the world is loaded.
class InputScript {
 public:
  static InputScript& Get() {
    static InputScript script;
    return script;
  }
  bool State(X_INPUT_STATE* out) {
    const std::string& path = REXCVAR_GET(input_script);
    if (path.empty()) return false;
    const auto now = std::chrono::steady_clock::now();
    // Checked once a second: a rewritten file restarts the script from its new steps (so a
    // capture script can drive a run step by step), a deleted one hands input back to the pads.
    if (now >= next_check_) {
      next_check_ = now + std::chrono::seconds(1);
      std::error_code ec;
      const auto stamp = std::filesystem::last_write_time(path, ec);
      if (ec) {
        steps_.clear();
        loaded_stamp_ = {};
      } else if (stamp != loaded_stamp_) {
        loaded_stamp_ = stamp;
        steps_.clear();
        length_ = 0;
        Load(path);
        start_ = now;
        REXLOG_INFO("input script: {} steps, loop {:.1f} s", steps_.size(), length_);
      }
    }
    if (steps_.empty()) return false;
    const double t = std::fmod(std::chrono::duration<double>(now - start_).count(), length_);
    X_INPUT_GAMEPAD pad = {};
    for (const Step& s : steps_) {
      if (t < s.start || t >= s.start + s.duration) continue;
      pad.buttons = uint16_t(pad.buttons | s.buttons);
      pad.left_trigger = std::max<uint8_t>(pad.left_trigger, s.lt);
      pad.right_trigger = std::max<uint8_t>(pad.right_trigger, s.rt);
      if (s.lx || s.ly) pad.thumb_lx = s.lx, pad.thumb_ly = s.ly;
      if (s.rx || s.ry) pad.thumb_rx = s.rx, pad.thumb_ry = s.ry;
    }
    out->packet_number = ++packet_;
    out->gamepad = pad;
    return true;
  }

 private:
  struct Step {
    double start, duration;
    uint16_t buttons;
    uint8_t lt, rt;
    int16_t lx, ly, rx, ry;
  };
  void Load(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return;
    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
      double start, duration;
      unsigned buttons, lt, rt;
      int lx, ly, rx, ry;
      if (std::sscanf(line, "%lf %lf %x %u %u %d %d %d %d", &start, &duration, &buttons, &lt, &rt, &lx,
                      &ly, &rx, &ry) != 9)
        continue;
      steps_.push_back({start, duration, uint16_t(buttons), uint8_t(std::min(lt, 255u)),
                        uint8_t(std::min(rt, 255u)), int16_t(std::clamp(lx, -32768, 32767)),
                        int16_t(std::clamp(ly, -32768, 32767)), int16_t(std::clamp(rx, -32768, 32767)),
                        int16_t(std::clamp(ry, -32768, 32767))});
      length_ = std::max(length_, start + duration);
    }
    std::fclose(f);
  }
  std::vector<Step> steps_;
  double length_ = 0;
  uint32_t packet_ = 0;
  std::chrono::steady_clock::time_point start_, next_check_;
  std::filesystem::file_time_type loaded_stamp_{};
};

// Synthetic devices are parked past every physical ordinal so they cannot push
// a real pad off guest user 0. SlotAssignment routes them by their synthetic
// flag and never reads this value.
constexpr uint32_t kSyntheticOrdinal = UINT32_MAX;

}  // namespace

InputSystem::InputSystem(rex::ui::Window* window) : window_(window) {}

InputSystem::~InputSystem() = default;

X_STATUS InputSystem::Setup() {
  return X_STATUS_SUCCESS;
}

void InputSystem::Shutdown() {
  // device_owners_ holds raw driver pointers.
  devices_.clear();
  device_owners_.clear();
  drivers_.clear();
}

void InputSystem::AddDriver(std::unique_ptr<InputDriver> driver) {
  drivers_.push_back(std::move(driver));
}

void InputSystem::AttachWindow(rex::ui::Window* window) {
  window_ = window;
  for (auto& driver : drivers_) {
    driver->OnWindowAvailable(window);
  }
}

void InputSystem::SetActiveCallback(std::function<bool()> callback) {
  for (auto& driver : drivers_) {
    driver->set_is_active_callback(callback);
  }
}

void InputSystem::SetDeviceAssignment(std::unique_ptr<DeviceAssignment> assignment) {
  assignment_ = std::move(assignment);
  if (assignment_) {
    assignment_->OnDevicesChanged(devices_);
  }
}

void InputSystem::RefreshDevices() {
  /*
   * At most one enumeration every 50 ms.
   *
   * Stack sampling found the thread that prepares each game frame spending 5.8 % of its time
   * inside SetState (vibration), and every game call to the controller went through here:
   * three new vectors and a query to each driver, several times per frame. A controller is not
   * connected or disconnected 30 times per second; with 50 ms a new controller shows up before
   * anyone notices. The first call always enumerates.
   */
  {
    const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();
    const int64_t last = last_refresh_ns_.load(std::memory_order_relaxed);
    if (last != 0 && !devices_.empty() && now - last < 50000000) {
      return;
    }
    last_refresh_ns_.store(now, std::memory_order_relaxed);
  }
  std::vector<DeviceInfo> seen;
  std::vector<InputDriver*> owners;
  std::vector<DeviceInfo> enumerated;
  for (auto& driver : drivers_) {
    enumerated.clear();
    driver->EnumerateDevices(enumerated);
    for (auto& info : enumerated) {
      seen.push_back(info);
      owners.push_back(driver.get());
    }
  }

  // Carry forward ordinals already handed out, so a device keeps its guest user
  // when another pad is unplugged.
  bool changed = seen.size() != devices_.size();
  std::vector<bool> fresh(seen.size(), false);
  for (size_t i = 0; i < seen.size(); i++) {
    auto existing = std::find_if(devices_.begin(), devices_.end(),
                                 [&](const DeviceInfo& d) { return d.id == seen[i].id; });
    if (existing != devices_.end()) {
      seen[i].ordinal = existing->ordinal;
      continue;
    }
    fresh[i] = true;
    changed = true;
  }

  // Runs after the carry-forward pass so a new device cannot take an ordinal a
  // live one is still holding. Lowest free rather than a growing counter,
  // because a reconnected pad arrives as a new device and would otherwise walk
  // off the end of the guest users.
  for (size_t i = 0; i < seen.size(); i++) {
    if (!fresh[i]) {
      continue;
    }
    // Only physical devices consume an ordinal, so the first pad to connect is
    // guest user 0 however many synthetic devices enumerated ahead of it.
    if (seen[i].synthetic) {
      seen[i].ordinal = kSyntheticOrdinal;
      fresh[i] = false;
      continue;
    }
    auto taken = [&](uint32_t candidate) {
      for (size_t j = 0; j < seen.size(); j++) {
        if (!fresh[j] && !seen[j].synthetic && seen[j].ordinal == candidate) {
          return true;
        }
      }
      return false;
    };
    uint32_t ordinal = 0;
    while (taken(ordinal)) {
      ordinal++;
    }
    seen[i].ordinal = ordinal;
    fresh[i] = false;
  }

  for (const auto& old : devices_) {
    if (std::none_of(seen.begin(), seen.end(),
                     [&](const DeviceInfo& d) { return d.id == old.id; })) {
      active_devices_.Forget(old.id);
      changed = true;
    }
  }

  std::vector<size_t> order(seen.size());
  for (size_t i = 0; i < order.size(); i++) {
    order[i] = i;
  }
  // Stable: synthetic devices share one ordinal, and their relative order
  // decides which answers GetCapabilities when no pad is attached.
  std::stable_sort(order.begin(), order.end(),
                   [&](size_t a, size_t b) { return seen[a].ordinal < seen[b].ordinal; });

  devices_.clear();
  device_owners_.clear();
  for (size_t i : order) {
    devices_.push_back(seen[i]);
    device_owners_.push_back(owners[i]);
  }

  if (changed && assignment_) {
    assignment_->OnDevicesChanged(devices_);
  }
}

InputDriver* InputSystem::DriverForDevice(DeviceId id) {
  for (size_t i = 0; i < devices_.size(); i++) {
    if (devices_[i].id == id) {
      return device_owners_[i];
    }
  }
  return nullptr;
}

const DeviceInfo* InputSystem::DeviceInfoFor(DeviceId id) const {
  for (const auto& device : devices_) {
    if (device.id == id) {
      return &device;
    }
  }
  return nullptr;
}

X_RESULT InputSystem::GetCapabilities(uint32_t user_index, uint32_t flags,
                                      X_INPUT_CAPABILITIES* out_caps) {
  SCOPE_profile_cpu_f("hid");
  std::lock_guard lock(mutex_);  // guest threads poll input concurrently; RefreshDevices rebuilds devices_
  if (!out_caps || !assignment_) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  RefreshDevices();
  std::vector<DeviceId> ids;
  assignment_->DevicesForUser(user_index, ids);
  if (ids.empty()) {
    if (user_index == 0 && !REXCVAR_GET(input_script).empty()) {
      // --input_script stands in for a standard pad when none is connected.
      *out_caps = {};
      out_caps->type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
      out_caps->sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
      out_caps->gamepad.buttons = 0xF3FF;
      out_caps->gamepad.left_trigger = out_caps->gamepad.right_trigger = 0xFF;
      out_caps->gamepad.thumb_lx = out_caps->gamepad.thumb_ly = int16_t(0xFFC0);
      out_caps->gamepad.thumb_rx = out_caps->gamepad.thumb_ry = int16_t(0xFFC0);
      return X_ERROR_SUCCESS;
    }
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // Prefer the pad in hand, so button glyphs follow it rather than whichever
  // device enumerated first.
  DeviceId chosen = active_devices_.Active(user_index);
  if (std::find(ids.begin(), ids.end(), chosen) == ids.end()) {
    chosen = ids.front();
  }

  auto* driver = DriverForDevice(chosen);
  if (!driver) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  return driver->GetDeviceCapabilities(chosen, flags, out_caps);
}

X_RESULT InputSystem::GetState(uint32_t user_index, X_INPUT_STATE* out_state) {
  SCOPE_profile_cpu_f("hid");
  std::lock_guard lock(mutex_);  // guest threads poll input concurrently; RefreshDevices rebuilds devices_
  if (user_index == 0) {
    X_INPUT_STATE scripted = {};
    if (InputScript::Get().State(&scripted)) {
      if (out_state) *out_state = scripted;
      return X_ERROR_SUCCESS;
    }
  }
  if (!assignment_) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  RefreshDevices();
  std::vector<DeviceId> ids;
  assignment_->DevicesForUser(user_index, ids);

  X_INPUT_STATE merged = {};
  bool any = false;
  for (DeviceId id : ids) {
    auto* driver = DriverForDevice(id);
    if (!driver) {
      continue;
    }
    X_INPUT_STATE state = {};
    if (driver->GetDeviceState(id, &state) != X_ERROR_SUCCESS) {
      continue;
    }
    if (const DeviceInfo* info = DeviceInfoFor(id); info && !info->synthetic) {
      ApplyPadRemap(state);  // the title's controller remap (real pads only)
    }
    active_devices_.Observe(user_index, id, state.gamepad);
    if (!any) {
      merged = state;
      any = true;
    } else {
      MergeInto(merged, state);
    }
  }

  if (!any) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (out_state) {
    *out_state = merged;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT InputSystem::SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) {
  SCOPE_profile_cpu_f("hid");
  std::lock_guard lock(mutex_);  // guest threads poll input concurrently; RefreshDevices rebuilds devices_
  if (!assignment_) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  RefreshDevices();
  std::vector<DeviceId> ids;
  assignment_->DevicesForUser(user_index, ids);

  // Every pad on this user belongs to the same player, so all of them buzz.
  // Only pads decide the result: synthetic devices accept any vibration and
  // would otherwise report success for a pad that never rumbled.
  bool any_pad = false;
  bool any_synthetic = false;
  bool pad_rumbled = false;
  X_RESULT pad_error = X_ERROR_DEVICE_NOT_CONNECTED;
  // input_rumble = false: the game still sees its call serviced, but the controllers only
  // receive zero vibration.
  X_INPUT_VIBRATION no_rumble{};
  if (!REXCVAR_GET(input_rumble)) {
    vibration = &no_rumble;
  }
  for (DeviceId id : ids) {
    auto* driver = DriverForDevice(id);
    const DeviceInfo* info = DeviceInfoFor(id);
    if (!driver || !info) {
      continue;
    }
    X_RESULT result = driver->SetDeviceVibration(id, vibration);
    if (info->synthetic) {
      any_synthetic = true;
      continue;
    }
    any_pad = true;
    if (result == X_ERROR_SUCCESS) {
      pad_rumbled = true;
    } else {
      pad_error = result;
    }
  }
  if (any_pad) {
    return pad_rumbled ? X_ERROR_SUCCESS : pad_error;
  }
  return any_synthetic ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
}

X_RESULT InputSystem::GetKeystroke(uint32_t user_index, uint32_t flags,
                                   X_INPUT_KEYSTROKE* out_keystroke) {
  SCOPE_profile_cpu_f("hid");
  std::lock_guard lock(mutex_);  // guest threads poll input concurrently; RefreshDevices rebuilds devices_
  if (!assignment_) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  RefreshDevices();
  std::vector<DeviceId> ids;
  assignment_->DevicesForUser(user_index, ids);

  bool any_connected = false;
  for (DeviceId id : ids) {
    auto* driver = DriverForDevice(id);
    if (!driver) {
      continue;
    }
    X_RESULT result = driver->GetDeviceKeystroke(id, flags, out_keystroke);
    if (result == X_ERROR_SUCCESS) {
      out_keystroke->user_index = static_cast<uint8_t>(user_index);
      return result;
    }
    if (result != X_ERROR_DEVICE_NOT_CONNECTED) {
      any_connected = true;
    }
  }
  return any_connected ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
}

std::unique_ptr<InputSystem> CreateDefaultInputSystem(bool tool_mode) {
  auto input = std::make_unique<InputSystem>(nullptr);

  if (!tool_mode) {
#if REX_PLATFORM_WIN32
    if (REXCVAR_GET(input_backend) == "xinput") {
      auto xinput_driver = std::make_unique<xinput::XinputInputDriver>(nullptr, 0);
      if (xinput_driver->Setup() == X_STATUS_SUCCESS) {
        input->AddDriver(std::move(xinput_driver));
      }
    }
#endif

#if REX_PLATFORM_SWITCH
    if (REXCVAR_GET(input_backend) == "switch") {
      auto switch_driver = std::make_unique<nx::SwitchInputDriver>(nullptr, 0);
      if (switch_driver->Setup() == X_STATUS_SUCCESS) {
        input->AddDriver(std::move(switch_driver));
      }
    }
#else
    if (REXCVAR_GET(input_backend) == "sdl") {
      auto sdl_driver = std::make_unique<sdl::SDLInputDriver>(nullptr, 0);
      if (sdl_driver->Setup() == X_STATUS_SUCCESS) {
        input->AddDriver(std::move(sdl_driver));
      }
    }
#endif

    // MnK driver (keyboard/mouse -> controller emulation)
    auto mnk_driver = std::make_unique<mnk::MnkInputDriver>(nullptr, 0);
    if (mnk_driver->Setup() == X_STATUS_SUCCESS) {
      input->AddDriver(std::move(mnk_driver));
    }
  }

  // NOP driver (primary in tool mode, fallback otherwise)
  uint8_t nop_index = tool_mode ? 0 : 1;
  input->AddDriver(std::make_unique<nop::NopInputDriver>(nullptr, nop_index));
  input->SetDeviceAssignment(std::make_unique<SlotAssignment>());
  return input;
}

}  // namespace rex::input
