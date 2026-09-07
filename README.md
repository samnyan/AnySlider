# AnySlider

[English](README.md) | [中文](README_CN.md)

Allows any controller to use Project DIVA Mega Mix+'s “Arcade Controller” mode.
Based on version 1.03.

## V2 New Features

Provides a native input API through hooks, supporting shared-memory input and optional keyboard/mouse frontends without relying on keyboard or gamepad emulation.

## How It Works (V1)

V1 removes the game's restriction on official controller VID/PIDs, allowing other controllers to be recognized as HORI Type 6 or Type 7 controllers.

## Configuration

AnySlider contains two independent features: V1 VID/PID remapping and V2 native IO.

### V1 VID/PID Remapping

```toml
enabled = true
dll = ["AnySlider.dll"]
vid_pids = ["054C:05C4"]
target_controller_type = 7
```

Set `vid_pids` to the source controller's VID:PID. `target_controller_type` can be set to `6` (HORI NSW-230 Mega39's) or `7` (HORI PS4-161 Future Tone DX). This feature is independent of V2 native IO.

### V2 Native IO

For configuration examples, see [CONFIG.md](CONFIG.md).

```toml
# Disabled by default when omitted.
io_enabled = true

# Debug logging.
debug = false

# Advanced: block the game's built-in input and accept input only from the mod.
io_exclusive_controller_input = true

# Keep the game running instead of automatically pausing when it is not the foreground window.
# This is useful when running a virtual arcade controller locally.
io_keep_game_active_unfocused = true

# ============================
# Generic IO Settings
# ============================

# Keyboard LR and gamepad left/right slider mode:
# "arcade" emulates keyboard/gamepad directions as slider movement
# (for example, right simulates a movement from the left touch area to the right).
# "joystick" maps keyboard/gamepad directions to the game's built-in left/right input.
io_slider_mode = "arcade"

# Slider emulation speed in Arcade mode, measured in cells per second.
# Only affects left/right directional input; it does not affect direct 32-cell input or mouse_as_slider mode.
io_arcade_slider_emu_cells_per_second = 32.0

# ============================
# Keyboard/Mouse IO Settings (AnySlider replaces the game's built-in keyboard/mouse input)
# ============================

# Built-in keyboard/mouse frontend. The game processes it as controller input.
io_keyboard_mouse_frontend = false

# Block the game's own keyboard/mouse input to avoid duplicate input with the built-in frontend.
io_block_keyboard_mouse_input = false

# Use mouse input as slider input, for example with an arcade controller that has two knobs.
# Requires the built-in keyboard/mouse frontend to be enabled.
io_use_mouse_as_slider = false

# Mouse movement is relative, so stopping movement is treated as releasing the touch.
# Keep the simulated slider touch active for this long after mouse movement stops.
# If the touch disconnects while moving, increase this value if the device has a low report rate.
io_mouse_slider_touch_hold_ms = 20

# Specify the mouse device VID:PID. If omitted, data from all mouse devices is accepted.
io_mouse_slider_device = "0E8F:1118"

# ============================
# Gamepad IO Settings (AnySlider replaces the game's built-in gamepad input)
# ============================

# Gamepad frontend. Supports DS4, DualSense, Pro Controller, and Joy-Con.
io_gamepad_frontend = false
io_gamepad_device = "auto"
# Use a DS4/DualSense touchpad as slider input.
io_gamepad_touchpad_slider = true
# Reverse touchpad left/right.
io_gamepad_touchpad_invert = false
# Stick deadzone, from 0.1 to 0.95.
io_gamepad_stick_slider_deadzone = 0.3
```

### Key Bindings

Each binding accepts a string array or a comma-delimited string. Use an empty array (`[]`) to disable a binding. Supported names include `W`, `F1`, `Enter`, `Left`, `LeftShift`, `NumPad0`, `Space`, and `MouseLeft`. Virtual key codes such as `0x41` are also accepted.

```toml
io_key_test = ["F1"]
io_key_service = ["F2"]
io_key_pause = ["Escape"]
io_key_start = ["Enter"]
io_key_dpad_up = ["Up"]
io_key_dpad_down = ["Down"]
io_key_dpad_left = ["Left"]
io_key_dpad_right = ["Right"]
io_key_triangle = ["W"]
io_key_square = ["A"]
io_key_cross = ["S"]
io_key_circle = ["D"]
io_key_l1 = ["Z"]
io_key_r1 = ["X"]
io_key_l2 = []
io_key_r2 = []
io_key_select = []
io_key_l3 = []
io_key_r3 = []

io_key_slider_1_left = ["Q"]
io_key_slider_1_right = ["E"]
io_key_slider_2_left = ["U"]
io_key_slider_2_right = ["O"]
```

You can also bind `io_key_slider_01` through `io_key_slider_32` directly to the 32 slider cells. `01` is the leftmost native sensor; all of these bindings are empty (`[]`) by default.

## Building

Build the `AnySlider.vcxproj` project.
