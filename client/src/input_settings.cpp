#include "input_settings.hpp"
#include "macro_client.hpp"
#include "mouse_input.hpp"
#include "stream_runtime.hpp"
#include "shared/macros.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <QSettings>
#include <QStandardPaths>
#include <QDir>
#include <QString>
#include <QCoreApplication>

std::atomic<int> g_keyboardMode{KB_OFF};
static std::atomic<unsigned int> g_keyboardMouseInputSuspendDepth{0};
std::atomic<bool> g_gyroEnabled{true};
std::atomic<bool> g_rumbleEnabled{true};
std::atomic<bool> g_switch2AudioEnabled{false};
std::atomic<bool> g_switch2MicrophoneEnabled{false};
std::atomic<bool> g_homeShortcutEnabled{true};
std::atomic<bool> g_captureShortcutEnabled{true};
std::atomic<bool> g_mouseModeEnabled{false};
std::atomic<bool> g_joyconMouseModeEnabled{false};
std::atomic<double> g_mouseSensitivity{1.0};
std::atomic<int> g_controllerType{ns::CONTROLLER_TYPE_PRO};
std::atomic<bool> g_joyconHorizontalMode{false};
std::atomic<bool> g_switch2ModeEnabled{false}; // Runtime server-selected mode, not saved locally.
std::atomic<bool> g_switch2AudioSupported{false}; // Runtime server capability, not saved locally.
namespace {
std::mutex g_switch2AudioDeviceMutex;
std::string g_switch2PlaybackDevice = S2_AUDIO_DEVICE_DEFAULT;
std::string g_switch2MicrophoneDevice = S2_AUDIO_DEVICE_DEFAULT;
}
std::atomic<bool> g_horiModeEnabled{false};    // Runtime server-selected mode, not saved locally.
std::unordered_map<std::string, std::string> g_keyBindings;
std::mutex g_keyBindingsMutex;
std::mutex g_pressedKeysMutex;
std::unordered_set<std::string> g_pressedKeys;
SDLInputManager g_sdlInput;
std::atomic<uint64_t> g_serverLastReplyUs{0};
std::atomic<bool> g_serverRequestedDisconnect{false};
std::atomic<bool> g_serverFullDisconnect{false};
std::atomic<bool> g_serverProfileUnsupportedDisconnect{false};
std::atomic<bool> g_serverProbeFull{false};
std::mutex g_kbCacheMutex;
std::unordered_map<std::string, bool> g_kbStateCache;


std::pair<std::string, std::string> switch2_audio_device_selections() {
    std::lock_guard<std::mutex> lk(g_switch2AudioDeviceMutex);
    return {g_switch2PlaybackDevice, g_switch2MicrophoneDevice};
}

void set_switch2_audio_device_selections(std::string playback, std::string microphone) {
    if (playback.empty()) playback = S2_AUDIO_DEVICE_DEFAULT;
    if (microphone.empty()) microphone = S2_AUDIO_DEVICE_DEFAULT;
    std::lock_guard<std::mutex> lk(g_switch2AudioDeviceMutex);
    g_switch2PlaybackDevice = std::move(playback);
    g_switch2MicrophoneDevice = std::move(microphone);
}

void sync_sdl_input_options() {
    g_sdlInput.set_motion_enabled(g_gyroEnabled.load());
    g_sdlInput.set_home_shortcut_enabled(g_homeShortcutEnabled.load());
    g_sdlInput.set_capture_shortcut_enabled(g_captureShortcutEnabled.load());
}

std::string macros_path() {
    QString configDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(configDir);
    return QDir(configDir).filePath("macros.json").toStdString();
}

