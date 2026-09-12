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
#include <cstring>

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
constexpr uint32_t VirtualGamepadDeviceType = 7;

using MergeSliderSensorButtons = int64_t(__fastcall*)(void* state, void* sensorState);
using MergeConnectedDevice = int64_t(__fastcall*)(void* state, void* deviceState, uint32_t playerIndex);
using MergeSelectedDevice = int64_t(__fastcall*)(void* state, void* deviceState, uint32_t playerIndex, uint32_t* selectedDeviceType);
using IsArcadeControllerEnabled = bool(__fastcall*)();

MergeSliderSensorButtons originalMergeSliderSensorButtons = nullptr;
MergeConnectedDevice originalMergeConnectedDevice = nullptr;
MergeSelectedDevice originalMergeSelectedDevice = nullptr;
IsArcadeControllerEnabled originalIsArcadeControllerEnabled = nullptr;

MmIoConfig mmIoConfig;
MmIoConsumer mmIoConsumer;
MmIoKeyboardMouseFrontend keyboardFrontend;
MmIoJoyShockFrontend joyShockFrontend;
MmIoSliderModeResolver sliderModeResolver;
std::atomic_bool arcadeSliderActive = false;
std::atomic_bool exclusiveControllerActive = false;
bool installed = false;
bool selectedDeviceHookInstalled = false;
bool exclusiveControllerHooksInstalled = false;
bool arcadeControllerHookInstalled = false;
thread_local bool debugSliderStateInitialized = false;
thread_local MmIoSliderMode debugLastSliderMode = MmIoSliderMode::Arcade;
thread_local bool debugLastDirectTouch = false;
thread_local uint32_t debugLastDirectionHeld = 0;
thread_local bool debugInjectedSliderInitialized = false;
thread_local mmio::Mode debugLastInjectedMode = mmio::Mode::None;
thread_local uint32_t debugLastInjectedValue = 0;

