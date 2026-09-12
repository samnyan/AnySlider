#include "pch.h"

#include "mm_io_keyboard.h"
#include "mm_io_raw_input.h"

#include "mm_io_shared_memory.h"
#include "mm_io_window_hooks.h"
#include "anyslider_log.h"

#include <algorithm>
#include <cmath>

namespace anyslider
{
namespace
{
bool IsDown(const MmIoRawKeyboardSnapshot& snapshot, int virtualKey)
{
    return virtualKey >= 0 && virtualKey < static_cast<int>(snapshot.down.size()) &&
        snapshot.down[virtualKey];
}

bool WasPressed(const MmIoRawKeyboardSnapshot& snapshot, int virtualKey)
{
    return virtualKey >= 0 && virtualKey < static_cast<int>(snapshot.pressed.size()) &&
        snapshot.pressed[virtualKey];
}

bool IsDown(const MmIoRawKeyboardSnapshot& snapshot, const MmIoKeyBinding& binding)
{
    return std::any_of(binding.begin(), binding.end(), [&](int key)
    {
        return IsDown(snapshot, key);
    });
}

void DebugLogPressedKeys(
    const MmIoRawKeyboardSnapshot& snapshot,
    const MmIoKeyBinding& binding,
    const char* name)
{
    for (const int key : binding)
    {
        if (WasPressed(snapshot, key))
        {
            DebugLog("Keyboard press: action=%s vk=%d", name, key);
        }
    }
}

int64_t SelectMouseAxis(const MmIoRawMouseDelta& delta, MmIoMouseAxis axis)
{
    switch (axis)
    {
    case MmIoMouseAxis::X: return delta.x;
    case MmIoMouseAxis::Y: return delta.y;
    case MmIoMouseAxis::Wheel: return delta.wheel;
    default: return 0;
    }
}
}

MmIoKeyboardBindings DefaultMmIoKeyboardBindings()
{
    MmIoKeyboardBindings bindings;
    bindings.test = { VK_F1 };
    bindings.service = { VK_F2 };
    bindings.pause = { VK_ESCAPE };
    bindings.start = { VK_RETURN };
    bindings.dpad_up = { VK_UP };
    bindings.dpad_down = { VK_DOWN };
    bindings.dpad_left = { VK_LEFT };
    bindings.dpad_right = { VK_RIGHT };
    bindings.triangle = { 'W' };
    bindings.square = { 'A' };
    bindings.cross = { 'S' };
    bindings.circle = { 'D' };
    bindings.l1 = { 'Z' };
    bindings.r1 = { 'X' };
    bindings.slider_1_left = { 'Q' };
    bindings.slider_1_right = { 'E' };
    bindings.slider_2_left = { 'U' };
    bindings.slider_2_right = { 'O' };
    return bindings;
}

void MmIoKeyboardMouseFrontend::Initialize(
    bool enabled,
    float sliderCellsPerSecond,
    const MmIoKeyboardBindings& bindings,
    const MmIoMouseSliderConfig& mouseSliderConfig,
    MmIoSliderMode sliderMode)
{
    enabled_ = enabled;
    slider_cells_per_second_ = sliderCellsPerSecond;
    bindings_ = bindings;
    mouse_slider_ = mouseSliderConfig;
    slider_mode_ = sliderMode;
    mouse_slider_enabled_ = enabled && mouseSliderConfig.enabled;
    last_poll_time_ = {};
    has_last_poll_time_ = false;
    logical_buttons_ = {};
    ResetMmIoRawMouseInput();
    left_contact_ = {};
    left_contact_.movement.position = 7.5f;
    right_contact_ = {};
    right_contact_.movement.position = 23.5f;
    debug_left_arcade_cell_ = -1;
    debug_right_arcade_cell_ = -1;
}

bool MmIoKeyboardMouseFrontend::IsEnabled() const
{
    return enabled_;
}

bool MmIoKeyboardMouseFrontend::IsMouseSliderEnabled() const
{
    return mouse_slider_enabled_;
}

void MmIoKeyboardMouseFrontend::DisableMouseSlider()
{
    mouse_slider_enabled_ = false;
}

void MmIoKeyboardMouseFrontend::UpdateContact(
    SliderContact& contact,
    const MmIoRawKeyboardSnapshot& keyboard,
    const MmIoKeyBinding& leftBinding,
    const MmIoKeyBinding& rightBinding,
    float deltaSeconds,
    float startPosition,
    float endPosition,
    bool resetOnTap)
{
    const bool left = IsDown(keyboard, leftBinding);
    const bool right = IsDown(keyboard, rightBinding);
    UpdateMmIoSliderContact(
        contact.movement,
        left,
        right,
        deltaSeconds,
        slider_cells_per_second_,
        resetOnTap,
        startPosition,
        endPosition);
}

void MmIoKeyboardMouseFrontend::UpdateMouseContact(
    SliderContact& contact,
    int64_t rawDelta,
    const MmIoMouseSliderBinding& binding,
    float startPosition,
    float endPosition,
    std::chrono::steady_clock::time_point now)
{
    if (!mouse_slider_enabled_ || rawDelta == 0)
    {
        return;
    }
    const float range = endPosition - startPosition;
    if (!contact.mouse_active)
    {
        contact.movement.position = startPosition + (range - 1.0f) * 0.5f;
        DebugLog("Mouse slider activated at %.3f", contact.movement.position);
    }
    contact.mouse_active = true;
    contact.last_mouse_move_time = now;
    const float direction = binding.invert ? -1.0f : 1.0f;
    contact.movement.position += static_cast<float>(rawDelta) *
        (range / mouse_slider_.counts_per_cycle) *
        binding.sensitivity * direction;
    contact.movement.position = WrapMmIoSliderPosition(
        contact.movement.position, startPosition, endPosition);
}

void MmIoKeyboardMouseFrontend::UpdateMouseContactRelease(
    SliderContact& contact,
    bool movedThisPoll,
    std::chrono::steady_clock::time_point now)
{
    if (!mouse_slider_enabled_ || !contact.mouse_active || movedThisPoll)
    {
        return;
    }
    if (mouse_slider_.touch_hold_ms == 0 ||
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now - contact.last_mouse_move_time).count() >= mouse_slider_.touch_hold_ms)
    {
        contact.mouse_active = false;
        DebugLog("Mouse slider released");
    }
}

