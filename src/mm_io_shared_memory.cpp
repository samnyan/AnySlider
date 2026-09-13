#include "pch.h"

#include "mm_io_shared_memory.h"

#include "anyslider_log.h"

#include <cstring>
#include <string>

namespace anyslider
{
namespace
{
bool IsCompatible(const mmio::SharedBuffer& buffer)
{
    return buffer.magic == mmio::kMagic &&
        buffer.major == mmio::kAbiMajor &&
        buffer.minor == mmio::kAbiMinor &&
        buffer.struct_size == sizeof(mmio::SharedBuffer);
}

void InitializeBuffer(mmio::SharedBuffer& buffer, uint32_t capabilities)
{
    std::memset(&buffer, 0, sizeof(buffer));
    buffer.magic = mmio::kMagic;
    buffer.major = mmio::kAbiMajor;
    buffer.minor = mmio::kAbiMinor;
    buffer.struct_size = sizeof(buffer);
    buffer.capabilities = capabilities;
}

int64_t ReadSequence(const volatile int64_t* sequence)
{
    return InterlockedCompareExchange64(const_cast<volatile int64_t*>(sequence), 0, 0);
}

void WriteHeartbeat(uint64_t& heartbeatMs, uint64_t nowMs)
{
    InterlockedExchange64(reinterpret_cast<volatile int64_t*>(&heartbeatMs),
        static_cast<int64_t>(nowMs));
}

bool HasAnyGameButton(const uint64_t (&gamebtn)[mmio::kGameButtonWordCount])
{
    for (uint32_t word = 0; word < mmio::kGameButtonWordCount; ++word)
    {
        if (gamebtn[word] != 0)
            return true;
    }
    return false;
}

void CopyGameButtons(
    uint64_t (&destination)[mmio::kGameButtonWordCount],
    const uint64_t (&source)[mmio::kGameButtonWordCount])
{
    std::memcpy(destination, source, sizeof(destination));
}

void ClearGameButtons(uint64_t (&gamebtn)[mmio::kGameButtonWordCount])
{
    std::memset(gamebtn, 0, sizeof(gamebtn));
}

void GetSnapshotGameButtons(
    const mmio::InputSnapshot& snapshot,
    uint64_t (&gamebtn)[mmio::kGameButtonWordCount])
{
    if (snapshot.mode == static_cast<uint32_t>(mmio::Mode::None))
        // None时清理按键状态
        ClearGameButtons(gamebtn);
    else
        CopyGameButtons(gamebtn, snapshot.gamebtn);
}

/// <summary>
/// 读取指定序列的slot，防止在读的时候刚好被写入
/// </summary>
/// <returns>读取的是否是想要的那个序列</returns>
bool TryReadInputSlot(
    const mmio::SharedBuffer& buffer,
    int64_t expectedSequence,
    mmio::InputSnapshot& result)
{
    const auto& slot = buffer.inputs[mmio::InputSlotIndex(expectedSequence)];
    // 先检查读回是不是想要的序列
    const int64_t before = ReadSequence(&slot.sequence);
    if (before != expectedSequence)
        return false;

    std::memcpy(&result, &slot.input, sizeof(result));
    MemoryBarrier();

    // 再检查一次读回的序列，确保在读的时候不会刚好在写入这个slot
    return ReadSequence(&slot.sequence) == expectedSequence;
}

// 对上一次按键和这次最新snapshot进行差分
void SetNetButtonDiff(
    MmIoConsumer::InputFrame& frame,
    const uint64_t (&before)[mmio::kGameButtonWordCount],
    const uint64_t (&after)[mmio::kGameButtonWordCount])
{
    for (uint32_t word = 0; word < mmio::kGameButtonWordCount; ++word)
    {
        frame.gamebtn_tapped[word] = after[word] & ~before[word];
        frame.gamebtn_released[word] = before[word] & ~after[word];
        frame.gamebtn_down[word] = after[word];
    }
}
}

uint64_t MmIoNowMilliseconds()
{
    return GetTickCount64();
}

MmIoSharedMemory::~MmIoSharedMemory()
{
    Close();
}

bool MmIoSharedMemory::OpenOrCreate(std::wstring_view name, uint32_t capabilities)
{
    Close();

    const std::wstring nullTerminatedName(name);
    SetLastError(ERROR_SUCCESS);
    HANDLE mapping = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        0,
        sizeof(mmio::SharedBuffer),
        nullTerminatedName.c_str());
    if (!mapping)
    {
        Log("Could not open MMIO shared memory: %lu", GetLastError());
        return false;
    }

    const bool created = GetLastError() != ERROR_ALREADY_EXISTS;
    auto* buffer = static_cast<mmio::SharedBuffer*>(MapViewOfFile(
        mapping,
        FILE_MAP_ALL_ACCESS,
        0,
        0,
        sizeof(mmio::SharedBuffer)));
    if (!buffer)
    {
        Log("Could not map MMIO shared memory: %lu", GetLastError());
        CloseHandle(mapping);
        return false;
    }

