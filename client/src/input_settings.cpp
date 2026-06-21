#include "input_settings.hpp"
#include "shared/macros.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>

std::atomic<int> g_keyboardMode{KB_OFF};
std::atomic<bool> g_gyroEnabled{true};
std::atomic<bool> g_rumbleEnabled{true};
std::atomic<bool> g_homeShortcutEnabled{true};
std::atomic<bool> g_captureShortcutEnabled{true};
std::unordered_map<std::string, std::string> g_keyBindings;
std::mutex g_pressedKeysMutex;
std::unordered_set<std::string> g_pressedKeys;
SDLInputManager g_sdlInput;
std::atomic<uint64_t> g_serverLastReplyUs{0};

void sync_sdl_input_options() {
    g_sdlInput.set_motion_enabled(g_gyroEnabled.load());
    g_sdlInput.set_home_shortcut_enabled(g_homeShortcutEnabled.load());
    g_sdlInput.set_capture_shortcut_enabled(g_captureShortcutEnabled.load());
}


std::string path_join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    char sep =
#ifdef _WIN32
        '\\';
#else
        '/';
#endif
    if (a.back() == '/' || a.back() == '\\') return a + b;
    return a + sep + b;
}

std::string dirname_of(std::string path) {
    size_t slash = path.find_last_of("\\/");
    if (slash == std::string::npos) return ".";
    return path.substr(0, slash);
}

std::string executable_dir() {
#ifdef _WIN32
    char buf[MAX_PATH]{};
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return dirname_of(buf);
    return ".";
#elif defined(__APPLE__)
    char buf[PATH_MAX]{};
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) return dirname_of(buf);
    std::vector<char> big(size + 1);
    if (_NSGetExecutablePath(big.data(), &size) == 0) {
        big[size] = '\0';
        return dirname_of(big.data());
    }
    return ".";
#else
    char buf[PATH_MAX]{};
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) { buf[n] = '\0'; return dirname_of(buf); }
    return ".";
#endif
}

void make_dir_if_needed(const std::string& dir) {
#ifdef _WIN32
    CreateDirectoryA(dir.c_str(), nullptr);
#else
    mkdir(dir.c_str(), 0755);
#endif
}

std::string user_config_dir() {
#ifdef _WIN32
    char appdata[MAX_PATH]{};
    DWORD n = GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH);
    std::string dir = (n > 0 && n < MAX_PATH) ? path_join(appdata, "NSPCControl") : "NSPCControl";
    make_dir_if_needed(dir);
    return dir;
#elif defined(__APPLE__)
    const char* home = std::getenv("HOME");
    std::string base = home ? path_join(home, "Library/Application Support") : ".";
    std::string dir = path_join(base, "NSPCControl");
    make_dir_if_needed(dir);
    return dir;
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    std::string base = xdg && *xdg ? xdg : (home ? path_join(home, ".config") : ".");
    std::string dir = path_join(base, "NSPCControl");
    make_dir_if_needed(dir);
    return dir;
#endif
}

std::string settings_path() { return path_join(user_config_dir(), "settings.ini"); }
std::string bindings_path() { return path_join(user_config_dir(), "bindings.ini"); }
std::string macros_path() { return path_join(user_config_dir(), "macros.json"); }

std::unordered_map<std::string, std::string> read_kv_file(const std::string& path) {
    std::unordered_map<std::string, std::string> out;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        out[ns::macro::trim(line.substr(0, eq))] = ns::macro::trim(line.substr(eq + 1));
    }
    return out;
}

bool write_kv_file(const std::string& path, const std::unordered_map<std::string, std::string>& values) {
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    for (const auto& kv : values) f << kv.first << "=" << kv.second << "\n";
    return (bool)f;
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
    g_keyBindings = default_key_bindings();
    auto kv = read_kv_file(bindings_path());
    for (auto& it : g_keyBindings) {
        auto found = kv.find(it.first);
        if (found != kv.end()) it.second = normalize_key_name(found->second);
    }
}

void save_bindings() {
    write_kv_file(bindings_path(), g_keyBindings);
}

std::string load_saved_ip() {
    auto kv = read_kv_file(settings_path());
    auto it = kv.find("LastIP");
    if (it != kv.end() && !it->second.empty()) return it->second;
    return "192.168.1.100";
}

void save_last_ip(const std::string& ip) {
    auto kv = read_kv_file(settings_path());
    kv["LastIP"] = ip;
    write_kv_file(settings_path(), kv);
}

int load_saved_keyboard_mode() {
    auto kv = read_kv_file(settings_path());
    auto it = kv.find("KeyboardMode");
    if (it == kv.end()) return KB_OFF;
    int mode = std::atoi(it->second.c_str());
    return (mode >= KB_OFF && mode <= KB_OVERRIDE) ? mode : KB_OFF;
}

void save_keyboard_mode(int mode) {
    auto kv = read_kv_file(settings_path());
    kv["KeyboardMode"] = std::to_string(mode);
    write_kv_file(settings_path(), kv);
}

bool parse_bool_setting(const std::unordered_map<std::string, std::string>& kv,
                               const std::string& key,
                               bool fallback) {
    auto it = kv.find(key);
    if (it == kv.end()) return fallback;
    std::string v = ns::macro::upper(ns::macro::trim(it->second));
    if (v == "1" || v == "TRUE" || v == "YES" || v == "ON") return true;
    if (v == "0" || v == "FALSE" || v == "NO" || v == "OFF") return false;
    return fallback;
}

void load_saved_feature_toggles() {
    auto kv = read_kv_file(settings_path());
    g_gyroEnabled.store(parse_bool_setting(kv, "GyroEnabled", true));
    g_rumbleEnabled.store(parse_bool_setting(kv, "RumbleEnabled", true));
    g_homeShortcutEnabled.store(parse_bool_setting(kv, "HomeShortcutEnabled", true));
    g_captureShortcutEnabled.store(parse_bool_setting(kv, "CaptureShortcutEnabled", true));
    sync_sdl_input_options();
}

void save_feature_toggles() {
    auto kv = read_kv_file(settings_path());
    kv["GyroEnabled"] = g_gyroEnabled.load() ? "1" : "0";
    kv["RumbleEnabled"] = g_rumbleEnabled.load() ? "1" : "0";
    kv["HomeShortcutEnabled"] = g_homeShortcutEnabled.load() ? "1" : "0";
    kv["CaptureShortcutEnabled"] = g_captureShortcutEnabled.load() ? "1" : "0";
    write_kv_file(settings_path(), kv);
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

bool key_is_down(const std::string& name_raw) {
    std::string name = normalize_key_name(name_raw);
    if (name.empty()) return false;
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
