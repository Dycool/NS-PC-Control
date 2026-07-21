#include "web_server.hpp"
#include "app_state.hpp"
#include "gadget_wakeup.hpp"
#include "s2_uac1_audio.hpp"
#include "udp_audio.hpp"
#include "virtual_controller.hpp"
#include "webapp_embed.h"

#include <libwebsockets.h>
#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <deque>
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
    // Amiibo (Switch 2): console scan requests + writebacks forwarded to the
    // WS client, mirroring the UDP path in udp_feedback.cpp.
    uint8_t pending_amiibo_request[4][sizeof(AmiiboRequestPacket)];
    bool has_pending_amiibo_request[4] = {};
    uint8_t pending_amiibo_data[4][sizeof(AmiiboDataPacket)];
    bool has_pending_amiibo_data[4] = {};
    bool close_after_write = false;
    bool close_profile_unsupported = false;
    uint64_t assigned_sleep_seq = 0;
    bool had_slot = false;
};

static bool g_serve_http_webapp = false;

// ── Switch 2 audio over WebSocket ──────────────────────────────────────────
// Single WS audio client (mirrors the single-endpoint UDP model). The lws
// service thread owns the wsi pointer; the audio playback thread only
// enqueues packets under the mutex. Draining happens via the session TIMER
// (10 ms) + SERVER_WRITEABLE, so every lws call stays on the service thread.
namespace {
struct WsAudioSink {
    std::mutex mutex;
    struct lws* wsi = nullptr;
    std::deque<ns::S2AudioPcmPacket> queue;
};
WsAudioSink g_ws_audio;
// ~40 ms of buffered console audio; beyond that, drop oldest (latency wins).
constexpr size_t WS_AUDIO_QUEUE_MAX = 8;

void ws_audio_unregister_locked(struct lws* wsi) {
    if (g_ws_audio.wsi != wsi) return;
    g_ws_audio.wsi = nullptr;
    g_ws_audio.queue.clear();
}
} // namespace

