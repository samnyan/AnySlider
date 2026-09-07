#pragma once

#include "mm_io/shared_memory.h"

#include <cstdint>

namespace anyslider
{
enum class MmIoSliderMode
{
    Arcade,
    Joystick,
};

struct MmIoSliderContact
{
    float position = 0.0f;
    bool left_down = false;
    bool right_down = false;
};

void ResetMmIoSliderContact(
    MmIoSliderContact& contact,
    float startPosition,
    float endPosition);

float WrapMmIoSliderPosition(float position, float startPosition, float endPosition);

bool HasMmIoSliderTouch(const uint8_t (&touchCells)[mmio::kTouchCellCount]);

void UpdateMmIoSliderContact(
    MmIoSliderContact& contact,
    bool left,
    bool right,
    float deltaSeconds,
    float cellsPerSecond,
    bool resetOnTap,
    float startPosition,
    float endPosition);

uint32_t GetMmIoSliderDirection(bool left, bool right, uint32_t leftBit, uint32_t rightBit);

class MmIoSliderModeResolver
{
public:
    void Initialize(MmIoSliderMode configuredMode);
    void Reset();
    [[nodiscard]] MmIoSliderMode Resolve(bool directTouchActive, uint32_t directionHeld);

private:
    MmIoSliderMode configured_mode_ = MmIoSliderMode::Arcade;
    bool direct_touch_override_ = false;
};
}