    if (created)
        InitializeBuffer(*buffer, capabilities);
    else if (!IsCompatible(*buffer))
    {
        Log("MMIO shared memory ABI is incompatible.");
        UnmapViewOfFile(buffer);
        CloseHandle(mapping);
        return false;
    }

    mapping_ = mapping;
    buffer_ = buffer;
    return true;
}

void MmIoSharedMemory::Close()
{
    if (buffer_)
    {
        UnmapViewOfFile(buffer_);
        buffer_ = nullptr;
    }
    if (mapping_)
    {
        CloseHandle(mapping_);
        mapping_ = nullptr;
    }
}

bool MmIoSharedMemory::IsOpen() const
{
    return buffer_ != nullptr;
}

mmio::SharedBuffer* MmIoSharedMemory::Get() const
{
    return buffer_;
}

bool MmIoConsumer::Initialize(std::wstring_view name, uint32_t capabilities)
{
    if (!shared_memory_.OpenOrCreate(name, capabilities))
        return false;

    auto& endpoint = shared_memory_.Get()->hook;
    const uint64_t nowMs = MmIoNowMilliseconds();
    endpoint.process_id = GetCurrentProcessId();
    endpoint.protocol_major = mmio::kAbiMajor;
    endpoint.protocol_minor = mmio::kAbiMinor;
    endpoint.started_ms = nowMs;
    WriteHeartbeat(endpoint.heartbeat_ms, nowMs);
    ResetButtonState();
    return true;
}

void MmIoConsumer::Shutdown()
{
    shared_memory_.Close();
    ResetButtonState();
}

void MmIoConsumer::ResetButtonState()
{
    last_input_sequence_ = 0;
    ResetSourceState();
}

void MmIoConsumer::ResetSourceState()
{
    ClearGameButtons(accepted_gamebtn_down_);
    producer_process_id_ = 0;
    producer_started_ms_ = 0;
    overflow_logged_ = false;
}

