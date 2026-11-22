#include "pch.h"

#include "mm_io_hooks.h"
#include "mm_device_binding.h"
#include "mm_ui_input.h"
#include "mm_ui_aet.h"
#include "mm_pointer_merge_policy.h"
#include "mm_input_diagnostics.h"
#include <intrin.h>
#include "mm_led_hooks.h"
#include "mm_led_diagnostics.h"
#include "mm_arcade_led.h"

#include "Dependencies/Signature.h"
#include "anyslider_log.h"
#include "mm_game_state.h"
#include "mm_io_keyboard.h"
#include "mm_io_joyshock.h"
#include "mm_io_raw_input.h"
#include "mm_io/shared_memory.h"
#include "mm_io_shared_memory.h"

#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstring>

// The detour entry is implemented in mm_arcade_query_bridge.asm, not C++.
extern "C" uint32_t IsArcadeControllerEnabledHook();

namespace anyslider
{
namespace
{

constexpr size_t InputTappedButtonsOffset = 0x00;
constexpr size_t InputReleasedButtonsOffset = 0x18;
constexpr size_t InputHeldButtonsOffset = 0x30;
constexpr size_t InputAnalogOffset = 0xE0;
constexpr size_t InputSelectedDevicePresentOffset = 0x2C9;
constexpr size_t SliderCellsAnalogIndex = 6;
constexpr size_t LeftStickXAnalogIndex = 0x14;
constexpr size_t LeftStickYAnalogIndex = 0x15;
constexpr size_t RightStickXAnalogIndex = 0x16;
constexpr size_t RightStickYAnalogIndex = 0x17;
// Use how many cell at two side of ther slider to act as Left Right action
constexpr size_t ArcadeSliderSwitchCellCount = 4;

constexpr uint32_t DualShock4ControllerType = 2;
constexpr uint32_t DualSenseControllerType = 3;
constexpr uint32_t NintendoControllerType = 4;
constexpr uint32_t HoriFTDXArcadeControllerType = 7;
constexpr uint32_t NoControllerType = 11;

using MergeSliderSensorButtons = int64_t(__fastcall*)(void* state, void* sensorState);
using MergeConnectedDevice = int64_t(__fastcall*)(void* state, void* deviceState, uint32_t playerIndex, uint8_t primaryContext);
using MergeSelectedDevice = int64_t(__fastcall*)(void* state, void* deviceState, uint32_t playerIndex, uint32_t* selectedDeviceType);
using DirectInputDevicePollState = char(__fastcall*)(void* device, void* deviceState);
using ResetInputState = int64_t(__fastcall*)(void* state);
using InputConfigGetDeviceActionBinding = uint32_t(__fastcall*)(uint32_t action, uint32_t deviceIndex);
using GetActionBinding = uint32_t*(__fastcall*)(void*, uint32_t*, uint32_t);
using IsArcadeControllerEnabled = uint32_t(__fastcall*)();

MergeSliderSensorButtons originalMergeSliderSensorButtons = nullptr;
MergeConnectedDevice originalMergeConnectedDevice = nullptr;
MergeSelectedDevice originalMergeSelectedDevice = nullptr;
DirectInputDevicePollState originalDirectInputDevicePollState = nullptr;
ResetInputState resetInputState = nullptr;
InputConfigGetDeviceActionBinding originalInputConfigGetDeviceActionBinding = nullptr;
GetActionBinding originalGetActionBinding = nullptr;
IsArcadeControllerEnabled originalIsArcadeControllerEnabled = nullptr;

using MouseDeviceGateFn = int64_t(__fastcall*)(uint8_t);
MouseDeviceGateFn originalMouseDeviceGate = nullptr;
bool mouseDeviceGateInstalled = false;
MmIoConfig mmIoConfig;
int64_t __fastcall MouseDeviceGateHook(uint8_t enabled)
{
    // Preserve scene +1920 and explicit mouse-block policy. Do not change identity.
    return originalMouseDeviceGate(mmIoConfig.show_mouse_with_controller &&
        !mmIoConfig.block_mouse_input ? 1 : enabled);
}
MmIoConsumer mmIoConsumer;
MmIoBoundInputFrontend keyboardFrontend;
MmIoJoyShockFrontend joyShockFrontend;
MmIoIo3Frontend io3Frontend;
MmIoArcadeSliderFrontend arcadeSliderFrontend;
MmIoSliderModeResolver sliderModeResolver;
std::atomic_bool arcadeSliderActive = false;
std::atomic_bool exclusiveControllerActive = false;
std::atomic_uint32_t virtualControllerType = DualSenseControllerType;
bool installed = false;
bool selectedDeviceHookInstalled = false;
bool exclusiveControllerHooksInstalled = false;
bool directInputHooksInstalled = false;
bool inputConfigBindingFallbackHookInstalled = false;
bool arcadeControllerHookInstalled = false;
thread_local bool debugSliderStateInitialized = false;
thread_local MmIoSliderMode debugLastSliderMode = MmIoSliderMode::Arcade;
thread_local bool debugLastDirectTouch = false;
thread_local uint32_t debugLastDirectionHeld = 0;
thread_local bool debugInjectedSliderInitialized = false;
thread_local mmio::Mode debugLastInjectedMode = mmio::Mode::None;
thread_local uint32_t debugLastInjectedValue = 0;
thread_local bool debugInjectedAxesInitialized = false;
thread_local bool debugLastInjectedAxesSimulated = false;
thread_local int32_t debugLastInjectedAxes[4]{};
thread_local bool debugSharedMemorySnapshotInitialized = false;
thread_local uint32_t debugLastSharedMemoryMode = 0;
thread_local uint32_t debugLastSharedMemoryTouchMask = 0;
thread_local uint64_t debugLastSharedMemoryButtons[mmio::kGameButtonWordCount]{};

struct AcceptedInputFrame
{
    uint64_t gamebtn_tapped[mmio::kGameButtonWordCount]{};
    uint64_t gamebtn_released[mmio::kGameButtonWordCount]{};
    uint64_t gamebtn_down[mmio::kGameButtonWordCount]{};
    uint8_t touch_cells[mmio::kTouchCellCount]{};
    float stick_lx = 0.0f;
    float stick_ly = 0.0f;
    float stick_rx = 0.0f;
    float stick_ry = 0.0f;
    bool axes_active = false;
    bool simulated_axes = false;
    bool direct_slider_touch_active = false;
    uint32_t gamepad_slide = 0;
    mmio::Mode slider_mode = mmio::Mode::None;
    bool active = false;
    bool exclusive = false;
    bool ui_activity_pending = false;
};

thread_local AcceptedInputFrame acceptedInputFrame;

struct ButtonEdgeTracker
{
    uint64_t previous_down[mmio::kGameButtonWordCount]{};

    void Reset()
    {
        std::memset(previous_down, 0, sizeof(previous_down));
    }

