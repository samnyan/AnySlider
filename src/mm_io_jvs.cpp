#include "mm_io_jvs.h"
#include <algorithm>
#include <array>
#include <iterator>
#include <utility>

namespace anyslider::jvs
{
namespace
{
constexpr uint8_t kSenseDisconnected = 1;
constexpr uint8_t kSenseAssigned = 2;
constexpr uint8_t kSenseUnassigned = 3;
constexpr uint8_t kMaxAddress = 31;
constexpr int kQueryAttempts = 3;
constexpr uint32_t kResetWaitMs = 1000;
constexpr uint32_t kAddressWaitMs = 1000;
constexpr uint32_t kNextAddressWaitMs = 100;

bool IsDivaSwitchNode(const Node& node)
{
    return node.id.find("837-14572") != std::string::npos && node.players >= 1 &&
        node.players <= 4 && node.switches >= 10 && node.switches <= 16;
}

bool ParseCapabilities(const Bytes& response, Node& node)
{
    // 在临时对象中解析；坏包不得将部分字段带入下一次重试。
    Node parsed;
    parsed.address = node.address;
    size_t pos = 2;
    while (pos < response.size() && response[pos] != 0)
    {
        if (response.size() - pos < 4) return false;
        if (response[pos] == 1)
        {
            parsed.players = response[pos + 1];
            parsed.switches = response[pos + 2];
        }
        pos += 4;
    }
    if (pos >= response.size() || response[pos++] != 0) return false;
    if (response.size() - pos != 6 || response[pos] != 1 ||
        response[pos + 2] != 1 || response[pos + 4] != 1) return false;
    parsed.command_version = response[pos + 1];
    parsed.jvs_version = response[pos + 3];
    parsed.communication_version = response[pos + 5];
    node = std::move(parsed);
    return true;
}
}

Bytes Encode(uint8_t address, std::span<const uint8_t> payload)
{
    if (payload.size() > 254) return {};
    Bytes raw{address, static_cast<uint8_t>(payload.size() + 1)};
    raw.insert(raw.end(), payload.begin(), payload.end());
    unsigned sum = 0;
    for (auto value : raw) sum += value;
    raw.push_back(static_cast<uint8_t>(sum));
    Bytes wire{0xE0};
    for (auto value : raw)
    {
        if (value == 0xE0 || value == 0xD0)
        {
            wire.push_back(0xD0);
            wire.push_back(static_cast<uint8_t>(value - 1));
        }
        else wire.push_back(value);
    }
    // AMEX 接口限制的是转义后帧长度，不是未转义的 payload 长度。
    return wire.size() <= 255 ? wire : Bytes{};
}

bool Decode(std::span<const uint8_t> wire, Bytes& payload)
{
    payload.clear();
    if (wire.size() < 4 || wire.size() > 255 || wire.front() != 0xE0) return false;
    Bytes raw;
    for (size_t i = 1; i < wire.size(); ++i)
    {
        auto value = wire[i];
        if (value == 0xE0) return false;
        if (value == 0xD0)
        {
            if (++i == wire.size() || (wire[i] != 0xCF && wire[i] != 0xDF)) return false;
            value = static_cast<uint8_t>(wire[i] + 1);
        }
        raw.push_back(value);
    }
    if (raw.size() < 3 || raw[0] != 0 || raw[1] == 0 || raw.size() != size_t(raw[1]) + 2) return false;
    unsigned sum = 0;
    for (size_t i = 0; i + 1 < raw.size(); ++i) sum += raw[i];
    if (static_cast<uint8_t>(sum) != raw.back()) return false;
    payload.assign(raw.begin() + 2, raw.end() - 1);
    return !payload.empty() && payload[0] == 1;
}

void MapDivaSwitches(uint8_t system, uint8_t high, uint8_t low,
    uint64_t (&buttons)[mmio::kGameButtonWordCount])
{
    // 玩家输入按大端顺序传输；此映射仅使用 P1，保持动作编号与线上位编号独立。
    const uint16_t player = static_cast<uint16_t>((high << 8) | low);
    const std::array<std::pair<uint16_t, uint32_t>, 6> mapping{{
        {0x0200, mmio::Triangle}, {0x0100, mmio::Square},
        {0x0080, mmio::Cross}, {0x0040, mmio::Circle},
        {0x8000, mmio::Start}, {0x4000, mmio::Service}}};
    for (auto [mask, action] : mapping)
        if (player & mask) mmio::SetGameButton(buttons, action);
    if (system & 0x80) mmio::SetGameButton(buttons, mmio::Test);
}

bool Session::Fail(const char* reason)
{
    error_ = reason;
    return false;
}

bool Session::Request(Transport& transport, uint8_t address, const Bytes& request, Bytes& response)
{
    Bytes wire = Encode(address, request), rx;
    if (wire.empty() || transport.Transact(wire, rx) != Result::Ok || !Decode(rx, response)) return false;
    return response.size() >= 2 && response[1] == 1;
}

bool Session::Initialize(Transport& transport, const std::function<bool(uint32_t)>& wait)
{
    nodes_.clear();
    error_.clear();
    Bytes response;
    // 参考原初始化逻辑：广播复位两次，无 ACK，每次等待 1000ms。
    for (int i = 0; i < 2; ++i)
    {
        Bytes rx;
        if (transport.Transact(Encode(0xFF, Bytes{0xF0, 0xD9}), rx) != Result::NoData)
            return Fail("reset did not complete with ERROR_NO_DATA_DETECTED");
        if (!wait(kResetWaitMs)) return Fail("stopped during reset");
    }
    uint8_t sense = 0;
    if (!transport.Sense(sense)) return Fail("SENSE failed");
    if (sense == kSenseDisconnected) return Fail("no downstream IO3 board");
    if (sense != kSenseUnassigned) return Fail("expected unassigned SENSE after reset");
    uint8_t last = 0;
    for (uint8_t address = 1; address <= kMaxAddress; ++address)
    {
        bool assigned = false;
        for (int retry = 0; retry < kQueryAttempts && !assigned; ++retry)
            assigned = Request(transport, 0xFF, Bytes{0xF1, address}, response) && response.size() == 2;
        if (!assigned) return Fail("address assignment failed");
        last = address;
        if (!wait(kAddressWaitMs) || !transport.Sense(sense)) return Fail("SENSE after address failed");
        if (sense == kSenseAssigned) break;
        if (sense != kSenseUnassigned || address == kMaxAddress) return Fail("invalid or unterminated JVS chain");
        if (!wait(kNextAddressWaitMs)) return Fail("stopped during address assignment");
    }
    for (uint8_t address = 1; address <= last; ++address)
    {
        Node node;
        node.address = address;
        bool queried = false;
        for (int retry = 0; retry < kQueryAttempts && !queried; ++retry)
        {
            if (!Request(transport, address, Bytes{0x14, 0x11, 0x12, 0x13}, response)) continue;
            queried = ParseCapabilities(response, node);
        }
        // 能力查询失败的节点不保留；板号查询只尝试一次。
        if (!queried || !Request(transport, address, Bytes{0x10}, response)) continue;
        auto end = std::find(response.begin() + 2, response.end(), uint8_t{0});
        if (end == response.end() || end + 1 != response.end()) continue;
        if (std::any_of(response.begin() + 2, end, [](uint8_t c){ return c < 0x20 || c > 0x7E; })) continue;
        node.id.assign(response.begin() + 2, end);
        nodes_.push_back(node);
    }
    if (!std::any_of(nodes_.begin(), nodes_.end(), IsDivaSwitchNode))
        return Fail("no usable 837-14572 switch node");
    return true;
}

bool Session::Poll(Transport& transport, uint64_t (&buttons)[mmio::kGameButtonWordCount])
{
    std::fill(std::begin(buttons), std::end(buttons), 0);
    Bytes response;
    for (const Node& node : nodes_)
    {
        if (!IsDivaSwitchNode(node)) continue;
        if (!Request(transport, node.address, Bytes{0x20, node.players, 2}, response) ||
            response.size() != size_t(3 + node.players * 2))
        {
            std::fill(std::begin(buttons), std::end(buttons), 0);
            return Fail("switch input transaction failed");
        }
        MapDivaSwitches(response[2], response[3], response[4], buttons);
    }
    return true;
}
}
