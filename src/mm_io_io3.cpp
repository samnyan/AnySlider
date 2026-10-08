#include <windows.h>
#include <setupapi.h>
#include "mm_io_io3.h"
#include "mm_io_jvs.h"
#include "anyslider_log.h"
#include <algorithm>
#include <array>
#include <exception>
#include <iterator>
#include <cstring>
#include <vector>

namespace anyslider
{
namespace
{
constexpr GUID JvsGuid{0xDB6BBB45, 0xCC96, 0x4288, {0xAA,0x00,0x6C,0x00,0xD7,0x67,0xBD,0xBF}};
constexpr DWORD kHelloIoctl = 0x80006004;
constexpr DWORD kTransactIoctl = 0x8000E008;
constexpr DWORD kSenseIoctl = 0x8000600C;
constexpr uint32_t kReconnectWaitMs = 1000;
constexpr auto kInputLease = std::chrono::milliseconds(500);

// 即使分配或解析抛异常，也必须释放系统资源。
class ScopedHandle
{
public:
    explicit ScopedHandle(HANDLE handle) : handle_(handle) {}
    ~ScopedHandle()
    {
        if (handle_ && handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    }
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    HANDLE Get() const { return handle_; }
private:
    HANDLE handle_;
};
class ScopedDeviceInfo
{
public:
    explicit ScopedDeviceInfo(HDEVINFO handle) : handle_(handle) {}
    ~ScopedDeviceInfo() { SetupDiDestroyDeviceInfoList(handle_); }
    ScopedDeviceInfo(const ScopedDeviceInfo&) = delete;
    ScopedDeviceInfo& operator=(const ScopedDeviceInfo&) = delete;
private:
    HDEVINFO handle_;
};
class AmexTransport final : public jvs::Transport
{
public:
    AmexTransport(uint32_t timeout, std::stop_token stop) : timeout_(timeout), stop_(stop) {}
    ~AmexTransport()
    {
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    }
    AmexTransport(const AmexTransport&) = delete;
    AmexTransport& operator=(const AmexTransport&) = delete;
    bool Open()
    {
        HDEVINFO devices = SetupDiGetClassDevsW(&JvsGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (devices == INVALID_HANDLE_VALUE) { Log("IO3: SetupAPI failed: %lu", GetLastError()); return false; }
        ScopedDeviceInfo deviceInfo(devices);
        SP_DEVICE_INTERFACE_DATA info{};
        info.cbSize = sizeof(info);
        DWORD required = 0;
        bool opened = false;
        // 参考原初始化逻辑选择首个 JVS 接口；必须枚举设备路径，不能当作串口打开。
        if (SetupDiEnumDeviceInterfaces(devices, nullptr, &JvsGuid, 0, &info))
        {
            SetupDiGetDeviceInterfaceDetailW(devices, &info, nullptr, 0, &required, nullptr);
            if (required >= sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W))
            {
                std::vector<uint8_t> buffer(required);
                auto detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
                detail->cbSize = sizeof(*detail);
                if (SetupDiGetDeviceInterfaceDetailW(devices, &info, detail, required, nullptr, nullptr))
                {
                    handle_ = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
                    opened = handle_ != INVALID_HANDLE_VALUE;
                    if (opened) Log("IO3: opened AMEX JVS interface %ls", detail->DevicePath);
                    else Log("IO3: CreateFile failed: Win32=%lu", GetLastError());
                }
            }
        }
        if (!opened) { Log("IO3: no accessible AMEX JVS interface (driver required)"); return false; }
        std::array<uint8_t, 2> hello{};
        DWORD length = 0;
        if (Ioctl(kHelloIoctl, {}, hello, length) != jvs::Result::Ok) return false;
        if (length != hello.size())
        {
            Log("IO3: invalid HELLO completion length: %lu", length);
            return false;
        }
        Log("IO3: HELLO raw=%02X %02X", hello[0], hello[1]);
        return true;
    }
    jvs::Result Transact(std::span<const uint8_t> tx, jvs::Bytes& rx) override
    {
        rx.clear();
        if (tx.empty() || tx.size() > 255) return jvs::Result::Error;
        std::array<uint8_t, 255> buffer{};
        DWORD length = 0;
        auto result = Ioctl(kTransactIoctl, tx, buffer, length);
        if (result == jvs::Result::Ok && length <= buffer.size()) rx.assign(buffer.begin(), buffer.begin() + length);
        else if (result == jvs::Result::Ok) return jvs::Result::Error;
        return result;
    }
    bool Sense(uint8_t& value) override
    {
        DWORD length = 0;
        std::span<uint8_t> output(&value, 1);
        const bool ok = Ioctl(kSenseIoctl, {}, output, length) == jvs::Result::Ok && length == 1;
        if (ok) DebugLog("IO3: SENSE=%u", value);
        return ok;
    }
private:
    jvs::Result Ioctl(DWORD code, std::span<const uint8_t> input, std::span<uint8_t> output, DWORD& length)
    {
        if (stop_.stop_requested()) return jvs::Result::Error;
        OVERLAPPED overlapped{};
        ScopedHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event.Get())
        {
            Log("IO3: CreateEvent failed: Win32=%lu", GetLastError());
            return jvs::Result::Error;
        }
        overlapped.hEvent = event.Get();
        BOOL ok = DeviceIoControl(handle_, code, const_cast<uint8_t*>(input.data()), static_cast<DWORD>(input.size()),
            output.data(), static_cast<DWORD>(output.size()), &length, &overlapped);
        DWORD error = ok ? ERROR_SUCCESS : GetLastError();
        bool cancelled = false;
        if (!ok && error == ERROR_IO_PENDING)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_);
            while (true)
            {
                DWORD wait = WaitForSingleObject(overlapped.hEvent, 10);
                if (wait == WAIT_OBJECT_0)
                {
                    ok = GetOverlappedResult(handle_, &overlapped, &length, FALSE);
                    error = ok ? ERROR_SUCCESS : GetLastError();
                    break;
                }
                if (wait == WAIT_FAILED || stop_.stop_requested() || std::chrono::steady_clock::now() >= deadline)
                {
                    Log("IO3: IOCTL 0x%08lX cancelled (%s)", code,
                        stop_.stop_requested() ? "shutdown" : wait == WAIT_FAILED ? "wait failed" : "timeout");
                    CancelIoEx(handle_, &overlapped);
                    // 取消请求不等于完成：内核结束访问前，缓冲区与 OVERLAPPED 必须保持有效。
                    GetOverlappedResult(handle_, &overlapped, &length, TRUE);
                    error = ERROR_OPERATION_ABORTED;
                    cancelled = true;
                    break;
                }
            }
        }
        if (cancelled) return jvs::Result::Error;
        if (ok) return jvs::Result::Ok;
        if (error == ERROR_NO_DATA_DETECTED) return jvs::Result::NoData;
        Log("IO3: IOCTL 0x%08lX failed: Win32=%lu", code, error);
        return jvs::Result::Error;
    }
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    uint32_t timeout_;
    std::stop_token stop_;
};
}

