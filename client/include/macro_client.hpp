#pragma once

#include "platform.hpp"
#include "shared/macros.hpp"
#include "udp_protocol.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <expected>

extern uint32_t g_macro_udp_seq;
extern std::mutex g_macro_mtx;
extern std::vector<ns::macro::Step> g_macro_steps;
extern std::atomic<bool> g_macro_running;
extern std::atomic<bool> g_macro_recording;
extern uint64_t g_macro_start_us;
extern std::string g_macro_upload_pending;
extern std::vector<ns::macro::Entry> g_macro_entries;
extern std::unordered_map<std::string, bool> g_macro_hotkey_down;

uint32_t next_macro_upload_id();
bool send_macro_udp_packet(SOCKET sock, const sockaddr_in& dest, const uint8_t hmac_key[32],
                           const std::string& json_or_commands, uint8_t subpad = 0);
int find_macro_entry_by_name(const std::string& name);
std::string unique_macro_name(const std::string& base_raw);
bool macro_hotkey_conflicts(const std::string& hotkey, std::string* conflict_name = nullptr);
bool macro_entry_hotkey_conflicts(const std::string& hotkey, int skip_index, std::string* conflict_name = nullptr);
void rebuild_macro_hotkey_state();
void load_macro_entries();
bool save_macro_entries_to_disk();
std::expected<void, std::string> start_macro_text(const std::string& txt);
std::expected<void, std::string> upsert_macro_entry(ns::macro::Entry e, bool force_unique_name);
void poll_macro_entry_hotkeys();
void macro_record_start();
std::string macro_record_stop();
void macro_record_sample(const ns::HoriHIDReport& report);
bool poll_macro_record_p1(ns::HoriHIDReport& report);
void macro_record_sample_p1();
bool apply_macro_override(ns::HoriHIDReport logical_reports[4], bool present[4], bool has_motion[4]);
