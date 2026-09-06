#pragma once

#include "mm_io_keyboard.h"

#include <cstdint>
#include <string>

namespace anyslider
{
struct MmIoConfig
{
    bool enabled = false;
    bool keyboard_mouse_frontend = false;
    bool keep_game_active_unfocused = false;
    bool block_keyboard_mouse_input = false;
    bool exclusive_controller_input = false;
    uint64_t max_input_lease_ms = 500;
    float keyboard_slider_cells_per_second = 18.0f;
    MmIoKeyboardBindings keyboard_bindings;
    std::wstring shared_memory_name = L"Local\\MMIO_SHARED_BUFFER";
};

bool LoadMmIoConfig(MmIoConfig& config);
}