struct AcceptedInputFrame
{
    uint64_t gamebtn_tapped[mmio::kGameButtonWordCount]{};
    uint64_t gamebtn_released[mmio::kGameButtonWordCount]{};
    uint64_t gamebtn_down[mmio::kGameButtonWordCount]{};
    uint8_t touch_cells[mmio::kTouchCellCount]{};
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

void MergeGameButtons(
    uint64_t (&destination)[mmio::kGameButtonWordCount],
    const uint64_t (&source)[mmio::kGameButtonWordCount])
{
    for (uint32_t word = 0; word < mmio::kGameButtonWordCount; ++word)
    {
        destination[word] |= source[word];
    }
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

void* FindSignature(const char* bytes, const char* mask)
{
    const MODULEINFO& module = getModuleInfo();
    return sigScan(bytes, mask, std::strlen(mask), module.lpBaseOfDll, module.SizeOfImage);
}

uint64_t MapGamepadSlide(uint32_t slideHeld)
{
    uint64_t result = 0;
    if (slideHeld & mmio::SlideLeft1)
    {
        result |= 1ull << 26;
    }
    if (slideHeld & mmio::SlideRight1)
    {
        result |= 1ull << 27;
    }
    if (slideHeld & mmio::SlideLeft2)
    {
        result |= 1ull << 30;
    }
    if (slideHeld & mmio::SlideRight2)
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
    { mmio::Pause, "Pause" },
};

void DebugLogSharedMemoryButtons(const MmIoConsumer::InputFrame& frame)
{
    for (const SharedMemoryButtonDebugEntry& entry : kSharedMemoryButtonDebugEntries)
    {
        if (mmio::IsGameButtonDown(frame.gamebtn_tapped, entry.action))
        {
            DebugLog(
                "Shared memory button: mode=%s source=0x%08X action=%s(%u) down",
                SharedMemoryModeName(frame.snapshot.mode),
                frame.snapshot.source_id,
                entry.name,
                entry.action);
        }
        if (mmio::IsGameButtonDown(frame.gamebtn_released, entry.action))
        {
            DebugLog(
                "Shared memory button: mode=%s source=0x%08X action=%s(%u) up",
                SharedMemoryModeName(frame.snapshot.mode),
                frame.snapshot.source_id,
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
    const bool keyboardActive = keyboardFrontend.IsEnabled();
    MmIoJoyShockFrame controllerFrame{};
    bool controllerAvailable = false;
    bool controllerTakeoverActive = false;
    if (mmIoConfig.gamepad.enabled)
    {
        controllerFrame = joyShockFrontend.Consume();
        controllerAvailable = controllerFrame.has_activity;
        controllerTakeoverActive =
            mmIoConfig.exclusive_controller_input && controllerFrame.connected;
    }

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

    if (controllerTakeoverActive)
    {
        MergeGameButtons(
            acceptedInputFrame.gamebtn_tapped,
            controllerFrame.gamebtn_tapped);
        MergeGameButtons(
            acceptedInputFrame.gamebtn_released,
            controllerFrame.gamebtn_released);
        MergeGameButtons(providerHeld, controllerFrame.gamebtn_down);
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
    const bool externalArcadeActive =
        externalMode == mmio::Mode::ArcadeSlider &&
        HasMmIoSliderTouch(externalFrame.snapshot.touch_cells);
    const uint32_t externalGamepadSlide =
        externalMode == mmio::Mode::GamepadDualStick
            ? externalFrame.snapshot.gamepad_slide
            : 0;
    const bool keyboardDirectTouchActive =
        HasMmIoSliderTouch(keyboardFrame.direct_touch_cells);
    const bool controllerDirectTouchActive =
        controllerAvailable && HasMmIoSliderTouch(controllerFrame.touch_cells);
    const bool frontendDirectionActive =
        (keyboardActive && keyboardFrame.slider_direction != 0) ||
        (controllerTakeoverActive && controllerFrame.gamepad_slide != 0);
    const uint32_t directionHeld = externalGamepadSlide |
        (keyboardActive ? keyboardFrame.slider_direction : 0) |
        (controllerTakeoverActive ? controllerFrame.gamepad_slide : 0);
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
            controllerTakeoverActive ? controllerFrame.gamepad_slide : 0,
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
                externalFrame.snapshot.touch_cells);
        }
        if (keyboardActive)
        {
            MergeTouchCells(
                acceptedInputFrame.touch_cells,
                keyboardSnapshot.touch_cells);
        }
        if (controllerAvailable)
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
    }

    acceptedInputFrame.ui_activity_pending = HasPrimaryButtonTap(
        acceptedInputFrame.gamebtn_tapped);
    acceptedInputFrame.exclusive =
        (externalActive || keyboardActive || controllerTakeoverActive) &&
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
        heldButtons[0] |= gamepadMask;
        if (!debugInjectedSliderInitialized ||
            debugLastInjectedMode != mmio::Mode::GamepadDualStick ||
            debugLastInjectedValue != gamepadMask)
        {
            DebugLog(
                "Injected slider: mode=GamepadDualStick button-mask=0x%08X slide=0x%X",
                gamepadMask,
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
    if (acceptedInputFrame.exclusive)
    {
        return 0;
    }
    return originalMergeConnectedDevice(state, deviceState, playerIndex);
}

int64_t __fastcall MergeSelectedDeviceHook(
    void* state,
    void* deviceState,
    uint32_t playerIndex,
    uint32_t* selectedDeviceType)
{
    const int64_t result = originalMergeSelectedDevice(
        state, deviceState, playerIndex, selectedDeviceType);
    const bool uiActivityPending = acceptedInputFrame.ui_activity_pending;
    acceptedInputFrame.ui_activity_pending = false;
    if (uiActivityPending)
    {
        static_cast<uint8_t*>(state)[InputSelectedDevicePresentOffset] = 1;
        if (selectedDeviceType)
        {
            *selectedDeviceType = VirtualGamepadDeviceType;
        }
        return VirtualGamepadDeviceType;
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
    constexpr char selectedMergeBytes[] =
        "\x48\x89\x5C\x24\x00\x48\x89\x74\x24\x00\x48\x89\x7C\x24\x00\x41\x56\x48\x83\xEC\x20\x48\x8B\xFA";
    constexpr char selectedMergeMask[] = "xxxx?xxxx?xxxx?xxxxxxxxx";
    constexpr char arcadeBytes[] =
        "\x48\x83\xEC\x28\xE8\x00\x00\x00\x00\x0F\xB6\x40";
    constexpr char arcadeMask[] = "xxxxx????xxx";
    const bool installSelectedDeviceHook =
        mmIoConfig.keyboard_mouse_frontend || mmIoConfig.gamepad.enabled;
    // Shared-memory producers may publish ArcadeSlider without a built-in frontend.
    const bool installArcadeControllerHook = true;

    originalMergeSliderSensorButtons = reinterpret_cast<MergeSliderSensorButtons>(
        FindSignature(sliderMergeBytes, sliderMergeMask));
    if (installArcadeControllerHook)
    {
        originalIsArcadeControllerEnabled = reinterpret_cast<IsArcadeControllerEnabled>(
            FindSignature(arcadeBytes, arcadeMask));
    }
    if (!originalMergeSliderSensorButtons ||
        (installArcadeControllerHook && !originalIsArcadeControllerEnabled))
    {
        Log("MMIO core input signatures were not found; no MMIO hooks were installed.");
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
        if (!originalMergeConnectedDevice)
        {
            Log("MMIO exclusive controller signature was not found.");
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
    debugSliderStateInitialized = false;
    debugInjectedSliderInitialized = false;
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
            mmio::SupportsArcadeSlider | mmio::SupportsGamepadDualStick))
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

    Log("MMIO backend ready: mapping=%ls lease=%llu ms exclusive=%s keyboard-mouse=%s mouse-slider=%s gamepad=%s selected-device-hook=%s arcade-query-hook=%s",
        config.shared_memory_name.c_str(),
        config.max_input_lease_ms,
        config.exclusive_controller_input ? "true" : "false",
        config.keyboard_mouse_frontend ? "true" : "false",
        keyboardFrontend.IsMouseSliderEnabled() ? "true" : "false",
        joyShockFrontend.IsEnabled() ? "true" : "false",
        selectedDeviceHookInstalled ? "true" : "false",
        arcadeControllerHookInstalled ? "true" : "false");
    return true;
}

void ShutdownMmIoHooks()
{
    arcadeSliderActive.store(false, std::memory_order_relaxed);
    exclusiveControllerActive.store(false, std::memory_order_relaxed);
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
    if (arcadeControllerHookInstalled)
    {
        DetourDetach(
            reinterpret_cast<void**>(&originalIsArcadeControllerEnabled),
            IsArcadeControllerEnabledHook);
    }
    DetourTransactionCommit();
    selectedDeviceHookInstalled = false;
    exclusiveControllerHooksInstalled = false;
    arcadeControllerHookInstalled = false;
    installed = false;
}
}
