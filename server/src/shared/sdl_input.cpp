#include "shared/sdl_input.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

void DigitalReleaseFilter::reset() {
    last_buttons = 0;
    std::fill(std::begin(button_until), std::end(button_until), 0);
    last_hat = ns::HAT_NEUTRAL;
    hat_until = 0;
}

void DigitalReleaseFilter::apply(ns::HIDReport& r, uint64_t now) {
    for (int i = 0; i < 16; ++i) {
        uint16_t bit = (uint16_t)(1u << i);
        if (r.buttons & bit) {
            last_buttons |= bit;
            button_until[i] = now + SDL_DIGITAL_RELEASE_GRACE_US;
        } else if ((last_buttons & bit) && button_until[i] != 0 && now <= button_until[i]) {
            r.buttons |= bit;
        } else {
            last_buttons &= (uint16_t)~bit;
            button_until[i] = 0;
        }
    }

    if (r.hat != ns::HAT_NEUTRAL) {
        last_hat = r.hat;
        hat_until = now + SDL_DIGITAL_RELEASE_GRACE_US;
    } else if (hat_until != 0 && now <= hat_until) {
        r.hat = last_hat;
    } else {
        last_hat = ns::HAT_NEUTRAL;
        hat_until = 0;
    }
}

uint8_t sdl_axis_to_byte(Sint16 val, bool invert, int deadzone) {
    if (val > -deadzone && val < deadzone) return 128;
    int scaled = 128;
    if (val >= deadzone) scaled = 128 + ((int)(val - deadzone) * 127) / (32767 - deadzone);
    else scaled = 128 - ((int)(-val - deadzone) * 128) / (32768 - deadzone);
    scaled = std::clamp(scaled, 0, 255);
    return (uint8_t)(invert ? (255 - scaled) : scaled);
}

int16_t clamp_motion_i16(float v) {
    if (v > 32767.0f) return 32767;
    if (v < -32768.0f) return -32768;
    return (int16_t)std::lround(v);
}

int16_t gyro_deadzone_i16(int16_t v) {
    constexpr int16_t GYRO_DEADZONE = 32;
    return std::abs((int)v) <= GYRO_DEADZONE ? 0 : v;
}

bool SDLInputManager::start() {
        std::lock_guard<std::mutex> lk(mtx);
        if (initialized) return true;
        SDL_SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
        SDL_SetHint("SDL_JOYSTICK_HIDAPI", "1");
        SDL_SetHint("SDL_JOYSTICK_HIDAPI_" "SW" "ITCH", "1");
        SDL_SetHint("SDL_JOYSTICK_HIDAPI_" "JOY" "_CONS", "1");
        SDL_SetHint("SDL_JOYSTICK_HIDAPI_PS4", "1");
        SDL_SetHint("SDL_JOYSTICK_HIDAPI_PS5", "1");
        SDL_SetHint("SDL_JOYSTICK_HIDAPI_XBOX", "1");
        SDL_SetHint("SDL_JOYSTICK_ENHANCED_REPORTS", "1");
        Uint32 flags = SDL_INIT_GAMEPAD | SDL_INIT_EVENTS;
#ifdef SDL_INIT_SENSOR
        flags |= SDL_INIT_SENSOR;
#endif
#ifdef SDL_INIT_HAPTIC
        flags |= SDL_INIT_HAPTIC;
#endif
        if (!SDL_Init(flags)) {
            const char* e = SDL_GetError();
            last_error = (e && *e) ? e : "SDL_Init failed";
            return false;
        }
        initialized = true;
        scan_locked(true);
        return true;
    }

void SDLInputManager::stop() {
        std::lock_guard<std::mutex> lk(mtx);
        if (!initialized) return;
        close_all_locked();
        clear_states_locked();
        Uint32 flags = SDL_INIT_GAMEPAD | SDL_INIT_EVENTS;
#ifdef SDL_INIT_SENSOR
        flags |= SDL_INIT_SENSOR;
#endif
#ifdef SDL_INIT_HAPTIC
        flags |= SDL_INIT_HAPTIC;
#endif
        SDL_QuitSubSystem(flags);
        initialized = false;
    }

void SDLInputManager::poll() {
        std::lock_guard<std::mutex> lk(mtx);
        if (!initialized) return;
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_GAMEPAD_ADDED || ev.type == SDL_EVENT_GAMEPAD_REMOVED) force_scan = true;
        }
        SDL_UpdateGamepads();
        SDL_UpdateSensors();
        uint64_t now = ns::now_us();
        if (force_scan || last_scan_us == 0 || now - last_scan_us > 500000ULL) scan_locked(false);
        refresh_states_locked(now);
    }

std::array<SdlPadState, 4> SDLInputManager::snapshot() {
        std::lock_guard<std::mutex> lk(mtx);
        return states;
    }

