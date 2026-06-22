#include "shared/protocol.hpp"
#include "shared/macros.hpp"
#include "shared/sha256.h"

#include <print>
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
#include <CLI/CLI.hpp>

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
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
static void on_signal(int) { g_ctx.running.store(false, std::memory_order_relaxed); }

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
    std::vector<std::string> cli_args;
    cli_args.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        cli_args.emplace_back(argv[i] ? argv[i] : "");
        if (cli_args.back() == "-wake") cli_args.back() = "--wake";
        else if (cli_args.back() == "-hori") cli_args.back() = "--hori";
        else if (cli_args.back() == "-bt") cli_args.back() = "--bt";
    }

    uint16_t    port      = DEFAULT_PORT;
    std::string bind_addr = "0.0.0.0";
    bool        do_upnp   = false;
    int         web_port  = 8080; // WebSocket server is enabled by default
    bool        serve_http_webapp = false;
    bool        bluetooth_enabled = false;
    bool        bt_explicit       = false;

    CLI::App app{"ns-backend - Switch Input Server\n\n  By default, UDP and WebSocket input are both enabled.\n  WebSocket listens on port 8080 and does not serve the browser webapp."};

    std::string bind_arg;
    app.add_option("-b", bind_arg, "Bind UDP to an address and optional port (ADDR[:PORT] or PORT).");
    app.add_flag("-v", g_ctx.verbose, "Enable verbose output");
    app.add_flag("--wake", g_ctx.switch2_wakeup_setup_requested, "Run interactive Joy-Con 2 wake setup, save config, test wake, then exit");
    app.add_flag("--hori", g_ctx.legacy_mode, "Expose the legacy 8-byte HORI controller gadget (default exposes 64-byte mode)");
    app.add_flag("--bt", bt_explicit, "Explicitly enable local SDL3 Bluetooth/controller input and disable Switch 2 wake");
    app.add_flag("--upnp", do_upnp, "Forward the UDP port via UPnP for PC clients only");
    
    auto opt_w = app.add_option("-w", web_port, "Serve the browser webapp too, using this port or 8080")
                   ->expected(0, 1);
                   
    bool legacy_p = false;
    app.add_flag("-p", legacy_p, "")->group("");

    try {
        std::vector<char*> cli_argv;
        cli_argv.reserve(cli_args.size());
        for (std::string& arg : cli_args) cli_argv.push_back(arg.data());
        app.parse(static_cast<int>(cli_argv.size()), cli_argv.data());
    } catch (const CLI::ParseError &e) {
        return app.exit(e);
    }

    if (legacy_p) {
        std::println(stderr, "error: -p was removed; use -b PORT or -b ADDR:PORT instead");
        return 1;
    }

    if (!bind_arg.empty() && !parse_bind_arg(bind_arg, bind_addr, port)) {
        std::println(stderr, "error: invalid bind value; use -b ADDR, -b PORT, or -b ADDR:PORT");
        return 1;
    }
    
    serve_http_webapp = opt_w->count() > 0;

    // -wake setup mode: run interactive config and exit (takes priority)
    if (g_ctx.switch2_wakeup_setup_requested)
        return run_switch2_wakeup_setup();

    // Determine operating mode:
    //   * -bt  →  Bluetooth mode, wake disabled (explicit user choice)
    //   * else →  auto-detect: wake mode if config exists, Bluetooth mode otherwise
    if (bt_explicit) {
        g_ctx.switch2_wake_adv_enabled = false;
        if (!bluetooth_input_available()) {
            std::println(stderr, "error: -bt requested, but ns-backend was built without SDL3 support");
            return 1;
        }
        enter_bluetooth_runtime_mode();
    } else {
        g_ctx.switch2_wake_adv_enabled = load_switch2_wakeup_config(true);
        if (g_ctx.switch2_wake_adv_enabled) {
            bluetooth_enabled = false;
            enter_switch2_wake_runtime_mode();
            if (g_ctx.verbose)
                std::println("[wake] Switch 2 wake config loaded; wake mode active, Bluetooth disabled");
        } else {
            bluetooth_enabled = bluetooth_input_available();
            if (g_ctx.verbose && bluetooth_enabled)
                std::println("[bt] No Switch 2 wake config found; Bluetooth controller mode active");
        }
    }

    // If Bluetooth mode is active, warn if rfkill blocks the adapter.
    if (bluetooth_enabled) {
        int rc = std::system("rfkill list bluetooth 2>/dev/null | grep -qi 'blocked: yes'");
        bool blocked = (rc != -1 && WIFEXITED(rc) && WEXITSTATUS(rc) == 0);
        if (blocked) {
            std::println(stderr,
                "[bt] WARNING: Bluetooth controllers connected directly to the Raspberry Pi\n"
                "[bt]          will NOT work — the adapter is blocked by rfkill.\n"
                "[bt]          Unblock it with: sudo rfkill unblock bluetooth");
        }
    }

    randomize_controller_identity();

    // Always recreate the built-in gadget at process startup.  This makes every
    // launch self-healing: stale configfs state, leftover /dev/hidg* nodes, or a
    // previous unclean shutdown are cleared before the backend starts talking to
    // the console.
    run_gadget_setup_if_needed(true, "startup gadget recreation requested");

    derive_key(DEFAULT_SECRET, g_ctx.hmac_key);
    signal(SIGINT,  on_signal); signal(SIGTERM, on_signal); signal(SIGPIPE, SIG_IGN);

    if (do_upnp) upnp_add_mapping(port);

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("socket"); return 1; }
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("fcntl");
        close(sock);
        return 1;
    }

    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    int rbuf = 2 * 1024 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rbuf, sizeof(rbuf));

    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(port);
    if (inet_pton(AF_INET, bind_addr.c_str(), &addr.sin_addr) != 1) {
        std::println(stderr, "error: invalid IPv4 bind address: {}", bind_addr);
        close(sock);
        return 1;
    }
    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); close(sock); return 1; }

    // Start the WebSocket proxy after UDP is bound. The HTTP webapp is only
    // served when -w is passed.
    std::jthread web_thread(web_server_thread, web_port, port, serve_http_webapp);
    std::jthread bluetooth_thread;
    if (bluetooth_enabled)
        bluetooth_thread = std::jthread(bluetooth_input_thread);
    
    std::println("UDP {}:{} writer={} Hz mode={}",
                bind_addr, port, PRO_WRITER_HZ,
                g_ctx.legacy_mode ? "hori" : "modern");
    std::jthread wt(writer_thread, PRO_WRITER_HZ);
    std::jthread st(stats_thread);

    std::vector<uint8_t> udp_rx(std::max(UDP_RX_MAX_PACKET_SIZE, ns::macro::CHUNK_HEADER_SIZE + ns::macro::UDP_CHUNK_MAX + HMAC_TAG_SIZE));
    pollfd udp_poll{};
    udp_poll.fd = sock;
    udp_poll.events = POLLIN;

    while (g_ctx.running.load(std::memory_order_relaxed)) {
        udp_poll.revents = 0;
        int n = poll(&udp_poll, 1, 200);
        if (n < 0) {
            if (errno == EINTR) continue;
            std::println(stderr, "[udp] poll error: {}", std::strerror(errno));
            break;
        }
        if (n == 0) continue;
        if (udp_poll.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            std::println(stderr, "[udp] socket error in poll: revents=0x{:02X}", udp_poll.revents);
            break;
        }
        if ((udp_poll.revents & POLLIN) == 0) continue;

        sockaddr_in sender{};
        socklen_t slen;
        ssize_t bytes;

        // Drain all available packets from the kernel buffer.
        // Two UDP packet formats are accepted:
        //   1) legacy Packet: input only, unchanged, authenticated with HMAC.
        //   2) ExtendedUdpPacket: ExtendedMultiReport with motion/gyro, authenticated
        //      with HMAC, and opted into UDP rumble replies.
        while (g_ctx.running.load(std::memory_order_relaxed)) {
            slen = sizeof(sender);
            bytes = recvfrom(sock, udp_rx.data(), udp_rx.size(), 0, (sockaddr*)&sender, &slen);
            if (bytes <= 0) break; // EAGAIN or error — ring is drained

            if (bytes == (ssize_t)sizeof(ServerInfoProbe)) {
                ServerInfoProbe probe{};
                memcpy(&probe, udp_rx.data(), sizeof(probe));
                if (probe.magic == SERVER_INFO_MAGIC && probe.version == SERVER_INFO_VERSION) {
                    ServerInfoReply reply{};
                    reply.backend = g_ctx.legacy_mode ? SERVER_BACKEND_LEGACY : SERVER_BACKEND_PRO;
                    reply.udp_interval_ms = g_ctx.legacy_mode ? LEGACY_UDP_INTERVAL_MS : PRO_UDP_INTERVAL_MS;
                    reply.udp_hz = g_ctx.legacy_mode ? LEGACY_UDP_HZ : PRO_UDP_HZ;
                    sendto(sock, &reply, sizeof(reply), 0, (sockaddr*)&sender, slen);
                    continue;
                }
            }


            if (bytes >= 4) {
                uint32_t mmagic = 0;
                memcpy(&mmagic, udp_rx.data(), 4);
                if (mmagic == ns::macro::UDP_CHUNK_MAGIC) {
                    server_macro_handle_chunk_packet(std::span<const uint8_t>(udp_rx.data(), bytes), sender);
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
                        if (hmac_verify(std::span<const uint8_t>(g_ctx.hmac_key, 32), std::span<const uint8_t>(udp_rx.data(), ns::macro::UDP_HEADER_SIZE + text_len), std::span<const uint8_t>(recv_hmac, HMAC_TAG_SIZE)) == 0) {
                            if (!rate_allow(sender.sin_addr.s_addr)) continue;
                            int client_idx = server_macro_client_for_sender(sender);
                            if (client_idx >= 0) {
                                {
                                    std::lock_guard<std::mutex> lk(g_ctx.mtx[client_idx]);
                                    g_ctx.clients[client_idx].uses_pad_presence = true;
                                    int sp = mh.subpad < 4 ? mh.subpad : 0;
                                    g_ctx.clients[client_idx].pad_present[sp] = true;
                                    g_ctx.clients[client_idx].pad_last_present_us[sp] = now_us();
                                }
                                std::string text(reinterpret_cast<char*>(udp_rx.data() + ns::macro::UDP_HEADER_SIZE), text_len);
                                server_macro_start(client_idx, mh.subpad < 4 ? mh.subpad : 0, text);
                            }
                        } else if (g_ctx.verbose) std::println("bad macro HMAC, dropped");
                    } else if (g_ctx.verbose) std::println("bad macro packet size, dropped");
                    continue;
                }
            }

            bool is_extended_udp = false;
            bool is_extended_udp3 = false;
            Packet pkt{};
            ExtendedUdpPacket ext_pkt{};
            ExtendedUdpPacketPc ext3_pkt{};

            if (bytes == (ssize_t)PACKET_SIZE) {
                memcpy(&pkt, udp_rx.data(), sizeof(pkt));
            } else if (bytes == (ssize_t)EXT_UDP_PACKET_SIZE) {
                memcpy(&ext_pkt, udp_rx.data(), sizeof(ext_pkt));
                is_extended_udp = true;
            } else if (bytes == (ssize_t)EXT3_UDP_PACKET_SIZE) {
                memcpy(&ext3_pkt, udp_rx.data(), sizeof(ext3_pkt));
                is_extended_udp3 = true;
            }
            if (bytes < (ssize_t)sizeof(ns::Packet)) {
                if (g_ctx.verbose) std::println("[udp] unexpected packet size={}, dropped", bytes);
                continue;
            }

            // ── 1. Per-IP rate limiter ────────────────────────────────────────────
            uint32_t src_ip = sender.sin_addr.s_addr;
            if (!rate_allow(src_ip)) {
                if (g_ctx.verbose) std::println("rate limit exceeded, dropped");
                continue;
            }

            // ── 2. Magic + version check ──────────────────────────────────────────
            if (is_extended_udp) {
                if (!extended_udp_packet_ok(ext_pkt)) {
                    if (g_ctx.verbose) std::println("bad extended UDP magic/version, dropped");
                    continue;
                }
            } else if (is_extended_udp3) {
                if (!extended_udp3_packet_ok(ext3_pkt)) {
                    if (g_ctx.verbose) std::println("bad extended UDP v3 magic/version, dropped");
                    continue;
                }
            } else if (!packet_ok(pkt)) {
                if (g_ctx.verbose) std::println("bad magic/version, dropped");
                continue;
            }

            // ── 3. Find Client Session or Pin new IP:port ─────────────────────────
            int client_idx = -1;
            uint64_t now = now_us();
            bool wake_on_new_client = false;

            for (int i = 0; i < MAX_CLIENTS; ++i) {
                std::lock_guard<std::mutex> lk(g_ctx.mtx[i]);
                if (g_ctx.clients[i].active &&
                    g_ctx.clients[i].addr.sin_addr.s_addr == src_ip &&
                    g_ctx.clients[i].addr.sin_port == sender.sin_port) {
                    client_idx = i;
                    break;
                }
            }

            // If not found, assign to a free/timed-out slot.
            if (client_idx == -1) {
                for (int i = 0; i < MAX_CLIENTS; ++i) {
                    std::lock_guard<std::mutex> lk(g_ctx.mtx[i]);
                    if (!g_ctx.clients[i].active || elapsed_us_over(now, g_ctx.clients[i].last_rx_us, CLIENT_TIMEOUT_US)) {
                        client_idx = i;
                        g_ctx.clients[i].active = true;
                        g_ctx.clients[i].addr = sender;
                        g_ctx.clients[i].first_pkt = true;
                        g_ctx.clients[i].expected_seq = 0;
                        g_ctx.clients[i].report.reset();
                        clear_all_motion(g_ctx.clients[i]);
                        g_ctx.clients[i].uses_pad_presence = false;
                        clear_udp_rumble_state(g_ctx.clients[i]);
                        for (int s = 0; s < 4; ++s) {
                            g_ctx.clients[i].pad_present[s] = false;
                            g_ctx.clients[i].pad_last_present_us[s] = 0;
                        }
                        g_ctx.clients[i].last_rx_us = now;
                        wake_on_new_client = true;
                        if (g_ctx.verbose) std::println("New UDP client accepted into Server Slot {}/4", i+1);
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
                if (g_ctx.verbose) std::println("server is full (4 PCs already active), dropped");
                continue;
            }

            // ── 4. HMAC authentication ────────────────────────────────────────────
            int hmac_ok = 0;
            if (is_extended_udp) {
                hmac_ok = hmac_verify(std::span<const uint8_t>(g_ctx.hmac_key, 32),
                                      std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&ext_pkt), EXT_UDP_PACKET_AUTH_SIZE),
                                      std::span<const uint8_t>(ext_pkt.hmac, HMAC_TAG_SIZE));
            } else if (is_extended_udp3) {
                hmac_ok = hmac_verify(std::span<const uint8_t>(g_ctx.hmac_key, 32),
                                      std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&ext3_pkt), EXT3_UDP_PACKET_AUTH_SIZE),
                                      std::span<const uint8_t>(ext3_pkt.hmac, HMAC_TAG_SIZE));
            } else {
                hmac_ok = hmac_verify(std::span<const uint8_t>(g_ctx.hmac_key, 32),
                                      std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&pkt), PACKET_AUTH_SIZE),
                                      std::span<const uint8_t>(pkt.hmac, HMAC_TAG_SIZE));
            }
            if (hmac_ok != 0) {
                if (g_ctx.verbose) std::println("bad HMAC, dropped");
                continue;
            }

            uint8_t packet_flags = is_extended_udp3 ? ext3_pkt.flags : (is_extended_udp ? ext_pkt.flags : pkt.flags);
            if (packet_flags & FLAG_DISCONNECT) {
                server_macro_stop_all_for_client(client_idx);
                {
                    std::lock_guard<std::mutex> lk(g_ctx.mtx[client_idx]);
                    reset_udp_client_session_locked(g_ctx.clients[client_idx]);
                }
                rearm_switch2_wake_after_client_disconnect();
                if (g_ctx.verbose) std::println("UDP client {} sent disconnect and was released.", client_idx + 1);
                ++g_ctx.pkts_rx;
                continue;
            }

            if (wake_on_new_client)
                maybe_send_switch2_wake_advert("client connected via UDP input");

            // ── 5. Sequence counter + Apply to shared state ───────────────────────
            bool accepted = false;
            {
                std::lock_guard<std::mutex> lk(g_ctx.mtx[client_idx]);

                // Re-validate: writer may have deactivated the slot during HMAC.
                if (!g_ctx.clients[client_idx].active) continue;

                uint8_t flags = is_extended_udp3 ? ext3_pkt.flags : (is_extended_udp ? ext_pkt.flags : pkt.flags);
                uint32_t seq = is_extended_udp3 ? ext3_pkt.seq : (is_extended_udp ? ext_pkt.seq : pkt.seq);
                bool is_reset = (flags & FLAG_RESET);
                bool sequence_jump = (g_ctx.clients[client_idx].expected_seq > seq) &&
                                     ((g_ctx.clients[client_idx].expected_seq - seq) > 100);

                if (!g_ctx.clients[client_idx].first_pkt && seq < g_ctx.clients[client_idx].expected_seq && !is_reset && !sequence_jump) {
                    if (g_ctx.verbose)
                        std::println("UDP client {} out-of-order seq={}, dropped", client_idx+1, seq);
                    continue;
                }
                g_ctx.clients[client_idx].first_pkt = false;
                g_ctx.clients[client_idx].expected_seq = seq + 1;

                if (is_reset) {
                    g_ctx.clients[client_idx].report.reset();
                    clear_all_motion(g_ctx.clients[client_idx]);
                    for (int s = 0; s < 4; ++s) {
                        g_ctx.clients[client_idx].pad_present[s] = false;
                        g_ctx.clients[client_idx].pad_last_present_us[s] = 0;
                    }
                } else if (is_extended_udp || is_extended_udp3) {
                    // Extended UDP carries motion/gyro and pad-present flags, so
                    // neutral-but-connected pads can still receive rumble.  Version 6
                    // also carries the three Pro-controller IMU samples explicitly.
                    g_ctx.clients[client_idx].uses_pad_presence = true;
                    enable_udp_rumble_state(g_ctx.clients[client_idx]);

                    ExtendedHIDReport* dst_pads[4] = {
                        &g_ctx.clients[client_idx].report.p1,
                        &g_ctx.clients[client_idx].report.p2,
                        &g_ctx.clients[client_idx].report.p3,
                        &g_ctx.clients[client_idx].report.p4,
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
                                    set_motion_samples(g_ctx.clients[client_idx], s, src_pads3[s]->motion);
                                else
                                    clear_motion(g_ctx.clients[client_idx], s);
                            } else {
                                *dst_pads[s] = *src_pads[s];
                                if (src_pads[s]->has_motion)
                                    set_motion(g_ctx.clients[client_idx], s, src_pads[s]->motion);
                                else
                                    clear_motion(g_ctx.clients[client_idx], s);
                            }
                            g_ctx.clients[client_idx].pad_present[s] = true;
                            g_ctx.clients[client_idx].pad_last_present_us[s] = now;
                        } else {
                            g_ctx.clients[client_idx].pad_present[s] = false;
                            uint64_t last_seen = g_ctx.clients[client_idx].pad_last_present_us[s];
                            if (last_seen == 0 || now - last_seen >= WEB_PAD_ABSENT_RELEASE_US) {
                                dst_pads[s]->reset();
                                clear_motion(g_ctx.clients[client_idx], s);
                            }
                        }
                    }
                } else {
                    // Legacy UDP remains 100% compatible: input-only, no pad-present
                    // tracking, no gyro, and no UDP rumble replies.
                    g_ctx.clients[client_idx].uses_pad_presence = false;
                    clear_udp_rumble_state(g_ctx.clients[client_idx]);
                    for (int s = 0; s < 4; ++s) {
                        g_ctx.clients[client_idx].pad_present[s] = false;
                        g_ctx.clients[client_idx].pad_last_present_us[s] = 0;
                    }
                    clear_all_motion(g_ctx.clients[client_idx]);
                    legacy_multi_to_extended(pkt.report, g_ctx.clients[client_idx].report);
                }

                g_ctx.clients[client_idx].last_rx_us = now_us();
                accepted = true;
            }

            if (!accepted) continue;
            ++g_ctx.pkts_rx;

            // Extended UDP clients opted into rumble by using the new packet
            // format.  Legacy clients are not sent unexpected traffic.
            if (is_extended_udp || is_extended_udp3) {
                flush_rumble_to_udp(sock, client_idx);
            }
        } // drain loop
    } // poll loop

    std::println("[backend] shutting down");
    upnp_remove_mapping(port);
    close(sock);
    // std::jthread auto-joins and requests stop on destruction.
    // Ensure all loop threads terminate.
    wt.request_stop(); st.request_stop();
    if (web_thread.joinable()) web_thread.request_stop();
    if (bluetooth_thread.joinable()) bluetooth_thread.request_stop();

    teardown_gadget();

    return 0;
}
