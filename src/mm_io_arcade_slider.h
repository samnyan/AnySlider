#pragma once
#include "mm_io/shared_memory.h"
#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace anyslider
{
namespace slider
{
struct Packet
{
    uint8_t command = 0;
    std::vector<uint8_t> payload;
};
std::vector<uint8_t> Encode(uint8_t command, std::span<const uint8_t> payload = {});
class Decoder
{
public:
    std::vector<Packet> Feed(std::span<const uint8_t> bytes);
private:
    std::vector<uint8_t> raw_;
    bool escaped_ = false;
    bool active_ = false;
};
}
struct MmIoArcadeSliderConfig
{
    bool enabled = false;
    std::string port = "COM11";
    bool assert_dtr_rts = true;
};
struct MmIoArcadeSliderFrame
{
    uint8_t touch_cells[mmio::kTouchCellCount]{};
    bool connected = false;
};
class MmIoArcadeSliderFrontend
{
public:
    ~MmIoArcadeSliderFrontend();
    void Initialize(const MmIoArcadeSliderConfig& config);
    void Shutdown();
    MmIoArcadeSliderFrame Consume();
private:
    void Run(std::stop_token stop);
    bool Wait(std::stop_token stop, uint32_t milliseconds);
    void Publish(const std::array<uint8_t, mmio::kTouchCellCount>& cells, bool connected);
    MmIoArcadeSliderConfig config_;
    std::mutex mutex_;
    std::condition_variable_any wake_;
    MmIoArcadeSliderFrame frame_;
    std::chrono::steady_clock::time_point updated_{};
    std::jthread worker_;
};
}
