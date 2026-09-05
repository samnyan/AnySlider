#pragma once

#include "mm_io_config.h"

namespace anyslider
{
bool InitializeMmIoHooks(const MmIoConfig& config);
void ShutdownMmIoHooks();
}
