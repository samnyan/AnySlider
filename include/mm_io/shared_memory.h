#pragma once

#include <cstddef>
#include <cstdint>

// AnySlider MMIO shared-memory ABI.
//
// External programs publish complete input snapshots into a ring buffer.
// AnySlider reads all snapshots since the previous game frame and derives
// button press/release edges by comparing consecutive snapshots.
//
// Producer:
//   1. Fill InputSnapshot.
//   2. Write it to the next InputSlot.
//   3. Commit the slot by setting InputSlot::sequence.
//   4. Publish it by updating SharedBuffer::input_sequence.
//
// sequence is 1, 2, 3...

namespace anyslider::mmio
{
    inline constexpr wchar_t kSharedMemoryName[] = L"Local\\MMIO_SHARED_BUFFER";
    inline constexpr wchar_t kLedNotifyEventName[] = L"Local\\MMIO_LED_OUTPUT_NOTIFY_EVENT";

    inline constexpr uint32_t kMagic = 0x4F494D4D; // "MMIO"
    inline constexpr uint16_t kAbiMajor = 3;
    inline constexpr uint16_t kAbiMinor = 1;

    inline constexpr uint32_t kTouchCellCount = 32;
    inline constexpr uint32_t kGameButtonWordCount = 3;
    inline constexpr uint32_t kGameButtonCount = kGameButtonWordCount * 64;

    inline constexpr uint32_t kInputCapacity = 128;
    inline constexpr uint32_t kOutputCapacity = 4;

    enum class Mode : uint32_t
    {
        None = 0,
        ArcadeSlider = 1,
        GamepadDualStick = 2,
    };


    // Game-state values.
    enum class GameState : uint32_t
    {
        Unknown = 0,
        Loading = 1,
        MainMenu = 2,
        SongSelect = 3,
        Results = 4, // Current version not support..
        InGame = 5,
        Title = 6,
        Options = 7,
        Customization = 8,
        Gallery = 9,
        Transition = 10,
    };


    enum GameButton : uint32_t
    {
        Test = 0,       // Arcade TEST button.
        Service = 1,    // Arcade SERVICE button.
        Start = 2,      // Arcade/game START action.

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
        L3 = 15,
        R3 = 16,

        Select = 17,

        Sw1 = 18, // ArcadeController MenuLeftAction
        Sw2 = 19, // ArcadeController MenuRightAction

        Stick1Up = 24,
        Stick1Down = 25,
        Stick1Left = 26,
        Stick1Right = 27,

        Stick2Up = 28,
        Stick2Down = 29,
        Stick2Left = 30,
        Stick2Right = 31,

        Pause = 160,    // PS Options / ProController + / keyboard Escape pause action.
    };


    constexpr bool IsGameButtonValid(uint32_t action)
    {
        return action < kGameButtonCount;
    }


    constexpr bool IsGameButtonDown(
        const uint64_t(&gamebtn)[kGameButtonWordCount],
        uint32_t action)
    {
        return IsGameButtonValid(action) &&
            (gamebtn[action / 64] & (1ull << (action % 64))) != 0;
    }


    constexpr void SetGameButton(
        uint64_t(&gamebtn)[kGameButtonWordCount],
        uint32_t action,
        bool down = true)
    {
        if (!IsGameButtonValid(action))
            return;

        const uint64_t bit = 1ull << (action % 64);

        if (down)
            gamebtn[action / 64] |= bit;
        else
            gamebtn[action / 64] &= ~bit;
    }


    enum Capability : uint32_t
    {
        SupportsArcadeSlider = 1u << 0,
        SupportsGamepadDualStick = 1u << 1,
        SupportsKeyboardFrontend = 1u << 2,
    };


    // Complete controller state at one point in time.
    struct InputSnapshot
    {
        // Raw MM+ action IDs 0..191.
        uint64_t gamebtn[kGameButtonWordCount];

        // 32 arcade slider cells, left to right 0 to 31.
        uint32_t touch_mask;

        uint32_t mode;

        uint64_t timestamp_ms;
    };


