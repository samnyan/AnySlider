# Configuration Examples

## GAMO2 FAUCETWO SDVX Controller

For the FAUCETWO controller, use HID keyboard/mouse mode (hold START + FX-L + FX-R + BT-C for three seconds).

```toml
# Enable the core IO features.
io_enabled = true

# Keep the game running when it is not the foreground window.
io_keep_game_active_unfocused = true

# Enable keyboard/mouse IO.
io_keyboard_mouse_frontend = true
# Block the game's own keyboard/mouse input to avoid duplicate input.
io_block_keyboard_mouse_input = true

# Use Arcade mode for keyboard LR and gamepad left/right slider input.
io_slider_mode = "arcade"
# Slider emulation speed in Arcade mode; adjust as needed.
io_arcade_slider_emu_cells_per_second = 32.0

# Use mouse input as slider input; this must be enabled.
io_use_mouse_as_slider = true
# Specify the FAUCETWO device ID to filter out other mouse input.
io_mouse_slider_device = "0E8F:1118"


# FAUCETWO key configuration.
io_key_triangle = ["D"]
io_key_square = ["F"]
io_key_cross = ["J"]
io_key_circle = ["K"]
```

## Using a DualShock 4/DualSense Touchpad as a Slider

```toml
# Enable the core IO features.
io_enabled = true

# Gamepad frontend. Supports DS4, DualSense, Pro Controller, and Joy-Con.
io_gamepad_frontend = true

# Use a DS4/DualSense touchpad as slider input.
io_gamepad_touchpad_slider = true
```

## Simulating High-Speed Sliders in Arcade Controller Mode

Enable the keyboard or gamepad frontend.

```toml
# Slider mode: "arcade" emulates keyboard/gamepad directions as slider movement.
io_slider_mode = "arcade"

# Slider emulation speed in Arcade mode, measured in cells per second.
# Only affects left/right directional input; it does not affect direct 32-cell input or mouse_as_slider mode.
io_arcade_slider_emu_cells_per_second = 32.0

# The keyboard mode requires the following key bindings:
io_key_slider_1_left = ["Q"]
io_key_slider_1_right = ["E"]
io_key_slider_2_left = ["U"]
io_key_slider_2_right = ["O"]
```