bool MmIoConsumer::ReadFrame(InputFrame& frame, uint64_t maxLeaseMs)
{
    frame = {};
    auto* buffer = shared_memory_.Get();
    if (!buffer)
        return false;

    // 写入时间戳和基础信息
    const uint64_t nowMs = MmIoNowMilliseconds();
    WriteHeartbeat(buffer->hook.heartbeat_ms, nowMs);
    const uint32_t processId = buffer->producer.process_id;
    const uint64_t startedMs = buffer->producer.started_ms;

    // sessionChanged 是外部输入程序变了
    const bool sessionChanged = processId != producer_process_id_ || startedMs != producer_started_ms_;
    if (sessionChanged)
    {
        producer_process_id_ = processId;
        producer_started_ms_ = startedMs;
        last_input_sequence_ = 0;
        overflow_logged_ = false;

        // 把之前hold的按钮全部release
        if (HasAnyGameButton(accepted_gamebtn_down_))
        {
            CopyGameButtons(frame.gamebtn_released, accepted_gamebtn_down_);
            ClearGameButtons(accepted_gamebtn_down_);
            return true;
        }

        // 下一轮再读取
        return false;
    }

    const int64_t latestSequence = ReadSequence(&buffer->input_sequence);
    if (latestSequence <= 0)
    {
        // 无任何输入，同样把之前hold的按钮全部release
        const bool hadHeldButtons = HasAnyGameButton(accepted_gamebtn_down_);
        if (hadHeldButtons)
            CopyGameButtons(frame.gamebtn_released, accepted_gamebtn_down_);
        ClearGameButtons(accepted_gamebtn_down_);
        last_input_sequence_ = 0;
        return hadHeldButtons;
    }

    mmio::InputSnapshot latestSnapshot{};
    if (!TryReadInputSlot(*buffer, latestSequence, latestSnapshot))
        return false;

    // 检查输入
    const bool validMode = latestSnapshot.mode <= static_cast<uint32_t>(mmio::Mode::GamepadDualStick);
    const bool validTime = maxLeaseMs != 0 && latestSnapshot.timestamp_ms <= nowMs && nowMs - latestSnapshot.timestamp_ms <= maxLeaseMs;
    const bool inactive = latestSnapshot.mode == static_cast<uint32_t>(mmio::Mode::None);

    if (!validMode || !validTime || inactive)
    {
        const bool hadHeldButtons = HasAnyGameButton(accepted_gamebtn_down_);
        if (hadHeldButtons)
            CopyGameButtons(frame.gamebtn_released, accepted_gamebtn_down_);
        ClearGameButtons(accepted_gamebtn_down_);
        last_input_sequence_ = latestSequence;
        return hadHeldButtons;
    }

    // 防止seq回退，有可能是外部输入程序重启了
    if (latestSequence < last_input_sequence_)
    {
        const bool hadHeldButtons = HasAnyGameButton(accepted_gamebtn_down_);
        if (hadHeldButtons)
            CopyGameButtons(frame.gamebtn_released, accepted_gamebtn_down_);
        ClearGameButtons(accepted_gamebtn_down_);
        last_input_sequence_ = 0;
        return hadHeldButtons;
    }

    frame.snapshot = latestSnapshot;
    frame.source_active = true;
    uint64_t latestButtons[mmio::kGameButtonWordCount]{};
    GetSnapshotGameButtons(latestSnapshot, latestButtons);

    const bool overflow = latestSequence - last_input_sequence_ > mmio::kInputCapacity;
    // 距离上次读取超过了array容量，有一部分的snapshot应该已经被覆盖了
    if (overflow)
    {
        if (!overflow_logged_)
        {
            Log("MMIO snapshot ring overflow; resynchronizing to latest input.");
            overflow_logged_ = true;
        }
        // 直接用上次和这次的差分
        SetNetButtonDiff(frame, accepted_gamebtn_down_, latestButtons);
        CopyGameButtons(accepted_gamebtn_down_, latestButtons);
        last_input_sequence_ = latestSequence;
        return true;
    }

    uint64_t localDown[mmio::kGameButtonWordCount]{};
    CopyGameButtons(localDown, accepted_gamebtn_down_);
    uint64_t tapped[mmio::kGameButtonWordCount]{};
    uint64_t released[mmio::kGameButtonWordCount]{};
    // 读取中间所有的snapshot，不会遗漏两次查询之间的按键变化
    for (int64_t sequence = last_input_sequence_ + 1; sequence <= latestSequence; ++sequence)
    {
        mmio::InputSnapshot snapshot{};
        if (!TryReadInputSlot(*buffer, sequence, snapshot))
        {
            // 读到的不是需要的sequence，说明在追赶的时候中间已经被写入了。
            if (!overflow_logged_)
            {
                Log("MMIO snapshot ring changed while reading; resynchronizing to latest input.");
                overflow_logged_ = true;
            }
            SetNetButtonDiff(frame, accepted_gamebtn_down_, latestButtons);
            CopyGameButtons(accepted_gamebtn_down_, latestButtons);
            last_input_sequence_ = latestSequence;
            return true;
        }

        uint64_t currentDown[mmio::kGameButtonWordCount]{};
        GetSnapshotGameButtons(snapshot, currentDown);
        for (uint32_t word = 0; word < mmio::kGameButtonWordCount; ++word)
        {
            // 可以保证每次poll之间不会丢失按下和释放的状态，但如果按了多次就没法保留了。
            tapped[word] |= currentDown[word] & ~localDown[word];
            released[word] |= localDown[word] & ~currentDown[word];
            localDown[word] = currentDown[word];
        }
    }

    // 最后提交
    CopyGameButtons(frame.gamebtn_tapped, tapped);
    CopyGameButtons(frame.gamebtn_released, released);
    CopyGameButtons(frame.gamebtn_down, localDown);
    CopyGameButtons(accepted_gamebtn_down_, localDown);
    last_input_sequence_ = latestSequence;
    overflow_logged_ = false;
    return true;
}

bool MmIoPublisher::Initialize(std::wstring_view name, uint32_t capabilities)
{
    if (!shared_memory_.OpenOrCreate(name, capabilities))
        return false;

    auto* buffer = shared_memory_.Get();
    auto& endpoint = buffer->producer;
    const uint64_t nowMs = MmIoNowMilliseconds();
    endpoint.process_id = GetCurrentProcessId();
    endpoint.protocol_major = mmio::kAbiMajor;
    endpoint.protocol_minor = mmio::kAbiMinor;
    endpoint.started_ms = nowMs;
    WriteHeartbeat(endpoint.heartbeat_ms, nowMs);

    // A new producer session never exposes snapshots from the previous owner.
    InterlockedExchange64(&buffer->input_sequence, 0);
    for (auto& slot : buffer->inputs)
        InterlockedExchange64(&slot.sequence, 0);
    return true;
}

void MmIoPublisher::Shutdown()
{
    shared_memory_.Close();
}

bool MmIoPublisher::Publish(const mmio::InputSnapshot& snapshot)
{
    auto* buffer = shared_memory_.Get();
    if (!buffer)
        return false;

    const int64_t nextSequence = ReadSequence(&buffer->input_sequence) + 1;
    auto& slot = buffer->inputs[mmio::InputSlotIndex(nextSequence)];

    // Hide the slot before overwriting it so a wrapping reader rejects it.
    InterlockedExchange64(&slot.sequence, 0);
    MemoryBarrier();
    std::memcpy(&slot.input, &snapshot, sizeof(snapshot));
    MemoryBarrier();
    InterlockedExchange64(&slot.sequence, nextSequence);
    MemoryBarrier();
    InterlockedExchange64(&buffer->input_sequence, nextSequence);
    WriteHeartbeat(buffer->producer.heartbeat_ms, MmIoNowMilliseconds());
    return true;
}
}