std::string SDLInputManager::error() const {
        std::lock_guard<std::mutex> lk(mtx);
        return last_error;
    }

void SDLInputManager::request_rescan() {
        std::lock_guard<std::mutex> lk(mtx);
        force_scan = true;
    }

void SDLInputManager::set_gyro_enabled(bool enabled) { set_motion_enabled(enabled); }

void SDLInputManager::set_motion_enabled(bool enabled) {
        std::lock_guard<std::mutex> lk(mtx);
        motion_enabled.store(enabled, std::memory_order_relaxed);
        if (!initialized) return;
        for (auto& d : devices) {
            if (!d.pad || !SDL_GamepadConnected(d.pad)) continue;
            if (SDL_GamepadHasSensor(d.pad, SDL_SENSOR_ACCEL)) {
                d.accel_enabled = SDL_SetGamepadSensorEnabled(d.pad, SDL_SENSOR_ACCEL, enabled);
            } else {
                d.accel_enabled = false;
            }
            if (SDL_GamepadHasSensor(d.pad, SDL_SENSOR_GYRO)) {
                d.gyro_enabled = SDL_SetGamepadSensorEnabled(d.pad, SDL_SENSOR_GYRO, enabled);
            } else {
                d.gyro_enabled = false;
            }
            if (!enabled) {
                d.has_motion_samples = false;
                for (int i = 0; i < 3; ++i) d.motion_samples[i].reset();
            }
        }
        if (!enabled) {
            for (auto& st : states) {
                st.motion.reset();
                for (int i = 0; i < 3; ++i) st.motion_samples[i].reset();
                st.has_motion = false;
            }
        }
    }

void SDLInputManager::set_home_shortcut_enabled(bool enabled) {
        home_shortcut_enabled.store(enabled, std::memory_order_relaxed);
    }

void SDLInputManager::set_capture_shortcut_enabled(bool enabled) {
        capture_shortcut_enabled.store(enabled, std::memory_order_relaxed);
    }

void SDLInputManager::set_rumble(int sdl_slot, uint8_t low, uint8_t high, uint32_t duration_ms) {
        std::lock_guard<std::mutex> lk(mtx);
        if (!initialized || sdl_slot < 0 || sdl_slot >= 4) return;
        Device* d = device_for_slot_locked(sdl_slot);
        if (!d || !d->pad || !SDL_GamepadConnected(d->pad)) return;
        const Uint16 low_word = motor_word(low);
        const Uint16 high_word = motor_word(high);
        const bool stop = (low_word == 0 && high_word == 0) || duration_ms == 0;
        bool ok_main = SDL_RumbleGamepad(d->pad, stop ? 0 : low_word, stop ? 0 : high_word, duration_ms);
        bool ok_trigger = true;
        SDL_PropertiesID props = SDL_GetGamepadProperties(d->pad);
        bool trigger_capable = props && SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN, false);
        if (trigger_capable || !ok_main || stop) {
            ok_trigger = SDL_RumbleGamepadTriggers(d->pad, stop ? 0 : low_word, stop ? 0 : high_word, duration_ms);
        }
        if (!stop && !ok_main && !ok_trigger) {
            const char* e = SDL_GetError();
            last_error = (e && *e) ? e : "SDL rumble failed";
        }
    }

void SDLInputManager::stop_all_rumble() {
        std::lock_guard<std::mutex> lk(mtx);
        stop_all_rumble_locked();
    }


Uint16 SDLInputManager::motor_word(uint8_t v) { return (Uint16)((uint32_t)v * 65535u / 255u); }
bool SDLInputManager::button(SDL_Gamepad* pad, SDL_GamepadButton b) { return SDL_GetGamepadButton(pad, b); }

