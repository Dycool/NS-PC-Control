#include "web_server.hpp"
#include "app_state.hpp"
#include "gadget_wakeup.hpp"
#include "virtual_controller.hpp"
#include "shared/sha256.h"
#include "webapp_embed.h"

#include <libwebsockets.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <mutex>
#include <vector>

using namespace ns;

void clear_udp_rumble_state(ClientSession& c) {
    for (int i = 0; i < 4; i++) {
        c.rumble[i] = {};
        c.rumble_seq[i] = 0;
        c.rumble_pending[i] = false;
    }
}

void enable_udp_rumble_state(ClientSession& c) {
    c.rumble_enabled = true;
}

void reset_udp_client_session_locked(ClientSession& c) {
    c.active = false;
    c.first_pkt = true;
    c.report.reset();
    clear_all_motion(c);
    c.uses_pad_presence = false;
    clear_udp_rumble_state(c);
    for (int s = 0; s < 4; ++s) {
        c.pad_present[s] = false;
        c.pad_last_present_us[s] = 0;
    }
}

void flush_rumble_to_udp(int sock, int client_idx) {
    (void)sock;
    (void)client_idx;
    // UDP rumble pushing logic is intentionally separate from WebSocket server logic.
    // However, it is required for UDP clients to receive rumble, but since this
    // module handles both, UDP is managed in writers.cpp.
}

struct SessionData {
    int ws_slot = -1;
    uint32_t ws_seq = 0;
    bool ws_first = true;
    uint32_t last_rumble_seq[4] = {};
    uint8_t pending_rumble[4][sizeof(RumblePacket)];
    bool has_pending_rumble[4] = {};
};

static bool g_serve_http_webapp = false;
static struct lws_context *g_lws_context = nullptr;

static int callback_http(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len) {
    (void)user;
    (void)len;
    switch (reason) {
        case LWS_CALLBACK_HTTP: {
            if (!g_serve_http_webapp) {
                lws_return_http_status(wsi, HTTP_STATUS_NOT_FOUND, "WebSocket only");
                return -1;
            }
            const char* url = (const char*)in;
            const unsigned char* html = nullptr;
            size_t html_len = 0;
            if (strcmp(url, "/") == 0 || strcmp(url, "/index.html") == 0) {
                html = index_html;
                html_len = index_html_len - 1;
            } else if (strcmp(url, "/mobile.html") == 0) {
                html = mobile_html;
                html_len = mobile_html_len - 1;
            } else if (strcmp(url, "/editor.html") == 0) {
                html = editor_html;
                html_len = editor_html_len - 1;
            }

            if (html) {
                uint8_t buffer[2048 + LWS_PRE];
                uint8_t *start = &buffer[LWS_PRE];
                uint8_t *p = start;
                uint8_t *end = &buffer[sizeof(buffer) - 1];

                if (lws_add_http_common_headers(wsi, HTTP_STATUS_OK, "text/html; charset=utf-8", html_len, &p, end))
                    return -1;
                
                if (lws_finalize_write_http_header(wsi, start, &p, end))
                    return -1;

                unsigned char* body = (unsigned char*)malloc(LWS_PRE + html_len);
                if (!body) return -1;
                memcpy(body + LWS_PRE, html, html_len);
                lws_write(wsi, body + LWS_PRE, html_len, LWS_WRITE_HTTP_FINAL);
                free(body);
                
                // HTTP transaction finished, close connection
                return -1;
            } else {
                lws_return_http_status(wsi, HTTP_STATUS_NOT_FOUND, "Not Found");
                return -1;
            }
        }
        default:
            break;
    }
    return 0;
}

