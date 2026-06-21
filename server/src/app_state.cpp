#include "app_state.hpp"
#include "gadget_wakeup.hpp"
#include "shared/sha256.h"

#include <print>
#include <string>
#include <cstring>
#include <stdexcept>
#include <utility>

using namespace ns;

// ── Global flags ──────────────────────────────────────────────────────────────
std::atomic<bool> g_running{true};
bool g_verbose = false;

// Normal-rumble build: decode console rumble into classic low/high packets only.
std::string g_usb_serial = "NSBRIDGE000001";
bool g_legacy_mode = false;

// Built-in USB gadget lifecycle.  ns-backend can now create/bind the
// USB gamepad gadget itself on startup and unbind/remove it
// on shutdown, so setup_gadget.sh is no longer needed at runtime.
std::atomic<bool> g_gadget_setup_attempted{false};

// Experimental Switch 2 wake helper. When at least one client is connected
// but the USB HID host disappears/looks asleep, briefly advertise the same
// Nintendo manufacturer payload observed from a Joy-Con 2 HOME wake attempt.
// This is only the BLE advertisement layer; if the console requires a full
// bonded Joy-Con 2 GATT session, the advert alone may not be enough.
bool g_switch2_wake_adv_enabled = false;
bool g_switch2_wakeup_setup_requested = false;
std::string g_switch2_wakeup_config_path = "/etc/ns-pc-control/switch2_wakeup.conf";
std::string g_switch2_wake_mac;
std::string g_switch2_wake_adv_hex;
std::string g_switch2_wake_hci_dev = "hci0";
bool g_switch2_wake_config_loaded = false;
std::atomic<bool> g_switch2_wake_adv_running{false};
std::atomic<uint64_t> g_switch2_last_wake_adv_us{0};
std::atomic<bool> g_switch2_usb_host_connected{false};
std::atomic<uint64_t> g_switch2_last_usb_activity_us{0};
std::atomic<bool> g_switch2_force_next_wake{false};
std::atomic<bool> g_switch2_delayed_wake_check_running{false};
std::atomic<uint64_t> g_switch2_suspend_disconnect_seq{0};

// HMAC authentication (key derived from DEFAULT_SECRET at startup)
uint8_t  g_hmac_key[32];

RateSlot g_rate_table[RATE_TABLE];

std::mutex    g_mtx[MAX_CLIENTS];
ClientSession g_clients[MAX_CLIENTS];

uint64_t elapsed_us_saturated(uint64_t now, uint64_t then) {
    // All runtime timestamps should come from ns::now_us()/steady_clock, but
    // never let a bad/future timestamp wrap an unsigned subtraction into
    // ~UINT64_MAX.  That was causing bogus logs like:
    //   timed out after 18446744073709.6s
    if (then == 0 || then > now) return 0;
    return now - then;
}

bool elapsed_us_over(uint64_t now, uint64_t then, uint64_t limit) {
    return then != 0 && elapsed_us_saturated(now, then) > limit;
}

void mark_switch2_usb_activity(uint64_t now) {
    if (now == 0) now = now_us();
    g_switch2_last_usb_activity_us.store(now, std::memory_order_relaxed);
    g_switch2_usb_host_connected.store(true, std::memory_order_relaxed);
}

void clear_switch2_usb_activity() {
    g_switch2_usb_host_connected.store(false, std::memory_order_relaxed);
    g_switch2_last_usb_activity_us.store(0, std::memory_order_relaxed);
}

void mark_switch2_usb_host_disconnected() {
    clear_switch2_usb_activity();
    g_switch2_force_next_wake.store(true, std::memory_order_relaxed);
    g_switch2_suspend_disconnect_seq.fetch_add(1, std::memory_order_relaxed);
}

bool switch2_usb_host_recently_active(uint64_t now) {
    const uint64_t last = g_switch2_last_usb_activity_us.load(std::memory_order_relaxed);
    if (last == 0 || elapsed_us_saturated(now, last) > SWITCH2_USB_ACTIVITY_FRESH_US) {
        clear_switch2_usb_activity();
        return false;
    }
    g_switch2_usb_host_connected.store(true, std::memory_order_relaxed);
    return true;
}

void rearm_switch2_wake_after_client_disconnect() {
    if (!switch2_usb_host_recently_active(now_us()))
        g_switch2_force_next_wake.store(true, std::memory_order_relaxed);
}

bool any_recent_client_active(uint64_t now) {
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        std::lock_guard<std::mutex> lk(g_mtx[i]);
        const ClientSession& c = g_clients[i];
        if (c.active && c.last_rx_us != 0 &&
            elapsed_us_saturated(now, c.last_rx_us) <= CLIENT_TIMEOUT_US) {
            return true;
        }
    }
    return false;
}

