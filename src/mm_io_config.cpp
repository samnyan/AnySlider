#include "pch.h"

#include "mm_io_config.h"

#include "Dependencies/toml.hpp"
#include "anyslider_log.h"
#include "mm_io/shared_memory.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace anyslider
{
namespace
{
std::string Trim(std::string value)
{
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char c)
    {
        return std::isspace(c) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c)
    {
        return std::isspace(c) != 0;
    }).base();
    return first >= last ? std::string{} : std::string(first, last);
}

std::string ToUpper(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c)
    {
        return static_cast<char>(std::toupper(c));
    });
    return value;
}

bool TryParseVirtualKey(const std::string& text, int& virtualKey)
{
    const std::string key = Trim(text);
    if (key.size() == 1)
    {
        virtualKey = static_cast<unsigned char>(std::toupper(static_cast<unsigned char>(key[0])));
        return true;
    }

    const std::string normalized = ToUpper(key);
    static const std::unordered_map<std::string, int> namedKeys = {
        { "ENTER", VK_RETURN }, { "RETURN", VK_RETURN }, { "TAB", VK_TAB },
        { "BACK", VK_BACK }, { "BACKSPACE", VK_BACK }, { "SPACE", VK_SPACE },
        { "SPACEBAR", VK_SPACE }, { "ESC", VK_ESCAPE }, { "ESCAPE", VK_ESCAPE },
        { "UP", VK_UP }, { "DOWN", VK_DOWN }, { "LEFT", VK_LEFT }, { "RIGHT", VK_RIGHT },
        { "LEFTSHIFT", VK_LSHIFT }, { "LSHIFT", VK_LSHIFT },
        { "RIGHTSHIFT", VK_RSHIFT }, { "RSHIFT", VK_RSHIFT },
        { "LEFTCONTROL", VK_LCONTROL }, { "LCONTROL", VK_LCONTROL }, { "LCTRL", VK_LCONTROL },
        { "RIGHTCONTROL", VK_RCONTROL }, { "RCONTROL", VK_RCONTROL }, { "RCTRL", VK_RCONTROL },
        { "LEFTALT", VK_LMENU }, { "LALT", VK_LMENU },
        { "RIGHTALT", VK_RMENU }, { "RALT", VK_RMENU },
        { "INSERT", VK_INSERT }, { "INS", VK_INSERT }, { "DELETE", VK_DELETE }, { "DEL", VK_DELETE },
        { "HOME", VK_HOME }, { "END", VK_END }, { "PAGEUP", VK_PRIOR }, { "PAGEDOWN", VK_NEXT },
        { "COMMA", VK_OEM_COMMA }, { "PERIOD", VK_OEM_PERIOD }, { "SLASH", VK_OEM_2 },
        { "BACKSLASH", VK_OEM_5 }, { "SEMICOLON", VK_OEM_1 }, { "APOSTROPHE", VK_OEM_7 },
        { "LEFTBRACKET", VK_OEM_4 }, { "RIGHTBRACKET", VK_OEM_6 },
        { "PLUS", VK_ADD }, { "MINUS", VK_SUBTRACT }, { "MULTIPLY", VK_MULTIPLY }, { "DIVIDE", VK_DIVIDE },
        { "MOUSELEFT", VK_LBUTTON }, { "MOUSEMIDDLE", VK_MBUTTON }, { "MOUSERIGHT", VK_RBUTTON },
        { "MOUSEX1", VK_XBUTTON1 }, { "MOUSEX2", VK_XBUTTON2 },
    };
    if (const auto it = namedKeys.find(normalized); it != namedKeys.end())
    {
        virtualKey = it->second;
        return true;
    }

    if (normalized.size() == 2 && normalized[0] == 'F' && normalized[1] >= '1' && normalized[1] <= '9')
    {
        virtualKey = VK_F1 + normalized[1] - '1';
        return true;
    }
    if (normalized.size() == 3 && normalized[0] == 'F' && normalized[1] >= '1' && normalized[1] <= '2' &&
        normalized[2] >= '0' && normalized[2] <= '9')
    {
        const int number = (normalized[1] - '0') * 10 + normalized[2] - '0';
        if (number <= 24)
        {
            virtualKey = VK_F1 + number - 1;
            return true;
        }
    }
    if (normalized.size() == 7 && normalized.starts_with("NUMPAD") && normalized[6] >= '0' && normalized[6] <= '9')
    {
        virtualKey = VK_NUMPAD0 + normalized[6] - '0';
        return true;
    }
    if (normalized.size() == 4 && normalized.starts_with("0X"))
    {
        unsigned int value = 0;
        const auto [end, error] = std::from_chars(
            normalized.data() + 2,
            normalized.data() + normalized.size(),
            value,
            16);
        if (error == std::errc{} && end == normalized.data() + normalized.size() && value <= 0xFF)
        {
            virtualKey = static_cast<int>(value);
            return true;
        }
    }
    return false;
}

