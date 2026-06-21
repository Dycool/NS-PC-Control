#include "web_server.hpp"
#include "app_state.hpp"
#include "gadget_wakeup.hpp"
#include "virtual_controller.hpp"
#include "shared/sha256.h"
#include "webapp_embed.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <thread>
#include <vector>

using namespace ns;

// ══════════════════════════════════════════════════════════════════════════════
// ── Embedded WebSocket proxy + optional HTTP webapp ──────────────────────────
// ══════════════════════════════════════════════════════════════════════════════

const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void base64_encode(const uint8_t *in, size_t len, char *out) {
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i+1 < len) v |= (uint32_t)in[i+1] << 8;
        if (i+2 < len) v |= in[i+2];
        *out++ = B64[(v >> 18) & 0x3F];
        *out++ = B64[(v >> 12) & 0x3F];
        *out++ = (i+1 < len) ? B64[(v >> 6) & 0x3F] : '=';
        *out++ = (i+2 < len) ? B64[v & 0x3F] : '=';
    }
    *out = '\0';
}


// ── Minimal SHA-1 (for WebSocket handshake) ───────────────────────────────────
struct Sha1Ctx {
    uint32_t state[5];
    uint64_t count;
    uint8_t  buffer[64];
};

void sha1_transform(uint32_t state[5], const uint8_t block[64]) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = (block[i*4]<<24) | (block[i*4+1]<<16) | (block[i*4+2]<<8) | block[i*4+3];
    for (int i = 16; i < 80; i++) {
        uint32_t t = w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16];
        w[i] = (t << 1) | (t >> 31);
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)       { f = (b & c) | (~b & d);       k = 0x5A827999; }
        else if (i < 40)  { f = b ^ c ^ d;                k = 0x6ED9EBA1; }
        else if (i < 60)  { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else              { f = b ^ c ^ d;                k = 0xCA62C1D6; }
        uint32_t temp = ((a << 5) | (a >> 27)) + f + e + k + w[i];
        e = d; d = c; c = (b << 30) | (b >> 2); b = a; a = temp;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
}

void sha1_init(Sha1Ctx *ctx) {
    ctx->state[0] = 0x67452301; ctx->state[1] = 0xEFCDAB89;
    ctx->state[2] = 0x98BADCFE; ctx->state[3] = 0x10325476;
    ctx->state[4] = 0xC3D2E1F0; ctx->count = 0;
}

void sha1_update(Sha1Ctx *ctx, const uint8_t *data, size_t len) {
    size_t idx = ctx->count & 63;
    ctx->count += len;
    while (len--) {
        ctx->buffer[idx++] = *data++;
        if (idx == 64) { sha1_transform(ctx->state, ctx->buffer); idx = 0; }
    }
}

void sha1_final(Sha1Ctx *ctx, uint8_t digest[20]) {
    uint64_t bits = ctx->count * 8;
    size_t idx = ctx->count & 63;
    size_t pad = (idx < 56) ? (56 - idx) : (120 - idx);
    uint8_t padding[64];
    memset(padding, 0, pad);
    padding[0] = 0x80;
    sha1_update(ctx, padding, pad);
    uint8_t len_bytes[8];
    for (int i = 0; i < 8; i++) len_bytes[7-i] = (bits >> (i*8)) & 0xFF;
    sha1_update(ctx, len_bytes, 8);
    for (int i = 0; i < 5; i++) {
        digest[i*4]   = (ctx->state[i] >> 24) & 0xFF;
        digest[i*4+1] = (ctx->state[i] >> 16) & 0xFF;
        digest[i*4+2] = (ctx->state[i] >> 8) & 0xFF;
        digest[i*4+3] = ctx->state[i] & 0xFF;
    }
}


// ── Single-threaded WebSocket/HTTP client state ─────────────────────────────
struct WebClient {
    int fd = -1;

    uint8_t buf[65536];
    size_t fill = 0;

    enum State : uint8_t {
        READ_HTTP,
        WRITE_RESP,
        WS_ACTIVE,
        CLOSED
    } state = CLOSED;

    char http_buf[8192];
    size_t http_len = 0;
    uint64_t connect_time = 0;
    uint32_t ip = 0;

    int      ws_slot = -1;
    uint32_t ws_seq = 0;
    bool     ws_first = true;
    uint64_t ws_last_rx = 0;
    uint64_t last_ping_us = 0;
    uint32_t last_rumble_seq[4] = {};