void repair_future_client_timestamp(ClientSession& c, uint64_t now) {
    if (c.active && (c.last_rx_us == 0 || c.last_rx_us > now)) {
        c.last_rx_us = now;
    }
}

void clear_motion(ClientSession& c, int subpad) {
    if (subpad < 0 || subpad >= 4) return;
    for (int i = 0; i < 3; ++i) c.motion_samples[subpad][i].reset();
    c.has_motion[subpad] = false;
    c.motion_last_collect_us[subpad] = 0;
}

void clear_all_motion(ClientSession& c) {
    for (int s = 0; s < 4; ++s) clear_motion(c, s);
}

void set_motion(ClientSession& c, int subpad, const MotionReport& motion) {
    if (subpad < 0 || subpad >= 4) return;

    uint64_t now = now_us();
    if (!c.has_motion[subpad]) {
        for (int i = 0; i < 3; ++i) c.motion_samples[subpad][i] = motion;
    } else if (elapsed_us_saturated(now, c.motion_last_collect_us[subpad]) > 5000ULL) {
        c.motion_samples[subpad][0] = c.motion_samples[subpad][1];
        c.motion_samples[subpad][1] = c.motion_samples[subpad][2];
    }
    c.motion_samples[subpad][2] = motion;
    c.has_motion[subpad] = true;
    c.motion_last_collect_us[subpad] = now;
}

void set_motion_samples(ClientSession& c, int subpad, const MotionReport samples[3]) {
    if (subpad < 0 || subpad >= 4 || !samples) return;
    c.motion_samples[subpad][0] = samples[0];
    c.motion_samples[subpad][1] = samples[1];
    c.motion_samples[subpad][2] = samples[2];
    c.has_motion[subpad] = true;
    c.motion_last_collect_us[subpad] = now_us();
}

// Diagnostics
std::atomic<uint64_t> g_pkts_rx{0};
std::atomic<uint64_t> g_hid_writes{0};


// ── Server-side macro playback ───────────────────────────────────────────────────────────
// Shared parser/export/playback helpers live in include/shared/macros.hpp.
// This file keeps only server-specific runtime/upload wiring.
std::mutex g_server_macro_mtx;
ServerMacroRuntime g_server_macros[MAX_CLIENTS][4];

std::mutex g_server_macro_upload_mtx;
ServerMacroUploadRuntime g_server_macro_uploads[MAX_CLIENTS];

bool rate_allow(uint32_t ip);
bool server_macro_start(int client_idx, int subpad, const std::string& json_or_commands);
void maybe_send_switch2_wake_advert(const char* reason);
int run_switch2_wakeup_setup();
bool load_switch2_wakeup_config(bool quiet_if_missing);

int server_macro_client_for_sender(const sockaddr_in& sender) {
    uint32_t src_ip = sender.sin_addr.s_addr;
    uint64_t now = now_us();
    int client_idx = -1;
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        std::lock_guard<std::mutex> lk(g_mtx[i]);
        if (g_clients[i].active && g_clients[i].addr.sin_addr.s_addr == src_ip && g_clients[i].addr.sin_port == sender.sin_port) { client_idx = i; break; }
    }
    if (client_idx == -1) {
        for (int i = 0; i < MAX_CLIENTS; ++i) {
            std::lock_guard<std::mutex> lk(g_mtx[i]);
            repair_future_client_timestamp(g_clients[i], now);
            if (!g_clients[i].active || elapsed_us_over(now, g_clients[i].last_rx_us, CLIENT_TIMEOUT_US)) {
                client_idx = i;
                g_clients[i].active = true;
                g_clients[i].addr = sender;
                g_clients[i].first_pkt = true;
                g_clients[i].expected_seq = 0;
                g_clients[i].report.reset();
                clear_all_motion(g_clients[i]);
                g_clients[i].last_rx_us = now;
                break;
            }
        }
    }
    if (client_idx >= 0) {
        std::lock_guard<std::mutex> lk(g_mtx[client_idx]);
        g_clients[client_idx].active = true;
        g_clients[client_idx].addr = sender;
        g_clients[client_idx].last_rx_us = now;
    }
    // Macro upload/start packets intentionally do not trigger Switch 2 wake.
    // Wake is reserved for real controller/input connection edges.
    return client_idx;
}

