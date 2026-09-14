#include "pch.h"

#include "mm_game_state.h"

#include "Dependencies/Signature.h"
#include "anyslider_log.h"

#include <mutex>
#include <type_traits>

namespace anyslider
{
namespace
{
// MegaMix+ v1.03 verified executable layout.
constexpr DWORD NativeModuleImageSize = 0x23E73000;
constexpr uintptr_t NativeStateTupleRva = 0xCC61094;
constexpr uintptr_t NativePvSelectGlobalRva = 0xCC5EF18;
constexpr uintptr_t NativeSongSelectStateOffset = 0x6C;
// Poll every second.
constexpr uint64_t GameStatePollIntervalMs = 1000;

// Game internal state values, not the same as mmio::GameState.
constexpr int32_t NativeLoading = 1;
constexpr int32_t NativeIntro = 2;
constexpr int32_t NativeTitle = 3;
constexpr int32_t NativeRhythmGame = 5;
constexpr int32_t NativeCustomPlaylist = 6;
constexpr int32_t NativeInGame = 7;
constexpr int32_t NativeCustomization = 36;
constexpr int32_t NativeGallery = 38;
constexpr int32_t NativeMainMenuTransition = 41;
constexpr int32_t NativeMainMenu = 42;
constexpr int32_t NativeOptions = 44;

constexpr int32_t NativeSongSelectWheel = 5;
constexpr int32_t NativeSongSelectInput = 6;
constexpr int32_t NativeSongSelectDetail = 7;
constexpr int32_t NativeSongSelectConfirm = 8;

struct NativeStateTuple
{
    int32_t current;
    int32_t next;
    int32_t previous;
    int32_t start_change;

    bool operator==(const NativeStateTuple&) const = default;
};

static_assert(sizeof(NativeStateTuple) == sizeof(int32_t) * 4);

struct GameStateReading
{
    int32_t current = -1;
    int32_t next = -1;
    int32_t start_change = -1;
    int32_t song_select_state = -1;

    bool operator==(const GameStateReading&) const = default;
};

template <typename T>
bool TryReadNativeMemory(uintptr_t address, T& result) noexcept
{
    static_assert(std::is_trivially_copyable_v<T>);

    T value{};
    SIZE_T bytesRead = 0;
    if (address == 0 ||
        ReadProcessMemory(
            GetCurrentProcess(),
            reinterpret_cast<const void*>(address),
            &value,
            sizeof(value),
            &bytesRead) == FALSE ||
        bytesRead != sizeof(value))
    {
        return false;
    }

    result = value;
    return true;
}

bool TryResolveModuleAddress(
    const MODULEINFO& module,
    uintptr_t rva,
    size_t size,
    uintptr_t& result)
{
    const uintptr_t moduleBase = reinterpret_cast<uintptr_t>(module.lpBaseOfDll);
    if (moduleBase == 0 ||
        rva > module.SizeOfImage ||
        size > module.SizeOfImage - rva ||
        moduleBase > UINTPTR_MAX - rva)
    {
        return false;
    }

    result = moduleBase + rva;
    return true;
}

bool TryReadStableStateTuple(uintptr_t address, NativeStateTuple& result)
{
    // 连续读取两次，避免状态切换时拿到混合数据。
    NativeStateTuple first{};
    NativeStateTuple second{};
    if (!TryReadNativeMemory(address, first) ||
        !TryReadNativeMemory(address, second) ||
        first != second)
    {
        return false;
    }

    result = second;
    return true;
}

bool TryReadSongSelectState(uintptr_t pvSelectGlobalAddress, int32_t& result)
{
    // PVsel 是动态对象，每次都从全局指针重新获取。
    uintptr_t pvSelectObject = 0;
    if (!TryReadNativeMemory(pvSelectGlobalAddress, pvSelectObject) ||
        pvSelectObject == 0 ||
        pvSelectObject > UINTPTR_MAX - NativeSongSelectStateOffset)
    {
        return false;
    }

    return TryReadNativeMemory(
        pvSelectObject + NativeSongSelectStateOffset,
        result);
}

bool TryCaptureGameState(
    uintptr_t stateTupleAddress,
    uintptr_t pvSelectGlobalAddress,
    GameStateReading& reading)
{
    NativeStateTuple tuple{};
    if (!TryReadStableStateTuple(stateTupleAddress, tuple))
        return false;

    reading.current = tuple.current;
    reading.next = tuple.next;
    reading.start_change = tuple.start_change;

    if (reading.current == NativeRhythmGame &&
        reading.next == NativeCustomPlaylist)
    {
        TryReadSongSelectState(
            pvSelectGlobalAddress,
            reading.song_select_state);
    }

    return true;
}

bool IsSongSelectVisible(int32_t state)
{
    return state == NativeSongSelectWheel ||
        state == NativeSongSelectInput ||
        state == NativeSongSelectDetail ||
        state == NativeSongSelectConfirm;
}

mmio::GameState ClassifyGameState(const GameStateReading& reading)
{
    switch (reading.current)
    {
    case NativeLoading:
        return mmio::GameState::Loading;

    case NativeIntro:
    case NativeMainMenuTransition:
        return mmio::GameState::Transition;

    case NativeTitle:
        return mmio::GameState::Title;

    case NativeRhythmGame:
        if (reading.next == NativeInGame && reading.start_change == 2)
            return mmio::GameState::Transition;

        if (reading.next == NativeCustomPlaylist &&
            !IsSongSelectVisible(reading.song_select_state))
        {
            return mmio::GameState::Transition;
        }

        return mmio::GameState::SongSelect;

    case NativeCustomPlaylist:
        return mmio::GameState::SongSelect;

    case NativeInGame:
        if (reading.next == NativeRhythmGame && reading.start_change == 2)
            return mmio::GameState::Transition;

        // 结算也保持 InGame，目前还找到只读能获取的地址。
        return mmio::GameState::InGame;

    case NativeCustomization:
        return mmio::GameState::Customization;

    case NativeGallery:
        return mmio::GameState::Gallery;

    case NativeMainMenu:
        return mmio::GameState::MainMenu;

    case NativeOptions:
        return mmio::GameState::Options;

    default:
        return mmio::GameState::Unknown;
    }
}

class MmGameStateReader
{
public:
    bool Initialize(MmIoConsumer& consumer)
    {
        std::scoped_lock lock(mutex_);
        Reset();

        const MODULEINFO& module = getModuleInfo();
        if (module.SizeOfImage != NativeModuleImageSize)
        {
            Log(
                "Native game-state reader only supports MegaMix+ v1.03 "
                "(image size expected=0x%lX actual=0x%lX).",
                NativeModuleImageSize,
                module.SizeOfImage);
            return false;
        }

        if (!TryResolveModuleAddress(
                module,
                NativeStateTupleRva,
                sizeof(NativeStateTuple),
                stateTupleAddress_) ||
            !TryResolveModuleAddress(
                module,
                NativePvSelectGlobalRva,
                sizeof(uintptr_t),
                pvSelectGlobalAddress_))
        {
            Log("Native game-state addresses are unavailable; publishing Unknown.");
            Reset();
            return false;
        }

        consumer_ = &consumer;
        return true;
    }

