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
extern std::atomic<bool> g_homeShortcutEnabled;
extern std::atomic<bool> g_captureShortcutEnabled;
extern std::unordered_map<std::string, std::string> g_keyBindings;
extern std::mutex g_pressedKeysMutex;
extern std::unordered_set<std::string> g_pressedKeys;
extern SDLInputManager g_sdlInput;
extern std::atomic<uint64_t> g_serverLastReplyUs;

void sync_sdl_input_options();
std::string macros_path();
std::vector<std::pair<std::string, std::string>> binding_keys();
std::unordered_map<std::string, std::string> default_key_bindings();
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
bool key_is_down(const std::string& name_raw);
void apply_keyboard_to_report(ns::HIDReport& rep, bool override_mode);
