#include "input_settings.hpp"
#include "shared/macros.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <QSettings>
#include <QStandardPaths>
#include <QDir>
#include <QString>
#include <QCoreApplication>

std::atomic<int> g_keyboardMode{KB_OFF};
std::atomic<bool> g_gyroEnabled{true};
std::atomic<bool> g_rumbleEnabled{true};
std::atomic<bool> g_homeShortcutEnabled{true};
std::atomic<bool> g_captureShortcutEnabled{true};
std::unordered_map<std::string, std::string> g_keyBindings;
std::mutex g_keyBindingsMutex;
std::mutex g_pressedKeysMutex;
std::unordered_set<std::string> g_pressedKeys;
SDLInputManager g_sdlInput;
std::atomic<uint64_t> g_serverLastReplyUs{0};
std::mutex g_kbCacheMutex;
std::unordered_map<std::string, bool> g_kbStateCache;

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
        {"HOME", "HOME"}, {"LSTICK_UP", "W"}, {"LSTICK_DOWN", "S"},
        {"LSTICK_LEFT", "A"}, {"LSTICK_RIGHT", "D"},
        {"RSTICK_UP", "I"}, {"RSTICK_DOWN", "K"},
        {"RSTICK_LEFT", "J"}, {"RSTICK_RIGHT", "L"},
        {"DPAD_UP", "UP"}, {"DPAD_DOWN", "DOWN"},
        {"DPAD_LEFT", "LEFT"}, {"DPAD_RIGHT", "RIGHT"},
        {"CAPTURE", "SNAPSHOT"}
    };
}

std::unordered_map<std::string, std::string> default_key_bindings() {
    std::unordered_map<std::string, std::string> out;
    for (const auto& kv : binding_keys()) out[kv.first] = kv.second;
    return out;
}

std::string normalize_key_name(std::string s) {
    return ns::macro::upper(ns::macro::trim(std::move(s)));
}

bool is_valid_key_code(const std::string& s) {
    std::string c = normalize_key_name(s);
    if (c.empty()) return true;
    static const char* named[] = {"ESC","ESCAPE","SPACE","ENTER","TAB","BACKSPACE","DELETE","INSERT","HOME","END","PAGEUP","PAGEDOWN","CAPSLOCK","NUMLOCK","SCROLLLOCK","PAUSE","SNAPSHOT","PRINTSCREEN","CONTEXTMENU","UP","DOWN","LEFT","RIGHT","LSHIFT","RSHIFT","LCTRL","RCTRL","LALT","RALT","LMETA","RMETA"};
    for (const char* n : named) if (c == n) return true;
    if (c.size() == 1 && ((c[0] >= 'A' && c[0] <= 'Z') || (c[0] >= '0' && c[0] <= '9'))) return true;
    if (c.size() == 2 && c[0] == 'F' && c[1] >= '1' && c[1] <= '9') return true;
    if (c.size() == 3 && c[0] == 'F' && c[1] == '1' && c[2] >= '0' && c[2] <= '9') return true;
    if (c.size() == 3 && c[0] == 'F' && c[1] == '2' && c[2] >= '0' && c[2] <= '4') return true;
    if (c.size() > 3 && c.substr(0, 3) == "KEY" && c.size() == 4 && c[3] >= 'A' && c[3] <= 'Z') return true;
    if (c.size() > 4 && c.substr(0, 5) == "DIGIT" && c.size() == 6 && c[5] >= '0' && c[5] <= '9') return true;
    if (c.size() > 4 && c.substr(0, 5) == "ARROW" && (c == "ARROWUP" || c == "ARROWDOWN" || c == "ARROWLEFT" || c == "ARROWRIGHT")) return true;
    if (c.size() > 4 && (c.substr(0, 5) == "SHIFT" || c.substr(0, 5) == "METAL") && (c == "SHIFTLEFT" || c == "SHIFTRIGHT" || c == "METALEFT" || c == "METARIGHT")) return true;
    if (c.size() > 6 && c.substr(0, 7) == "CONTROL" && (c == "CONTROLLEFT" || c == "CONTROLRIGHT")) return true;
    if (c.size() > 2 && c.substr(0, 3) == "ALT" && (c == "ALTLEFT" || c == "ALTRIGHT")) return true;
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
    QSettings settings(QSettings::IniFormat, QSettings::UserScope, "NSPCControl", "NSControl");
    return settings.value("LastIP", "192.168.1.100").toString().toStdString();
}