void web_server_push_s2_audio_pcm(const ns::S2AudioPcmPacket& packet) {
    std::lock_guard<std::mutex> lk(g_ws_audio.mutex);
    if (!g_ws_audio.wsi) return;
    if (g_ws_audio.queue.size() >= WS_AUDIO_QUEUE_MAX) g_ws_audio.queue.pop_front();
    g_ws_audio.queue.push_back(packet);
}

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
    {"/css/ns_ui.css", css_ns_ui_css, css_ns_ui_css_len, "text/css; charset=utf-8"},
    {"/js/bridge.js", js_bridge_js, js_bridge_js_len, "application/javascript; charset=utf-8"},
    {"/js/ns_core.js", js_ns_core_js, js_ns_core_js_len, "application/javascript; charset=utf-8"},
    {"/js/controller_layouts.js", js_controller_layouts_js, js_controller_layouts_js_len, "application/javascript; charset=utf-8"},
    {"/js/index.js", js_index_js, js_index_js_len, "application/javascript; charset=utf-8"},
    {"/js/mobile.js", js_mobile_js, js_mobile_js_len, "application/javascript; charset=utf-8"},
    {"/js/editor.js", js_editor_js, js_editor_js_len, "application/javascript; charset=utf-8"},
    {"/js/feat_settings.js", js_feat_settings_js, js_feat_settings_js_len, "application/javascript; charset=utf-8"},
    {"/js/feat_rumble.js", js_feat_rumble_js, js_feat_rumble_js_len, "application/javascript; charset=utf-8"},
    {"/js/feat_motion.js", js_feat_motion_js, js_feat_motion_js_len, "application/javascript; charset=utf-8"},
    {"/js/feat_mouse.js", js_feat_mouse_js, js_feat_mouse_js_len, "application/javascript; charset=utf-8"},
    {"/js/feat_amiibo.js", js_feat_amiibo_js, js_feat_amiibo_js_len, "application/javascript; charset=utf-8"},
    {"/js/feat_audio.js", js_feat_audio_js, js_feat_audio_js_len, "application/javascript; charset=utf-8"}
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
        // WebSocket input is tied to the embedded webapp: it exists to serve
        // browser clients, which cannot speak UDP. Without -w the backend is
        // UDP-only (native desktop + mobile apps), so refuse the upgrade
        // before any session state is allocated.
        case LWS_CALLBACK_FILTER_PROTOCOL_CONNECTION:
            if (!g_serve_http_webapp) {
                if (g_ctx.verbose)
                    std::println("[ws] refused WebSocket client: webapp disabled (start with -w to enable the WS input path)");
                return -1;
            }
            break;

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
            std::fill(sd->has_pending_amiibo_request, sd->has_pending_amiibo_request + 4, false);
            std::fill(sd->has_pending_amiibo_data, sd->has_pending_amiibo_data + 4, false);
            sd->close_after_write = false;
            sd->close_profile_unsupported = false;
            lws_set_timer_usecs(wsi, 10 * 1000);
            std::println("[ws] Connection established from client");
            break;

        case LWS_CALLBACK_CLOSED:
            {
                std::lock_guard<std::mutex> lk(g_ws_audio.mutex);
                if (g_ws_audio.wsi == wsi) {
                    ws_audio_unregister_locked(wsi);
                    s2_ws_audio_set_capabilities(0);
                }
            }
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
                        if (active_client_count(now) < configured_client_capacity() && free_virtual_slot_count(now) > 0) {
                            sd->ws_slot = allocate_client_session(now, nullptr, true, InputSource::WebSocket);
                        }
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
                        const uint64_t now = now_us();
                        if (active_client_count(now) < configured_client_capacity() && free_virtual_slot_count(now) > 0) {
                            sd->ws_slot = allocate_client_session(now, nullptr, true, InputSource::WebSocket);
                        }
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

            // --- Amiibo upload (Switch 2, magic NSAD) ---
            // Mirror of the UDP ingest in main.cpp, minus the sender-address
            // lookup: the WS session already identifies the client. WS is a
            // trusted-network transport, so no HMAC here (see docs/web-app.md).
            if (len >= sizeof(uint32_t)) {
                uint32_t amagic = 0; memcpy(&amagic, payload, 4);
                if (amagic == ns::AMIIBO_DATA_MAGIC) {
                    constexpr size_t amiibo_header = offsetof(ns::AmiiboDataPacket, data);
                    if (len >= amiibo_header && sd->ws_slot >= 0) {
                        ns::AmiiboDataPacket ad{};
                        memcpy(&ad, payload, std::min(len, sizeof(ad)));
                        const uint16_t amiibo_data_len = ad.data_len;
                        const bool size_supported = amiibo_data_len == ns::AMIIBO_RAW_DUMP_SIZE
                            || amiibo_data_len == ns::AMIIBO_EXTENDED_DUMP_SIZE;
                        const bool packet_complete = len >= amiibo_header + amiibo_data_len;
                        if (size_supported && packet_complete && ad.subpad < 4) {
                            int port = console_port_for_client_subpad(sd->ws_slot, ad.subpad);
                            // Joy-Con L+R pair exposes NFC on the right virtual
                            // port; fall back to any assigned NFC-capable port.
                            if (port < 0 || !controller_port_supports_amiibo(port)) {
                                uint8_t mask = 0;
                                {
                                    std::lock_guard<std::mutex> lk(g_ctx.mtx[sd->ws_slot]);
                                    mask = g_ctx.clients[sd->ws_slot].client_assignment[ad.subpad].console_port_mask;
                                }
                                port = -1;
                                for (int p = 0; p < HID_PORT_COUNT; ++p) {
                                    if ((mask & (1u << p)) && controller_port_supports_amiibo(p)) { port = p; break; }
                                }
                            }
                            if (port >= 0) {
                                if (g_ctx.verbose)
                                    std::println("[s2][nfc][ws-rx] forwarding upload client={} subpad={} -> port={} len={}",
                                                 sd->ws_slot, static_cast<unsigned>(ad.subpad), port,
                                                 static_cast<unsigned>(amiibo_data_len));
                                set_amiibo_data_for_port(port, ad.data, amiibo_data_len);
                            } else if (g_ctx.verbose) {
                                std::println(stderr, "[s2][nfc][ws-rx] upload dropped: no assigned NFC-capable port client={} subpad={}",
                                             sd->ws_slot, static_cast<unsigned>(ad.subpad));
                            }
                        } else if (g_ctx.verbose) {
                            std::println(stderr, "[s2][nfc][ws-rx] upload rejected: supported_size={} complete_packet={} declared_len={} frame_len={}",
                                         size_supported, packet_complete,
                                         static_cast<unsigned>(amiibo_data_len), len);
                        }
                    }
                    break;
                }
            }

            // --- Native Joy-Con 2 optical mouse over WS (magic NSJM) ---
            // Mirror of the UDP handler in main.cpp minus the HMAC/endpoint
            // match: the WS transport is trusted and session-scoped.
            if (len == sizeof(ns::JoyconMousePacket)) {
                uint32_t mouse_magic = 0; memcpy(&mouse_magic, payload, 4);
                if (mouse_magic == ns::JOYCON_MOUSE_MAGIC) {
                    ns::JoyconMousePacket mouse{};
                    memcpy(&mouse, payload, sizeof(mouse));
                    if (mouse.version == ns::JOYCON_MOUSE_VERSION && mouse.subpad < 4
                            && sd->ws_slot >= 0) {
                        update_joycon_mouse_stream(sd->ws_slot, mouse, now_us());
                    }
                    break;
                }
            }

            // --- Switch 2 audio over WS (magic NSAC: capabilities) ---
            // Mirror of the UDP capability ingest in udp_audio.cpp minus the
            // HMAC/endpoint association: the WS session identifies the client.
            if (len == sizeof(ns::S2AudioCapabilitiesPacket)) {
                uint32_t audio_magic = 0; memcpy(&audio_magic, payload, 4);
                if (audio_magic == ns::S2_AUDIO_CAPS_MAGIC) {
                    if (g_ctx.usb_controller_family == UsbControllerFamily::Switch2
                            && sd->ws_slot >= 0) {
                        ns::S2AudioCapabilitiesPacket caps_pkt{};
                        memcpy(&caps_pkt, payload, sizeof(caps_pkt));
                        if (caps_pkt.version == ns::S2_AUDIO_VERSION) {
                            // Audio is a Pro Controller 2-only feature; enforce the
                            // same profile rule as the authenticated UDP path.
                            uint8_t profile = ns::CONTROLLER_TYPE_DEFAULT;
                            {
                                std::lock_guard<std::mutex> lk(g_ctx.mtx[sd->ws_slot]);
                                profile = g_ctx.clients[sd->ws_slot].report.p1.reserved[2];
                            }
                            if (profile == ns::CONTROLLER_TYPE_PRO
                                    || profile == ns::CONTROLLER_TYPE_PRO_S2) {
                                {
                                    std::lock_guard<std::mutex> lk(g_ws_audio.mutex);
                                    if (caps_pkt.flags != 0) {
                                        if (g_ws_audio.wsi != wsi) g_ws_audio.queue.clear();
                                        g_ws_audio.wsi = wsi;
                                    } else {
                                        ws_audio_unregister_locked(wsi);
                                    }
                                }
                                s2_ws_audio_set_capabilities(caps_pkt.flags);
                                if (g_ctx.verbose)
                                    std::println("[s2][audio][ws] capabilities from slot {}: 0x{:02x}",
                                                 sd->ws_slot + 1, caps_pkt.flags);
                            }
                        }
                    }
                    break;
                }
            }

            // --- Switch 2 audio over WS (magic NSAU: microphone PCM) ---
            if (len == sizeof(ns::S2AudioPcmPacket)) {
                uint32_t audio_magic = 0; memcpy(&audio_magic, payload, 4);
                if (audio_magic == ns::S2_AUDIO_PCM_MAGIC) {
                    if (g_ctx.usb_controller_family == UsbControllerFamily::Switch2) {
                        bool is_audio_client = false;
                        {
                            std::lock_guard<std::mutex> lk(g_ws_audio.mutex);
                            is_audio_client = g_ws_audio.wsi == wsi;
                        }
                        ns::S2AudioPcmPacket pcm_pkt{};
                        memcpy(&pcm_pkt, payload, sizeof(pcm_pkt));
                        if (is_audio_client
                                && pcm_pkt.version == ns::S2_AUDIO_VERSION
                                && pcm_pkt.direction == ns::S2_AUDIO_DIR_CLIENT_TO_CONSOLE
                                && pcm_pkt.payload_bytes == ns::S2_AUDIO_PCM_BYTES) {
                            s2_ws_audio_touch();
                            (void)s2_uac1_submit_microphone_audio(pcm_pkt.pcm, pcm_pkt.payload_bytes);
                        }
                    }
                    break;
                }
            }

            uint8_t flags = 0; uint32_t seq = 0;
            MultiReport report{};
            bool pad_present[4] = {};
            if (!parse_client_packet(payload, len, flags, seq, report, pad_present)) break;

            const bool unsupported_s2_pair = report_requests_unsupported_s2_pair(report, pad_present, true);
            if (g_ctx.usb_controller_family == UsbControllerFamily::Switch2) {
                report.p2.reset(); report.p3.reset(); report.p4.reset();
                pad_present[1] = pad_present[2] = pad_present[3] = false;
            }

            if (!sd->ws_first && !(flags & FLAG_RESET) && (int32_t)(seq - sd->ws_seq) < 0) break;
            sd->ws_first = false; sd->ws_seq = seq + 1;

            uint64_t now = now_us();
            bool wake_on_new_client = false;
            if (sd->ws_slot >= 0) {
                std::lock_guard<std::mutex> lk(g_ctx.mtx[sd->ws_slot]);
                if (!g_ctx.clients[sd->ws_slot].active || g_ctx.clients[sd->ws_slot].source != InputSource::WebSocket) sd->ws_slot = -1;
            }
            if (unsupported_s2_pair) {
                if (sd->ws_slot >= 0) {
                    reset_client_session_if_source(sd->ws_slot, InputSource::WebSocket);
                    sd->ws_slot = -1;
                }
                ns::ClientAssignmentPacket unsupported = make_server_profile_unsupported_assignment_packet(
                    static_cast<uint8_t>(std::clamp(active_client_count(now), 0, configured_client_capacity())),
                    static_cast<uint8_t>(std::clamp(free_virtual_slot_count(now), 0, configured_virtual_port_count())),
                    switch2_sleep_confirmed(now));
                std::memcpy(sd->pending_assignment[0], &unsupported, sizeof(unsupported));
                sd->has_pending_assignment[0] = true;
                sd->close_after_write = true;
                sd->close_profile_unsupported = true;
                if (g_ctx.verbose) std::println("[ws] refused Joy-Con L+R in native S2 single-controller mode");
                lws_callback_on_writable(wsi);
                break;
            }
            if (sd->ws_slot < 0) {
                const int required_slots = requested_virtual_slots_for_report(report, pad_present, true);
                const int free_slots_now = free_virtual_slot_count(now);
                if (required_slots > free_slots_now || active_client_count(now) >= configured_client_capacity()) {
                    ns::ClientAssignmentPacket full = make_server_full_assignment_packet(
                        static_cast<uint8_t>(std::clamp(active_client_count(now), 0, configured_client_capacity())),
                        static_cast<uint8_t>(std::clamp(free_slots_now, 0, configured_virtual_port_count())),
                        switch2_sleep_confirmed(now));
                    std::memcpy(sd->pending_assignment[0], &full, sizeof(full));
                    sd->has_pending_assignment[0] = true;
                    sd->close_after_write = true;
                    if (g_ctx.verbose) std::println("[ws] server virtual controller slot full, refusing client");
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
                    ++c.report_generation;
                    enable_udp_rumble_state(c);
                }

                for (int s = 0; s < 4; ++s) {
                    c.pad_present[s] = pad_present[s];
                    c.pad_last_present_us[s] = pad_present[s] ? now : 0;
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
                if (sd->has_pending_amiibo_request[s]) {
                    uint8_t buffer[LWS_PRE + sizeof(AmiiboRequestPacket)];
                    memcpy(buffer + LWS_PRE, sd->pending_amiibo_request[s], sizeof(AmiiboRequestPacket));
                    if (lws_write(wsi, buffer + LWS_PRE, sizeof(AmiiboRequestPacket), LWS_WRITE_BINARY) != (int)sizeof(AmiiboRequestPacket)) return -1;
                    sd->has_pending_amiibo_request[s] = false;
                    wrote = true;
                    break;
                }
                if (sd->has_pending_amiibo_data[s]) {
                    uint8_t buffer[LWS_PRE + sizeof(AmiiboDataPacket)];
                    memcpy(buffer + LWS_PRE, sd->pending_amiibo_data[s], sizeof(AmiiboDataPacket));
                    if (lws_write(wsi, buffer + LWS_PRE, sizeof(AmiiboDataPacket), LWS_WRITE_BINARY) != (int)sizeof(AmiiboDataPacket)) return -1;
                    sd->has_pending_amiibo_data[s] = false;
                    wrote = true;
                    break;
                }
            }
            if (!wrote && sd->has_pending_roster) {
                uint8_t buffer[LWS_PRE + sizeof(RosterPacket)];
                memcpy(buffer + LWS_PRE, sd->pending_roster, sizeof(RosterPacket));
                if (lws_write(wsi, buffer + LWS_PRE, sizeof(RosterPacket), LWS_WRITE_BINARY) != (int)sizeof(RosterPacket)) return -1;
                sd->has_pending_roster = false;
                wrote = true;
            }
            if (!wrote) {
                // Console-audio PCM for the registered WS audio client.
                ns::S2AudioPcmPacket audio_pkt{};
                bool has_audio = false;
                {
                    std::lock_guard<std::mutex> lk(g_ws_audio.mutex);
                    if (g_ws_audio.wsi == wsi && !g_ws_audio.queue.empty()) {
                        audio_pkt = g_ws_audio.queue.front();
                        g_ws_audio.queue.pop_front();
                        has_audio = true;
                    }
                }
                if (has_audio) {
                    uint8_t buffer[LWS_PRE + sizeof(ns::S2AudioPcmPacket)];
                    memcpy(buffer + LWS_PRE, &audio_pkt, sizeof(audio_pkt));
                    if (lws_write(wsi, buffer + LWS_PRE, sizeof(ns::S2AudioPcmPacket), LWS_WRITE_BINARY)
                            != (int)sizeof(ns::S2AudioPcmPacket)) return -1;
                }
            }
            if (sd->close_after_write &&
                    !std::ranges::any_of(sd->has_pending_assignment, [](bool h) { return h; }) &&
                    !std::ranges::any_of(sd->has_pending_status, [](bool h) { return h; }) &&
                    !std::ranges::any_of(sd->has_pending_rumble, [](bool h) { return h; })) {
                if (sd->close_profile_unsupported) {
                    static unsigned char reason[] = "S2 does not support L+R";
                    lws_close_reason(wsi, static_cast<lws_close_status>(1008), reason, sizeof(reason) - 1);
                } else {
                    static unsigned char reason[] = "server full";
                    lws_close_reason(wsi, static_cast<lws_close_status>(1013), reason, sizeof(reason) - 1);
                }
                return -1;
            }
            bool audio_pending = false;
            {
                std::lock_guard<std::mutex> lk(g_ws_audio.mutex);
                audio_pending = g_ws_audio.wsi == wsi && !g_ws_audio.queue.empty();
            }
            if (sd->has_pending_roster || audio_pending ||
                std::ranges::any_of(sd->has_pending_assignment, [](bool h) { return h; }) ||
                std::ranges::any_of(sd->has_pending_status, [](bool h) { return h; }) ||
                std::ranges::any_of(sd->has_pending_rumble, [](bool h) { return h; }) ||
                std::ranges::any_of(sd->has_pending_amiibo_request, [](bool h) { return h; }) ||
                std::ranges::any_of(sd->has_pending_amiibo_data, [](bool h) { return h; })) lws_callback_on_writable(wsi);
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
                bool new_amiibo = false;
                const uint64_t state_seq = refresh_server_state_seq();
                const uint8_t active_clients = static_cast<uint8_t>(std::clamp(active_client_count(), 0, configured_client_capacity()));
                const uint8_t free_slots = static_cast<uint8_t>(std::clamp(free_virtual_slot_count(), 0, configured_virtual_port_count()));
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
                        ap.max_clients = static_cast<uint8_t>(configured_client_capacity());
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
                    // Amiibo scan requests / writebacks (mirror of the UDP flush
                    // in udp_feedback.cpp). WS is reliable, so a single send is
                    // enough: consume the repeat counter entirely.
                    ClientSession& cs = g_ctx.clients[sd->ws_slot];
                    if (cs.amiibo_request_pending[s]) {
                        ns::AmiiboRequestPacket rq{};
                        rq.subpad = static_cast<uint8_t>(s);
                        rq.requested = cs.amiibo_requested[s] ? 1 : 0;
                        rq.sequence_le[0] = static_cast<uint8_t>(cs.amiibo_request_seq[s]);
                        rq.sequence_le[1] = static_cast<uint8_t>(cs.amiibo_request_seq[s] >> 8);
                        memcpy(sd->pending_amiibo_request[s], &rq, sizeof(rq));
                        sd->has_pending_amiibo_request[s] = true;
                        cs.amiibo_request_pending[s] = false;
                        cs.amiibo_request_repeats[s] = 0;
                        new_amiibo = true;
                        if (g_ctx.verbose)
                            std::println("[s2][nfc][ws-tx] queuing ui request client={} subpad={} requested={} seq={}",
                                         sd->ws_slot, s, cs.amiibo_requested[s], cs.amiibo_request_seq[s]);
                    }
                    if (cs.amiibo_writeback_pending[s]) {
                        ns::AmiiboDataPacket wb{};
                        wb.subpad = static_cast<uint8_t>(s);
                        wb.data_len = cs.amiibo_writeback_len[s];
                        std::memcpy(wb.data, cs.amiibo_writeback_data[s],
                                    std::min<size_t>(cs.amiibo_writeback_len[s], ns::AMIIBO_EXTENDED_DUMP_SIZE));
                        memcpy(sd->pending_amiibo_data[s], &wb, sizeof(wb));
                        sd->has_pending_amiibo_data[s] = true;
                        cs.amiibo_writeback_pending[s] = false;
                        new_amiibo = true;
                        if (g_ctx.verbose)
                            std::println("[s2][nfc][ws-tx] queuing writeback client={} subpad={} len={}",
                                         sd->ws_slot, s, cs.amiibo_writeback_len[s]);
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
                        ap.max_clients = static_cast<uint8_t>(configured_client_capacity());
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
                if (new_assignment || new_rumble || new_status || new_roster || new_amiibo) lws_callback_on_writable(wsi);
            }
            // Console-audio PCM queued by the playback thread for this session.
            {
                std::lock_guard<std::mutex> lk(g_ws_audio.mutex);
                if (g_ws_audio.wsi == wsi && !g_ws_audio.queue.empty())
                    lws_callback_on_writable(wsi);
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
            std::println("[web] HTTP webapp + WebSocket input listening on port {}", web_port);
        else
            std::println("[web] webapp disabled: WebSocket input path off, UDP only (use -w to enable the webapp + WS)");
    }

    while (!stoken.stop_requested()) {
        lws_service(g_ctx.lws_context, 5);
    }

    lws_context_destroy(g_ctx.lws_context);
    g_ctx.lws_context = nullptr;
}
