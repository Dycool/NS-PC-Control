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

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
ServerContext g_ctx{
    .usb_serial = "NSBRIDGE000001",
    .switch2_wakeup_config_path = "/etc/ns-pc-control/switch2_wakeup.conf",
    .switch2_wake_hci_dev = "hci0",
};
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

uint64_t elapsed_us_saturated(uint64_t now, uint64_t then) {
    return (then == 0 || then > now) ? 0 : now - then;
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
    if (g_ctx.switch2_last_usb_activity_us.load(std::memory_order_relaxed) == 0 ||
        elapsed_us_saturated(now, g_ctx.switch2_last_usb_activity_us.load(std::memory_order_relaxed)) > SWITCH2_USB_ACTIVITY_FRESH_US) {
        clear_switch2_usb_activity(); return false;
    }
    g_ctx.switch2_usb_host_connected.store(true, std::memory_order_relaxed);
    return true;
}

void rearm_switch2_wake_after_client_disconnect() {
    if (!switch2_usb_host_recently_active(now_us())) g_ctx.switch2_force_next_wake.store(true, std::memory_order_relaxed);
}

bool any_recent_client_active(uint64_t now) {
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        std::lock_guard<std::mutex> lk(g_ctx.mtx[i]);
        if (g_ctx.clients[i].active && g_ctx.clients[i].last_rx_us != 0 && elapsed_us_saturated(now, g_ctx.clients[i].last_rx_us) <= CLIENT_TIMEOUT_US) return true;
    }
    return false;
}

void repair_future_client_timestamp(ClientSession& c, uint64_t now) {
    if (c.active && (c.last_rx_us == 0 || c.last_rx_us > now)) c.last_rx_us = now;
}

void clear_motion(ClientSession& c, int subpad) {
    if (subpad < 0 || subpad >= 4) return;
    for (int i = 0; i < 3; ++i) c.motion_samples[subpad][i].reset();
    c.has_motion[subpad] = false; c.motion_last_collect_us[subpad] = 0;
}

void clear_all_motion(ClientSession& c) { for (int s = 0; s < 4; ++s) clear_motion(c, s); }

void set_motion(ClientSession& c, int subpad, const MotionReport& motion) {
    if (subpad < 0 || subpad >= 4) return;
    if (!c.has_motion[subpad]) {
        for (int i = 0; i < 3; ++i) c.motion_samples[subpad][i] = motion;
    } else {
        c.motion_samples[subpad][0] = c.motion_samples[subpad][1];
        c.motion_samples[subpad][1] = c.motion_samples[subpad][2];
        c.motion_samples[subpad][2] = motion;
    }
    c.has_motion[subpad] = true; c.motion_last_collect_us[subpad] = now_us();
}

void set_motion_samples(ClientSession& c, int subpad, const MotionReport samples[3]) {
    if (subpad < 0 || subpad >= 4 || !samples) return;
    c.motion_samples[subpad][0] = samples[0];
    c.motion_samples[subpad][1] = samples[1];
    c.motion_samples[subpad][2] = samples[2];
    c.has_motion[subpad] = true; c.motion_last_collect_us[subpad] = now_us();
}

bool rate_allow(uint32_t ip);
bool server_macro_start(int client_idx, int subpad, const std::string& json_or_commands);

static bool handle_macro_chunk(int client_idx, uint32_t upload_id, uint8_t subpad, uint32_t total_len, uint32_t chunk_count, uint32_t chunk_index, std::span<const uint8_t> payload) {
    uint64_t now = now_us();
    std::string completed;
    {
        std::lock_guard<std::mutex> lk(g_ctx.server_macro_upload_mtx);
        ServerMacroUploadRuntime& up = g_ctx.server_macro_uploads[client_idx];
        if (!up.active || up.upload_id != upload_id) {
            up = ServerMacroUploadRuntime{};
            up.active = true;
            up.upload_id = upload_id;
            up.subpad = (uint8_t)(subpad < 4 ? subpad : 0);
            up.total_len = total_len;
            up.chunk_count = chunk_count;
            up.chunks.assign(chunk_count, {}); up.got.assign(chunk_count, 0);
        }
        if (up.total_len != total_len || up.chunk_count != chunk_count) return true;
        up.last_rx_us = now;
        if (!up.got[chunk_index]) {
            up.chunks[chunk_index].assign((const char*)payload.data(), payload.size());
            up.got[chunk_index] = 1; up.received_count++;
        }
        if (up.received_count == up.chunk_count) {
            size_t total = 0; for (const auto& c : up.chunks) total += c.size();
            if (total != up.total_len) { up = ServerMacroUploadRuntime{}; return true; }
            completed.reserve(total); for (const auto& c : up.chunks) completed += c;
            subpad = up.subpad; up = ServerMacroUploadRuntime{};
        }
    }
    if (!completed.empty()) {
        if (g_ctx.verbose) std::println("[macro] received chunked macro {} bytes", completed.size());
        server_macro_start(client_idx, subpad, completed);
    }
    return true;
}

