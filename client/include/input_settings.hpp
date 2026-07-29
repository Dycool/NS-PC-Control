#pragma once

#include "platform.hpp"
#include "shared/protocol.hpp"
#include "shared/sdl_input.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

enum { KB_OFF = 0, KB_SINGLE = 1, KB_OVERRIDE = 2 };

extern std::atomic<int> g_keyboardMode;
extern std::atomic<bool> g_gyroEnabled;
extern std::atomic<bool> g_rumbleEnabled;
extern std::atomic<bool> g_switch2AudioEnabled;
extern std::atomic<bool> g_switch2MicrophoneEnabled;
extern std::atomic<bool> g_homeShortcutEnabled;
extern std::atomic<bool> g_captureShortcutEnabled;
extern std::atomic<bool> g_mouseModeEnabled;
extern std::atomic<bool> g_joyconMouseModeEnabled;
extern std::atomic<double> g_mouseSensitivity;
extern std::atomic<int> g_controllerType;
extern std::atomic<bool> g_joyconHorizontalMode;
extern std::atomic<bool> g_switch2ModeEnabled;
extern std::atomic<bool> g_switch2AudioSupported;

inline constexpr const char* S2_AUDIO_DEVICE_DEFAULT = "@default";
std::pair<std::string, std::string> switch2_audio_device_selections();
void set_switch2_audio_device_selections(std::string playback, std::string microphone);
extern std::atomic<bool> g_horiModeEnabled;
extern std::unordered_map<std::string, std::string> g_keyBindings;
extern std::mutex g_keyBindingsMutex;
extern std::unordered_map<std::string, std::string> g_controllerBindings;
extern std::mutex g_controllerBindingsMutex;
extern std::mutex g_pressedKeysMutex;
extern std::unordered_set<std::string> g_pressedKeys;
extern SDLInputManager g_sdlInput;
extern std::atomic<uint64_t> g_serverLastReplyUs;
extern std::atomic<bool> g_serverRequestedDisconnect;
extern std::atomic<bool> g_serverFullDisconnect;
extern std::atomic<bool> g_serverProfileUnsupportedDisconnect;
extern std::atomic<bool> g_serverProbeFull;

void sync_sdl_input_options();
std::string macros_path();
std::vector<std::pair<std::string, std::string>> binding_keys();
std::vector<std::pair<std::string, std::string>> s2_binding_keys();
std::unordered_map<std::string, std::string> default_key_bindings();
std::vector<std::pair<std::string, std::string>> controller_binding_keys();
std::unordered_map<std::string, std::string> default_controller_bindings();
std::string normalize_key_name(std::string s);
bool is_valid_key_code(const std::string& s);
std::string normalize_macro_hotkey_for_io(const std::string& s);
void load_saved_bindings();
void save_bindings();
std::string load_saved_ip();
void save_last_ip(const std::string& ip);
int load_saved_keyboard_mode();
void save_keyboard_mode(int mode);
bool parse_bool_setting(const std::unordered_map<std::string, std::string>& kv,
                        const std::string& key,
                        bool fallback);
void load_saved_feature_toggles();
void save_feature_toggles();
void set_key_pressed(const std::string& key, bool down);
bool pressed_key_cache_contains(const std::string& key);
void clear_pressed_key_cache();
void update_keyboard_state_cache();
bool key_is_down(const std::string& name_raw);
void apply_keyboard_to_report(ns::HoriHIDReport& rep, bool override_mode);
void apply_controller_bindings(ns::HoriHIDReport& rep);
void apply_joycon_horizontal_transform(ns::HoriHIDReport& rep, int controller_type);
void apply_joycon_horizontal_motion_transform(ns::MotionReport& m, int controller_type);

bool is_mouse_button_name(const std::string& name);
bool mouse_mode_active();
bool joycon_mouse_mode_supported();
bool joycon_mouse_mode_active();
bool mouse_capture_active();
void suspend_keyboard_mouse_input();
void resume_keyboard_mouse_input();
bool keyboard_mouse_input_suspended();
void clear_mouse_button_inputs();
