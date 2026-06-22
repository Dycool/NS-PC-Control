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
#endif

using namespace ns;

#include "app_state.hpp"

static void on_signal(int) { g_ctx.running.store(false, std::memory_order_relaxed); }
#include "virtual_controller.hpp"
#include "gadget_wakeup.hpp"
#include "writers.hpp"
#include "web_server.hpp"
#include "bluetooth_input.hpp"

#ifdef USE_UPNP
static bool     g_upnp_active = false;
static uint16_t g_upnp_port   = 0;
static UPNPUrls g_upnp_urls{};
static IGDdatas g_upnp_data{};
static char     g_upnp_lan_addr[64]{};

bool upnp_add_mapping(uint16_t port) {
    if (g_upnp_active) return false;
    struct UPNPDev* devlist = upnpDiscover(2000, nullptr, nullptr, 0, 0, 2, nullptr);
    if (!devlist) return false;
#if MINIUPNPC_API_VERSION >= 18
    int igd = UPNP_GetValidIGD(devlist, &g_upnp_urls, &g_upnp_data, g_upnp_lan_addr, sizeof(g_upnp_lan_addr), nullptr, 0);
#else
    int igd = UPNP_GetValidIGD(devlist, &g_upnp_urls, &g_upnp_data, g_upnp_lan_addr, sizeof(g_upnp_lan_addr));
#endif
    freeUPNPDevlist(devlist);
    if (igd != 1 && igd != 2) { FreeUPNPUrls(&g_upnp_urls); return false; }

    std::string port_str = std::to_string(port);
    if (UPNP_AddPortMapping(g_upnp_urls.controlURL, g_upnp_data.first.servicetype, port_str.c_str(), port_str.c_str(), g_upnp_lan_addr, "ns-backend", "UDP", nullptr, "0") != 0) {
        FreeUPNPUrls(&g_upnp_urls); return false;
    }
    g_upnp_active = true; g_upnp_port = port;
    char external_ip[40];
    if (UPNP_GetExternalIPAddress(g_upnp_urls.controlURL, g_upnp_data.first.servicetype, external_ip) == 0) {
        std::println("UPnP: UDP port {} successfully forwarded!\nUPnP: Tell your clients to connect to -> {}:{}", port, external_ip, port);
    }
    return true;
}

void upnp_remove_mapping(uint16_t) {
    if (!g_upnp_active) return;
    std::string port_str = std::to_string(g_upnp_port);
    UPNP_DeletePortMapping(g_upnp_urls.controlURL, g_upnp_data.first.servicetype, port_str.c_str(), "UDP", nullptr);
    std::println("UPnP: port mapping removed cleanly");
    FreeUPNPUrls(&g_upnp_urls); g_upnp_active = false; g_upnp_port = 0;
}
#else
bool upnp_add_mapping(uint16_t) { return false; }
void upnp_remove_mapping(uint16_t) {}
#endif

static bool parse_bind_arg(const std::string& raw, std::string& bind_addr, uint16_t& port) {
    if (raw.empty()) return false;
    uint32_t numeric_port = 0;
    if (ns::macro::parse_uint32_strict(raw, numeric_port)) {
        if (numeric_port > 65535) return false;
        bind_addr = "0.0.0.0"; port = (uint16_t)numeric_port; return true;
    }
    std::string addr = raw;
    size_t sep = raw.rfind(':');
    if (sep != std::string::npos) {
        uint32_t parsed_port = 0;
        if (!ns::macro::parse_uint32_strict(raw.substr(sep + 1), parsed_port) || parsed_port > 65535) return false;
        addr = raw.substr(0, sep); port = (uint16_t)parsed_port;
    }
    bind_addr = addr.empty() ? "0.0.0.0" : addr;
    return true;
}

