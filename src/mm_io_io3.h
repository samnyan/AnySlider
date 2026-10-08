#pragma once
#include "mm_io/shared_memory.h"
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace anyslider
{
struct MmIoIo3Config
{
    bool enabled = false;
    uint32_t poll_interval_ms = 4;
    uint32_t timeout_ms = 1000;
};
struct MmIoIo3Frame
{
    uint64_t gamebtn_down[mmio::kGameButtonWordCount]{};
    uint64_t gamebtn_tapped[mmio::kGameButtonWordCount]{};
    uint64_t gamebtn_released[mmio::kGameButtonWordCount]{};
    bool connected = false;
};
class MmIoIo3Frontend
{
public:
    ~MmIoIo3Frontend();
    void Initialize(const MmIoIo3Config& config);
    void Shutdown();
    MmIoIo3Frame Consume();
private:
    void Run(std::stop_token stop);
    bool Wait(std::stop_token stop, uint32_t milliseconds);
    void Publish(const uint64_t (&buttons)[mmio::kGameButtonWordCount], bool connected);
    MmIoIo3Config config_;
    std::mutex mutex_;
    std::condition_variable_any wake_;
    MmIoIo3Frame frame_;
    std::chrono::steady_clock::time_point updated_{};
    // 最后声明，确保异常销毁时线程先结束，再销毁其依赖的同步对象。
    std::jthread worker_;
};
}
