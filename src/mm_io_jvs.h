#pragma once

#include "mm_io/shared_memory.h"
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace anyslider::jvs
{
using Bytes = std::vector<uint8_t>;
enum class Result { Ok, NoData, Error };
class Transport
{
public:
    virtual ~Transport() = default;
    virtual Result Transact(std::span<const uint8_t> tx, Bytes& rx) = 0;
    virtual bool Sense(uint8_t& value) = 0;
};
struct Node
{
    uint8_t address = 0;
    uint8_t players = 0;
    uint8_t switches = 0;
    uint8_t command_version = 0;
    uint8_t jvs_version = 0;
    uint8_t communication_version = 0;
    std::string id;
};
Bytes Encode(uint8_t address, std::span<const uint8_t> payload);
bool Decode(std::span<const uint8_t> wire, Bytes& payload);
void MapDivaSwitches(uint8_t system, uint8_t high, uint8_t low,
    uint64_t (&buttons)[mmio::kGameButtonWordCount]);
class Session
{
public:
    // 参考原初始化逻辑的顺序与等待时间；wait 在停止时返回 false。
    bool Initialize(Transport& transport, const std::function<bool(uint32_t)>& wait);
    bool Poll(Transport& transport, uint64_t (&buttons)[mmio::kGameButtonWordCount]);
    const std::vector<Node>& Nodes() const { return nodes_; }
    const std::string& Error() const { return error_; }
private:
    bool Request(Transport&, uint8_t, const Bytes&, Bytes&);
    bool Fail(const char* reason);
    std::vector<Node> nodes_;
    std::string error_;
};
}
