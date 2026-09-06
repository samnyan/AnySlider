#pragma once

#include "mm_io_config.h"

namespace anyslider
{
bool InitializeMmIoWindowHooks(const MmIoConfig& config);
void UpdateMmIoWindowHooks();

class ScopedMmIoKeyboardMousePoll
{
public:
    ScopedMmIoKeyboardMousePoll();
    ~ScopedMmIoKeyboardMousePoll();

    ScopedMmIoKeyboardMousePoll(const ScopedMmIoKeyboardMousePoll&) = delete;
    ScopedMmIoKeyboardMousePoll& operator=(const ScopedMmIoKeyboardMousePoll&) = delete;
};
}
