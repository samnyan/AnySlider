# AnySlider

[English](README.md) | 中文

让任何控制器支持 Project DIVA Mega Mix+ 的“街机控制器”模式。
基于1.03版本开发。

## V2新功能

通过Hook方式提供输入API，以便原生支持各种控制器类型，而不需要经过键盘或者手柄模拟的逻辑。

## 工作原理（V1）

原本游戏只允许下列控制器使用“街机控制器”的选项。

| VID:PID | Type | Controller |
| --- | ---: | --- |
| `0F0D:00FB` | 6 | HORI NSW-230 Mega39's |
| `0F0D:013C` | 7 | HORI PS4-161 Future Tone DX |

这个Mod会移除VID PID验证，让所有控制器都能识别出Type 6或者Type 7方式的控制器。

## 设置说明

```toml
enabled = true
dll = ["AnySlider.dll"]

vid_pids = ["054C:05C4"]
target_controller_type = 7
```

`vid_pids` 设置为你的源控制器的值，比如 "054C:05C4" 是常见的手柄模拟 ViGEm 的VID PID。
然后 `target_controller_type` 设为你想要识别出的控制器类型，理论上6和7没区别，只是显示的按键图例会不一样。

启动时会出现日志（如果你在DML打开了console选项）

```text
[AnySlider] DirectInput VID:PID 054C:05C4 -> 0F0D:013C (type 7)
[AnySlider] DirectInput VID:PID 054C:0CE6 unchanged
```

接着在游戏里打开“街机控制器”模式即可。

## 构建

1. 构建 `AnySlider.vcxproj` 项目即可。

