#pragma once

#include "protocol.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

constexpr uint64_t SDL_DIGITAL_RELEASE_GRACE_US = 35000ULL;

struct DigitalReleaseFilter {
    uint16_t last_buttons = 0;
    uint64_t button_until[16]{};
    uint8_t last_hat = ns::HAT_NEUTRAL;
    uint64_t hat_until = 0;

    void reset();
    void apply(ns::HoriHIDReport& r, uint64_t now);
};

struct SdlPadState {
    bool connected = false;
    ns::HoriHIDReport input{};
    ns::MotionReport motion{};
    ns::MotionReport motion_samples[3]{};
    bool has_motion = false;
    uint64_t last_input_us = 0;
    std::string name;
    uint16_t vid = 0;
    uint16_t pid = 0;
    SDL_JoystickID instance_id = 0;
};

uint8_t sdl_axis_to_byte(Sint16 val, bool invert = false, int deadzone = 8000);
int16_t clamp_motion_i16(float v);
int16_t gyro_deadzone_i16(int16_t v);

class SDLInputManager {
public:
    bool start();
    void stop();
    void poll();
    std::array<SdlPadState, 4> snapshot();
    std::string error() const;
    void request_rescan();
    void set_connection_callback(std::function<void(int slot, bool connected)> cb);
    void set_gyro_enabled(bool enabled);
    void set_motion_enabled(bool enabled);
    void set_home_shortcut_enabled(bool enabled);
    void set_capture_shortcut_enabled(bool enabled);
    void set_rumble(int sdl_slot, uint8_t low, uint8_t high, uint32_t duration_ms, bool allow_trigger_rumble = true);
    void stop_all_rumble();

private:
    struct Device {
        SDL_Gamepad* pad = nullptr;
        SDL_JoystickID id = 0;
        int slot = -1;
        bool gyro_enabled = false;
        bool accel_enabled = false;
        bool rumble_capable = false;
        bool trigger_rumble_capable = false;
        std::string name;
        uint16_t vid = 0;
        uint16_t pid = 0;
        ns::MotionReport motion_samples[3]{};
        bool has_motion_samples = false;
    };

    mutable std::mutex mtx;
    bool initialized = false;
    bool force_scan = false;
    uint64_t last_scan_us = 0;
    std::string last_error;
    std::array<SdlPadState, 4> states{};
    std::vector<Device> devices;
    std::function<void(int slot, bool connected)> connection_callback;
    std::atomic<bool> motion_enabled{true};
    std::atomic<bool> home_shortcut_enabled{true};
    std::atomic<bool> capture_shortcut_enabled{true};

    static Uint16 motor_word(uint8_t v);
    static bool button(SDL_Gamepad* pad, SDL_GamepadButton b);
    static bool report_non_neutral(const ns::HoriHIDReport& r);
    static void push_motion_sample(Device& d, const ns::MotionReport& sample);

    ns::HoriHIDReport map_gamepad(const Device& d) const;
    void apply_motion(Device& d, ns::MotionReport out_samples[3], bool& has_motion);
    Device* device_for_slot_locked(int slot);
    int first_free_slot_locked() const;
    bool has_device_locked(SDL_JoystickID id) const;
    void close_device_locked(Device& d);
    void close_all_locked();
    void clear_states_locked();
    void stop_all_rumble_locked();
    void scan_locked(bool initial);
    void refresh_states_locked(uint64_t now);
};
