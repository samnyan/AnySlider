#include "pch.h"

#include "mm_io_slider.h"

#include <algorithm>
#include <cmath>

namespace anyslider
{
namespace
{
float WrapContactPositionInLength(float position, float startPosition, float length)
{
    return startPosition + std::fmod(
        std::fmod(position - startPosition, length) + length,
        length);
}

}

float WrapMmIoSliderPosition(float position, float startPosition, float endPosition)
{
    return WrapContactPositionInLength(
        position, startPosition, endPosition - startPosition);
}

bool HasMmIoSliderTouch(const uint8_t (&touchCells)[mmio::kTouchCellCount])
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

void ResetMmIoSliderContact(
    MmIoSliderContact& contact,
    float startPosition,
    float endPosition)
{
    contact = {};
    contact.position = startPosition + (endPosition - startPosition - 1.0f) * 0.5f;
}

void UpdateMmIoSliderContact(
    MmIoSliderContact& contact,
    bool left,
    bool right,
    float deltaSeconds,
    float cellsPerSecond,
    bool resetOnTap,
    float startPosition,
    float endPosition)
{
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
    contact.position = WrapMmIoSliderPosition(
        contact.position + direction * cellsPerSecond * deltaSeconds,
        startPosition,
        endPosition);
}

uint32_t GetMmIoSliderDirection(bool left, bool right, uint32_t leftBit, uint32_t rightBit)
{
    if (left == right)
    {
        return 0;
    }
    return left ? leftBit : rightBit;
}

void MmIoSliderModeResolver::Initialize(MmIoSliderMode configuredMode)
{
    configured_mode_ = configuredMode;
    direct_touch_override_ = false;
}

void MmIoSliderModeResolver::Reset()
{
    direct_touch_override_ = false;
}

MmIoSliderMode MmIoSliderModeResolver::Resolve(
    bool directTouchActive,
    uint32_t directionHeld)
{
    if (configured_mode_ == MmIoSliderMode::Arcade)
    {
        return MmIoSliderMode::Arcade;
    }

    if (directionHeld != 0)
    {
        direct_touch_override_ = false;
        return MmIoSliderMode::Joystick;
    }
    if (directTouchActive)
    {
        direct_touch_override_ = true;
    }
    return direct_touch_override_
        ? MmIoSliderMode::Arcade
        : MmIoSliderMode::Joystick;
}
}
