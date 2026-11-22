#pragma once
#include <string_view>
#include <functional>
#include "mm_io/shared_memory.h"
namespace anyslider {
bool InitializeMmLedHooks(std::wstring_view name, void (*sink)(const mmio::LedOutput&), bool diagnostics);
void ShutdownMmLedHooks();
}
