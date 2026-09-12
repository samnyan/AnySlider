#include "pch.h"

#include "src/anyslider_log.h"
#include "src/mm_io_config.h"
#include "src/mm_io_hooks.h"
#include "src/mm_io_window_hooks.h"
#include "src/vid_pid_remap.h"

struct IDXGISwapChain;

extern "C"
{
// DIVA Mod Loader invokes this immediately before IDXGISwapChain::Present.
__declspec(dllexport) void OnFrame(IDXGISwapChain*)
{
    anyslider::UpdateMmIoWindowHooks();
}

// DIVA Mod Loader invokes this from WinMain after game globals are initialized.
__declspec(dllexport) void Init()
{
    static bool initialized = false;
    if (initialized)
    {
        return;
    }
    initialized = true;

    anyslider::Log("Initializing...");
    if (!anyslider::InitializeVidPidRemap())
    {
        anyslider::Log("VID/PID remapping is unavailable for this executable version.");
    }

    anyslider::MmIoConfig mmIoConfig;
    if (!anyslider::LoadMmIoConfig(mmIoConfig))
    {
        return;
    }
    anyslider::SetDirectInputDeviceSuppression(
        mmIoConfig.enabled && mmIoConfig.exclusive_controller_input);
    if (!mmIoConfig.enabled)
    {
        anyslider::Log("Native IO disabled by io_enabled=false.");
        return;
    }
    if (!anyslider::InitializeMmIoWindowHooks(mmIoConfig))
    {
        anyslider::Log("MMIO window hooks are unavailable.");
    }
    if (!anyslider::InitializeMmIoHooks(mmIoConfig))
    {
        anyslider::Log("MMIO backend is unavailable for this executable version.");
    }
}
}
