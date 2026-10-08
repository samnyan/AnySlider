#pragma once

#include "mm_io_keyboard.h"
#include "mm_io_io3.h"
#include "mm_io_arcade_slider.h"
#include "mm_io_slider.h"

#include <cstdint>
#include <string>

namespace anyslider
{
struct MmIoGamepadConfig
{
    bool enabled = false;
    std::string device = "auto";
    bool touchpad_slider = true;
    bool touchpad_invert = false;
    float stick_slider_deadzone = 0.5f;
};

struct MmIoConfig
{
    bool enabled = false;
    bool keyboard_mouse_frontend = false;
    bool keep_game_active_unfocused = false;
    bool block_keyboard_mouse_input = false;
    bool exclusive_controller_input = false;
    MmIoSliderMode slider_mode = MmIoSliderMode::Arcade;
    uint64_t max_input_lease_ms = 500;
    float arcade_slider_emu_cells_per_second = 32.0f;
    MmIoKeyboardBindings keyboard_bindings;
    MmIoMouseSliderConfig mouse_slider;
    MmIoGamepadConfig gamepad;
    MmIoIo3Config io3;
    MmIoArcadeSliderConfig arcade_slider;
    std::wstring shared_memory_name = L"Local\\MMIO_SHARED_BUFFER";
};

bool LoadMmIoConfig(MmIoConfig& config);
}
