#include "shared/sdl_input.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <print>
#include <format>
#include <cstdlib>
#include <utility>

namespace {
constexpr int SDL_RUMBLE_DEFAULT_GAIN_PERCENT = 60;
constexpr int SDL_RUMBLE_PLAYSTATION_GAIN_PERCENT = 60;
constexpr int SDL_RUMBLE_XBOX_GAIN_PERCENT = 20;

constexpr auto SDL_INPUT_POLL_INTERVAL = std::chrono::milliseconds(4);
constexpr float STANDARD_GRAVITY = 9.80665f;
constexpr float ACCEL_SCALE = 4096.0f / STANDARD_GRAVITY;
constexpr float RAD_TO_DEG = 57.29577951308232f;
constexpr float GYRO_SCALE = RAD_TO_DEG * 16.384f;

uint8_t scale_sdl_rumble_motor(uint8_t v, int gain_percent) {
    int scaled = ((int)v * gain_percent) / 100;
    if (scaled == 0 && v != 0) scaled = 1;
    return (uint8_t)std::clamp(scaled, 0, 255);
}

bool contains_case_insensitive(const std::string& haystack, const char* needle) {
    if (!needle || !*needle) return false;
    std::string h;
    std::string n;
    h.reserve(haystack.size());
    for (unsigned char c : haystack) h.push_back((char)std::tolower(c));
    for (const unsigned char* p = (const unsigned char*)needle; *p; ++p) n.push_back((char)std::tolower(*p));
    return h.find(n) != std::string::npos;
}

bool is_playstation_controller(const std::string& name, uint16_t vid) {
    return vid == 0x054c ||
           contains_case_insensitive(name, "playstation") ||
           contains_case_insensitive(name, "dualsense") ||
           contains_case_insensitive(name, "dualshock") ||
           contains_case_insensitive(name, "wireless controller") ||
           contains_case_insensitive(name, "wirless controller");
}

bool is_xbox_controller(const std::string& name, uint16_t vid) {
    return vid == 0x045e ||
           contains_case_insensitive(name, "xbox") ||
           contains_case_insensitive(name, "microsoft") ||
           contains_case_insensitive(name, "elite");
}

}

void DigitalReleaseFilter::reset() {
    last_buttons = 0;
    std::fill(std::begin(button_until), std::end(button_until), 0);
    last_hat = ns::HAT_NEUTRAL;
    hat_until = 0;
}