MmIoIo3Frontend::~MmIoIo3Frontend() { Shutdown(); }
void MmIoIo3Frontend::Initialize(const MmIoIo3Config& config)
{
    Shutdown();
    config_ = config;
    if (config.enabled) worker_ = std::jthread([this](std::stop_token stop) { Run(stop); });
}
void MmIoIo3Frontend::Shutdown()
{
    if (worker_.joinable())
    {
        worker_.request_stop();
        wake_.notify_all();
        worker_.join();
    }
    std::lock_guard lock(mutex_);
    frame_ = {};
}
bool MmIoIo3Frontend::Wait(std::stop_token stop, uint32_t milliseconds)
{
    std::unique_lock lock(mutex_);
    wake_.wait_for(lock, stop, std::chrono::milliseconds(milliseconds), [] { return false; });
    return !stop.stop_requested();
}
void MmIoIo3Frontend::Publish(const uint64_t (&buttons)[mmio::kGameButtonWordCount], bool connected)
{
    std::lock_guard lock(mutex_);
    // 累积边沿直到消费，避免两次游戏帧之间的短按被最新快照覆盖。
    for (size_t i = 0; i < mmio::kGameButtonWordCount; ++i)
    {
        frame_.gamebtn_tapped[i] |= buttons[i] & ~frame_.gamebtn_down[i];
        frame_.gamebtn_released[i] |= frame_.gamebtn_down[i] & ~buttons[i];
        frame_.gamebtn_down[i] = buttons[i];
    }
    frame_.connected = connected;
    updated_ = std::chrono::steady_clock::now();
}
MmIoIo3Frame MmIoIo3Frontend::Consume()
{
    std::lock_guard lock(mutex_);
    if (frame_.connected && std::chrono::steady_clock::now() - updated_ > kInputLease)
    {
        for (size_t i = 0; i < mmio::kGameButtonWordCount; ++i)
        {
            frame_.gamebtn_released[i] |= frame_.gamebtn_down[i];
            frame_.gamebtn_down[i] = 0;
        }
        frame_.connected = false;
    }
    const auto result = frame_;
    std::memset(frame_.gamebtn_tapped, 0, sizeof(frame_.gamebtn_tapped));
    std::memset(frame_.gamebtn_released, 0, sizeof(frame_.gamebtn_released));
    return result;
}
void MmIoIo3Frontend::Run(std::stop_token stop)
{
    while (!stop.stop_requested())
    {
        uint64_t buttons[mmio::kGameButtonWordCount]{};
        try
        {
            AmexTransport transport(config_.timeout_ms, stop);
            jvs::Session session;
            if (transport.Open() && session.Initialize(transport, [this, stop](uint32_t ms) { return Wait(stop, ms); }))
            {
                for (const auto& node : session.Nodes())
                    Log("IO3: node=%u id=%s players=%u switches=%u versions=%02X/%02X/%02X",
                        node.address, node.id.c_str(), node.players, node.switches,
                        node.command_version, node.jvs_version, node.communication_version);
                while (!stop.stop_requested() && session.Poll(transport, buttons))
                {
                    Publish(buttons, true);
                    if (!Wait(stop, config_.poll_interval_ms)) break;
                }
            }
            if (!stop.stop_requested() && !session.Error().empty()) Log("IO3: %s", session.Error().c_str());
        }
        catch (const std::exception& exception) { Log("IO3: worker error: %s", exception.what()); }
        std::fill(std::begin(buttons), std::end(buttons), 0);
        Publish(buttons, false);
        if (!Wait(stop, kReconnectWaitMs)) break;
    }
}
}
