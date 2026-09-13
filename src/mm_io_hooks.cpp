#include "pch.h"

#include "mm_io_hooks.h"

#include "Dependencies/Signature.h"
#include "anyslider_log.h"
#include "mm_io_keyboard.h"
#include "mm_io_joyshock.h"
#include "mm_io_raw_input.h"
#include "mm_io/shared_memory.h"
#include "mm_io_shared_memory.h"

#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace anyslider
{
namespace
{
constexpr bool EnableInputCheckActionFallback = true;

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

constexpr uint32_t DualSenseControllerType = 3;
constexpr uint32_t DualShock4ControllerType = 2;
constexpr uint32_t NintendoControllerType = 4;
constexpr uint32_t NoControllerType = 11;

using MergeSliderSensorButtons = int64_t(__fastcall*)(void* state, void* sensorState);
using MergeConnectedDevice = int64_t(__fastcall*)(void* state, void* deviceState, uint32_t playerIndex);
using MergeSelectedDevice = int64_t(__fastcall*)(void* state, void* deviceState, uint32_t playerIndex, uint32_t* selectedDeviceType);
using DirectInputDevicePollState = char(__fastcall*)(void* device, void* deviceState);
using ResetInputState = int64_t(__fastcall*)(void* state);
using InputConfigGetDeviceActionBinding = uint32_t(__fastcall*)(uint32_t action, uint32_t deviceIndex);
using InputActionPredicate = int64_t(__fastcall*)(void* state, uint64_t action);
using InputCheckAction = char(__fastcall*)(void* state, uint64_t action, InputActionPredicate predicate);
using IsArcadeControllerEnabled = bool(__fastcall*)();

MergeSliderSensorButtons originalMergeSliderSensorButtons = nullptr;
MergeConnectedDevice originalMergeConnectedDevice = nullptr;
MergeSelectedDevice originalMergeSelectedDevice = nullptr;
DirectInputDevicePollState originalDirectInputDevicePollState = nullptr;
ResetInputState resetInputState = nullptr;
InputConfigGetDeviceActionBinding originalInputConfigGetDeviceActionBinding = nullptr;
InputCheckAction originalInputCheckAction = nullptr;
IsArcadeControllerEnabled originalIsArcadeControllerEnabled = nullptr;

MmIoConfig mmIoConfig;
MmIoConsumer mmIoConsumer;
MmIoKeyboardMouseFrontend keyboardFrontend;
MmIoJoyShockFrontend joyShockFrontend;
MmIoSliderModeResolver sliderModeResolver;
std::atomic_bool arcadeSliderActive = false;
std::atomic_bool exclusiveControllerActive = false;
std::atomic_uint32_t virtualControllerType = DualSenseControllerType;
bool installed = false;
bool selectedDeviceHookInstalled = false;
bool exclusiveControllerHooksInstalled = false;
bool directInputHooksInstalled = false;
bool inputConfigBindingFallbackHookInstalled = false;
bool inputCheckActionHookInstalled = false;
bool arcadeControllerHookInstalled = false;
thread_local bool debugSliderStateInitialized = false;
thread_local MmIoSliderMode debugLastSliderMode = MmIoSliderMode::Arcade;
thread_local bool debugLastDirectTouch = false;
thread_local uint32_t debugLastDirectionHeld = 0;
thread_local bool debugInjectedSliderInitialized = false;
thread_local mmio::Mode debugLastInjectedMode = mmio::Mode::None;
thread_local uint32_t debugLastInjectedValue = 0;
thread_local bool debugLogicalActionInitialized = false;
thread_local uint32_t debugLastLogicalAction = 0;
thread_local uint32_t debugLastLogicalRawAction = 0;
thread_local bool debugLastLogicalResult = false;

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

void MergeGameButtons(
    uint64_t (&destination)[mmio::kGameButtonWordCount],
    const uint64_t (&source)[mmio::kGameButtonWordCount])
{
    for (uint32_t word = 0; word < mmio::kGameButtonWordCount; ++word)
    {
        destination[word] |= source[word];
    }
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

uint32_t MapGamepadLogicalAction(uint64_t action)
{
    switch (action)
    {
    // 游戏菜单的垂直 action 顺序与 shared-memory ABI 的名称相反。
    case mmio::DpadUp: return 25;
    case mmio::DpadDown: return 24;
    case mmio::DpadLeft: return 26;
    case mmio::DpadRight: return 27;
    default: return UINT32_MAX;
    }
}

// PS布局
uint32_t MapDualSenseBindingIndex(uint32_t action)
{
    switch (action)
    {
    case 3: return 0; // D-pad Up
    case 4: return 2; // D-pad Down
    case 5: return 1; // D-pad Left
    case 6: return 3; // D-pad Right
    case 7: return 5; // Square / Xbox X
    case 8: return 4; // Triangle / Xbox Y
    case 9: return 7; // Circle / Xbox B
    case 10: return 6; // Cross / Xbox A
    case 11: return 8; // L1 / LB
    case 12: return 9; // R1 / RB
    case 13: return 10; // L2 / LT
    case 14: return 11; // R2 / RT
    case 15: return 12; // Select / Share
    case 16: return 13; // L3 / left-stick click
    case 17: return 14; // R3 / right-stick click
    case 160: return 15; // Pause / Options
    default: return UINT32_MAX;
    }
}

// 任天堂布局
uint32_t MapNintendoBindingIndex(uint32_t action)
{
    switch (action)
    {
    case 7: return 4; // Square / Xbox X / Nintendo X
    case 8: return 5; // Triangle / Xbox Y / Nintendo Y
    case 9: return 6; // Circle / Xbox B / Nintendo B
    case 10: return 7; // Cross / Xbox A / Nintendo A
    default: return MapDualSenseBindingIndex(action);
    }
}

uint32_t MapControllerBindingIndex(
    MmIoControllerType controllerType,
    uint32_t action)
{
    return controllerType == MmIoControllerType::Nintendo
        ? MapNintendoBindingIndex(action)
        : MapDualSenseBindingIndex(action);
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

MmIoControllerType ControllerLayoutFromNativeType(uint32_t controllerType)
{
    return controllerType == NintendoControllerType
        ? MmIoControllerType::Nintendo
        : controllerType == DualShock4ControllerType
            ? MmIoControllerType::DualShock4
            : MmIoControllerType::DualSense;
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
    { mmio::Select, "Select" },
    { mmio::L3, "L3" },
    { mmio::R3, "R3" },
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

void RefreshAcceptedInputFrame()
{
    MmIoConsumer::InputFrame externalFrame{};
    const bool externalFrameAvailable = mmIoConsumer.ReadFrame(
        externalFrame, mmIoConfig.max_input_lease_ms);
    const bool externalActive = externalFrame.source_active;
    const MmIoKeyboardFrame keyboardFrame = keyboardFrontend.Poll();
    const mmio::InputSnapshot& keyboardSnapshot = keyboardFrame.snapshot;
    uint8_t keyboardTouchCells[mmio::kTouchCellCount]{};
    ExpandTouchMask(keyboardSnapshot.touch_mask, keyboardTouchCells);
    const bool keyboardActive = keyboardFrontend.IsEnabled();
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
    virtualControllerType.store(
        controllerInputActive
            ? NativeControllerType(controllerFrame.controller_type)
            : DualSenseControllerType,
        std::memory_order_release);

    acceptedInputFrame = {};
    uint64_t providerHeld[mmio::kGameButtonWordCount]{};
    if (externalFrameAvailable)
    {
        if (IsDebugLoggingEnabled())
        {
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
        keyboardButtonEdges.Reset();
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
    const bool frontendDirectionActive =
        (keyboardActive && keyboardFrame.slider_direction != 0) ||
        (controllerInputActive && controllerFrame.gamepad_slide != 0);
    const uint32_t directionHeld = externalGamepadSlide |
        (keyboardActive ? keyboardFrame.slider_direction : 0) |
        (controllerInputActive ? controllerFrame.gamepad_slide : 0);
    if (!controllerInputActive && directionHeld != 0)
    {
        // 键盘和 shared memory 只有方向，先转换成一次完整的虚拟摇杆轴。
        SetSimulatedGamepadAxes(acceptedInputFrame, directionHeld);
    }
    const bool directTouchActive = externalArcadeActive ||
        keyboardDirectTouchActive || controllerDirectTouchActive;
    const bool preserveExternalGamepad =
        externalGamepadSlide != 0 && !keyboardDirectTouchActive &&
        !controllerDirectTouchActive && !frontendDirectionActive;
    const MmIoSliderMode effectiveSliderMode = preserveExternalGamepad
        ? MmIoSliderMode::Joystick
        : sliderModeResolver.Resolve(
            directTouchActive,
            directionHeld);
    const mmio::Mode outputMode = effectiveSliderMode == MmIoSliderMode::Arcade
        ? mmio::Mode::ArcadeSlider
        : directionHeld != 0 ? mmio::Mode::GamepadDualStick : mmio::Mode::None;

    if (!debugSliderStateInitialized ||
        debugLastSliderMode != effectiveSliderMode ||
        debugLastDirectTouch != directTouchActive ||
        debugLastDirectionHeld != directionHeld)
    {
        DebugLog(
            "Slider decision: configured=%s effective=%s direct-touch=%s direction=0x%X keyboard-direction=0x%X gamepad-direction=0x%X controller-frame=0x%X takeover=%s output=%s",
            SliderModeName(mmIoConfig.slider_mode),
            SliderModeName(effectiveSliderMode),
            directTouchActive ? "yes" : "no",
            directionHeld,
            keyboardActive ? keyboardFrame.slider_direction : 0,
            controllerInputActive ? controllerFrame.gamepad_slide : 0,
            controllerFrame.gamepad_slide,
            controllerTakeoverActive ? "yes" : "no",
            InputModeName(outputMode));
        debugSliderStateInitialized = true;
        debugLastSliderMode = effectiveSliderMode;
        debugLastDirectTouch = directTouchActive;
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
    if (effectiveSliderMode == MmIoSliderMode::Arcade)
    {
        gamepadDirectionEdges.Reset();
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

void InjectAcceptedInput(void* state)
{
    if (!acceptedInputFrame.active)
    {
        debugInjectedSliderInitialized = false;
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

    for (uint32_t word = 0; word < mmio::kGameButtonWordCount; ++word)
    {
        tappedButtons[word] |= acceptedInputFrame.gamebtn_tapped[word];
        releasedButtons[word] |= acceptedInputFrame.gamebtn_released[word];
        heldButtons[word] |= acceptedInputFrame.gamebtn_down[word];
    }
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
}

int32_t NativeAxisValue(float value)
{
    if (!std::isfinite(value))
        return 0;
    return static_cast<int32_t>(std::lround(
        std::clamp(value, -1.0f, 1.0f) * 1000.0f));
}

// 注入摇杆轴值，NewClassics会需要
void InjectAcceptedGamepadAxes(void* state)
{
    if (!state || !acceptedInputFrame.axes_active)
        return;

    auto* analog = reinterpret_cast<int32_t*>(static_cast<uint8_t*>(state) + InputAnalogOffset);
    // 游戏用 1000 倍整数保存摇杆轴，GetPosition 会还原成 -1.0 到 1.0。
    analog[LeftStickXAnalogIndex] = NativeAxisValue(acceptedInputFrame.stick_lx);
    analog[LeftStickYAnalogIndex] = NativeAxisValue(acceptedInputFrame.stick_ly);
    analog[RightStickXAnalogIndex] = NativeAxisValue(acceptedInputFrame.stick_rx);
    analog[RightStickYAnalogIndex] = NativeAxisValue(acceptedInputFrame.stick_ry);
}

// 让游戏显示的图例和实际一致
uint32_t __fastcall InputConfigGetDeviceActionBindingHook(
    uint32_t action,
    uint32_t deviceIndex)
{
    const uint32_t result = originalInputConfigGetDeviceActionBinding(
        action, deviceIndex);
    if (result != UINT32_MAX || deviceIndex != 0 || !mmIoConfig.enabled)
    {
        return result;
    }

    return MapControllerBindingIndex(
        ControllerLayoutFromNativeType(
            virtualControllerType.load(std::memory_order_acquire)),
        action);
}

// 目前UI菜单会用
char __fastcall InputCheckActionHook(
    void* state,
    uint64_t action,
    InputActionPredicate predicate)
{
    char result = originalInputCheckAction(state, action, predicate);
    if (!acceptedInputFrame.active ||
        acceptedInputFrame.slider_mode == mmio::Mode::ArcadeSlider)
    {
        debugLogicalActionInitialized = false;
        return result;
    }

    const uint32_t rawAction = MapGamepadLogicalAction(action);
    if (rawAction == UINT32_MAX)
    {
        return result;
    }

    const char rawResult = predicate ? static_cast<char>(predicate(state, rawAction)) : 0;
    if (!IsDebugLoggingEnabled())
    {
        debugLogicalActionInitialized = false;
        return static_cast<char>(result || rawResult);
    }
    const uint64_t heldWord = state
        ? *reinterpret_cast<const uint64_t*>(static_cast<const uint8_t*>(state) + InputHeldButtonsOffset)
        : 0;
    const uint64_t repeatWord = state
        ? *reinterpret_cast<const uint64_t*>(static_cast<const uint8_t*>(state) + 0x90)
        : 0;
    result = static_cast<char>(result || rawResult);
    if (false && (!debugLogicalActionInitialized ||
        debugLastLogicalAction != action ||
        debugLastLogicalRawAction != rawAction ||
        debugLastLogicalResult != (result != 0)))
    {
        DebugLog(
            "Logical action shim: action=%llu raw-axis-action=%u predicate=%p held0=0x%016llX repeat0=0x%016llX raw-result=%s result=%s",
            action,
            rawAction,
            reinterpret_cast<const void*>(predicate),
            heldWord,
            repeatWord,
            rawResult ? "true" : "false",
            result ? "true" : "false");
    }
    debugLogicalActionInitialized = true;
    debugLastLogicalAction = static_cast<uint32_t>(action);
    debugLastLogicalRawAction = rawAction;
    debugLastLogicalResult = result != 0;
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

int64_t __fastcall MergeConnectedDeviceHook(void* state, void* deviceState, uint32_t playerIndex)
{
    if (mmIoConfig.exclusive_controller_input)
    {
        return 0;
    }
    return originalMergeConnectedDevice(state, deviceState, playerIndex);
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

bool __fastcall IsArcadeControllerEnabledHook()
{
    return originalIsArcadeControllerEnabled() ||
        arcadeSliderActive.load(std::memory_order_relaxed);
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
    constexpr char inputCheckActionBytes[] =
        "\x40\x55\x41\x56\x48\x83\xEC\x38\x4D\x8B\xF0\x48\x8B\xE9\x4D\x85\xC0\x75";
    constexpr char inputCheckActionMask[] = "xxxxxxxxxxxxxxxxxx";
    constexpr char selectedMergeBytes[] =
        "\x48\x89\x5C\x24\x00\x48\x89\x74\x24\x00\x48\x89\x7C\x24\x00\x41\x56\x48\x83\xEC\x20\x48\x8B\xFA";
    constexpr char selectedMergeMask[] = "xxxx?xxxx?xxxx?xxxxxxxxx";
    constexpr char arcadeBytes[] =
        "\x48\x83\xEC\x28\xE8\x00\x00\x00\x00\x0F\xB6\x40";
    constexpr char arcadeMask[] = "xxxxx????xxx";
    const bool installSelectedDeviceHook = true;
    const bool installInputConfigBindingFallbackHook = true;
    const bool installInputCheckActionHook = EnableInputCheckActionFallback;
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
    if (installInputCheckActionHook)
    {
        originalInputCheckAction = reinterpret_cast<InputCheckAction>(
            FindSignature(inputCheckActionBytes, inputCheckActionMask));
    }
    if (installArcadeControllerHook)
    {
        originalIsArcadeControllerEnabled = reinterpret_cast<IsArcadeControllerEnabled>(
            FindSignature(arcadeBytes, arcadeMask));
    }
    if (!originalMergeSliderSensorButtons ||
        (installInputConfigBindingFallbackHook &&
            !originalInputConfigGetDeviceActionBinding) ||
        (installInputCheckActionHook && !originalInputCheckAction) ||
        (installArcadeControllerHook && !originalIsArcadeControllerEnabled))
    {
        Log("MMIO core input signatures were not found; slider=%s binding-fallback=%s action-query=%s arcade-query=%s; no MMIO hooks were installed.",
            originalMergeSliderSensorButtons ? "ok" : "missing",
            installInputConfigBindingFallbackHook
                ? (originalInputConfigGetDeviceActionBinding ? "ok" : "missing")
                : "disabled",
            installInputCheckActionHook
                ? (originalInputCheckAction ? "ok" : "missing")
                : "disabled",
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

    LONG error = DetourTransactionBegin();
    if (error == NO_ERROR)
    {
        error = DetourUpdateThread(GetCurrentThread());
    }
    if (error == NO_ERROR)
    {
        error = DetourAttach(
            reinterpret_cast<void**>(&originalMergeSliderSensorButtons),
            MergeSliderSensorButtonsHook);
    }
    if (error == NO_ERROR && installInputConfigBindingFallbackHook)
    {
        error = DetourAttach(
            reinterpret_cast<void**>(&originalInputConfigGetDeviceActionBinding),
            InputConfigGetDeviceActionBindingHook);
    }
    if (error == NO_ERROR && installInputCheckActionHook)
    {
        error = DetourAttach(
            reinterpret_cast<void**>(&originalInputCheckAction),
            InputCheckActionHook);
    }
    if (error == NO_ERROR && installSelectedDeviceHook)
    {
        error = DetourAttach(
            reinterpret_cast<void**>(&originalMergeSelectedDevice),
            MergeSelectedDeviceHook);
    }
    if (error == NO_ERROR && mmIoConfig.exclusive_controller_input)
    {
        error = DetourAttach(
            reinterpret_cast<void**>(&originalMergeConnectedDevice),
            MergeConnectedDeviceHook);
    }
    if (error == NO_ERROR && mmIoConfig.exclusive_controller_input)
    {
        error = DetourAttach(
            reinterpret_cast<void**>(&originalDirectInputDevicePollState),
            DirectInputDevicePollStateHook);
    }
    if (error == NO_ERROR && installArcadeControllerHook)
    {
        error = DetourAttach(
            reinterpret_cast<void**>(&originalIsArcadeControllerEnabled),
            IsArcadeControllerEnabledHook);
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
    selectedDeviceHookInstalled = installSelectedDeviceHook;
    arcadeControllerHookInstalled = installArcadeControllerHook;
    exclusiveControllerHooksInstalled = mmIoConfig.exclusive_controller_input;
    directInputHooksInstalled = mmIoConfig.exclusive_controller_input;
    inputConfigBindingFallbackHookInstalled = installInputConfigBindingFallbackHook;
    inputCheckActionHookInstalled = installInputCheckActionHook;
    return true;
}
}

bool InitializeMmIoHooks(const MmIoConfig& config)
{
    if (!config.enabled)
    {
        return true;
    }

    mmIoConfig = config;
    virtualControllerType.store(DualSenseControllerType, std::memory_order_release);
    debugSliderStateInitialized = false;
    debugInjectedSliderInitialized = false;
    debugLogicalActionInitialized = false;
    sliderModeResolver.Initialize(config.slider_mode);
    joyShockFrontend.Initialize(
        config.gamepad,
        config.slider_mode,
        config.arcade_slider_emu_cells_per_second);
    keyboardFrontend.Initialize(
        config.keyboard_mouse_frontend,
        config.arcade_slider_emu_cells_per_second,
        config.keyboard_bindings,
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
        sliderModeResolver.Reset();
        return false;
    }

    if (!InstallDetours())
    {
        joyShockFrontend.Shutdown();
        sliderModeResolver.Reset();
        mmIoConsumer.Shutdown();
        return false;
    }

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
        inputCheckActionHookInstalled ? "true" : "false",
        arcadeControllerHookInstalled ? "true" : "false");
    return true;
}

void ShutdownMmIoHooks()
{
    arcadeSliderActive.store(false, std::memory_order_relaxed);
    exclusiveControllerActive.store(false, std::memory_order_relaxed);
    virtualControllerType.store(DualSenseControllerType, std::memory_order_release);
    acceptedInputFrame = {};
    debugSliderStateInitialized = false;
    debugInjectedSliderInitialized = false;
    joyShockFrontend.Shutdown();
    sliderModeResolver.Reset();
    mmIoConsumer.Shutdown();
    if (!installed)
    {
        return;
    }

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(
        reinterpret_cast<void**>(&originalMergeSliderSensorButtons),
        MergeSliderSensorButtonsHook);
    if (inputConfigBindingFallbackHookInstalled)
    {
        DetourDetach(
            reinterpret_cast<void**>(&originalInputConfigGetDeviceActionBinding),
            InputConfigGetDeviceActionBindingHook);
    }
    if (inputCheckActionHookInstalled)
    {
        DetourDetach(
            reinterpret_cast<void**>(&originalInputCheckAction),
            InputCheckActionHook);
    }
    if (selectedDeviceHookInstalled)
    {
        DetourDetach(
            reinterpret_cast<void**>(&originalMergeSelectedDevice),
            MergeSelectedDeviceHook);
    }
    if (exclusiveControllerHooksInstalled)
    {
        DetourDetach(
            reinterpret_cast<void**>(&originalMergeConnectedDevice),
            MergeConnectedDeviceHook);
    }
    if (directInputHooksInstalled)
    {
        DetourDetach(
            reinterpret_cast<void**>(&originalDirectInputDevicePollState),
            DirectInputDevicePollStateHook);
    }
    if (arcadeControllerHookInstalled)
    {
        DetourDetach(
            reinterpret_cast<void**>(&originalIsArcadeControllerEnabled),
            IsArcadeControllerEnabledHook);
    }
    DetourTransactionCommit();
    selectedDeviceHookInstalled = false;
    exclusiveControllerHooksInstalled = false;
    directInputHooksInstalled = false;
    inputConfigBindingFallbackHookInstalled = false;
    inputCheckActionHookInstalled = false;
    arcadeControllerHookInstalled = false;
    installed = false;
}
}