void DigitalReleaseFilter::apply(ns::HoriHIDReport& r, uint64_t now) {
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

void sdl_stick_to_bytes(Sint16 raw_x, Sint16 raw_y, uint8_t& out_x, uint8_t& out_y, int deadzone) {
    // Radial (circular) deadzone. Applying a deadzone per axis (as
    // sdl_axis_to_byte does) carves a plus/cross-shaped dead region: a diagonal
    // push only registers once BOTH axes independently clear the deadzone, so a
    // diagonal needs ~deadzone*sqrt(2) of deflection versus deadzone for a
    // cardinal. That is what makes diagonals hard to hold (e.g. entering a
    // password on the Switch on-screen keyboard). Here the threshold is applied
    // to the vector magnitude and the original direction is preserved, so a
    // diagonal activates at the same push distance as a cardinal and keeps its
    // 45-degree aim. Full deflection maps to the same per-axis values a real
    // round-gated stick reports, so there is no over-driving.
    const float x = static_cast<float>(raw_x);
    const float y = static_cast<float>(raw_y);
    const float mag = std::sqrt(x * x + y * y);
    if (mag <= static_cast<float>(deadzone)) { out_x = 128; out_y = 128; return; }

    constexpr float kMaxMag = 32767.0f;
    const float clamped = std::min(mag, kMaxMag);
    const float scaled = (clamped - static_cast<float>(deadzone)) / (kMaxMag - static_cast<float>(deadzone));
    const float ux = x / mag;
    const float uy = y / mag;
    auto to_byte = [](float axis) -> uint8_t {
        const int v = 128 + static_cast<int>(std::lround(axis * 127.0f));
        return static_cast<uint8_t>(std::clamp(v, 0, 255));
    };
    out_x = to_byte(ux * scaled);
    out_y = to_byte(uy * scaled);
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

SDLInputManager::~SDLInputManager() {
        stop();
    }

bool SDLInputManager::start() {
        std::lock_guard<std::mutex> lk(life_mtx);
        if (active.load(std::memory_order_relaxed)) return true;
        thread_should_run.store(true, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> il(init_mtx);
            init_done = false;
            init_ok = false;
        }
        input_thread = std::thread([this] { thread_main(); });
        {
            std::unique_lock<std::mutex> il(init_mtx);
            init_cv.wait(il, [this] { return init_done; });
        }
        if (!init_ok) {
            thread_should_run.store(false, std::memory_order_relaxed);
            if (input_thread.joinable()) input_thread.join();
            return false;
        }
        active.store(true, std::memory_order_relaxed);
        return true;
    }

void SDLInputManager::stop() {
        std::lock_guard<std::mutex> lk(life_mtx);
        if (!input_thread.joinable()) return;
        thread_should_run.store(false, std::memory_order_relaxed);
        input_thread.join();
        active.store(false, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> cl(cmd_mtx);
            cmd_queue.clear();
            cmd_processed_seq = cmd_enqueue_seq;
        }
        cmd_drained_cv.notify_all();
    }

void SDLInputManager::poll() {}

std::array<SdlPadState, 4> SDLInputManager::snapshot() {
        std::lock_guard<std::mutex> lk(pub_mtx);
        return published_states;
    }

std::string SDLInputManager::error() const {
        std::lock_guard<std::mutex> lk(pub_mtx);
        return last_error;
    }

void SDLInputManager::request_rescan() {
        force_scan.store(true, std::memory_order_relaxed);
    }

void SDLInputManager::set_connection_callback(std::function<void(int slot, bool connected)> cb) {
        std::lock_guard<std::mutex> lk(cb_mtx);
        connection_callback = std::move(cb);
    }

void SDLInputManager::set_gyro_enabled(bool enabled) { set_motion_enabled(enabled); }

void SDLInputManager::set_motion_enabled(bool enabled) {
        motion_enabled.store(enabled, std::memory_order_relaxed);
        if (!active.load(std::memory_order_relaxed)) return;
        Command c{.type = Command::Type::ReapplyMotion};
        c.flag = enabled;
        enqueue_command(c);
    }

void SDLInputManager::set_home_shortcut_enabled(bool enabled) {
        home_shortcut_enabled.store(enabled, std::memory_order_relaxed);
    }

void SDLInputManager::set_capture_shortcut_enabled(bool enabled) {
        capture_shortcut_enabled.store(enabled, std::memory_order_relaxed);
    }

void SDLInputManager::set_controller_leds_enabled(bool enabled) {
        controller_leds_enabled.store(enabled, std::memory_order_relaxed);
    }

void SDLInputManager::set_rumble(int sdl_slot, uint8_t low, uint8_t high, uint32_t duration_ms, bool allow_trigger_rumble) {
        if (!active.load(std::memory_order_relaxed) || sdl_slot < 0 || sdl_slot >= 4) return;
        Command c{.type = Command::Type::Rumble};
        c.slot = sdl_slot;
        c.low = low;
        c.high = high;
        c.duration_ms = duration_ms;
        c.allow_trigger_rumble = allow_trigger_rumble;
        enqueue_command(c);
    }

void SDLInputManager::set_player_status(int sdl_slot, int player_index, uint8_t player_leds, const uint8_t* body_rgb) {
        if (!active.load(std::memory_order_relaxed) || sdl_slot < 0 || sdl_slot >= 4) return;
        Command c{.type = Command::Type::PlayerStatus};
        c.slot = sdl_slot;
        c.player_index = player_index;
        c.player_leds = player_leds;
        if (body_rgb) {
            c.body_rgb[0] = body_rgb[0];
            c.body_rgb[1] = body_rgb[1];
            c.body_rgb[2] = body_rgb[2];
            c.body_rgb_valid = true;
        }
        enqueue_command(c);
    }

void SDLInputManager::clear_player_status(int sdl_slot) {
        set_player_status(sdl_slot, -1, 0);
    }

void SDLInputManager::clear_all_player_status() {
        if (!active.load(std::memory_order_relaxed)) return;
        enqueue_command(Command{.type = Command::Type::ClearAllPlayerStatus});
    }

void SDLInputManager::stop_all_rumble() {
        if (!active.load(std::memory_order_relaxed)) return;
        enqueue_command(Command{.type = Command::Type::StopAllRumble});
    }

void SDLInputManager::disconnect_all() {
        if (!active.load(std::memory_order_relaxed)) return;
        uint64_t seq = enqueue_command(Command{.type = Command::Type::DisconnectAll});
        wait_drained(seq);
    }

void SDLInputManager::thread_main() {
        bool ok = init_sdl();
        {
            std::lock_guard<std::mutex> il(init_mtx);
            init_ok = ok;
            init_done = true;
        }
        init_cv.notify_one();
        if (!ok) return;

        while (thread_should_run.load(std::memory_order_relaxed)) {
            std::deque<Command> batch;
            uint64_t drained_up_to;
            {
                std::lock_guard<std::mutex> cl(cmd_mtx);
                batch.swap(cmd_queue);
                drained_up_to = cmd_enqueue_seq;
            }
            for (const auto& c : batch) process_command(c);

            poll_once();
            publish_states();

            {
                std::lock_guard<std::mutex> cl(cmd_mtx);
                cmd_processed_seq = drained_up_to;
            }
            cmd_drained_cv.notify_all();

            std::this_thread::sleep_for(SDL_INPUT_POLL_INTERVAL);
        }

        shutdown_sdl();
    }

bool SDLInputManager::init_sdl() {
#ifdef _WIN32
        SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");
        SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");
#endif
        SDL_SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
        SDL_SetHint("SDL_JOYSTICK_THREAD", "1");
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
            set_error((e && *e) ? e : "SDL_Init failed");
            return false;
        }
        initialized = true;
        // The initial device scan/open is deferred to the first poll loop iteration (last_scan_us == 0
        // forces a scan there) so start() returns as soon as SDL is initialized. Opening controllers
        // — especially slow or virtual ones — must never block application startup.
        return true;
    }

void SDLInputManager::shutdown_sdl() {
        if (!initialized) return;
        close_all_locked();
        clear_states_locked();
        publish_states();
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

void SDLInputManager::poll_once() {
        if (!initialized) return;
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_GAMEPAD_ADDED || ev.type == SDL_EVENT_GAMEPAD_REMOVED)
                force_scan.store(true, std::memory_order_relaxed);
        }
        SDL_UpdateGamepads();
        if (motion_enabled.load(std::memory_order_relaxed))
            SDL_UpdateSensors();
        uint64_t now = ns::now_us();
        if (force_scan.load(std::memory_order_relaxed) || last_scan_us == 0 || now - last_scan_us > 500000ULL)
            scan_locked(false);
        refresh_states_locked(now);
    }

