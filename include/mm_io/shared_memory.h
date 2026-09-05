#pragma once

#include <cstddef>
#include <cstdint>

namespace anyslider::mmio
{
inline constexpr wchar_t kSharedMemoryName[] = L"Local\\MMIO_SHARED_BUFFER";
inline constexpr uint32_t kMagic = 0x4F494D4D; // MMIO
inline constexpr uint16_t kAbiMajor = 1;
inline constexpr uint16_t kAbiMinor = 0;
inline constexpr uint32_t kTouchCellCount = 32;
inline constexpr uint32_t kGameButtonWordCount = 3;
inline constexpr uint32_t kGameButtonCount = kGameButtonWordCount * 64;
inline constexpr uint32_t kButtonEventCapacity = 128;

enum class Mode : uint32_t
{
    None = 0,
    ArcadeSlider = 1,
    GamepadDualStick = 2,
};

// Raw MM+ unified input action IDs. gamebtn contains all 192 raw action bits,
// so external frontends can also pass through unlisted IDs when needed.
enum GameButton : uint32_t
{
    Test = 0,
    Service = 1,
    Start = 2,
    DpadUp = 3,
    DpadDown = 4,
    DpadLeft = 5,
    DpadRight = 6,
    Square = 7,
    Triangle = 8,
    Circle = 9,
    Cross = 10,
    L1 = 11,
    R1 = 12,
    L2 = 13,
    R2 = 14,
    Select = 15,
    L3 = 16,
    R3 = 17,
    Sw1 = 18,
    Sw2 = 19,
    Pause = 160,
};

constexpr bool IsGameButtonValid(uint32_t action)
{
    return action < kGameButtonCount;
}

constexpr bool IsGameButtonDown(const uint64_t (&gamebtn)[kGameButtonWordCount], uint32_t action)
{
    return IsGameButtonValid(action) &&
        (gamebtn[action / 64] & (1ull << (action % 64))) != 0;
}

constexpr void SetGameButton(
    uint64_t (&gamebtn)[kGameButtonWordCount],
    uint32_t action,
    bool down = true)
{
    if (!IsGameButtonValid(action))
    {
        return;
    }

    const uint64_t bit = 1ull << (action % 64);
    if (down)
    {
        gamebtn[action / 64] |= bit;
    }
    else
    {
        gamebtn[action / 64] &= ~bit;
    }
}

enum GamepadSlide : uint32_t
{
    SlideLeft1 = 1u << 0,
    SlideRight1 = 1u << 1,
    SlideLeft2 = 1u << 2,
    SlideRight2 = 1u << 3,
};

enum Capability : uint32_t
{
    SupportsArcadeSlider = 1u << 0,
    SupportsGamepadDualStick = 1u << 1,
    SupportsKeyboardFrontend = 1u << 2,
};

struct InputSnapshot
{
    uint32_t mode;
    uint64_t gamebtn[kGameButtonWordCount];
    uint8_t touch_cells[kTouchCellCount];
    uint32_t gamepad_slide;
    uint32_t source_id;
    uint64_t timestamp_us;
    uint64_t lease_ms;
};

// A producer appends one of these whenever gamebtn changes. The consumer applies
// at most one matching event per game input frame so an intervening press and
// release remain visible to the game.
struct alignas(8) ButtonEvent
{
    volatile int64_t sequence;
    uint32_t source_id;
    uint32_t mode;
    uint64_t gamebtn_pressed[kGameButtonWordCount];
    uint64_t gamebtn_released[kGameButtonWordCount];
    uint64_t producer_started_us;
    uint64_t timestamp_us;
};

struct Endpoint
{
    uint32_t process_id;
    uint16_t protocol_major;
    uint16_t protocol_minor;
    uint64_t started_us;
    uint64_t heartbeat_us;
    volatile int64_t input_sequence;
    volatile int64_t event_sequence;
};

struct alignas(8) SharedBuffer
{
    uint32_t magic;
    uint16_t major;
    uint16_t minor;
    uint32_t struct_size;
    uint32_t capabilities;
    volatile int64_t input_sequence;
    InputSnapshot input;
    volatile int64_t button_event_sequence;
    ButtonEvent button_events[kButtonEventCapacity];
    Endpoint producer;
    Endpoint hook;
};

static_assert(sizeof(InputSnapshot) == 88);
static_assert(sizeof(ButtonEvent) == 80);
static_assert(offsetof(SharedBuffer, input_sequence) == 16);
static_assert(offsetof(SharedBuffer, input) == 24);
static_assert(alignof(SharedBuffer) >= alignof(int64_t));
}
