#include "pch.h"

#include "mm_io_hooks.h"

#include "Dependencies/Signature.h"
#include "anyslider_log.h"
#include "mm_io_keyboard.h"
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
std::atomic_bool arcadeSliderActive = false;
std::atomic_bool exclusiveControllerActive = false;
bool installed = false;
bool exclusiveControllerHooksInstalled = false;

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

bool HasActiveTouchCell(const uint8_t (&touchCells)[mmio::kTouchCellCount])
{
    for (const uint8_t cell : touchCells)
    {
        if (cell != 0)
        {
            return true;
        }
    }
    return false;
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

void RefreshAcceptedInputFrame()
{
    MmIoConsumer::InputFrame externalFrame{};
    const bool externalFrameAvailable = mmIoConsumer.ReadFrame(
        externalFrame, mmIoConfig.max_input_lease_ms);
    const bool externalActive = externalFrame.source_active;
    const mmio::InputSnapshot keyboardSnapshot = keyboardFrontend.Poll();
    const bool keyboardActive = keyboardFrontend.IsEnabled();

    acceptedInputFrame = {};
    uint64_t providerHeld[mmio::kGameButtonWordCount]{};
    if (externalFrameAvailable)
    {
        MergeGameButtons(
            acceptedInputFrame.gamebtn_tapped,
            externalFrame.gamebtn_tapped);
        MergeGameButtons(
            acceptedInputFrame.gamebtn_released,
            externalFrame.gamebtn_released);
        MergeGameButtons(providerHeld, externalFrame.gamebtn_down);
        acceptedInputFrame.active = true;
    }
    if (externalActive)
    {
        acceptedInputFrame.slider_mode = static_cast<mmio::Mode>(
            externalFrame.snapshot.mode);
        if (acceptedInputFrame.slider_mode == mmio::Mode::ArcadeSlider)
        {
            MergeTouchCells(
                acceptedInputFrame.touch_cells,
                externalFrame.snapshot.touch_cells);
        }
        else
        {
            acceptedInputFrame.gamepad_slide =
                externalFrame.snapshot.gamepad_slide;
        }
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
        if (!externalActive)
        {
            acceptedInputFrame.slider_mode = mmio::Mode::ArcadeSlider;
        }
        acceptedInputFrame.active = true;
    }
    else
    {
        keyboardButtonEdges.Reset();
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

    if (keyboardActive)
    {
        const bool keyboardSliderActive = HasActiveTouchCell(
            keyboardSnapshot.touch_cells);
        if (keyboardSliderActive &&
            acceptedInputFrame.slider_mode == mmio::Mode::GamepadDualStick)
        {
            acceptedInputFrame.slider_mode = mmio::Mode::ArcadeSlider;
            acceptedInputFrame.gamepad_slide = 0;
        }

        if (acceptedInputFrame.slider_mode == mmio::Mode::ArcadeSlider)
        {
            MergeTouchCells(
                acceptedInputFrame.touch_cells,
                keyboardSnapshot.touch_cells);
        }
    }

    acceptedInputFrame.exclusive =
        (externalActive || keyboardActive) && mmIoConfig.exclusive_controller_input;
    arcadeSliderActive.store(
        acceptedInputFrame.active &&
            acceptedInputFrame.slider_mode == mmio::Mode::ArcadeSlider,
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
        analog[SliderCellsAnalogIndex] |= MapTouchCells(
            acceptedInputFrame.touch_cells);
    }
    else if (acceptedInputFrame.slider_mode == mmio::Mode::GamepadDualStick)
    {
        heldButtons[0] |= MapGamepadSlide(acceptedInputFrame.gamepad_slide);
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
    if (acceptedInputFrame.exclusive)
    {
        static_cast<uint8_t*>(state)[InputSelectedDevicePresentOffset] = 1;
        if (selectedDeviceType)
        {
            *selectedDeviceType = VirtualGamepadDeviceType;
        }
        return VirtualGamepadDeviceType;
    }
    return originalMergeSelectedDevice(state, deviceState, playerIndex, selectedDeviceType);
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

    originalMergeSliderSensorButtons = reinterpret_cast<MergeSliderSensorButtons>(
        FindSignature(sliderMergeBytes, sliderMergeMask));
    originalIsArcadeControllerEnabled = reinterpret_cast<IsArcadeControllerEnabled>(
        FindSignature(arcadeBytes, arcadeMask));
    if (!originalMergeSliderSensorButtons || !originalIsArcadeControllerEnabled)
    {
        Log("MMIO core input signatures were not found; no MMIO hooks were installed.");
        return false;
    }

    if (mmIoConfig.exclusive_controller_input)
    {
        originalMergeConnectedDevice = reinterpret_cast<MergeConnectedDevice>(
            FindSignature(connectedMergeBytes, connectedMergeMask));
        originalMergeSelectedDevice = reinterpret_cast<MergeSelectedDevice>(
            FindSignature(selectedMergeBytes, selectedMergeMask));
        if (!originalMergeConnectedDevice || !originalMergeSelectedDevice)
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
    if (error == NO_ERROR && mmIoConfig.exclusive_controller_input)
    {
        error = DetourAttach(
            reinterpret_cast<void**>(&originalMergeConnectedDevice),
            MergeConnectedDeviceHook);
    }
    if (error == NO_ERROR && mmIoConfig.exclusive_controller_input)
    {
        error = DetourAttach(
            reinterpret_cast<void**>(&originalMergeSelectedDevice),
            MergeSelectedDeviceHook);
    }
    if (error == NO_ERROR)
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
    keyboardFrontend.Initialize(
        config.keyboard_mouse_frontend,
        config.keyboard_slider_cells_per_second,
        config.keyboard_bindings,
        config.mouse_slider);
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
        return false;
    }

    if (!InstallDetours())
    {
        mmIoConsumer.Shutdown();
        return false;
    }

    Log("MMIO backend ready: mapping=%ls lease=%llu ms exclusive=%s keyboard-mouse=%s mouse-slider=%s",
        config.shared_memory_name.c_str(),
        config.max_input_lease_ms,
        config.exclusive_controller_input ? "true" : "false",
        config.keyboard_mouse_frontend ? "true" : "false",
        keyboardFrontend.IsMouseSliderEnabled() ? "true" : "false");
    return true;
}

void ShutdownMmIoHooks()
{
    arcadeSliderActive.store(false, std::memory_order_relaxed);
    exclusiveControllerActive.store(false, std::memory_order_relaxed);
    acceptedInputFrame = {};
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
    if (exclusiveControllerHooksInstalled)
    {
        DetourDetach(
            reinterpret_cast<void**>(&originalMergeConnectedDevice),
            MergeConnectedDeviceHook);
        DetourDetach(
            reinterpret_cast<void**>(&originalMergeSelectedDevice),
            MergeSelectedDeviceHook);
        exclusiveControllerHooksInstalled = false;
    }
    DetourDetach(
        reinterpret_cast<void**>(&originalIsArcadeControllerEnabled),
        IsArcadeControllerEnabledHook);
    DetourTransactionCommit();
    installed = false;
}
}