static int callback_ws(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len) {
    SessionData* sd = (SessionData*)user;
    switch (reason) {
        case LWS_CALLBACK_ESTABLISHED: {
            sd->ws_slot = -1;
            sd->ws_seq = 0;
            sd->ws_first = true;
            for (int i = 0; i < 4; ++i) {
                sd->last_rumble_seq[i] = 0;
                sd->has_pending_rumble[i] = false;
            }
            lws_set_timer_usecs(wsi, 10 * LWS_USEC_PER_MSEC); // Poll rumble every 10ms
            break;
        }

        case LWS_CALLBACK_CLOSED: {
            if (sd->ws_slot >= 0) {
                std::lock_guard<std::mutex> lk(g_mtx[sd->ws_slot]);
                if (g_clients[sd->ws_slot].active) {
                    reset_udp_client_session_locked(g_clients[sd->ws_slot]);
                    g_clients[sd->ws_slot].active = false;
                }
            }
            break;
        }

        case LWS_CALLBACK_RECEIVE: {
            bool is_binary = lws_frame_is_binary(wsi);
            size_t flen = len;
            uint8_t* payload = (uint8_t*)in;

            if (!is_binary) {
                std::string text((char*)payload, flen);
                const std::string prefix = "MACRO_RUN:";
                if (text.starts_with(prefix)) {
                    uint64_t now = now_us();
                    if (sd->ws_slot < 0) {
                        for (int i = 0; i < MAX_CLIENTS; ++i) {
                            std::lock_guard<std::mutex> lk(g_mtx[i]);
                            if (!g_clients[i].active) {
                                sd->ws_slot = i;
                                g_clients[i].active = true;
                                g_clients[i].first_pkt = true;
                                g_clients[i].report.reset();
                                clear_all_motion(g_clients[i]);
                                g_clients[i].uses_pad_presence = true;
                                clear_udp_rumble_state(g_clients[i]);
                                for (int s = 0; s < 4; ++s) { g_clients[i].pad_present[s] = false; g_clients[i].pad_last_present_us[s] = 0; }
                                g_clients[i].last_rx_us = now;
                                break;
                            }
                        }
                    }
                    if (sd->ws_slot >= 0) {
                        {
                            std::lock_guard<std::mutex> lk(g_mtx[sd->ws_slot]);
                            g_clients[sd->ws_slot].active = true;
                            g_clients[sd->ws_slot].uses_pad_presence = true;
                            g_clients[sd->ws_slot].pad_present[0] = true;
                            g_clients[sd->ws_slot].pad_last_present_us[0] = now;
                            g_clients[sd->ws_slot].last_rx_us = now;
                        }
                        server_macro_start(sd->ws_slot, 0, text.substr(prefix.size()));
                    }
                }
                break;
            }

            // Binary processing
            if (flen >= ns::macro::CHUNK_HEADER_SIZE) {
                uint32_t maybe_macro_magic = 0;
                memcpy(&maybe_macro_magic, payload, 4);
                if (maybe_macro_magic == ns::macro::UDP_CHUNK_MAGIC) {
                    uint64_t now = now_us();
                    if (sd->ws_slot < 0) {
                        for (int i = 0; i < MAX_CLIENTS; ++i) {
                            std::lock_guard<std::mutex> lk(g_mtx[i]);
                            if (!g_clients[i].active) {
                                sd->ws_slot = i;
                                g_clients[i].active = true;
                                g_clients[i].first_pkt = true;
                                g_clients[i].report.reset();
                                clear_all_motion(g_clients[i]);
                                g_clients[i].uses_pad_presence = true;
                                g_clients[i].last_rx_us = now;
                                break;
                            }
                        }
                    }
                    if (sd->ws_slot >= 0) {
                        std::lock_guard<std::mutex> lk(g_mtx[sd->ws_slot]);
                        g_clients[sd->ws_slot].active = true;
                        g_clients[sd->ws_slot].uses_pad_presence = true;
                        g_clients[sd->ws_slot].pad_present[0] = true;
                        g_clients[sd->ws_slot].pad_last_present_us[0] = now;
                        g_clients[sd->ws_slot].last_rx_us = now;
                    }
                    if (sd->ws_slot >= 0) server_macro_handle_ws_chunk_packet(sd->ws_slot, payload, (size_t)flen);
                    break;
                }
            }

            if (flen != PACKET_SIZE && flen != WEB_PACKET_SIZE && flen != WEB_PACKET3_SIZE) break;

            uint32_t magic; memcpy(&magic, payload, 4);
            if (magic != PROTO_MAGIC) break;
            uint8_t ver; memcpy(&ver, payload + 4, 1);
            uint8_t flags; memcpy(&flags, payload + 5, 1);
            bool is_reset = (flags & FLAG_RESET);
            uint32_t seq; memcpy(&seq, payload + 8, 4);

            ExtendedMultiReport report;
            ExtendedMultiReport3 report3;
            report.reset();
            report3.reset();
            bool is_report3 = false;
            bool pad_present[4] = {};

            if (ver == PROTO_VERSION && flen == PACKET_SIZE) {
                MultiReport legacy;
                memcpy(&legacy, payload + 20, sizeof(MultiReport));
                legacy_multi_to_extended(legacy, report);
                pad_present[0] = !extended_is_neutral(report.p1);
                pad_present[1] = !extended_is_neutral(report.p2);
                pad_present[2] = !extended_is_neutral(report.p3);
                pad_present[3] = !extended_is_neutral(report.p4);
            } else if ((ver == WEB_PROTO_VERSION || ver == PROTO_VERSION) && flen == WEB_PACKET_SIZE) {
                memcpy(&report, payload + 20, sizeof(ExtendedMultiReport));
                for (int s = 0; s < 4; ++s)
                    pad_present[s] = (payload[20 + s * sizeof(ExtendedHIDReport) + 7] & 0x01) != 0;
                if (flags & FLAG_SINGLE_PAD) {
                    report.p2.reset(); report.p3.reset(); report.p4.reset();
                    pad_present[0] = true;
                    pad_present[1] = false; pad_present[2] = false; pad_present[3] = false;
                }
            } else if (ver == WEB_PROTO_VERSION_3 && flen == WEB_PACKET3_SIZE) {
                is_report3 = true;
                memcpy(&report3, payload + 20, sizeof(ExtendedMultiReport3));
                const ExtendedHIDReport3* src3[4] = { &report3.p1, &report3.p2, &report3.p3, &report3.p4 };
                ExtendedHIDReport* dst1[4] = { &report.p1, &report.p2, &report.p3, &report.p4 };
                for (int s = 0; s < 4; ++s) {
                    pad_present[s] = (payload[20 + s * sizeof(ExtendedHIDReport3) + 7] & 0x01) != 0;
                    extended3_to_extended_latest(*src3[s], *dst1[s]);
                }
                if (flags & FLAG_SINGLE_PAD) {
                    report.p2.reset(); report.p3.reset(); report.p4.reset();
                    report3.p2.reset(); report3.p3.reset(); report3.p4.reset();
                    pad_present[0] = true;
                    pad_present[1] = false; pad_present[2] = false; pad_present[3] = false;
                }
            } else {
                break;
            }

            if (!sd->ws_first && !is_reset && (int32_t)(seq - sd->ws_seq) < 0) break;
            sd->ws_first = false;
            sd->ws_seq = seq + 1;

            uint64_t now = now_us();
            bool wake_on_new_client = false;

            if (sd->ws_slot >= 0) {
                std::lock_guard<std::mutex> lk(g_mtx[sd->ws_slot]);
                if (!g_clients[sd->ws_slot].active)
                    sd->ws_slot = -1;
            }
            if (sd->ws_slot < 0) {
                for (int i = 0; i < MAX_CLIENTS; ++i) {
                    std::lock_guard<std::mutex> lk(g_mtx[i]);
                    if (!g_clients[i].active) {
                        sd->ws_slot = i;
                        g_clients[i].active = true;
                        g_clients[i].first_pkt = true;
                        g_clients[i].report.reset();
                        clear_all_motion(g_clients[i]);
                        g_clients[i].uses_pad_presence = true;
                        clear_udp_rumble_state(g_clients[i]);
                        for (int s = 0; s < 4; ++s) {
                            g_clients[i].pad_present[s] = false;
                            g_clients[i].pad_last_present_us[s] = 0;
                        }
                        g_clients[i].last_rx_us = now;
                        wake_on_new_client = true;
                        for (int s = 0; s < 4; ++s)
                            sd->last_rumble_seq[s] = g_clients[i].rumble_seq[s];
                        break;
                    }
                }
            }

            if (wake_on_new_client)
                maybe_send_switch2_wake_advert("client connected via WebSocket input");

            if (sd->ws_slot >= 0) {
                std::lock_guard<std::mutex> lk(g_mtx[sd->ws_slot]);
                g_clients[sd->ws_slot].active = true;
                g_clients[sd->ws_slot].uses_pad_presence = true;
                g_clients[sd->ws_slot].last_rx_us = now;

                if (is_reset) {
                    g_clients[sd->ws_slot].report.reset();
                    g_clients[sd->ws_slot].first_pkt = true;
                    clear_all_motion(g_clients[sd->ws_slot]);
                    for (int s = 0; s < 4; ++s) {
                        g_clients[sd->ws_slot].pad_present[s] = false;
                        g_clients[sd->ws_slot].pad_last_present_us[s] = 0;
                    }
                }

                if (is_report3) {
                    if (g_clients[sd->ws_slot].first_pkt ||
                        memcmp(&g_clients[sd->ws_slot].report3, &report3, sizeof(ExtendedMultiReport3)) != 0) {
                        g_clients[sd->ws_slot].report3 = report3;
                        g_clients[sd->ws_slot].has_new_report3 = true;
                        g_clients[sd->ws_slot].has_new_report = true;
                        g_clients[sd->ws_slot].first_pkt = false;
                        g_clients[sd->ws_slot].report = report;
                        enable_udp_rumble_state(g_clients[sd->ws_slot]);
                    }
                } else {
                    if (g_clients[sd->ws_slot].first_pkt ||
                        memcmp(&g_clients[sd->ws_slot].report, &report, sizeof(ExtendedMultiReport)) != 0) {
                        g_clients[sd->ws_slot].report = report;
                        g_clients[sd->ws_slot].has_new_report = true;
                        g_clients[sd->ws_slot].first_pkt = false;
                        enable_udp_rumble_state(g_clients[sd->ws_slot]);
                    }
                }

                for (int s = 0; s < 4; ++s) {
                    if (pad_present[s]) {
                        g_clients[sd->ws_slot].pad_present[s] = true;
                        g_clients[sd->ws_slot].pad_last_present_us[s] = now;
                    }
                }
            }
            break;
        }

        case LWS_CALLBACK_SERVER_WRITEABLE: {
            if (sd->ws_slot < 0) break;
            
            bool sent_any = false;
            for (int s = 0; s < 4; ++s) {
                if (sd->has_pending_rumble[s]) {
                    uint8_t buffer[LWS_PRE + sizeof(RumblePacket)];
                    memcpy(buffer + LWS_PRE, sd->pending_rumble[s], sizeof(RumblePacket));
                    lws_write(wsi, buffer + LWS_PRE, sizeof(RumblePacket), LWS_WRITE_BINARY);
                    sd->has_pending_rumble[s] = false;
                    sent_any = true;
                    break; 
                }
            }

            bool still_has = false;
            for (int s = 0; s < 4; ++s) {
                if (sd->has_pending_rumble[s]) still_has = true;
            }
            if (still_has) lws_callback_on_writable(wsi);

            break;
        }

        case LWS_CALLBACK_TIMER: {
            if (sd->ws_slot >= 0) {
                bool new_rumble = false;
                std::lock_guard<std::mutex> lk(g_mtx[sd->ws_slot]);
                for (int s = 0; s < 4; ++s) {
                    uint32_t seq = g_clients[sd->ws_slot].rumble_seq[s];
                    if (seq != sd->last_rumble_seq[s]) {
                        memcpy(sd->pending_rumble[s], &g_clients[sd->ws_slot].rumble[s], sizeof(RumblePacket));
                        sd->has_pending_rumble[s] = true;
                        sd->last_rumble_seq[s] = seq;
                        new_rumble = true;
                    }
                }
                if (new_rumble) lws_callback_on_writable(wsi);
            }
            lws_set_timer_usecs(wsi, 10 * LWS_USEC_PER_MSEC);
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

    // Supress noisy libwebsockets logs
    lws_set_log_level(LLL_ERR | LLL_WARN, NULL);

    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    info.port = web_port;
    info.protocols = protocols;
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT | LWS_SERVER_OPTION_VALIDATE_UTF8;

    g_lws_context = lws_create_context(&info);
    if (!g_lws_context) {
        std::println("libwebsockets init failed");
        return;
    }

    if (serve_http_webapp)
        std::println("[web] HTTP webapp + WebSocket proxy listening on port {}", web_port);
    else
        std::println("[ws] WebSocket proxy listening on port {}; HTTP webapp disabled (use -w to enable)", web_port);

    while (!stoken.stop_requested()) {
        lws_service(g_lws_context, 50); // wait up to 50ms
    }

    lws_context_destroy(g_lws_context);
    g_lws_context = nullptr;
}
