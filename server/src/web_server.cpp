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

    {
        std::lock_guard<std::mutex> lk(g_ctx.mtx[client_idx]);
        ClientSession& c = g_ctx.clients[client_idx];
        if (!c.active || !c.udp_rumble_enabled) return;
        dest = c.addr;
        for (int s = 0; s < 4; ++s) {
            uint32_t seq = c.rumble_seq[s];
            if (seq != c.udp_last_rumble_seq[s]) {
                pending[s] = c.rumble[s];
                c.udp_last_rumble_seq[s] = seq;
                has[s] = true;
            }
        }
    }

    for (int s = 0; s < 4; ++s) {
        if (!has[s]) continue;
        ssize_t sent = sendto(sock, &pending[s], sizeof(ns::RumblePacket), 0,
                              reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
        if (g_ctx.verbose && sent != static_cast<ssize_t>(sizeof(ns::RumblePacket)))
            std::println(stderr, "[udp] failed to send rumble packet: {}", std::strerror(errno));
    }
}

struct SessionData {
    int ws_slot = -1;
    uint32_t ws_seq = 0;
    bool ws_first = true;
    uint32_t last_rumble_seq[4] = {};
    uint32_t pending_rumble_seq[4] = {};
    uint8_t pending_rumble[4][sizeof(RumblePacket)];
    bool has_pending_rumble[4] = {};
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
            std::fill(sd->last_rumble_seq, sd->last_rumble_seq + 4, 0);
            std::fill(sd->pending_rumble_seq, sd->pending_rumble_seq + 4, 0);
            std::fill(sd->has_pending_rumble, sd->has_pending_rumble + 4, false);
            lws_set_timer_usecs(wsi, 10 * 1000);
            break;

        case LWS_CALLBACK_CLOSED:
            if (sd->ws_slot >= 0) reset_client_session(sd->ws_slot);
            break;

        case LWS_CALLBACK_RECEIVE: {
            uint8_t* payload = (uint8_t*)in;
            if (!lws_frame_is_binary(wsi)) {
                std::string text((char*)payload, len);
                if (text.starts_with("MACRO_RUN:")) {
                    uint64_t now = now_us();
                    if (sd->ws_slot < 0) sd->ws_slot = allocate_client_session(now, nullptr, true);
                    if (sd->ws_slot >= 0) {
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

            if (len >= ns::macro::CHUNK_HEADER_SIZE) {
                uint32_t magic; memcpy(&magic, payload, 4);
                if (magic == ns::macro::UDP_CHUNK_MAGIC) {
                    if (sd->ws_slot < 0) sd->ws_slot = allocate_client_session(now_us(), nullptr, true);
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
            if (sd->ws_slot >= 0) {
                std::lock_guard<std::mutex> lk(g_ctx.mtx[sd->ws_slot]);
                if (!g_ctx.clients[sd->ws_slot].active) sd->ws_slot = -1;
            }
            if (sd->ws_slot < 0) {
                sd->ws_slot = allocate_client_session(now, nullptr, true);
                if (sd->ws_slot >= 0) {
                    maybe_send_switch2_wake_advert("client connected via WebSocket input");
                    for (int s = 0; s < 4; ++s) sd->last_rumble_seq[s] = g_ctx.clients[sd->ws_slot].rumble_seq[s];
                }
            }

            if (sd->ws_slot >= 0) {
                std::lock_guard<std::mutex> lk(g_ctx.mtx[sd->ws_slot]);
                ClientSession& c = g_ctx.clients[sd->ws_slot];
                c.active = true; c.uses_pad_presence = true; c.last_rx_us = now;

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
            break;
        }

        case LWS_CALLBACK_SERVER_WRITEABLE: {
            if (sd->ws_slot < 0) break;
            for (int s = 0; s < 4; ++s) {
                if (sd->has_pending_rumble[s]) {
                    uint8_t buffer[LWS_PRE + sizeof(RumblePacket)];
                    memcpy(buffer + LWS_PRE, sd->pending_rumble[s], sizeof(RumblePacket));
                    if (lws_write(wsi, buffer + LWS_PRE, sizeof(RumblePacket), LWS_WRITE_BINARY) != sizeof(RumblePacket)) return -1;
                    sd->has_pending_rumble[s] = false;
                    sd->last_rumble_seq[s] = sd->pending_rumble_seq[s];
                    break;
                }
            }
            if (std::ranges::any_of(sd->has_pending_rumble, [](bool h) { return h; })) lws_callback_on_writable(wsi);
            break;
        }

        case LWS_CALLBACK_TIMER: {
            if (sd->ws_slot >= 0) {
                bool new_rumble = false;
                std::lock_guard<std::mutex> lk(g_ctx.mtx[sd->ws_slot]);
                for (int s = 0; s < 4; ++s) {
                    uint32_t seq = g_ctx.clients[sd->ws_slot].rumble_seq[s];
                    if (seq != sd->last_rumble_seq[s]) {
                        memcpy(sd->pending_rumble[s], &g_ctx.clients[sd->ws_slot].rumble[s], sizeof(RumblePacket));
                        sd->pending_rumble_seq[s] = seq;
                        sd->has_pending_rumble[s] = true;
                        new_rumble = true;
                    }
                }
                if (new_rumble) lws_callback_on_writable(wsi);
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
        std::println("libwebsockets init failed");
        return;
    }

    if (serve_http_webapp)
        std::println("[web] HTTP webapp + WebSocket proxy listening on port {}", web_port);
    else
        std::println("[ws] WebSocket proxy listening on port {}; HTTP webapp disabled (use -w to enable)", web_port);

    while (!stoken.stop_requested()) {
        lws_service(g_ctx.lws_context, 5);
    }

    lws_context_destroy(g_ctx.lws_context);
    g_ctx.lws_context = nullptr;
}
