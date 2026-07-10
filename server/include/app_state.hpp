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
#include <span>

using Clock = std::chrono::steady_clock;
using us = std::chrono::microseconds;
using ms = std::chrono::milliseconds;


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
// Number of slots to probe when the primary hash slot holds a different IP.
constexpr int RATE_PROBE = 4;
// Switch activity is confirmed only by host-originated HID OUT/protocol RX.
// The Switch answers the Pro Controller protocol frequently while awake; when
// that RX stream stops after having been active, treat the console as suspended.
// Writes to /dev/hidg* are intentionally not used as wake/sleep evidence.
constexpr uint64_t SWITCH2_USB_ACTIVITY_FRESH_US = 1'500'000ULL;
// A few RX packets can appear while the Switch is entering sleep. Do not re-arm
// suspend disconnects until the Switch has answered continuously for this long.
constexpr uint64_t SWITCH2_USB_ACTIVITY_STABLE_US = 3'000'000ULL;
constexpr uint64_t SWITCH2_WAKE_ADV_COOLDOWN_US = 8'000'000ULL;
// While a wake advert has just been sent, UDP clients must be allowed to stay
// connected even though Switch RX has not resumed yet. Otherwise a desktop
// client can connect, trigger wake, receive "Switch asleep" on its next probe,
// and immediately disconnect during the wake window.
constexpr uint64_t SWITCH2_WAKE_CLIENT_GRACE_US = 30'000'000ULL;
constexpr int SWITCH2_WAKE_ADV_BURST_MS = 8000;
// Server/UI state is not gameplay-critical. Refresh at most 20 Hz so
// high-rate UDP input and USB HID writes stay the priority.
constexpr uint64_t SERVER_STATE_REFRESH_MIN_US = 50'000ULL;


struct RateSlot {
    uint32_t ip;
    uint32_t count;
    uint64_t window_start;
};

enum class InputSource : uint8_t {
    None = 0,
    Udp,
    WebSocket,
    Bluetooth,
};

const char* input_source_name(InputSource source);

enum class UsbControllerFamily : uint8_t {
    Switch1,
    Switch2,
    Hori,
};

const char* usb_controller_family_name(UsbControllerFamily family);

struct ControllerStatusState {
    uint8_t player_leds = 0;
    uint8_t player_index = ns::CONTROLLER_PLAYER_INDEX_UNKNOWN;
    uint8_t body_rgb[3]{};
    bool body_rgb_valid = false;
};

struct ClientAssignmentState {
    uint8_t console_port_mask = 0;
    uint8_t primary_console_port = ns::CONTROLLER_CONSOLE_PORT_NONE;
    uint8_t requested_type = ns::CONTROLLER_TYPE_DEFAULT;
    uint8_t virtual_type = ns::CONTROLLER_TYPE_DEFAULT;
};

struct ClientSession {
    bool active = false;
    InputSource source = InputSource::None;
    sockaddr_in addr{};
    uint64_t last_rx_us = 0;
    uint32_t expected_seq = 0;
    bool first_pkt = true;
    ns::MultiReport report{};
    bool has_new_report = false;
    ns::RumblePacket rumble[4]{};
    ns::PrecisionRumblePacket precision_rumble[4]{};
    uint32_t rumble_seq[4]{};
    bool rumble_active[4]{};
    ControllerStatusState controller_status[4]{};
    uint32_t controller_status_seq[4]{};
    ClientAssignmentState client_assignment[4]{};
    uint32_t client_assignment_seq[4]{};
    uint32_t udp_last_controller_status_seq[4]{};
    uint32_t udp_last_client_assignment_seq[4]{};
    uint64_t udp_last_server_state_seq = 0;
    bool udp_rumble_enabled = false;
    uint32_t udp_last_rumble_seq[4]{};
    bool pad_present[4]{};
    uint64_t pad_last_present_us[4]{};
    bool uses_pad_presence = false;
    ns::RosterEntry source_pads[4]{};
    uint64_t udp_last_roster_seq = 0;
    uint64_t udp_last_roster_send_us = 0;

    // Amiibo support
    bool amiibo_request_pending[4] = {};
    bool amiibo_writeback_pending[4] = {};
    uint16_t amiibo_writeback_len[4] = {};
    uint8_t amiibo_writeback_data[4][540] = {};
};


struct ServerMacroRuntime {
    std::vector<ns::macro::Step> steps;
    bool running = false;
    uint64_t start_us = 0;
};