void save_last_ip(const std::string& ip) {
    QSettings settings(QSettings::IniFormat, QSettings::UserScope, "NSPCControl", "NSControl");
    settings.setValue("LastIP", QString::fromStdString(ip));
}

int load_saved_keyboard_mode() {
    QSettings settings(QSettings::IniFormat, QSettings::UserScope, "NSPCControl", "NSControl");
    int mode = settings.value("KeyboardMode", KB_OFF).toInt();
    return (mode >= KB_OFF && mode <= KB_OVERRIDE) ? mode : KB_OFF;
}

void save_keyboard_mode(int mode) {
    QSettings settings(QSettings::IniFormat, QSettings::UserScope, "NSPCControl", "NSControl");
    settings.setValue("KeyboardMode", mode);
}

void load_saved_feature_toggles() {
    QSettings settings(QSettings::IniFormat, QSettings::UserScope, "NSPCControl", "NSControl");
    g_gyroEnabled.store(settings.value("GyroEnabled", true).toBool());
    g_rumbleEnabled.store(settings.value("RumbleEnabled", true).toBool());
    g_homeShortcutEnabled.store(settings.value("HomeShortcutEnabled", true).toBool());
    g_captureShortcutEnabled.store(settings.value("CaptureShortcutEnabled", true).toBool());
    sync_sdl_input_options();
}

