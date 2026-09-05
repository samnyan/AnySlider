#include "pch.h"

#include "mm_io_hooks.h"

#include "Dependencies/Signature.h"
#include "anyslider_log.h"
#include "mm_io_keyboard.h"
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
using SteamInputPollAndMapDirections = char(__fastcall*)(void* manager, uint32_t* state);

MergeSliderSensorButtons originalMergeSliderSensorButtons = nullptr;
MergeConnectedDevice originalMergeConnectedDevice = nullptr;
MergeSelectedDevice originalMergeSelectedDevice = nullptr;
IsArcadeControllerEnabled originalIsArcadeControllerEnabled = nullptr;
SteamInputPollAndMapDirections originalSteamInputPollAndMapDirections = nullptr;

MmIoConfig mmIoConfig;
MmIoConsumer mmIoConsumer;
MmIoKeyboardFrontend keyboardFrontend;
std::atomic_bool arcadeSliderActive = false;
std::atomic_bool steamInputDetected = false;
std::atomic_bool takeoverActive = false;
bool installed = false;
bool steamInputHookInstalled = false;

struct AcceptedInputFrame
{
    mmio::InputSnapshot snapshot{};
    uint64_t gamebtn_tapped[mmio::kGameButtonWordCount]{};
    uint64_t gamebtn_released[mmio::kGameButtonWordCount]{};
    uint64_t gamebtn_down[mmio::kGameButtonWordCount]{};
    bool active = false;
    bool takeover = false;
    bool arcade_mode = false;
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
        externalFrame, mmIoConfig.input_lease_ms);
    const bool externalActive = externalFrame.source_active;
    const mmio::InputSnapshot keyboardSnapshot = keyboardFrontend.Poll();
    const bool keyboardActive = keyboardFrontend.IsEnabled();

    acceptedInputFrame = {};
    if (externalFrameAvailable)
    {
        acceptedInputFrame.snapshot = externalFrame.snapshot;
        std::memcpy(acceptedInputFrame.gamebtn_tapped, externalFrame.gamebtn_tapped, sizeof(externalFrame.gamebtn_tapped));
        std::memcpy(acceptedInputFrame.gamebtn_released, externalFrame.gamebtn_released, sizeof(externalFrame.gamebtn_released));
        std::memcpy(acceptedInputFrame.gamebtn_down, externalFrame.gamebtn_down, sizeof(externalFrame.gamebtn_down));
        acceptedInputFrame.active = true;
        keyboardButtonEdges.Reset();
    }
    else if (keyboardActive)
    {
        acceptedInputFrame.snapshot = keyboardSnapshot;
        acceptedInputFrame.active = true;
        keyboardButtonEdges.Apply(
            keyboardSnapshot.gamebtn,
            acceptedInputFrame.gamebtn_tapped,
            acceptedInputFrame.gamebtn_released,
            acceptedInputFrame.gamebtn_down);
    }
    else
    {
        keyboardButtonEdges.Reset();
    }

    acceptedInputFrame.takeover = externalActive && mmIoConfig.takeover;
    if (!externalFrameAvailable && keyboardActive)
    {
        acceptedInputFrame.takeover = mmIoConfig.takeover;
    }
    acceptedInputFrame.arcade_mode = acceptedInputFrame.active &&
        acceptedInputFrame.snapshot.mode == static_cast<uint32_t>(mmio::Mode::ArcadeSlider);
    arcadeSliderActive.store(acceptedInputFrame.arcade_mode, std::memory_order_relaxed);

    const bool previousTakeover = takeoverActive.exchange(
        acceptedInputFrame.takeover,
        std::memory_order_relaxed);
    if (previousTakeover != acceptedInputFrame.takeover)
    {
        Log("MMIO takeover %s; physical input merge is %s.",
            acceptedInputFrame.takeover ? "active" : "inactive",
            acceptedInputFrame.takeover ? "blocked" : "enabled");
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
    if (acceptedInputFrame.arcade_mode)
    {
        analog[SliderCellsAnalogIndex] |= MapTouchCells(
            acceptedInputFrame.snapshot.touch_cells);
    }
    else
    {
        heldButtons[0] |= MapGamepadSlide(acceptedInputFrame.snapshot.gamepad_slide);
    }
}