struct ServerMacroUploadRuntime {
    bool active = false;
    sockaddr_in sender{};
    uint32_t upload_id = 0;
    uint8_t subpad = 0;
    ns::macro::UploadKind kind = ns::macro::UploadKind::Macro;
    uint32_t total_len = 0;
    uint32_t chunk_count = 0;
    uint32_t received_count = 0;
    uint64_t last_rx_us = 0;
    std::vector<std::string> chunks;
    std::vector<uint8_t> got;
};

struct ServerContext {
    std::atomic<bool> running{true};
    bool verbose = false;
    std::string usb_serial;
    UsbControllerFamily usb_controller_family = UsbControllerFamily::Switch1;
    // Native Switch 2 mode exposes three full Pro2 FunctionFS controllers.
    // Endpoint 7 is kept for one upstream-style Switch 1 f_hid fallback.
    int switch2_native_port_count = 3;
    // FunctionFS transport is used only for native S2 controllers.
    std::atomic<bool> functionfs_transport_active{false};
    bool bluetooth_input_disabled = false;
    bool bluetooth_disabled = false; // reserved: disables all Bluetooth stack access, including wake setup/runtime
    std::atomic<bool> gadget_setup_attempted{false};
    bool switch2_wake_adv_enabled = false;
    bool switch2_wakeup_setup_requested = false;
    std::string switch2_wakeup_config_path;
    std::string switch2_wake_mac;
    std::string switch2_wake_adv_hex;
    std::string switch2_wake_hci_dev;
    bool switch2_wake_config_loaded = false;
    std::atomic<bool> switch2_wake_adv_running{false};
    std::atomic<uint64_t> switch2_last_wake_adv_us{0};
    std::atomic<bool> switch2_usb_host_connected{false};
    std::atomic<uint64_t> switch2_last_usb_activity_us{0};
    std::atomic<uint64_t> switch2_rx_stream_since_us{0};
    std::atomic<bool> switch2_rx_stream_stable{false};
    std::atomic<bool> switch2_sleep_confirmed{false};
    std::atomic<uint64_t> switch2_sleep_seq{0};
    sockaddr_in switch2_dormant_udp_addrs[MAX_CLIENTS]{};
    bool switch2_dormant_udp_valid[MAX_CLIENTS]{};
    std::atomic<bool> switch2_force_next_wake{false}; // compatibility/no-op; runtime wake is RX-state based
    std::atomic<bool> switch2_delayed_wake_check_running{false};
    std::atomic<uint8_t> console_player_leds[HID_PORT_COUNT]{};
    uint8_t hmac_key[32]{0};
    RateSlot rate_table[RATE_TABLE]{};
    std::mutex mtx[MAX_CLIENTS];
    ClientSession clients[MAX_CLIENTS]{};
    std::atomic<uint64_t> pkts_rx{0};
    std::atomic<uint64_t> hid_writes{0};
    std::atomic<uint64_t> host_out_reports{0};
    std::atomic<uint32_t> server_state_packed{UINT32_MAX};
    std::atomic<uint64_t> server_state_seq{1};
    std::atomic<uint64_t> server_state_last_refresh_us{0};
    std::mutex roster_mtx;
    ns::RosterEntry roster[HID_PORT_COUNT]{};
    std::atomic<uint64_t> roster_seq{1};
    std::atomic<uint64_t> roster_last_refresh_us{0};
    std::mutex server_macro_mtx;
    ServerMacroRuntime server_macros[MAX_CLIENTS][4]{};
    std::mutex server_macro_upload_mtx;
    ServerMacroUploadRuntime server_macro_uploads[MAX_CLIENTS]{};
    struct lws_context* lws_context = nullptr;
};
extern ServerContext g_ctx;
uint64_t elapsed_us_saturated(uint64_t now, uint64_t then);
bool elapsed_us_over(uint64_t now, uint64_t then, uint64_t limit);
void mark_switch2_usb_activity(uint64_t now = 0);
void clear_switch2_usb_activity();
void mark_switch2_usb_host_disconnected();
void rearm_switch2_wake_after_client_disconnect();
bool switch2_usb_host_recently_active(uint64_t now);
bool switch2_sleep_confirmed(uint64_t now = 0);
bool switch2_wake_recent(uint64_t now = 0);
void poll_switch2_sleep_state(uint64_t now = 0);
void forget_switch2_dormant_udp_endpoint(const sockaddr_in& addr);
bool switch2_dormant_udp_endpoint_matches(const sockaddr_in& addr);
bool any_recent_client_active(uint64_t now);
int active_client_count(uint64_t now = 0);
int requested_virtual_slots_for_report(const ns::MultiReport& report, const bool pad_present[4], bool reserve_when_idle = true);
bool controller_profile_supported_by_usb_family(uint8_t profile);
UsbControllerFamily usb_family_for_profile(uint8_t profile);
uint8_t coerce_profile_to_family(uint8_t profile, UsbControllerFamily family);
int active_requested_virtual_slots(uint64_t now = 0, int ignore_client_idx = -1);
int free_virtual_slot_count(uint64_t now = 0, int ignore_client_idx = -1);
uint32_t pack_server_state(uint8_t active_clients, uint8_t free_virtual_slots, bool switch_asleep);
uint64_t refresh_server_state_seq(uint64_t now = 0, bool force = false);
uint8_t switch_player_index_from_leds(uint8_t player_leds);
void publish_controller_status_event(int client_idx, int sub_idx, uint8_t player_leds,
                                     const uint8_t* body_rgb = nullptr);
