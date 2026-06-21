#pragma once

#include "shared/macros.hpp"
#include "shared/protocol.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <vector>

using Clock = std::chrono::steady_clock;
using us = std::chrono::microseconds;
using ms = std::chrono::milliseconds;

extern std::atomic<bool> g_running;
extern bool g_verbose;

constexpr uint64_t CLIENT_TIMEOUT_US = 30'000'000ULL;
constexpr uint64_t CLIENT_STALE_NEUTRAL_US = 350'000ULL;
constexpr uint8_t RUMBLE_MIN_NONZERO = 1;
constexpr int RUMBLE_GAIN_PERCENT = 40;
constexpr uint32_t RUMBLE_BT_MIN_DURATION_MS = 40;
constexpr int HID_PORT_COUNT = 4;
constexpr int MAX_CLIENTS = 4;
constexpr uint64_t RATE_WINDOW_US = 1'000'000;
constexpr uint32_t RATE_MAX_PKT = 2000;
constexpr int RATE_TABLE = 32;
constexpr uint64_t SWITCH2_USB_ACTIVITY_FRESH_US = 2'000'000ULL;
constexpr uint64_t SWITCH2_WAKE_ADV_COOLDOWN_US = 8'000'000ULL;
constexpr int SWITCH2_WAKE_ADV_BURST_MS = 3000;

extern std::string g_usb_serial;
extern bool g_legacy_mode;
extern std::atomic<bool> g_gadget_setup_attempted;
extern bool g_switch2_wake_adv_enabled;
extern bool g_switch2_wakeup_setup_requested;
extern std::string g_switch2_wakeup_config_path;
extern std::string g_switch2_wake_mac;
extern std::string g_switch2_wake_adv_hex;
extern std::string g_switch2_wake_hci_dev;
extern bool g_switch2_wake_config_loaded;
extern std::atomic<bool> g_switch2_wake_adv_running;
extern std::atomic<uint64_t> g_switch2_last_wake_adv_us;
extern std::atomic<bool> g_switch2_usb_host_connected;
extern std::atomic<uint64_t> g_switch2_last_usb_activity_us;
extern std::atomic<bool> g_switch2_force_next_wake;
extern std::atomic<bool> g_switch2_delayed_wake_check_running;
extern uint8_t g_hmac_key[32];

struct RateSlot {
    uint32_t ip;
    uint32_t count;
    uint64_t window_start;
};
extern RateSlot g_rate_table[RATE_TABLE];

struct ClientSession {
    bool active = false;
    sockaddr_in addr{};
    uint64_t last_rx_us = 0;
    uint32_t expected_seq = 0;
    bool first_pkt = true;
    ns::ExtendedMultiReport report{};
    ns::MotionReport motion_samples[4][3]{};
    bool has_motion[4]{};
    uint64_t motion_last_collect_us[4]{};
    ns::RumblePacket rumble[4]{};
    ns::PrecisionRumblePacket precision_rumble[4]{};
    uint32_t rumble_seq[4]{};
    bool rumble_active[4]{};
    bool udp_rumble_enabled = false;
    uint32_t udp_last_rumble_seq[4]{};
    bool pad_present[4]{};
    uint64_t pad_last_present_us[4]{};
    bool uses_pad_presence = false;
};

extern std::mutex g_mtx[MAX_CLIENTS];
extern ClientSession g_clients[MAX_CLIENTS];
extern std::atomic<uint64_t> g_pkts_rx;
extern std::atomic<uint64_t> g_hid_writes;

struct ServerMacroRuntime {
    std::vector<ns::macro::Step> steps;
    bool running = false;
    uint64_t start_us = 0;
};
extern std::mutex g_server_macro_mtx;
extern ServerMacroRuntime g_server_macros[MAX_CLIENTS][4];

struct ServerMacroUploadRuntime {
    bool active = false;
    sockaddr_in sender{};
    uint32_t upload_id = 0;
    uint8_t subpad = 0;
    uint32_t total_len = 0;
    uint32_t chunk_count = 0;
    uint32_t received_count = 0;
    uint64_t last_rx_us = 0;
    std::vector<std::string> chunks;
    std::vector<uint8_t> got;
};
extern std::mutex g_server_macro_upload_mtx;
extern ServerMacroUploadRuntime g_server_macro_uploads[MAX_CLIENTS];

uint64_t elapsed_us_saturated(uint64_t now, uint64_t then);
bool elapsed_us_over(uint64_t now, uint64_t then, uint64_t limit);
void mark_switch2_usb_activity(uint64_t now = 0);
void clear_switch2_usb_activity();
void mark_switch2_usb_host_disconnected();
void rearm_switch2_wake_after_client_disconnect();
bool switch2_usb_host_recently_active(uint64_t now);
bool any_recent_client_active(uint64_t now);
void repair_future_client_timestamp(ClientSession& c, uint64_t now);
void clear_motion(ClientSession& c, int subpad);
void clear_all_motion(ClientSession& c);
void set_motion(ClientSession& c, int subpad, const ns::MotionReport& motion);
void set_motion_samples(ClientSession& c, int subpad, const ns::MotionReport samples[3]);

bool rate_allow(uint32_t ip);
int server_macro_client_for_sender(const sockaddr_in& sender);
bool server_macro_handle_chunk_packet(const uint8_t* data, size_t bytes, const sockaddr_in& sender);
bool server_macro_handle_ws_chunk_packet(int client_idx, const uint8_t* data, size_t bytes);
bool server_macro_running(int client_idx, int subpad);
void server_macro_apply(int client_idx, int subpad, ns::HIDReport& live);
bool server_macro_start(int client_idx, int subpad, const std::string& json_or_commands);
void server_macro_stop_all_for_client(int client_idx);