    // One committed snapshot in the ring.
    //
    // sequence == 0: slot is being written / unavailable.
    // sequence == N: slot contains snapshot N.
    struct alignas(8) InputSlot
    {
        volatile int64_t sequence;
        InputSnapshot input;
    };


    // Endpoint status, heartbeat, and input contract.
    struct alignas(8) Endpoint
    {
        uint32_t process_id;

        uint16_t protocol_major;
        uint16_t protocol_minor;

        uint64_t started_ms;
        uint64_t heartbeat_ms;

        // Maximum accepted age of an input snapshot. Should publish
        // even no buttons were changed before the lease expires.
        uint64_t lease_ms;

        // Current game state. Unknown when not supported
        volatile uint32_t game_state;
    };


    enum Io3Led : uint32_t
    {
        Io3Led_Triangle = 1u << 3,
        Io3Led_Square = 1u << 4,
        Io3Led_Cross = 1u << 6,
        Io3Led_Circle = 1u << 7,
        Io3Led_LeftRed = 1u << 8,
        Io3Led_LeftGreen = 1u << 9,
        Io3Led_LeftBlue = 1u << 10,
        Io3Led_RightRed = 1u << 11,
        Io3Led_RightGreen = 1u << 12,
        Io3Led_RightBlue = 1u << 13,
    };

    struct alignas(8) LedOutput
    {
        uint64_t timestamp_ms; // GetTickCount64, complete snapshot publication time.
        uint32_t io3_bitmap;   // Native cabinet logical outputs; not JVS wire bytes.
        uint8_t slider[97];    // Command 02 payload: 0x3F + 32 BRG triplets (0..254).
        uint8_t reserved[3];   // Must be zero. No channel conversion or reversal.
    };

    struct alignas(8) SharedBuffer
    {
        uint32_t magic;
        uint16_t major;
        uint16_t minor;

        uint32_t struct_size;
        uint32_t capabilities;

        // Latest completely published snapshot.
        // Sequence 0: no valid input.
        // Sequence starts at 1 and increases normally.
        volatile int64_t input_sequence;

        InputSlot inputs[kInputCapacity];

        Endpoint producer;
        Endpoint hook;

        // Latest completely published Led data.
        // Sequence 0: no valid output.
        // Sequence starts at 1 and increases normally.
        volatile int64_t output_sequence;

        LedOutput output[kOutputCapacity];
    };


    constexpr uint32_t InputSlotIndex(int64_t sequence)
    {
        return static_cast<uint32_t>(
            static_cast<uint64_t>(sequence) % kInputCapacity);
    }
    constexpr uint32_t OutputSlotIndex(int64_t sequence)
    {
        return static_cast<uint32_t>(
            static_cast<uint64_t>(sequence) % kOutputCapacity);
    }


    // ABI layout checks.
    static_assert(sizeof(InputSnapshot) == 40);
    static_assert(sizeof(InputSlot) == 48);
    static_assert(sizeof(Endpoint) == 40);

    static_assert(offsetof(InputSnapshot, gamebtn) == 0);
    static_assert(offsetof(InputSnapshot, touch_mask) == 24);
    static_assert(offsetof(InputSnapshot, mode) == 28);
    static_assert(offsetof(InputSnapshot, timestamp_ms) == 32);
    static_assert(offsetof(InputSlot, input) == 8);
    static_assert(offsetof(SharedBuffer, input_sequence) == 16);
    static_assert(offsetof(SharedBuffer, inputs) == 24);
    static_assert(offsetof(Endpoint, game_state) == 32);
    static_assert(sizeof(LedOutput) == 112);
    static_assert(alignof(LedOutput) == 8);
    static_assert(offsetof(LedOutput, io3_bitmap) == 8);
    static_assert(offsetof(LedOutput, slider) == 12);
    static_assert(offsetof(LedOutput, reserved) == 109);
    static_assert(offsetof(SharedBuffer, output_sequence) == 6248);
    static_assert(offsetof(SharedBuffer, output) == 6256);
    static_assert(sizeof(SharedBuffer) == 6704);

}
