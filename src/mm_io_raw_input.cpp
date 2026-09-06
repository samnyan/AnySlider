#include "pch.h"

#include "mm_io_raw_mouse.h"

#include "anyslider_log.h"

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <mutex>
#include <string>
#include <unordered_map>

namespace anyslider
{
namespace
{
constexpr wchar_t kRawInputWindowClass[] = L"AnySliderRawInputWindow";

std::atomic<int64_t> pending_mouse_x{ 0 };
std::atomic<int64_t> pending_mouse_y{ 0 };
std::atomic<int64_t> pending_mouse_wheel{ 0 };
std::mutex mouse_device_cache_mutex;
std::unordered_map<HANDLE, bool> mouse_device_match_cache;
MmIoMouseSliderConfig mouse_slider_config;
HANDLE raw_input_thread = nullptr;
HANDLE raw_input_ready_event = nullptr;
HWND raw_input_window = nullptr;
std::atomic_bool raw_input_ready{ false };

bool ContainsCaseInsensitive(const std::wstring& value, const std::wstring& search)
{
    if (search.empty())
    {
        return true;
    }
    return std::search(
        value.begin(), value.end(), search.begin(), search.end(),
        [](wchar_t left, wchar_t right)
        {
            return std::towlower(left) == std::towlower(right);
        }) != value.end();
}

bool MatchesMouseSliderDevice(HANDLE device)
{
    if (!device)
    {
        return false;
    }
    {
        std::lock_guard lock(mouse_device_cache_mutex);
        if (const auto it = mouse_device_match_cache.find(device); it != mouse_device_match_cache.end())
        {
            return it->second;
        }
    }

    UINT length = 0;
    if (GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, nullptr, &length) == static_cast<UINT>(-1) || length == 0)
    {
        Log("Could not read Raw mouse device name: device=%p error=%lu", device, GetLastError());
        return false;
    }
    std::wstring name(length, L'\0');
    if (GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, name.data(), &length) == static_cast<UINT>(-1))
    {
        Log("Could not read Raw mouse device name: device=%p error=%lu", device, GetLastError());
        return false;
    }
    name.resize(std::wcslen(name.c_str()));
    const bool matches = mouse_slider_config.device_filter.empty() ||
        ContainsCaseInsensitive(name, mouse_slider_config.device_filter);
    DebugLog("Raw mouse detected: device=%p name=%ls mouse-slider=%s",
        device, name.c_str(), matches ? "yes" : "no");
    {
        std::lock_guard lock(mouse_device_cache_mutex);
        mouse_device_match_cache.emplace(device, matches);
    }
    return matches;
}

bool RegisterMmIoRawMouse(HWND targetWindow)
{
    RAWINPUTDEVICE mouseDevice{};
    mouseDevice.usUsagePage = 0x01;
    mouseDevice.usUsage = 0x02;
    mouseDevice.dwFlags = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY;
    mouseDevice.hwndTarget = targetWindow;
    if (!RegisterRawInputDevices(&mouseDevice, 1, sizeof(mouseDevice)))
    {
        Log("Could not register Raw Input mouse: %lu", GetLastError());
        return false;
    }
    Log("Raw Input receiver registered: hwnd=%p flags=%08lX", targetWindow, mouseDevice.dwFlags);
    return true;
}

void SignalRawInputReady(bool ready)
{
    raw_input_ready.store(ready, std::memory_order_release);
    SetEvent(raw_input_ready_event);
}