int main(int argc, char** argv) {
    std::vector<std::string> cli_args;
    cli_args.reserve((size_t)argc);
    for (int i = 0; i < argc; ++i) {
        std::string s = argv[i] ? argv[i] : "";
        if (s == "-wake") s = "--wake";
        else if (s == "-hori") s = "--hori";
        else if (s == "-bt") s = "--bt";
        cli_args.push_back(s);
    }

    uint16_t port = DEFAULT_PORT;
    std::string bind_addr = "0.0.0.0";
    bool do_upnp = false, serve_http_webapp = false, bluetooth_enabled = false, bt_explicit = false, legacy_p = false;
    int web_port = 8080;

    CLI::App app{"ns-backend - Switch Input Server\n\n  By default, UDP and WebSocket input are both enabled."};
    std::string bind_arg;
    app.add_option("-b", bind_arg, "Bind UDP to ADDR[:PORT] or PORT");
    app.add_flag("-v", g_ctx.verbose, "Enable verbose output");
    app.add_flag("--wake", g_ctx.switch2_wakeup_setup_requested, "Run interactive Joy-Con 2 wake setup and exit");
    app.add_flag("--hori", g_ctx.legacy_mode, "Expose the legacy 8-byte HORI controller gadget");
    app.add_flag("--bt", bt_explicit, "Explicitly enable local SDL3 Bluetooth controller input");
    app.add_flag("--upnp", do_upnp, "Forward UDP port via UPnP");
    auto opt_w = app.add_option("-w", web_port, "Serve browser webapp on this port")->expected(0, 1);
    app.add_flag("-p", legacy_p, "")->group("");

    try {
        std::vector<char*> cli_argv;
        for (std::string& arg : cli_args) cli_argv.push_back(arg.data());
        app.parse((int)cli_argv.size(), cli_argv.data());
    } catch (const CLI::ParseError &e) { return app.exit(e); }

    if (legacy_p) {
        std::println(stderr, "error: -p was removed; use -b PORT or -b ADDR:PORT instead"); return 1;
    }
    if (!bind_arg.empty() && !parse_bind_arg(bind_arg, bind_addr, port)) {
        std::println(stderr, "error: invalid bind value; use -b ADDR, -b PORT, or -b ADDR:PORT"); return 1;
    }
    serve_http_webapp = opt_w->count() > 0;

    if (g_ctx.switch2_wakeup_setup_requested) return run_switch2_wakeup_setup();

    if (bt_explicit) {
        g_ctx.switch2_wake_adv_enabled = false;
        if (!bluetooth_input_available()) {
            std::println(stderr, "error: -bt requested, but built without SDL3 support"); return 1;
        }
        enter_bluetooth_runtime_mode();
    } else {
        g_ctx.switch2_wake_adv_enabled = load_switch2_wakeup_config(true);
        if (g_ctx.switch2_wake_adv_enabled) {
            bluetooth_enabled = false; enter_switch2_wake_runtime_mode();
            if (g_ctx.verbose) std::println("[wake] Switch 2 wake mode active, Bluetooth disabled");
        } else {
            bluetooth_enabled = bluetooth_input_available();
            if (g_ctx.verbose && bluetooth_enabled) std::println("[bt] Bluetooth controller mode active");
        }
    }

    if (bluetooth_enabled) {
        int rc = std::system("rfkill list bluetooth 2>/dev/null | grep -qi 'blocked: yes'");
        if (rc != -1 && WIFEXITED(rc) && WEXITSTATUS(rc) == 0) {
            std::println(stderr, "[bt] WARNING: Bluetooth adapter is blocked. Unblock with: sudo rfkill unblock bluetooth");
        }
    }

    randomize_controller_identity();
    run_gadget_setup_if_needed(true, "startup gadget recreation requested");
    derive_key(DEFAULT_SECRET, g_ctx.hmac_key);
    signal(SIGINT, on_signal); signal(SIGTERM, on_signal); signal(SIGPIPE, SIG_IGN);

    if (do_upnp) upnp_add_mapping(port);

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("socket"); return 1; }
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) { perror("fcntl"); close(sock); return 1; }

    int yes = 1; setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    int rbuf = 2 * 1024 * 1024; setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rbuf, sizeof(rbuf));

    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(port);
    if (inet_pton(AF_INET, bind_addr.c_str(), &addr.sin_addr) != 1) {
        std::println(stderr, "error: invalid IPv4: {}", bind_addr); close(sock); return 1;
    }
    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); close(sock); return 1; }

    std::jthread web_thread(web_server_thread, web_port, port, serve_http_webapp);
    std::jthread bluetooth_thread;
    if (bluetooth_enabled) bluetooth_thread = std::jthread(bluetooth_input_thread);

    std::println("UDP {}:{} writer={} Hz mode={}", bind_addr, port, PRO_WRITER_HZ, g_ctx.legacy_mode ? "hori" : "modern");
    std::jthread wt(writer_thread, PRO_WRITER_HZ), st(stats_thread);

    std::vector<uint8_t> udp_rx(std::max(UDP_RX_MAX_PACKET_SIZE, ns::macro::CHUNK_HEADER_SIZE + ns::macro::UDP_CHUNK_MAX + HMAC_TAG_SIZE));
    pollfd udp_poll{.fd = sock, .events = POLLIN};

    while (g_ctx.running.load(std::memory_order_relaxed)) {
        udp_poll.revents = 0;
        int n = poll(&udp_poll, 1, 200);
        if (n < 0) {
            if (errno == EINTR) continue;
            std::println(stderr, "[udp] poll error: {}", std::strerror(errno)); break;
        }
        if (n == 0) continue;
        if (udp_poll.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            std::println(stderr, "[udp] socket error in poll"); break;
        }
        if ((udp_poll.revents & POLLIN) == 0) continue;

        sockaddr_in sender{};
        socklen_t slen;
        ssize_t bytes;

        while (g_ctx.running.load(std::memory_order_relaxed)) {
            slen = sizeof(sender);
            bytes = recvfrom(sock, udp_rx.data(), udp_rx.size(), 0, (sockaddr*)&sender, &slen);
            if (bytes <= 0) break;

            if (bytes == (ssize_t)sizeof(ServerInfoProbe)) {
                ServerInfoProbe probe{}; memcpy(&probe, udp_rx.data(), sizeof(probe));
                if (probe.magic == SERVER_INFO_MAGIC && probe.version == SERVER_INFO_VERSION) {
                    ServerInfoReply reply{
                        .backend = (uint8_t)(g_ctx.legacy_mode ? SERVER_BACKEND_LEGACY : SERVER_BACKEND_PRO),
                        .udp_interval_ms = (uint16_t)(g_ctx.legacy_mode ? LEGACY_UDP_INTERVAL_MS : PRO_UDP_INTERVAL_MS),
                        .udp_hz = (uint16_t)(g_ctx.legacy_mode ? LEGACY_UDP_HZ : PRO_UDP_HZ)
                    };
                    sendto(sock, &reply, sizeof(reply), 0, (sockaddr*)&sender, slen); continue;
                }
            }

            if (bytes >= 4) {
                uint32_t mmagic = 0; memcpy(&mmagic, udp_rx.data(), 4);
                if (mmagic == ns::macro::UDP_CHUNK_MAGIC) {
                    server_macro_handle_chunk_packet({udp_rx.data(), (size_t)bytes}, sender); continue;
                }
            }

            if (bytes >= (ssize_t)(ns::macro::UDP_HEADER_SIZE + HMAC_TAG_SIZE)) {
                uint32_t mmagic = 0; memcpy(&mmagic, udp_rx.data(), 4);
                if (mmagic == ns::macro::UDP_MAGIC) {
                    ns::macro::MacroUdpHeaderWire mh{}; memcpy(&mh, udp_rx.data(), sizeof(mh));
                    uint32_t text_len = mh.text_len;
                    if (text_len <= ns::macro::UDP_TEXT_MAX && bytes == (ssize_t)(ns::macro::UDP_HEADER_SIZE + text_len + HMAC_TAG_SIZE)) {
                        const uint8_t* r_hmac = udp_rx.data() + ns::macro::UDP_HEADER_SIZE + text_len;
                        if (hmac_verify({g_ctx.hmac_key, 32}, {udp_rx.data(), ns::macro::UDP_HEADER_SIZE + text_len}, {r_hmac, HMAC_TAG_SIZE}) == 0) {
                            if (!rate_allow(sender.sin_addr.s_addr)) continue;
                            int cidx = server_macro_client_for_sender(sender);
                            if (cidx >= 0) {
                                {
                                    std::lock_guard<std::mutex> lk(g_ctx.mtx[cidx]);
                                    g_ctx.clients[cidx].uses_pad_presence = true;
                                    int sp = mh.subpad < 4 ? mh.subpad : 0;
                                    g_ctx.clients[cidx].pad_present[sp] = true;
                                    g_ctx.clients[cidx].pad_last_present_us[sp] = now_us();
                                }
                                std::string text((char*)udp_rx.data() + ns::macro::UDP_HEADER_SIZE, text_len);
                                server_macro_start(cidx, mh.subpad < 4 ? mh.subpad : 0, text);
                            }
                        } else if (g_ctx.verbose) std::println("bad macro HMAC, dropped");
                    }
                    continue;
                }
            }

            uint32_t src_ip = sender.sin_addr.s_addr;
            if (!rate_allow(src_ip)) continue;

            if (bytes < (ssize_t)(20 + HMAC_TAG_SIZE)) {
                if (g_ctx.verbose) std::println("[udp] short packet, dropped"); continue;
            }

            size_t auth_len = bytes - HMAC_TAG_SIZE;
            if (hmac_verify({g_ctx.hmac_key, 32}, {udp_rx.data(), auth_len}, {udp_rx.data() + auth_len, HMAC_TAG_SIZE}) != 0) {
                if (g_ctx.verbose) std::println("bad HMAC, dropped"); continue;
            }

            uint8_t flags = 0; uint32_t seq = 0;
            ExtendedMultiReport report; ExtendedMultiReport3 report3;
            bool pad_present[4] = {}; bool is_report3 = false;
            if (!parse_client_packet(udp_rx.data(), bytes, flags, seq, report, pad_present, is_report3, report3)) continue;

            if (flags & FLAG_DISCONNECT) {
                int cidx = -1;
                for (int i = 0; i < MAX_CLIENTS; ++i) {
                    std::lock_guard<std::mutex> lk(g_ctx.mtx[i]);
                    if (g_ctx.clients[i].active && g_ctx.clients[i].addr.sin_addr.s_addr == src_ip && g_ctx.clients[i].addr.sin_port == sender.sin_port) {
                        cidx = i; break;
                    }
                }
                if (cidx >= 0) {
                    reset_client_session(cidx); rearm_switch2_wake_after_client_disconnect();
                    if (g_ctx.verbose) std::println("UDP client {} disconnected.", cidx + 1);
                }
                ++g_ctx.pkts_rx; continue;
            }

            int cidx = -1;
            uint64_t now = now_us();
            bool wake_on_new_client = false;
            for (int i = 0; i < MAX_CLIENTS; ++i) {
                std::lock_guard<std::mutex> lk(g_ctx.mtx[i]);
                if (g_ctx.clients[i].active && g_ctx.clients[i].addr.sin_addr.s_addr == src_ip && g_ctx.clients[i].addr.sin_port == sender.sin_port) {
                    cidx = i; break;
                }
            }

            if (cidx == -1) {
                cidx = allocate_client_session(now, &sender, bytes != (ssize_t)PACKET_SIZE);
                if (cidx >= 0) {
                    wake_on_new_client = true;
                    if (g_ctx.verbose) std::println("New UDP client accepted into Slot {}", cidx + 1);
                }
            }

            if (cidx == -1) {
                if (g_ctx.verbose) std::println("server is full, dropped"); continue;
            }

            bool accepted = false;
            {
                std::lock_guard<std::mutex> lk(g_ctx.mtx[cidx]);
                ClientSession& c = g_ctx.clients[cidx];
                if (c.active) {
                    bool is_reset = (flags & FLAG_RESET);
                    bool sequence_jump = (c.expected_seq > seq) && ((c.expected_seq - seq) > 100);
                    if (!c.first_pkt && seq < c.expected_seq && !is_reset && !sequence_jump) continue;

                    c.first_pkt = false; c.expected_seq = seq + 1;
                    if (is_reset) {
                        reset_client_session_locked(c);
                        c.active = true; c.addr = sender; c.last_rx_us = now;
                    } else {
                        c.last_rx_us = now;
                        if (bytes == (ssize_t)PACKET_SIZE) {
                            c.uses_pad_presence = c.udp_rumble_enabled = false;
                            std::fill(c.pad_present, c.pad_present + 4, false);
                            std::fill(c.pad_last_present_us, c.pad_last_present_us + 4, 0);
                            clear_all_motion(c);
                            c.report = report; c.has_new_report = true;
                        } else {
                            c.uses_pad_presence = c.udp_rumble_enabled = true;
                            if (is_report3) { c.report3 = report3; c.has_new_report3 = true; }
                            else { c.report = report; c.has_new_report = true; }

                            ExtendedHIDReport* dst_pads[4] = { &c.report.p1, &c.report.p2, &c.report.p3, &c.report.p4 };
                            ExtendedHIDReport3* dst_pads3[4] = { &c.report3.p1, &c.report3.p2, &c.report3.p3, &c.report3.p4 };
                            const ExtendedHIDReport* src_pads[4] = { &report.p1, &report.p2, &report.p3, &report.p4 };
                            const ExtendedHIDReport3* src_pads3[4] = { &report3.p1, &report3.p2, &report3.p3, &report3.p4 };

                            for (int s = 0; s < 4; ++s) {
                                if (pad_present[s]) {
                                    c.pad_present[s] = true; c.pad_last_present_us[s] = now;
                                    if (is_report3) {
                                        *dst_pads3[s] = *src_pads3[s];
                                        if (src_pads3[s]->has_motion) set_motion_samples(c, s, src_pads3[s]->motion);
                                        else clear_motion(c, s);
                                    } else {
                                        *dst_pads[s] = *src_pads[s];
                                        if (src_pads[s]->has_motion) set_motion(c, s, src_pads[s]->motion);
                                        else clear_motion(c, s);
                                    }
                                } else {
                                    c.pad_present[s] = false;
                                    uint64_t last_seen = c.pad_last_present_us[s];
                                    if (last_seen == 0 || now - last_seen >= WEB_PAD_ABSENT_RELEASE_US) {
                                        if (is_report3) dst_pads3[s]->reset();
                                        else dst_pads[s]->reset();
                                        clear_motion(c, s);
                                    }
                                }
                            }
                        }
                    }
                    accepted = true;
                }
            }

            if (!accepted) continue;
            ++g_ctx.pkts_rx;
            if (wake_on_new_client) maybe_send_switch2_wake_advert("client connected via UDP input");
            if (bytes != (ssize_t)PACKET_SIZE) flush_rumble_to_udp(sock, cidx);
        }
    }

    std::println("[backend] shutting down");
    upnp_remove_mapping(port);
    close(sock);
    wt.request_stop(); st.request_stop();
    if (web_thread.joinable()) web_thread.request_stop();
    if (bluetooth_thread.joinable()) bluetooth_thread.request_stop();
    teardown_gadget();
    return 0;
}
