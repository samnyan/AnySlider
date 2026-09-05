#pragma once

#include <cstddef>
#include <cstdint>

// MM+ shared-memory IO ABI v1.0.
//
// This file is the normative layout for external producers. It is intentionally
// limited to fixed-width integer fields and natural x64 alignment, so a C, C#,
// Rust, or other producer can redeclare the same layout.
//
// Interoperability requirements:
//   * Windows x64 only; use 8-byte structure alignment. Do not use packed structs.
//   * SharedBuffer is exactly 10,440 bytes. Create/open the named mapping with
//     read/write access and this exact length.
//   * 64-bit sequence fields are shared atomics. C++ uses Interlocked*; C# should
//     use Interlocked/Volatile against an 8-byte-aligned field.
//   * ABI compatibility requires magic, major, minor, and struct_size to match.
//   * A producer must publish both the latest InputSnapshot and an ordered
//     ButtonEvent for every gamebtn transition. The snapshot alone is not enough
//     to preserve tapped/released edges between game input frames.
//
// gamebtn stores raw MM+ action IDs. For action N:
//     word = N / 64; bit = 1ULL << (N % 64)
// Actions not named below remain available for direct raw pass-through.

namespace anyslider::mmio
{
inline constexpr wchar_t kSharedMemoryName[] = L"Local\\MMIO_SHARED_BUFFER";
inline constexpr uint32_t kMagic = 0x4F494D4D; // ASCII "MMIO", little-endian.
inline constexpr uint16_t kAbiMajor = 1;
inline constexpr uint16_t kAbiMinor = 0;

// The game maintains three 64-bit input-state words: raw action IDs 0..191.
inline constexpr uint32_t kTouchCellCount = 32;
inline constexpr uint32_t kGameButtonWordCount = 3;
inline constexpr uint32_t kGameButtonCount = kGameButtonWordCount * 64;
inline constexpr uint32_t kButtonEventCapacity = 128;
inline constexpr uint32_t kSharedBufferSize = 10440;

enum class Mode : uint32_t
{
    // No active source. Publish this with an empty gamebtn before a deliberate
    // source or mode switch, so no held state survives the switch.
    None = 0,

    // gamebtn plus touch_cells feed the native 32-cell arcade slider path.
    ArcadeSlider = 1,

    // gamebtn plus gamepad_slide feed the two independent stick-slide paths.
    GamepadDualStick = 2,
};

// Known MM+ raw action IDs. The values are action numbers, not pre-shifted masks.
enum GameButton : uint32_t
{
    Test = 0,       // Arcade TEST button.
    Service = 1,    // Arcade SERVICE button.
    Start = 2,      // Arcade/game START action.
    DpadUp = 3,
    DpadDown = 4,
    DpadLeft = 5,
    DpadRight = 6,
    Square = 7,     // PS Square / Xbox X.
    Triangle = 8,   // PS Triangle / Xbox Y.
    Circle = 9,     // PS Circle / Xbox B.
    Cross = 10,     // PS Cross / Xbox A.
    L1 = 11,
    R1 = 12,
    L2 = 13,
    R2 = 14,
    Select = 15,    // Select / Share class button.
    L3 = 16,
    R3 = 17,
    Sw1 = 18,       // Arcade slider-derived system switch 1.
    Sw2 = 19,       // Arcade slider-derived system switch 2.
    Pause = 160,    // PS Options / keyboard Escape pause action.
};

constexpr bool IsGameButtonValid(uint32_t action)
{
    return action < kGameButtonCount;
}

constexpr bool IsGameButtonDown(
    const uint64_t (&gamebtn)[kGameButtonWordCount],
    uint32_t action)
{
    return IsGameButtonValid(action) &&
        (gamebtn[action / 64] & (1ull << (action % 64))) != 0;
}

// C++ convenience only. Non-C++ producers use the word/bit formula above.
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

// Four semantic directions for two independent gamepad slide contacts. These are
// only consumed when InputSnapshot::mode is GamepadDualStick. Bit 0/1 is stick 1;
// bit 2/3 is stick 2. Both sticks may hold the same direction simultaneously.
enum GamepadSlide : uint32_t
{
    SlideLeft1 = 1u << 0,
    SlideRight1 = 1u << 1,
    SlideLeft2 = 1u << 2,
    SlideRight2 = 1u << 3,
};

// Capabilities advertised by the hook in SharedBuffer::capabilities.
enum Capability : uint32_t
{
    SupportsArcadeSlider = 1u << 0,
    SupportsGamepadDualStick = 1u << 1,
    SupportsKeyboardFrontend = 1u << 2,
};

// Current input level, copied under SharedBuffer::input_sequence.
//
// Offset  Size  Field
// 0       4     mode
// 8       24    gamebtn[3]
// 32      32    touch_cells[32]
// 64      4     gamepad_slide
// 68      4     source_id
// 72      8     timestamp_us
// 80      8     lease_ms
struct InputSnapshot
{
    // One of Mode. The hook rejects values above GamepadDualStick.
    uint32_t mode;

    // Current held action state for raw action IDs 0..191.
    uint64_t gamebtn[kGameButtonWordCount];

    // Arcade-slider contact state in left-to-right native sensor order.
    // Each item is 0 for inactive or nonzero for active. Multiple cells may be
    // active at once; the game performs its own slider contact processing.
    uint8_t touch_cells[kTouchCellCount];