int64_t __fastcall MergeSliderSensorButtonsHook(void* state, void* sensorState)
{
    RefreshAcceptedInputFrame();
    if (acceptedInputFrame.takeover)
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
    if (acceptedInputFrame.takeover)
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
    if (acceptedInputFrame.takeover ||
        (acceptedInputFrame.active && mmIoConfig.force_gamepad_ui))
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

char __fastcall SteamInputPollAndMapDirectionsHook(void* manager, uint32_t* state)
{
    const char result = originalSteamInputPollAndMapDirections(manager, state);
    if (result && !steamInputDetected.exchange(true, std::memory_order_relaxed))
    {
        Log("Steam Input controller detected; gamepad UI may be selected by the game.");
    }
    return result;
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
    constexpr char steamInputBytes[] =
        "\x48\x8B\xC4\x48\x89\x58\x00\x48\x89\x70\x00\x55\x57\x41\x56\x48\x8D\x68\x00\x48\x81\xEC\x90\x00\x00\x00\x0F\x29\x70\x00\x0F\x29\x78";
    constexpr char steamInputMask[] = "xxxxxx?xxx?xxxxxxx?xxxxxxxxxx?xxx";

    originalMergeSliderSensorButtons = reinterpret_cast<MergeSliderSensorButtons>(
        FindSignature(sliderMergeBytes, sliderMergeMask));
    originalMergeConnectedDevice = reinterpret_cast<MergeConnectedDevice>(
        FindSignature(connectedMergeBytes, connectedMergeMask));
    originalMergeSelectedDevice = reinterpret_cast<MergeSelectedDevice>(
        FindSignature(selectedMergeBytes, selectedMergeMask));
    originalIsArcadeControllerEnabled = reinterpret_cast<IsArcadeControllerEnabled>(
        FindSignature(arcadeBytes, arcadeMask));
    if (!originalMergeSliderSensorButtons ||
        !originalMergeConnectedDevice ||
        !originalMergeSelectedDevice ||
        !originalIsArcadeControllerEnabled)
    {
        Log("MMIO input signatures were not found; no MMIO hooks were installed.");
        return false;
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
    if (error == NO_ERROR)
    {
        error = DetourAttach(
            reinterpret_cast<void**>(&originalMergeConnectedDevice),
            MergeConnectedDeviceHook);
    }
    if (error == NO_ERROR)
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

    originalSteamInputPollAndMapDirections =
        reinterpret_cast<SteamInputPollAndMapDirections>(
            FindSignature(steamInputBytes, steamInputMask));
    if (!originalSteamInputPollAndMapDirections)
    {
        Log("Steam Input detection signature was not found.");
        return true;
    }

    error = DetourTransactionBegin();
    if (error == NO_ERROR)
    {
        error = DetourUpdateThread(GetCurrentThread());
    }
    if (error == NO_ERROR)
    {
        error = DetourAttach(
            reinterpret_cast<void**>(&originalSteamInputPollAndMapDirections),
            SteamInputPollAndMapDirectionsHook);
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
        Log("Could not install Steam Input detection hook: %ld", error);
        return true;
    }

    steamInputHookInstalled = true;
    Log("Steam Input detection hook installed.");
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
        config.keyboard_frontend,
        config.keyboard_slider_cells_per_second,
        config.keyboard_bindings);
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

    Log("MMIO backend ready: mapping=%ls lease=%llu ms takeover=%s keyboard=%s gamepad-ui=%s",
        config.shared_memory_name.c_str(),
        config.input_lease_ms,
        config.takeover ? "true" : "false",
        config.keyboard_frontend ? "true" : "false",
        config.force_gamepad_ui ? "true" : "false");
    return true;
}

void ShutdownMmIoHooks()
{
    arcadeSliderActive.store(false, std::memory_order_relaxed);
    steamInputDetected.store(false, std::memory_order_relaxed);
    takeoverActive.store(false, std::memory_order_relaxed);
    mmIoConsumer.Shutdown();
    if (!installed)
    {
        return;
    }

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    if (steamInputHookInstalled)
    {
        DetourDetach(
            reinterpret_cast<void**>(&originalSteamInputPollAndMapDirections),
            SteamInputPollAndMapDirectionsHook);
        steamInputHookInstalled = false;
    }
    DetourDetach(
        reinterpret_cast<void**>(&originalMergeSliderSensorButtons),
        MergeSliderSensorButtonsHook);
    DetourDetach(
        reinterpret_cast<void**>(&originalMergeConnectedDevice),
        MergeConnectedDeviceHook);
    DetourDetach(
        reinterpret_cast<void**>(&originalMergeSelectedDevice),
        MergeSelectedDeviceHook);
    DetourDetach(
        reinterpret_cast<void**>(&originalIsArcadeControllerEnabled),
        IsArcadeControllerEnabledHook);
    DetourTransactionCommit();
    installed = false;
}
}
