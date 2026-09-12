#include "pch.h"

#include "vid_pid_remap.h"

#include "Dependencies/Signature.h"
#include "Dependencies/toml.hpp"
#include "anyslider_log.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace anyslider
{
namespace
{
constexpr size_t ProductGuidData1Offset = 0x14;
constexpr uint16_t HoriVendorId = 0x0F0D;
constexpr uint16_t HoriMega39sProductId = 0x00FB;
constexpr uint16_t HoriFutureToneProductId = 0x013C;

using EnumerateAndRegisterDevice = void(__fastcall*)(int64_t, int64_t);
EnumerateAndRegisterDevice originalEnumerateAndRegisterDevice = nullptr;
std::vector<uint32_t> configuredSourceVidPids;
int configuredTargetControllerType = 7;
bool suppressNativeDevices = false;
SRWLOCK loggedDevicesLock = SRWLOCK_INIT;
std::vector<uint32_t> loggedDevices;

uint32_t PackVidPid(uint16_t vid, uint16_t pid)
{
    return (static_cast<uint32_t>(pid) << 16) | vid;
}

uint16_t GetVid(uint32_t vidPid) { return static_cast<uint16_t>(vidPid); }
uint16_t GetPid(uint32_t vidPid) { return static_cast<uint16_t>(vidPid >> 16); }

uint32_t GetTargetVidPid()
{
    const uint16_t productId = configuredTargetControllerType == 6
        ? HoriMega39sProductId : HoriFutureToneProductId;
    return PackVidPid(HoriVendorId, productId);
}

bool IsConfiguredSource(uint32_t vidPid)
{
    return std::find(configuredSourceVidPids.begin(), configuredSourceVidPids.end(), vidPid)
        != configuredSourceVidPids.end();
}

void LogDeviceOnce(uint32_t sourceVidPid, bool remapped, bool suppressed)
{
    AcquireSRWLockExclusive(&loggedDevicesLock);
    const bool alreadyLogged = std::find(loggedDevices.begin(), loggedDevices.end(), sourceVidPid)
        != loggedDevices.end();
    if (!alreadyLogged)
    {
        loggedDevices.push_back(sourceVidPid);
    }
    ReleaseSRWLockExclusive(&loggedDevicesLock);
    if (alreadyLogged)
    {
        return;
    }

    if (suppressed)
    {
        Log("DirectInput VID:PID %04X:%04X suppressed (exclusive input)",
            GetVid(sourceVidPid), GetPid(sourceVidPid));
        return;
    }
    if (remapped)
    {
        const uint32_t targetVidPid = GetTargetVidPid();
        Log("DirectInput VID:PID %04X:%04X -> %04X:%04X (type %d)",
            GetVid(sourceVidPid), GetPid(sourceVidPid),
            GetVid(targetVidPid), GetPid(targetVidPid), configuredTargetControllerType);
        return;
    }
    Log("DirectInput VID:PID %04X:%04X unchanged", GetVid(sourceVidPid), GetPid(sourceVidPid));
}

bool LoadConfig()
{
    try
    {
        const toml::table config = toml::parse_file("config.toml");
        configuredTargetControllerType = static_cast<int>(
            config["target_controller_type"].value_or<int64_t>(7));
        if (configuredTargetControllerType != 6 && configuredTargetControllerType != 7)
        {
            Log("Invalid target_controller_type %d; using type 7.", configuredTargetControllerType);
            configuredTargetControllerType = 7;
        }

        configuredSourceVidPids.clear();
        if (const toml::array* vidPids = config["vid_pids"].as_array())
        {
            for (const auto& node : *vidPids)
            {
                const auto text = node.value<std::string>();
                if (!text)
                {
                    continue;
                }
                unsigned int vid = 0;
                unsigned int pid = 0;
                if (sscanf_s(text->c_str(), "%x:%x", &vid, &pid) == 2 &&
                    vid <= 0xFFFF && pid <= 0xFFFF)
                {
                    configuredSourceVidPids.push_back(PackVidPid(
                        static_cast<uint16_t>(vid), static_cast<uint16_t>(pid)));
                }
                else
                {
                    Log("Ignoring invalid vid_pids entry: %s", text->c_str());
                }
            }
        }
        return true;
    }
    catch (const std::exception& exception)
    {
        Log("Could not read remap configuration: %s", exception.what());
        return false;
    }
}

void __fastcall EnumerateAndRegisterDeviceHook(int64_t context, int64_t deviceInstance)
{
    if (!deviceInstance)
    {
        originalEnumerateAndRegisterDevice(context, deviceInstance);
        return;
    }

    auto& productGuidData1 = *reinterpret_cast<uint32_t*>(deviceInstance + ProductGuidData1Offset);
    const uint32_t sourceVidPid = productGuidData1;
    if (suppressNativeDevices)
    {
        LogDeviceOnce(sourceVidPid, false, true);
        return;
    }
    // Registration creates the native controller binding metadata. Exclusive
    // input is enforced later in PollState and state aggregation.
    const bool remapped = IsConfiguredSource(sourceVidPid);
    LogDeviceOnce(sourceVidPid, remapped, false);
    if (remapped)
    {
        productGuidData1 = GetTargetVidPid();
    }
    originalEnumerateAndRegisterDevice(context, deviceInstance);
    productGuidData1 = sourceVidPid;
}

bool InstallHook()
{
    constexpr char bytes[] =
        "\x48\x85\xD2\x0F\x84\x00\x00\x00\x00\x48\x89\x5C\x24\x00\x48\x89\x74\x24\x00\x55\x57\x41\x54";
    constexpr char mask[] = "xxxxx????xxxx?xxxx?xxxx";
    const MODULEINFO& module = getModuleInfo();
    originalEnumerateAndRegisterDevice = reinterpret_cast<EnumerateAndRegisterDevice>(
        sigScan(bytes, mask, std::strlen(mask), module.lpBaseOfDll, module.SizeOfImage));
    if (!originalEnumerateAndRegisterDevice)
    {
        Log("The DirectInput device-registration signature was not found.");
        return false;
    }

    LONG error = DetourTransactionBegin();
    if (error == NO_ERROR) error = DetourUpdateThread(GetCurrentThread());
    if (error == NO_ERROR)
    {
        error = DetourAttach(reinterpret_cast<void**>(&originalEnumerateAndRegisterDevice),
            EnumerateAndRegisterDeviceHook);
    }
    if (error == NO_ERROR) error = DetourTransactionCommit();
    else DetourTransactionAbort();
    if (error != NO_ERROR)
    {
        Log("Failed to install device-registration hook: %ld", error);
        return false;
    }
    return true;
}
}

bool InitializeVidPidRemap(bool suppressDevices)
{
    suppressNativeDevices = suppressDevices;
    if (!LoadConfig() || !InstallHook())
    {
        return false;
    }
    Log("Initialized selective VID/PID remapping hook.");
    return true;
}
}
