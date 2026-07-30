#include "pch.h"

#include "Dependencies/Signature.h"
#include "Dependencies/toml.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

SIG_SCAN
(
    sigDirectInputEnumerateAndRegisterDevice,
    0x1402B2CE0,
    "\x48\x85\xD2\x0F\x84\x00\x00\x00\x00\x48\x89\x5C\x24\x00"
    "\x48\x89\x74\x24\x00\x55\x57\x41\x54",
    "xxxxx????xxxx?xxxx?xxxx"
);

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
SRWLOCK loggedDevicesLock = SRWLOCK_INIT;
std::vector<uint32_t> loggedDevices;

void Log(const char* message)
{
    printf("[AnySlider] %s\n", message);
    fflush(stdout);
}

uint32_t PackVidPid(uint16_t vid, uint16_t pid)
{
    return (static_cast<uint32_t>(pid) << 16) | vid;
}

uint16_t GetVid(uint32_t vidPid)
{
    return static_cast<uint16_t>(vidPid);
}

uint16_t GetPid(uint32_t vidPid)
{
    return static_cast<uint16_t>(vidPid >> 16);
}

uint32_t GetTargetVidPid()
{
    const uint16_t productId = configuredTargetControllerType == 6
        ? HoriMega39sProductId
        : HoriFutureToneProductId;
    return PackVidPid(HoriVendorId, productId);
}

bool IsConfiguredSource(uint32_t vidPid)
{
    return std::find(
        configuredSourceVidPids.begin(),
        configuredSourceVidPids.end(),
        vidPid) != configuredSourceVidPids.end();
}

void LogDeviceOnce(uint32_t sourceVidPid, bool remapped)
{
    AcquireSRWLockExclusive(&loggedDevicesLock);
    const bool alreadyLogged = std::find(
        loggedDevices.begin(),
        loggedDevices.end(),
        sourceVidPid) != loggedDevices.end();
    if (!alreadyLogged)
    {
        loggedDevices.push_back(sourceVidPid);
    }
    ReleaseSRWLockExclusive(&loggedDevicesLock);

    if (alreadyLogged)
    {
        return;
    }

    if (remapped)
    {
        const uint32_t targetVidPid = GetTargetVidPid();
        printf(
            "[AnySlider] DirectInput VID:PID %04X:%04X -> %04X:%04X (type %d)\n",
            GetVid(sourceVidPid),
            GetPid(sourceVidPid),
            GetVid(targetVidPid),
            GetPid(targetVidPid),
            configuredTargetControllerType);
    }
    else
    {
        printf(
            "[AnySlider] DirectInput VID:PID %04X:%04X unchanged\n",
            GetVid(sourceVidPid),
            GetPid(sourceVidPid));
    }
    fflush(stdout);
}

bool LoadConfig()
{
    try
    {
        const toml::table config = toml::parse_file("config.toml");

        configuredTargetControllerType =
            static_cast<int>(config["target_controller_type"].value_or<int64_t>(7));
        if (configuredTargetControllerType != 6 && configuredTargetControllerType != 7)
        {
            printf(
                "[AnySlider] Invalid target_controller_type %d; using type 7.\n",
                configuredTargetControllerType);
            configuredTargetControllerType = 7;
        }

        if (const toml::array* vidPids = config["vid_pids"].as_array())
        {
            configuredSourceVidPids.clear();
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
                    configuredSourceVidPids.push_back(
                        PackVidPid(static_cast<uint16_t>(vid), static_cast<uint16_t>(pid)));
                }
                else
                {
                    printf("[AnySlider] Ignoring invalid vid_pids entry: %s\n", text->c_str());
                }
            }
        }
    }
    catch (const std::exception& exception)
    {
        printf("[AnySlider] Could not read config.toml: %s\n", exception.what());
        return false;
    }

    printf("[AnySlider] Source VID/PIDs:");
    for (const uint32_t vidPid : configuredSourceVidPids)
    {
        printf(" %04X:%04X", GetVid(vidPid), GetPid(vidPid));
    }
    if (configuredSourceVidPids.empty())
    {
        printf(" (none)");
    }

    const uint32_t targetVidPid = GetTargetVidPid();
    printf(
        "\n[AnySlider] Target: %04X:%04X (controller type %d)\n",
        GetVid(targetVidPid),
        GetPid(targetVidPid),
        configuredTargetControllerType);
    fflush(stdout);
    return true;
}

void __fastcall EnumerateAndRegisterDeviceHook(int64_t context, int64_t deviceInstance)
{
    if (!deviceInstance)
    {
        originalEnumerateAndRegisterDevice(context, deviceInstance);
        return;
    }

    auto& productGuidData1 =
        *reinterpret_cast<uint32_t*>(deviceInstance + ProductGuidData1Offset);
    const uint32_t sourceVidPid = productGuidData1;
    const bool remapped = IsConfiguredSource(sourceVidPid);
    LogDeviceOnce(sourceVidPid, remapped);

    if (remapped)
    {
        productGuidData1 = GetTargetVidPid();
    }

    originalEnumerateAndRegisterDevice(context, deviceInstance);
    productGuidData1 = sourceVidPid;
}

bool InstallHook()
{
    originalEnumerateAndRegisterDevice =
        reinterpret_cast<EnumerateAndRegisterDevice>(
            sigDirectInputEnumerateAndRegisterDevice());
    if (!originalEnumerateAndRegisterDevice)
    {
        Log("The DirectInput device-registration signature was not found.");
        return false;
    }

    LONG error = DetourTransactionBegin();
    if (error == NO_ERROR)
    {
        error = DetourUpdateThread(GetCurrentThread());
    }
    if (error == NO_ERROR)
    {
        error = DetourAttach(
            reinterpret_cast<void**>(&originalEnumerateAndRegisterDevice),
            EnumerateAndRegisterDeviceHook);
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
        printf("[AnySlider] Failed to install device-registration hook: %ld\n", error);
        fflush(stdout);
        return false;
    }

    Log("Initialized selective VID/PID remapping hook.");
    return true;
}
}

extern "C"
{
__declspec(dllexport) void OnFrame()
{
}

__declspec(dllexport) void Init()
{
    static bool initialized = false;
    if (initialized)
    {
        return;
    }

    initialized = true;
    Log("Initializing...");
    if (!LoadConfig() ||
        !sigValid ||
        !sigDirectInputEnumerateAndRegisterDevice() ||
        !InstallHook())
    {
        Log("Initialization failed. This build currently supports Mega Mix+ v1.03.");
    }
}
}
