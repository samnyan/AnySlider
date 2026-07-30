# AnySlider

AnySlider allows any controllers to use Mega Mix+'s native arcade slider
Test with v1.03

## How it works

The game only allow the following controller type to use Arcade Slider mode.

| VID:PID | Type | Controller |
| --- | ---: | --- |
| `0F0D:00FB` | 6 | HORI NSW-230 Mega39's |
| `0F0D:013C` | 7 | HORI PS4-161 Future Tone DX |

This mod will make the game recognize any controller to type 6 or 7.

## Configuration

```toml
enabled = true
dll = ["AnySlider.dll"]

vid_pids = ["054C:05C4"]
target_controller_type = 7
```

`vid_pids` accepts multiple hexadecimal source identities. Target type `6`
means HORI NSW-230 (`0F0D:00FB`); type `7` means HORI PS4-161
(`0F0D:013C`). Type 7 is normally preferable for a ViGEm DualShock 4 because
it keeps PS4-oriented button mapping.

The console reports each first-seen identity:

```text
[AnySlider] DirectInput VID:PID 054C:05C4 -> 0F0D:013C (type 7)
[AnySlider] DirectInput VID:PID 054C:0CE6 unchanged
```

## Build

1. Build `AnySlider.vcxproj` as Release x64.
2. Copy both `AnySlider.dll` and the updated `config.toml` into the mods folder.
3. Configure the source VID/PID and target type.
4. Enable the in-game Arcade Controller setting.