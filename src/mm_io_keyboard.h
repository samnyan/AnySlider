#pragma once

#include "mm_io/shared_memory.h"

#include <array>
#include <vector>

namespace anyslider
{
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

MmIoKeyboardBindings DefaultMmIoKeyboardBindings();

class MmIoKeyboardMouseFrontend
{
public:
    void Initialize(
        bool enabled,
        float sliderCellsPerSecond,
        const MmIoKeyboardBindings& bindings);
    [[nodiscard]] bool IsEnabled() const;
    mmio::InputSnapshot Poll();

private:
    struct SliderContact
    {
        float position;
    };

    bool enabled_ = false;
    float slider_cells_per_second_ = 18.0f;
    uint64_t last_poll_us_ = 0;
    MmIoKeyboardBindings bindings_;
    SliderContact left_contact_{ 7.5f };
    SliderContact right_contact_{ 23.5f };

    void UpdateContact(
        SliderContact& contact,
        const MmIoKeyBinding& leftBinding,
        const MmIoKeyBinding& rightBinding,
        float deltaSeconds);
};
}