ns::HIDReport SDLInputManager::map_gamepad(const Device& d) const {
        ns::HIDReport r;
        r.reset();
        SDL_Gamepad* pad = d.pad;
        if (!pad) return r;
        if (button(pad, SDL_GAMEPAD_BUTTON_SOUTH)) r.buttons |= ns::BTN_B;
        if (button(pad, SDL_GAMEPAD_BUTTON_EAST)) r.buttons |= ns::BTN_A;
        if (button(pad, SDL_GAMEPAD_BUTTON_WEST)) r.buttons |= ns::BTN_Y;
        if (button(pad, SDL_GAMEPAD_BUTTON_NORTH)) r.buttons |= ns::BTN_X;
        if (button(pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) r.buttons |= ns::BTN_L;
        if (button(pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) r.buttons |= ns::BTN_R;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16384) r.buttons |= ns::BTN_ZL;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16384) r.buttons |= ns::BTN_ZR;
        if (button(pad, SDL_GAMEPAD_BUTTON_BACK)) r.buttons |= ns::BTN_MINUS;
        if (button(pad, SDL_GAMEPAD_BUTTON_START)) r.buttons |= ns::BTN_PLUS;
        if (button(pad, SDL_GAMEPAD_BUTTON_LEFT_STICK)) r.buttons |= ns::BTN_LSTICK;
        if (button(pad, SDL_GAMEPAD_BUTTON_RIGHT_STICK)) r.buttons |= ns::BTN_RSTICK;
        if (button(pad, SDL_GAMEPAD_BUTTON_GUIDE)) r.buttons |= ns::BTN_HOME;
        if (button(pad, SDL_GAMEPAD_BUTTON_MISC1)) r.buttons |= ns::BTN_CAPTURE;
        if (home_shortcut_enabled.load(std::memory_order_relaxed) &&
            button(pad, SDL_GAMEPAD_BUTTON_LEFT_STICK) && button(pad, SDL_GAMEPAD_BUTTON_RIGHT_STICK)) {
            r.buttons |= ns::BTN_HOME;
            r.buttons &= ~(ns::BTN_LSTICK | ns::BTN_RSTICK);
        }
        if (capture_shortcut_enabled.load(std::memory_order_relaxed) &&
            button(pad, SDL_GAMEPAD_BUTTON_BACK) && button(pad, SDL_GAMEPAD_BUTTON_START)) {
            r.buttons |= ns::BTN_CAPTURE;
            r.buttons &= ~(ns::BTN_MINUS | ns::BTN_PLUS);
        }
        bool up = button(pad, SDL_GAMEPAD_BUTTON_DPAD_UP);
        bool down = button(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
        bool left = button(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
        bool right = button(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
        if (up && right) r.hat = ns::HAT_NE;
        else if (up && left) r.hat = ns::HAT_NW;
        else if (down && right) r.hat = ns::HAT_SE;
        else if (down && left) r.hat = ns::HAT_SW;
        else if (up) r.hat = ns::HAT_N;
        else if (down) r.hat = ns::HAT_S;
        else if (left) r.hat = ns::HAT_W;
        else if (right) r.hat = ns::HAT_E;
        r.lx = sdl_axis_to_byte(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX));
        r.ly = sdl_axis_to_byte(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY));
        r.rx = sdl_axis_to_byte(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX));
        r.ry = sdl_axis_to_byte(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY));
        return r;
    }

bool SDLInputManager::report_non_neutral(const ns::HIDReport& r) {
        return r.buttons != 0 || r.hat != ns::HAT_NEUTRAL ||
               r.lx != 128 || r.ly != 128 || r.rx != 128 || r.ry != 128;
    }

void SDLInputManager::push_motion_sample(Device& d, const ns::MotionReport& sample) {
        if (!d.has_motion_samples) {
            d.motion_samples[0] = sample;
            d.motion_samples[1] = sample;
            d.motion_samples[2] = sample;
            d.has_motion_samples = true;
            return;
        }
        d.motion_samples[0] = d.motion_samples[1];
        d.motion_samples[1] = d.motion_samples[2];
        d.motion_samples[2] = sample;
    }

void SDLInputManager::apply_motion(Device& d, ns::MotionReport out_samples[3], bool& has_motion) {
        SDL_Gamepad* pad = d.pad;
        for (int i = 0; i < 3; ++i) out_samples[i].reset();
        has_motion = false;

        if (!motion_enabled.load(std::memory_order_relaxed)) {
            d.has_motion_samples = false;
            for (int i = 0; i < 3; ++i) d.motion_samples[i].reset();
            return;
        }

        ns::MotionReport sample{};
        constexpr float STANDARD_GRAVITY = 9.80665f;
        constexpr float ACCEL_SCALE = 4096.0f / STANDARD_GRAVITY;
        constexpr float RAD_TO_DEG = 57.29577951308232f;
        constexpr float GYRO_SCALE = RAD_TO_DEG * 16.384f;

        if (d.accel_enabled) {
            float accel[3] = {0, 0, 0};
            if (SDL_GetGamepadSensorData(pad, SDL_SENSOR_ACCEL, accel, 3)) {
                sample.ax = clamp_motion_i16(-accel[2] * ACCEL_SCALE);
                sample.ay = clamp_motion_i16(-accel[0] * ACCEL_SCALE);
                sample.az = clamp_motion_i16(accel[1] * ACCEL_SCALE);
                has_motion = true;
            } else {
                sample.az = 4096;
            }
        } else {
            sample.az = 4096;
        }

        if (d.gyro_enabled) {
            float gyro[3] = {0, 0, 0};
            if (SDL_GetGamepadSensorData(pad, SDL_SENSOR_GYRO, gyro, 3)) {
                const float gx = -gyro[2];
                const float gy = -gyro[0];
                const float gz =  gyro[1];
                sample.gx = gyro_deadzone_i16(clamp_motion_i16(gx * GYRO_SCALE));
                sample.gy = gyro_deadzone_i16(clamp_motion_i16(gy * GYRO_SCALE));
                sample.gz = gyro_deadzone_i16(clamp_motion_i16(gz * GYRO_SCALE));
                has_motion = true;
            }
        }

        if (has_motion) {
            push_motion_sample(d, sample);
            out_samples[0] = d.motion_samples[0];
            out_samples[1] = d.motion_samples[1];
            out_samples[2] = d.motion_samples[2];
        } else {
            d.has_motion_samples = false;
            for (int i = 0; i < 3; ++i) d.motion_samples[i].reset();
        }
    }

