#pragma once

#include "mm_io_keyboard.h"

namespace anyslider
{
bool InitializeMmIoRawMouse(const MmIoMouseSliderConfig& config);
MmIoRawMouseDelta ConsumeMmIoRawMouseDelta();
void ResetMmIoRawMouseInput();
}