void AppendDelimitedKeys(const std::string& text, MmIoKeyBinding& binding, const char* configKey)
{
    size_t start = 0;
    while (start <= text.size())
    {
        const size_t end = text.find(',', start);
        const std::string key = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!Trim(key).empty())
        {
            int virtualKey = 0;
            if (TryParseVirtualKey(key, virtualKey))
            {
                binding.push_back(virtualKey);
            }
            else
            {
                Log("Invalid keyboard binding %s=%s", configKey, key.c_str());
            }
        }
        if (end == std::string::npos)
        {
            break;
        }
        start = end + 1;
    }
}

void LoadKeyboardBinding(const toml::table& document, const char* configKey, MmIoKeyBinding& binding)
{
    const auto node = document[configKey];
    if (!node)
    {
        return;
    }

    MmIoKeyBinding parsed;
    if (const toml::array* values = node.as_array())
    {
        for (const toml::node& value : *values)
        {
            if (const auto key = value.value<std::string>())
            {
                AppendDelimitedKeys(*key, parsed, configKey);
            }
            else
            {
                Log("Invalid keyboard binding array entry for %s", configKey);
            }
        }
    }
    else if (const auto value = node.value<std::string>())
    {
        AppendDelimitedKeys(*value, parsed, configKey);
    }
    else
    {
        Log("Invalid keyboard binding value for %s", configKey);
        return;
    }
    binding = std::move(parsed);
}

bool TryParseMouseAxis(const std::string& text, MmIoMouseAxis& axis)
{
    const std::string normalized = ToUpper(Trim(text));
    if (normalized == "X") { axis = MmIoMouseAxis::X; return true; }
    if (normalized == "Y") { axis = MmIoMouseAxis::Y; return true; }
    if (normalized == "WHEEL") { axis = MmIoMouseAxis::Wheel; return true; }
    return false;
}

float LoadMouseSensitivity(const toml::table& document, const char* key)
{
    const float value = document[key].value_or(1.0f);
    if (value < 0.01f || value > 100.0f)
    {
        Log("Invalid %s; using 1.0.", key);
        return 1.0f;
    }
    return value;
}

void LoadMouseSliderConfig(const toml::table& document, MmIoConfig& config)
{
    auto& mouse = config.mouse_slider;
    mouse.enabled = document["io_use_mouse_as_slider"].value_or(false);
    const std::string axis1 = document["io_mouse_slider_1_axis"].value_or(std::string("x"));
    const std::string axis2 = document["io_mouse_slider_2_axis"].value_or(std::string("y"));
    if (!TryParseMouseAxis(axis1, mouse.slider_1.axis))
    {
        Log("Invalid io_mouse_slider_1_axis=%s; using x.", axis1.c_str());
        mouse.slider_1.axis = MmIoMouseAxis::X;
    }
    if (!TryParseMouseAxis(axis2, mouse.slider_2.axis))
    {
        Log("Invalid io_mouse_slider_2_axis=%s; using y.", axis2.c_str());
        mouse.slider_2.axis = MmIoMouseAxis::Y;
    }
    mouse.slider_1.invert = document["io_mouse_slider_1_invert"].value_or(false);
    mouse.slider_2.invert = document["io_mouse_slider_2_invert"].value_or(false);
    mouse.slider_1.sensitivity = LoadMouseSensitivity(document, "io_mouse_slider_1_sensitivity");
    mouse.slider_2.sensitivity = LoadMouseSensitivity(document, "io_mouse_slider_2_sensitivity");
    mouse.counts_per_cycle = document["io_mouse_slider_counts_per_cycle"].value_or(256.0f);
    if (mouse.counts_per_cycle < 1.0f || mouse.counts_per_cycle > 100000.0f)
    {
        Log("Invalid io_mouse_slider_counts_per_cycle; using 256.");
        mouse.counts_per_cycle = 256.0f;
    }
    const std::string device = document["io_mouse_slider_device"].value_or(std::string{});
    mouse.device_filter.assign(device.begin(), device.end());
    if (mouse.enabled && !config.keyboard_mouse_frontend)
    {
        Log("Mouse slider is enabled but io_keyboard_mouse_frontend is disabled; ignoring mouse slider input.");
    }
}

