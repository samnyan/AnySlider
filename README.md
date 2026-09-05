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
4. Enable the in-game Arcade Controller setting for VID/PID remapping. The native IO backend temporarily enables its effective arcade mode itself.

## DivaModLoader lifecycle

AnySlider is loaded as a DLL mod through the `dll = ["AnySlider.dll"]` entry in
its mod `config.toml`. `DllMain` deliberately does no work. DivaModLoader calls
the exported `Init()` after MM+'s global state is ready; it loads this mod's
configuration and installs the input hooks. DivaModLoader calls
`OnFrame(IDXGISwapChain*)` before each presentation, which lets AnySlider locate
and maintain its game-window hooks.

## Native IO backend

Set `io_hook = true` to consume `Local\\MMIO_SHARED_BUFFER`. The public ABI is in [include/mm_io/shared_memory.h](include/mm_io/shared_memory.h). The DLL consumes a stable shared-memory snapshot during the game's input frame and writes the game-owned tapped, released, held, and slider state.

ABI v1 follows Waccon naming: `gamebtn[3]` contains MM+'s complete 192-bit raw action state, including pause at bit 160; `touch_cells[32]` holds the native slider cells. A 128-item ordered event ring records each `gamebtn` transition. The DLL consumes at most one event per game input frame, so a press and release that both happen between two game frames are still delivered as separate input frames. `hook.event_sequence` reports the most recently consumed event; `hook.input_sequence` remains the latest consumed snapshot.

For the first in-game test frontend, set `io_keyboard_frontend = true`. It is built into `AnySlider.dll` and uses these bindings:

| Input | Keys |
| --- | --- |
| Triangle / Square / Cross / Circle | W / A / S / D |
| D-pad | Arrow keys |
| Pause, Start, L1, R1 | Escape, Enter, Z, X |
| L2, R2, Select, L3, R3 | Unbound by default |
| Test, Service | F1, F2 |
| Slider contact 1 | Q / E |
| Slider contact 2 | U / O |

Each slider pair moves one independent contact across the 32 native slider cells while held. `io_keyboard_slider_cells_per_second` controls its speed. An active external shared-memory producer takes priority over the built-in frontend.

Every built-in binding can be overridden by the `io_key_*` entries in
`config.toml`. They follow PD-Loader's key names and accept either a
comma-delimited string or an array, so `io_key_triangle = ["W", "I"]` and
`io_key_triangle = "W, I"` both make either key press Triangle. `[]` disables
that binding. The optional `io_key_slider_01` through `io_key_slider_32`
entries activate individual native slider cells directly; cell 01 is the
leftmost sensor.

## Overlay focus and physical keyboard

`io_bypass_focus_loss = true` keeps MM+ active when an overlay takes foreground focus. It intercepts the game window's deactivate messages and makes the game's `GetForegroundWindow` check continue to see its own window.

`io_block_keyboard_input = true` suppresses the game's Win32 key messages and `GetAsyncKeyState` keyboard polling. The built-in keyboard frontend remains available because its own polling is scoped around the block. It is only a keyboard-message compatibility option; DirectInput, Steam Input, Raw Input device paths, and controller input are controlled by `io_takeover`.

With an active MMIO source and `io_takeover = true`, AnySlider bypasses MM+'s slider, connected-device, and selected-device merges. The backend snapshot and event ring are then the only game-controller source for that input frame. This also applies to the built-in keyboard frontend, so its configured keys enter through MMIO rather than through MM+'s physical keyboard device path. Pointer UI requires separate runtime validation because song selection also reads mouse button and pointer-position inputs from the suppressed device path.

With takeover active, AnySlider supplies MM+'s selected-device state as controller type 7 (PS-style) so controller prompts have a valid source. `io_force_gamepad_ui = true` applies that virtual selected device even when physical device merging remains enabled.

When the IO backend is enabled, AnySlider also logs the first successful Steam Input controller poll. MM+ has a native Steam Input path alongside DirectInput; the message identifies that a Steam Input controller reached the game, rather than merely that `steam_api64.dll` was loaded. The shared-memory ABI is version 1.0 and remains private to this test phase.
