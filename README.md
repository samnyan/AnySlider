# AnySlider

English | [中文](README_CN.md)

Allows any controller to use the "Arcade Controller" mode in Project DIVA Mega Mix+.
Developed based on version 1.03.

## What's New in V2

Provides an input API via hooking to natively support various controller types without relying on keyboard or gamepad emulation logic.

## How It Works (V1)

Originally, the game only allows the following controllers to use the "Arcade Controller" option:

| VID:PID | Type | Controller |
| --- | ---: | --- |
| `0F0D:00FB` | 6 | HORI NSW-230 Mega39's |
| `0F0D:013C` | 7 | HORI PS4-161 Future Tone DX |

This mod removes the VID/PID validation, allowing any controller to be recognized as a Type 6 or Type 7 controller.

## Configuration

```toml
enabled = true
dll = ["AnySlider.dll"]

vid_pids = ["054C:05C4"]
target_controller_type = 7
```

Set `vid_pids` to the VID:PID of your source controller. For example, `"054C:05C4"` is the common VID:PID used by the ViGEm controller emulator.
Then set `target_controller_type` to the controller type you want it to be recognized as. In theory, there is no functional difference between 6 and 7—only the button icons displayed in-game will differ.

Startup logs will appear on launch (if you have enabled the console option in DML):

```text
[AnySlider] DirectInput VID:PID 054C:05C4 -> 0F0D:013C (type 7)
[AnySlider] DirectInput VID:PID 054C:0CE6 unchanged
```

Then simply enable the "Arcade Controller" mode in the game settings.

## Building

1. Simply build the `AnySlider.vcxproj` project.
