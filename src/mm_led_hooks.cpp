#include "pch.h"
#include "mm_led_hooks.h"
#include "mm_led_diagnostics.h"
#include "mm_arcade_led.h"
#include "mm_led_setter_bridge.h"
#include "mm_io_shared_memory.h"
#include "mm_io/led_output.h"
#include "anyslider_log.h"
#include "hook_validation.h"
#include <string>

namespace anyslider {
namespace {
using Pack = uint64_t(__fastcall*)(void*);
using SetOutput = uint64_t(__fastcall*)(void*, int, char);
Pack originalPack = nullptr;
SetOutput originalSetOutput = nullptr;
mmio::LedOutput latest{};
bool active = false;
int64_t packCount = 0, ioCount = 0;
MmIoSharedMemory mapping;
HANDLE ledNotifyEvent = nullptr;
void (*g_sink)(const mmio::LedOutput&) = nullptr; //也許這個變數需要一個好一點的名字。但我不是很確定這是做什麼用的
bool installed = false;
LedSetterBridge setterBridge;

// 唯一跨进程/外设发布入口：Hook 更新内存，然後透過此函式發布。
void PublishLedData() {
    latest.timestamp_ms = MmIoNowMilliseconds();
    auto buf = mapping.Get();
    if (buf) mmio::PublishLedOutput(*buf, latest);
    if (g_sink) g_sink(latest);
    if (ledNotifyEvent) SetEvent(ledNotifyEvent);
}
uint64_t __fastcall PackHook(void* task) {
    ApplyArcadeLedScene(task);
    const auto result = originalPack(task);
    ObserveLedPack(task);
    if (active) {
        void* device = nullptr;
        std::memcpy(&device, static_cast<uint8_t*>(task) + 104, sizeof(device));
        if (device) {
            const uint8_t* payload = static_cast<const uint8_t*>(device) + 6300;
            if (payload[0] == 0x3f) {
                if (std::memcmp(latest.slider, payload, sizeof(latest.slider))) {
                    std::memcpy(latest.slider, payload, sizeof(latest.slider));
                    PublishLedData();
                }
                ++packCount;
            }
        }
    }
    return result;
}
uint64_t __fastcall SetOutputHook(void* state, int id, char on) {
    const auto result = originalSetOutput(state, id, on);
    ObserveLedOutput(state, id, on);
    if (active && state && id >= 0 && id < 22) {
        if (std::memcmp(&latest.io3_bitmap, static_cast<uint8_t*>(state) + 216, sizeof(latest.io3_bitmap))) {
            std::memcpy(&latest.io3_bitmap, static_cast<uint8_t*>(state) + 216, sizeof(latest.io3_bitmap));
            PublishLedData();
        }
        ++ioCount;
    }
    return result;
}
}
bool InitializeMmLedHooks(std::wstring_view name, void (*sink)(const mmio::LedOutput&), bool diagnostics) {
    if (installed) return true;
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const uint8_t packBytes[] = {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x7c,0x24,0x10,0x33,0xff,0xbb,0xfe,0,0,0};
    const uint8_t ioBytes[] = {0x4c,0x63,0xca,0x45,0x84,0xc0,0x74,0x29,0x49,0x83,0xf9,0x16,0x73,0x4e};
    // 未识别的版本不安装固定入口，避免错误修改代码。
    if (nt->OptionalHeader.SizeOfImage != 0x23e73000 ||
        !ValidateHookBytes(base, 0x5a60f0, packBytes, sizeof(packBytes), "mm_led_hooks/packBytes") ||
        !ValidateHookBytes(base, 0x2ab300, ioBytes, sizeof(ioBytes), "mm_led_hooks/ioBytes")) {
        Log("LED: unsupported game build; output capture disabled");
        return false;
    }
    if (!mapping.OpenOrCreate(name, 0)) return false;
    ledNotifyEvent = CreateEventW(
        NULL,
        TRUE,   // Manual-reset
        FALSE,
        mmio::kLedNotifyEventName
    );
    if (ledNotifyEvent == nullptr) { goto failed; }
    latest = {}; latest.slider[0] = 0x3f;
    packCount = 0; ioCount = 0;
    mmio::OutputSequence(*mapping.Get()).store(0, std::memory_order_relaxed);
    originalPack = reinterpret_cast<Pack>(base + 0x5a60f0);
    originalSetOutput = reinterpret_cast<SetOutput>(base + 0x2ab300);
    if (!setterBridge.Create(reinterpret_cast<void*>(SetOutputHook))) {
        Log("LED: setter ABI bridge allocation failed"); goto failed;
    }
    {
        LONG error = DetourTransactionBegin();
        if (error) { Log("LED: transaction unavailable: %ld", error); goto failed; }
        if (!error) error = DetourUpdateThread(GetCurrentThread());
        if (!error) error = DetourAttach(reinterpret_cast<void**>(&originalPack), reinterpret_cast<void*>(PackHook));
        if (!error) error = DetourAttach(reinterpret_cast<void**>(&originalSetOutput), setterBridge.Entry());
        if (!error) error = DetourTransactionCommit(); else DetourTransactionAbort();
        if (error) { Log("LED: hook installation failed: %ld", error); goto failed; }
    }
    installed = true;
    active = true;
    g_sink = sink;
    StartLedDiagnostics(diagnostics);
    Log("LED: native pending-payload and cabinet output capture installed");
    return true;
failed:
    mapping.Close();
    if (ledNotifyEvent) {
        CloseHandle(ledNotifyEvent);
        ledNotifyEvent = nullptr;
    }
    return false;
}
void ShutdownMmLedHooks() {
    StopLedDiagnostics();
    ShutdownArcadeLed();
    active = false;
    if (!installed) return;
    LONG error = DetourTransactionBegin();
    if (error) { Log("LED: detach transaction unavailable: %ld", error); return; }
    if (!error) error = DetourUpdateThread(GetCurrentThread());
    if (!error) error = DetourDetach(reinterpret_cast<void**>(&originalPack), reinterpret_cast<void*>(PackHook));
    if (!error) error = DetourDetach(reinterpret_cast<void**>(&originalSetOutput), setterBridge.Entry());
    if (!error) error = DetourTransactionCommit(); else DetourTransactionAbort();
    if (!error) installed = false;
    else Log("LED: hook detach failed: %ld", error);
    latest = {}; latest.slider[0] = 0x3f;
    PublishLedData();
    mapping.Close();
    CloseHandle(ledNotifyEvent); ledNotifyEvent = nullptr;
    Log("LED: capture calls slider=%lld cabinet=%lld", packCount, ioCount);
}
}