void LoadKeyboardBindings(const toml::table& document, MmIoKeyboardBindings& bindings)
{
    LoadKeyboardBinding(document, "io_key_test", bindings.test);
    LoadKeyboardBinding(document, "io_key_service", bindings.service);
    LoadKeyboardBinding(document, "io_key_pause", bindings.pause);
    LoadKeyboardBinding(document, "io_key_start", bindings.start);
    LoadKeyboardBinding(document, "io_key_dpad_up", bindings.dpad_up);
    LoadKeyboardBinding(document, "io_key_dpad_down", bindings.dpad_down);
    LoadKeyboardBinding(document, "io_key_dpad_left", bindings.dpad_left);
    LoadKeyboardBinding(document, "io_key_dpad_right", bindings.dpad_right);
    LoadKeyboardBinding(document, "io_key_triangle", bindings.triangle);
    LoadKeyboardBinding(document, "io_key_square", bindings.square);
    LoadKeyboardBinding(document, "io_key_cross", bindings.cross);
    LoadKeyboardBinding(document, "io_key_circle", bindings.circle);
    LoadKeyboardBinding(document, "io_key_l1", bindings.l1);
    LoadKeyboardBinding(document, "io_key_r1", bindings.r1);
    LoadKeyboardBinding(document, "io_key_l2", bindings.l2);
    LoadKeyboardBinding(document, "io_key_r2", bindings.r2);
    LoadKeyboardBinding(document, "io_key_select", bindings.select);
    LoadKeyboardBinding(document, "io_key_l3", bindings.l3);
    LoadKeyboardBinding(document, "io_key_r3", bindings.r3);
    LoadKeyboardBinding(document, "io_key_slider_1_left", bindings.slider_1_left);
    LoadKeyboardBinding(document, "io_key_slider_1_right", bindings.slider_1_right);
    LoadKeyboardBinding(document, "io_key_slider_2_left", bindings.slider_2_left);
    LoadKeyboardBinding(document, "io_key_slider_2_right", bindings.slider_2_right);

    for (size_t index = 0; index < bindings.slider_cells.size(); ++index)
    {
        char configKey[32];
        sprintf_s(configKey, "io_key_slider_%02zu", index + 1);
        LoadKeyboardBinding(document, configKey, bindings.slider_cells[index]);
    }
}
}

bool LoadMmIoConfig(MmIoConfig& config)
{
    try
    {
        const toml::table document = toml::parse_file("config.toml");
        config.enabled = document["io_enabled"].value_or(false);
        config.keyboard_mouse_frontend = document["io_keyboard_mouse_frontend"].value_or(false);
        config.keep_game_active_unfocused = document["io_keep_game_active_unfocused"].value_or(false);
        config.block_keyboard_mouse_input = document["io_block_keyboard_mouse_input"].value_or(false);
        config.exclusive_controller_input = document["io_exclusive_controller_input"].value_or(false);
        config.max_input_lease_ms = document["io_max_input_lease_ms"].value_or<uint64_t>(500);
        if (config.max_input_lease_ms == 0 || config.max_input_lease_ms > 10'000)
        {
            Log("Invalid io_max_input_lease_ms; using 500 ms.");
            config.max_input_lease_ms = 500;
        }
        config.keyboard_slider_cells_per_second =
            document["io_keyboard_slider_cells_per_second"].value_or(18.0f);
        if (config.keyboard_slider_cells_per_second <= 0.0f ||
            config.keyboard_slider_cells_per_second > 64.0f)
        {
            Log("Invalid io_keyboard_slider_cells_per_second; using 18.");
            config.keyboard_slider_cells_per_second = 18.0f;
        }
        config.keyboard_bindings = DefaultMmIoKeyboardBindings();
        LoadKeyboardBindings(document, config.keyboard_bindings);
        LoadMouseSliderConfig(document, config);

        const std::string mappingName = document["io_shared_memory"]
            .value_or(std::string("Local\\MMIO_SHARED_BUFFER"));
        config.shared_memory_name.assign(mappingName.begin(), mappingName.end());
        if (config.shared_memory_name.empty())
        {
            config.shared_memory_name = mmio::kSharedMemoryName;
        }

        return true;
    }
    catch (const std::exception& exception)
    {
        Log("Could not read MMIO configuration: %s", exception.what());
        return false;
    }
}
}