void SDLInputManager::publish_states() {
        std::lock_guard<std::mutex> lk(pub_mtx);
        published_states = states;
    }

void SDLInputManager::set_error(std::string e) {
        std::lock_guard<std::mutex> lk(pub_mtx);
        last_error = std::move(e);
    }

void SDLInputManager::notify_connection(int slot, bool connected) {
        std::function<void(int, bool)> cb;
        {
            std::lock_guard<std::mutex> lk(cb_mtx);
            cb = connection_callback;
        }
        if (cb) cb(slot, connected);
    }

uint64_t SDLInputManager::enqueue_command(const Command& c) {
        std::lock_guard<std::mutex> lk(cmd_mtx);
        uint64_t seq = ++cmd_enqueue_seq;
        cmd_queue.push_back(c);
        return seq;
    }

void SDLInputManager::wait_drained(uint64_t seq) {
        std::unique_lock<std::mutex> lk(cmd_mtx);
        cmd_drained_cv.wait(lk, [&] {
            return cmd_processed_seq >= seq || !active.load(std::memory_order_relaxed);
        });
    }

void SDLInputManager::process_command(const Command& c) {
        if (!initialized) return;
        switch (c.type) {
            case Command::Type::Rumble:
                apply_rumble_locked(c.slot, c.low, c.high, c.duration_ms, c.allow_trigger_rumble);
                break;
            case Command::Type::PlayerStatus: {
                if (c.slot < 0 || c.slot >= 4) break;
                Device* d = device_for_slot_locked(c.slot);
                if (!d || !d->pad || !SDL_GamepadConnected(d->pad)) break;
                apply_player_status_locked(*d, c.player_index, c.player_leds,
                                           c.body_rgb_valid ? c.body_rgb : nullptr);
                break;
            }
            case Command::Type::ClearAllPlayerStatus:
                for (auto& d : devices) {
                    if (d.pad && SDL_GamepadConnected(d.pad)) apply_player_status_locked(d, -1, 0, nullptr);
                }
                break;
            case Command::Type::StopAllRumble:
                stop_all_rumble_locked();
                break;
            case Command::Type::DisconnectAll:
                stop_all_rumble_locked();
                close_all_locked();
                clear_states_locked();
                force_scan.store(true, std::memory_order_relaxed);
                break;
            case Command::Type::ReapplyMotion:
                apply_motion_enabled_locked(c.flag);
                break;
        }
    }