std::vector<std::pair<std::string, std::string>> binding_keys() {
    return {
        {"A", "V"}, {"B", "X"}, {"X", "C"}, {"Y", "Z"},
        {"L", "Q"}, {"R", "E"}, {"ZL", "1"}, {"ZR", "2"},
        {"MINUS", "3"}, {"PLUS", "4"},
        {"LSTICK", "LSHIFT"}, {"RSTICK", "RSHIFT"},
        {"HOME", "HOME"}, {"CAPTURE", "SNAPSHOT"},
        {"SL", "F4"}, {"SR", "F5"},
        {"LSTICK_UP", "W"}, {"LSTICK_DOWN", "S"},
        {"LSTICK_LEFT", "A"}, {"LSTICK_RIGHT", "D"},
        {"RSTICK_UP", "I"}, {"RSTICK_DOWN", "K"},
        {"RSTICK_LEFT", "J"}, {"RSTICK_RIGHT", "L"},
        {"DPAD_UP", "UP"}, {"DPAD_DOWN", "DOWN"},
        {"DPAD_LEFT", "LEFT"}, {"DPAD_RIGHT", "RIGHT"}
    };
}

std::vector<std::pair<std::string, std::string>> s2_binding_keys() {
    return {{"C", "F1"}, {"GL", "F2"}, {"GR", "F3"}};
}

std::unordered_map<std::string, std::string> default_key_bindings() {
    std::unordered_map<std::string, std::string> out;
    for (const auto& kv : binding_keys()) out[kv.first] = kv.second;
    for (const auto& kv : s2_binding_keys()) out[kv.first] = kv.second;
    return out;
}

std::string normalize_key_name(std::string s) {
    s = ns::macro::upper(ns::macro::trim(std::move(s)));
    if (s.size() == 4 && s.compare(0, 3, "KEY") == 0
            && s[3] >= 'A' && s[3] <= 'Z')
        return s.substr(3);
    if (s.size() == 6 && s.compare(0, 5, "DIGIT") == 0
            && s[5] >= '0' && s[5] <= '9')
        return s.substr(5);
    static const std::pair<const char*, const char*> aliases[] = {
        {"ESCAPE", "ESC"},
        {"ARROWUP", "UP"}, {"ARROWDOWN", "DOWN"},
        {"ARROWLEFT", "LEFT"}, {"ARROWRIGHT", "RIGHT"},
        {"SHIFTLEFT", "LSHIFT"}, {"SHIFTRIGHT", "RSHIFT"},
        {"CONTROLLEFT", "LCTRL"}, {"CONTROLRIGHT", "RCTRL"},
        {"ALTLEFT", "LALT"}, {"ALTRIGHT", "RALT"},
        {"METALEFT", "LMETA"}, {"METARIGHT", "RMETA"},
        {"PRINTSCREEN", "SNAPSHOT"},
    };
    for (const auto& [alias, canonical] : aliases) {
        if (s == alias) return canonical;
    }
    return s;
}

bool is_mouse_button_name(const std::string& name) {
    std::string c = normalize_key_name(name);
    return c.size() == 6 && c.compare(0, 5, "MOUSE") == 0 && c[5] >= '1' && c[5] <= '5';
}

bool mouse_mode_active() {
    return g_mouseModeEnabled.load(std::memory_order_relaxed)
        && g_keyboardMode.load(std::memory_order_relaxed) != KB_OFF
        && !joycon_mouse_mode_active()
        && !keyboard_mouse_input_suspended();
}

bool joycon_mouse_mode_supported() {
    const int type = g_controllerType.load(std::memory_order_relaxed);
    return mouse_input_native_joycon_supported()
        && g_keyboardMode.load(std::memory_order_relaxed) != KB_OFF
        && g_connected.load(std::memory_order_relaxed)
        && g_switch2ModeEnabled.load(std::memory_order_relaxed)
        && type == ns::CONTROLLER_TYPE_JOYCON_R;
}

bool joycon_mouse_mode_active() {
    return g_joyconMouseModeEnabled.load(std::memory_order_relaxed)
        && joycon_mouse_mode_supported()
        && !keyboard_mouse_input_suspended();
}

bool mouse_capture_active() {
    return mouse_mode_active() || joycon_mouse_mode_active();
}

void suspend_keyboard_mouse_input() {
    if (g_keyboardMouseInputSuspendDepth.fetch_add(1, std::memory_order_acq_rel) == 0) {
        clear_pressed_key_cache();
        mouse_input_reset();
    }
}

