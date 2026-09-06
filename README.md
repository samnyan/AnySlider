# AnySlider

English | [中文](README_CN.md)

Allows any controller to use Project DIVA Mega Mix+'s “Arcade Controller” mode.
Based on version 1.03.

## What's New in V2

Provides a native input API through hooks, supporting shared-memory input and an optional built-in keyboard/mouse frontend without relying on keyboard or gamepad emulation.

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

Set `vid_pids` to the VID:PID of the source controller. `target_controller_type` can be set to `6` (HORI NSW-230 Mega39's) or `7` (HORI PS4-161 Future Tone DX). This feature is independent of V2 native IO.

### V2 Native IO

```toml
# Disabled by default when omitted.
io_enabled = true
io_shared_memory = "Local\\MMIO_SHARED_BUFFER"
io_max_input_lease_ms = 500

# Advanced: suppress the game's native controller input and use only
# input provided by the mod. Disabled by default; enabling it may have
# unknown issues.
io_exclusive_controller_input = false

# Built-in keyboard/mouse frontend. The game processes it as controller input.
io_keyboard_mouse_frontend = false

# Block the game's own keyboard/mouse input to avoid duplicate input
# when using the built-in frontend.
io_block_keyboard_mouse_input = false

# Keyboard slider emulation speed, in cells per second.
io_keyboard_slider_cells_per_second = 32.0

# Keep the game running instead of automatically pausing when it is not
# the foreground window. Useful when running a virtual arcade controller locally.
io_keep_game_active_unfocused = false
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
