#include "mm_io_arcade_slider.h"
#include <algorithm>
#include <utility>

namespace anyslider::slider
{
std::vector<uint8_t> Encode(uint8_t command, std::span<const uint8_t> payload)
{
    if (payload.size() > 255) return {};
    std::vector<uint8_t> raw{command, static_cast<uint8_t>(payload.size())};
    raw.insert(raw.end(), payload.begin(), payload.end());
    unsigned sum = 0xFF;
    for (auto byte : raw) sum += byte;
    raw.push_back(static_cast<uint8_t>(0u - sum));
    std::vector<uint8_t> wire{0xFF};
    for (auto byte : raw)
    {
        if (byte == 0xFD || byte == 0xFF)
        {
            wire.push_back(0xFD);
            wire.push_back(static_cast<uint8_t>(byte - 1));
        }
        else wire.push_back(byte);
    }
    return wire;
}

std::vector<Packet> Decoder::Feed(std::span<const uint8_t> bytes)
{
    std::vector<Packet> packets;
    for (auto byte : bytes)
    {
        // 未转义的同步字节总是开始新帧，坏长度和噪音不能吞掉后续完整帧。
        if (byte == 0xFF)
        {
            raw_.clear();
            escaped_ = false;
            active_ = true;
            continue;
        }
        if (!active_) continue;
        if (escaped_)
        {
            escaped_ = false;
            if (byte != 0xFC && byte != 0xFE)
            {
                active_ = false;
                raw_.clear();
                continue;
            }
            byte = static_cast<uint8_t>(byte + 1);
        }
        else if (byte == 0xFD)
        {
            escaped_ = true;
            continue;
        }
        raw_.push_back(byte);
        if (raw_.size() < 2 || raw_.size() != size_t(raw_[1]) + 3) continue;
        unsigned sum = 0xFF;
        for (auto value : raw_) sum += value;
        if (static_cast<uint8_t>(sum) == 0)
            packets.push_back({raw_[0], {raw_.begin() + 2, raw_.end() - 1}});
        raw_.clear();
        active_ = false;
    }
    return packets;
}
}