    uint8_t *wbuf = nullptr;
    size_t   wlen = 0;
    size_t   woff = 0;
    State    after_write = CLOSED;
};

void legacy_multi_to_extended(const MultiReport& in, ExtendedMultiReport& out) {
    out.reset();
    out.p1.input = in.p1;
    out.p2.input = in.p2;
    out.p3.input = in.p3;
    out.p4.input = in.p4;
}

bool extended_udp_packet_ok(const ExtendedUdpPacket& p) {
    return p.magic == PROTO_MAGIC &&
           (p.version == WEB_PROTO_VERSION || p.version == PROTO_VERSION);
}

bool extended_udp3_packet_ok(const ExtendedUdpPacket3& p) {
    return p.magic == PROTO_MAGIC && p.version == WEB_PROTO_VERSION_3;
}

bool extended_report_pad_present(const ExtendedMultiReport& report, int subpad) {
    if (subpad < 0 || subpad >= 4) return false;
    const uint8_t* raw = reinterpret_cast<const uint8_t*>(&report);
    return (raw[subpad * sizeof(ExtendedHIDReport) + 7] & 0x01) != 0;
}

bool extended3_report_pad_present(const ExtendedMultiReport3& report, int subpad) {
    if (subpad < 0 || subpad >= 4) return false;
    const uint8_t* raw = reinterpret_cast<const uint8_t*>(&report);
    return (raw[subpad * sizeof(ExtendedHIDReport3) + 7] & 0x01) != 0;
}

void extended3_to_extended_latest(const ExtendedHIDReport3& in, ExtendedHIDReport& out) {
    out.reset();
    out.input = in.input;
    out.has_motion = in.has_motion;
    if (in.has_motion) out.motion = in.motion[2];
}

void clear_udp_rumble_state(ClientSession& c) {
    c.udp_rumble_enabled = false;
    for (int s = 0; s < 4; ++s)
        c.udp_last_rumble_seq[s] = c.rumble_seq[s];
}

void reset_udp_client_session_locked(ClientSession& c) {
    c.active = false;
    c.first_pkt = true;
    c.expected_seq = 0;
    c.last_rx_us = 0;
    c.report.reset();
    clear_all_motion(c);
    c.uses_pad_presence = false;
    clear_udp_rumble_state(c);
    for (int s = 0; s < 4; ++s) {
        c.pad_present[s] = false;
        c.pad_last_present_us[s] = 0;
    }
}

void enable_udp_rumble_state(ClientSession& c) {
    if (!c.udp_rumble_enabled) {
        c.udp_rumble_enabled = true;
        for (int s = 0; s < 4; ++s)
            c.udp_last_rumble_seq[s] = c.rumble_seq[s];
    }
}

void flush_rumble_to_udp(int sock, int client_idx) {
    if (sock < 0 || client_idx < 0 || client_idx >= MAX_CLIENTS) return;
    sockaddr_in dest{};
    RumblePacket pending[4]{};
    bool has[4]{};
    {
        std::lock_guard<std::mutex> lk(g_mtx[client_idx]);
        ClientSession& c = g_clients[client_idx];
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
        ssize_t sent = sendto(sock, &pending[s], sizeof(RumblePacket), 0,
                              reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
        if (g_verbose && sent != (ssize_t)sizeof(RumblePacket))
            std::fprintf(stderr, "[udp] failed to send rumble packet: %s\n", std::strerror(errno));
    }
}

bool send_ws_binary_frame(WebClient* c, const uint8_t* payload, size_t len) {
    if (!c || c->state != WebClient::WS_ACTIVE || c->fd < 0) return false;
    if (c->wbuf != nullptr) return false;
    if (len >= 126) return false;

    const size_t hdr = 2;
    const size_t total = hdr + len;
    uint8_t small_frame[2 + sizeof(RumblePacket)] = {};
    if (total > sizeof(small_frame)) return false;

    small_frame[0] = 0x82;
    small_frame[1] = (uint8_t)len;
    memcpy(small_frame + hdr, payload, len);

    ssize_t w = write(c->fd, small_frame, total);
    if (w == (ssize_t)total) return true;

    size_t written = 0;
    if (w < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) return false;
    } else if (w > 0) {
        written = (size_t)w;
    }

    c->wbuf = (uint8_t*)malloc(total);
    if (!c->wbuf) return false;
    memcpy(c->wbuf, small_frame, total);
    c->wlen = total;
    c->woff = written;
    c->after_write = WebClient::WS_ACTIVE;
    return true;
}