    void Apply(
        const uint64_t (&currentDown)[mmio::kGameButtonWordCount],
        uint64_t (&tapped)[mmio::kGameButtonWordCount],
        uint64_t (&released)[mmio::kGameButtonWordCount],
        uint64_t (&down)[mmio::kGameButtonWordCount])
    {
        for (uint32_t word = 0; word < mmio::kGameButtonWordCount; ++word)
        {
            tapped[word] = currentDown[word] & ~previous_down[word];
            released[word] = previous_down[word] & ~currentDown[word];
            down[word] = currentDown[word];
            previous_down[word] = currentDown[word];
        }
    }
};

ButtonEdgeTracker keyboardButtonEdges;
ButtonEdgeTracker mergedProviderEdges;
ButtonEdgeTracker gamepadDirectionEdges;
ButtonEdgeTracker arcadeSliderSwitchEdges;

void MergeGameButtons(
    uint64_t (&destination)[mmio::kGameButtonWordCount],
    const uint64_t (&source)[mmio::kGameButtonWordCount])
{
    for (uint32_t word = 0; word < mmio::kGameButtonWordCount; ++word)
    {
        destination[word] |= source[word];
    }
}

bool HasTouchInRange(
    const uint8_t (&touchCells)[mmio::kTouchCellCount],
    size_t begin,
    size_t end)
{
    for (size_t cell = begin; cell < end; ++cell)
    {
        if (touchCells[cell] != 0)
            return true;
    }
    return false;
}

void ApplyArcadeSliderSwitches(AcceptedInputFrame& frame)
{
    const bool leftSwitch = HasTouchInRange(
        frame.touch_cells, 0, ArcadeSliderSwitchCellCount);
    const bool rightSwitch = HasTouchInRange(
        frame.touch_cells,
        mmio::kTouchCellCount - ArcadeSliderSwitchCellCount,
        mmio::kTouchCellCount);
    uint64_t switchHeld[mmio::kGameButtonWordCount]{};

    // Action 18/19 是街机控制器模式下的菜单左右
    mmio::SetGameButton(switchHeld, mmio::Sw1, leftSwitch);
    mmio::SetGameButton(switchHeld, mmio::Sw2, rightSwitch);

    uint64_t switchTapped[mmio::kGameButtonWordCount]{};
    uint64_t switchReleased[mmio::kGameButtonWordCount]{};
    uint64_t switchDown[mmio::kGameButtonWordCount]{};
    // 计算变动状态
    arcadeSliderSwitchEdges.Apply(switchHeld, switchTapped, switchReleased, switchDown);
    MergeGameButtons(frame.gamebtn_tapped, switchTapped);
    MergeGameButtons(frame.gamebtn_released, switchReleased);
    MergeGameButtons(frame.gamebtn_down, switchDown);
    // Do not release a switch while another input provider still holds it.
    for (uint32_t word = 0; word < mmio::kGameButtonWordCount; ++word)
        frame.gamebtn_released[word] &= ~frame.gamebtn_down[word];
    frame.active = frame.active || leftSwitch || rightSwitch ||
        mmio::IsGameButtonDown(switchTapped, mmio::Sw1) ||
        mmio::IsGameButtonDown(switchTapped, mmio::Sw2) ||
        mmio::IsGameButtonDown(switchReleased, mmio::Sw1) ||
        mmio::IsGameButtonDown(switchReleased, mmio::Sw2);
}

float SimulatedAxisValue(uint32_t direction, uint32_t negative, uint32_t positive)
{
    const bool negativeHeld = (direction & negative) != 0;
    const bool positiveHeld = (direction & positive) != 0;
    if (negativeHeld == positiveHeld)
        return 0.0f;
    return negativeHeld ? -1.0f : 1.0f;
}

void SetSimulatedGamepadAxes(AcceptedInputFrame& frame, uint32_t direction)
{
    frame.stick_lx = SimulatedAxisValue(
        direction, MmIoSlideLeft1, MmIoSlideRight1);
    frame.stick_rx = SimulatedAxisValue(
        direction, MmIoSlideLeft2, MmIoSlideRight2);
    frame.stick_ly = 0.0f;
    frame.stick_ry = 0.0f;
    frame.axes_active = true;
    frame.simulated_axes = true;
}

bool HasPrimaryButtonTap(
    const uint64_t (&tapped)[mmio::kGameButtonWordCount])
{
    return
        mmio::IsGameButtonDown(tapped, mmio::Square) ||
        mmio::IsGameButtonDown(tapped, mmio::Triangle) ||
        mmio::IsGameButtonDown(tapped, mmio::Circle) ||
        mmio::IsGameButtonDown(tapped, mmio::Cross);
}

void MergeTouchCells(
    uint8_t (&destination)[mmio::kTouchCellCount],
    const uint8_t (&source)[mmio::kTouchCellCount])
{
    for (uint32_t cell = 0; cell < mmio::kTouchCellCount; ++cell)
    {
        destination[cell] = static_cast<uint8_t>(
            destination[cell] != 0 || source[cell] != 0);
    }
}

void ExpandTouchMask(
    uint32_t mask,
    uint8_t (&cells)[mmio::kTouchCellCount])
{
    for (uint32_t cell = 0; cell < mmio::kTouchCellCount; ++cell)
    {
        // 从左到右是0到31
        cells[cell] = static_cast<uint8_t>((mask & (1u << cell)) != 0);
    }
}

void* FindSignature(const char* bytes, const char* mask)
{
    const MODULEINFO& module = getModuleInfo();
    return sigScan(bytes, mask, std::strlen(mask), module.lpBaseOfDll, module.SizeOfImage);
}

uint64_t MapGamepadSlide(uint32_t slideHeld)
{
    uint64_t result = 0;
    if (slideHeld & MmIoSlideLeft1)
    {
        result |= 1ull << 26;
    }
    if (slideHeld & MmIoSlideRight1)
    {
        result |= 1ull << 27;
    }
    if (slideHeld & MmIoSlideLeft2)
    {
        result |= 1ull << 30;
    }
    if (slideHeld & MmIoSlideRight2)
    {
        result |= 1ull << 31;
    }
    return result;
}

uint32_t MapTouchCells(const uint8_t* touchCells)
{
    uint32_t result = 0;
    for (uint32_t index = 0; index < mmio::kTouchCellCount; ++index)
    {
        if (touchCells[index] != 0)
        {
            result |= 1u << (mmio::kTouchCellCount - 1 - index);
        }
    }
    return result;
}

uint32_t NativeControllerType(MmIoControllerType controllerType)
{
    switch (controllerType)
    {
    case MmIoControllerType::DualShock4:
        return DualShock4ControllerType;
    case MmIoControllerType::Nintendo:
        return NintendoControllerType;
    default:
        return DualSenseControllerType;
    }
}

const char* SliderModeName(MmIoSliderMode mode)
{
    return mode == MmIoSliderMode::Arcade ? "arcade" : "joystick";
}

const char* InputModeName(mmio::Mode mode)
{
    switch (mode)
    {
    case mmio::Mode::ArcadeSlider: return "ArcadeSlider";
    case mmio::Mode::GamepadDualStick: return "GamepadDualStick";
    default: return "None";
    }
}

const char* SharedMemoryModeName(uint32_t mode)
{
    switch (static_cast<mmio::Mode>(mode))
    {
    case mmio::Mode::ArcadeSlider: return "arcade";
    case mmio::Mode::GamepadDualStick: return "dualstick";
    default: return "none";
    }
}

struct SharedMemoryButtonDebugEntry
{
    uint32_t action;
    const char* name;
};

constexpr SharedMemoryButtonDebugEntry kSharedMemoryButtonDebugEntries[] = {
    { mmio::Test, "Test" },
    { mmio::Service, "Service" },
    { mmio::Start, "Start" },
    { mmio::DpadUp, "DpadUp" },
    { mmio::DpadDown, "DpadDown" },
    { mmio::DpadLeft, "DpadLeft" },
    { mmio::DpadRight, "DpadRight" },
    { mmio::Triangle, "Triangle" },
    { mmio::Square, "Square" },
    { mmio::Cross, "Cross" },
    { mmio::Circle, "Circle" },
    { mmio::L1, "L1" },
    { mmio::R1, "R1" },
    { mmio::L2, "L2" },
    { mmio::R2, "R2" },
    { mmio::L3, "L3" },
    { mmio::R3, "R3" },
    { mmio::Select, "Select" },
    { mmio::Stick1Up, "Stick1Up" },
    { mmio::Stick1Down, "Stick1Down" },
    { mmio::Stick1Left, "Stick1Left" },
    { mmio::Stick1Right, "Stick1Right" },
    { mmio::Stick2Up, "Stick2Up" },
    { mmio::Stick2Down, "Stick2Down" },
    { mmio::Stick2Left, "Stick2Left" },
    { mmio::Stick2Right, "Stick2Right" },
    { mmio::Pause, "Pause" },
};

void DebugLogSharedMemoryButtons(const MmIoConsumer::InputFrame& frame)
{
    for (const SharedMemoryButtonDebugEntry& entry : kSharedMemoryButtonDebugEntries)
    {
        if (mmio::IsGameButtonDown(frame.gamebtn_tapped, entry.action))
        {
            DebugLog(
                "Shared memory button: mode=%s action=%s(%u) down",
                SharedMemoryModeName(frame.snapshot.mode),
                entry.name,
                entry.action);
        }
        if (mmio::IsGameButtonDown(frame.gamebtn_released, entry.action))
        {
            DebugLog(
                "Shared memory button: mode=%s action=%s(%u) up",
                SharedMemoryModeName(frame.snapshot.mode),
                entry.name,
                entry.action);
        }
    }
}

void DebugLogSharedMemorySnapshot(const MmIoConsumer::InputFrame& frame)
{
    const mmio::InputSnapshot& snapshot = frame.snapshot;
    const bool changed = !debugSharedMemorySnapshotInitialized ||
        debugLastSharedMemoryMode != snapshot.mode ||
        debugLastSharedMemoryTouchMask != snapshot.touch_mask ||
        std::memcmp(debugLastSharedMemoryButtons, snapshot.gamebtn,
            sizeof(snapshot.gamebtn)) != 0;
    if (!changed)
        return;

    // Log source state only when it changes; timestamp-only publishes stay quiet.
    DebugLog(
        "Shared memory snapshot: mode=%s buttons=[%016llX,%016llX,%016llX] touch-mask=0x%08X",
        SharedMemoryModeName(snapshot.mode),
        static_cast<unsigned long long>(snapshot.gamebtn[0]),
        static_cast<unsigned long long>(snapshot.gamebtn[1]),
        static_cast<unsigned long long>(snapshot.gamebtn[2]),
        snapshot.touch_mask);
    debugSharedMemorySnapshotInitialized = true;
    debugLastSharedMemoryMode = snapshot.mode;
    debugLastSharedMemoryTouchMask = snapshot.touch_mask;
    std::memcpy(debugLastSharedMemoryButtons, snapshot.gamebtn,
        sizeof(debugLastSharedMemoryButtons));
}

void RefreshAcceptedInputFrame()
{
    if (InputDiagnosticsEnabled()) ++inputDiagnosticFrame;
    MmIoConsumer::InputFrame externalFrame{};
    const bool externalFrameAvailable = mmIoConsumer.ReadFrame(
        externalFrame, mmIoConfig.max_input_lease_ms);
    const bool externalActive = externalFrame.source_active;
    const auto io3Frame = io3Frontend.Consume();
    const MmIoKeyboardFrame keyboardFrame = keyboardFrontend.Poll(
        io3Frame.binding_switches, io3Frame.pressed_switches, io3Frame.connected);
    const mmio::InputSnapshot& keyboardSnapshot = keyboardFrame.snapshot;
    uint8_t keyboardTouchCells[mmio::kTouchCellCount]{};
    ExpandTouchMask(keyboardSnapshot.touch_mask, keyboardTouchCells);
    const bool keyboardActive = keyboardFrontend.IsEnabled() || io3Frame.connected || io3Frame.pressed_switches != 0;
    MmIoJoyShockFrame controllerFrame{};
    bool controllerInputActive = false;
    bool controllerTakeoverActive = false;
    if (mmIoConfig.gamepad.enabled)
    {
        controllerFrame = joyShockFrontend.Consume();
        controllerInputActive = controllerFrame.connected;
        controllerTakeoverActive =
            mmIoConfig.exclusive_controller_input && controllerInputActive;
    }
    const uint32_t reportedType = mmIoConfig.controller_type != -1
        ? static_cast<uint32_t>(mmIoConfig.controller_type)
        : controllerInputActive ? NativeControllerType(controllerFrame.controller_type) : DualSenseControllerType;
    virtualControllerType.store(reportedType, std::memory_order_release);

    // 上一帧模拟过轴时，即使本帧没有新 flick，也要写回零轴。
    const bool clearSimulatedAxes = acceptedInputFrame.simulated_axes;
    acceptedInputFrame = {};
    acceptedInputFrame.axes_active = clearSimulatedAxes;
    uint64_t providerHeld[mmio::kGameButtonWordCount]{};
    if (externalFrameAvailable)
    {
        if (IsDebugLoggingEnabled())
        {
            DebugLogSharedMemorySnapshot(externalFrame);
            DebugLogSharedMemoryButtons(externalFrame);
        }
        MergeGameButtons(
            acceptedInputFrame.gamebtn_tapped,
            externalFrame.gamebtn_tapped);
        MergeGameButtons(
            acceptedInputFrame.gamebtn_released,
            externalFrame.gamebtn_released);
        MergeGameButtons(providerHeld, externalFrame.gamebtn_down);
        acceptedInputFrame.active = true;
    }
    if (keyboardActive)
    {
        uint64_t keyboardTapped[mmio::kGameButtonWordCount]{};
        uint64_t keyboardReleased[mmio::kGameButtonWordCount]{};
        uint64_t keyboardHeld[mmio::kGameButtonWordCount]{};
        keyboardButtonEdges.Apply(
            keyboardSnapshot.gamebtn,
            keyboardTapped,
            keyboardReleased,
            keyboardHeld);
        MergeGameButtons(acceptedInputFrame.gamebtn_tapped, keyboardTapped);
        MergeGameButtons(acceptedInputFrame.gamebtn_released, keyboardReleased);
        MergeGameButtons(providerHeld, keyboardHeld);
        acceptedInputFrame.active = true;
    }
    else
    {
        uint64_t empty[mmio::kGameButtonWordCount]{}, tapped[mmio::kGameButtonWordCount]{},
            released[mmio::kGameButtonWordCount]{}, held[mmio::kGameButtonWordCount]{};
        keyboardButtonEdges.Apply(empty, tapped, released, held);
        MergeGameButtons(acceptedInputFrame.gamebtn_released, released);
        for (const auto word : released) acceptedInputFrame.active |= word != 0;
    }

    if (controllerInputActive)
    {
        MergeGameButtons(
            acceptedInputFrame.gamebtn_tapped,
            controllerFrame.gamebtn_tapped);
        MergeGameButtons(
            acceptedInputFrame.gamebtn_released,
            controllerFrame.gamebtn_released);
        MergeGameButtons(providerHeld, controllerFrame.gamebtn_down);
        acceptedInputFrame.stick_lx = controllerFrame.stick_lx;
        acceptedInputFrame.stick_ly = controllerFrame.stick_ly;
        acceptedInputFrame.stick_rx = controllerFrame.stick_rx;
        acceptedInputFrame.stick_ry = controllerFrame.stick_ry;
        acceptedInputFrame.axes_active = true;
        acceptedInputFrame.active = true;
    }

    uint64_t providerTapped[mmio::kGameButtonWordCount]{};
    uint64_t providerReleased[mmio::kGameButtonWordCount]{};
    uint64_t mergedHeld[mmio::kGameButtonWordCount]{};
    mergedProviderEdges.Apply(
        providerHeld,
        providerTapped,
        providerReleased,
        mergedHeld);
    for (uint32_t word = 0; word < mmio::kGameButtonWordCount; ++word)
    {
        // Source-local taps remain visible so another provider can retrigger a
        // held action. Releases require every provider to be up.
        acceptedInputFrame.gamebtn_tapped[word] |= providerTapped[word];
        acceptedInputFrame.gamebtn_released[word] |= providerReleased[word];
        acceptedInputFrame.gamebtn_released[word] &= ~providerHeld[word];
        acceptedInputFrame.gamebtn_down[word] = mergedHeld[word];
    }

    const auto externalMode = externalActive
        ? static_cast<mmio::Mode>(externalFrame.snapshot.mode)
        : mmio::Mode::None;
    uint8_t externalTouchCells[mmio::kTouchCellCount]{};
    ExpandTouchMask(externalFrame.snapshot.touch_mask, externalTouchCells);
    const bool externalArcadeActive =
        externalMode == mmio::Mode::ArcadeSlider &&
        externalFrame.snapshot.touch_mask != 0;
    const uint32_t externalGamepadSlide = GetExternalSliderDirection(
        externalFrame.snapshot);
    const bool keyboardDirectTouchActive =
        HasMmIoSliderTouch(keyboardFrame.direct_touch_cells);
    const bool controllerDirectTouchActive =
        controllerInputActive && HasMmIoSliderTouch(controllerFrame.touch_cells);
    const auto arcadeSliderFrame = arcadeSliderFrontend.Consume();
    const bool cabinetTouchActive = HasMmIoSliderTouch(arcadeSliderFrame.touch_cells);
    const bool directSliderTouchActive = externalArcadeActive || keyboardDirectTouchActive || controllerDirectTouchActive || cabinetTouchActive;
    acceptedInputFrame.direct_slider_touch_active = directSliderTouchActive;
    const bool frontendDirectionActive =
        (keyboardActive && keyboardFrame.slider_direction != 0) ||
        (controllerInputActive && controllerFrame.gamepad_slide != 0);
    const uint32_t directionHeld = externalGamepadSlide |
        (keyboardActive ? keyboardFrame.slider_direction : 0) |
        (controllerInputActive ? controllerFrame.gamepad_slide : 0);
    acceptedInputFrame.active = acceptedInputFrame.active || arcadeSliderFrame.connected;
    const bool preserveExternalGamepad = externalGamepadSlide != 0 && !directSliderTouchActive && !frontendDirectionActive;
    const MmIoSliderMode effectiveSliderMode = preserveExternalGamepad
        ? MmIoSliderMode::Joystick
        : sliderModeResolver.Resolve(
            directSliderTouchActive,
            directionHeld);
    // ArcadeSlider 有触摸时，依然保留虚拟摇杆方向.
    if (directionHeld != 0 && (externalGamepadSlide != 0 || !controllerInputActive))
        SetSimulatedGamepadAxes(acceptedInputFrame, directionHeld);
    const mmio::Mode outputMode = effectiveSliderMode == MmIoSliderMode::Arcade
        ? mmio::Mode::ArcadeSlider
        : directionHeld != 0 ? mmio::Mode::GamepadDualStick : mmio::Mode::None;

    if (!debugSliderStateInitialized ||
        debugLastSliderMode != effectiveSliderMode ||
        debugLastDirectTouch != directSliderTouchActive ||
        debugLastDirectionHeld != directionHeld)
    {
        DebugLog(
            "Slider decision: configured=%s effective=%s direct-touch=%s direction=0x%X keyboard-direction=0x%X gamepad-direction=0x%X controller-frame=0x%X takeover=%s output=%s",
            SliderModeName(mmIoConfig.slider_mode),
            SliderModeName(effectiveSliderMode),
            directSliderTouchActive ? "yes" : "no",
            directionHeld,
            keyboardActive ? keyboardFrame.slider_direction : 0,
            controllerInputActive ? controllerFrame.gamepad_slide : 0,
            controllerFrame.gamepad_slide,
            controllerTakeoverActive ? "yes" : "no",
            InputModeName(outputMode));
        debugSliderStateInitialized = true;
        debugLastSliderMode = effectiveSliderMode;
        debugLastDirectTouch = directSliderTouchActive;
        debugLastDirectionHeld = directionHeld;
    }

    if (effectiveSliderMode == MmIoSliderMode::Arcade)
    {
        if (externalArcadeActive)
        {
            MergeTouchCells(
                acceptedInputFrame.touch_cells,
                externalTouchCells);
        }
        if (keyboardActive)
        {
            MergeTouchCells(
                acceptedInputFrame.touch_cells,
                keyboardTouchCells);
        }
        if (controllerInputActive)
        {
            MergeTouchCells(
                acceptedInputFrame.touch_cells,
                controllerFrame.touch_cells);
            MergeTouchCells(
                acceptedInputFrame.touch_cells,
                controllerFrame.arcade_touch_cells);
        }
        MergeTouchCells(acceptedInputFrame.touch_cells, arcadeSliderFrame.touch_cells);
        acceptedInputFrame.slider_mode = mmio::Mode::ArcadeSlider;
        acceptedInputFrame.gamepad_slide = 0;
        acceptedInputFrame.active = acceptedInputFrame.active ||
            HasMmIoSliderTouch(acceptedInputFrame.touch_cells);
    }
    else
    {
        acceptedInputFrame.gamepad_slide = directionHeld;
        acceptedInputFrame.slider_mode = directionHeld != 0
            ? mmio::Mode::GamepadDualStick
            : mmio::Mode::None;
        acceptedInputFrame.active = acceptedInputFrame.active || directionHeld != 0;

        uint64_t directionHeldState[mmio::kGameButtonWordCount]{};
        // Menu 中检查26/27/30/31
        directionHeldState[0] = MapGamepadSlide(directionHeld);
        // Gameplay 中检查154/155/158/159作为摇杆方向
        directionHeldState[2] = MapGamepadSlide(directionHeld);
        uint64_t directionTapped[mmio::kGameButtonWordCount]{};
        uint64_t directionReleased[mmio::kGameButtonWordCount]{};
        uint64_t directionDown[mmio::kGameButtonWordCount]{};
        gamepadDirectionEdges.Apply(
            directionHeldState,
            directionTapped,
            directionReleased,
            directionDown);
        MergeGameButtons(acceptedInputFrame.gamebtn_tapped, directionTapped);
        MergeGameButtons(acceptedInputFrame.gamebtn_released, directionReleased);
        MergeGameButtons(acceptedInputFrame.gamebtn_down, directionDown);
    }
    // 用左右两端的触摸当作菜单左右的action.
    ApplyArcadeSliderSwitches(acceptedInputFrame);
    if (effectiveSliderMode == MmIoSliderMode::Arcade)
    {
        gamepadDirectionEdges.Reset();
    }

    if (InputDiagnosticsEnabled())
    {
        bool edge = false;
        for (uint32_t word = 0; word < mmio::kGameButtonWordCount; ++word)
            edge |= (acceptedInputFrame.gamebtn_tapped[word] | acceptedInputFrame.gamebtn_released[word]) != 0;
        if (edge)
        {
            inputDiagnosticWindowEnd = inputDiagnosticFrame + 3;
            inputDiagnosticRecordCount = 0;
            for (uint32_t word = 0; word < mmio::kGameButtonWordCount; ++word)
                DebugLog("InputDiag frame=%llu merged word=%u tapped=%016llX held=%016llX released=%016llX",
                    inputDiagnosticFrame, word, acceptedInputFrame.gamebtn_tapped[word],
                    acceptedInputFrame.gamebtn_down[word], acceptedInputFrame.gamebtn_released[word]);
        }
    }
    acceptedInputFrame.ui_activity_pending = HasPrimaryButtonTap(
        acceptedInputFrame.gamebtn_tapped);
    acceptedInputFrame.exclusive =
        (externalActive || keyboardActive || controllerInputActive) &&
        mmIoConfig.exclusive_controller_input;
    arcadeSliderActive.store(
        acceptedInputFrame.slider_mode == mmio::Mode::ArcadeSlider &&
            HasMmIoSliderTouch(acceptedInputFrame.touch_cells),
        std::memory_order_relaxed);

    const bool previousExclusive = exclusiveControllerActive.exchange(
        acceptedInputFrame.exclusive,
        std::memory_order_relaxed);
    if (previousExclusive != acceptedInputFrame.exclusive)
    {
        Log("MMIO exclusive controller input %s; physical input merge is %s.",
            acceptedInputFrame.exclusive ? "active" : "inactive",
            acceptedInputFrame.exclusive ? "blocked" : "enabled");
    }
}

int32_t NativeAxisValue(float value)
{
    if (!std::isfinite(value))
        return 0;
    return static_cast<int32_t>(std::lround(std::clamp(value, -1.0f, 1.0f) * 1000.0f));
}

// 注入摇杆轴值，NewClassics会需要
void InjectAcceptedGamepadAxes(void* state)
{
    if (!state || !acceptedInputFrame.axes_active)
        return;

    const int32_t axes[4] = {
        NativeAxisValue(acceptedInputFrame.stick_lx),
        NativeAxisValue(acceptedInputFrame.stick_ly),
        NativeAxisValue(acceptedInputFrame.stick_rx),
        NativeAxisValue(acceptedInputFrame.stick_ry),
    };
    auto* analog = reinterpret_cast<int32_t*>(
        static_cast<uint8_t*>(state) + InputAnalogOffset);
    // 游戏用 1000 倍整数保存摇杆轴，GetPosition 会还原成 -1.0 到 1.0。
    analog[LeftStickXAnalogIndex] = axes[0];
    analog[LeftStickYAnalogIndex] = axes[1];
    analog[RightStickXAnalogIndex] = axes[2];
    analog[RightStickYAnalogIndex] = axes[3];

    // Reduce log
    if (acceptedInputFrame.simulated_axes)
    {
        if (!debugInjectedAxesInitialized ||
            !debugLastInjectedAxesSimulated ||
            std::memcmp(debugLastInjectedAxes, axes, sizeof(axes)) != 0)
        {
            DebugLog("Injected gamepad axes: lx=%d ly=%d rx=%d ry=%d simulated=yes", axes[0], axes[1], axes[2], axes[3]);
        }
    }
    else if (debugLastInjectedAxesSimulated)
    {
        DebugLog("Injected gamepad axes: lx=%d ly=%d rx=%d ry=%d simulated=clear", axes[0], axes[1], axes[2], axes[3]);
    }
    else
    {
        return;
    }
    std::memcpy(debugLastInjectedAxes, axes, sizeof(axes));
    debugInjectedAxesInitialized = true;
    debugLastInjectedAxesSimulated = acceptedInputFrame.simulated_axes;
}

void InjectAcceptedInput(void* state)
{
    if (!acceptedInputFrame.active)
    {
        debugInjectedSliderInitialized = false;
        InjectAcceptedGamepadAxes(state);
        return;
    }

    auto* tappedButtons = reinterpret_cast<uint64_t*>(
        static_cast<uint8_t*>(state) + InputTappedButtonsOffset);
    auto* releasedButtons = reinterpret_cast<uint64_t*>(
        static_cast<uint8_t*>(state) + InputReleasedButtonsOffset);
    auto* heldButtons = reinterpret_cast<uint64_t*>(
        static_cast<uint8_t*>(state) + InputHeldButtonsOffset);
    auto* analog = reinterpret_cast<uint32_t*>(
        static_cast<uint8_t*>(state) + InputAnalogOffset);

    if (InputDiagnosticRecord())
        DebugLog("InputDiag frame=%llu inject-before state=%p face-tapped=%llX face-held=%llX face-released=%llX",
            inputDiagnosticFrame, state, tappedButtons[0] & 0x780ull, heldButtons[0] & 0x780ull, releasedButtons[0] & 0x780ull);
    for (uint32_t word = 0; word < mmio::kGameButtonWordCount; ++word)
    {
        tappedButtons[word] |= acceptedInputFrame.gamebtn_tapped[word];
        releasedButtons[word] |= acceptedInputFrame.gamebtn_released[word];
        heldButtons[word] |= acceptedInputFrame.gamebtn_down[word];
    }
    if (InputDiagnosticRecord())
        DebugLog("InputDiag frame=%llu inject-after state=%p face-tapped=%llX face-held=%llX face-released=%llX",
            inputDiagnosticFrame, state, tappedButtons[0] & 0x780ull, heldButtons[0] & 0x780ull, releasedButtons[0] & 0x780ull);
    if (acceptedInputFrame.slider_mode == mmio::Mode::ArcadeSlider)
    {
        const uint32_t touchMask = MapTouchCells(acceptedInputFrame.touch_cells);
        analog[SliderCellsAnalogIndex] |= touchMask;
        if (!debugInjectedSliderInitialized ||
            debugLastInjectedMode != mmio::Mode::ArcadeSlider ||
            debugLastInjectedValue != touchMask)
        {
            DebugLog(
                "Injected slider: mode=ArcadeSlider touch-mask=0x%08X",
                touchMask);
        }
        debugInjectedSliderInitialized = true;
        debugLastInjectedMode = mmio::Mode::ArcadeSlider;
        debugLastInjectedValue = touchMask;
    }
    else if (acceptedInputFrame.slider_mode == mmio::Mode::GamepadDualStick)
    {
        const uint32_t gamepadMask = static_cast<uint32_t>(
            MapGamepadSlide(acceptedInputFrame.gamepad_slide));
        if (!debugInjectedSliderInitialized ||
            debugLastInjectedMode != mmio::Mode::GamepadDualStick ||
            debugLastInjectedValue != gamepadMask)
        {
            DebugLog(
                "Injected slider: mode=GamepadDualStick raw-mask=0x%08X gameplay-word2=0x%016llX slide=0x%X",
                gamepadMask,
                static_cast<unsigned long long>(acceptedInputFrame.gamebtn_down[2]),
                acceptedInputFrame.gamepad_slide);
        }
        debugInjectedSliderInitialized = true;
        debugLastInjectedMode = mmio::Mode::GamepadDualStick;
        debugLastInjectedValue = gamepadMask;
    }
    else
    {
        debugInjectedSliderInitialized = false;
    }

    // 统一在当前输入状态写入完成后注入模拟摇杆轴。
    InjectAcceptedGamepadAxes(state);
}

uintptr_t InputCallerRva(void* caller)
{
    return reinterpret_cast<uintptr_t>(caller) - reinterpret_cast<uintptr_t>(getModuleInfo().lpBaseOfDll);
}

void ObserveUiAction(void* state, uint64_t action, ui_input::Predicate predicate,
    uint32_t result, uintptr_t caller)
{
    if (!predicate || action < 7 || action > 10 || !InputDiagnosticWindow()) return;
    if (InputDiagnosticRecord())
        DebugLog("InputDiag frame=%llu query caller=+%llX state=%p action=%llu predicate=+%llX arcade=%d result=%d native-predicate=unchanged",
            inputDiagnosticFrame, static_cast<unsigned long long>(caller), state, action,
            static_cast<unsigned long long>(InputCallerRva(reinterpret_cast<void*>(predicate))),
            inputDiagnosticArcade, result);
}

// 让游戏显示的图例和实际一致
uint32_t __fastcall InputConfigGetDeviceActionBindingHook(
    uint32_t action,
    uint32_t deviceIndex)
{
    const uint32_t result = originalInputConfigGetDeviceActionBinding(
        action, deviceIndex);
    const uint32_t finalBinding = mmIoConfig.enabled
        ? ResolveVirtualDeviceBinding(virtualControllerType.load(std::memory_order_acquire), action, deviceIndex, result)
        : result;
    if (InputDiagnosticsEnabled() && action >= 7 && action <= 10 && deviceIndex == 0)
    {
        struct BindingRecord { uint32_t original = 0; uint32_t finalValue = 0; uint32_t type = 0; bool seen = false; };
        static thread_local BindingRecord records[4];
        auto& record = records[action - 7];
        const uint32_t type = virtualControllerType.load(std::memory_order_acquire);
        if (!record.seen || record.original != result || record.finalValue != finalBinding || record.type != type)
        {
            DebugLog("InputDiag frame=%llu binding caller=+%llX action=%u device=%u original=%u final=%u virtual-type=%u policy=missing-only",
                inputDiagnosticFrame, static_cast<unsigned long long>(InputCallerRva(_ReturnAddress())),
                action, deviceIndex, result, finalBinding, type);
            record = {result, finalBinding, type, true};
        }
    }
    return finalBinding;
}

uint32_t* __fastcall MenuPromptBindingHook(void* state, uint32_t* output, uint32_t action)
{
    auto* result = originalGetActionBinding(state, output, action);
    if (action >= 7 && action <= 10 && output && InputDiagnosticRecord())
        DebugLog("InputDiag frame=%llu resolved-binding caller=+%llX state=%p action=%u binding=%u",
            inputDiagnosticFrame, static_cast<unsigned long long>(InputCallerRva(_ReturnAddress())), state, action, *output);
    return result;
}

int64_t __fastcall MergeSliderSensorButtonsHook(void* state, void* sensorState)
{
    RefreshAcceptedInputFrame();
    if (acceptedInputFrame.exclusive)
    {
        InjectAcceptedInput(state);
        return 0;
    }

    const int64_t result = originalMergeSliderSensorButtons(state, sensorState);
    InjectAcceptedInput(state);
    return result;
}

int64_t __fastcall MergeConnectedDeviceHook(
    void* state, void* deviceState, uint32_t playerIndex, uint8_t primaryContext)
{
    if (!mmIoConfig.exclusive_controller_input)
        return originalMergeConnectedDevice(state, deviceState, playerIndex, primaryContext);

    const bool allowPointer = mmIoConfig.touchscreen.enabled ||
        (mmIoConfig.show_mouse_with_controller && !mmIoConfig.block_mouse_input);
    if (!allowPointer || playerIndex != 0 || !primaryContext) return 0;

    // Sample native keyboard/mouse into an isolated player-shaped buffer.
    // Only pointer channels enter the unified IO state; native keyboard actions,
    // any-key synthesis and device identity never escape this buffer.
    alignas(16) uint8_t pointerState[PointerPlayerStateSize]{};
    originalMergeConnectedDevice(pointerState, deviceState, playerIndex, primaryContext);
    const bool active = MergeExclusivePointerState(state, pointerState);
    if (InputDiagnosticsEnabled() && active && InputDiagnosticRecord())
    {
        uint64_t tapped = 0, held = 0, released = 0;
        int32_t x = 0, y = 0;
        std::memcpy(&tapped, pointerState + 0x10, 8);
        std::memcpy(&released, pointerState + 0x28, 8);
        std::memcpy(&held, pointerState + 0x40, 8);
        std::memcpy(&x, pointerState + PointerAnalogOffset, 4);
        std::memcpy(&y, pointerState + PointerAnalogOffset + sizeof(x), 4);
        DebugLog("PointerMerge exclusive=1 raw145 tap=%d held=%d release=%d xy=%d,%d identity=virtual",
            !!(tapped & PointerLeftButtonMask), !!(held & PointerLeftButtonMask),
            !!(released & PointerLeftButtonMask), x, y);
    }
    return active ? 1 : 0;
}

char __fastcall DirectInputDevicePollStateHook(void* device, void* deviceState)
{
    if (!mmIoConfig.exclusive_controller_input)
    {
        return originalDirectInputDevicePollState(device, deviceState);
    }

    if (deviceState && resetInputState)
    {
        // PollState normally performs this reset before reading the physical device.
        resetInputState(static_cast<uint8_t*>(deviceState) + 0x20);
    }
    return 0;
}

// 设定当前的控制器类型
int64_t __fastcall MergeSelectedDeviceHook(
    void* state,
    void* deviceState,
    uint32_t playerIndex,
    uint32_t* selectedDeviceType)
{
    // 游戏原本的控制器类型
    const int64_t result = originalMergeSelectedDevice(
        state, deviceState, playerIndex, selectedDeviceType);

    if (playerIndex == 0)
    {
        InjectAcceptedGamepadAxes(state);
    }

    // 刚才是否通过Mod按下过按键
    const bool uiActivityPending = acceptedInputFrame.ui_activity_pending;
    acceptedInputFrame.ui_activity_pending = false;
    if (uiActivityPending)
    {
        static_cast<uint8_t*>(state)[InputSelectedDevicePresentOffset] = 1;
    }

    const uint32_t nativeType = selectedDeviceType
        ? *selectedDeviceType
        : NoControllerType;
    bool useVirtualController = false;
    if (mmIoConfig.exclusive_controller_input)
    {
        // 独占输入时，不再使用游戏自己的控制器类型。
        useVirtualController = true;
    }
    if (acceptedInputFrame.active)
    {
        // AnySlider 当前帧有输入时，使用它对应的虚拟控制器布局。
        useVirtualController = true;
    }
    if (nativeType == NoControllerType)
    {
        // 游戏没有设备时，仍需要给输入状态一个明确的控制器类型。
        useVirtualController = true;
    }

    if (playerIndex == 0 && InputDiagnosticsEnabled())
    {
        const uint32_t effectiveType = useVirtualController
            ? virtualControllerType.load(std::memory_order_acquire) : nativeType;
        static thread_local uint32_t lastNative = UINT32_MAX, lastEffective = UINT32_MAX;
        if (lastNative != nativeType || lastEffective != effectiveType || InputDiagnosticWindow())
        {
            if (lastNative != nativeType || lastEffective != effectiveType || InputDiagnosticRecord())
                DebugLog("InputDiag frame=%llu device native-type=%u native-result=%lld effective-type=%u exclusive=%d virtual-active=%d state-type=%u",
                    inputDiagnosticFrame, nativeType, result, effectiveType, mmIoConfig.exclusive_controller_input,
                    acceptedInputFrame.active, *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(state) + 0x2E8));
            lastNative = nativeType; lastEffective = effectiveType;
        }
    }
    if (playerIndex == 0 && useVirtualController)
    {
        const uint32_t controllerType = virtualControllerType.load(std::memory_order_acquire);
        if (selectedDeviceType)
        {
            *selectedDeviceType = controllerType;
        }
        return controllerType;
    }
    if (playerIndex == 0 && result == NoControllerType && nativeType != NoControllerType)
    {
        // 特殊情况，防止11类型返回，游戏会认为没有任何设备
        return nativeType;
    }
    return result;
}

// Only called behind the register-preserving assembly entry.
uint32_t EvaluateArcadeControllerEnabled()
{
    const bool native = originalIsArcadeControllerEnabled();
    const bool slider = arcadeSliderActive.load(std::memory_order_relaxed);
    const bool effective = InputDiagnosticsEnabled() && mmIoConfig.input_diagnostics_arcade != -1
        ? mmIoConfig.input_diagnostics_arcade != 0 : native || slider;
    if (InputDiagnosticsEnabled())
    {
        static thread_local int lastNative = -1, lastEffective = -1, lastSlider = -1;
        inputDiagnosticArcade = effective;
        if (lastNative != static_cast<int>(native) ||
            lastEffective != static_cast<int>(effective) ||
            lastSlider != static_cast<int>(slider))
        {
            DebugLog("InputDiag frame=%llu arcade native=%d slider=%d effective=%d override=%d",
                inputDiagnosticFrame, native, slider, effective, mmIoConfig.input_diagnostics_arcade);
            lastNative = native; lastEffective = effective; lastSlider = slider;
        }
    }
    return effective ? 1u : 0u;
}

bool PrepareMenuPromptHook()
{
    if (!InputDiagnosticsEnabled())
        return true;
    const auto& module = getModuleInfo();
    const auto* base = static_cast<const uint8_t*>(module.lpBaseOfDll);
    constexpr uint8_t getter[] = {0x48,0x83,0xEC,0x38,0x45,0x8B,0xC8,0x4C,0x8D,0x81,0xF0,0x02,0x00,0x00};
    if (module.SizeOfImage != 0x23E73000)
        return false;
    bool valid = std::memcmp(base + 0x2AAF30, getter, sizeof(getter)) == 0;
    for (const uintptr_t rva : {0x60924B, 0x609454, 0x60A97F})
    {
        int32_t displacement = 0;
        std::memcpy(&displacement, base + rva + 1, sizeof(displacement));
        valid = valid && base[rva] == 0xE8 &&
            static_cast<int64_t>(rva + 5) + displacement == 0x2AAF30;
    }
    if (!valid)
    {
        return false;
    }
    originalGetActionBinding = reinterpret_cast<GetActionBinding>(
        const_cast<uint8_t*>(base) + 0x2AAF30);
    return true;
}

bool InstallDetours()
{
    constexpr char sliderMergeBytes[] =
        "\x48\x89\x5C\x24\x00\x57\x48\x83\xEC\x20\x48\x8B\xDA\x48\x8B\xF9\xE8\x00\x00\x00\x00\x4C\x8B\x1A";
    constexpr char sliderMergeMask[] = "xxxx?xxxxxxxxxxxx????xxx";
    constexpr char connectedMergeBytes[] =
        "\x40\x53\x48\x83\xEC\x20\x4C\x8B\xDA\x4C\x8B\xD1";
    constexpr char connectedMergeMask[] = "xxxxxxxxxxxx";
    constexpr char directInputPollBytes[] =
        "\x48\x8B\xC4\x55\x56\x57\x41\x56\x41\x57\x48\x8D\xA8\x00\x00\x00\x00\x48\x81\xEC\xB0\x01\x00\x00";
    constexpr char directInputPollMask[] = "xxxxxxxxxxxxx????xxxxxxx";
    constexpr char resetInputStateBytes[] =
        "\x33\xD2\x33\xC0\x0F\x57\xC0\x0F\x11\x01\x0F\x11\x41\x10\x0F\x11\x41\x20\x0F\x11\x41\x30\x48\x89\x41\x40";
    constexpr char resetInputStateMask[] = "xxxxxxxxxxxxxxxxxxxxxxxxxx";
    constexpr char inputConfigBindingBytes[] =
        "\x48\x83\x3D\x00\x00\x00\x00\x00\x74\x00\x44\x8B\xC2\x8B\xD1\xE9\x00\x00\x00\x00\xB8\xFF\xFF\xFF\xFF\xC3";
    constexpr char inputConfigBindingMask[] = "xxx????xx?xxxxxx????xxxxxx";
    constexpr char selectedMergeBytes[] =
        "\x48\x89\x5C\x24\x00\x48\x89\x74\x24\x00\x48\x89\x7C\x24\x00\x41\x56\x48\x83\xEC\x20\x48\x8B\xFA";
    constexpr char selectedMergeMask[] = "xxxx?xxxx?xxxx?xxxxxxxxx";
    constexpr char arcadeBytes[] =
        "\x48\x83\xEC\x28\xE8\x00\x00\x00\x00\x0F\xB6\x40";
    constexpr char arcadeMask[] = "xxxxx????xxx";
    const bool installSelectedDeviceHook = true;
    const bool installInputConfigBindingFallbackHook = true;
    // Shared-memory producers may publish ArcadeSlider without a built-in frontend.
    const bool installArcadeControllerHook = true;

    originalMergeSliderSensorButtons = reinterpret_cast<MergeSliderSensorButtons>(
        FindSignature(sliderMergeBytes, sliderMergeMask));
    if (installInputConfigBindingFallbackHook)
    {
        originalInputConfigGetDeviceActionBinding =
            reinterpret_cast<InputConfigGetDeviceActionBinding>(
                FindSignature(inputConfigBindingBytes, inputConfigBindingMask));
    }
    if (installArcadeControllerHook)
    {
        originalIsArcadeControllerEnabled = reinterpret_cast<IsArcadeControllerEnabled>(
            FindSignature(arcadeBytes, arcadeMask));
    }
    if (!originalMergeSliderSensorButtons ||
        (installInputConfigBindingFallbackHook &&
            !originalInputConfigGetDeviceActionBinding) ||
        (installArcadeControllerHook && !originalIsArcadeControllerEnabled))
    {
        Log("MMIO core input signatures were not found; slider=%s binding-fallback=%s action-query=%s arcade-query=%s; no MMIO hooks were installed.",
            originalMergeSliderSensorButtons ? "ok" : "missing",
            installInputConfigBindingFallbackHook
                ? (originalInputConfigGetDeviceActionBinding ? "ok" : "missing")
                : "disabled",
            "separate-ui-module",
            originalIsArcadeControllerEnabled ? "ok" : "missing");
        return false;
    }

    if (installSelectedDeviceHook)
    {
        originalMergeSelectedDevice = reinterpret_cast<MergeSelectedDevice>(
            FindSignature(selectedMergeBytes, selectedMergeMask));
        if (!originalMergeSelectedDevice)
        {
            Log("MMIO selected-device signature was not found.");
            return false;
        }
    }
    if (mmIoConfig.exclusive_controller_input)
    {
        originalMergeConnectedDevice = reinterpret_cast<MergeConnectedDevice>(
            FindSignature(connectedMergeBytes, connectedMergeMask));
        originalDirectInputDevicePollState = reinterpret_cast<DirectInputDevicePollState>(
            FindSignature(directInputPollBytes, directInputPollMask));
        resetInputState = reinterpret_cast<ResetInputState>(
            FindSignature(resetInputStateBytes, resetInputStateMask));
        if (!originalMergeConnectedDevice ||
            !originalDirectInputDevicePollState ||
            !resetInputState)
        {
            Log("MMIO exclusive controller signatures were not found.");
            return false;
        }
    }

    originalMouseDeviceGate = nullptr;
    if (mmIoConfig.show_mouse_with_controller)
    {
        const MODULEINFO& module = getModuleInfo();
        constexpr uint8_t gateBytes[] = {0x48,0x83,0x3d,0xa0,0x70,0xe9,0x00,0x00,0x74,0x08,0x0f,0xb6,0xd1};
        if (module.SizeOfImage != 0x23E73000 ||
            std::memcmp(static_cast<uint8_t*>(module.lpBaseOfDll) + 0x2B1140,
                gateBytes, sizeof(gateBytes)) != 0)
        {
            Log("Optional mouse/controller gate: unsupported build; mouse override disabled; core IO unchanged.");
        }
        else
        {
            originalMouseDeviceGate = reinterpret_cast<MouseDeviceGateFn>(
                static_cast<uint8_t*>(module.lpBaseOfDll) + 0x2B1140);
        }
    }
    // Isolation build: diagnostics must not detour the shared binding getter.
    // Keep action tracing and menu policy unchanged to isolate this observer.
    originalGetActionBinding = nullptr;
    if (InputDiagnosticsEnabled())
        Log("Input diagnostics isolation: shared binding/prompt observer NOT installed.");

    LONG error = DetourTransactionBegin();
    if (error != NO_ERROR)
    {
        Log("Could not begin MMIO installation: %ld", error);
        return false;
    }
    error = DetourUpdateThread(GetCurrentThread());
    if (error == NO_ERROR)
    {
        error = DetourAttach(
            reinterpret_cast<void**>(&originalMergeSliderSensorButtons),
            reinterpret_cast<void*>(MergeSliderSensorButtonsHook));
    }
    if (error == NO_ERROR && installInputConfigBindingFallbackHook)
    {
        error = DetourAttach(
            reinterpret_cast<void**>(&originalInputConfigGetDeviceActionBinding),
            reinterpret_cast<void*>(InputConfigGetDeviceActionBindingHook));
    }
    if (error == NO_ERROR && installSelectedDeviceHook)
    {
        error = DetourAttach(
            reinterpret_cast<void**>(&originalMergeSelectedDevice),
            reinterpret_cast<void*>(MergeSelectedDeviceHook));
    }
    if (error == NO_ERROR && mmIoConfig.exclusive_controller_input)
    {
        error = DetourAttach(
            reinterpret_cast<void**>(&originalMergeConnectedDevice),
            reinterpret_cast<void*>(MergeConnectedDeviceHook));
    }
    if (error == NO_ERROR && mmIoConfig.exclusive_controller_input)
    {
        error = DetourAttach(
            reinterpret_cast<void**>(&originalDirectInputDevicePollState),
            reinterpret_cast<void*>(DirectInputDevicePollStateHook));
    }
    if (error == NO_ERROR && installArcadeControllerHook)
    {
        error = DetourAttach(
            reinterpret_cast<void**>(&originalIsArcadeControllerEnabled),
            reinterpret_cast<void*>(IsArcadeControllerEnabledHook));
    }
    if (error == NO_ERROR)
    {
        error = DetourTransactionCommit();
    }
    else
    {
        DetourTransactionAbort();
    }

    if (error != NO_ERROR)
    {
        Log("Could not install MMIO hooks: %ld", error);
        return false;
    }

    installed = true;
    // Optional hooks each get their own transaction AFTER core installation.
    // Even attach/commit failure cannot roll back working core input.
    auto installOptional = [](void** original, void* hook, const char* name) {
        LONG optionalError = DetourTransactionBegin();
        if (optionalError != NO_ERROR)
        {
            Log("Optional %s transaction unavailable: %ld; core IO unchanged.", name, optionalError);
            return false;
        }
        optionalError = DetourUpdateThread(GetCurrentThread());
        if (optionalError == NO_ERROR) optionalError = DetourAttach(original, hook);
        if (optionalError == NO_ERROR) optionalError = DetourTransactionCommit();
        else DetourTransactionAbort();
        if (optionalError != NO_ERROR)
            Log("Optional %s installation failed: %ld; core IO unchanged.", name, optionalError);
        return optionalError == NO_ERROR;
    };
    mouseDeviceGateInstalled = originalMouseDeviceGate && installOptional(
        reinterpret_cast<void**>(&originalMouseDeviceGate),
        reinterpret_cast<void*>(MouseDeviceGateHook), "mouse/controller gate");
    const bool uiPolicyInstalled = ui_input::Install({mmIoConfig.use_ps_circle_confirm,
        InputDiagnosticsEnabled() ? ObserveUiAction : nullptr});
    if (!uiPolicyInstalled) {
        Log("Optional UI input policy unavailable; core IO remains active.");
        // A rejected migration must not disable independent Action diagnostics.
        if (!ui_input::IsInstalled() && InputDiagnosticsEnabled())
            ui_input::Install({false, ObserveUiAction});
    }
    if (mmIoConfig.use_ps_circle_confirm && mmIoConfig.swap_ox_sprite &&
        uiPolicyInstalled && !ui_aet::Install())
        Log("Optional PS AET replacement unavailable; input policy remains active.");
    if (originalGetActionBinding && !installOptional(
        reinterpret_cast<void**>(&originalGetActionBinding),
        reinterpret_cast<void*>(MenuPromptBindingHook), "binding observer"))
        originalGetActionBinding = nullptr;
    selectedDeviceHookInstalled = installSelectedDeviceHook;
    arcadeControllerHookInstalled = installArcadeControllerHook;
    exclusiveControllerHooksInstalled = mmIoConfig.exclusive_controller_input;
    directInputHooksInstalled = mmIoConfig.exclusive_controller_input;
    inputConfigBindingFallbackHookInstalled = installInputConfigBindingFallbackHook;
    return true;
}
}