bool server_macro_handle_chunk_packet(const uint8_t* data, size_t bytes, const sockaddr_in& sender) {
    if (bytes < ns::macro::CHUNK_HEADER_SIZE + HMAC_TAG_SIZE) return false;
    ns::macro::MacroUdpChunkHeaderWire h{};
    memcpy(&h, data, sizeof(h));
    if (h.magic != ns::macro::UDP_CHUNK_MAGIC) return false;
    if (h.version != PROTO_VERSION) { if (g_verbose) std::println("bad macro chunk version, dropped"); return true; }
    if (h.total_len > ns::macro::UDP_TEXT_MAX) { if (g_verbose) std::println("macro chunk total over 50MB, dropped"); return true; }
    if (h.chunk_len > ns::macro::UDP_CHUNK_MAX) { if (g_verbose) std::println("macro chunk too large, dropped"); return true; }
    if (h.chunk_count == 0 || h.chunk_index >= h.chunk_count) { if (g_verbose) std::println("bad macro chunk index/count, dropped"); return true; }
    if (bytes != ns::macro::CHUNK_HEADER_SIZE + (size_t)h.chunk_len + HMAC_TAG_SIZE) { if (g_verbose) std::println("bad macro chunk packet size, dropped"); return true; }
    const uint8_t* recv_hmac = data + ns::macro::CHUNK_HEADER_SIZE + h.chunk_len;
    if (hmac_verify(std::span<const uint8_t>(g_hmac_key, 32), std::span<const uint8_t>(data, ns::macro::CHUNK_HEADER_SIZE + h.chunk_len), std::span<const uint8_t>(recv_hmac, HMAC_TAG_SIZE)) != 0) { if (g_verbose) std::println("bad macro chunk HMAC, dropped"); return true; }
    if (!rate_allow(sender.sin_addr.s_addr)) return true;

    uint64_t now = now_us();
    int client_idx = server_macro_client_for_sender(sender);
    if (client_idx < 0) return true;

    std::string completed;
    uint8_t completed_subpad = h.subpad < 4 ? h.subpad : 0;
    {
        std::lock_guard<std::mutex> lk(g_server_macro_upload_mtx);
        ServerMacroUploadRuntime& up = g_server_macro_uploads[client_idx];
        bool same = up.active && up.upload_id == h.upload_id &&
                    up.sender.sin_addr.s_addr == sender.sin_addr.s_addr &&
                    up.sender.sin_port == sender.sin_port;
        if (!same) {
            up = ServerMacroUploadRuntime{};
            up.active = true;
            up.sender = sender;
            up.upload_id = h.upload_id;
            up.subpad = h.subpad < 4 ? h.subpad : 0;
            up.total_len = h.total_len;
            up.chunk_count = h.chunk_count;
            try {
                up.chunks.assign(h.chunk_count, std::string());
                up.got.assign(h.chunk_count, 0);
            } catch (...) {
                up = ServerMacroUploadRuntime{};
                if (g_verbose) std::println("macro chunk allocation failed");
                return true;
            }
        }
        if (up.total_len != h.total_len || up.chunk_count != h.chunk_count) { if (g_verbose) std::println("macro chunk metadata mismatch, dropped"); return true; }
        up.last_rx_us = now;
        if (!up.got[h.chunk_index]) {
            up.chunks[h.chunk_index].assign(reinterpret_cast<const char*>(data + ns::macro::CHUNK_HEADER_SIZE), h.chunk_len);
            up.got[h.chunk_index] = 1;
            up.received_count++;
        }
        if (up.received_count == up.chunk_count) {
            size_t total = 0;
            for (const auto& c : up.chunks) total += c.size();
            if (total != up.total_len) { if (g_verbose) std::println("macro chunk final size mismatch"); up = ServerMacroUploadRuntime{}; return true; }
            completed.reserve(total);
            for (const auto& c : up.chunks) completed += c;
            completed_subpad = up.subpad;
            up = ServerMacroUploadRuntime{};
        }
    }
    if (!completed.empty()) {
        if (g_verbose) std::println("[macro] received chunked macro {} bytes", completed.size());
        server_macro_start(client_idx, completed_subpad, completed);
    }
    return true;
}