SDLInputManager::Device* SDLInputManager::device_for_slot_locked(int slot) {
        for (auto& d : devices) if (d.slot == slot) return &d;
        return nullptr;
    }

int SDLInputManager::first_free_slot_locked() const {
        bool used[4] = {false, false, false, false};
        for (const auto& d : devices) if (d.slot >= 0 && d.slot < 4) used[d.slot] = true;
        for (int i = 0; i < 4; ++i) if (!used[i]) return i;
        return -1;
    }

bool SDLInputManager::has_device_locked(SDL_JoystickID id) const {
        for (const auto& d : devices) if (d.id == id) return true;
        return false;
    }

void SDLInputManager::close_device_locked(Device& d) {
        if (d.pad) {
            SDL_RumbleGamepad(d.pad, 0, 0, 0);
            SDL_RumbleGamepadTriggers(d.pad, 0, 0, 0);
            SDL_CloseGamepad(d.pad);
            d.pad = nullptr;
        }
    }

void SDLInputManager::close_all_locked() {
        for (auto& d : devices) close_device_locked(d);
        devices.clear();
    }

void SDLInputManager::clear_states_locked() {
        for (auto& s : states) s = SdlPadState{};
    }

void SDLInputManager::stop_all_rumble_locked() {
        for (auto& d : devices) {
            if (d.pad) {
                SDL_RumbleGamepad(d.pad, 0, 0, 0);
                SDL_RumbleGamepadTriggers(d.pad, 0, 0, 0);
            }
        }
    }

void SDLInputManager::scan_locked(bool initial) {
        (void)initial;
        force_scan = false;
        last_scan_us = ns::now_us();
        for (auto it = devices.begin(); it != devices.end();) {
            if (!it->pad || !SDL_GamepadConnected(it->pad)) {
                if (it->slot >= 0 && it->slot < 4) states[it->slot] = SdlPadState{};
                close_device_locked(*it);
                it = devices.erase(it);
            } else {
                ++it;
            }
        }
        int count = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&count);
        if (!ids) return;
        for (int i = 0; i < count; ++i) {
            SDL_JoystickID id = ids[i];
            if (has_device_locked(id)) continue;
            int slot = first_free_slot_locked();
            if (slot < 0) break;
            SDL_Gamepad* pad = SDL_OpenGamepad(id);
            if (!pad) continue;
            Device d{};
            d.pad = pad;
            d.id = SDL_GetGamepadID(pad);
            d.slot = slot;
            const char* name = SDL_GetGamepadName(pad);
            d.name = (name && *name) ? name : "SDL3 Gamepad";
            d.vid = SDL_GetGamepadVendor(pad);
            d.pid = SDL_GetGamepadProduct(pad);
            SDL_PropertiesID props = SDL_GetGamepadProperties(pad);
            if (props) {
                d.rumble_capable = SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
                d.trigger_rumble_capable = SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN, false);
            }
            const bool enable_motion = motion_enabled.load(std::memory_order_relaxed);
            if (SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL)) d.accel_enabled = SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_ACCEL, enable_motion);
            if (SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO)) d.gyro_enabled = SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, enable_motion);
            devices.push_back(d);
        }
        SDL_free(ids);
    }

void SDLInputManager::refresh_states_locked(uint64_t now) {
        clear_states_locked();
        for (auto& d : devices) {
            if (!d.pad || d.slot < 0 || d.slot >= 4 || !SDL_GamepadConnected(d.pad)) continue;
            SdlPadState st{};
            st.connected = true;
            st.input = map_gamepad(d);
            st.name = d.name;
            st.vid = d.vid;
            st.pid = d.pid;
            st.instance_id = d.id;
            apply_motion(d, st.motion_samples, st.has_motion);
            st.motion = st.has_motion ? st.motion_samples[2] : ns::MotionReport{};
            if (report_non_neutral(st.input) || st.has_motion) st.last_input_us = now;
            states[d.slot] = st;
        }
    }