void SDLInputManager::apply_rumble_locked(int sdl_slot, uint8_t low, uint8_t high, uint32_t duration_ms, bool allow_trigger_rumble) {
        if (sdl_slot < 0 || sdl_slot >= 4) return;
        Device* d = device_for_slot_locked(sdl_slot);
        if (!d || !d->pad || !SDL_GamepadConnected(d->pad)) return;
        int gain_percent = SDL_RUMBLE_DEFAULT_GAIN_PERCENT;
        if (is_playstation_controller(d->name, d->vid)) {
            gain_percent = SDL_RUMBLE_PLAYSTATION_GAIN_PERCENT;
        } else if (is_xbox_controller(d->name, d->vid)) {
            gain_percent = SDL_RUMBLE_XBOX_GAIN_PERCENT;
            allow_trigger_rumble = false;
        }
        low = scale_sdl_rumble_motor(low, gain_percent);
        high = scale_sdl_rumble_motor(high, gain_percent);
        const Uint16 low_word = motor_word(low);
        const Uint16 high_word = motor_word(high);
        const bool stop = (low_word == 0 && high_word == 0) || duration_ms == 0;
        bool ok_main = SDL_RumbleGamepad(d->pad, stop ? 0 : low_word, stop ? 0 : high_word, duration_ms);
        bool ok_trigger = true;
        SDL_PropertiesID props = SDL_GetGamepadProperties(d->pad);
        bool trigger_capable = props && SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN, false);
        if (stop || (allow_trigger_rumble && (trigger_capable || !ok_main))) {
            ok_trigger = SDL_RumbleGamepadTriggers(d->pad, stop ? 0 : low_word, stop ? 0 : high_word, duration_ms);
        }
        if (!stop && !ok_main && !ok_trigger) {
            const char* e = SDL_GetError();
            set_error((e && *e) ? e : "SDL rumble failed");
        }
    }

void SDLInputManager::apply_motion_enabled_locked(bool enabled) {
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
            d.accel_rate_hz = d.accel_enabled
                ? SDL_GetGamepadSensorDataRate(d.pad, SDL_SENSOR_ACCEL) : 0.0f;
            d.gyro_rate_hz = d.gyro_enabled
                ? SDL_GetGamepadSensorDataRate(d.pad, SDL_SENSOR_GYRO) : 0.0f;
        }
        if (!enabled) {
            for (auto& st : states) {
                st.motion.reset();
                for (int i = 0; i < 3; ++i) st.motion_samples[i].reset();
                st.has_motion = false;
            }
        }
    }


Uint16 SDLInputManager::motor_word(uint8_t v) { return (Uint16)((uint32_t)v * 65535u / 255u); }
bool SDLInputManager::button(SDL_Gamepad* pad, SDL_GamepadButton b) { return SDL_GetGamepadButton(pad, b); }

ns::HoriHIDReport SDLInputManager::map_gamepad(const Device& d) const {
        ns::HoriHIDReport r;
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
        // Radial deadzone per stick so diagonals register as easily as
        // cardinals (see sdl_stick_to_bytes). Y is not inverted here: SDL's
        // down-positive Y already matches the Switch report convention used by
        // the previous per-axis path.
        sdl_stick_to_bytes(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX),
                           SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY), r.lx, r.ly);
        sdl_stick_to_bytes(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX),
                           SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY), r.rx, r.ry);
        return r;
    }