void publish_client_assignment_event(int client_idx, int sub_idx, uint8_t console_port_mask,
                                     uint8_t primary_console_port, uint8_t requested_type,
                                     uint8_t virtual_type);
void publish_amiibo_request(int client_idx, int sub_idx, bool requested);
void publish_amiibo_writeback(int client_idx, int sub_idx, const uint8_t* data, uint16_t len);
bool get_controller_status_packet(int client_idx, int sub_idx, uint32_t& seq, ns::ControllerStatusPacket& packet);
bool get_client_assignment_packet(int client_idx, int sub_idx, uint8_t active_clients,
                                  uint8_t free_virtual_slots, uint32_t& seq,
                                  ns::ClientAssignmentPacket& packet);
ns::ClientAssignmentPacket make_server_full_assignment_packet(uint8_t active_clients,
                                                              uint8_t free_virtual_slots,
                                                              bool switch_asleep = false);
ns::ClientAssignmentPacket make_server_profile_unsupported_assignment_packet(uint8_t active_clients,
                                                                              uint8_t free_virtual_slots,
                                                                              bool switch_asleep = false);
int console_port_for_client_subpad(int client_idx, int sub_idx);
bool client_subpad_for_console_port(int console_port, int& client_idx, int& sub_idx);
void store_client_source_names(int client_idx, const ns::ClientNamesPacket& packet);
uint64_t refresh_roster_seq(uint64_t now = 0, bool force = false);
uint64_t get_roster_packet(ns::RosterPacket& packet);
bool any_client_source_active(InputSource source, uint64_t now = 0);
void repair_future_client_timestamp(ClientSession& c, uint64_t now);
void clear_motion(ClientSession& c, int subpad);
void clear_all_motion(ClientSession& c);
void set_motion(ClientSession& c, int subpad, const ns::MotionReport& motion);
void set_motion_samples(ClientSession& c, int subpad, const ns::MotionReport samples[3]);
void reset_client_session_locked(ClientSession& c);
void reset_client_session(int client_idx);
bool reset_client_session_if_source(int client_idx, InputSource source);
bool client_session_is_source(int client_idx, InputSource source);
int allocate_client_session(uint64_t now, const sockaddr_in* addr, bool uses_pad_presence,
                            InputSource source, int preferred_client_idx = -1);
bool parse_client_packet(const uint8_t* data, size_t len,
                         uint8_t& flags, uint32_t& seq,
                         ns::MultiReport& report,
                         bool pad_present[4]);
bool hid_is_neutral(const ns::HIDReport& r);
bool multi_report_has_real_input(const ns::MultiReport& report, const bool pad_present[4], bool uses_pad_presence);
bool input_is_neutral(const ns::HoriHIDReport& r);
bool motion_is_neutral(const ns::MotionReport& m);




bool rate_allow(uint32_t ip);
int server_macro_client_for_sender(const sockaddr_in& sender);
bool server_macro_handle_chunk_packet(std::span<const uint8_t> data, const sockaddr_in& sender);
bool server_macro_handle_ws_chunk_packet(int client_idx, std::span<const uint8_t> data);

bool server_macro_running(int client_idx, int subpad);
void server_macro_apply(int client_idx, int subpad, ns::HoriHIDReport& live);
bool server_macro_start(int client_idx, int subpad, const std::string& json_or_commands);
void server_macro_stop_all_for_client(int client_idx);