MmIoKeyboardFrame MmIoKeyboardMouseFrontend::Poll()
{
    MmIoKeyboardFrame frame{};
    mmio::InputSnapshot& snapshot = frame.snapshot;
    if (!enabled_)
    {
        return frame;
    }

    const auto now = std::chrono::steady_clock::now();
    const float deltaSeconds = has_last_poll_time_
        ? std::chrono::duration<float>(now - last_poll_time_).count()
        : 0.0f;
    last_poll_time_ = now;
    has_last_poll_time_ = true;
    const auto keyboard = ConsumeMmIoRawKeyboardSnapshot();
    DebugLogPressedKeys(keyboard, bindings_.slider_1_left, "Slider1Left");
    DebugLogPressedKeys(keyboard, bindings_.slider_1_right, "Slider1Right");
    DebugLogPressedKeys(keyboard, bindings_.slider_2_left, "Slider2Left");
    DebugLogPressedKeys(keyboard, bindings_.slider_2_right, "Slider2Right");
    const bool calculateKeyboardSlider = slider_mode_ == MmIoSliderMode::Arcade;
    if (calculateKeyboardSlider)
    {
        UpdateContact(
            left_contact_,
            keyboard,
            bindings_.slider_1_left,
            bindings_.slider_1_right,
            deltaSeconds,
            0.0f,
            16.0f,
            !mouse_slider_enabled_);
    }

    frame.slider_direction =
        GetMmIoSliderDirection(
            IsDown(keyboard, bindings_.slider_1_left),
            IsDown(keyboard, bindings_.slider_1_right),
            mmio::SlideLeft1,
            mmio::SlideRight1) |
        GetMmIoSliderDirection(
            IsDown(keyboard, bindings_.slider_2_left),
            IsDown(keyboard, bindings_.slider_2_right),
            mmio::SlideLeft2,
            mmio::SlideRight2);
    if (calculateKeyboardSlider)
    {
        UpdateContact(
            right_contact_,
            keyboard,
            bindings_.slider_2_left,
            bindings_.slider_2_right,
            deltaSeconds,
            16.0f,
            32.0f,
            !mouse_slider_enabled_);
    }

    const auto mouseDelta = ConsumeMmIoRawMouseDelta();
    if (mouseDelta.x != 0 || mouseDelta.y != 0 || mouseDelta.wheel != 0)
    {
        DebugLog("Mouse slider delta: x=%lld y=%lld wheel=%lld",
            mouseDelta.x, mouseDelta.y, mouseDelta.wheel);
    }
    const int64_t slider1Delta = SelectMouseAxis(mouseDelta, mouse_slider_.slider_1.axis);
    const int64_t slider2Delta = SelectMouseAxis(mouseDelta, mouse_slider_.slider_2.axis);
    UpdateMouseContact(
        left_contact_, slider1Delta, mouse_slider_.slider_1, 0.0f, 16.0f, now);
    UpdateMouseContact(
        right_contact_, slider2Delta, mouse_slider_.slider_2, 16.0f, 32.0f, now);
    UpdateMouseContactRelease(left_contact_, slider1Delta != 0, now);
    UpdateMouseContactRelease(right_contact_, slider2Delta != 0, now);
    struct Binding
    {
        const MmIoKeyBinding* keys;
        uint32_t action;
        const char* name;
    };
    const Binding bindings[] = {
        { &bindings_.test, mmio::Test, "Test" },
        { &bindings_.service, mmio::Service, "Service" },
        { &bindings_.pause, mmio::Pause, "Pause" },
        { &bindings_.start, mmio::Start, "Start" },
        { &bindings_.dpad_up, mmio::DpadUp, "DpadUp" },
        { &bindings_.dpad_down, mmio::DpadDown, "DpadDown" },
        { &bindings_.dpad_left, mmio::DpadLeft, "DpadLeft" },
        { &bindings_.dpad_right, mmio::DpadRight, "DpadRight" },
        { &bindings_.triangle, mmio::Triangle, "Triangle" },
        { &bindings_.square, mmio::Square, "Square" },
        { &bindings_.cross, mmio::Cross, "Cross" },
        { &bindings_.circle, mmio::Circle, "Circle" },
        { &bindings_.l1, mmio::L1, "L1" },
        { &bindings_.r1, mmio::R1, "R1" },
        { &bindings_.l2, mmio::L2, "L2" },
        { &bindings_.r2, mmio::R2, "R2" },
        { &bindings_.select, mmio::Select, "Select" },
        { &bindings_.l3, mmio::L3, "L3" },
        { &bindings_.r3, mmio::R3, "R3" },
    };

    snapshot.mode = static_cast<uint32_t>(mmio::Mode::ArcadeSlider);
    for (size_t index = 0; index < std::size(bindings); ++index)
    {
        const Binding& binding = bindings[index];
        auto& state = logical_buttons_[index];
        int newest_pressed_key = -1;
        for (const int key : *binding.keys)
        {
            if (WasPressed(keyboard, key))
            {
                newest_pressed_key = key;
                DebugLog(
                    "Keyboard press: action=%s vk=%d",
                    binding.name,
                    key);
            }
        }

        if (newest_pressed_key >= 0 && newest_pressed_key != state.active_key)
        {
            // Transfer ownership and expose one UP poll so the game sees a new press edge.
            state.active_key = newest_pressed_key;
            state.retrigger = state.down;
        }

        const bool active_down = IsDown(keyboard, state.active_key);
        if (state.retrigger)
        {
            state.retrigger = false;
            state.down = false;
        }
        else
        {
            state.down = active_down;
            if (!active_down)
            {
                state.active_key = -1;
            }
        }
        if (state.down)
        {
            mmio::SetGameButton(snapshot.gamebtn, binding.action);
        }
    }

    const bool leftContactActive =
        left_contact_.mouse_active ||
        (calculateKeyboardSlider &&
            (IsDown(keyboard, bindings_.slider_1_left) ||
                IsDown(keyboard, bindings_.slider_1_right)));
    const bool rightContactActive =
        right_contact_.mouse_active ||
        (calculateKeyboardSlider &&
            (IsDown(keyboard, bindings_.slider_2_left) ||
                IsDown(keyboard, bindings_.slider_2_right)));
    if (leftContactActive)
    {
        const auto sensor = static_cast<unsigned int>(std::floor(left_contact_.movement.position));
        snapshot.touch_cells[sensor] = 1;
        if (debug_left_arcade_cell_ != static_cast<int>(sensor))
        {
            DebugLog(
                "Keyboard ArcadeSlider candidate: slider=1 cell=%u position=%.2f source=%s",
                sensor,
                left_contact_.movement.position,
                left_contact_.mouse_active ? "mouse" : "keyboard");
            debug_left_arcade_cell_ = static_cast<int>(sensor);
        }
        if (left_contact_.mouse_active)
        {
            frame.direct_touch_cells[sensor] = 1;
        }
    }
    else
    {
        debug_left_arcade_cell_ = -1;
    }
    if (rightContactActive)
    {
        const auto sensor = static_cast<unsigned int>(std::floor(right_contact_.movement.position));
        snapshot.touch_cells[sensor] = 1;
        if (debug_right_arcade_cell_ != static_cast<int>(sensor))
        {
            DebugLog(
                "Keyboard ArcadeSlider candidate: slider=2 cell=%u position=%.2f source=%s",
                sensor,
                right_contact_.movement.position,
                right_contact_.mouse_active ? "mouse" : "keyboard");
            debug_right_arcade_cell_ = static_cast<int>(sensor);
        }
        if (right_contact_.mouse_active)
        {
            frame.direct_touch_cells[sensor] = 1;
        }
    }
    else
    {
        debug_right_arcade_cell_ = -1;
    }

    for (size_t sensor = 0; sensor < bindings_.slider_cells.size(); ++sensor)
    {
        if (IsDown(keyboard, bindings_.slider_cells[sensor]))
        {
            snapshot.touch_cells[sensor] = 1;
            frame.direct_touch_cells[sensor] = 1;
            for (const int key : bindings_.slider_cells[sensor])
            {
                if (WasPressed(keyboard, key))
                {
                    DebugLog(
                        "Keyboard press: action=SliderCell%02zu cell=%zu vk=%d",
                        sensor + 1,
                        sensor,
                        key);
                }
            }
        }
    }

    snapshot.source_id = 0x4B424D4D; // MMBK
    snapshot.timestamp_us = MmIoNowMicroseconds();
    snapshot.lease_ms = 500;
    return frame;
}
}