bool SDLInputManager::report_non_neutral(const ns::HoriHIDReport& r) {
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

void SDLInputManager::apply_motion(Device& d, ns::MotionReport out_samples[3],
                                   bool& has_motion, bool& motion_sample_fresh) {
        SDL_Gamepad* pad = d.pad;
        for (int i = 0; i < 3; ++i) out_samples[i].reset();
        has_motion = false;
        motion_sample_fresh = false;

        if (!motion_enabled.load(std::memory_order_relaxed)) {
            d.has_motion_samples = false;
            for (int i = 0; i < 3; ++i) d.motion_samples[i].reset();
            return;
        }

        ns::MotionReport sample{};
        if (d.accel_enabled) {
            float accel[3] = {0, 0, 0};
            if (SDL_GetGamepadSensorData(pad, SDL_SENSOR_ACCEL, accel, 3)) {
                sample.ax = clamp_motion_i16(-accel[2] * ACCEL_SCALE);
                sample.ay = clamp_motion_i16(-accel[0] * ACCEL_SCALE);
                sample.az = clamp_motion_i16( accel[1] * ACCEL_SCALE);
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
            motion_sample_fresh = true;
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
            apply_player_status_locked(d, -1, 0, nullptr);
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

void SDLInputManager::apply_player_status_locked(Device& d, int player_index, uint8_t player_leds, const uint8_t* body_rgb) {
        (void)player_leds;
        (void)body_rgb;
        if (!d.pad || !SDL_GamepadConnected(d.pad)) return;
        if (player_index < 0 || player_index >= 4) player_index = -1;
        if (d.applied_player_index == player_index) return;

        d.applied_player_index = player_index;
        d.applied_player_leds = 0;
        d.applied_body_rgb_valid = false;

        (void)SDL_SetGamepadPlayerIndex(d.pad, player_index);

        // Turn off the lightbar color/LED for PlayStation controllers to minimize Bluetooth overhead/disconnects if disabled.
        if (!controller_leds_enabled.load(std::memory_order_relaxed) && is_playstation_controller(d.name, d.vid)) {
            (void)SDL_SetGamepadLED(d.pad, 0, 0, 0);
        }
    }

void SDLInputManager::scan_locked(bool initial) {
        (void)initial;
        force_scan = false;
        last_scan_us = ns::now_us();
        std::erase_if(devices, [this](Device& d) {
            if (!d.pad || !SDL_GamepadConnected(d.pad)) {
                if (d.slot >= 0 && d.slot < 4) {
                    states[d.slot] = SdlPadState{};
                    notify_connection(d.slot, false);
                }
                close_device_locked(d);
                return true;
            }
            return false;
        });
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
            d.applied_player_index = -2;
            d.applied_player_leds = 0xFF;
            d.applied_body_rgb_valid = false;
            SDL_PropertiesID props = SDL_GetGamepadProperties(pad);
            if (props) {
                d.rumble_capable = SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
                d.trigger_rumble_capable = SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN, false);
            }
            notify_connection(d.slot, true);
            std::println("[sdl] controller slot={} name=\"{}\" vid={:04x} pid={:04x} rumble={} trigger_rumble={} profile={}",
                         d.slot + 1, d.name, d.vid, d.pid,
                         d.rumble_capable ? "yes" : "no",
                         d.trigger_rumble_capable ? "yes" : "no",
                         is_xbox_controller(d.name, d.vid) ? "xbox" :
                         (is_playstation_controller(d.name, d.vid) ? "playstation" : "default"));
            const bool enable_motion = motion_enabled.load(std::memory_order_relaxed);
            if (SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL)) d.accel_enabled = SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_ACCEL, enable_motion);
            if (SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO)) d.gyro_enabled = SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, enable_motion);
            d.accel_rate_hz = d.accel_enabled ? SDL_GetGamepadSensorDataRate(pad, SDL_SENSOR_ACCEL) : 0.0f;
            d.gyro_rate_hz = d.gyro_enabled ? SDL_GetGamepadSensorDataRate(pad, SDL_SENSOR_GYRO) : 0.0f;
            if (d.accel_enabled || d.gyro_enabled) {
                std::println("[sdl] motion slot={} accel={} ({:.1f} Hz) gyro={} ({:.1f} Hz)",
                             d.slot + 1, d.accel_enabled ? "yes" : "no", d.accel_rate_hz,
                             d.gyro_enabled ? "yes" : "no", d.gyro_rate_hz);
            }
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
            int battery_percent = -1;
            SDL_PowerState power_state = SDL_GetGamepadPowerInfo(d.pad, &battery_percent);
            st.battery_percent = (battery_percent >= 0 && battery_percent <= 100) ? battery_percent : -1;
            // SDL_POWERSTATE_CHARGED often means "full/not discharging", not necessarily actively charging.
            // Only set the Switch charging bit when SDL explicitly reports charging.
            st.battery_charging = (power_state == SDL_POWERSTATE_CHARGING);
            apply_motion(d, st.motion_samples, st.has_motion, st.motion_sample_fresh);
            st.motion = st.has_motion ? st.motion_samples[2] : ns::MotionReport{};
            if (report_non_neutral(st.input) || st.has_motion) st.last_input_us = now;
            states[d.slot] = st;
        }
    }