    void Shutdown()
    {
        std::scoped_lock lock(mutex_);
        Reset();
    }

    void Update()
    {
        std::scoped_lock lock(mutex_);
        if (!consumer_ || !consumer_->IsOpen())
            return;

        const uint64_t nowMs = GetTickCount64();
        if (nextPollMs_ != 0 && nowMs < nextPollMs_)
            return;

        GameStateReading reading{};
        if (!TryCaptureGameState(
                stateTupleAddress_,
                pvSelectGlobalAddress_,
                reading))
        {
            return;
        }
        nextPollMs_ = nowMs + GameStatePollIntervalMs;

        const mmio::GameState state = ClassifyGameState(reading);
        // 非 Debug 时不比较原始字段。
        const bool debugEnabled = IsDebugLoggingEnabled();
        const bool rawChanged = debugEnabled &&
            (!hasLastDebugReading_ || lastDebugReading_ != reading);

        if (debugEnabled)
        {
            lastDebugReading_ = reading;
            hasLastDebugReading_ = true;
        }

        if (state == publishedState_)
        {
            if (rawChanged)
                LogRawChange(reading, state);
            return;
        }

        const mmio::GameState previousState = publishedState_;
        publishedState_ = state;
        consumer_->PublishGameState(state);

        if (debugEnabled)
            LogPublicChange(reading, previousState, state);
    }

private:
    void Reset()
    {
        consumer_ = nullptr;
        stateTupleAddress_ = 0;
        pvSelectGlobalAddress_ = 0;
        nextPollMs_ = 0;
        publishedState_ = mmio::GameState::Unknown;
        lastDebugReading_ = {};
        hasLastDebugReading_ = false;
    }

    static void LogRawChange(
        const GameStateReading& reading,
        mmio::GameState state)
    {
        DebugLog(
            "Game state raw changed: raw=(%d,%d,%d) song_select=%d public=%u",
            reading.current,
            reading.next,
            reading.start_change,
            reading.song_select_state,
            static_cast<uint32_t>(state));
    }

    static void LogPublicChange(
        const GameStateReading& reading,
        mmio::GameState previousState,
        mmio::GameState state)
    {
        DebugLog(
            "Game state changed: raw=(%d,%d,%d) song_select=%d public=%u -> %u",
            reading.current,
            reading.next,
            reading.start_change,
            reading.song_select_state,
            static_cast<uint32_t>(previousState),
            static_cast<uint32_t>(state));
    }

    std::mutex mutex_;
    MmIoConsumer* consumer_ = nullptr;
    uintptr_t stateTupleAddress_ = 0;
    uintptr_t pvSelectGlobalAddress_ = 0;
    uint64_t nextPollMs_ = 0;
    mmio::GameState publishedState_ = mmio::GameState::Unknown;
    GameStateReading lastDebugReading_{};
    bool hasLastDebugReading_ = false;
};

MmGameStateReader gameStateReader;
}

bool InitializeMmGameState(MmIoConsumer& consumer)
{
    return gameStateReader.Initialize(consumer);
}

void ShutdownMmGameState()
{
    gameStateReader.Shutdown();
}

void UpdateMmGameState()
{
    gameStateReader.Update();
}
}
