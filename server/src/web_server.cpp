#include "web_server.hpp"
#include "app_state.hpp"
#include "gadget_wakeup.hpp"
#include "webapp_embed.h"

#include <libwebsockets.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <print>
#include <string>
#include <vector>

using namespace ns;

static void enable_udp_rumble_state(ClientSession& c) {
    if (!c.udp_rumble_enabled) {
        c.udp_rumble_enabled = true;
        for (int i = 0; i < 4; i++) c.udp_last_rumble_seq[i] = c.rumble_seq[i];
    }
}

void flush_rumble_to_udp(int sock, int client_idx) {
    if (sock < 0 || client_idx < 0 || client_idx >= MAX_CLIENTS) return;
    sockaddr_in dest{};
    ns::RumblePacket pending[4]{};
    bool has[4]{};
    ns::ClientAssignmentPacket pending_assignment[4]{};
    bool has_assignment[4]{};
    ns::ControllerStatusPacket pending_status[4]{};
    bool has_status[4]{};
    const uint64_t state_seq = refresh_server_state_seq();
    const uint8_t active_clients = static_cast<uint8_t>(std::clamp(active_client_count(), 0, MAX_CLIENTS));
    const uint8_t free_slots = static_cast<uint8_t>(std::clamp(free_virtual_slot_count(), 0, HID_PORT_COUNT));
    const bool switch_asleep = switch2_sleep_confirmed();
    const uint64_t roster_seq = refresh_roster_seq();
    ns::RosterPacket roster_pkt{};
    get_roster_packet(roster_pkt);
    bool has_roster = false;

    {
        std::lock_guard<std::mutex> lk(g_ctx.mtx[client_idx]);
        ClientSession& c = g_ctx.clients[client_idx];
        if (!c.active || !c.udp_rumble_enabled) return;
        dest = c.addr;
        bool any_assignment_packet = false;
        for (int s = 0; s < 4; ++s) {
            uint32_t assignment_seq = c.client_assignment_seq[s];
            if (assignment_seq != c.udp_last_client_assignment_seq[s]) {
                pending_assignment[s] = ns::ClientAssignmentPacket{};
                pending_assignment[s].flags = ns::CLIENT_ASSIGNMENT_FLAG_ACCEPTED;
                pending_assignment[s].server_slot = static_cast<uint8_t>(client_idx);
                pending_assignment[s].subpad = static_cast<uint8_t>(s);
                pending_assignment[s].console_port_mask = c.client_assignment[s].console_port_mask;
                pending_assignment[s].primary_console_port = c.client_assignment[s].primary_console_port;
                pending_assignment[s].requested_type = c.client_assignment[s].requested_type;
                pending_assignment[s].virtual_type = c.client_assignment[s].virtual_type;
                if (pending_assignment[s].console_port_mask != 0) pending_assignment[s].flags |= ns::CLIENT_ASSIGNMENT_FLAG_ASSIGNMENT_VALID;
                if (switch_asleep) pending_assignment[s].flags |= ns::CLIENT_ASSIGNMENT_FLAG_SWITCH_ASLEEP;
                pending_assignment[s].active_clients = active_clients;
                pending_assignment[s].max_clients = MAX_CLIENTS;
                pending_assignment[s].free_virtual_slots = free_slots;
                c.udp_last_client_assignment_seq[s] = assignment_seq;
                has_assignment[s] = true;
                any_assignment_packet = true;
            }
            uint32_t seq = c.rumble_seq[s];
            if (seq != c.udp_last_rumble_seq[s]) {
                pending[s] = c.rumble[s];
                c.udp_last_rumble_seq[s] = seq;
                has[s] = true;
            }
            uint32_t status_seq = c.controller_status_seq[s];
            if (status_seq != c.udp_last_controller_status_seq[s]) {
                pending_status[s] = ns::ControllerStatusPacket{};
                pending_status[s].subpad = static_cast<uint8_t>(s);
                pending_status[s].player_index = c.controller_status[s].player_index;
                pending_status[s].player_leds = c.controller_status[s].player_leds;
                if (c.controller_status[s].body_rgb_valid) {
                    pending_status[s].reserved[0] = c.controller_status[s].body_rgb[0];
                    pending_status[s].reserved[1] = c.controller_status[s].body_rgb[1];
                    pending_status[s].reserved[2] = c.controller_status[s].body_rgb[2];
                    pending_status[s].reserved[3] |= ns::CONTROLLER_STATUS_FLAG_BODY_RGB_VALID;
                }
                c.udp_last_controller_status_seq[s] = status_seq;
                has_status[s] = true;
            }
        }
        const uint64_t now = ns::now_us();
        const bool periodic_resend = c.udp_last_roster_send_us == 0 || now - c.udp_last_roster_send_us >= 2'000'000ULL;
        if (roster_seq != c.udp_last_roster_seq || periodic_resend) {
            c.udp_last_roster_seq = roster_seq;
            c.udp_last_roster_send_us = now;
            has_roster = true;
        }
        if (state_seq != c.udp_last_server_state_seq) {
            c.udp_last_server_state_seq = state_seq;
            if (!any_assignment_packet) {
                pending_assignment[0] = ns::ClientAssignmentPacket{};
                pending_assignment[0].flags = ns::CLIENT_ASSIGNMENT_FLAG_ACCEPTED;
                pending_assignment[0].server_slot = static_cast<uint8_t>(client_idx);
                pending_assignment[0].subpad = 0;
                pending_assignment[0].console_port_mask = c.client_assignment[0].console_port_mask;
                pending_assignment[0].primary_console_port = c.client_assignment[0].primary_console_port;
                pending_assignment[0].requested_type = c.client_assignment[0].requested_type;
                pending_assignment[0].virtual_type = c.client_assignment[0].virtual_type;
                if (pending_assignment[0].console_port_mask != 0) pending_assignment[0].flags |= ns::CLIENT_ASSIGNMENT_FLAG_ASSIGNMENT_VALID;
                if (switch_asleep) pending_assignment[0].flags |= ns::CLIENT_ASSIGNMENT_FLAG_SWITCH_ASLEEP;
                pending_assignment[0].active_clients = active_clients;
                pending_assignment[0].max_clients = MAX_CLIENTS;
                pending_assignment[0].free_virtual_slots = free_slots;
                has_assignment[0] = true;
            }
        }
    }

    // Gameplay feedback first; UI/server-state packets are event-driven and
    // should never delay rumble when both are pending in the same tick.
    for (int s = 0; s < 4; ++s) {
        if (!has[s]) continue;
        ssize_t sent = sendto(sock, &pending[s], sizeof(ns::RumblePacket), 0,
                              reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
        if (g_ctx.verbose && sent != static_cast<ssize_t>(sizeof(ns::RumblePacket)))
            std::println(stderr, "[udp] failed to send rumble packet: {}", std::strerror(errno));
    }
    for (int s = 0; s < 4; ++s) {
        if (has_assignment[s]) {
            ssize_t sent = sendto(sock, &pending_assignment[s], sizeof(ns::ClientAssignmentPacket), 0,
                                  reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
            if (g_ctx.verbose && sent != static_cast<ssize_t>(sizeof(ns::ClientAssignmentPacket)))
                std::println(stderr, "[udp] failed to send assignment packet: {}", std::strerror(errno));
        }
        if (has_status[s]) {
            ssize_t sent = sendto(sock, &pending_status[s], sizeof(ns::ControllerStatusPacket), 0,
                                  reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
            if (g_ctx.verbose && sent != static_cast<ssize_t>(sizeof(ns::ControllerStatusPacket)))
                std::println(stderr, "[udp] failed to send controller status packet: {}", std::strerror(errno));
        }
        }
    }
    if (has_roster) {
        ssize_t sent = sendto(sock, &roster_pkt, sizeof(ns::RosterPacket), 0,
                              reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
        if (g_ctx.verbose && sent != static_cast<ssize_t>(sizeof(ns::RosterPacket)))
            std::println(stderr, "[udp] failed to send roster packet: {}", std::strerror(errno));
    }
}

struct SessionData {
    int ws_slot = -1;
    uint32_t ws_seq = 0;
    bool ws_first = true;
    uint32_t last_rumble_seq[4] = {};
    uint32_t last_status_seq[4] = {};
    uint32_t last_assignment_seq[4] = {};
    uint64_t last_server_state_seq = 0;
    uint32_t pending_rumble_seq[4] = {};
    uint8_t pending_rumble[4][sizeof(RumblePacket)];
    uint8_t pending_status[4][sizeof(ControllerStatusPacket)];
    uint8_t pending_assignment[4][sizeof(ClientAssignmentPacket)];
    bool has_pending_rumble[4] = {};
    bool has_pending_status[4] = {};
    bool has_pending_assignment[4] = {};
    uint8_t pending_roster[sizeof(RosterPacket)];
    bool has_pending_roster = false;
    uint64_t last_roster_seq = 0;
    bool close_after_write = false;
    uint64_t assigned_sleep_seq = 0;
    bool had_slot = false;
};

static bool g_serve_http_webapp = false;

struct StaticResource {
    const char* url;
    const unsigned char* content;
    unsigned int len;
    const char* mime;
};

static const StaticResource RESOURCES[] = {
    {"/", index_html, index_html_len, "text/html; charset=utf-8"},
    {"/index.html", index_html, index_html_len, "text/html; charset=utf-8"},
    {"/mobile.html", mobile_html, mobile_html_len, "text/html; charset=utf-8"},
    {"/editor.html", editor_html, editor_html_len, "text/html; charset=utf-8"},
    {"/css/index.css", css_index_css, css_index_css_len, "text/css; charset=utf-8"},
    {"/css/mobile.css", css_mobile_css, css_mobile_css_len, "text/css; charset=utf-8"},
    {"/css/editor.css", css_editor_css, css_editor_css_len, "text/css; charset=utf-8"},
    {"/js/bridge.js", js_bridge_js, js_bridge_js_len, "application/javascript; charset=utf-8"},
    {"/js/index.js", js_index_js, js_index_js_len, "application/javascript; charset=utf-8"},
    {"/js/mobile.js", js_mobile_js, js_mobile_js_len, "application/javascript; charset=utf-8"},
    {"/js/editor.js", js_editor_js, js_editor_js_len, "application/javascript; charset=utf-8"}
};

static int callback_http(struct lws *wsi, enum lws_callback_reasons reason, void*, void *in, size_t) {
    if (reason != LWS_CALLBACK_HTTP) return 0;
    if (!g_serve_http_webapp) {
        lws_return_http_status(wsi, HTTP_STATUS_NOT_FOUND, "WebSocket only");
        return -1;
    }
    const char* url = (const char*)in;
    for (const auto& res : RESOURCES) {
        if (strcmp(url, res.url) == 0) {
            uint8_t hdr_buf[2048 + LWS_PRE];
            uint8_t *start = &hdr_buf[LWS_PRE], *p = start, *end = &hdr_buf[sizeof(hdr_buf) - 1];
            if (lws_add_http_common_headers(wsi, HTTP_STATUS_OK, res.mime, res.len, &p, end) ||
                lws_finalize_write_http_header(wsi, start, &p, end))
                return -1;
            std::vector<uint8_t> body(LWS_PRE + res.len);
            std::memcpy(body.data() + LWS_PRE, res.content, res.len);
            lws_write(wsi, body.data() + LWS_PRE, res.len, LWS_WRITE_HTTP_FINAL);
            return -1;
        }
    }
    lws_return_http_status(wsi, HTTP_STATUS_NOT_FOUND, "Not Found");
    return -1;
}

static int callback_ws(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len) {
    SessionData* sd = (SessionData*)user;
    switch (reason) {
        case LWS_CALLBACK_ESTABLISHED:
            sd->ws_slot = -1; sd->ws_seq = 0; sd->ws_first = true;
            sd->assigned_sleep_seq = g_ctx.switch2_sleep_seq.load(std::memory_order_relaxed);
            sd->had_slot = false;
            std::fill(sd->last_rumble_seq, sd->last_rumble_seq + 4, 0);
            std::fill(sd->last_status_seq, sd->last_status_seq + 4, 0);
            std::fill(sd->last_assignment_seq, sd->last_assignment_seq + 4, 0);
            sd->last_server_state_seq = 0;
            std::fill(sd->pending_rumble_seq, sd->pending_rumble_seq + 4, 0);
            std::fill(sd->has_pending_rumble, sd->has_pending_rumble + 4, false);
            std::fill(sd->has_pending_status, sd->has_pending_status + 4, false);
            std::fill(sd->has_pending_assignment, sd->has_pending_assignment + 4, false);
            sd->has_pending_roster = false;
            sd->last_roster_seq = 0;
            sd->close_after_write = false;
            lws_set_timer_usecs(wsi, 10 * 1000);
            std::println("[ws] Connection established from client");
            break;

        case LWS_CALLBACK_CLOSED:
            if (sd->ws_slot >= 0) {
                std::println("WebSocket client released from Slot {}", sd->ws_slot + 1);
                reset_client_session_if_source(sd->ws_slot, InputSource::WebSocket);
            } else {
                if (g_ctx.verbose) std::println("[ws] Connection closed");
            }
            break;

        case LWS_CALLBACK_RECEIVE: {
            uint8_t* payload = (uint8_t*)in;
            if (!lws_frame_is_binary(wsi)) {
                std::string text((char*)payload, len);
                if (text.starts_with("MACRO_RUN:")) {
                    uint64_t now = now_us();
                    if (sd->ws_slot < 0) {
                        sd->ws_slot = allocate_client_session(now, nullptr, true, InputSource::WebSocket);
                        if (sd->ws_slot >= 0) {
                            sd->assigned_sleep_seq = g_ctx.switch2_sleep_seq.load(std::memory_order_relaxed);
                            sd->had_slot = true;
                            std::println("New WebSocket client accepted into Slot {} (macro)", sd->ws_slot + 1);
                        }
                    }
                    if (sd->ws_slot >= 0) {
                        if (g_ctx.verbose) {
                            std::println("[ws] Client Slot {} triggered macro: {}", sd->ws_slot + 1, text.substr(10));
                        }
                        {
                            std::lock_guard<std::mutex> lk(g_ctx.mtx[sd->ws_slot]);
                            g_ctx.clients[sd->ws_slot].pad_present[0] = true;
                            g_ctx.clients[sd->ws_slot].pad_last_present_us[0] = now;
                        }
                        server_macro_start(sd->ws_slot, 0, text.substr(10));
                    }
                }
                break;
            }

            if (len == sizeof(ns::ClientNamesPacket)) {
                uint32_t magic; memcpy(&magic, payload, 4);
                if (magic == ns::CLIENT_NAMES_MAGIC) {
                    if (sd->ws_slot >= 0) {
                        ns::ClientNamesPacket names{};
                        memcpy(&names, payload, sizeof(names));
                        if (names.version == ns::SERVER_INFO_VERSION) store_client_source_names(sd->ws_slot, names);
                    }
                    break;
                }
            }

            if (len >= ns::macro::CHUNK_HEADER_SIZE) {
                uint32_t magic; memcpy(&magic, payload, 4);
                if (magic == ns::macro::UDP_CHUNK_MAGIC) {
                    if (sd->ws_slot < 0) {
                        sd->ws_slot = allocate_client_session(now_us(), nullptr, true, InputSource::WebSocket);
                        if (sd->ws_slot >= 0) {
                            sd->assigned_sleep_seq = g_ctx.switch2_sleep_seq.load(std::memory_order_relaxed);
                            sd->had_slot = true;
                            std::println("New WebSocket client accepted into Slot {} (macro chunk)", sd->ws_slot + 1);
                        }
                    }
                    if (sd->ws_slot >= 0) server_macro_handle_ws_chunk_packet(sd->ws_slot, {payload, len});
                    break;
                }
            }

            uint8_t flags = 0; uint32_t seq = 0;
            MultiReport report{};
            bool pad_present[4] = {};
            if (!parse_client_packet(payload, len, flags, seq, report, pad_present)) break;

            if (!sd->ws_first && !(flags & FLAG_RESET) && (int32_t)(seq - sd->ws_seq) < 0) break;
            sd->ws_first = false; sd->ws_seq = seq + 1;

            uint64_t now = now_us();
            bool wake_on_new_client = false;
            if (sd->ws_slot >= 0) {
                std::lock_guard<std::mutex> lk(g_ctx.mtx[sd->ws_slot]);
                if (!g_ctx.clients[sd->ws_slot].active || g_ctx.clients[sd->ws_slot].source != InputSource::WebSocket) sd->ws_slot = -1;
            }
            if (sd->ws_slot < 0) {
                const int required_slots = requested_virtual_slots_for_report(report, pad_present, g_ctx.legacy_mode, true);
                const int free_slots_now = free_virtual_slot_count(now);
                if (required_slots > free_slots_now) {
                    ns::ClientAssignmentPacket full = make_server_full_assignment_packet(
                        static_cast<uint8_t>(std::clamp(active_client_count(now), 0, MAX_CLIENTS)),
                        static_cast<uint8_t>(std::clamp(free_slots_now, 0, HID_PORT_COUNT)),
                        switch2_sleep_confirmed(now));
                    std::memcpy(sd->pending_assignment[0], &full, sizeof(full));
                    sd->has_pending_assignment[0] = true;
                    sd->close_after_write = true;
                    if (g_ctx.verbose) std::println("[ws] server virtual controller slots full, refusing client");
                    lws_callback_on_writable(wsi);
                    break;
                }
                sd->ws_slot = allocate_client_session(now, nullptr, true, InputSource::WebSocket);
                if (sd->ws_slot >= 0) {
                    sd->assigned_sleep_seq = g_ctx.switch2_sleep_seq.load(std::memory_order_relaxed);
                    sd->had_slot = true;
                    std::println("New WebSocket client accepted into Slot {}", sd->ws_slot + 1);
                    wake_on_new_client = true;
                    for (int s = 0; s < 4; ++s) {
                        sd->last_rumble_seq[s] = g_ctx.clients[sd->ws_slot].rumble_seq[s];
                        sd->last_status_seq[s] = g_ctx.clients[sd->ws_slot].controller_status_seq[s];
                        sd->last_assignment_seq[s] = 0;
                    }
                    sd->last_server_state_seq = 0;
                }
            }

            if (sd->ws_slot >= 0) {
                std::lock_guard<std::mutex> lk(g_ctx.mtx[sd->ws_slot]);
                ClientSession& c = g_ctx.clients[sd->ws_slot];
                c.active = true; c.source = InputSource::WebSocket; c.uses_pad_presence = true; c.last_rx_us = now;

                if (flags & FLAG_RESET) {
                    c.report.reset(); c.first_pkt = true;
                    clear_all_motion(c);
                    for (int s = 0; s < 4; ++s) { c.pad_present[s] = false; c.pad_last_present_us[s] = 0; }
                }

                if (c.first_pkt || memcmp(&c.report, &report, sizeof(MultiReport)) != 0) {
                    c.report = report;
                    c.has_new_report = true; c.first_pkt = false;
                    enable_udp_rumble_state(c);
                }

                for (int s = 0; s < 4; ++s) {
                    if (pad_present[s]) { c.pad_present[s] = true; c.pad_last_present_us[s] = now; }
                }
            }
            if (sd->ws_slot >= 0 && wake_on_new_client) {
                maybe_send_switch2_wake_advert("WebSocket client connected");
            }
            break;
        }

        case LWS_CALLBACK_SERVER_WRITEABLE: {
            if (sd->ws_slot < 0 && !sd->has_pending_roster &&
                    !std::ranges::any_of(sd->has_pending_assignment, [](bool h) { return h; })) break;
            bool wrote = false;
            for (int s = 0; s < 4; ++s) {
                if (sd->has_pending_rumble[s]) {
                    uint8_t buffer[LWS_PRE + sizeof(RumblePacket)];
                    memcpy(buffer + LWS_PRE, sd->pending_rumble[s], sizeof(RumblePacket));
                    if (lws_write(wsi, buffer + LWS_PRE, sizeof(RumblePacket), LWS_WRITE_BINARY) != sizeof(RumblePacket)) return -1;
                    sd->has_pending_rumble[s] = false;
                    sd->last_rumble_seq[s] = sd->pending_rumble_seq[s];
                    wrote = true;
                    break;
                }
                if (sd->has_pending_status[s]) {
                    uint8_t buffer[LWS_PRE + sizeof(ControllerStatusPacket)];
                    memcpy(buffer + LWS_PRE, sd->pending_status[s], sizeof(ControllerStatusPacket));
                    if (lws_write(wsi, buffer + LWS_PRE, sizeof(ControllerStatusPacket), LWS_WRITE_BINARY) != sizeof(ControllerStatusPacket)) return -1;
                    sd->has_pending_status[s] = false;
                    wrote = true;
                    break;
                }
                if (sd->has_pending_assignment[s]) {
                    uint8_t buffer[LWS_PRE + sizeof(ClientAssignmentPacket)];
                    memcpy(buffer + LWS_PRE, sd->pending_assignment[s], sizeof(ClientAssignmentPacket));
                    if (lws_write(wsi, buffer + LWS_PRE, sizeof(ClientAssignmentPacket), LWS_WRITE_BINARY) != sizeof(ClientAssignmentPacket)) return -1;
                    sd->has_pending_assignment[s] = false;
                    wrote = true;
                    break;
                }
            }
            if (!wrote && sd->has_pending_roster) {
                uint8_t buffer[LWS_PRE + sizeof(RosterPacket)];
                memcpy(buffer + LWS_PRE, sd->pending_roster, sizeof(RosterPacket));
                if (lws_write(wsi, buffer + LWS_PRE, sizeof(RosterPacket), LWS_WRITE_BINARY) != (int)sizeof(RosterPacket)) return -1;
                sd->has_pending_roster = false;
            }
            if (sd->close_after_write &&
                    !std::ranges::any_of(sd->has_pending_assignment, [](bool h) { return h; }) &&
                    !std::ranges::any_of(sd->has_pending_status, [](bool h) { return h; }) &&
                    !std::ranges::any_of(sd->has_pending_rumble, [](bool h) { return h; })) {
                lws_close_reason(wsi, static_cast<lws_close_status>(1013),
                                 (unsigned char*)"server full", 11);
                return -1;
            }
            if (sd->has_pending_roster ||
                std::ranges::any_of(sd->has_pending_assignment, [](bool h) { return h; }) ||
                std::ranges::any_of(sd->has_pending_status, [](bool h) { return h; }) ||
                std::ranges::any_of(sd->has_pending_rumble, [](bool h) { return h; })) lws_callback_on_writable(wsi);
            break;
        }

        case LWS_CALLBACK_TIMER: {
            const uint64_t sleep_seq = g_ctx.switch2_sleep_seq.load(std::memory_order_relaxed);
            if (sd->had_slot && sleep_seq != sd->assigned_sleep_seq) {
                if (sd->ws_slot >= 0) {
                    reset_client_session_if_source(sd->ws_slot, InputSource::WebSocket);
                    sd->ws_slot = -1;
                }
                lws_close_reason(wsi, LWS_CLOSE_STATUS_NORMAL, nullptr, 0);
                return -1;
            }

            if (sd->ws_slot >= 0) {
                bool new_rumble = false;
                bool new_status = false;
                bool new_assignment = false;
                bool new_roster = false;
                const uint64_t state_seq = refresh_server_state_seq();
                const uint8_t active_clients = static_cast<uint8_t>(std::clamp(active_client_count(), 0, MAX_CLIENTS));
                const uint8_t free_slots = static_cast<uint8_t>(std::clamp(free_virtual_slot_count(), 0, HID_PORT_COUNT));
                const bool switch_asleep = switch2_sleep_confirmed();
                const uint64_t roster_seq = refresh_roster_seq();
                ns::RosterPacket roster_pkt{};
                get_roster_packet(roster_pkt);
                std::lock_guard<std::mutex> lk(g_ctx.mtx[sd->ws_slot]);
                if (!g_ctx.clients[sd->ws_slot].active || g_ctx.clients[sd->ws_slot].source != InputSource::WebSocket) {
                    sd->ws_slot = -1;
                    break;
                }
                for (int s = 0; s < 4; ++s) {
                    uint32_t assignment_seq = g_ctx.clients[sd->ws_slot].client_assignment_seq[s];
                    if (assignment_seq != sd->last_assignment_seq[s]) {
                        ClientAssignmentPacket ap{};
                        ap.flags = ns::CLIENT_ASSIGNMENT_FLAG_ACCEPTED;
                        ap.server_slot = static_cast<uint8_t>(sd->ws_slot);
                        ap.subpad = static_cast<uint8_t>(s);
                        ap.console_port_mask = g_ctx.clients[sd->ws_slot].client_assignment[s].console_port_mask;
                        ap.primary_console_port = g_ctx.clients[sd->ws_slot].client_assignment[s].primary_console_port;
                        ap.requested_type = g_ctx.clients[sd->ws_slot].client_assignment[s].requested_type;
                        ap.virtual_type = g_ctx.clients[sd->ws_slot].client_assignment[s].virtual_type;
                        if (ap.console_port_mask != 0) ap.flags |= ns::CLIENT_ASSIGNMENT_FLAG_ASSIGNMENT_VALID;
                        if (switch_asleep) ap.flags |= ns::CLIENT_ASSIGNMENT_FLAG_SWITCH_ASLEEP;
                        ap.active_clients = active_clients;
                        ap.max_clients = MAX_CLIENTS;
                        ap.free_virtual_slots = free_slots;
                        memcpy(sd->pending_assignment[s], &ap, sizeof(ap));
                        sd->last_assignment_seq[s] = assignment_seq;
                        sd->has_pending_assignment[s] = true;
                        new_assignment = true;
                    }
                    uint32_t seq = g_ctx.clients[sd->ws_slot].rumble_seq[s];
                    if (seq != sd->last_rumble_seq[s]) {
                        memcpy(sd->pending_rumble[s], &g_ctx.clients[sd->ws_slot].rumble[s], sizeof(RumblePacket));
                        sd->pending_rumble_seq[s] = seq;
                        sd->has_pending_rumble[s] = true;
                        new_rumble = true;
                    }
                    uint32_t status_seq = g_ctx.clients[sd->ws_slot].controller_status_seq[s];
                    if (status_seq != sd->last_status_seq[s]) {
                        ControllerStatusPacket sp{};
                        sp.subpad = static_cast<uint8_t>(s);
                        sp.player_index = g_ctx.clients[sd->ws_slot].controller_status[s].player_index;
                        sp.player_leds = g_ctx.clients[sd->ws_slot].controller_status[s].player_leds;
                        if (g_ctx.clients[sd->ws_slot].controller_status[s].body_rgb_valid) {
                            sp.reserved[0] = g_ctx.clients[sd->ws_slot].controller_status[s].body_rgb[0];
                            sp.reserved[1] = g_ctx.clients[sd->ws_slot].controller_status[s].body_rgb[1];
                            sp.reserved[2] = g_ctx.clients[sd->ws_slot].controller_status[s].body_rgb[2];
                            sp.reserved[3] |= ns::CONTROLLER_STATUS_FLAG_BODY_RGB_VALID;
                        }
                        memcpy(sd->pending_status[s], &sp, sizeof(sp));
                        sd->last_status_seq[s] = status_seq;
                        sd->has_pending_status[s] = true;
                        new_status = true;
                    }
                }
                if (state_seq != sd->last_server_state_seq) {
                    sd->last_server_state_seq = state_seq;
                    if (!std::ranges::any_of(sd->has_pending_assignment, [](bool h) { return h; })) {
                        ClientAssignmentPacket ap{};
                        ap.flags = ns::CLIENT_ASSIGNMENT_FLAG_ACCEPTED;
                        ap.server_slot = static_cast<uint8_t>(sd->ws_slot);
                        ap.subpad = 0;
                        ap.console_port_mask = g_ctx.clients[sd->ws_slot].client_assignment[0].console_port_mask;
                        ap.primary_console_port = g_ctx.clients[sd->ws_slot].client_assignment[0].primary_console_port;
                        ap.requested_type = g_ctx.clients[sd->ws_slot].client_assignment[0].requested_type;
                        ap.virtual_type = g_ctx.clients[sd->ws_slot].client_assignment[0].virtual_type;
                        if (ap.console_port_mask != 0) ap.flags |= ns::CLIENT_ASSIGNMENT_FLAG_ASSIGNMENT_VALID;
                        if (switch_asleep) ap.flags |= ns::CLIENT_ASSIGNMENT_FLAG_SWITCH_ASLEEP;
                        ap.active_clients = active_clients;
                        ap.max_clients = MAX_CLIENTS;
                        ap.free_virtual_slots = free_slots;
                        memcpy(sd->pending_assignment[0], &ap, sizeof(ap));
                        sd->has_pending_assignment[0] = true;
                        new_assignment = true;
                    }
                }
                if (roster_seq != sd->last_roster_seq) {
                    sd->last_roster_seq = roster_seq;
                    memcpy(sd->pending_roster, &roster_pkt, sizeof(roster_pkt));
                    sd->has_pending_roster = true;
                    new_roster = true;
                }
                if (new_assignment || new_rumble || new_status || new_roster) lws_callback_on_writable(wsi);
            }
            lws_set_timer_usecs(wsi, 10 * 1000);
            break;
        }
        default:
            break;
    }
    return 0;
}

static struct lws_protocols protocols[] = {
    { "http-only", callback_http, 0, 0, 0, NULL, 0 },
    { "nspc-protocol", callback_ws, sizeof(SessionData), 65536, 0, NULL, 0 },
    { NULL, NULL, 0, 0, 0, NULL, 0 }
};

void web_server_thread(std::stop_token stoken, int web_port, uint16_t udp_port, bool serve_http_webapp) {
    (void)udp_port;
    g_serve_http_webapp = serve_http_webapp;
    lws_set_log_level(LLL_ERR | LLL_WARN, NULL);

    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    info.port = web_port;
    info.protocols = protocols;
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT | LWS_SERVER_OPTION_VALIDATE_UTF8;

    g_ctx.lws_context = lws_create_context(&info);
    if (!g_ctx.lws_context) {
        std::println(stderr, "libwebsockets init failed");
        return;
    }

    if (g_ctx.verbose) {
        if (serve_http_webapp)
            std::println("[web] HTTP webapp + WebSocket proxy listening on port {}", web_port);
        else
            std::println("[ws] WebSocket proxy listening on port {}; HTTP webapp disabled (use -w to enable)", web_port);
    }

    while (!stoken.stop_requested()) {
        lws_service(g_ctx.lws_context, 5);
    }

    lws_context_destroy(g_ctx.lws_context);
    g_ctx.lws_context = nullptr;
}
