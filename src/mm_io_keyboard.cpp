#include "pch.h"

#include "mm_io_keyboard.h"

#include "mm_io_shared_memory.h"
#include "mm_io_window_hooks.h"
#include "anyslider_log.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwctype>
#include <mutex>
#include <unordered_map>

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

float WrapContactPosition(float position, float startPosition, float length)
{
    return startPosition + std::fmod(
        std::fmod(position - startPosition, length) + length,
        length);
}

std::atomic<int64_t> pending_mouse_x{ 0 };
std::atomic<int64_t> pending_mouse_y{ 0 };
std::atomic<int64_t> pending_mouse_wheel{ 0 };
std::mutex mouse_device_cache_mutex;
std::unordered_map<HANDLE, bool> mouse_device_match_cache;
MmIoMouseSliderConfig mouse_slider_config;

bool ContainsCaseInsensitive(const std::wstring& value, const std::wstring& search)
{
    if (search.empty())
    {
        return true;
    }
    return std::search(
        value.begin(), value.end(),
        search.begin(), search.end(),
        [](wchar_t left, wchar_t right)
        {
            return std::towlower(left) == std::towlower(right);
        }) != value.end();
}

bool MatchesMouseSliderDevice(HANDLE device)
{
    if (!device)
    {
        static bool loggedNullDevice = false;
        if (!loggedNullDevice)
        {
            loggedNullDevice = true;
            DebugLog("Ignoring Raw mouse with null device handle.");
        }
        return false;
    }
    {
        std::lock_guard lock(mouse_device_cache_mutex);
        if (const auto it = mouse_device_match_cache.find(device); it != mouse_device_match_cache.end())
        {
            return it->second;
        }
    }

    UINT length = 0;
    if (GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, nullptr, &length) == static_cast<UINT>(-1) || length == 0)
    {
        Log("Could not read Raw mouse device name: device=%p error=%lu", device, GetLastError());
        return false;
    }
    std::wstring name(length, L'\0');
    if (GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, name.data(), &length) == static_cast<UINT>(-1))
    {
        Log("Could not read Raw mouse device name: device=%p error=%lu", device, GetLastError());
        return false;
    }
    name.resize(std::wcslen(name.c_str()));
    const bool matches = mouse_slider_config.device_filter.empty() ||
        ContainsCaseInsensitive(name, mouse_slider_config.device_filter);
    DebugLog("Raw mouse detected: device=%p name=%ls mouse-slider=%s",
        device, name.c_str(), matches ? "yes" : "no");
    {
        std::lock_guard lock(mouse_device_cache_mutex);
        mouse_device_match_cache.emplace(device, matches);
    }
    return matches;
}

MmIoRawMouseDelta ConsumeMmIoRawMouseDelta()
{
    return {
        pending_mouse_x.exchange(0, std::memory_order_relaxed),
        pending_mouse_y.exchange(0, std::memory_order_relaxed),
        pending_mouse_wheel.exchange(0, std::memory_order_relaxed),
    };
}

