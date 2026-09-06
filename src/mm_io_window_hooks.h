#pragma once

#include "mm_io_config.h"

namespace anyslider
{
bool InitializeMmIoWindowHooks(const MmIoConfig& config);
void UpdateMmIoWindowHooks();
}

