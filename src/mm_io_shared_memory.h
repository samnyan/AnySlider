#pragma once

#include "mm_io/shared_memory.h"

#include <string_view>

namespace anyslider
{
uint64_t MmIoNowMicroseconds();

class MmIoSharedMemory
{
public:
    MmIoSharedMemory() = default;
    ~MmIoSharedMemory();

    MmIoSharedMemory(const MmIoSharedMemory&) = delete;
    MmIoSharedMemory& operator=(const MmIoSharedMemory&) = delete;

    bool OpenOrCreate(std::wstring_view name, uint32_t capabilities);
    void Close();

    [[nodiscard]] bool IsOpen() const;
    [[nodiscard]] mmio::SharedBuffer* Get() const;

private:
    void* mapping_ = nullptr;
    mmio::SharedBuffer* buffer_ = nullptr;
};

class MmIoConsumer
{
public:
    struct InputFrame
    {
        mmio::InputSnapshot snapshot{};
        uint64_t gamebtn_tapped[mmio::kGameButtonWordCount]{};
        uint64_t gamebtn_released[mmio::kGameButtonWordCount]{};
        uint64_t gamebtn_down[mmio::kGameButtonWordCount]{};
        bool source_active = false;
    };

    bool Initialize(std::wstring_view name, uint32_t capabilities);
    void Shutdown();
    bool ReadFrame(InputFrame& frame, uint64_t maxLeaseMs);

private:
    void ResetButtonState();
    void ResetSourceState();

    MmIoSharedMemory shared_memory_;
    int64_t last_event_sequence_ = 0;
    uint64_t gamebtn_down_[mmio::kGameButtonWordCount]{};
    uint32_t source_id_ = 0;
    uint64_t producer_started_us_ = 0;
    bool source_was_active_ = false;
    bool overflow_logged_ = false;
    bool pending_source_resync_ = false;
};

class MmIoPublisher
{
public:
    bool Initialize(std::wstring_view name, uint32_t capabilities);
    void Shutdown();
    bool Publish(const mmio::InputSnapshot& snapshot);
    [[nodiscard]] int64_t LastConsumedSequence() const;
    [[nodiscard]] int64_t LastConsumedEventSequence() const;

private:
    MmIoSharedMemory shared_memory_;
    uint64_t last_gamebtn_[mmio::kGameButtonWordCount]{};
    bool has_published_snapshot_ = false;
};
}