// External C linkage for the MASM bridge; keep this outside the anonymous namespace.
extern "C" uint32_t IsArcadeControllerEnabledHookImpl()
{
    return EvaluateArcadeControllerEnabled();
}

bool InitializeMmIoHooks(const MmIoConfig& config)
{
    if (installed) return true;
    if (!config.enabled)
    {
        return true;
    }

    mmIoConfig = config;
    inputDiagnosticsEnabled.store(config.input_diagnostics && IsDebugLoggingEnabled());
    inputDiagnosticFrame = inputDiagnosticWindowEnd = 0;
    Log("Input diagnostics=%s type-override=%d arcade-override=%d; binding fallback is always unswapped; action tracing is pass-through",
        InputDiagnosticsEnabled() ? "on" : "off",
        config.controller_type, config.input_diagnostics_arcade);
    const uint32_t initialType = config.controller_type != -1
        ? static_cast<uint32_t>(config.controller_type) : DualSenseControllerType;
    virtualControllerType.store(initialType, std::memory_order_release);
    debugSliderStateInitialized = false;
    debugInjectedSliderInitialized = false;
    debugInjectedAxesInitialized = false;
    debugLastInjectedAxesSimulated = false;
    debugSharedMemorySnapshotInitialized = false;
    arcadeSliderSwitchEdges.Reset();
    sliderModeResolver.Initialize(config.slider_mode);
    joyShockFrontend.Initialize(
        config.gamepad,
        config.slider_mode,
        config.arcade_slider_emu_cells_per_second);
    keyboardFrontend.Initialize(
        config.keyboard_mouse_frontend,
        config.arcade_slider_emu_cells_per_second,
        config.input_bindings,
        config.mouse_slider,
        config.slider_mode);
    if (config.keyboard_mouse_frontend &&
        !(config.mouse_slider.enabled
            ? InitializeMmIoRawInput(config.mouse_slider, true)
            : InitializeMmIoRawKeyboard()))
    {
        Log("Raw Input receiver unavailable; disabling keyboard/mouse frontend.");
        keyboardFrontend.DisableMouseSlider();
    }
    if (!mmIoConsumer.Initialize(
            config.shared_memory_name,
            mmio::SupportsArcadeSlider | mmio::SupportsGamepadDualStick,
            config.max_input_lease_ms))
    {
        joyShockFrontend.Shutdown();
        ShutdownMmIoRawInput();
        sliderModeResolver.Reset();
        return false;
    }

    if (!InitializeMmGameState(mmIoConsumer))
        Log("Native game-state reader is unavailable; state will remain Unknown.");

    if (!InstallDetours())
    {
        joyShockFrontend.Shutdown();
        sliderModeResolver.Reset();
        ShutdownMmGameState();
        mmIoConsumer.Shutdown();
        ShutdownMmIoRawInput();
        return false;
    }

    if (!InitializeMmLedHooks(config.shared_memory_name, [](const mmio::LedOutput& frame) {
            io3Frontend.SetLeds(frame.io3_bitmap);
            arcadeSliderFrontend.SetLeds(frame.slider);
        }, false)) {
        Log("LED capture unavailable; Arcade LED restoration skipped; input remains enabled.");
    } else {
        InitializeArcadeLed(config.arcade_led, config.io3.led_use_hardware_position
            ? HardwareLampReader([](uint32_t& held) { return io3Frontend.ReadHardwareLampHeld(held); })
            : HardwareLampReader{});
        StartLedDiagnostics(config.led_diagnostics);
    }
    arcadeSliderFrontend.Initialize(config.arcade_slider);
    io3Frontend.Initialize(config.io3);
    Log("IO3 backend %s (AMEX native driver)", config.io3.enabled ? "enabled" : "disabled");
    Log("MMIO backend ready: mapping=%ls lease=%llu ms exclusive=%s keyboard-mouse=%s mouse-slider=%s gamepad=%s selected-device-hook=%s directinput-poll-hooks=%s binding-fallback-hook=%s action-query-hook=%s arcade-query-hook=%s",
        config.shared_memory_name.c_str(),
        config.max_input_lease_ms,
        config.exclusive_controller_input ? "true" : "false",
        config.keyboard_mouse_frontend ? "true" : "false",
        keyboardFrontend.IsMouseSliderEnabled() ? "true" : "false",
        joyShockFrontend.IsEnabled() ? "true" : "false",
        selectedDeviceHookInstalled ? "true" : "false",
        directInputHooksInstalled ? "true" : "false",
        inputConfigBindingFallbackHookInstalled ? "true" : "false",
        ui_input::IsInstalled() ? "true" : "false",
        arcadeControllerHookInstalled ? "true" : "false");
    return true;
}