int server_macro_client_for_sender(const sockaddr_in& sender) {
    uint64_t now = now_us();
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        std::lock_guard<std::mutex> lk(g_ctx.mtx[i]);
        if (g_ctx.clients[i].active && g_ctx.clients[i].addr.sin_addr.s_addr == sender.sin_addr.s_addr && g_ctx.clients[i].addr.sin_port == sender.sin_port) {
            g_ctx.clients[i].last_rx_us = now; return i;
        }
    }
    return allocate_client_session(now, &sender, false);
}

bool server_macro_handle_chunk_packet(std::span<const uint8_t> data, const sockaddr_in& sender) {
    if (data.size() < ns::macro::CHUNK_HEADER_SIZE + HMAC_TAG_SIZE) return false;
    ns::macro::MacroUdpChunkHeaderWire h{};
    std::ranges::copy(data.subspan(0, sizeof(h)), (uint8_t*)&h);
    if (h.magic != ns::macro::UDP_CHUNK_MAGIC) return false;
    if (h.version != PROTO_VERSION || h.total_len > ns::macro::UDP_TEXT_MAX || h.chunk_len > ns::macro::UDP_CHUNK_MAX || h.chunk_count == 0 || h.chunk_index >= h.chunk_count || data.size() != ns::macro::CHUNK_HEADER_SIZE + h.chunk_len + HMAC_TAG_SIZE) return true;
    if (hmac_verify({g_ctx.hmac_key, 32}, data.subspan(0, ns::macro::CHUNK_HEADER_SIZE + h.chunk_len), data.subspan(ns::macro::CHUNK_HEADER_SIZE + h.chunk_len, HMAC_TAG_SIZE)) != 0 || !rate_allow(sender.sin_addr.s_addr)) return true;
    int cidx = server_macro_client_for_sender(sender);
    if (cidx >= 0) handle_macro_chunk(cidx, h.upload_id, h.subpad, h.total_len, h.chunk_count, h.chunk_index, data.subspan(ns::macro::CHUNK_HEADER_SIZE, h.chunk_len));
    return true;
}

bool server_macro_handle_ws_chunk_packet(int client_idx, std::span<const uint8_t> data) {
    if (client_idx < 0 || client_idx >= MAX_CLIENTS || data.size() < ns::macro::CHUNK_HEADER_SIZE) return false;
    ns::macro::MacroUdpChunkHeaderWire h{};
    std::ranges::copy(data.subspan(0, sizeof(h)), (uint8_t*)&h);
    if (h.magic != ns::macro::UDP_CHUNK_MAGIC) return false;
    if (h.version != PROTO_VERSION && h.version != WEB_PROTO_VERSION) return true;
    if (h.total_len > ns::macro::UDP_TEXT_MAX || h.chunk_len > ns::macro::UDP_CHUNK_MAX || h.chunk_count == 0 || h.chunk_index >= h.chunk_count || data.size() != ns::macro::CHUNK_HEADER_SIZE + h.chunk_len) return true;
    return handle_macro_chunk(client_idx, h.upload_id, h.subpad, h.total_len, h.chunk_count, h.chunk_index, data.subspan(ns::macro::CHUNK_HEADER_SIZE, h.chunk_len));
}

bool server_macro_running(int client_idx, int subpad) {
    if (client_idx < 0 || client_idx >= MAX_CLIENTS || subpad < 0 || subpad >= 4) return false;
    std::lock_guard<std::mutex> lk(g_ctx.server_macro_mtx);
    ServerMacroRuntime& rt = g_ctx.server_macros[client_idx][subpad];
    if (!rt.running) return false;
    if ((now_us() - rt.start_us) / 1000ULL > ns::macro::total_ms(rt.steps) + 120) { rt.running = false; return false; }
    return true;
}

