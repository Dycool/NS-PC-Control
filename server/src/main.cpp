#include "shared/protocol.hpp"
#include "shared/macros.hpp"
#include "shared/sha256.h"

#include <atomic>
#include <thread>
#include <mutex>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <string>
#include <algorithm>
#include <vector>
#include <cstdint>
#include <sstream>
#include <fstream>
#include <iostream>

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <dirent.h>
#include <ctype.h>

#ifdef USE_UPNP
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#include <miniupnpc/upnperrors.h>
#include <stdexcept>
#include <limits>
#endif

using namespace ns;
using Clock = std::chrono::steady_clock;
using us    = std::chrono::microseconds;
using ms    = std::chrono::milliseconds;

#include "app_state.hpp"

// ── Signal ────────────────────────────────────────────────────────────────────
static void on_signal(int) { g_running.store(false, std::memory_order_relaxed); }

#include "virtual_controller.hpp"
#include "gadget_wakeup.hpp"
#include "writers.hpp"
#include "upnp.hpp"
#include "web_server.hpp"
#include "bluetooth_input.hpp"

// ══════════════════════════════════════════════════════════════════════════════
// ── UDP receive loop (main thread) ────────────────────────────────────────────
static bool parse_bind_arg(const std::string& raw, std::string& bind_addr, uint16_t& port) {
    if (raw.empty()) return false;

    uint32_t numeric_port = 0;
    if (ns::macro::parse_uint32_strict(raw, numeric_port)) {
        if (numeric_port > 65535) return false;
        bind_addr = "0.0.0.0";
        port = (uint16_t)numeric_port;
        return true;
    }

    std::string addr = raw;
    size_t sep = raw.rfind(':');
    if (sep != std::string::npos) {
        std::string port_text = raw.substr(sep + 1);
        uint32_t parsed_port = 0;
        if (!ns::macro::parse_uint32_strict(port_text, parsed_port) || parsed_port > 65535)
            return false;

        addr = raw.substr(0, sep);
        port = (uint16_t)parsed_port;
    }

    if (addr.empty()) addr = "0.0.0.0";
    bind_addr = addr;
    return true;
}

