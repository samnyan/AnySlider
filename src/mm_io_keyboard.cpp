#include "pch.h"

#include "mm_io_keyboard.h"

#include "mm_io_shared_memory.h"
#include "mm_io_window_hooks.h"

#include <algorithm>

namespace anyslider
{
namespace
{
bool IsDown(int virtualKey)
{
    return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
}

bool IsDown(const MmIoKeyBinding& binding)
{
    return std::any_of(binding.begin(), binding.end(), [](int virtualKey)
    {
        return IsDown(virtualKey);
    });
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

void MmIoKeyboardFrontend::Initialize(
    bool enabled,
    float sliderCellsPerSecond,
    const MmIoKeyboardBindings& bindings)
{
    enabled_ = enabled;
    slider_cells_per_second_ = sliderCellsPerSecond;
    bindings_ = bindings;
    last_poll_us_ = 0;
}

bool MmIoKeyboardFrontend::IsEnabled() const
{
    return enabled_;
}

void MmIoKeyboardFrontend::UpdateContact(
    SliderContact& contact,
    const MmIoKeyBinding& leftBinding,
    const MmIoKeyBinding& rightBinding,
    float deltaSeconds)
{
    const bool left = IsDown(leftBinding);
    const bool right = IsDown(rightBinding);
    if (left == right)
    {
        return;
    }

    const float direction = left ? -1.0f : 1.0f;
    contact.position = std::clamp(
        contact.position + direction * slider_cells_per_second_ * deltaSeconds,
        0.0f,
        31.0f);
}

mmio::InputSnapshot MmIoKeyboardFrontend::Poll()
{
    mmio::InputSnapshot snapshot{};
    if (!enabled_)
    {
        return snapshot;
    }

    const ScopedMmIoKeyboardPoll keyboardPoll;
    const uint64_t nowUs = MmIoNowMicroseconds();
    const float deltaSeconds = last_poll_us_ == 0
        ? 0.0f
        : static_cast<float>(nowUs - last_poll_us_) / 1'000'000.0f;
    last_poll_us_ = nowUs;
    UpdateContact(
        left_contact_,
        bindings_.slider_1_left,
        bindings_.slider_1_right,
        deltaSeconds);
    UpdateContact(
        right_contact_,
        bindings_.slider_2_left,
        bindings_.slider_2_right,
        deltaSeconds);

    struct Binding
    {
        const MmIoKeyBinding* keys;
        uint32_t action;
    };
    const Binding bindings[] = {
        { &bindings_.test, mmio::Test },
        { &bindings_.service, mmio::Service },
        { &bindings_.pause, mmio::Pause },
        { &bindings_.start, mmio::Start },
        { &bindings_.dpad_up, mmio::DpadUp },
        { &bindings_.dpad_down, mmio::DpadDown },
        { &bindings_.dpad_left, mmio::DpadLeft },
        { &bindings_.dpad_right, mmio::DpadRight },
        { &bindings_.triangle, mmio::Triangle },
        { &bindings_.square, mmio::Square },
        { &bindings_.cross, mmio::Cross },
        { &bindings_.circle, mmio::Circle },
        { &bindings_.l1, mmio::L1 },
        { &bindings_.r1, mmio::R1 },
        { &bindings_.l2, mmio::L2 },
        { &bindings_.r2, mmio::R2 },
        { &bindings_.select, mmio::Select },
        { &bindings_.l3, mmio::L3 },
        { &bindings_.r3, mmio::R3 },
    };

    snapshot.mode = static_cast<uint32_t>(mmio::Mode::ArcadeSlider);
    for (const Binding& binding : bindings)
    {
        if (IsDown(*binding.keys))
        {
            mmio::SetGameButton(snapshot.gamebtn, binding.action);
        }
    }

    const bool leftContactActive =
        IsDown(bindings_.slider_1_left) || IsDown(bindings_.slider_1_right);
    const bool rightContactActive =
        IsDown(bindings_.slider_2_left) || IsDown(bindings_.slider_2_right);
    if (leftContactActive)
    {
        const auto sensor = static_cast<unsigned int>(left_contact_.position + 0.5f);
        snapshot.touch_cells[sensor] = 1;
    }
    if (rightContactActive)
    {
        const auto sensor = static_cast<unsigned int>(right_contact_.position + 0.5f);
        snapshot.touch_cells[sensor] = 1;
    }

    for (size_t sensor = 0; sensor < bindings_.slider_cells.size(); ++sensor)
    {
        if (IsDown(bindings_.slider_cells[sensor]))
        {
            snapshot.touch_cells[sensor] = 1;
        }
    }

    snapshot.source_id = 0x4B424D4D; // MMBK
    snapshot.timestamp_us = nowUs;
    snapshot.lease_ms = 500;
    return snapshot;
}
}