void ProcessMmIoRawMouseInput(HRAWINPUT rawInput)
{
    if (!raw_input_ready.load(std::memory_order_relaxed) || !rawInput)
    {
        return;
    }
    UINT size = sizeof(RAWINPUT);
    RAWINPUT input{};
    if (GetRawInputData(rawInput, RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1))
    {
        Log("GetRawInputData failed: error=%lu", GetLastError());
        return;
    }
    if (input.header.dwType != RIM_TYPEMOUSE || !MatchesMouseSliderDevice(input.header.hDevice))
    {
        return;
    }
    const RAWMOUSE& mouse = input.data.mouse;
    if ((mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0)
    {
        pending_mouse_x.fetch_add(mouse.lLastX, std::memory_order_relaxed);
        pending_mouse_y.fetch_add(mouse.lLastY, std::memory_order_relaxed);
    }
    if ((mouse.usButtonFlags & RI_MOUSE_WHEEL) != 0)
    {
        pending_mouse_wheel.fetch_add(static_cast<SHORT>(mouse.usButtonData), std::memory_order_relaxed);
    }
}

LRESULT CALLBACK RawInputWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_INPUT:
        ProcessMmIoRawMouseInput(reinterpret_cast<HRAWINPUT>(lParam));
        if (GET_RAWINPUT_CODE_WPARAM(wParam) == RIM_INPUT)
        {
            return DefWindowProcW(window, message, wParam, lParam);
        }
        return 0;
    case WM_INPUT_DEVICE_CHANGE:
        if (wParam == GIDC_REMOVAL)
        {
            std::lock_guard lock(mouse_device_cache_mutex);
            mouse_device_match_cache.erase(reinterpret_cast<HANDLE>(lParam));
        }
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

DWORD WINAPI RawInputThreadProc(LPVOID)
{
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = RawInputWindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kRawInputWindowClass;
    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        Log("Could not register Raw Input window class: %lu", GetLastError());
        SignalRawInputReady(false);
        return 0;
    }

    raw_input_window = CreateWindowExW(
        0, kRawInputWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
    if (!raw_input_window)
    {
        Log("Could not create Raw Input window: %lu", GetLastError());
        SignalRawInputReady(false);
        return 0;
    }
    if (!RegisterMmIoRawMouse(raw_input_window))
    {
        SignalRawInputReady(false);
        return 0;
    }

    SignalRawInputReady(true);
    Log("Raw Input receiver ready: hwnd=%p", raw_input_window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    raw_input_ready.store(false, std::memory_order_release);
    return 0;
}
}

bool InitializeMmIoRawMouse(const MmIoMouseSliderConfig& config)
{
    if (!config.enabled)
    {
        return false;
    }
    mouse_slider_config = config;
    ResetMmIoRawMouseInput();
    if (raw_input_thread)
    {
        return raw_input_ready.load(std::memory_order_acquire);
    }
    raw_input_ready_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!raw_input_ready_event)
    {
        Log("Could not create Raw Input ready event: %lu", GetLastError());
        return false;
    }
    raw_input_thread = CreateThread(nullptr, 0, RawInputThreadProc, nullptr, 0, nullptr);
    if (!raw_input_thread)
    {
        Log("Could not create Raw Input thread: %lu", GetLastError());
        CloseHandle(raw_input_ready_event);
        raw_input_ready_event = nullptr;
        return false;
    }
    const DWORD waitResult = WaitForSingleObject(raw_input_ready_event, 5000);
    if (waitResult != WAIT_OBJECT_0 || !raw_input_ready.load(std::memory_order_acquire))
    {
        Log("Raw Input receiver did not become ready.");
        return false;
    }
    return true;
}

MmIoRawMouseDelta ConsumeMmIoRawMouseDelta()
{
    return {
        pending_mouse_x.exchange(0, std::memory_order_relaxed),
        pending_mouse_y.exchange(0, std::memory_order_relaxed),
        pending_mouse_wheel.exchange(0, std::memory_order_relaxed),
    };
}

void ResetMmIoRawMouseInput()
{
    pending_mouse_x.store(0, std::memory_order_relaxed);
    pending_mouse_y.store(0, std::memory_order_relaxed);
    pending_mouse_wheel.store(0, std::memory_order_relaxed);
    std::lock_guard lock(mouse_device_cache_mutex);
    mouse_device_match_cache.clear();
}
}
