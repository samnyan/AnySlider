#pragma once

#include "mm_io_keyboard.h"

namespace anyslider
{
bool InitializeMmIoRawInput(
    const MmIoMouseSliderConfig& config,
    bool includeMouse);
void ShutdownMmIoRawInput();
MmIoRawMouseDelta ConsumeMmIoRawMouseDelta();
void ResetMmIoRawMouseInput();
bool InitializeMmIoRawKeyboard();
bool IsMmIoRawKeyboardDown(int virtualKey);
bool ConsumeMmIoRawKeyboardPressed(int virtualKey);
}