void resume_keyboard_mouse_input() {
    unsigned int depth = g_keyboardMouseInputSuspendDepth.load(std::memory_order_acquire);
    while (depth != 0) {
        if (g_keyboardMouseInputSuspendDepth.compare_exchange_weak(
                depth, depth - 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
            if (depth == 1) {
                clear_pressed_key_cache();
                mouse_input_reset();
            }
            return;
        }
    }
}

bool keyboard_mouse_input_suspended() {
    return g_keyboardMouseInputSuspendDepth.load(std::memory_order_acquire) != 0;
}

bool is_valid_key_code(const std::string& s) {
    std::string c = normalize_key_name(s);
    if (c.empty()) return true;
    if (is_mouse_button_name(c)) return true;
    static const char* named[] = {
        "ESC","ESCAPE","SPACE","ENTER","TAB","BACKSPACE","DELETE","INSERT","HOME","END","PAGEUP","PAGEDOWN",
        "CAPSLOCK","NUMLOCK","SCROLLLOCK","PAUSE","SNAPSHOT","PRINTSCREEN","CONTEXTMENU","UP","DOWN","LEFT","RIGHT",
        "LSHIFT","RSHIFT","LCTRL","RCTRL","LALT","RALT","LMETA","RMETA", "SHIFTLEFT","SHIFTRIGHT","METALEFT","METARIGHT",
        "CONTROLLEFT","CONTROLRIGHT","ALTLEFT","ALTRIGHT","ARROWUP","ARROWDOWN","ARROWLEFT","ARROWRIGHT"
    };
    for (const char* n : named) if (c == n) return true;
    if (c.size() == 1 && std::isalnum((unsigned char)c[0])) return true;
    if (c.size() >= 2 && c.size() <= 3 && c[0] == 'F' && std::all_of(c.begin() + 1, c.end(), ::isdigit)) {
        int val = std::stoi(c.substr(1));
        return val >= 1 && val <= 24;
    }
    if (c.substr(0, 3) == "KEY" && c.size() == 4 && std::isalpha((unsigned char)c[3])) return true;
    if (c.substr(0, 5) == "DIGIT" && c.size() == 6 && std::isdigit((unsigned char)c[5])) return true;
    return false;
}

std::string normalize_macro_hotkey_for_io(const std::string& s) {
    return normalize_key_name(s);
}

void load_saved_bindings() {
    std::lock_guard<std::mutex> lk(g_keyBindingsMutex);
    g_keyBindings = default_key_bindings();
    QSettings settings(QSettings::IniFormat, QSettings::UserScope, "NSPCControl", "NSControl");
    settings.beginGroup("Bindings");
    for (const auto& key : settings.childKeys()) {
        auto it = g_keyBindings.find(key.toStdString());
        if (it != g_keyBindings.end()) {
            it->second = normalize_key_name(settings.value(key).toString().toStdString());
        }
    }
    settings.endGroup();
}

void save_bindings() {
    std::lock_guard<std::mutex> lk(g_keyBindingsMutex);
    QSettings settings(QSettings::IniFormat, QSettings::UserScope, "NSPCControl", "NSControl");
    settings.beginGroup("Bindings");
    for (const auto& kv : g_keyBindings) {
        settings.setValue(QString::fromStdString(kv.first), QString::fromStdString(kv.second));
    }
    settings.endGroup();
}

std::string load_saved_ip() {
    return QSettings(QSettings::IniFormat, QSettings::UserScope, "NSPCControl", "NSControl")
        .value("LastIP", "192.168.1.100").toString().toStdString();
}

void save_last_ip(const std::string& ip) {
    QSettings(QSettings::IniFormat, QSettings::UserScope, "NSPCControl", "NSControl")
        .setValue("LastIP", QString::fromStdString(ip));
}

int load_saved_keyboard_mode() {
    int mode = QSettings(QSettings::IniFormat, QSettings::UserScope, "NSPCControl", "NSControl")
        .value("KeyboardMode", KB_OFF).toInt();
    return (mode >= KB_OFF && mode <= KB_OVERRIDE) ? mode : KB_OFF;
}

void save_keyboard_mode(int mode) {
    QSettings(QSettings::IniFormat, QSettings::UserScope, "NSPCControl", "NSControl")
        .setValue("KeyboardMode", mode);
}

void load_saved_feature_toggles() {
    QSettings settings(QSettings::IniFormat, QSettings::UserScope, "NSPCControl", "NSControl");
    g_gyroEnabled.store(settings.value("GyroEnabled", true).toBool());
    g_rumbleEnabled.store(settings.value("RumbleEnabled", true).toBool());
    g_switch2AudioEnabled.store(settings.value("Switch2AudioEnabled", false).toBool());
    g_switch2MicrophoneEnabled.store(settings.value("Switch2MicrophoneEnabled", false).toBool());
    set_switch2_audio_device_selections(
        settings.value("Switch2PlaybackDevice", S2_AUDIO_DEVICE_DEFAULT).toString().toStdString(),
        settings.value("Switch2MicrophoneDevice", S2_AUDIO_DEVICE_DEFAULT).toString().toStdString());
    g_homeShortcutEnabled.store(settings.value("HomeShortcutEnabled", true).toBool());
    g_captureShortcutEnabled.store(settings.value("CaptureShortcutEnabled", true).toBool());
    g_mouseModeEnabled.store(settings.value("MouseModeEnabled", false).toBool());
    // Native Joy-Con mouse mode is deliberately session-only. Never restore it
    // after an application restart, and remove keys written by older builds.
    g_joyconMouseModeEnabled.store(false, std::memory_order_relaxed);
    settings.remove("JoyconMouseModeEnabled");
    g_switch2ModeEnabled.store(false, std::memory_order_relaxed);
    g_switch2AudioSupported.store(false, std::memory_order_relaxed);
    g_horiModeEnabled.store(false, std::memory_order_relaxed);
    double sens = settings.value("MouseSensitivity", 1.0).toDouble();
    if (!std::isfinite(sens)) sens = 1.0;
    g_mouseSensitivity.store(std::clamp(sens, 0.0, 5.0));
    int controllerType = settings.value("ControllerType", ns::CONTROLLER_TYPE_PRO).toInt();
    if (controllerType != ns::CONTROLLER_TYPE_PRO &&
        controllerType != ns::CONTROLLER_TYPE_JOYCON_L &&
        controllerType != ns::CONTROLLER_TYPE_JOYCON_R &&
        controllerType != ns::CONTROLLER_TYPE_JOYCON_PAIR) {
        controllerType = ns::CONTROLLER_TYPE_PRO;
    }
    g_controllerType.store(controllerType);
    const bool single_joycon = controllerType == ns::CONTROLLER_TYPE_JOYCON_L
        || controllerType == ns::CONTROLLER_TYPE_JOYCON_R;
    g_joyconHorizontalMode.store(single_joycon && settings.value("JoyconHorizontalMode", false).toBool());
    sync_sdl_input_options();
}

void save_feature_toggles() {
    QSettings settings(QSettings::IniFormat, QSettings::UserScope, "NSPCControl", "NSControl");
    settings.setValue("GyroEnabled", g_gyroEnabled.load());
    settings.setValue("RumbleEnabled", g_rumbleEnabled.load());
    settings.setValue("Switch2AudioEnabled", g_switch2AudioEnabled.load());
    settings.setValue("Switch2MicrophoneEnabled", g_switch2MicrophoneEnabled.load());
    const auto [playbackDevice, microphoneDevice] = switch2_audio_device_selections();
    settings.setValue("Switch2PlaybackDevice", QString::fromStdString(playbackDevice));
    settings.setValue("Switch2MicrophoneDevice", QString::fromStdString(microphoneDevice));
    settings.setValue("HomeShortcutEnabled", g_homeShortcutEnabled.load());
    settings.setValue("CaptureShortcutEnabled", g_captureShortcutEnabled.load());
    settings.setValue("MouseModeEnabled", g_mouseModeEnabled.load());
    // Deliberately not persisted: the native optical-sensor mode must be
    // explicitly enabled for each live S2 Joy-Con session.
    settings.remove("JoyconMouseModeEnabled");
    settings.setValue("MouseSensitivity", g_mouseSensitivity.load());
    settings.setValue("ControllerType", g_controllerType.load());
    settings.setValue("JoyconHorizontalMode", g_joyconHorizontalMode.load());
}

void clear_mouse_button_inputs() {
    bool bindings_changed = false;
    {
        std::lock_guard<std::mutex> lk(g_keyBindingsMutex);
        for (auto& kv : g_keyBindings) {
            if (is_mouse_button_name(kv.second)) { kv.second.clear(); bindings_changed = true; }
        }
    }
    if (bindings_changed) save_bindings();

    bool macros_changed = false;
    {
        std::lock_guard<std::mutex> lk(g_macro_mtx);
        for (auto& e : g_macro_entries) {
            if (is_mouse_button_name(e.hotkey)) { e.hotkey.clear(); macros_changed = true; }
        }
        if (macros_changed) rebuild_macro_hotkey_state();
    }
    if (macros_changed) save_macro_entries_to_disk();
}

void set_key_pressed(const std::string& key, bool down) {
    std::string k = normalize_key_name(key);
    if (k.empty()) return;
    std::lock_guard<std::mutex> lk(g_pressedKeysMutex);
    if (down) g_pressedKeys.insert(k);
    else g_pressedKeys.erase(k);
}

bool pressed_key_cache_contains(const std::string& key) {
    std::lock_guard<std::mutex> lk(g_pressedKeysMutex);
    return g_pressedKeys.count(normalize_key_name(key)) != 0;
}

void clear_pressed_key_cache() {
    std::lock_guard<std::mutex> lk(g_pressedKeysMutex);
    g_pressedKeys.clear();
}

void update_keyboard_state_cache() {
    std::lock_guard<std::mutex> lk_bind(g_keyBindingsMutex);
    std::lock_guard<std::mutex> lk_cache(g_kbCacheMutex);
    for (const auto& kv : g_keyBindings) {
        std::string key = normalize_key_name(kv.second);
        if (key.empty()) continue;
        if (is_mouse_button_name(key) && !mouse_mode_active()) { g_kbStateCache[key] = false; continue; }
        bool down = false;
        if (!mouse_input_query_key_state(key, down))
            down = pressed_key_cache_contains(key);
        g_kbStateCache[key] = down;
    }
}

bool key_is_down(const std::string& name_raw) {
    std::string name = normalize_key_name(name_raw);
    if (name.empty()) return false;
    if (is_mouse_button_name(name) && !mouse_mode_active()) return false;
    // Native probes are authoritative and are cheap enough to consult at the
    // 250 Hz report cadence. This avoids both focus lag and stale Qt key-down
    // entries after the application loses focus.
    bool native_down = false;
    if (mouse_input_query_key_state(name, native_down)) return native_down;
    {
        std::lock_guard<std::mutex> lk(g_kbCacheMutex);
        auto it = g_kbStateCache.find(name);
        if (it != g_kbStateCache.end()) return it->second;
    }
    return pressed_key_cache_contains(name);
}

void apply_keyboard_to_report(ns::HoriHIDReport& rep, bool override_mode) {
    if (keyboard_mouse_input_suspended()) return;
    std::lock_guard<std::mutex> lk(g_keyBindingsMutex);
    auto get = [](const std::string& btn) -> std::string {
        auto it = g_keyBindings.find(btn);
        return it != g_keyBindings.end() ? it->second : "";
    };
    struct BtnMap { const char* name; uint32_t flag; };
    static const BtnMap btn_map[] = {
        {"Y", ns::BTN_Y}, {"B", ns::BTN_B}, {"A", ns::BTN_A}, {"X", ns::BTN_X},
        {"L", ns::BTN_L}, {"R", ns::BTN_R}, {"ZL", ns::BTN_ZL}, {"ZR", ns::BTN_ZR},
        {"MINUS", ns::BTN_MINUS}, {"PLUS", ns::BTN_PLUS}, {"LSTICK", ns::BTN_LSTICK},
        {"RSTICK", ns::BTN_RSTICK}, {"HOME", ns::BTN_HOME}, {"CAPTURE", ns::BTN_CAPTURE}
    };
    for (const auto& m : btn_map) {
        if (auto k = get(m.name); !k.empty() && key_is_down(k)) rep.buttons |= m.flag;
    }
    if (mouse_mode_active()) {
        bool mouse1_custom = false;
        bool mouse2_custom = false;
        for (const auto& binding : g_keyBindings) {
            const std::string key = normalize_key_name(binding.second);
            mouse1_custom = mouse1_custom || key == "MOUSE1";
            mouse2_custom = mouse2_custom || key == "MOUSE2";
        }
        if (!mouse1_custom && key_is_down("MOUSE1")) rep.buttons |= ns::BTN_ZR;
        if (!mouse2_custom && key_is_down("MOUSE2")) rep.buttons |= ns::BTN_ZL;
    }
    struct ExtraBtnMap { const char* name; uint8_t flag; };
    static const ExtraBtnMap extra_btn_map[] = {
        {"C", ns::EXT_BUTTON_C}, {"GL", ns::EXT_BUTTON_GL}, {"GR", ns::EXT_BUTTON_GR},
        {"SL", ns::EXT_BUTTON_SL}, {"SR", ns::EXT_BUTTON_SR},
    };
    for (const auto& m : extra_btn_map) {
        if (auto k = get(m.name); !k.empty() && key_is_down(k)) rep.vendor |= m.flag;
    }

    bool du = !get("DPAD_UP").empty() && key_is_down(get("DPAD_UP"));
    bool dd = !get("DPAD_DOWN").empty() && key_is_down(get("DPAD_DOWN"));
    bool dl = !get("DPAD_LEFT").empty() && key_is_down(get("DPAD_LEFT"));
    bool dr = !get("DPAD_RIGHT").empty() && key_is_down(get("DPAD_RIGHT"));
    const bool dpad_active = du || dd || dl || dr;
    if (dpad_active || !override_mode) {
        rep.hat = ns::HAT_NEUTRAL;
        if (du && dr) rep.hat = ns::HAT_NE;
        else if (du && dl) rep.hat = ns::HAT_NW;
        else if (dd && dr) rep.hat = ns::HAT_SE;
        else if (dd && dl) rep.hat = ns::HAT_SW;
        else if (du) rep.hat = ns::HAT_N;
        else if (dd) rep.hat = ns::HAT_S;
        else if (dr) rep.hat = ns::HAT_E;
        else if (dl) rep.hat = ns::HAT_W;
    }

    auto apply_axis = [&](const char* min_key, const char* max_key, uint8_t& val) {
        bool min_d = !get(min_key).empty() && key_is_down(get(min_key));
        bool max_d = !get(max_key).empty() && key_is_down(get(max_key));
        if (min_d && !max_d) val = 0;
        else if (max_d && !min_d) val = 255;
        else if (!override_mode) val = 128;
    };
    apply_axis("LSTICK_LEFT", "LSTICK_RIGHT", rep.lx);
    apply_axis("LSTICK_UP", "LSTICK_DOWN", rep.ly);
    apply_axis("RSTICK_LEFT", "RSTICK_RIGHT", rep.rx);
    apply_axis("RSTICK_UP", "RSTICK_DOWN", rep.ry);

}

void apply_joycon_horizontal_transform(ns::HoriHIDReport& rep, int controller_type) {
    const bool left = controller_type == ns::CONTROLLER_TYPE_JOYCON_L;
    const bool right = controller_type == ns::CONTROLLER_TYPE_JOYCON_R;
    if (!left && !right) return;

    // A sideways Joy-Con exposes its rail buttons as the two shoulders. Treat
    // either left-side shoulder input as SL and either right-side input as SR,
    // then remove the full-controller shoulder bits from the Joy-Con report.
    const uint16_t shoulders = rep.buttons & (ns::BTN_L | ns::BTN_ZL | ns::BTN_R | ns::BTN_ZR);
    if (shoulders & (ns::BTN_L | ns::BTN_ZL)) rep.vendor |= ns::EXT_BUTTON_SL;
    if (shoulders & (ns::BTN_R | ns::BTN_ZR)) rep.vendor |= ns::EXT_BUTTON_SR;
    rep.buttons &= static_cast<uint16_t>(~(ns::BTN_L | ns::BTN_ZL | ns::BTN_R | ns::BTN_ZR));

    // Rotate both sticks around their 8-bit centre. The two Joy-Con halves are
    // held in opposite directions when used horizontally.
    const auto rotate_stick = [left](uint8_t& x, uint8_t& y) {
        const uint8_t old_x = x;
        const uint8_t old_y = y;
        if (left) { x = static_cast<uint8_t>(255 - old_y); y = old_x; }
        else      { x = old_y; y = static_cast<uint8_t>(255 - old_x); }
    };
    rotate_stick(rep.lx, rep.ly);
    rotate_stick(rep.rx, rep.ry);

    const auto rotate_hat = [left](uint8_t hat) {
        if (hat == ns::HAT_NEUTRAL) return hat;
        static constexpr uint8_t clockwise[] = {
            ns::HAT_E, ns::HAT_SE, ns::HAT_S, ns::HAT_SW,
            ns::HAT_W, ns::HAT_NW, ns::HAT_N, ns::HAT_NE,
        };
        static constexpr uint8_t counter_clockwise[] = {
            ns::HAT_W, ns::HAT_NW, ns::HAT_N, ns::HAT_NE,
            ns::HAT_E, ns::HAT_SE, ns::HAT_S, ns::HAT_SW,
        };
        return hat <= ns::HAT_NW ? (left ? clockwise[hat] : counter_clockwise[hat]) : hat;
    };
    rep.hat = rotate_hat(rep.hat);

    // Rotate the ABXY diamond too, so a physical layout remains consistent
    // when the right Joy-Con is held sideways.
    const uint16_t face = rep.buttons & (ns::BTN_A | ns::BTN_B | ns::BTN_X | ns::BTN_Y);
    rep.buttons &= static_cast<uint16_t>(~(ns::BTN_A | ns::BTN_B | ns::BTN_X | ns::BTN_Y));
    if (left) {
        if (face & ns::BTN_X) rep.buttons |= ns::BTN_A;
        if (face & ns::BTN_A) rep.buttons |= ns::BTN_B;
        if (face & ns::BTN_B) rep.buttons |= ns::BTN_Y;
        if (face & ns::BTN_Y) rep.buttons |= ns::BTN_X;
    } else {
        if (face & ns::BTN_B) rep.buttons |= ns::BTN_A;
        if (face & ns::BTN_Y) rep.buttons |= ns::BTN_B;
        if (face & ns::BTN_X) rep.buttons |= ns::BTN_Y;
        if (face & ns::BTN_A) rep.buttons |= ns::BTN_X;
    }
}

void apply_joycon_horizontal_motion_transform(ns::MotionReport& m, int controller_type) {
    const bool left = controller_type == ns::CONTROLLER_TYPE_JOYCON_L;
    const bool right = controller_type == ns::CONTROLLER_TYPE_JOYCON_R;
    if (!left && !right) return;

    // Motion is in the Pro-normalised frame (X = forward, Y = left, Z = up).
    // Rotate Y/Z the same way the stick is rotated above so a physical
    // gesture lands on the same in-game axis in both orientations.
    const int16_t old_ay = m.ay, old_az = m.az;
    const int16_t old_gy = m.gy, old_gz = m.gz;
    if (left) {
        m.ay = static_cast<int16_t>(-old_az); m.az = old_ay;
        m.gy = static_cast<int16_t>(-old_gz); m.gz = old_gy;
    } else {
        m.ay = old_az;                        m.az = static_cast<int16_t>(-old_ay);
        m.gy = old_gz;                        m.gz = static_cast<int16_t>(-old_gy);
    }
}