void server_macro_apply(int client_idx, int subpad, HIDReport& live) {
    if (client_idx < 0 || client_idx >= MAX_CLIENTS || subpad < 0 || subpad >= 4) return;
    std::lock_guard<std::mutex> lk(g_ctx.server_macro_mtx);
    ServerMacroRuntime& rt = g_ctx.server_macros[client_idx][subpad];
    if (!rt.running) return;
    ns::macro::Step step{};
    if (!ns::macro::step_at(rt.steps, (now_us() - rt.start_us) / 1000ULL, step)) { rt.running = false; return; }
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
    rt.steps = std::move(steps); rt.running = true; rt.start_us = now_us();
    if (g_ctx.verbose) std::println("[macro] started server macro slot={} pad={}", client_idx + 1, subpad + 1);
    return true;
}

void server_macro_stop_all_for_client(int client_idx) {
    if (client_idx < 0 || client_idx >= MAX_CLIENTS) return;
    std::lock_guard<std::mutex> lk(g_ctx.server_macro_mtx);
    for (int s = 0; s < 4; ++s) g_ctx.server_macros[client_idx][s].running = false;
}

void reset_client_session_locked(ClientSession& c) {
    c.active = false; c.first_pkt = true; c.expected_seq = 0; c.last_rx_us = 0;
    c.report.reset(); c.report3.reset(); c.has_new_report = c.has_new_report3 = false;
    clear_all_motion(c);
    c.uses_pad_presence = c.udp_rumble_enabled = false;
    for (int s = 0; s < 4; ++s) {
        c.rumble[s] = RumblePacket{}; c.precision_rumble[s] = PrecisionRumblePacket{};
        c.rumble_active[s] = false; c.rumble_seq[s]++;
        c.udp_last_rumble_seq[s] = c.rumble_seq[s];
        c.pad_present[s] = false; c.pad_last_present_us[s] = 0;
    }
}

void reset_client_session(int client_idx) {
    if (client_idx < 0 || client_idx >= MAX_CLIENTS) return;
    std::lock_guard<std::mutex> lk(g_ctx.mtx[client_idx]);
    reset_client_session_locked(g_ctx.clients[client_idx]);
    server_macro_stop_all_for_client(client_idx);
}

int allocate_client_session(uint64_t now, const sockaddr_in* addr, bool uses_pad_presence) {
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        std::lock_guard<std::mutex> lk(g_ctx.mtx[i]);
        repair_future_client_timestamp(g_ctx.clients[i], now);
        if (!g_ctx.clients[i].active || elapsed_us_over(now, g_ctx.clients[i].last_rx_us, CLIENT_TIMEOUT_US)) {
            g_ctx.clients[i].active = true; g_ctx.clients[i].first_pkt = true; g_ctx.clients[i].expected_seq = 0;
            g_ctx.clients[i].last_rx_us = now; g_ctx.clients[i].addr = addr ? *addr : sockaddr_in{};
            g_ctx.clients[i].report.reset(); g_ctx.clients[i].report3.reset();
            clear_all_motion(g_ctx.clients[i]);
            g_ctx.clients[i].uses_pad_presence = uses_pad_presence; g_ctx.clients[i].udp_rumble_enabled = false;
            for (int s = 0; s < 4; ++s) {
                g_ctx.clients[i].rumble[s] = RumblePacket{};
                g_ctx.clients[i].precision_rumble[s] = PrecisionRumblePacket{};
                g_ctx.clients[i].rumble_active[s] = false; g_ctx.clients[i].rumble_seq[s]++;
                g_ctx.clients[i].udp_last_rumble_seq[s] = g_ctx.clients[i].rumble_seq[s];
                g_ctx.clients[i].pad_present[s] = false; g_ctx.clients[i].pad_last_present_us[s] = 0;
            }
            return i;
        }
    }
    return -1;
}

bool input_is_neutral(const HIDReport& r) {
    return r.buttons == 0 && r.hat == HAT_NEUTRAL && r.lx == 128 && r.ly == 128 && r.rx == 128 && r.ry == 128;
}

