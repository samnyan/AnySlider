#pragma once

#include "mm_io_config.h"

namespace anyslider
{
bool InitializeMmIoWindowHooks(const MmIoConfig& config);
void UpdateMmIoWindowHooks();

class ScopedMmIoKeyboardPoll
{
public:
    ScopedMmIoKeyboardPoll();
    ~ScopedMmIoKeyboardPoll();

    ScopedMmIoKeyboardPoll(const ScopedMmIoKeyboardPoll&) = delete;
    ScopedMmIoKeyboardPoll& operator=(const ScopedMmIoKeyboardPoll&) = delete;
};
}