int main(int argc, char** argv) {
    uint16_t    port      = DEFAULT_PORT;
    std::string bind_addr = "0.0.0.0";
    bool        do_upnp   = false;
    int         web_port  = 8080; // WebSocket server is enabled by default
    bool        serve_http_webapp = false;
    bool        bluetooth_enabled = false;
    bool        bt_explicit       = false;

    for (int i = 1; i < argc; ++i) {
        std::string a(argv[i]);
        if (a == "-p") {
            std::fprintf(stderr, "error: -p was removed; use -b PORT or -b ADDR:PORT instead\n");
            return 1;
        }
        else if (a == "-b") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: -b requires ADDR, PORT, or ADDR:PORT\n");
                return 1;
            }
            if (!parse_bind_arg(argv[++i], bind_addr, port)) {
                std::fprintf(stderr, "error: invalid bind value; use -b ADDR, -b PORT, or -b ADDR:PORT\n");
                return 1;
            }
        }
        else if (a == "-v")               g_verbose  = true;
        else if (a == "-wake")          g_switch2_wakeup_setup_requested = true;
        else if (a == "-hori")          g_legacy_mode = true;
        else if (a == "-bt")            { bluetooth_enabled = true; bt_explicit = true; }
        else if (a == "--upnp")           do_upnp    = true;
        else if (a == "-w") {
            serve_http_webapp = true;
            if (i+1 < argc && argv[i+1][0] >= '0' && argv[i+1][0] <= '9')
                web_port = std::atoi(argv[++i]);
            else
                web_port = 8080;
        }
        else if (a == "-h") {
            puts("ns-backend  [-b ADDR[:PORT]|PORT] [--upnp] [-w [WEB_PORT]] [-v] [-hori] [-wake] [-bt]");
            puts("");
            puts("  By default, UDP and WebSocket input are both enabled.");
            puts("  WebSocket listens on port 8080 and does not serve the browser webapp.");
            puts("");
            puts("  -b ADDR[:PORT]  Bind UDP to an address and optional port.");
            puts("  -b PORT         Keep 0.0.0.0 with a custom UDP port.");
            puts("  -w [PORT]       Serve the browser webapp too, using this port or 8080.");
            puts("  --upnp          Forward the UDP port via UPnP for PC clients only.");
            puts("                  Mobile/web clients connect via WebSocket and don't need this.");
            puts("  -wake           Run interactive Joy-Con 2 wake setup, save switch2_wakeup.conf, test wake, then exit.");
            puts("  -bt             Explicitly enable local SDL3 Bluetooth/controller input and disable Switch 2 wake.");
            puts("                  By default, the backend auto-detects: Bluetooth is used unless a valid");
            puts("                  Switch 2 wake config is present at /etc/ns-pc-control/switch2_wakeup.conf.");
            puts("  -hori           Expose the legacy 8-byte HORI controller gadget.");
            puts("                  Default mode exposes the 64-byte motion/rumble gadget.");
            puts("");
            return 0;
        }
        else {
            std::fprintf(stderr, "error: unknown argument: %s\n", a.c_str());
            return 1;
        }
    }

    // -wake setup mode: run interactive config and exit (takes priority)
    if (g_switch2_wakeup_setup_requested)
        return run_switch2_wakeup_setup();

    // Determine operating mode:
    //   * -bt  →  Bluetooth mode, wake disabled (explicit user choice)
    //   * else →  auto-detect: wake mode if config exists, Bluetooth mode otherwise
    if (bt_explicit) {
        g_switch2_wake_adv_enabled = false;
        if (!bluetooth_input_available()) {
            std::fprintf(stderr, "error: -bt requested, but ns-backend was built without SDL3 support\n");
            return 1;
        }
    } else {
        g_switch2_wake_adv_enabled = load_switch2_wakeup_config(true);
        if (g_switch2_wake_adv_enabled) {
            bluetooth_enabled = false;
            enter_switch2_wake_runtime_mode();
            if (g_verbose)
                std::printf("[wake] Switch 2 wake config loaded; wake mode active, Bluetooth disabled\n");
        } else {
            bluetooth_enabled = bluetooth_input_available();
            if (g_verbose && bluetooth_enabled)
                std::printf("[bt] No Switch 2 wake config found; Bluetooth controller mode active\n");
        }
    }

    // If Bluetooth mode is active, warn if rfkill blocks the adapter.
    if (bluetooth_enabled) {
        int rc = std::system("rfkill list bluetooth 2>/dev/null | grep -qi 'blocked: yes'");
        bool blocked = (rc != -1 && WIFEXITED(rc) && WEXITSTATUS(rc) == 0);
        if (blocked) {
            std::fprintf(stderr,
                "[bt] WARNING: Bluetooth controllers connected directly to the Raspberry Pi\n"
                "[bt]          will NOT work — the adapter is blocked by rfkill.\n"
                "[bt]          Unblock it with: sudo rfkill unblock bluetooth\n");
        }
    }

    randomize_controller_identity();

    // Always recreate the built-in gadget at process startup.  This makes every
    // launch self-healing: stale configfs state, leftover /dev/hidg* nodes, or a
    // previous unclean shutdown are cleared before the backend starts talking to
    // the console.
    run_gadget_setup_if_needed(true, "startup gadget recreation requested");

    derive_key(DEFAULT_SECRET, g_hmac_key);
    signal(SIGINT,  on_signal); signal(SIGTERM, on_signal); signal(SIGPIPE, SIG_IGN);

    if (do_upnp) upnp_add_mapping(port);

    int sock = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (sock < 0) { perror("socket"); return 1; }

    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    int rbuf = 2 * 1024 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rbuf, sizeof(rbuf));

    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(port);
    if (inet_pton(AF_INET, bind_addr.c_str(), &addr.sin_addr) != 1) {
        std::fprintf(stderr, "error: invalid IPv4 bind address: %s\n", bind_addr.c_str());
        close(sock);
        return 1;
    }
    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); close(sock); return 1; }

    // Start the WebSocket proxy after UDP is bound. The HTTP webapp is only
    // served when -w is passed.
    std::thread web_thread(web_server_thread, web_port, port, serve_http_webapp);
    std::thread bluetooth_thread;
    if (bluetooth_enabled)
        bluetooth_thread = std::thread(bluetooth_input_thread);
    
    std::printf("UDP %s:%u writer=%d Hz mode=%s\n",
                bind_addr.c_str(), port, PRO_WRITER_HZ,
                g_legacy_mode ? "hori" : "modern");
    std::thread wt(writer_thread, PRO_WRITER_HZ);
    std::thread st(stats_thread);

    int ep = epoll_create1(0); epoll_event ev{}; ev.events = EPOLLIN; ev.data.fd = sock; epoll_ctl(ep, EPOLL_CTL_ADD, sock, &ev);

    std::vector<uint8_t> udp_rx(std::max(UDP_RX_MAX_PACKET_SIZE, ns::macro::CHUNK_HEADER_SIZE + ns::macro::UDP_CHUNK_MAX + HMAC_TAG_SIZE));
    epoll_event evs[4];

    while (g_running.load(std::memory_order_relaxed)) {
        int n = epoll_wait(ep, evs, 4, 200);
        if (n <= 0) continue;

        sockaddr_in sender{};
        socklen_t slen;
        ssize_t bytes;

        // Drain all available packets from the kernel buffer.
        // Two UDP packet formats are accepted:
        //   1) legacy Packet: input only, unchanged, authenticated with HMAC.
        //   2) ExtendedUdpPacket: ExtendedMultiReport with motion/gyro, authenticated
        //      with HMAC, and opted into UDP rumble replies.
        while (g_running.load(std::memory_order_relaxed)) {
            slen = sizeof(sender);
            bytes = recvfrom(sock, udp_rx.data(), udp_rx.size(), 0, (sockaddr*)&sender, &slen);
            if (bytes <= 0) break; // EAGAIN or error — ring is drained

            if (bytes == (ssize_t)sizeof(ServerInfoProbe)) {
                ServerInfoProbe probe{};
                memcpy(&probe, udp_rx.data(), sizeof(probe));
                if (probe.magic == SERVER_INFO_MAGIC && probe.version == SERVER_INFO_VERSION) {
                    ServerInfoReply reply{};
                    reply.backend = g_legacy_mode ? SERVER_BACKEND_LEGACY : SERVER_BACKEND_PRO;
                    reply.udp_interval_ms = g_legacy_mode ? LEGACY_UDP_INTERVAL_MS : PRO_UDP_INTERVAL_MS;
                    reply.udp_hz = g_legacy_mode ? LEGACY_UDP_HZ : PRO_UDP_HZ;
                    sendto(sock, &reply, sizeof(reply), 0, (sockaddr*)&sender, slen);
                    continue;
                }
            }


            if (bytes >= 4) {
                uint32_t mmagic = 0;
                memcpy(&mmagic, udp_rx.data(), 4);
                if (mmagic == ns::macro::UDP_CHUNK_MAGIC) {
                    server_macro_handle_chunk_packet(udp_rx.data(), (size_t)bytes, sender);
                    continue;
                }
            }

            if (bytes >= (ssize_t)(ns::macro::UDP_HEADER_SIZE + HMAC_TAG_SIZE)) {
                uint32_t mmagic = 0;
                memcpy(&mmagic, udp_rx.data(), 4);
                if (mmagic == ns::macro::UDP_MAGIC) {
                    ns::macro::MacroUdpHeaderWire mh{};
                    memcpy(&mh, udp_rx.data(), sizeof(mh));
                    uint32_t text_len = mh.text_len;
                    if (text_len <= ns::macro::UDP_TEXT_MAX && bytes == (ssize_t)(ns::macro::UDP_HEADER_SIZE + text_len + HMAC_TAG_SIZE)) {
                        const uint8_t* recv_hmac = udp_rx.data() + ns::macro::UDP_HEADER_SIZE + text_len;
                        if (hmac_verify(g_hmac_key, 32, udp_rx.data(), ns::macro::UDP_HEADER_SIZE + text_len, recv_hmac, HMAC_TAG_SIZE) == 0) {
                            if (!rate_allow(sender.sin_addr.s_addr)) continue;
                            int client_idx = server_macro_client_for_sender(sender);
                            if (client_idx >= 0) {
                                {
                                    std::lock_guard<std::mutex> lk(g_mtx[client_idx]);
                                    g_clients[client_idx].uses_pad_presence = true;
                                    int sp = mh.subpad < 4 ? mh.subpad : 0;
                                    g_clients[client_idx].pad_present[sp] = true;
                                    g_clients[client_idx].pad_last_present_us[sp] = now_us();
                                }
                                std::string text(reinterpret_cast<char*>(udp_rx.data() + ns::macro::UDP_HEADER_SIZE), text_len);
                                server_macro_start(client_idx, mh.subpad < 4 ? mh.subpad : 0, text);
                            }
                        } else if (g_verbose) puts("bad macro HMAC, dropped");
                    } else if (g_verbose) puts("bad macro packet size, dropped");
                    continue;
                }
            }

            bool is_extended_udp = false;
            bool is_extended_udp3 = false;
            Packet pkt{};
            ExtendedUdpPacket ext_pkt{};
            ExtendedUdpPacket3 ext3_pkt{};

            if (bytes == (ssize_t)PACKET_SIZE) {
                memcpy(&pkt, udp_rx.data(), sizeof(pkt));
            } else if (bytes == (ssize_t)EXT_UDP_PACKET_SIZE) {
                memcpy(&ext_pkt, udp_rx.data(), sizeof(ext_pkt));
                is_extended_udp = true;
            } else if (bytes == (ssize_t)EXT3_UDP_PACKET_SIZE) {
                memcpy(&ext3_pkt, udp_rx.data(), sizeof(ext3_pkt));
                is_extended_udp3 = true;
            } else {
                if (g_verbose) std::printf("[udp] unexpected packet size=%zd, dropped\n", bytes);
                continue;
            }

            // ── 1. Per-IP rate limiter ────────────────────────────────────────────
            uint32_t src_ip = sender.sin_addr.s_addr;
            if (!rate_allow(src_ip)) {
                if (g_verbose) puts("rate limit exceeded, dropped");
                continue;
            }

            // ── 2. Magic + version check ──────────────────────────────────────────
            if (is_extended_udp) {
                if (!extended_udp_packet_ok(ext_pkt)) {
                    if (g_verbose) puts("bad extended UDP magic/version, dropped");
                    continue;
                }
            } else if (is_extended_udp3) {
                if (!extended_udp3_packet_ok(ext3_pkt)) {
                    if (g_verbose) puts("bad extended UDP v3 magic/version, dropped");
                    continue;
                }
            } else if (!packet_ok(pkt)) {
                if (g_verbose) puts("bad magic/version, dropped");
                continue;
            }

            // ── 3. Find Client Session or Pin new IP:port ─────────────────────────
            int client_idx = -1;
            uint64_t now = now_us();
            bool wake_on_new_client = false;

            for (int i = 0; i < MAX_CLIENTS; ++i) {
                std::lock_guard<std::mutex> lk(g_mtx[i]);
                if (g_clients[i].active &&
                    g_clients[i].addr.sin_addr.s_addr == src_ip &&
                    g_clients[i].addr.sin_port == sender.sin_port) {
                    client_idx = i;
                    break;
                }
            }

            // If not found, assign to a free/timed-out slot.
            if (client_idx == -1) {
                for (int i = 0; i < MAX_CLIENTS; ++i) {
                    std::lock_guard<std::mutex> lk(g_mtx[i]);
                    if (!g_clients[i].active || elapsed_us_over(now, g_clients[i].last_rx_us, CLIENT_TIMEOUT_US)) {
                        client_idx = i;
                        g_clients[i].active = true;
                        g_clients[i].addr = sender;
                        g_clients[i].first_pkt = true;
                        g_clients[i].expected_seq = 0;
                        g_clients[i].report.reset();
                        clear_all_motion(g_clients[i]);
                        g_clients[i].uses_pad_presence = false;
                        clear_udp_rumble_state(g_clients[i]);
                        for (int s = 0; s < 4; ++s) {
                            g_clients[i].pad_present[s] = false;
                            g_clients[i].pad_last_present_us[s] = 0;
                        }
                        g_clients[i].last_rx_us = now;
                        wake_on_new_client = true;
                        if (g_verbose) std::printf("New UDP client accepted into Server Slot %d/4\n", i+1);
                        break;
                    }
                }
            }

            // Do not send wake yet. A packet from a new endpoint might still be
            // invalid or it might be an explicit disconnect packet. Wake must be
            // tied to a real client connection/input packet, never to disconnect
            // cleanup.

            // If all 4 slots are taken by active PCs, drop the packet.
            if (client_idx == -1) {
                if (g_verbose) puts("server is full (4 PCs already active), dropped");
                continue;
            }

            // ── 4. HMAC authentication ────────────────────────────────────────────
            int hmac_ok = 0;
            if (is_extended_udp) {
                hmac_ok = hmac_verify(g_hmac_key, 32,
                                      reinterpret_cast<const uint8_t*>(&ext_pkt),
                                      EXT_UDP_PACKET_AUTH_SIZE,
                                      ext_pkt.hmac,
                                      HMAC_TAG_SIZE);
            } else if (is_extended_udp3) {
                hmac_ok = hmac_verify(g_hmac_key, 32,
                                      reinterpret_cast<const uint8_t*>(&ext3_pkt),
                                      EXT3_UDP_PACKET_AUTH_SIZE,
                                      ext3_pkt.hmac,
                                      HMAC_TAG_SIZE);
            } else {
                hmac_ok = hmac_verify(g_hmac_key, 32,
                                      reinterpret_cast<const uint8_t*>(&pkt),
                                      PACKET_AUTH_SIZE,
                                      pkt.hmac,
                                      HMAC_TAG_SIZE);
            }
            if (hmac_ok != 0) {
                if (g_verbose) puts("bad HMAC, dropped");
                continue;
            }

            uint8_t packet_flags = is_extended_udp3 ? ext3_pkt.flags : (is_extended_udp ? ext_pkt.flags : pkt.flags);
            if (packet_flags & FLAG_DISCONNECT) {
                server_macro_stop_all_for_client(client_idx);
                {
                    std::lock_guard<std::mutex> lk(g_mtx[client_idx]);
                    reset_udp_client_session_locked(g_clients[client_idx]);
                }
                rearm_switch2_wake_after_client_disconnect();
                if (g_verbose) std::printf("UDP client %d sent disconnect and was released.\n", client_idx + 1);
                ++g_pkts_rx;
                continue;
            }

            if (wake_on_new_client)
                maybe_send_switch2_wake_advert("client connected via UDP input");

            // ── 5. Sequence counter + Apply to shared state ───────────────────────
            bool accepted = false;
            {
                std::lock_guard<std::mutex> lk(g_mtx[client_idx]);

                // Re-validate: writer may have deactivated the slot during HMAC.
                if (!g_clients[client_idx].active) continue;

                uint8_t flags = is_extended_udp3 ? ext3_pkt.flags : (is_extended_udp ? ext_pkt.flags : pkt.flags);
                uint32_t seq = is_extended_udp3 ? ext3_pkt.seq : (is_extended_udp ? ext_pkt.seq : pkt.seq);
                bool is_reset = (flags & FLAG_RESET);
                bool sequence_jump = (g_clients[client_idx].expected_seq > seq) &&
                                     ((g_clients[client_idx].expected_seq - seq) > 100);

                if (!g_clients[client_idx].first_pkt && seq < g_clients[client_idx].expected_seq && !is_reset && !sequence_jump) {
                    if (g_verbose)
                        std::printf("UDP client %d out-of-order seq=%u, dropped\n", client_idx+1, seq);
                    continue;
                }
                g_clients[client_idx].first_pkt = false;
                g_clients[client_idx].expected_seq = seq + 1;

                if (is_reset) {
                    g_clients[client_idx].report.reset();
                    clear_all_motion(g_clients[client_idx]);
                    for (int s = 0; s < 4; ++s) {
                        g_clients[client_idx].pad_present[s] = false;
                        g_clients[client_idx].pad_last_present_us[s] = 0;
                    }
                } else if (is_extended_udp || is_extended_udp3) {
                    // Extended UDP carries motion/gyro and pad-present flags, so
                    // neutral-but-connected pads can still receive rumble.  Version 6
                    // also carries the three Pro-controller IMU samples explicitly.
                    g_clients[client_idx].uses_pad_presence = true;
                    enable_udp_rumble_state(g_clients[client_idx]);

                    ExtendedHIDReport* dst_pads[4] = {
                        &g_clients[client_idx].report.p1,
                        &g_clients[client_idx].report.p2,
                        &g_clients[client_idx].report.p3,
                        &g_clients[client_idx].report.p4,
                    };
                    const ExtendedHIDReport* src_pads[4] = {
                        &ext_pkt.report.p1, &ext_pkt.report.p2,
                        &ext_pkt.report.p3, &ext_pkt.report.p4,
                    };
                    const ExtendedHIDReport3* src_pads3[4] = {
                        &ext3_pkt.report.p1, &ext3_pkt.report.p2,
                        &ext3_pkt.report.p3, &ext3_pkt.report.p4,
                    };

                    for (int s = 0; s < 4; ++s) {
                        bool present = is_extended_udp3 ?
                            extended3_report_pad_present(ext3_pkt.report, s) :
                            extended_report_pad_present(ext_pkt.report, s);
                        if (present) {
                            if (is_extended_udp3) {
                                extended3_to_extended_latest(*src_pads3[s], *dst_pads[s]);
                                if (src_pads3[s]->has_motion)
                                    set_motion_samples(g_clients[client_idx], s, src_pads3[s]->motion);
                                else
                                    clear_motion(g_clients[client_idx], s);
                            } else {
                                *dst_pads[s] = *src_pads[s];
                                if (src_pads[s]->has_motion)
                                    set_motion(g_clients[client_idx], s, src_pads[s]->motion);
                                else
                                    clear_motion(g_clients[client_idx], s);
                            }
                            g_clients[client_idx].pad_present[s] = true;
                            g_clients[client_idx].pad_last_present_us[s] = now;
                        } else {
                            g_clients[client_idx].pad_present[s] = false;
                            uint64_t last_seen = g_clients[client_idx].pad_last_present_us[s];
                            if (last_seen == 0 || now - last_seen >= WEB_PAD_ABSENT_RELEASE_US) {
                                dst_pads[s]->reset();
                                clear_motion(g_clients[client_idx], s);
                            }
                        }
                    }
                } else {
                    // Legacy UDP remains 100% compatible: input-only, no pad-present
                    // tracking, no gyro, and no UDP rumble replies.
                    g_clients[client_idx].uses_pad_presence = false;
                    clear_udp_rumble_state(g_clients[client_idx]);
                    for (int s = 0; s < 4; ++s) {
                        g_clients[client_idx].pad_present[s] = false;
                        g_clients[client_idx].pad_last_present_us[s] = 0;
                    }
                    clear_all_motion(g_clients[client_idx]);
                    legacy_multi_to_extended(pkt.report, g_clients[client_idx].report);
                }

                g_clients[client_idx].last_rx_us = now_us();
                accepted = true;
            }

            if (!accepted) continue;
            ++g_pkts_rx;

            // Extended UDP clients opted into rumble by using the new packet
            // format.  Legacy clients are not sent unexpected traffic.
            if (is_extended_udp || is_extended_udp3) {
                flush_rumble_to_udp(sock, client_idx);
            }
        } // drain loop
    } // epoll loop

    puts("[backend] shutting down");
    upnp_remove_mapping(port);
    close(ep); close(sock);
    wt.join(); st.join();
    if (web_thread.joinable()) web_thread.join();
    if (bluetooth_thread.joinable()) bluetooth_thread.join();

    teardown_gadget();

    return 0;
}
