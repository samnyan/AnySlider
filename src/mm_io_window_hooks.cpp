#include "pch.h"

#include "mm_io_window_hooks.h"

#include "anyslider_log.h"

#include <atomic>

namespace anyslider
{
namespace
{
using GetAsyncKeyStateFunction = SHORT(WINAPI*)(int virtualKey);
using GetForegroundWindowFunction = HWND(WINAPI*)();

MmIoConfig windowHookConfig;
GetAsyncKeyStateFunction originalGetAsyncKeyState = GetAsyncKeyState;
GetForegroundWindowFunction originalGetForegroundWindow = GetForegroundWindow;
WNDPROC originalWindowProcedure = nullptr;
HWND gameWindow = nullptr;
thread_local bool allowKeyboardFrontendPoll = false;

bool ShouldBypassFocusLoss()
{
    return windowHookConfig.keep_game_active_unfocused;
}

bool ShouldBlockKeyboardMouseInput()
{
    return windowHookConfig.block_keyboard_mouse_input;
}

bool IsKeyboardVirtualKey(int virtualKey)
{
    return
        (virtualKey >= VK_BACK && virtualKey <= VK_CAPITAL) ||
        (virtualKey >= VK_KANA && virtualKey <= VK_MODECHANGE) ||
        (virtualKey >= VK_SPACE && virtualKey <= VK_HELP) ||
        (virtualKey >= '0' && virtualKey <= 'Z') ||
        (virtualKey >= VK_LWIN && virtualKey <= VK_SLEEP) ||
        (virtualKey >= VK_NUMPAD0 && virtualKey <= VK_F24) ||
        (virtualKey >= VK_NUMLOCK && virtualKey <= VK_SCROLL) ||
        (virtualKey >= VK_LSHIFT && virtualKey <= VK_RMENU) ||
        (virtualKey >= VK_BROWSER_BACK && virtualKey <= VK_LAUNCH_APP2) ||
        (virtualKey >= VK_OEM_1 && virtualKey <= VK_OEM_8) ||
        virtualKey == VK_OEM_102 ||
        virtualKey == VK_PROCESSKEY ||
        virtualKey == VK_PACKET ||
        (virtualKey >= VK_ATTN && virtualKey <= VK_PLAY);
}

bool IsMouseVirtualKey(int virtualKey)
{
    return virtualKey == VK_LBUTTON || virtualKey == VK_RBUTTON ||
        virtualKey == VK_MBUTTON || virtualKey == VK_XBUTTON1 ||
        virtualKey == VK_XBUTTON2;
}

bool IsKeyboardMouseVirtualKey(int virtualKey)
{
    return IsKeyboardVirtualKey(virtualKey) || IsMouseVirtualKey(virtualKey);
}

SHORT WINAPI GetAsyncKeyStateHook(int virtualKey)
{
    if (windowHookConfig.block_keyboard_mouse_input &&
        IsKeyboardMouseVirtualKey(virtualKey) &&
        !allowKeyboardFrontendPoll)
    {
        return 0;
    }
    return originalGetAsyncKeyState(virtualKey);
}

HWND WINAPI GetForegroundWindowHook()
{
    if (ShouldBypassFocusLoss() && gameWindow)
    {
        return gameWindow;
    }
    return originalGetForegroundWindow();
}

bool IsBlockedKeyboardMouseMessage(UINT message)
{
    switch (message)
    {
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
    case WM_CHAR:
    case WM_SYSCHAR:
    case WM_DEADCHAR:
    case WM_SYSDEADCHAR:
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK:
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
        return true;
    default:
        return false;
    }
}

LRESULT CALLBACK GameWindowProcedureHook(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (ShouldBypassFocusLoss())
    {
        if ((message == WM_ACTIVATEAPP && wParam == FALSE) ||
            (message == WM_ACTIVATE && LOWORD(wParam) == WA_INACTIVE) ||
            message == WM_KILLFOCUS)
        {
            return 0;
        }
    }

    if (ShouldBlockKeyboardMouseInput())
    {
        if (IsBlockedKeyboardMouseMessage(message))
        {
            return 0;
        }
        if (message == WM_INPUT)
        {
            if (GET_RAWINPUT_CODE_WPARAM(wParam) == RIM_INPUT)
            {
                return DefWindowProcW(window, message, wParam, lParam);
            }
            return 0;
        }
    }

    return CallWindowProcW(originalWindowProcedure, window, message, wParam, lParam);
}

BOOL CALLBACK FindGameWindow(HWND candidate, LPARAM parameter)
{
    if (!IsWindowVisible(candidate) || GetWindow(candidate, GW_OWNER))
    {
        return TRUE;
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(candidate, &processId);
    if (processId != GetCurrentProcessId())
    {
        return TRUE;
    }

    RECT rect{};
    GetClientRect(candidate, &rect);
    const auto area = static_cast<int64_t>(rect.right - rect.left) * (rect.bottom - rect.top);
    auto* best = reinterpret_cast<std::pair<HWND, int64_t>*>(parameter);
    if (area > best->second)
    {
        *best = { candidate, area };
    }
    return TRUE;
}

HWND FindMainGameWindow()
{
    std::pair<HWND, int64_t> best{ nullptr, 0 };
    EnumWindows(FindGameWindow, reinterpret_cast<LPARAM>(&best));
    return best.first;
}
}

bool InitializeMmIoWindowHooks(const MmIoConfig& config)
{
    windowHookConfig = config;
    if (!config.block_keyboard_mouse_input && !config.keep_game_active_unfocused)
    {
        return true;
    }

    LONG error = DetourTransactionBegin();
    if (error == NO_ERROR)
    {
        error = DetourUpdateThread(GetCurrentThread());
    }
    if (error == NO_ERROR && config.keep_game_active_unfocused)
    {
        error = DetourAttach(
            reinterpret_cast<void**>(&originalGetForegroundWindow),
            GetForegroundWindowHook);
    }
    if (error == NO_ERROR && config.block_keyboard_mouse_input)
    {
        error = DetourAttach(
            reinterpret_cast<void**>(&originalGetAsyncKeyState),
            GetAsyncKeyStateHook);
    }
    if (error == NO_ERROR)
    {
        error = DetourTransactionCommit();
    }
    else
    {
        DetourTransactionAbort();
    }
    if (error != NO_ERROR)
    {
        Log("Could not install GetAsyncKeyState hook: %ld", error);
        return false;
    }

    return true;
}

void UpdateMmIoWindowHooks()
{
    if ((!windowHookConfig.keep_game_active_unfocused && !windowHookConfig.block_keyboard_mouse_input) ||
        originalWindowProcedure)
    {
        return;
    }

    gameWindow = FindMainGameWindow();
    if (!gameWindow)
    {
        return;
    }

    SetLastError(ERROR_SUCCESS);
    originalWindowProcedure = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        gameWindow,
        GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(GameWindowProcedureHook)));
    if (!originalWindowProcedure)
    {
        Log("Could not install game window hook: %lu", GetLastError());
        return;
    }

    Log("Window hook attached: focus=%s keyboard-mouse=%s",
        windowHookConfig.keep_game_active_unfocused ? "true" : "false",
        windowHookConfig.block_keyboard_mouse_input ? "true" : "false");
}

ScopedMmIoKeyboardMousePoll::ScopedMmIoKeyboardMousePoll()
{
    allowKeyboardFrontendPoll = true;
}

ScopedMmIoKeyboardMousePoll::~ScopedMmIoKeyboardMousePoll()
{
    allowKeyboardFrontendPoll = false;
}
}