bool server_macro_handle_ws_chunk_packet(int client_idx, const uint8_t* data, size_t bytes) {
    if (client_idx < 0 || client_idx >= MAX_CLIENTS) return false;
    if (bytes < ns::macro::CHUNK_HEADER_SIZE) return false;
    ns::macro::MacroUdpChunkHeaderWire h{};
    memcpy(&h, data, sizeof(h));
    if (h.magic != ns::macro::UDP_CHUNK_MAGIC) return false;
    if (h.version != PROTO_VERSION && h.version != WEB_PROTO_VERSION) return true;
    if (h.total_len > ns::macro::UDP_TEXT_MAX || h.chunk_len > ns::macro::UDP_CHUNK_MAX || h.chunk_count == 0 || h.chunk_index >= h.chunk_count) return true;
    if (bytes != ns::macro::CHUNK_HEADER_SIZE + (size_t)h.chunk_len) return true;
    uint64_t now = now_us();
    std::string completed;
    uint8_t completed_subpad = h.subpad < 4 ? h.subpad : 0;
    {
        std::lock_guard<std::mutex> lk(g_server_macro_upload_mtx);
        ServerMacroUploadRuntime& up = g_server_macro_uploads[client_idx];
        bool same = up.active && up.upload_id == h.upload_id;
        if (!same) {
            up = ServerMacroUploadRuntime{};
            up.active = true;
            up.upload_id = h.upload_id;
            up.subpad = h.subpad < 4 ? h.subpad : 0;
            up.total_len = h.total_len;
            up.chunk_count = h.chunk_count;
            try { up.chunks.assign(h.chunk_count, std::string()); up.got.assign(h.chunk_count, 0); }
            catch (...) { up = ServerMacroUploadRuntime{}; return true; }
        }
        if (up.total_len != h.total_len || up.chunk_count != h.chunk_count) return true;
        up.last_rx_us = now;
        if (!up.got[h.chunk_index]) {
            up.chunks[h.chunk_index].assign(reinterpret_cast<const char*>(data + ns::macro::CHUNK_HEADER_SIZE), h.chunk_len);
            up.got[h.chunk_index] = 1;
            up.received_count++;
        }
        if (up.received_count == up.chunk_count) {
            size_t total = 0; for (const auto& c : up.chunks) total += c.size();
            if (total != up.total_len) { up = ServerMacroUploadRuntime{}; return true; }
            completed.reserve(total); for (const auto& c : up.chunks) completed += c;
            completed_subpad = up.subpad;
            up = ServerMacroUploadRuntime{};
        }
    }
    if (!completed.empty()) server_macro_start(client_idx, completed_subpad, completed);
    return true;
}

bool server_macro_running(int client_idx, int subpad) {
    if (client_idx < 0 || client_idx >= MAX_CLIENTS || subpad < 0 || subpad >= 4) return false;
    std::lock_guard<std::mutex> lk(g_server_macro_mtx);
    ServerMacroRuntime& rt = g_server_macros[client_idx][subpad];
    if (!rt.running) return false;
    uint64_t elapsed_ms = (now_us() - rt.start_us) / 1000ULL;
    if (elapsed_ms > ns::macro::total_ms(rt.steps) + 120) { rt.running = false; return false; }
    return true;
}

void server_macro_apply(int client_idx, int subpad, HIDReport& live) {
    if (client_idx < 0 || client_idx >= MAX_CLIENTS || subpad < 0 || subpad >= 4) return;
    std::lock_guard<std::mutex> lk(g_server_macro_mtx);
    ServerMacroRuntime& rt = g_server_macros[client_idx][subpad];
    if (!rt.running) return;
    uint64_t elapsed_ms = (now_us() - rt.start_us) / 1000ULL;
    ns::macro::Step step{};
    if (!ns::macro::step_at(rt.steps, elapsed_ms, step)) {
        rt.running = false;
        return;
    }
    live.buttons |= step.buttons;
    if (step.hat != HAT_NEUTRAL && live.hat == HAT_NEUTRAL) live.hat = step.hat;
    if (step.has_lstick) { live.lx = step.lx; live.ly = step.ly; }
    if (step.has_rstick) { live.rx = step.rx; live.ry = step.ry; }
}

bool server_macro_start(int client_idx, int subpad, const std::string& json_or_commands) {
    if (client_idx < 0 || client_idx >= MAX_CLIENTS) return false;
    if (subpad < 0 || subpad >= 4) subpad = 0;
    std::vector<ns::macro::Step> steps;
    if (!ns::macro::validate_text(json_or_commands, steps, nullptr)) {
        if (g_verbose) std::println("[macro] rejected: {}", ns::macro::last_error());
        return false;
    }
    std::lock_guard<std::mutex> lk(g_server_macro_mtx);
    ServerMacroRuntime& rt = g_server_macros[client_idx][subpad];
    rt.steps = std::move(steps);
    rt.running = true;
    rt.start_us = now_us();
    if (g_verbose) std::println("[macro] started server macro slot={} pad={}", client_idx + 1, subpad + 1);
    return true;
}

[[maybe_unused]] void server_macro_stop_all_for_client(int client_idx) {
    if (client_idx < 0 || client_idx >= MAX_CLIENTS) return;
    std::lock_guard<std::mutex> lk(g_server_macro_mtx);
    for (int s = 0; s < 4; ++s) g_server_macros[client_idx][s].running = false;
}
