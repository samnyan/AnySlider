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

bool HasAnyGameButton(const uint64_t (&gamebtn)[mmio::kGameButtonWordCount])
{
    for (uint32_t word = 0; word < mmio::kGameButtonWordCount; ++word)
    {
        if (gamebtn[word] != 0)
        {
            return true;
        }
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

void ApplyGameButtonEvent(
    uint64_t (&down)[mmio::kGameButtonWordCount],
    const uint64_t (&pressed)[mmio::kGameButtonWordCount],
    const uint64_t (&released)[mmio::kGameButtonWordCount])
{
    for (uint32_t word = 0; word < mmio::kGameButtonWordCount; ++word)
    {
        down[word] |= pressed[word];
        down[word] &= ~released[word];
    }
}
}

uint64_t MmIoNowMicroseconds()
{
    return GetTickCount64() * 1000;
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
    {
        InitializeBuffer(*buffer, capabilities);
    }
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
    {
        return false;
    }

    auto& endpoint = shared_memory_.Get()->hook;
    endpoint.process_id = GetCurrentProcessId();
    endpoint.protocol_major = mmio::kAbiMajor;
    endpoint.protocol_minor = mmio::kAbiMinor;
    endpoint.started_us = MmIoNowMicroseconds();
    endpoint.heartbeat_us = endpoint.started_us;
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
    last_event_sequence_ = 0;
    ClearGameButtons(gamebtn_down_);
    source_id_ = 0;
    mode_ = 0;
    producer_started_us_ = 0;
    source_was_active_ = false;
    overflow_logged_ = false;
}

bool MmIoConsumer::ReadFrame(InputFrame& frame, uint64_t maxLeaseMs)
{
    frame = {};
    auto* buffer = shared_memory_.Get();
    if (!buffer)
    {
        return false;
    }

    auto& hook = buffer->hook;
    hook.heartbeat_us = MmIoNowMicroseconds();
    bool sourceExpired = false;

    for (int attempt = 0; attempt != 3; ++attempt)
    {
        const int64_t firstSequence = ReadSequence(&buffer->input_sequence);
        if (firstSequence & 1)
        {
            continue;
        }

        mmio::InputSnapshot candidate{};
        std::memcpy(&candidate, &buffer->input, sizeof(candidate));
        MemoryBarrier();
        const int64_t secondSequence = ReadSequence(&buffer->input_sequence);
        if (firstSequence != secondSequence || (secondSequence & 1))
        {
            continue;
        }

        const uint64_t nowUs = MmIoNowMicroseconds();
        if (candidate.mode > static_cast<uint32_t>(mmio::Mode::GamepadDualStick) ||
            candidate.lease_ms == 0 || candidate.lease_ms > maxLeaseMs ||
            candidate.timestamp_us > nowUs ||
            nowUs - candidate.timestamp_us > candidate.lease_ms * 1000)
        {
            sourceExpired = true;
            break;
        }

        frame.snapshot = candidate;
        InterlockedExchange64(&hook.input_sequence, secondSequence);
        if (candidate.mode == static_cast<uint32_t>(mmio::Mode::None))
        {
            sourceExpired = true;
            break;
        }

        frame.source_active = true;
        const uint64_t producerStartedUs = buffer->producer.started_us;
        const bool sourceChanged = !source_was_active_ ||
            source_id_ != candidate.source_id || mode_ != candidate.mode ||
            producer_started_us_ != producerStartedUs;
        if (sourceChanged)
        {
            CopyGameButtons(frame.gamebtn_released, gamebtn_down_);
            ClearGameButtons(gamebtn_down_);
            source_id_ = candidate.source_id;
            mode_ = candidate.mode;
            producer_started_us_ = producerStartedUs;
            source_was_active_ = true;
            overflow_logged_ = false;
        }

        if (HasAnyGameButton(frame.gamebtn_released))
        {
            CopyGameButtons(frame.gamebtn_down, gamebtn_down_);
            return true;
        }

        const int64_t publishedEventSequence = ReadSequence(&buffer->button_event_sequence);
        if (publishedEventSequence < last_event_sequence_)
        {
            last_event_sequence_ = publishedEventSequence;
            CopyGameButtons(gamebtn_down_, candidate.gamebtn);
        }
        else if (publishedEventSequence - last_event_sequence_ > mmio::kButtonEventCapacity)
        {
            if (!overflow_logged_)
            {
                Log("MMIO button event queue overflow; resynchronizing gamebtn state.");
                overflow_logged_ = true;
            }
            last_event_sequence_ = publishedEventSequence;
            CopyGameButtons(gamebtn_down_, candidate.gamebtn);
            InterlockedExchange64(&hook.event_sequence, last_event_sequence_);
        }
        else
        {
            bool consumedMatchingEvent = false;
            while (last_event_sequence_ < publishedEventSequence)
            {
                const int64_t expectedSequence = last_event_sequence_ + 1;
                const auto& slot = buffer->button_events[
                    static_cast<uint64_t>(expectedSequence) % mmio::kButtonEventCapacity];
                mmio::ButtonEvent event{};
                std::memcpy(&event, &slot, sizeof(event));
                MemoryBarrier();
                if (ReadSequence(&slot.sequence) != expectedSequence)
                {
                    break;
                }

                last_event_sequence_ = expectedSequence;
                InterlockedExchange64(&hook.event_sequence, last_event_sequence_);
                if (event.source_id != candidate.source_id ||
                    event.mode != candidate.mode ||
                    event.producer_started_us != producerStartedUs)
                {
                    continue;
                }

                CopyGameButtons(frame.gamebtn_tapped, event.gamebtn_pressed);
                CopyGameButtons(frame.gamebtn_released, event.gamebtn_released);
                ApplyGameButtonEvent(
                    gamebtn_down_,
                    event.gamebtn_pressed,
                    event.gamebtn_released);
                consumedMatchingEvent = true;
                break;
            }

            if (sourceChanged && !consumedMatchingEvent &&
                HasAnyGameButton(candidate.gamebtn))
            {
                CopyGameButtons(frame.gamebtn_tapped, candidate.gamebtn);
                CopyGameButtons(gamebtn_down_, candidate.gamebtn);
            }
        }

        CopyGameButtons(frame.gamebtn_down, gamebtn_down_);
        return true;
    }

    if (!sourceExpired || !HasAnyGameButton(gamebtn_down_))
    {
        if (sourceExpired)
        {
            ResetButtonState();
        }
        return false;
    }

    CopyGameButtons(frame.gamebtn_released, gamebtn_down_);
    ResetButtonState();
    return true;
}

bool MmIoPublisher::Initialize(std::wstring_view name, uint32_t capabilities)
{
    if (!shared_memory_.OpenOrCreate(name, capabilities))
    {
        return false;
    }

    auto& endpoint = shared_memory_.Get()->producer;
    endpoint.process_id = GetCurrentProcessId();
    endpoint.protocol_major = mmio::kAbiMajor;
    endpoint.protocol_minor = mmio::kAbiMinor;
    endpoint.started_us = MmIoNowMicroseconds();
    endpoint.heartbeat_us = endpoint.started_us;
    ClearGameButtons(last_gamebtn_);
    has_published_snapshot_ = false;
    return true;
}

void MmIoPublisher::Shutdown()
{
    shared_memory_.Close();
    ClearGameButtons(last_gamebtn_);
    has_published_snapshot_ = false;
}

bool MmIoPublisher::Publish(const mmio::InputSnapshot& snapshot)
{
    auto* buffer = shared_memory_.Get();
    if (!buffer)
    {
        return false;
    }

    const int64_t currentSequence = ReadSequence(&buffer->input_sequence);
    if (currentSequence & 1)
    {
        return false;
    }

    uint64_t pressed[mmio::kGameButtonWordCount]{};
    uint64_t released[mmio::kGameButtonWordCount]{};
    bool changed = false;
    for (uint32_t word = 0; word < mmio::kGameButtonWordCount; ++word)
    {
        const uint64_t previous = has_published_snapshot_ ? last_gamebtn_[word] : 0;
        pressed[word] = snapshot.gamebtn[word] & ~previous;
        released[word] = previous & ~snapshot.gamebtn[word];
        changed = changed || pressed[word] != 0 || released[word] != 0;
    }

    InterlockedExchange64(&buffer->input_sequence, currentSequence + 1);
    MemoryBarrier();
    std::memcpy(&buffer->input, &snapshot, sizeof(snapshot));
    if (changed)
    {
        const int64_t nextEventSequence =
            ReadSequence(&buffer->button_event_sequence) + 1;
        auto& event = buffer->button_events[
            static_cast<uint64_t>(nextEventSequence) % mmio::kButtonEventCapacity];
        event.source_id = snapshot.source_id;
        event.mode = snapshot.mode;
        CopyGameButtons(event.gamebtn_pressed, pressed);
        CopyGameButtons(event.gamebtn_released, released);
        event.producer_started_us = buffer->producer.started_us;
        event.timestamp_us = snapshot.timestamp_us;
        MemoryBarrier();
        InterlockedExchange64(&event.sequence, nextEventSequence);
        MemoryBarrier();
        InterlockedExchange64(&buffer->button_event_sequence, nextEventSequence);
    }
    buffer->producer.heartbeat_us = MmIoNowMicroseconds();
    MemoryBarrier();
    InterlockedExchange64(&buffer->input_sequence, currentSequence + 2);
    CopyGameButtons(last_gamebtn_, snapshot.gamebtn);
    has_published_snapshot_ = true;
    return true;
}

int64_t MmIoPublisher::LastConsumedSequence() const
{
    const auto* buffer = shared_memory_.Get();
    return buffer ? ReadSequence(&buffer->hook.input_sequence) : 0;
}

int64_t MmIoPublisher::LastConsumedEventSequence() const
{
    const auto* buffer = shared_memory_.Get();
    return buffer ? ReadSequence(&buffer->hook.event_sequence) : 0;
}

}
