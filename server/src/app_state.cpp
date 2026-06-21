#include "app_state.hpp"
#include "gadget_wakeup.hpp"
#include "shared/sha256.h"

#include <print>
#include <string>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <span>
#include <algorithm>

using namespace ns;

ServerContext create_default_ctx() {
    ServerContext ctx;
    ctx.usb_serial = "NSBRIDGE000001";
    ctx.switch2_wakeup_config_path = "/etc/ns-pc-control/switch2_wakeup.conf";
    ctx.switch2_wake_hci_dev = "hci0";
    return ctx;
}
ServerContext g_ctx = create_default_ctx();

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
    g_ctx.switch2_last_usb_activity_us.store(now, std::memory_order_relaxed);
    g_ctx.switch2_usb_host_connected.store(true, std::memory_order_relaxed);
}

void clear_switch2_usb_activity() {
    g_ctx.switch2_usb_host_connected.store(false, std::memory_order_relaxed);
    g_ctx.switch2_last_usb_activity_us.store(0, std::memory_order_relaxed);
}

void mark_switch2_usb_host_disconnected() {
    clear_switch2_usb_activity();
    g_ctx.switch2_force_next_wake.store(true, std::memory_order_relaxed);
    g_ctx.switch2_suspend_disconnect_seq.fetch_add(1, std::memory_order_relaxed);
}

bool switch2_usb_host_recently_active(uint64_t now) {
    const uint64_t last = g_ctx.switch2_last_usb_activity_us.load(std::memory_order_relaxed);
    if (last == 0 || elapsed_us_saturated(now, last) > SWITCH2_USB_ACTIVITY_FRESH_US) {
        clear_switch2_usb_activity();
        return false;
    }
    g_ctx.switch2_usb_host_connected.store(true, std::memory_order_relaxed);
    return true;
}

void rearm_switch2_wake_after_client_disconnect() {
    if (!switch2_usb_host_recently_active(now_us()))
        g_ctx.switch2_force_next_wake.store(true, std::memory_order_relaxed);
}

bool any_recent_client_active(uint64_t now) {
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        std::lock_guard<std::mutex> lk(g_ctx.mtx[i]);
        const ClientSession& c = g_ctx.clients[i];
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

// ── Server-side macro playback ───────────────────────────────────────────────────────────
// Shared parser/export/playback helpers live in include/shared/macros.hpp.
// This file keeps only server-specific runtime/upload wiring.
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
        std::lock_guard<std::mutex> lk(g_ctx.mtx[i]);
        if (g_ctx.clients[i].active && g_ctx.clients[i].addr.sin_addr.s_addr == src_ip && g_ctx.clients[i].addr.sin_port == sender.sin_port) { client_idx = i; break; }
    }
    if (client_idx == -1) {
        for (int i = 0; i < MAX_CLIENTS; ++i) {
            std::lock_guard<std::mutex> lk(g_ctx.mtx[i]);
            repair_future_client_timestamp(g_ctx.clients[i], now);
            if (!g_ctx.clients[i].active || elapsed_us_over(now, g_ctx.clients[i].last_rx_us, CLIENT_TIMEOUT_US)) {
                client_idx = i;
                g_ctx.clients[i].active = true;
                g_ctx.clients[i].addr = sender;
                g_ctx.clients[i].first_pkt = true;
                g_ctx.clients[i].expected_seq = 0;
                g_ctx.clients[i].report.reset();
                clear_all_motion(g_ctx.clients[i]);
                g_ctx.clients[i].last_rx_us = now;
                break;
            }
        }
    }
    if (client_idx >= 0) {
        std::lock_guard<std::mutex> lk(g_ctx.mtx[client_idx]);
        g_ctx.clients[client_idx].active = true;
        g_ctx.clients[client_idx].addr = sender;
        g_ctx.clients[client_idx].last_rx_us = now;
    }
    // Macro upload/start packets intentionally do not trigger Switch 2 wake.
    // Wake is reserved for real controller/input connection edges.
    return client_idx;
}