void flush_rumble_to_ws(WebClient* c) {
    if (!c || c->state != WebClient::WS_ACTIVE || c->ws_slot < 0) return;

    RumblePacket pending[4]{};
    uint32_t seqs[4]{};
    bool has[4]{};

    {
        std::lock_guard<std::mutex> lk(g_mtx[c->ws_slot]);
        for (int s = 0; s < 4; ++s) {
            uint32_t seq = g_clients[c->ws_slot].rumble_seq[s];
            if (seq != c->last_rumble_seq[s]) {
                pending[s] = g_clients[c->ws_slot].rumble[s];
                seqs[s] = seq;
                has[s] = true;
            }
        }
    }

    for (int s = 0; s < 4; ++s) {
        if (!has[s]) continue;
        if (send_ws_binary_frame(c, (const uint8_t*)&pending[s], sizeof(RumblePacket))) {
            c->last_rumble_seq[s] = seqs[s];
        }
    }
}


// ── Process one complete WebSocket frame from client buffer ──────────────────
// Returns bytes consumed, or 0 if need more data.  Sets c->state = CLOSED on close/error.
size_t process_ws_frame(WebClient *c) {
    uint8_t *buf = c->buf;
    size_t len  = c->fill;
    if (len < 2) return 0;

    int      opcode  = buf[0] & 0x0F;
    bool     masked  = buf[1] & 0x80;
    uint64_t flen    = buf[1] & 0x7F;
    size_t   hdr_sz  = 2;

    if (flen == 126) {
        if (len < 4) return 0;
        flen   = ((uint64_t)buf[2] << 8) | buf[3];
        hdr_sz = 4;
    } else if (flen == 127) {
        if (len < 10) return 0;
        flen = 0;
        for (int i = 0; i < 8; i++) flen = (flen << 8) | buf[2 + i];
        hdr_sz = 10;
    }

    uint8_t mask[4] = {0};
    if (masked) {
        if (len < hdr_sz + 4) return 0;
        memcpy(mask, buf + hdr_sz, 4);
        hdr_sz += 4;
    }

    if (len < hdr_sz + flen) return 0;

    uint8_t *payload = buf + hdr_sz;
    size_t   total   = hdr_sz + flen;

    if (opcode == 9) {
        if (masked)
            for (uint64_t i = 0; i < flen; i++) payload[i] ^= mask[i & 3];
        uint8_t pong[2] = {0x8A, 0x00};
        ssize_t _u = write(c->fd, pong, 2); (void)_u;
        return total;
    }
    if (opcode == 8) {
        uint8_t close_frame[] = {0x88, 0x00};
        ssize_t _u = write(c->fd, close_frame, 2); (void)_u;
        c->state = WebClient::CLOSED;
        return total;
    }

    if (opcode == 1) {
        if (masked)
            for (uint64_t i = 0; i < flen; i++) payload[i] ^= mask[i & 3];
        std::string text(reinterpret_cast<char*>(payload), (size_t)flen);
        const std::string prefix = "MACRO_RUN:";
        if (text.rfind(prefix, 0) == 0) {
            uint64_t now = now_us();
            if (c->ws_slot < 0) {
                for (int i = 0; i < MAX_CLIENTS; ++i) {
                    std::lock_guard<std::mutex> lk(g_mtx[i]);
                    if (!g_clients[i].active) {
                        c->ws_slot = i;
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
            // Macro commands intentionally do not trigger Switch 2 wake.
            if (c->ws_slot >= 0) {
                {
                    std::lock_guard<std::mutex> lk(g_mtx[c->ws_slot]);
                    g_clients[c->ws_slot].active = true;
                    g_clients[c->ws_slot].uses_pad_presence = true;
                    g_clients[c->ws_slot].pad_present[0] = true;
                    g_clients[c->ws_slot].pad_last_present_us[0] = now;
                    g_clients[c->ws_slot].last_rx_us = now;
                }
                server_macro_start(c->ws_slot, 0, text.substr(prefix.size()));
            }
        }
        return total;
    }
    if (opcode == 0) {
        c->state = WebClient::CLOSED;
        return total;
    }
    if (opcode != 2) return total;

    if (masked)
        for (uint64_t i = 0; i < flen; i++) payload[i] ^= mask[i & 3];

    if (flen >= ns::macro::CHUNK_HEADER_SIZE) {
        uint32_t maybe_macro_magic = 0;
        memcpy(&maybe_macro_magic, payload, 4);
        if (maybe_macro_magic == ns::macro::UDP_CHUNK_MAGIC) {
            uint64_t now = now_us();
            if (c->ws_slot < 0) {
                for (int i = 0; i < MAX_CLIENTS; ++i) {
                    std::lock_guard<std::mutex> lk(g_mtx[i]);
                    if (!g_clients[i].active) {
                        c->ws_slot = i;
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
            // Macro chunk uploads intentionally do not trigger Switch 2 wake.
            if (c->ws_slot >= 0) {
                std::lock_guard<std::mutex> lk(g_mtx[c->ws_slot]);
                g_clients[c->ws_slot].active = true;
                g_clients[c->ws_slot].uses_pad_presence = true;
                g_clients[c->ws_slot].pad_present[0] = true;
                g_clients[c->ws_slot].pad_last_present_us[0] = now;
                g_clients[c->ws_slot].last_rx_us = now;
            }
            if (c->ws_slot >= 0) server_macro_handle_ws_chunk_packet(c->ws_slot, payload, (size_t)flen);
            return total;
        }
    }

    if (flen != PACKET_SIZE && flen != WEB_PACKET_SIZE && flen != WEB_PACKET3_SIZE) {
        c->state = WebClient::CLOSED;
        return total;
    }

    uint32_t magic; memcpy(&magic, payload, 4);
    if (magic != PROTO_MAGIC) return total;
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
        return total;
    }

    if (!c->ws_first && !is_reset && (int32_t)(seq - c->ws_seq) < 0) return total;
    c->ws_first = false;
    c->ws_seq = seq + 1;

    uint64_t now = now_us();
    bool wake_on_new_client = false;

    if (c->ws_slot >= 0) {
        std::lock_guard<std::mutex> lk(g_mtx[c->ws_slot]);
        if (!g_clients[c->ws_slot].active)
            c->ws_slot = -1;
    }
    if (c->ws_slot < 0) {
        for (int i = 0; i < MAX_CLIENTS; ++i) {
            std::lock_guard<std::mutex> lk(g_mtx[i]);
            if (!g_clients[i].active) {
                c->ws_slot = i;
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
                    c->last_rumble_seq[s] = g_clients[i].rumble_seq[s];
                break;
            }
        }
    }
    if (wake_on_new_client)
        maybe_send_switch2_wake_advert("client connected via WebSocket input");
    if (c->ws_slot >= 0) {
        std::lock_guard<std::mutex> lk(g_mtx[c->ws_slot]);

        ExtendedHIDReport* dst_pads[4] = {
            &g_clients[c->ws_slot].report.p1,
            &g_clients[c->ws_slot].report.p2,
            &g_clients[c->ws_slot].report.p3,
            &g_clients[c->ws_slot].report.p4,
        };
        const ExtendedHIDReport* src_pads[4] = {
            &report.p1, &report.p2, &report.p3, &report.p4,
        };
        const ExtendedHIDReport3* src_pads3[4] = {
            &report3.p1, &report3.p2, &report3.p3, &report3.p4,
        };

        if (is_reset) {
            g_clients[c->ws_slot].report.reset();
            clear_all_motion(g_clients[c->ws_slot]);
            for (int s = 0; s < 4; ++s) {
                g_clients[c->ws_slot].pad_present[s] = false;
                g_clients[c->ws_slot].pad_last_present_us[s] = 0;
            }
        } else {
            for (int s = 0; s < 4; ++s) {
                if (pad_present[s]) {
                    *dst_pads[s] = *src_pads[s];
                    if (is_report3) {
                        if (src_pads3[s]->has_motion)
                            set_motion_samples(g_clients[c->ws_slot], s, src_pads3[s]->motion);
                        else
                            clear_motion(g_clients[c->ws_slot], s);
                    } else {
                        if (src_pads[s]->has_motion)
                            set_motion(g_clients[c->ws_slot], s, src_pads[s]->motion);
                        else
                            clear_motion(g_clients[c->ws_slot], s);
                    }
                    g_clients[c->ws_slot].pad_present[s] = true;
                    g_clients[c->ws_slot].pad_last_present_us[s] = now;
                } else {
                    g_clients[c->ws_slot].pad_present[s] = false;
                    uint64_t last_seen = g_clients[c->ws_slot].pad_last_present_us[s];
                    if (last_seen == 0 || now - last_seen >= WEB_PAD_ABSENT_RELEASE_US) {
                        dst_pads[s]->reset();
                        clear_motion(g_clients[c->ws_slot], s);
                    }
                }
            }
        }
        g_clients[c->ws_slot].last_rx_us = now;
    }
    c->ws_last_rx = now;
    ++g_pkts_rx;

    return total;
}


// ── Perform WebSocket upgrade handshake ──────────────────────────────────────
// Returns response length, or -1 on failure.  Caller must queue via async write.
int ws_upgrade(const char *key_line, char *resp, size_t resp_sz) {
    const char *key_start = nullptr;
    const char *header_name = "sec-websocket-key:";
    const char *p = key_line;
    while (*p) {
        const char *h = header_name;
        const char *b = p;
        while (*h && *b && (tolower((unsigned char)*b) == tolower((unsigned char)*h))) {
            b++; h++;
        }
        if (!*h) { key_start = b; break; }
        p++;
    }

    if (!key_start) return -1;

    while (*key_start == ' ') key_start++;
    const char *key_end = strchr(key_start, '\r');
    if (!key_end) key_end = strchr(key_start, '\n');
    if (!key_end) return -1;

    char key[256];
    size_t klen = key_end - key_start;
    if (klen >= sizeof(key)) return -1;
    memcpy(key, key_start, klen);
    key[klen] = '\0';

    const char *magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    uint8_t sha_input[256];
    size_t slen = snprintf((char*)sha_input, sizeof(sha_input), "%s%s", key, magic);

    Sha1Ctx ctx;
    sha1_init(&ctx);
    sha1_update(&ctx, sha_input, slen);
    uint8_t digest[20];
    sha1_final(&ctx, digest);

    char b64out[64];
    base64_encode(digest, 20, b64out);

    return snprintf(resp, resp_sz,
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n"
        "\r\n", b64out);
}


// ── Format HTML body (heap-allocated, caller must free) ──────────────────────
// ── Case-insensitive header check ───────────────────────────────────────────
bool has_header(const char *buf, const char *header) {
    size_t hlen = strlen(header);
    const char *p = buf;
    while (*p) {
        if ((p == buf || p[-1] == '\n') &&
            strncasecmp(p, header, hlen) == 0)
            return true;
        p = strchr(p, '\n');
        if (!p) break;
        p++;
    }
    return false;
}

// ── Request-line prefix match (line-start only) ──────────────────────────────
bool req_match(const char *buf, const char *path) {
    size_t plen = strlen(path);
    const char *p = buf;
    while (*p) {
        if ((p == buf || p[-1] == '\n') &&
            strncmp(p, path, plen) == 0)
            return true;
        p = strchr(p, '\n');
        if (!p) break;
        p++;
    }
    return false;
}


// ── WebSocket/Web Server Thread (single-threaded poll reactor, fully non-blocking) ─────
void web_server_thread(int web_port, uint16_t udp_port, bool serve_http_webapp) {
    (void)udp_port;
    int srv = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (srv < 0) { perror("web socket"); return; }

    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(web_port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(srv, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("web bind"); close(srv); return; }
    if (listen(srv, 8) < 0) { perror("web listen"); close(srv); return; }

    if (serve_http_webapp)
        std::printf("[web] HTTP webapp + WebSocket proxy listening on port %d\n", web_port);
    else
        std::printf("[ws] WebSocket proxy listening on port %d; HTTP webapp disabled (use -w to enable)\n", web_port);

    struct pollfd pfds[1 + MAX_WS_CLIENTS];
    static WebClient clients[MAX_WS_CLIENTS];
    int           n_clients = 0;

    pfds[0].fd = srv; pfds[0].events = POLLIN; pfds[0].revents = 0;
    for (int i = 0; i < MAX_WS_CLIENTS; i++) { pfds[i+1].fd = -1; }

    while (g_running.load(std::memory_order_relaxed)) {
        // Push pending classic NSVR rumble events back to browser/mobile WebSocket clients.
        for (int i = 0; i < n_clients; i++)
            if (clients[i].state == WebClient::WS_ACTIVE)
                flush_rumble_to_ws(&clients[i]);

        // Periodic WebSocket ping every 10s so clients detect dead connections.
        uint64_t now_ws = now_us();
        for (int i = 0; i < n_clients; i++) {
            if (clients[i].state == WebClient::WS_ACTIVE &&
                now_ws - clients[i].last_ping_us >= 10000000 &&
                clients[i].wbuf == nullptr) {
                uint8_t ping[2] = {0x89, 0x00};
                ssize_t n = write(clients[i].fd, ping, sizeof(ping));
                if (n == (ssize_t)sizeof(ping))
                    clients[i].last_ping_us = now_ws;
            }
        }

        // Idle WS timeout (30s) and HTTP handshake timeout (5s)
        for (int i = 0; i < n_clients; i++) {
            if (clients[i].state == WebClient::WS_ACTIVE &&
                now_ws - clients[i].ws_last_rx > 30000000)
                clients[i].state = WebClient::CLOSED;
            if (clients[i].state == WebClient::READ_HTTP &&
                clients[i].connect_time > 0 &&
                now_ws - clients[i].connect_time > 5000000)
                clients[i].state = WebClient::CLOSED;
        }

        // Update poll events based on client state (POLLOUT for write, POLLIN for read)
        for (int i = 0; i < n_clients; i++) {
            if (clients[i].state != WebClient::CLOSED) {
                if (clients[i].state == WebClient::WRITE_RESP)
                    pfds[i+1].events = POLLOUT;
                else if (clients[i].state == WebClient::WS_ACTIVE && clients[i].wbuf != nullptr)
                    pfds[i+1].events = POLLIN | POLLOUT;
                else
                    pfds[i+1].events = POLLIN;
                pfds[i+1].revents = 0;
            }
        }

        int rc = poll(pfds, 1 + n_clients, 200);
        if (rc <= 0) continue;

        // ── Accept new connections ─────────────────────────────────────────
        if (pfds[0].revents & POLLIN) {
            sockaddr_in peer{};
            socklen_t   plen = sizeof(peer);
            int fd = accept(srv, (sockaddr*)&peer, &plen);
            if (fd >= 0) {
                uint32_t peer_ip = peer.sin_addr.s_addr;
                int cnt = 0;
                for (int i = 0; i < n_clients; i++)
                    if (clients[i].state != WebClient::CLOSED && clients[i].ip == peer_ip)
                        cnt++;
                if (cnt >= 8) {
                    close(fd);
                    if (g_verbose) std::printf("[web] client rejected: %d connections from this IP\n", cnt);
                    continue;
                }

                fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
                struct timeval tv = {10, 0};
                setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

                int slot = -1;
                for (int i = 0; i < MAX_WS_CLIENTS; i++) {
                    if (clients[i].state == WebClient::CLOSED) { slot = i; break; }
                }
                if (slot >= 0) {
                    clients[slot] = WebClient{};
                    clients[slot].fd = fd;
                    clients[slot].state = WebClient::READ_HTTP;
                    clients[slot].connect_time = now_us();
                    clients[slot].ip = peer_ip;
                    pfds[slot+1].fd = fd;
                    pfds[slot+1].events = POLLIN;
                    pfds[slot+1].revents = 0;
                    if (slot >= n_clients) n_clients = slot + 1;
                    if (g_verbose) std::printf("[web] client %d accepted (slot %d)\n", fd, slot);
                } else {
                    close(fd);
                    if (g_verbose) std::puts("[web] rejected: all slots full");
                }
            }
        }

        // ── Service existing clients ───────────────────────────────────────
        for (int i = 0; i < n_clients; i++) {
            WebClient *c = &clients[i];
            if (c->state == WebClient::CLOSED) continue;
            short rev = pfds[i+1].revents;
            if (rev & (POLLHUP | POLLERR)) { c->state = WebClient::CLOSED; continue; }

            // ── WRITE_RESP: flush queued HTTP response ──────────────────────
            if (c->state == WebClient::WRITE_RESP && (rev & POLLOUT)) {
                ssize_t n = write(c->fd, c->wbuf + c->woff, c->wlen - c->woff);
                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                        continue; // will retry on next poll cycle
                    c->state = WebClient::CLOSED;
                } else {
                    c->woff += n;
                    if (c->woff >= c->wlen) {
                        free(c->wbuf);
                        c->wbuf = nullptr;
                        c->state = c->after_write;
                    }
                }
                continue;
            }

            // ── WS_ACTIVE: flush queued server→browser binary frames ─────────
            if (c->state == WebClient::WS_ACTIVE && c->wbuf != nullptr && (rev & POLLOUT)) {
                ssize_t n = write(c->fd, c->wbuf + c->woff, c->wlen - c->woff);
                if (n < 0) {
                    if (!(errno == EAGAIN || errno == EWOULDBLOCK))
                        c->state = WebClient::CLOSED;
                } else {
                    c->woff += n;
                    if (c->woff >= c->wlen) {
                        free(c->wbuf);
                        c->wbuf = nullptr;
                        c->wlen = c->woff = 0;
                    }
                }
                if (c->state == WebClient::CLOSED) continue;
            }

            if (!(rev & POLLIN)) continue;

            // Read available data (defensive: check for buffer full first)
            if (c->fill >= sizeof(c->buf)) { c->state = WebClient::CLOSED; continue; }
            ssize_t n = read(c->fd, c->buf + c->fill, sizeof(c->buf) - c->fill);
            if (n <= 0) { c->state = WebClient::CLOSED; continue; }
            c->fill += n;

            // ── READ_HTTP: accumulate headers, then dispatch ────────────────
            if (c->state == WebClient::READ_HTTP) {
                size_t copy = std::min(sizeof(c->http_buf) - 1 - c->http_len, c->fill);
                if (copy == 0) { c->state = WebClient::CLOSED; continue; }
                memcpy(c->http_buf + c->http_len, c->buf, copy);
                c->http_len += copy;
                memmove(c->buf, c->buf + copy, c->fill - copy);
                c->fill -= copy;
                c->http_buf[c->http_len] = '\0';

                if (c->http_len >= 4 &&
                    c->http_buf[c->http_len-1] == '\n' &&
                    c->http_buf[c->http_len-2] == '\r' &&
                    c->http_buf[c->http_len-3] == '\n' &&
                    c->http_buf[c->http_len-4] == '\r')
                {
                    bool is_ws = has_header(c->http_buf, "upgrade: websocket") &&
                                 has_header(c->http_buf, "sec-websocket-key:");
                    if (is_ws) {
                        // Queue WS upgrade response via async write (never block on non-blocking socket)
                        char resp[512];
                        int n = ws_upgrade(c->http_buf, resp, sizeof(resp));
                        if (n > 0) {
                            c->wbuf = (uint8_t*)malloc(n);
                            if (c->wbuf) {
                                memcpy(c->wbuf, resp, n);
                                c->wlen = n;
                                c->woff = 0;
                                c->after_write = WebClient::WS_ACTIVE;
                                c->state = WebClient::WRITE_RESP;
                                c->ws_first = true;
                                c->ws_seq = 0;
                                c->ws_slot = -1;
                                c->ws_last_rx = now_us();
                                if (g_verbose) std::puts("[web] WS upgrade queued");
                            } else {
                                c->state = WebClient::CLOSED;
                            }
                        } else {
                            if (g_verbose) std::puts("[web] WS upgrade failed");
                            c->state = WebClient::CLOSED;
                        }
                    } else {
                        // Build HTTP response and queue it for non-blocking write
                        const char *body = nullptr;
                        size_t body_len = 0;
                        int status = 200;
                        const char *status_str = "OK";

                        if (!serve_http_webapp) {
                            body = "WebSocket only\n";
                            body_len = strlen(body);
                            status = 404;
                            status_str = "Not Found";
                        } else if (req_match(c->http_buf, "GET / ") ||
                                   req_match(c->http_buf, "GET /index.html ")) {
                            body = (const char*)index_html;
                            body_len = index_html_len - 1;
                        } else if (req_match(c->http_buf, "GET /mobile.html ")) {
                            body = (const char*)mobile_html;
                            body_len = mobile_html_len - 1;
                        } else if (req_match(c->http_buf, "GET /editor.html ")) {
                            body = (const char*)editor_html;
                            body_len = editor_html_len - 1;
                        } else {
                            body = "Not Found";
                            body_len = 9;
                            status = 404;
                            status_str = "Not Found";
                        }

                        if (!body) { c->state = WebClient::CLOSED; continue; }

                        char hdr[512];
                        int hdr_len = snprintf(hdr, sizeof(hdr),
                            "HTTP/1.1 %d %s\r\n"
                            "Content-Type: text/html; charset=utf-8\r\n"
                            "Content-Length: %zu\r\n"
                            "Connection: close\r\n"
                            "Cache-Control: no-cache\r\n"
                            "\r\n", status, status_str, body_len);

                        c->wbuf = (uint8_t*)malloc(hdr_len + body_len);
                        memcpy(c->wbuf, hdr, hdr_len);
                        memcpy(c->wbuf + hdr_len, body, body_len);
                        c->wlen = hdr_len + body_len;
                        c->woff = 0;
                        c->after_write = WebClient::CLOSED;
                        c->state = WebClient::WRITE_RESP;

                        if (g_verbose) std::printf("[web] HTTP %d queued (%zu bytes)\n", status, c->wlen);
                    }
                }
                continue;
            }

            // ── WS_ACTIVE: process WebSocket frames ─────────────────────────
            if (c->state == WebClient::WS_ACTIVE) {
                size_t used;
                do {
                    used = process_ws_frame(c);
                    if (used > 0) {
                        memmove(c->buf, c->buf + used, c->fill - used);
                        c->fill -= used;
                    }
                } while (used > 0 && c->state == WebClient::WS_ACTIVE);
            }
        }

        // ── Cleanup closed clients ────────────────────────────────────────
        for (int i = 0; i < n_clients; i++) {
            if (clients[i].state == WebClient::CLOSED && clients[i].fd >= 0) {
                free(clients[i].wbuf);
                clients[i].wbuf = nullptr;
                bool released_input_slot = false;
                if (clients[i].ws_slot >= 0) {
                    std::lock_guard<std::mutex> lk(g_mtx[clients[i].ws_slot]);
                    if (g_clients[clients[i].ws_slot].last_rx_us == clients[i].ws_last_rx) {
                        g_clients[clients[i].ws_slot].active = false;
                        g_clients[clients[i].ws_slot].report.reset();
                        clear_all_motion(g_clients[clients[i].ws_slot]);
                        g_clients[clients[i].ws_slot].uses_pad_presence = false;
                        for (int s = 0; s < 4; ++s) {
                            g_clients[clients[i].ws_slot].pad_present[s] = false;
                            g_clients[clients[i].ws_slot].pad_last_present_us[s] = 0;
                        }
                        released_input_slot = true;
                    }
                }
                if (released_input_slot)
                    rearm_switch2_wake_after_client_disconnect();
                if (g_verbose) std::printf("[web] client %d closed\n", clients[i].fd);
                close(clients[i].fd);
                clients[i].fd = -1;
                pfds[i+1].fd = -1;
            }
        }

        // Shrink n_clients
        while (n_clients > 0 && pfds[n_clients].fd == -1)
            n_clients--;
    }

    // Final cleanup
    for (int i = 0; i < n_clients; i++) {
        if (clients[i].fd >= 0) {
            free(clients[i].wbuf);
            bool released_input_slot = false;
            if (clients[i].ws_slot >= 0) {
                std::lock_guard<std::mutex> lk(g_mtx[clients[i].ws_slot]);
                if (g_clients[clients[i].ws_slot].last_rx_us == clients[i].ws_last_rx) {
                    g_clients[clients[i].ws_slot].active = false;
                    g_clients[clients[i].ws_slot].report.reset();
                    clear_all_motion(g_clients[clients[i].ws_slot]);
                    g_clients[clients[i].ws_slot].uses_pad_presence = false;
                    clear_udp_rumble_state(g_clients[clients[i].ws_slot]);
                    for (int s = 0; s < 4; ++s) {
                        g_clients[clients[i].ws_slot].pad_present[s] = false;
                        g_clients[clients[i].ws_slot].pad_last_present_us[s] = 0;
                    }
                    released_input_slot = true;
                }
            }
            if (released_input_slot)
                rearm_switch2_wake_after_client_disconnect();
            close(clients[i].fd);
        }
    }
    close(srv);
    std::printf("[web] server stopped\n");
}
