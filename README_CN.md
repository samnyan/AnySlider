# AnySlider

[English](README.md) | 中文

让任何控制器支持 Project DIVA Mega Mix+ 的“街机控制器”模式。
基于 1.03 版本开发。

## V2 新功能

通过 Hook 提供原生输入 API，支持共享内存输入和可选的键盘/鼠标前端，不依赖键盘或手柄模拟。

## 工作原理（V1）

V1 会移除游戏对官方控制器 VID/PID 的限制，让其他控制器被识别为 HORI Type 6 或 Type 7 控制器。

## 设置说明

AnySlider 包含两套独立功能：V1 VID/PID 重映射，以及 V2 原生 IO。

### V1 VID/PID 重映射

```toml
enabled = true
dll = ["AnySlider.dll"]
vid_pids = ["054C:05C4"]
target_controller_type = 7
```

`vid_pids` 设置源控制器的 VID:PID。`target_controller_type` 可设置为 `6`（HORI NSW-230 Mega39's）或 `7`（HORI PS4-161 Future Tone DX）。这部分功能与 V2 原生 IO 相互独立。

### V2 原生 IO

```toml
# 缺少此项时默认为关闭。
io_enabled = true
io_shared_memory = "Local\\MMIO_SHARED_BUFFER"
io_max_input_lease_ms = 500

# 高级选项：屏蔽游戏内置输入，只由mod提供输入。
# 默认关闭；开启后可能有未知问题。
io_exclusive_controller_input = false

# 内置键盘/鼠标前端，游戏会将其作为 controller 输入处理。
io_keyboard_mouse_frontend = false

# 屏蔽游戏自身的键盘/鼠标输入，避免与内置前端产生重复输入。
io_block_keyboard_mouse_input = false

# 键盘滑条模拟速度，单位为 cell/秒。
io_keyboard_slider_cells_per_second = 32.0

# 让游戏不在前台时保持运行，不自动暂停。如果需要在本机运行虚拟手台会比较有用。
io_keep_game_active_unfocused = false
```

### 按键绑定

每个绑定支持字符串数组，也支持逗号分隔的字符串。使用空数组 `[]` 可禁用绑定。支持 `W`、`F1`、`Enter`、`Left`、`LeftShift`、`NumPad0`、`Space`、`MouseLeft` 等名称，也可以填写虚拟键码，例如 `0x41`。

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

还可以使用 `io_key_slider_01` 至 `io_key_slider_32` 直接绑定 32 个滑条 cell；`01` 是最左侧的原生 sensor，默认均为 `[]`。

## 构建

构建 `AnySlider.vcxproj` 项目即可。