void save_feature_toggles() {
    QSettings settings(QSettings::IniFormat, QSettings::UserScope, "NSPCControl", "NSControl");
    settings.setValue("GyroEnabled", g_gyroEnabled.load());
    settings.setValue("RumbleEnabled", g_rumbleEnabled.load());
    settings.setValue("HomeShortcutEnabled", g_homeShortcutEnabled.load());
    settings.setValue("CaptureShortcutEnabled", g_captureShortcutEnabled.load());
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

#ifdef _WIN32
int windows_vk_for_key(const std::string& name) {
    if (name.size() == 1 && name[0] >= 'A' && name[0] <= 'Z') return name[0];
    if (name.size() == 1 && name[0] >= '0' && name[0] <= '9') return name[0];
    struct Map { const char* n; int vk; };
    static const Map map[] = {
        {"UP", VK_UP}, {"DOWN", VK_DOWN}, {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT},
        {"LSHIFT", VK_LSHIFT}, {"RSHIFT", VK_RSHIFT}, {"LCTRL", VK_LCONTROL}, {"RCTRL", VK_RCONTROL},
        {"LALT", VK_LMENU}, {"RALT", VK_RMENU}, {"SPACE", VK_SPACE}, {"ENTER", VK_RETURN},
        {"TAB", VK_TAB}, {"ESC", VK_ESCAPE}, {"BACKSPACE", VK_BACK}, {"HOME", VK_HOME},
        {"SNAPSHOT", VK_SNAPSHOT}, {"F1", VK_F1}, {"F2", VK_F2}, {"F3", VK_F3}, {"F4", VK_F4},
        {"F5", VK_F5}, {"F6", VK_F6}, {"F7", VK_F7}, {"F8", VK_F8}, {"F9", VK_F9},
        {"F10", VK_F10}, {"F11", VK_F11}, {"F12", VK_F12}
    };
    for (const auto& m : map) if (name == m.n) return m.vk;
    return 0;
}
#endif

#ifdef __APPLE__
int mac_keycode_for_key(const std::string& name) {
    static const std::unordered_map<std::string, int> map = {
        {"A", 0}, {"S", 1}, {"D", 2}, {"F", 3}, {"H", 4}, {"G", 5}, {"Z", 6}, {"X", 7},
        {"C", 8}, {"V", 9}, {"B", 11}, {"Q", 12}, {"W", 13}, {"E", 14}, {"R", 15},
        {"Y", 16}, {"T", 17}, {"1", 18}, {"2", 19}, {"3", 20}, {"4", 21}, {"6", 22},
        {"5", 23}, {"=", 24}, {"9", 25}, {"7", 26}, {"-", 27}, {"8", 28}, {"0", 29},
        {"O", 31}, {"U", 32}, {"I", 34}, {"P", 35}, {"L", 37}, {"J", 38}, {"K", 40},
        {"N", 45}, {"M", 46}, {"TAB", 48}, {"SPACE", 49}, {"BACKSPACE", 51}, {"ESC", 53},
        {"LCTRL", 59}, {"LSHIFT", 56}, {"LALT", 58}, {"RCTRL", 62}, {"RSHIFT", 60}, {"RALT", 61},
        {"LEFT", 123}, {"RIGHT", 124}, {"DOWN", 125}, {"UP", 126}, {"ENTER", 36}, {"HOME", 115},
        {"F1", 122}, {"F2", 120}, {"F3", 99}, {"F4", 118}, {"F5", 96}, {"F6", 97},
        {"F7", 98}, {"F8", 100}, {"F9", 101}, {"F10", 109}, {"F11", 103}, {"F12", 111}
    };
    auto it = map.find(name);
    return it == map.end() ? -1 : it->second;
}
#endif

void update_keyboard_state_cache() {
    std::lock_guard<std::mutex> lk_bind(g_keyBindingsMutex);
    std::lock_guard<std::mutex> lk_cache(g_kbCacheMutex);
    for (const auto& kv : g_keyBindings) {
        std::string key = normalize_key_name(kv.second);
        if (key.empty()) continue;
        bool down = false;
#ifdef _WIN32
        int vk = windows_vk_for_key(key);
        if (vk && (GetAsyncKeyState(vk) & 0x8000)) down = true;
#endif
#ifdef __APPLE__
        int kc = mac_keycode_for_key(key);
        if (kc >= 0 && CGEventSourceKeyState(kCGEventSourceStateHIDSystemState, (CGKeyCode)kc)) down = true;
#endif
        if (!down) down = pressed_key_cache_contains(key);
        g_kbStateCache[key] = down;
    }
}

bool key_is_down(const std::string& name_raw) {
    std::string name = normalize_key_name(name_raw);
    if (name.empty()) return false;
    {
        std::lock_guard<std::mutex> lk(g_kbCacheMutex);
        auto it = g_kbStateCache.find(name);
        if (it != g_kbStateCache.end()) return it->second;
    }
#ifdef _WIN32
    int vk = windows_vk_for_key(name);
    if (vk && (GetAsyncKeyState(vk) & 0x8000)) return true;
#endif
#ifdef __APPLE__
    int kc = mac_keycode_for_key(name);
    if (kc >= 0 && CGEventSourceKeyState(kCGEventSourceStateHIDSystemState, (CGKeyCode)kc)) return true;
#endif
    return pressed_key_cache_contains(name);
}

void apply_keyboard_to_report(ns::HIDReport& rep, bool override_mode) {
    std::lock_guard<std::mutex> lk(g_keyBindingsMutex);
    auto get = [](const std::string& btn) -> std::string {
        auto it = g_keyBindings.find(btn);
        return it != g_keyBindings.end() ? it->second : "";
    };
    std::string k;
    k = get("Y"); if (!k.empty() && key_is_down(k)) rep.buttons |= ns::BTN_Y;
    k = get("B"); if (!k.empty() && key_is_down(k)) rep.buttons |= ns::BTN_B;
    k = get("A"); if (!k.empty() && key_is_down(k)) rep.buttons |= ns::BTN_A;
    k = get("X"); if (!k.empty() && key_is_down(k)) rep.buttons |= ns::BTN_X;
    k = get("L"); if (!k.empty() && key_is_down(k)) rep.buttons |= ns::BTN_L;
    k = get("R"); if (!k.empty() && key_is_down(k)) rep.buttons |= ns::BTN_R;
    k = get("ZL"); if (!k.empty() && key_is_down(k)) rep.buttons |= ns::BTN_ZL;
    k = get("ZR"); if (!k.empty() && key_is_down(k)) rep.buttons |= ns::BTN_ZR;
    k = get("MINUS"); if (!k.empty() && key_is_down(k)) rep.buttons |= ns::BTN_MINUS;
    k = get("PLUS"); if (!k.empty() && key_is_down(k)) rep.buttons |= ns::BTN_PLUS;
    k = get("LSTICK"); if (!k.empty() && key_is_down(k)) rep.buttons |= ns::BTN_LSTICK;
    k = get("RSTICK"); if (!k.empty() && key_is_down(k)) rep.buttons |= ns::BTN_RSTICK;
    k = get("HOME"); if (!k.empty() && key_is_down(k)) rep.buttons |= ns::BTN_HOME;
    k = get("CAPTURE"); if (!k.empty() && key_is_down(k)) rep.buttons |= ns::BTN_CAPTURE;

    bool du = !get("DPAD_UP").empty() && key_is_down(get("DPAD_UP"));
    bool dd = !get("DPAD_DOWN").empty() && key_is_down(get("DPAD_DOWN"));
    bool dl = !get("DPAD_LEFT").empty() && key_is_down(get("DPAD_LEFT"));
    bool dr = !get("DPAD_RIGHT").empty() && key_is_down(get("DPAD_RIGHT"));
    rep.hat = ns::HAT_NEUTRAL;
    if (du && dr) rep.hat = ns::HAT_NE;
    else if (du && dl) rep.hat = ns::HAT_NW;
    else if (dd && dr) rep.hat = ns::HAT_SE;
    else if (dd && dl) rep.hat = ns::HAT_SW;
    else if (du) rep.hat = ns::HAT_N;
    else if (dd) rep.hat = ns::HAT_S;
    else if (dr) rep.hat = ns::HAT_E;
    else if (dl) rep.hat = ns::HAT_W;

    bool lsu = !get("LSTICK_UP").empty() && key_is_down(get("LSTICK_UP"));
    bool lsd = !get("LSTICK_DOWN").empty() && key_is_down(get("LSTICK_DOWN"));
    bool lsl = !get("LSTICK_LEFT").empty() && key_is_down(get("LSTICK_LEFT"));
    bool lsr = !get("LSTICK_RIGHT").empty() && key_is_down(get("LSTICK_RIGHT"));
    if (lsl && !lsr) rep.lx = 0;
    else if (lsr && !lsl) rep.lx = 255;
    else if (!override_mode) rep.lx = 128;
    if (lsu && !lsd) rep.ly = 0;
    else if (lsd && !lsu) rep.ly = 255;
    else if (!override_mode) rep.ly = 128;

    bool rsu = !get("RSTICK_UP").empty() && key_is_down(get("RSTICK_UP"));
    bool rsd = !get("RSTICK_DOWN").empty() && key_is_down(get("RSTICK_DOWN"));
    bool rsl = !get("RSTICK_LEFT").empty() && key_is_down(get("RSTICK_LEFT"));
    bool rsr = !get("RSTICK_RIGHT").empty() && key_is_down(get("RSTICK_RIGHT"));
    if (rsl && !rsr) rep.rx = 0;
    else if (rsr && !rsl) rep.rx = 255;
    else if (!override_mode) rep.rx = 128;
    if (rsu && !rsd) rep.ry = 0;
    else if (rsd && !rsu) rep.ry = 255;
    else if (!override_mode) rep.ry = 128;
}