bool server_macro_handle_chunk_packet(std::span<const uint8_t> data, const sockaddr_in& sender) {
    if (data.size() < ns::macro::CHUNK_HEADER_SIZE + HMAC_TAG_SIZE) return false;
    ns::macro::MacroUdpChunkHeaderWire h{};
    std::ranges::copy(data.subspan(0, sizeof(h)), reinterpret_cast<uint8_t*>(&h));
    if (h.magic != ns::macro::UDP_CHUNK_MAGIC) return false;
    if (h.version != PROTO_VERSION) { if (g_ctx.verbose) std::println("bad macro chunk version, dropped"); return true; }
    if (h.total_len > ns::macro::UDP_TEXT_MAX) { if (g_ctx.verbose) std::println("macro chunk total over 50MB, dropped"); return true; }
    if (h.chunk_len > ns::macro::UDP_CHUNK_MAX) { if (g_ctx.verbose) std::println("macro chunk too large, dropped"); return true; }
    if (h.chunk_count == 0 || h.chunk_index >= h.chunk_count) { if (g_ctx.verbose) std::println("bad macro chunk index/count, dropped"); return true; }
    if (data.size() != ns::macro::CHUNK_HEADER_SIZE + (size_t)h.chunk_len + HMAC_TAG_SIZE) { if (g_ctx.verbose) std::println("bad macro chunk packet size, dropped"); return true; }
    auto recv_hmac = data.subspan(ns::macro::CHUNK_HEADER_SIZE + h.chunk_len, HMAC_TAG_SIZE);
    if (hmac_verify(std::span<const uint8_t>(g_ctx.hmac_key, 32), data.subspan(0, ns::macro::CHUNK_HEADER_SIZE + h.chunk_len), recv_hmac) != 0) { if (g_ctx.verbose) std::println("bad macro chunk HMAC, dropped"); return true; }
    if (!rate_allow(sender.sin_addr.s_addr)) return true;

    uint64_t now = now_us();
    int client_idx = server_macro_client_for_sender(sender);
    if (client_idx < 0) return true;

    std::string completed;
    uint8_t completed_subpad = h.subpad < 4 ? h.subpad : 0;
    {
        std::lock_guard<std::mutex> lk(g_ctx.server_macro_upload_mtx);
        ServerMacroUploadRuntime& up = g_ctx.server_macro_uploads[client_idx];
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
                if (g_ctx.verbose) std::println("macro chunk allocation failed");
                return true;
            }
        }
        if (up.total_len != h.total_len || up.chunk_count != h.chunk_count) { if (g_ctx.verbose) std::println("macro chunk metadata mismatch, dropped"); return true; }
        up.last_rx_us = now;
        if (!up.got[h.chunk_index]) {
            up.chunks[h.chunk_index].assign(reinterpret_cast<const char*>(data.data() + ns::macro::CHUNK_HEADER_SIZE), h.chunk_len);
            up.got[h.chunk_index] = 1;
            up.received_count++;
        }
        if (up.received_count == up.chunk_count) {
            size_t total = 0;
            for (const auto& c : up.chunks) total += c.size();
            if (total != up.total_len) { if (g_ctx.verbose) std::println("macro chunk final size mismatch"); up = ServerMacroUploadRuntime{}; return true; }
            completed.reserve(total);
            for (const auto& c : up.chunks) completed += c;
            completed_subpad = up.subpad;
            up = ServerMacroUploadRuntime{};
        }
    }
    if (!completed.empty()) {
        if (g_ctx.verbose) std::println("[macro] received chunked macro {} bytes", completed.size());
        server_macro_start(client_idx, completed_subpad, completed);
    }
    return true;
}


bool server_macro_handle_ws_chunk_packet(int client_idx, std::span<const uint8_t> data) {
    if (client_idx < 0 || client_idx >= MAX_CLIENTS) return false;
    if (data.size() < ns::macro::CHUNK_HEADER_SIZE) return false;
    ns::macro::MacroUdpChunkHeaderWire h{};
    std::ranges::copy(data.subspan(0, sizeof(h)), reinterpret_cast<uint8_t*>(&h));
    if (h.magic != ns::macro::UDP_CHUNK_MAGIC) return false;
    if (h.version != PROTO_VERSION && h.version != WEB_PROTO_VERSION) return true;
    if (h.total_len > ns::macro::UDP_TEXT_MAX || h.chunk_len > ns::macro::UDP_CHUNK_MAX || h.chunk_count == 0 || h.chunk_index >= h.chunk_count) return true;
    if (data.size() != ns::macro::CHUNK_HEADER_SIZE + (size_t)h.chunk_len) return true;
    uint64_t now = now_us();
    std::string completed;
    uint8_t completed_subpad = h.subpad < 4 ? h.subpad : 0;
    {
        std::lock_guard<std::mutex> lk(g_ctx.server_macro_upload_mtx);
        ServerMacroUploadRuntime& up = g_ctx.server_macro_uploads[client_idx];
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
            up.chunks[h.chunk_index].assign(reinterpret_cast<const char*>(data.data() + ns::macro::CHUNK_HEADER_SIZE), h.chunk_len);
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
    std::lock_guard<std::mutex> lk(g_ctx.server_macro_mtx);
    ServerMacroRuntime& rt = g_ctx.server_macros[client_idx][subpad];
    if (!rt.running) return false;
    uint64_t elapsed_ms = (now_us() - rt.start_us) / 1000ULL;
    if (elapsed_ms > ns::macro::total_ms(rt.steps) + 120) { rt.running = false; return false; }
    return true;
}

void server_macro_apply(int client_idx, int subpad, HIDReport& live) {
    if (client_idx < 0 || client_idx >= MAX_CLIENTS || subpad < 0 || subpad >= 4) return;
    std::lock_guard<std::mutex> lk(g_ctx.server_macro_mtx);
    ServerMacroRuntime& rt = g_ctx.server_macros[client_idx][subpad];
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
        if (g_ctx.verbose) std::println("[macro] rejected: {}", ns::macro::last_error());
        return false;
    }
    std::lock_guard<std::mutex> lk(g_ctx.server_macro_mtx);
    ServerMacroRuntime& rt = g_ctx.server_macros[client_idx][subpad];
    rt.steps = std::move(steps);
    rt.running = true;
    rt.start_us = now_us();
    if (g_ctx.verbose) std::println("[macro] started server macro slot={} pad={}", client_idx + 1, subpad + 1);
    return true;
}

[[maybe_unused]] void server_macro_stop_all_for_client(int client_idx) {
    if (client_idx < 0 || client_idx >= MAX_CLIENTS) return;
    std::lock_guard<std::mutex> lk(g_ctx.server_macro_mtx);
    for (int s = 0; s < 4; ++s) g_ctx.server_macros[client_idx][s].running = false;
}
