# Config Preset

## GAMO2 FAUCETWO SDVX controller

For FAUCETWO controller, use HID keyboard&mouse mode (Press START + FX-L + FX-R + BT-C for 3 seconds.)

```toml
# Enable IO features
io_enabled = true

# Enable keyboard mouse io
io_keyboard_mouse_frontend = true
# Disable original keyboard/mouse input, otherwise it will cause dual input.
io_block_keyboard_mouse_input = true

# Enable to use mouse slider
io_use_mouse_as_slider = true
# Filter the device to only use FAUCETWO controller (VID:PID is also accepted)
io_mouse_slider_device = "0E8F:1118"

# Keep the game active even when not at foreground.
io_keep_game_active_unfocused = true

# Key config for FAUCETWO controller
io_key_triangle = ["D"]
io_key_square = ["F"]
io_key_cross = ["J"]
io_key_circle = ["K"]
```