void ShutdownMmIoHooks()
{
    if (!ui_aet::Uninstall()) {
        Log("PS AET detach failed; MMIO callbacks/resources retained.");
        return;
    }
    if (!ui_input::Uninstall()) {
        Log("UI input detach failed; MMIO callbacks/resources retained.");
        return;
    }
    // Detach before shutting down resources used by the input callbacks.
    // On any failure keep installation flags, trampolines and frontends alive.
    if (installed)
    {
        LONG error = DetourTransactionBegin();
        if (error != NO_ERROR)
        {
            Log("Could not begin MMIO detach: %ld; hooks/resources retained.", error);
            return;
        }
        error = DetourUpdateThread(GetCurrentThread());
        auto detach = [&error](void** original, void* hook) {
            if (error == NO_ERROR) error = DetourDetach(original, hook);
        };
        detach(reinterpret_cast<void**>(&originalMergeSliderSensorButtons), reinterpret_cast<void*>(MergeSliderSensorButtonsHook));
        if (mouseDeviceGateInstalled) detach(reinterpret_cast<void**>(&originalMouseDeviceGate), reinterpret_cast<void*>(MouseDeviceGateHook));
        if (inputConfigBindingFallbackHookInstalled) detach(reinterpret_cast<void**>(&originalInputConfigGetDeviceActionBinding), reinterpret_cast<void*>(InputConfigGetDeviceActionBindingHook));
        if (originalGetActionBinding) detach(reinterpret_cast<void**>(&originalGetActionBinding), reinterpret_cast<void*>(MenuPromptBindingHook));
        if (selectedDeviceHookInstalled) detach(reinterpret_cast<void**>(&originalMergeSelectedDevice), reinterpret_cast<void*>(MergeSelectedDeviceHook));
        if (exclusiveControllerHooksInstalled) detach(reinterpret_cast<void**>(&originalMergeConnectedDevice), reinterpret_cast<void*>(MergeConnectedDeviceHook));
        if (directInputHooksInstalled) detach(reinterpret_cast<void**>(&originalDirectInputDevicePollState), reinterpret_cast<void*>(DirectInputDevicePollStateHook));
        if (arcadeControllerHookInstalled) detach(reinterpret_cast<void**>(&originalIsArcadeControllerEnabled), reinterpret_cast<void*>(IsArcadeControllerEnabledHook));
        if (error == NO_ERROR) error = DetourTransactionCommit();
        else DetourTransactionAbort();
        if (error != NO_ERROR)
        {
            Log("Could not detach MMIO: %ld; hooks/resources retained.", error);
            return;
        }
    }
    inputDiagnosticsEnabled.store(false);
    ShutdownMmLedHooks();
    ShutdownMmGameState();
    arcadeSliderActive.store(false, std::memory_order_relaxed);
    exclusiveControllerActive.store(false, std::memory_order_relaxed);
    virtualControllerType.store(DualSenseControllerType, std::memory_order_release);
    acceptedInputFrame = {};
    debugSliderStateInitialized = false;
    debugInjectedSliderInitialized = false;
    debugInjectedAxesInitialized = false;
    debugLastInjectedAxesSimulated = false;
    debugSharedMemorySnapshotInitialized = false;
    arcadeSliderSwitchEdges.Reset();
    arcadeSliderFrontend.Shutdown();
    io3Frontend.Shutdown();
    joyShockFrontend.Shutdown();
    sliderModeResolver.Reset();
    mmIoConsumer.Shutdown();
    ShutdownMmIoRawInput();
    keyboardButtonEdges.Reset();
    mergedProviderEdges.Reset();
    gamepadDirectionEdges.Reset();
    selectedDeviceHookInstalled = false;
    exclusiveControllerHooksInstalled = false;
    directInputHooksInstalled = false;
    inputConfigBindingFallbackHookInstalled = false;
    arcadeControllerHookInstalled = false;
    originalGetActionBinding = nullptr;
    mouseDeviceGateInstalled = false;
    originalMouseDeviceGate = nullptr;
    installed = false;
}
}
