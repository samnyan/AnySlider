#pragma once

#include "mm_io_shared_memory.h"

namespace anyslider
{
bool InitializeMmGameState(MmIoConsumer& consumer);
void ShutdownMmGameState();
void UpdateMmGameState();
}