void ResetMmIoRawMouseInput()
{
    pending_mouse_x.store(0, std::memory_order_relaxed);
    pending_mouse_y.store(0, std::memory_order_relaxed);
    pending_mouse_wheel.store(0, std::memory_order_relaxed);
    std::lock_guard lock(mouse_device_cache_mutex);
    mouse_device_match_cache.clear();
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

void ProcessMmIoRawMouseInput(HRAWINPUT rawInput)
{
    if (!mouse_slider_config.enabled || !rawInput)
    {
        return;
    }
    UINT size = sizeof(RAWINPUT);
    RAWINPUT input{};
    const UINT result = GetRawInputData(
        rawInput, RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER));
    if (result == static_cast<UINT>(-1))
    {
        Log("GetRawInputData failed: error=%lu", GetLastError());
        return;
    }
    if (input.header.dwType != RIM_TYPEMOUSE ||
        !MatchesMouseSliderDevice(input.header.hDevice))
    {
        return;
    }
    const RAWMOUSE& mouse = input.data.mouse;
    static uint32_t debugCount = 0;
    if (debugCount++ < 100)
    {
        DebugLog("Raw mouse: device=%p flags=0x%04X dx=%ld dy=%ld buttons=0x%04X",
            input.header.hDevice, mouse.usFlags, mouse.lLastX, mouse.lLastY, mouse.usButtonFlags);
    }
    if ((mouse.usFlags & MOUSE_MOVE_ABSOLUTE) != 0)
    {
        if (debugCount <= 100)
        {
            DebugLog("Ignoring absolute raw mouse: x=%ld y=%ld", mouse.lLastX, mouse.lLastY);
        }
    }
    else
    {
        pending_mouse_x.fetch_add(mouse.lLastX, std::memory_order_relaxed);
        pending_mouse_y.fetch_add(mouse.lLastY, std::memory_order_relaxed);
    }
    if ((mouse.usButtonFlags & RI_MOUSE_WHEEL) != 0)
    {
        pending_mouse_wheel.fetch_add(static_cast<SHORT>(mouse.usButtonData), std::memory_order_relaxed);
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
    const MmIoMouseSliderConfig& mouseSliderConfig)
{
    enabled_ = enabled;
    slider_cells_per_second_ = sliderCellsPerSecond;
    bindings_ = bindings;
    mouse_slider_ = mouseSliderConfig;
    mouse_slider_enabled_ = enabled && mouseSliderConfig.enabled;
    mouse_slider_config = mouseSliderConfig;
    last_poll_time_ = {};
    has_last_poll_time_ = false;
    ResetMmIoRawMouseInput();
    left_contact_ = { 7.5f, false, false };
    right_contact_ = { 23.5f, false, false };
}

bool MmIoKeyboardMouseFrontend::IsEnabled() const
{
    return enabled_;
}

bool MmIoKeyboardMouseFrontend::IsMouseSliderEnabled() const
{
    return mouse_slider_enabled_;
}

void MmIoKeyboardMouseFrontend::UpdateContact(
    SliderContact& contact,
    const MmIoKeyBinding& leftBinding,
    const MmIoKeyBinding& rightBinding,
    float deltaSeconds,
    float startPosition,
    float endPosition,
    bool resetOnTap)
{
    const bool left = IsDown(leftBinding);
    const bool right = IsDown(rightBinding);
    const float range = endPosition - startPosition;

    const bool leftTapped = left && !contact.left_down;
    const bool rightTapped = right && !contact.right_down;
    contact.left_down = left;
    contact.right_down = right;

    if (resetOnTap && (leftTapped || rightTapped))
    {
        contact.position = startPosition + (range - 1.0f) * 0.5f;
    }

    if (left == right)
    {
        return;
    }

    const float direction = left ? -1.0f : 1.0f;
    contact.position += direction * slider_cells_per_second_ * deltaSeconds;
    contact.position = WrapContactPosition(contact.position, startPosition, range);
}

void MmIoKeyboardMouseFrontend::UpdateMouseContact(
    SliderContact& contact,
    int64_t rawDelta,
    const MmIoMouseSliderBinding& binding,
    float startPosition,
    float endPosition)
{
    if (!mouse_slider_enabled_ || rawDelta == 0)
    {
        return;
    }
    const float range = endPosition - startPosition;
    const float direction = binding.invert ? -1.0f : 1.0f;
    contact.position += static_cast<float>(rawDelta) *
        (range / mouse_slider_.counts_per_cycle) *
        binding.sensitivity * direction;
    contact.position = WrapContactPosition(contact.position, startPosition, range);
}

mmio::InputSnapshot MmIoKeyboardMouseFrontend::Poll()
{
    mmio::InputSnapshot snapshot{};
    if (!enabled_)
    {
        return snapshot;
    }

    const ScopedMmIoKeyboardMousePoll keyboardPoll;
    const auto now = std::chrono::steady_clock::now();
    const float deltaSeconds = has_last_poll_time_
        ? std::chrono::duration<float>(now - last_poll_time_).count()
        : 0.0f;
    last_poll_time_ = now;
    has_last_poll_time_ = true;
    UpdateContact(
        left_contact_,
        bindings_.slider_1_left,
        bindings_.slider_1_right,
        deltaSeconds,
        0.0f,
        16.0f,
        !mouse_slider_enabled_);
    UpdateContact(
        right_contact_,
        bindings_.slider_2_left,
        bindings_.slider_2_right,
        deltaSeconds,
        16.0f,
        32.0f,
        !mouse_slider_enabled_);

    const auto mouseDelta = ConsumeMmIoRawMouseDelta();
    if (mouseDelta.x != 0 || mouseDelta.y != 0 || mouseDelta.wheel != 0)
    {
        DebugLog("Mouse slider delta: x=%lld y=%lld wheel=%lld",
            mouseDelta.x, mouseDelta.y, mouseDelta.wheel);
    }
    UpdateMouseContact(
        left_contact_,
        SelectMouseAxis(mouseDelta, mouse_slider_.slider_1.axis),
        mouse_slider_.slider_1,
        0.0f,
        16.0f);
    UpdateMouseContact(
        right_contact_,
        SelectMouseAxis(mouseDelta, mouse_slider_.slider_2.axis),
        mouse_slider_.slider_2,
        16.0f,
        32.0f);
    if (mouseDelta.x != 0 || mouseDelta.y != 0 || mouseDelta.wheel != 0)
    {
        DebugLog("Mouse slider positions: left=%.3f right=%.3f",
            left_contact_.position, right_contact_.position);
    }
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
        mouse_slider_enabled_ ||
        IsDown(bindings_.slider_1_left) || IsDown(bindings_.slider_1_right);
    const bool rightContactActive =
        mouse_slider_enabled_ ||
        IsDown(bindings_.slider_2_left) || IsDown(bindings_.slider_2_right);
    if (leftContactActive)
    {
        const auto sensor = static_cast<unsigned int>(std::floor(left_contact_.position));
        snapshot.touch_cells[sensor] = 1;
    }
    if (rightContactActive)
    {
        const auto sensor = static_cast<unsigned int>(std::floor(right_contact_.position));
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
    snapshot.timestamp_us = MmIoNowMicroseconds();
    snapshot.lease_ms = 500;
    return snapshot;
}
}
