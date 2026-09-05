#pragma once

#include "mm_io_keyboard.h"

#include <cstdint>
#include <string>

namespace anyslider
{
struct MmIoConfig
{
    bool enabled = false;
    bool takeover = false;
    bool keyboard_frontend = false;
    bool bypass_focus_loss = false;
    bool block_keyboard_input = false;
    bool force_gamepad_ui = false;
    uint64_t input_lease_ms = 500;
    float keyboard_slider_cells_per_second = 18.0f;
    MmIoKeyboardBindings keyboard_bindings;
    std::wstring shared_memory_name = L"Local\\MMIO_SHARED_BUFFER";
};

bool LoadMmIoConfig(MmIoConfig& config);
}
