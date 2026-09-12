#pragma once

#include "mm_io_config.h"

#include "Dependencies/JoyShockLibrary/JoyShockLibrary/JoyShockLibrary.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace anyslider
{
enum class MmIoControllerType : uint8_t
{
    DualSense,
    DualShock4,
    Nintendo,
};

struct MmIoJoyShockFrame
{
    uint64_t gamebtn_tapped[mmio::kGameButtonWordCount]{};
    uint64_t gamebtn_released[mmio::kGameButtonWordCount]{};
    uint64_t gamebtn_down[mmio::kGameButtonWordCount]{};
    uint8_t touch_cells[mmio::kTouchCellCount]{};
    uint8_t arcade_touch_cells[mmio::kTouchCellCount]{};
    uint32_t gamepad_slide = 0;
    MmIoControllerType controller_type = MmIoControllerType::DualSense;
    bool connected = false;
    bool touchpad_active = false;
};

struct MmIoJoyShockMotionState
{
    float accel_x = 0.0f;
    float accel_y = 0.0f;
    float accel_z = 0.0f;
    float gyro_x = 0.0f;
    float gyro_y = 0.0f;
    float gyro_z = 0.0f;
};

class MmIoJoyShockFrontend
{
public:
    bool Initialize(
        const MmIoGamepadConfig& config,
        MmIoSliderMode sliderMode,
        float arcadeSliderCellsPerSecond);
    void Shutdown();

    [[nodiscard]] bool IsEnabled() const;
    [[nodiscard]] bool HasSelectedController() const;
    [[nodiscard]] MmIoJoyShockFrame Consume();
    [[nodiscard]] MmIoJoyShockMotionState LatestMotion() const;

private:
    static void InputCallback(int, JOY_SHOCK_STATE, JOY_SHOCK_STATE, IMU_STATE, IMU_STATE, float);
    static void TouchCallback(int, TOUCH_STATE, TOUCH_STATE, float);
    static void ConnectCallback(int);
    static void DisconnectCallback(int, bool);

    void OnInput(int, JOY_SHOCK_STATE, JOY_SHOCK_STATE, IMU_STATE);
    void OnTouch(int, TOUCH_STATE);
    void OnConnect(int);
    void OnDisconnect(int);
    void DiscoveryLoop(std::stop_token stopToken);
    void DiscoverControllers();
    void RequestDiscovery();
    void SelectController(int deviceId, int controllerType);
    void ClearControllerState(bool preserveRelease);

    static std::atomic<MmIoJoyShockFrontend*> activeInstance_;

    bool enabled_ = false;
    MmIoGamepadConfig config_{};
    MmIoSliderMode slider_mode_ = MmIoSliderMode::Arcade;
    std::jthread discoveryThread_;
    std::mutex discoveryMutex_;
    std::condition_variable_any discoveryCondition_;
    bool discoveryRequested_ = false;
    std::atomic_bool shuttingDown_ = false;
    std::atomic_int selectedHandle_ = -1;
    std::atomic_int selectedType_ = 0;
    std::atomic_bool connected_ = false;
    std::atomic<uint64_t> currentDown_[mmio::kGameButtonWordCount]{};
    std::atomic<uint64_t> pendingTapped_[mmio::kGameButtonWordCount]{};
    std::atomic<uint64_t> pendingReleased_[mmio::kGameButtonWordCount]{};
    std::atomic<uint32_t> currentTouchCells_ = 0;
    std::atomic<uint32_t> pendingTouchCells_ = 0;
    std::atomic<uint32_t> currentGamepadSlide_ = 0;
    std::atomic_bool touchpadActive_ = false;
    std::atomic<uint32_t> accelXBits_ = 0;
    std::atomic<uint32_t> accelYBits_ = 0;
    std::atomic<uint32_t> accelZBits_ = 0;
    std::atomic<uint32_t> gyroXBits_ = 0;
    std::atomic<uint32_t> gyroYBits_ = 0;
    std::atomic<uint32_t> gyroZBits_ = 0;
    float arcade_slider_cells_per_second_ = 32.0f;
    MmIoSliderContact left_contact_{};
    MmIoSliderContact right_contact_{};
    std::chrono::steady_clock::time_point last_consume_time_{};
    bool has_last_consume_time_ = false;
    std::mutex slider_mutex_;
    int debug_left_arcade_cell_ = -1;
    int debug_right_arcade_cell_ = -1;
};
}
