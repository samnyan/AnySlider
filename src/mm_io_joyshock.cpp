#include "pch.h"

#include "mm_io_joyshock.h"

#include "anyslider_log.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>

namespace anyslider
{
namespace
{
constexpr int kUnknownController = 0;
constexpr int kMaxConnectedControllers = 16;

const char* ControllerTypeName(int controllerType)
{
    switch (controllerType)
    {
    case JS_TYPE_DS: return "DualSense";
    case JS_TYPE_DS4: return "DualShock 4";
    case JS_TYPE_PRO_CONTROLLER: return "ProController";
    case JS_TYPE_JOYCON_LEFT: return "Joy-Con Left";
    case JS_TYPE_JOYCON_RIGHT: return "Joy-Con Right";
    default: return "Unknown";
    }
}

int ControllerPriority(int controllerType)
{
    switch (controllerType)
    {
    case JS_TYPE_DS: return 0;
    case JS_TYPE_DS4: return 1;
    case JS_TYPE_PRO_CONTROLLER: return 2;
    case JS_TYPE_JOYCON_LEFT:
    case JS_TYPE_JOYCON_RIGHT: return 3;
    default: return 100;
    }
}

void SetAction(
    uint64_t (&buttons)[mmio::kGameButtonWordCount],
    uint32_t action,
    bool down)
{
    mmio::SetGameButton(buttons, action, down);
}

bool IsAxisNegative(float value, float deadzone)
{
    return value <= -deadzone;
}

bool IsAxisPositive(float value, float deadzone)
{
    return value >= deadzone;
}

void MapJoyShockButtons(
    int controllerType,
    const MmIoGamepadConfig& config,
    JOY_SHOCK_STATE state,
    uint64_t (&buttons)[mmio::kGameButtonWordCount])
{
    const bool nintendoLayout =
        controllerType == JS_TYPE_PRO_CONTROLLER ||
        controllerType == JS_TYPE_JOYCON_LEFT ||
        controllerType == JS_TYPE_JOYCON_RIGHT;
    SetAction(buttons, mmio::DpadUp, (state.buttons & JSMASK_UP) != 0);
    SetAction(buttons, mmio::DpadDown, (state.buttons & JSMASK_DOWN) != 0);
    SetAction(buttons, mmio::DpadLeft, (state.buttons & JSMASK_LEFT) != 0);
    SetAction(buttons, mmio::DpadRight, (state.buttons & JSMASK_RIGHT) != 0);
    if (nintendoLayout)
    {
        // JoyShock reports face buttons by position. Nintendo's face-button
        // labels are rotated relative to the game's PS/Xbox action semantics.
        SetAction(buttons, mmio::Square, (state.buttons & JSMASK_N) != 0);
        SetAction(buttons, mmio::Triangle, (state.buttons & JSMASK_W) != 0);
        SetAction(buttons, mmio::Circle, (state.buttons & JSMASK_S) != 0);
        SetAction(buttons, mmio::Cross, (state.buttons & JSMASK_E) != 0);
    }
    else
    {
        SetAction(buttons, mmio::Square, (state.buttons & JSMASK_W) != 0);
        SetAction(buttons, mmio::Triangle, (state.buttons & JSMASK_N) != 0);
        SetAction(buttons, mmio::Circle, (state.buttons & JSMASK_E) != 0);
        SetAction(buttons, mmio::Cross, (state.buttons & JSMASK_S) != 0);
    }
    SetAction(buttons, mmio::L1, (state.buttons & JSMASK_L) != 0);
    SetAction(buttons, mmio::R1, (state.buttons & JSMASK_R) != 0);
    SetAction(buttons, mmio::L2, state.lTrigger >= 0.5f);
    SetAction(buttons, mmio::R2, state.rTrigger >= 0.5f);
    SetAction(buttons, mmio::L3, (state.buttons & JSMASK_LCLICK) != 0);
    SetAction(buttons, mmio::R3, (state.buttons & JSMASK_RCLICK) != 0);

    const float deadzone = config.stick_slider_deadzone;
    SetAction(buttons, mmio::Stick1Up, IsAxisNegative(state.stickLY, deadzone));
    SetAction(buttons, mmio::Stick1Down, IsAxisPositive(state.stickLY, deadzone));
    SetAction(buttons, mmio::Stick1Left, IsAxisNegative(state.stickLX, deadzone));
    SetAction(buttons, mmio::Stick1Right, IsAxisPositive(state.stickLX, deadzone));
    SetAction(buttons, mmio::Stick2Up, IsAxisNegative(state.stickRY, deadzone));
    SetAction(buttons, mmio::Stick2Down, IsAxisPositive(state.stickRY, deadzone));
    SetAction(buttons, mmio::Stick2Left, IsAxisNegative(state.stickRX, deadzone));
    SetAction(buttons, mmio::Stick2Right, IsAxisPositive(state.stickRX, deadzone));

    if (controllerType == JS_TYPE_DS || controllerType == JS_TYPE_DS4)
    {
        SetAction(buttons, mmio::Pause, (state.buttons & JSMASK_OPTIONS) != 0);
        SetAction(buttons, mmio::Select, (state.buttons & JSMASK_SHARE) != 0);
        SetAction(buttons, mmio::Start, (state.buttons & JSMASK_TOUCHPAD_CLICK) != 0);
    }
    else if (controllerType == JS_TYPE_PRO_CONTROLLER ||
        controllerType == JS_TYPE_JOYCON_LEFT ||
        controllerType == JS_TYPE_JOYCON_RIGHT)
    {
        // Native Type46 binding: Pro/Joy-Con + is action 160 (Pause),
        // while - is action 15 (Select). Action 2 is arcade START.
        SetAction(buttons, mmio::Pause, (state.buttons & JSMASK_PLUS) != 0);
        SetAction(buttons, mmio::Select, (state.buttons & JSMASK_MINUS) != 0);
    }
}

struct ButtonDebugEntry
{
    uint32_t action;
    const char* name;
};

constexpr ButtonDebugEntry kButtonDebugEntries[] = {
    { mmio::DpadUp, "DpadUp" },
    { mmio::DpadDown, "DpadDown" },
    { mmio::DpadLeft, "DpadLeft" },
    { mmio::DpadRight, "DpadRight" },
    { mmio::Square, "Square" },
    { mmio::Triangle, "Triangle" },
    { mmio::Circle, "Circle" },
    { mmio::Cross, "Cross" },
    { mmio::L1, "L1" },
    { mmio::R1, "R1" },
    { mmio::L2, "L2" },
    { mmio::R2, "R2" },
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
    { mmio::Select, "Select" },
    { mmio::Start, "Start" },
};

uint32_t MapStickSlider(const MmIoGamepadConfig& config, JOY_SHOCK_STATE state)
{
    uint32_t result = 0;
    if (IsAxisNegative(state.stickLX, config.stick_slider_deadzone))
    {
        result |= mmio::SlideLeft1;
    }
    else if (IsAxisPositive(state.stickLX, config.stick_slider_deadzone))
    {
        result |= mmio::SlideRight1;
    }
    if (IsAxisNegative(state.stickRX, config.stick_slider_deadzone))
    {
        result |= mmio::SlideLeft2;
    }
    else if (IsAxisPositive(state.stickRX, config.stick_slider_deadzone))
    {
        result |= mmio::SlideRight2;
    }
    return result;
}

int TouchXToCell(float x, bool invert)
{
    const float clamped = std::clamp(x, 0.0f, 1.0f);
    int cell = std::clamp(
        static_cast<int>(clamped * static_cast<float>(mmio::kTouchCellCount)),
        0,
        static_cast<int>(mmio::kTouchCellCount - 1));
    if (invert)
    {
        cell = static_cast<int>(mmio::kTouchCellCount - 1) - cell;
    }
    return cell;
}

uint32_t MapTouchCells(const MmIoGamepadConfig& config, TOUCH_STATE state)
{
    uint32_t result = 0;
    if (state.t0Down)
    {
        result |= 1u << TouchXToCell(state.t0X, config.touchpad_invert);
    }
    if (state.t1Down)
    {
        result |= 1u << TouchXToCell(state.t1X, config.touchpad_invert);
    }
    return result;
}

MmIoControllerType ControllerLayout(int controllerType)
{
    switch (controllerType)
    {
    case JS_TYPE_DS4:
        return MmIoControllerType::DualShock4;
    case JS_TYPE_PRO_CONTROLLER:
    case JS_TYPE_JOYCON_LEFT:
    case JS_TYPE_JOYCON_RIGHT:
        return MmIoControllerType::Nintendo;
    default:
        return MmIoControllerType::DualSense;
    }
}

void StoreFloat(std::atomic<uint32_t>& destination, float value)
{
    destination.store(std::bit_cast<uint32_t>(value), std::memory_order_relaxed);
}

float LoadFloat(const std::atomic<uint32_t>& source)
{
    return std::bit_cast<float>(source.load(std::memory_order_relaxed));
}
}

std::atomic<MmIoJoyShockFrontend*> MmIoJoyShockFrontend::activeInstance_ = nullptr;

bool MmIoJoyShockFrontend::Initialize(
    const MmIoGamepadConfig& config,
    MmIoSliderMode sliderMode,
    float arcadeSliderCellsPerSecond)
{
    Shutdown();
    config_ = config;
    slider_mode_ = sliderMode;
    arcade_slider_cells_per_second_ = arcadeSliderCellsPerSecond;
    enabled_ = config.enabled;
    if (!enabled_)
    {
        return true;
    }
    if (config.device != "auto")
    {
        Log("Unsupported io_gamepad_device=%s; using auto.", config.device.c_str());
    }

    shuttingDown_.store(false, std::memory_order_release);
    activeInstance_.store(this, std::memory_order_release);
    JslSetCallback(InputCallback);
    JslSetTouchCallback(TouchCallback);
    JslSetConnectCallback(ConnectCallback);
    JslSetDisconnectCallback(DisconnectCallback);
    discoveryThread_ = std::jthread([this](std::stop_token stopToken)
    {
        DiscoveryLoop(stopToken);
    });
    RequestDiscovery();
    return true;
}

void MmIoJoyShockFrontend::Shutdown()
{
    if (!enabled_ && !discoveryThread_.joinable())
    {
        return;
    }

    shuttingDown_.store(true, std::memory_order_release);
    discoveryThread_.request_stop();
    discoveryCondition_.notify_all();
    if (discoveryThread_.joinable())
    {
        discoveryThread_.join();
    }

    JslSetCallback(nullptr);
    JslSetTouchCallback(nullptr);
    JslSetConnectCallback(nullptr);
    JslSetDisconnectCallback(nullptr);
    JslDisconnectAndDisposeAll();
    activeInstance_.store(nullptr, std::memory_order_release);
    ClearControllerState(false);
    selectedHandle_.store(-1, std::memory_order_release);
    selectedType_.store(kUnknownController, std::memory_order_release);
    connected_.store(false, std::memory_order_release);
    enabled_ = false;
}

bool MmIoJoyShockFrontend::IsEnabled() const
{
    return enabled_;
}

bool MmIoJoyShockFrontend::HasSelectedController() const
{
    return connected_.load(std::memory_order_acquire);
}

MmIoJoyShockFrame MmIoJoyShockFrontend::Consume()
{
    MmIoJoyShockFrame frame{};
    std::lock_guard sliderLock(slider_mutex_);
    const auto now = std::chrono::steady_clock::now();
    const float deltaSeconds = has_last_consume_time_
        ? std::chrono::duration<float>(now - last_consume_time_).count()
        : 0.0f;
    last_consume_time_ = now;
    has_last_consume_time_ = true;
    for (uint32_t word = 0; word < mmio::kGameButtonWordCount; ++word)
    {
        frame.gamebtn_tapped[word] = pendingTapped_[word].exchange(0, std::memory_order_acq_rel);
        frame.gamebtn_released[word] = pendingReleased_[word].exchange(0, std::memory_order_acq_rel);
        frame.gamebtn_down[word] = currentDown_[word].load(std::memory_order_acquire);
    }

    frame.connected = connected_.load(std::memory_order_acquire);
    if (frame.connected)
    {
        frame.controller_type = ControllerLayout(
            selectedType_.load(std::memory_order_acquire));
    }
    const uint32_t currentTouch = currentTouchCells_.load(std::memory_order_acquire);
    const uint32_t observedTouch = pendingTouchCells_.exchange(0, std::memory_order_acq_rel);
    const uint32_t touchCells = currentTouch | observedTouch;
    for (uint32_t cell = 0; cell < mmio::kTouchCellCount; ++cell)
    {
        frame.touch_cells[cell] = static_cast<uint8_t>((touchCells >> cell) & 1u);
    }
    frame.touchpad_active = touchpadActive_.load(std::memory_order_acquire);
    frame.gamepad_slide = currentGamepadSlide_.load(std::memory_order_acquire);
    UpdateMmIoSliderContact(
        left_contact_,
        (frame.gamepad_slide & mmio::SlideLeft1) != 0,
        (frame.gamepad_slide & mmio::SlideRight1) != 0,
        deltaSeconds,
        arcade_slider_cells_per_second_,
        true,
        0.0f,
        16.0f);
    UpdateMmIoSliderContact(
        right_contact_,
        (frame.gamepad_slide & mmio::SlideLeft2) != 0,
        (frame.gamepad_slide & mmio::SlideRight2) != 0,
        deltaSeconds,
        arcade_slider_cells_per_second_,
        true,
        16.0f,
        32.0f);
    if ((frame.gamepad_slide & (mmio::SlideLeft1 | mmio::SlideRight1)) != 0)
    {
        const auto cell = static_cast<uint32_t>(
            std::floor(left_contact_.position));
        frame.arcade_touch_cells[cell] = 1;
        if (debug_left_arcade_cell_ != static_cast<int>(cell))
        {
            DebugLog(
                "Gamepad ArcadeSlider candidate: stick=1 cell=%u position=%.2f slide=0x%X",
                cell,
                left_contact_.position,
                frame.gamepad_slide);
            debug_left_arcade_cell_ = static_cast<int>(cell);
        }
    }
    else
    {
        debug_left_arcade_cell_ = -1;
    }
    if ((frame.gamepad_slide & (mmio::SlideLeft2 | mmio::SlideRight2)) != 0)
    {
        const auto cell = static_cast<uint32_t>(
            std::floor(right_contact_.position));
        frame.arcade_touch_cells[cell] = 1;
        if (debug_right_arcade_cell_ != static_cast<int>(cell))
        {
            DebugLog(
                "Gamepad ArcadeSlider candidate: stick=2 cell=%u position=%.2f slide=0x%X",
                cell,
                right_contact_.position,
                frame.gamepad_slide);
            debug_right_arcade_cell_ = static_cast<int>(cell);
        }
    }
    else
    {
        debug_right_arcade_cell_ = -1;
    }
    return frame;
}

MmIoJoyShockMotionState MmIoJoyShockFrontend::LatestMotion() const
{
    return {
        LoadFloat(accelXBits_), LoadFloat(accelYBits_), LoadFloat(accelZBits_),
        LoadFloat(gyroXBits_), LoadFloat(gyroYBits_), LoadFloat(gyroZBits_),
    };
}

void MmIoJoyShockFrontend::InputCallback(
    int deviceId,
    JOY_SHOCK_STATE current,
    JOY_SHOCK_STATE previous,
    IMU_STATE imu,
    IMU_STATE,
    float)
{
    if (auto* instance = activeInstance_.load(std::memory_order_acquire))
    {
        instance->OnInput(deviceId, current, previous, imu);
    }
}

void MmIoJoyShockFrontend::TouchCallback(
    int deviceId,
    TOUCH_STATE current,
    TOUCH_STATE,
    float)
{
    if (auto* instance = activeInstance_.load(std::memory_order_acquire))
    {
        instance->OnTouch(deviceId, current);
    }
}

void MmIoJoyShockFrontend::ConnectCallback(int deviceId)
{
    if (auto* instance = activeInstance_.load(std::memory_order_acquire))
    {
        instance->OnConnect(deviceId);
    }
}

void MmIoJoyShockFrontend::DisconnectCallback(int deviceId, bool)
{
    if (auto* instance = activeInstance_.load(std::memory_order_acquire))
    {
        instance->OnDisconnect(deviceId);
    }
}

void MmIoJoyShockFrontend::OnInput(
    int deviceId,
    JOY_SHOCK_STATE current,
    JOY_SHOCK_STATE previous,
    IMU_STATE imu)
{
    if (shuttingDown_.load(std::memory_order_acquire) ||
        deviceId != selectedHandle_.load(std::memory_order_acquire))
    {
        return;
    }

    uint64_t currentButtons[mmio::kGameButtonWordCount]{};
    uint64_t previousButtons[mmio::kGameButtonWordCount]{};
    const int controllerType = selectedType_.load(std::memory_order_acquire);
    MapJoyShockButtons(controllerType, config_, current, currentButtons);
    MapJoyShockButtons(controllerType, config_, previous, previousButtons);
    for (uint32_t word = 0; word < mmio::kGameButtonWordCount; ++word)
    {
        pendingTapped_[word].fetch_or(
            currentButtons[word] & ~previousButtons[word], std::memory_order_acq_rel);
        pendingReleased_[word].fetch_or(
            previousButtons[word] & ~currentButtons[word], std::memory_order_acq_rel);
        currentDown_[word].store(currentButtons[word], std::memory_order_release);
    }
    const uint32_t gamepadSlide = MapStickSlider(config_, current);
    const uint32_t previousGamepadSlide = currentGamepadSlide_.exchange(
        gamepadSlide,
        std::memory_order_acq_rel);
    if (previousGamepadSlide != gamepadSlide)
    {
        DebugLog(
            "Gamepad stick directions: handle=%d axes=(%.3f,%.3f,%.3f,%.3f) mapped=0x%X deadzone=%.3f mode=%s",
            deviceId,
            current.stickLX,
            current.stickLY,
            current.stickRX,
            current.stickRY,
            gamepadSlide,
            config_.stick_slider_deadzone,
            slider_mode_ == MmIoSliderMode::Arcade ? "arcade" : "joystick");
    }
    if (IsDebugLoggingEnabled())
    {
        for (const ButtonDebugEntry& entry : kButtonDebugEntries)
        {
            const bool isDown = mmio::IsGameButtonDown(currentButtons, entry.action);
            const bool wasDown = mmio::IsGameButtonDown(previousButtons, entry.action);
            if (isDown != wasDown)
            {
                DebugLog(
                    "Gamepad button: handle=%d action=%s(%u) %s",
                    deviceId,
                    entry.name,
                    entry.action,
                    isDown ? "down" : "up");
            }
        }
    }
    StoreFloat(accelXBits_, imu.accelX);
    StoreFloat(accelYBits_, imu.accelY);
    StoreFloat(accelZBits_, imu.accelZ);
    StoreFloat(gyroXBits_, imu.gyroX);
    StoreFloat(gyroYBits_, imu.gyroY);
    StoreFloat(gyroZBits_, imu.gyroZ);
}

void MmIoJoyShockFrontend::OnTouch(int deviceId, TOUCH_STATE current)
{
    if (!config_.touchpad_slider || shuttingDown_.load(std::memory_order_acquire) ||
        deviceId != selectedHandle_.load(std::memory_order_acquire))
    {
        return;
    }
    const uint32_t cells = MapTouchCells(config_, current);
    if (current.t0Down || current.t1Down)
    {
        DebugLog(
            "Gamepad touch: handle=%d t0=%s(%.3f,%.3f)->cell=%d t1=%s(%.3f,%.3f)->cell=%d mapped=0x%08X",
            deviceId,
            current.t0Down ? "down" : "up",
            current.t0X,
            current.t0Y,
            current.t0Down ? TouchXToCell(current.t0X, config_.touchpad_invert) : -1,
            current.t1Down ? "down" : "up",
            current.t1X,
            current.t1Y,
            current.t1Down ? TouchXToCell(current.t1X, config_.touchpad_invert) : -1,
            cells);
    }
    currentTouchCells_.store(cells, std::memory_order_release);
    touchpadActive_.store(current.t0Down || current.t1Down, std::memory_order_release);
    if (cells != 0)
    {
        pendingTouchCells_.fetch_or(cells, std::memory_order_acq_rel);
    }
}

void MmIoJoyShockFrontend::OnConnect(int deviceId)
{
    Log("JoyShock: controller connected handle=%d", deviceId);
    RequestDiscovery();
}

void MmIoJoyShockFrontend::OnDisconnect(int deviceId)
{
    if (deviceId != selectedHandle_.load(std::memory_order_acquire))
    {
        return;
    }
    Log("JoyShock: selected controller disconnected handle=%d", deviceId);
    ClearControllerState(true);
    selectedHandle_.store(-1, std::memory_order_release);
    selectedType_.store(kUnknownController, std::memory_order_release);
    connected_.store(false, std::memory_order_release);
    RequestDiscovery();
}

void MmIoJoyShockFrontend::DiscoveryLoop(std::stop_token stopToken)
{
    while (!stopToken.stop_requested())
    {
        {
            std::unique_lock lock(discoveryMutex_);
            discoveryCondition_.wait_for(lock, stopToken, std::chrono::seconds(2), [this]
            {
                return discoveryRequested_;
            });
            discoveryRequested_ = false;
        }
        if (stopToken.stop_requested() || shuttingDown_.load(std::memory_order_acquire))
        {
            break;
        }
        if (!connected_.load(std::memory_order_acquire))
        {
            DiscoverControllers();
        }
    }
}

void MmIoJoyShockFrontend::DiscoverControllers()
{
    const int connectedCount = JslConnectDevices();
    int handles[kMaxConnectedControllers]{};
    const int handleCount = JslGetConnectedDeviceHandles(handles, kMaxConnectedControllers);
    if (connectedCount > 0 || handleCount > 0)
    {
        Log("JoyShock: %d controller(s) available", handleCount);
    }

    int selectedHandle = -1;
    int selectedType = kUnknownController;
    int selectedPriority = 1000;
    for (int index = 0; index < handleCount; ++index)
    {
        const int type = JslGetControllerType(handles[index]);
        Log("JoyShock: controller found handle=%d type=%s", handles[index], ControllerTypeName(type));
        const int priority = ControllerPriority(type);
        if (priority < selectedPriority)
        {
            selectedHandle = handles[index];
            selectedType = type;
            selectedPriority = priority;
        }
    }
    if (selectedHandle >= 0)
    {
        SelectController(selectedHandle, selectedType);
        Log("JoyShock: selected handle=%d type=%s", selectedHandle, ControllerTypeName(selectedType));
    }
}

void MmIoJoyShockFrontend::RequestDiscovery()
{
    {
        std::lock_guard lock(discoveryMutex_);
        discoveryRequested_ = true;
    }
    discoveryCondition_.notify_all();
}

void MmIoJoyShockFrontend::SelectController(int deviceId, int controllerType)
{
    if (connected_.load(std::memory_order_acquire) &&
        selectedHandle_.load(std::memory_order_acquire) == deviceId)
    {
        return;
    }
    ClearControllerState(false);
    selectedType_.store(controllerType, std::memory_order_release);
    selectedHandle_.store(deviceId, std::memory_order_release);
    connected_.store(true, std::memory_order_release);
}

void MmIoJoyShockFrontend::ClearControllerState(bool preserveRelease)
{
    std::lock_guard sliderLock(slider_mutex_);
    for (uint32_t word = 0; word < mmio::kGameButtonWordCount; ++word)
    {
        const uint64_t held = currentDown_[word].exchange(0, std::memory_order_acq_rel);
        if (preserveRelease && held != 0)
        {
            pendingReleased_[word].fetch_or(held, std::memory_order_acq_rel);
        }
        else
        {
            pendingTapped_[word].store(0, std::memory_order_release);
            pendingReleased_[word].store(0, std::memory_order_release);
        }
    }
    currentTouchCells_.store(0, std::memory_order_release);
    pendingTouchCells_.store(0, std::memory_order_release);
    currentGamepadSlide_.store(0, std::memory_order_release);
    touchpadActive_.store(false, std::memory_order_release);
    ResetMmIoSliderContact(left_contact_, 0.0f, 16.0f);
    ResetMmIoSliderContact(right_contact_, 16.0f, 32.0f);
    last_consume_time_ = {};
    has_last_consume_time_ = false;
    debug_left_arcade_cell_ = -1;
    debug_right_arcade_cell_ = -1;
}
}