bool motion_is_neutral(const MotionReport& m) {
    return std::abs((int)m.ax) < 64 && std::abs((int)m.ay) < 64 && std::abs((int)m.az) < 64 &&
           std::abs((int)m.gx) < 64 && std::abs((int)m.gy) < 64 && std::abs((int)m.gz) < 64;
}

bool extended_is_neutral(const ExtendedHIDReport& r) {
    return input_is_neutral(r.input) && (!r.has_motion || motion_is_neutral(r.motion));
}

void legacy_multi_to_extended(const ns::MultiReport& in, ns::ExtendedMultiReport& out) {
    out.reset();
    const ns::HIDReport* src[4] = {&in.p1, &in.p2, &in.p3, &in.p4};
    ns::ExtendedHIDReport* dst[4] = {&out.p1, &out.p2, &out.p3, &out.p4};
    for (int i = 0; i < 4; ++i) {
        dst[i]->input = *src[i]; dst[i]->motion.reset(); dst[i]->has_motion = 0;
    }
}

void extended3_to_extended_latest(const ns::ExtendedHIDReport3& in, ns::ExtendedHIDReport& out) {
    out.reset(); out.input = in.input; out.has_motion = in.has_motion;
    if (in.has_motion) out.motion = in.motion[2];
}

bool parse_client_packet(const uint8_t* data, size_t len,
                         uint8_t& flags, uint32_t& seq,
                         ns::ExtendedMultiReport& report,
                         bool pad_present[4],
                         bool& is_report3,
                         ns::ExtendedMultiReport3& report3) {
    if (len < 20) return false;
    uint32_t magic; std::memcpy(&magic, data, 4);
    if (magic != PROTO_MAGIC) return false;

    uint8_t ver = data[4]; flags = data[5];
    std::memcpy(&seq, data + 8, 4);
    report.reset(); report3.reset(); is_report3 = false;
    std::fill(pad_present, pad_present + 4, false);

    if (ver == PROTO_VERSION && len == PACKET_SIZE) {
        ns::MultiReport legacy; std::memcpy(&legacy, data + 20, sizeof(ns::MultiReport));
        legacy_multi_to_extended(legacy, report);
        for (int s = 0; s < 4; ++s) pad_present[s] = !extended_is_neutral((&report.p1)[s]);
        return true;
    }

    constexpr size_t EXT_UDP_PACKET_SIZE = 20 + sizeof(ns::ExtendedMultiReport) + HMAC_TAG_SIZE;
    if ((ver == WEB_PROTO_VERSION || ver == PROTO_VERSION) && (len == WEB_PACKET_SIZE || len == EXT_UDP_PACKET_SIZE)) {
        std::memcpy(&report, data + 20, sizeof(ns::ExtendedMultiReport));
        for (int s = 0; s < 4; ++s) pad_present[s] = (data[20 + s * sizeof(ns::ExtendedHIDReport) + 7] & 0x01) != 0;
        if (flags & FLAG_SINGLE_PAD) {
            report.p2.reset(); report.p3.reset(); report.p4.reset();
            pad_present[0] = true; pad_present[1] = pad_present[2] = pad_present[3] = false;
        }
        return true;
    }

    constexpr size_t EXT3_UDP_PACKET_SIZE = 20 + sizeof(ns::ExtendedMultiReport3) + HMAC_TAG_SIZE;
    if (ver == WEB_PROTO_VERSION_3 && (len == WEB_PACKET3_SIZE || len == EXT3_UDP_PACKET_SIZE)) {
        is_report3 = true; std::memcpy(&report3, data + 20, sizeof(ns::ExtendedMultiReport3));
        const ns::ExtendedHIDReport3* src3[4] = { &report3.p1, &report3.p2, &report3.p3, &report3.p4 };
        ns::ExtendedHIDReport* dst1[4] = { &report.p1, &report.p2, &report.p3, &report.p4 };
        for (int s = 0; s < 4; ++s) {
            pad_present[s] = (data[20 + s * sizeof(ns::ExtendedHIDReport3) + 7] & 0x01) != 0;
            extended3_to_extended_latest(*src3[s], *dst1[s]);
        }
        if (flags & FLAG_SINGLE_PAD) {
            report.p2.reset(); report.p3.reset(); report.p4.reset();
            report3.p2.reset(); report3.p3.reset(); report3.p4.reset();
            pad_present[0] = true; pad_present[1] = pad_present[2] = pad_present[3] = false;
        }
        return true;
    }
    return false;
}
