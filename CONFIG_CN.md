# 配置范例

## GAMO2 FAUCETWO SDVX 手台

FAUCETWO 手台请使用 HID 键盘鼠标模式 (长按 START + FX-L + FX-R + BT-C 三秒)

```toml
# 开启核心IO功能
io_enabled = true

# 让游戏不在前台时保持运行
io_keep_game_active_unfocused = true

# 开启键鼠IO
io_keyboard_mouse_frontend = true
# 屏蔽游戏自身的键盘/鼠标输入，避免产生重复输入。
io_block_keyboard_mouse_input = true

# 键盘LR、手柄左右滑条模式使用arcade
io_slider_mode = "arcade"
# Arcade 模式下滑条模拟速度，可以自行增减
io_arcade_slider_emu_cells_per_second = 32.0

# 使用鼠标输入来作为滑条使用，需要开启
io_use_mouse_as_slider = true
# 指定 FAUCETWO 的设备ID，过滤掉其他鼠标输入
io_mouse_slider_device = "0E8F:1118"


# FAUCETWO 按键配置
io_key_triangle = ["D"]
io_key_square = ["F"]
io_key_cross = ["J"]
io_key_circle = ["K"]
```


## DualShock4/DualSense 手柄触摸板当作滑条使用

```toml
# 开启核心IO功能
io_enabled = true

# 手柄前端，支持 DS4、DualSense、Pro Controller 和 Joy-Con。
io_gamepad_frontend = true

# 使用DS4/DualSense的触摸板作为滑条输入
io_gamepad_touchpad_slider = true
```

## 模拟街机控制器模式的高速滑条

开启键盘或手柄前端

```toml
# 滑条模式："arcade" 将键盘/手柄方向模拟在滑条上滑动
io_slider_mode = "arcade"

# Arcade 模式下滑条模拟速度，单位为 cell/秒。
# 只对左右输入有效，对直接 32-cell 输入和 mouse_as_slider 模式无效
io_arcade_slider_emu_cells_per_second = 32.0

# 键盘模式需要绑定以下按键：
io_key_slider_1_left = ["Q"]
io_key_slider_1_right = ["E"]
io_key_slider_2_left = ["U"]
io_key_slider_2_right = ["O"]
```