    // OR-ed GamepadSlide bits. Ignored in ArcadeSlider mode.
    uint32_t gamepad_slide;

    // Producer-defined source identity. Changing it causes the hook to release
    // every held action before it accepts the new source.
    uint32_t source_id;

    // Monotonic producer timestamp in milliseconds * 1000. The hook rejects a
    // future timestamp and expires the source when lease_ms elapses.
    uint64_t timestamp_us;

    // Validity window in milliseconds. Must be nonzero and no greater than the
    // hook's configured io_input_lease_ms limit.
    uint64_t lease_ms;
};

// One ordered gamebtn transition. A producer appends exactly one item whenever
// any gamebtn bit changes; it may include several changed bits in that item.
//
// The hook consumes at most one matching event per game input frame. Therefore
// a producer may publish press then release between game frames without losing
// the tap: the game receives a pressed/down frame then a released frame.
//
// Offset  Size  Field
// 0       8     sequence
// 8       4     source_id
// 12      4     mode
// 16      24    gamebtn_pressed[3]
// 40      24    gamebtn_released[3]
// 64      8     producer_started_us
// 72      8     timestamp_us
struct alignas(8) ButtonEvent
{
    // Written last with the absolute 1-based event sequence after every other
    // field is complete. Consumers accept a slot only when it equals expected.
    volatile int64_t sequence;

    // Must match the current InputSnapshot source_id and mode.
    uint32_t source_id;
    uint32_t mode;

    // Bits that became down and bits that became up in this ordered transition.
    uint64_t gamebtn_pressed[kGameButtonWordCount];
    uint64_t gamebtn_released[kGameButtonWordCount];

    // Must match SharedBuffer::producer.started_us. This rejects events from a
    // previous producer process that occupied the same ring slot.
    uint64_t producer_started_us;

    // Informational producer time for diagnostics.
    uint64_t timestamp_us;
};

// Liveness and consumption state for producer or hook.
//
// Offset  Size  Field
// 0       4     process_id
// 4       2     protocol_major
// 6       2     protocol_minor
// 8       8     started_us
// 16      8     heartbeat_us
// 24      8     input_sequence
// 32      8     event_sequence
struct Endpoint
{
    // OS process ID of the owner; 0 means not initialized.
    uint32_t process_id;

    // ABI version understood by this endpoint.
    uint16_t protocol_major;
    uint16_t protocol_minor;

    // Monotonic start time. A changed value identifies a restarted producer.
    uint64_t started_us;

    // Owner refreshes this on every publish/read for external diagnostics.
    uint64_t heartbeat_us;

    // Producer endpoint: unused by the current hook. Hook endpoint: most
    // recently consumed even input_sequence.
    volatile int64_t input_sequence;

    // Producer endpoint: unused by the current hook. Hook endpoint: most
    // recently consumed absolute ButtonEvent sequence.
    volatile int64_t event_sequence;
};

// Mapping layout. Offsets are part of ABI v1.0:
//   0 magic, 4 major, 6 minor, 8 struct_size, 12 capabilities,
//   16 input_sequence, 24 input, 112 button_event_sequence,
//   120 button_events[128], 10360 producer, 10400 hook.
struct alignas(8) SharedBuffer
{
    // Must equal kMagic.
    uint32_t magic;
    uint16_t major;
    uint16_t minor;

    // Must equal sizeof(SharedBuffer), currently kSharedBufferSize.
    uint32_t struct_size;

    // Hook feature flags from Capability.
    uint32_t capabilities;

    // Snapshot seqlock. Producer writes odd, copies InputSnapshot and any new
    // ButtonEvent, then writes the next even value. Hook accepts only a stable,
    // identical even value before and after copying InputSnapshot.
    volatile int64_t input_sequence;

    InputSnapshot input;

    // Absolute count of completed ButtonEvent records. The producer increments it
    // only after it has written the selected ring slot and its slot sequence.
    volatile int64_t button_event_sequence;

    // Slot for absolute sequence S is button_events[S % kButtonEventCapacity].
    ButtonEvent button_events[kButtonEventCapacity];

    // Written by the external producer.
    Endpoint producer;

    // Written by AnySlider's in-game consumer.
    Endpoint hook;
};

static_assert(sizeof(InputSnapshot) == 88);
static_assert(sizeof(ButtonEvent) == 80);
static_assert(sizeof(Endpoint) == 40);
static_assert(sizeof(SharedBuffer) == kSharedBufferSize);
static_assert(offsetof(InputSnapshot, gamebtn) == 8);
static_assert(offsetof(InputSnapshot, touch_cells) == 32);
static_assert(offsetof(ButtonEvent, gamebtn_pressed) == 16);
static_assert(offsetof(SharedBuffer, input_sequence) == 16);
static_assert(offsetof(SharedBuffer, input) == 24);
static_assert(offsetof(SharedBuffer, button_event_sequence) == 112);
static_assert(offsetof(SharedBuffer, button_events) == 120);
static_assert(offsetof(SharedBuffer, producer) == 10360);
static_assert(offsetof(SharedBuffer, hook) == 10400);
static_assert(alignof(SharedBuffer) >= alignof(int64_t));
}
