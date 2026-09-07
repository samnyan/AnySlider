#pragma once

#include "mm_io/shared_memory.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace anyslider
{
struct MmIoRawKeyboardSnapshot;
using MmIoKeyBinding = std::vector<int>;

struct MmIoKeyboardBindings
{
    MmIoKeyBinding test;
    MmIoKeyBinding service;
    MmIoKeyBinding pause;
    MmIoKeyBinding start;
    MmIoKeyBinding dpad_up;
    MmIoKeyBinding dpad_down;
    MmIoKeyBinding dpad_left;
    MmIoKeyBinding dpad_right;
    MmIoKeyBinding triangle;
    MmIoKeyBinding square;
    MmIoKeyBinding cross;
    MmIoKeyBinding circle;
    MmIoKeyBinding l1;
    MmIoKeyBinding r1;
    MmIoKeyBinding l2;
    MmIoKeyBinding r2;
    MmIoKeyBinding select;
    MmIoKeyBinding l3;
    MmIoKeyBinding r3;
    MmIoKeyBinding slider_1_left;
    MmIoKeyBinding slider_1_right;
    MmIoKeyBinding slider_2_left;
    MmIoKeyBinding slider_2_right;
    std::array<MmIoKeyBinding, 32> slider_cells;
};

enum class MmIoMouseAxis
{
    X,
    Y,
    Wheel,
};

struct MmIoMouseSliderBinding
{
    MmIoMouseAxis axis = MmIoMouseAxis::X;
    bool invert = false;
    float sensitivity = 1.0f;
};

struct MmIoMouseSliderConfig
{
    bool enabled = false;
    MmIoMouseSliderBinding slider_1{ MmIoMouseAxis::X, false, 1.0f };
    MmIoMouseSliderBinding slider_2{ MmIoMouseAxis::Y, false, 1.0f };
    float counts_per_cycle = 256.0f;
    uint32_t touch_hold_ms = 0;
    std::wstring device_filter;
};

struct MmIoRawMouseDelta
{
    int64_t x = 0;
    int64_t y = 0;
    int64_t wheel = 0;
};

MmIoKeyboardBindings DefaultMmIoKeyboardBindings();

class MmIoKeyboardMouseFrontend
{
public:
    void Initialize(
        bool enabled,
        float sliderCellsPerSecond,
        const MmIoKeyboardBindings& bindings,
        const MmIoMouseSliderConfig& mouseSliderConfig);
    [[nodiscard]] bool IsEnabled() const;
    [[nodiscard]] bool IsMouseSliderEnabled() const;
    void DisableMouseSlider();
    mmio::InputSnapshot Poll();

private:
    struct SliderContact
    {
        float position = 0.0f;
        // Keyboard slider state.
        bool left_down = false;
        bool right_down = false;
        // Mouse-slider simulated touch state.
        bool mouse_active = false;
        std::chrono::steady_clock::time_point last_mouse_move_time{};
    };

    bool enabled_ = false;
    float slider_cells_per_second_ = 18.0f;
    std::chrono::steady_clock::time_point last_poll_time_{};
    bool has_last_poll_time_ = false;
    MmIoKeyboardBindings bindings_;
    MmIoMouseSliderConfig mouse_slider_;
    bool mouse_slider_enabled_ = false;
    SliderContact left_contact_{ 7.5f };
    SliderContact right_contact_{ 23.5f };
    struct LogicalButtonState
    {
        int active_key = -1;
        bool down = false;
        bool retrigger = false;
    };

    std::array<LogicalButtonState, 19> logical_buttons_{};

    void UpdateContact(
        SliderContact& contact,
        const MmIoRawKeyboardSnapshot& keyboard,
        const MmIoKeyBinding& leftBinding,
        const MmIoKeyBinding& rightBinding,
        float deltaSeconds,
        float startPosition,
        float endPosition,
        bool resetOnTap);

    void UpdateMouseContact(
        SliderContact& contact,
        int64_t rawDelta,
        const MmIoMouseSliderBinding& binding,
        float startPosition,
        float endPosition,
        std::chrono::steady_clock::time_point now);

    void UpdateMouseContactRelease(
        SliderContact& contact,
        bool movedThisPoll,
        std::chrono::steady_clock::time_point now);
};
}
