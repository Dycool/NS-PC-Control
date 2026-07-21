#include "shared/protocol.hpp"
#include "shared/macros.hpp"
#include "shared/sha256.h"

// Standard library
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <format>
#include <iostream>
#include <mutex>
#include <print>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// POSIX / Linux
#include <arpa/inet.h>
#include <csignal>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

// Third-party
#include <CLI/CLI.hpp>

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
#include "udp_feedback.hpp"
#include "udp_audio.hpp"
#include "bluetooth_input.hpp"
#include "bluetooth_manager.hpp"

static void on_fatal_signal(int sig) {
    static const char msg[] = "[gadget] fatal signal; unbinding USB gadget\n";
    ssize_t n = write(STDERR_FILENO, msg, sizeof(msg) - 1); (void)n;
    emergency_unbind_udc();
    signal(sig, SIG_DFL);
    raise(sig);
}

// ===========================================================================
// UPnP port mapping (optional)
// ===========================================================================

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
    int igd = UPNP_GetValidIGD(devlist, &g_upnp_urls, &g_upnp_data,
                               g_upnp_lan_addr, sizeof(g_upnp_lan_addr), nullptr, 0);
#else
    int igd = UPNP_GetValidIGD(devlist, &g_upnp_urls, &g_upnp_data,
                               g_upnp_lan_addr, sizeof(g_upnp_lan_addr));
#endif
    freeUPNPDevlist(devlist);
    if (igd != 1 && igd != 2) { FreeUPNPUrls(&g_upnp_urls); return false; }

    std::string port_str = std::to_string(port);
    if (UPNP_AddPortMapping(g_upnp_urls.controlURL, g_upnp_data.first.servicetype,
                            port_str.c_str(), port_str.c_str(), g_upnp_lan_addr,
                            "ns-backend", "UDP", nullptr, "0") != 0) {
        FreeUPNPUrls(&g_upnp_urls);
        return false;
    }

    g_upnp_active = true;
    g_upnp_port   = port;

    char external_ip[40];
    if (UPNP_GetExternalIPAddress(g_upnp_urls.controlURL,
                                  g_upnp_data.first.servicetype,
                                  external_ip) == 0) {
        std::println("UPnP: UDP port {} successfully forwarded!\n"
                     "UPnP: Tell your clients to connect to -> {}:{}",
                     port, external_ip, port);
    }
    return true;
}

void upnp_remove_mapping(uint16_t) {
    if (!g_upnp_active) return;
    std::string port_str = std::to_string(g_upnp_port);
    UPNP_DeletePortMapping(g_upnp_urls.controlURL,
                           g_upnp_data.first.servicetype,
                           port_str.c_str(), "UDP", nullptr);
    std::println("UPnP: port mapping removed cleanly");
    FreeUPNPUrls(&g_upnp_urls);
    g_upnp_active = false;
    g_upnp_port   = 0;
}
#else
bool upnp_add_mapping(uint16_t)  { return false; }
void upnp_remove_mapping(uint16_t) {}
#endif // USE_UPNP

// ===========================================================================
// Bind address parsing  (-b ADDR, -b PORT, -b ADDR:PORT)
// ===========================================================================

static bool parse_bind_arg(const std::string& raw, std::string& bind_addr, uint16_t& port) {
    if (raw.empty()) return false;

    uint32_t numeric_port = 0;
    if (ns::macro::parse_uint32_strict(raw, numeric_port)) {
        if (numeric_port > 65535) return false;
        bind_addr = "0.0.0.0";
        port      = static_cast<uint16_t>(numeric_port);
        return true;
    }

    std::string addr = raw;
    size_t sep = raw.rfind(':');
    if (sep != std::string::npos) {
        uint32_t parsed_port = 0;
        if (!ns::macro::parse_uint32_strict(raw.substr(sep + 1), parsed_port)
                || parsed_port > 65535) {
            return false;
        }
        addr = raw.substr(0, sep);
        port = static_cast<uint16_t>(parsed_port);
    }

    bind_addr = addr.empty() ? "0.0.0.0" : addr;
    return true;
}

// ===========================================================================
// main()
// ===========================================================================

int main(int argc, char** argv) {
    // Normalise legacy single-dash flags to double-dash for CLI11.
    std::vector<std::string> cli_args;
    cli_args.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        std::string s = argv[i] ? argv[i] : "";
        if      (s == "-wake")                  s = "--wake";
        else if (s == "-hori")                  s = "--hori";
        else if (s == "-s2")                    s = "--s2";
        else if (s == "-bt")                    s = "--bt";
        else if (s == "-no-bt" || s == "--no-bt") {
            std::println(stderr, "error: -no-bt was removed; Bluetooth controller input is now "
                                 "disabled by default. Use --bt to enable it.");
            return 1;
        }
        else if (s == "-pair")                  s = "--pair";
        cli_args.push_back(s);
    }

    // -----------------------------------------------------------------------
    // CLI definition
    // -----------------------------------------------------------------------
    uint16_t    port              = DEFAULT_PORT;
    std::string bind_addr         = "0.0.0.0";
    bool        do_upnp           = false;
    bool        serve_http_webapp = false;
    bool        bluetooth_enabled = false;
    bool        pair_explicit     = false;
    bool        bt_requested      = false;
    bool        legacy_p          = false;
    bool        use_hori          = false;
    bool        use_s2            = false;
    int         web_port          = 8080;

    CLI::App app{"ns-backend - Switch Input Server\n\n"
                 "  By default only the authenticated UDP input path is enabled. "
                 "Use --bt for local Bluetooth controller input and -w for the "
                 "browser webapp + WebSocket input; Switch 2 wake arms itself "
                 "when configured."};
    std::string bind_arg;
    app.add_option("-b",       bind_arg,                   "Bind UDP to ADDR[:PORT] or PORT");
    app.add_flag  ("-v",       g_ctx.verbose,              "Enable verbose output");
    app.add_flag  ("--wake",   g_ctx.switch2_wakeup_setup_requested,
                                                           "Run interactive Joy-Con 2 wake setup and exit");
    app.add_flag  ("--pair",   pair_explicit,              "Enable Bluetooth gamepad pairing window for 2 minutes on startup");
    app.add_flag  ("--bt",     bt_requested,               "Enable local SDL3 Bluetooth controller input (disabled by default); Switch 2 wake works either way if configured");
    app.add_flag  ("--hori",   use_hori,                   "Use legacy HORI USB controller identity");
    app.add_flag  ("--s2",     use_s2,                     "Use Switch 2 USB controller identity");
    app.add_flag  ("--upnp",   do_upnp,                    "Forward UDP port via UPnP");
    auto opt_w = app.add_option("-w", "Serve browser webapp on this port")->expected(0, 1);
    app.add_flag  ("-p",       legacy_p,                   "")->group("");

    try {
        std::vector<char*> cli_argv;
        for (std::string& arg : cli_args) cli_argv.push_back(arg.data());
        app.parse(static_cast<int>(cli_argv.size()), cli_argv.data());
    } catch (const CLI::ParseError& e) { return app.exit(e); }

    // -----------------------------------------------------------------------
    // Post-parse validation
    // -----------------------------------------------------------------------
    // Bluetooth controller input is opt-in: without --bt the backend never
    // touches the local adapter for input (Switch 2 wake stays independent).
    g_ctx.bluetooth_input_disabled = !bt_requested;
    if (legacy_p) {
        std::println(stderr, "error: -p was removed; use -b PORT or -b ADDR:PORT instead");
        return 1;
    }
    if (use_hori && use_s2) {
        std::println(stderr, "error: --hori and --s2 are mutually exclusive");
        return 1;
    }
    if (!bind_arg.empty() && !parse_bind_arg(bind_arg, bind_addr, port)) {
        std::println(stderr, "error: invalid bind value; use -b ADDR, -b PORT, or -b ADDR:PORT");
        return 1;
    }

    serve_http_webapp = opt_w->count() > 0;
    if (serve_http_webapp) {
        if (!opt_w->results().empty() && !opt_w->results()[0].empty()) {
            try {
                web_port = std::stoi(opt_w->results()[0]);
            } catch (...) {
                std::println(stderr, "error: invalid web port value: {}", opt_w->results()[0]);
                return 1;
            }
        } else {
            web_port = 8080;
        }
    }

    // -----------------------------------------------------------------------
    // Early-exit sub-commands
    // -----------------------------------------------------------------------
    if (g_ctx.switch2_wakeup_setup_requested) return run_switch2_wakeup_setup();
    if (pair_explicit) {
        bluetooth_manager_runtime_setup(true);
        return run_bluetooth_pairing_wizard() ? 0 : 1;
    }

    // -----------------------------------------------------------------------
    // Runtime initialisation
    // -----------------------------------------------------------------------
    g_ctx.switch2_wake_adv_enabled = load_switch2_wakeup_config(true);
    bluetooth_enabled = bt_requested && bluetooth_input_available();
    if (bluetooth_enabled) {
        bluetooth_manager_runtime_setup(g_ctx.verbose || pair_explicit);
    }

    if (bluetooth_enabled) {
        enter_bluetooth_runtime_mode();
        if (g_ctx.verbose) {
            std::println("[bt] Bluetooth controller mode active");
            if (g_ctx.switch2_wake_adv_enabled)
                std::println("[wake] Switch 2 wake is armed and will coexist with Bluetooth controller mode");
        }
    } else if (g_ctx.switch2_wake_adv_enabled) {
        // Wake remains armed without local BT controller input.
        enter_switch2_wake_runtime_mode();
        if (g_ctx.verbose) std::println("[wake] Switch 2 wake armed without local Bluetooth controller input");
    } else if (g_ctx.verbose && !bt_requested) {
        std::println("[bt] Local Bluetooth controller input off (default); use --bt to enable");
    } else if (g_ctx.verbose && bt_requested && !bluetooth_enabled) {
        std::println("[bt] --bt requested but Bluetooth controller input is unavailable on this build/host");
    }

    randomize_controller_identity();
    // Default is Switch 1. The only runtime choices are explicit startup flags:
    //   --s2   => Switch 2 USB identity/profile family
    //   --hori => legacy HORI USB identity/profile family
    const UsbControllerFamily selected_usb_family = use_s2 ? UsbControllerFamily::Switch2
        : (use_hori ? UsbControllerFamily::Hori : UsbControllerFamily::Switch1);
    // One Raspberry Pi UDC is one USB device. --s2 therefore exposes one
    // native Switch 2 controller only; multiplayer remains available in S1 mode.
    configure_usb_controller_family(selected_usb_family);
    if (!run_gadget_setup_if_needed(true, "startup gadget recreation requested")) {
        std::println(stderr, "[gadget] Fatal: USB gadget setup failed.");
        return 1;
    }
    derive_key(DEFAULT_SECRET, g_ctx.hmac_key);

    // -----------------------------------------------------------------------
    // Signal handling
    // -----------------------------------------------------------------------
    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // Clear SA_RESTART so blocking ops are interrupted cleanly.
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    struct sigaction sa_pipe{};
    sa_pipe.sa_handler = SIG_IGN;
    sigemptyset(&sa_pipe.sa_mask);
    sa_pipe.sa_flags = 0;
    sigaction(SIGPIPE, &sa_pipe, nullptr);

    struct sigaction sa_fatal{};
    sa_fatal.sa_handler = on_fatal_signal;
    sigemptyset(&sa_fatal.sa_mask);
    sa_fatal.sa_flags = SA_RESETHAND;
    sigaction(SIGSEGV, &sa_fatal, nullptr);
    sigaction(SIGABRT, &sa_fatal, nullptr);
    sigaction(SIGBUS,  &sa_fatal, nullptr);
    sigaction(SIGFPE,  &sa_fatal, nullptr);
    sigaction(SIGILL,  &sa_fatal, nullptr);

    // -----------------------------------------------------------------------
    // UDP socket setup
    // -----------------------------------------------------------------------
    if (do_upnp) upnp_add_mapping(port);

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("fcntl"); close(sock); return 1;
    }

    int yes  = 1;                 setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes,  sizeof(yes));
    int rbuf = 2 * 1024 * 1024;  setsockopt(sock, SOL_SOCKET, SO_RCVBUF,    &rbuf, sizeof(rbuf));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, bind_addr.c_str(), &addr.sin_addr) != 1) {
        std::println(stderr, "error: invalid IPv4: {}", bind_addr);
        close(sock); return 1;
    }
    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("bind"); close(sock); return 1;
    }

    // Desktop Switch 2 audio runs on its own UDP socket bound to port + offset,
    // serviced by dedicated audio threads. Keeping it off the input socket means
    // the controller-input receive loop never sees the ~200 audio datagrams/sec
    // and never spends time HMAC-verifying them, so audio cannot add jitter to
    // input latency.
    int audio_sock = -1;
    if (g_ctx.usb_controller_family == UsbControllerFamily::Switch2) {
        const uint16_t audio_port = static_cast<uint16_t>(port + ns::S2_AUDIO_PORT_OFFSET);
        audio_sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (audio_sock < 0) { perror("audio socket"); close(sock); return 1; }
        int aflags = fcntl(audio_sock, F_GETFL, 0);
        if (aflags < 0 || fcntl(audio_sock, F_SETFL, aflags | O_NONBLOCK) < 0) {
            perror("audio fcntl"); close(audio_sock); close(sock); return 1;
        }
        setsockopt(audio_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        setsockopt(audio_sock, SOL_SOCKET, SO_RCVBUF, &rbuf, sizeof(rbuf));
        sockaddr_in audio_addr{};
        audio_addr.sin_family = AF_INET;
        audio_addr.sin_port   = htons(audio_port);
        audio_addr.sin_addr   = addr.sin_addr;
        if (bind(audio_sock, reinterpret_cast<sockaddr*>(&audio_addr), sizeof(audio_addr)) < 0) {
            perror("audio bind"); close(audio_sock); close(sock); return 1;
        }
        // NOTE: the built-in UPnP helper maps a single port only, so the audio
        // port is not auto-forwarded. LAN play needs nothing extra; remote play
        // over UPnP would require forwarding this port manually.
        s2_udp_audio_start(audio_sock);
        std::println("[s2][audio] UDP port {} enabled", audio_port);
    }

    // -----------------------------------------------------------------------
    // Worker threads
    // -----------------------------------------------------------------------
    std::jthread web_thread(web_server_thread, web_port, port, serve_http_webapp);
    std::jthread bluetooth_thread;
    if (bluetooth_enabled) bluetooth_thread = std::jthread(bluetooth_input_thread);

    // -----------------------------------------------------------------------
    // Startup message
    // -----------------------------------------------------------------------
    if (g_ctx.verbose) {
        std::println("UDP {}:{} writer={} Hz mode=modern",
                     bind_addr, port, PRO_WRITER_HZ);
    }

    std::string start_msg = std::format("Started ns-backend server on {}:{}", bind_addr, port);
    if (serve_http_webapp)
        start_msg += std::format(" with webapp on {}:{}", bind_addr, web_port);

    std::vector<std::string> extras;
    if (pair_explicit)                    extras.push_back("pairing enabled");
    if (bluetooth_enabled)                extras.push_back("Bluetooth controller input");
    if (use_hori)                         extras.push_back("HORI USB mode");
    if (use_s2)                           extras.push_back("Switch 2 USB mode (single native controller)");
    if (do_upnp)                          extras.push_back("UPnP mapping");
    if (g_ctx.switch2_wake_adv_enabled)   extras.push_back("Switch 2 wake armed");
    if (g_ctx.verbose)                    extras.push_back("verbose");

    if (!extras.empty()) {
        start_msg += " (";
        for (size_t i = 0; i < extras.size(); ++i) {
            if (i > 0) start_msg += ", ";
            start_msg += extras[i];
        }
        start_msg += ")";
    }
    std::println("{}", start_msg);

    std::jthread wt(writer_thread, PRO_WRITER_HZ);
    std::jthread st(stats_thread);

    // -----------------------------------------------------------------------
    // Main UDP receive loop
    // -----------------------------------------------------------------------
    std::vector<uint8_t> udp_rx(
        std::max({UDP_RX_MAX_PACKET_SIZE,
                  ns::S2_AUDIO_PACKET_SIZE,
                  ns::macro::CHUNK_HEADER_SIZE + ns::macro::UDP_CHUNK_MAX + HMAC_TAG_SIZE}));

    pollfd udp_poll{.fd = sock, .events = POLLIN, .revents = 0};

    // Set by a Gadget mode request once accepted; consumed after shutdown below
    // to re-exec this same binary with the new --hori/--s2 flag.
    bool pending_restart = false;
    UsbControllerFamily pending_restart_family = UsbControllerFamily::Switch1;

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
            std::println(stderr, "[udp] socket error in poll");
            break;
        }
        if ((udp_poll.revents & POLLIN) == 0) continue;

        sockaddr_in sender{};
        socklen_t   slen;
        ssize_t     bytes;

        while (g_ctx.running.load(std::memory_order_relaxed)) {
            slen  = sizeof(sender);
            bytes = recvfrom(sock, udp_rx.data(), udp_rx.size(), 0,
                             reinterpret_cast<sockaddr*>(&sender), &slen);
            if (bytes <= 0) break;

            // --- Server info probe ---
            if (bytes == static_cast<ssize_t>(sizeof(ServerInfoProbe))) {
                ServerInfoProbe probe{};
                memcpy(&probe, udp_rx.data(), sizeof(probe));
                if (probe.magic == SERVER_INFO_MAGIC && probe.version == SERVER_INFO_VERSION) {
                    ServerInfoReply reply{
                        .backend        = static_cast<uint8_t>(SERVER_BACKEND_PRO),
                        .udp_interval_ms = static_cast<uint16_t>(PRO_UDP_INTERVAL_MS),
                        .udp_hz         = static_cast<uint16_t>(PRO_UDP_HZ),
                    };
                    const uint64_t reply_now = now_us();
                    const int free_slots_now = free_virtual_slot_count(reply_now);
                    const int active_now = active_client_count(reply_now);
                    if (switch2_sleep_confirmed(reply_now)
                            && switch2_dormant_udp_endpoint_matches(sender)) {
                        reply.reserved[0] |= SERVER_INFO_FLAG_SWITCH_ASLEEP;
                    }
                    if (g_ctx.usb_controller_family == UsbControllerFamily::Switch2) {
                        reply.reserved[0] |= SERVER_INFO_FLAG_SWITCH2_MODE;
                        reply.reserved[0] |= SERVER_INFO_FLAG_S2_AUDIO;
                    }
                    if (g_ctx.usb_controller_family == UsbControllerFamily::Hori) {
                        reply.reserved[0] |= SERVER_INFO_FLAG_HORI_MODE;
                    }
                    if (free_slots_now <= 0 || active_now >= configured_client_capacity()) {
                        reply.reserved[0] |= SERVER_INFO_FLAG_SERVER_FULL;
                    }
                    reply.reserved[1] = static_cast<uint8_t>(std::clamp(active_now, 0, configured_client_capacity()));
                    reply.reserved[2] = static_cast<uint8_t>(configured_client_capacity());
                    reply.reserved[3] = static_cast<uint8_t>(std::clamp(free_slots_now, 0, configured_virtual_port_count()));
                    sendto(sock, &reply, sizeof(reply), 0,
                           reinterpret_cast<const sockaddr*>(&sender), slen);
                    continue;
                }
            }

            // --- Gadget mode (controller family) change request ---
            // Restarting the USB gadget subsystem for a family change is far more
            // than "re-enumerate": Switch 2 uses a completely different Raw Gadget
            // transport (its own char device, threads, and UAC1/UDP audio tunnels)
            // from the legacy configfs f_hid gadget used by Switch 1/HORI. Rather
            // than trying to tear down and stand up two unrelated USB gadget
            // frameworks live in-process, do a full clean shutdown (identical to
            // Ctrl+C) and re-exec the same binary with the new --hori/--s2 flag,
            // reusing the already-proven cold-start path instead of a new one.
            if (bytes == static_cast<ssize_t>(sizeof(ns::GadgetModeRequestPacket))) {
                uint32_t gm_magic = 0;
                memcpy(&gm_magic, udp_rx.data(), sizeof(gm_magic));
                if (gm_magic == ns::GADGET_MODE_MAGIC) {
                    ns::GadgetModeRequestPacket req{};
                    memcpy(&req, udp_rx.data(), sizeof(req));
                    if (req.version == ns::GADGET_MODE_VERSION
                            && hmac_verify({g_ctx.hmac_key, 32},
                                           {udp_rx.data(), ns::GADGET_MODE_REQUEST_AUTH_SIZE},
                                           {udp_rx.data() + ns::GADGET_MODE_REQUEST_AUTH_SIZE, HMAC_TAG_SIZE}) == 0) {
                        UsbControllerFamily requested = g_ctx.usb_controller_family;
                        bool valid_family = true;
                        switch (req.requested_family) {
                            case ns::GADGET_FAMILY_SWITCH1: requested = UsbControllerFamily::Switch1; break;
                            case ns::GADGET_FAMILY_SWITCH2: requested = UsbControllerFamily::Switch2; break;
                            case ns::GADGET_FAMILY_HORI:    requested = UsbControllerFamily::Hori;    break;
                            default: valid_family = false; break;
                        }
                        if (valid_family) {
                            const uint64_t gm_now = now_us();
                            // A lone requesting ns-client doesn't block itself: only
                            // OTHER active sessions count as "the server isn't empty".
                            const bool blocked_by_others = other_active_clients_exist(sender, gm_now);
                            ns::GadgetModeReplyPacket reply{};
                            if (blocked_by_others) {
                                reply.result = ns::GADGET_MODE_RESULT_SERVER_FULL;
                                reply.active_family = static_cast<uint8_t>(g_ctx.usb_controller_family);
                                reply.active_clients = static_cast<uint8_t>(
                                    std::clamp(active_client_count(gm_now), 0, MAX_CLIENTS));
                            } else if (requested == g_ctx.usb_controller_family) {
                                reply.result = ns::GADGET_MODE_RESULT_UNCHANGED;
                                reply.active_family = static_cast<uint8_t>(g_ctx.usb_controller_family);
                            } else {
                                reply.result = ns::GADGET_MODE_RESULT_RESTARTING;
                                reply.active_family = static_cast<uint8_t>(requested);
                            }
                            sendto(sock, &reply, sizeof(reply), 0,
                                   reinterpret_cast<const sockaddr*>(&sender), slen);
                            if (reply.result == ns::GADGET_MODE_RESULT_RESTARTING) {
                                std::println("Controller type change requested by client -> {}; restarting",
                                             usb_controller_family_name(requested));
                                pending_restart = true;
                                pending_restart_family = requested;
                                g_ctx.running.store(false, std::memory_order_relaxed);
                            }
                        }
                    } else if (g_ctx.verbose) {
                        std::println("bad gadget-mode HMAC, dropped");
                    }
                    continue;
                }
            }

            // --- Native Joy-Con 2 mouse motion (desktop ns-client only) ---
            if (bytes == static_cast<ssize_t>(sizeof(ns::JoyconMousePacket))) {
                uint32_t mouse_magic = 0;
                std::memcpy(&mouse_magic, udp_rx.data(), sizeof(mouse_magic));
                if (mouse_magic == ns::JOYCON_MOUSE_MAGIC) {
                    if (hmac_verify({g_ctx.hmac_key, 32},
                                    {udp_rx.data(), ns::JOYCON_MOUSE_AUTH_SIZE},
                                    {udp_rx.data() + ns::JOYCON_MOUSE_AUTH_SIZE, HMAC_TAG_SIZE}) != 0) {
                        if (g_ctx.verbose) std::println("bad Joy-Con mouse HMAC, dropped");
                        continue;
                    }
                    if (!rate_allow(sender.sin_addr.s_addr)) continue;

                    ns::JoyconMousePacket mouse{};
                    std::memcpy(&mouse, udp_rx.data(), sizeof(mouse));
                    if (mouse.version != ns::JOYCON_MOUSE_VERSION || mouse.subpad >= 4) continue;

                    int client_idx = -1;
                    for (int i = 0; i < MAX_CLIENTS; ++i) {
                        std::lock_guard<std::mutex> lk(g_ctx.mtx[i]);
                        if (g_ctx.clients[i].active
                                && g_ctx.clients[i].source == InputSource::Udp
                                && g_ctx.clients[i].addr.sin_addr.s_addr == sender.sin_addr.s_addr
                                && g_ctx.clients[i].addr.sin_port == sender.sin_port) {
                            client_idx = i;
                            break;
                        }
                    }
                    if (client_idx >= 0) {
                        update_joycon_mouse_stream(client_idx, mouse, now_us());
                    } else if (g_ctx.verbose) {
                        std::println("[s2][mouse][udp-rx] packet from {}:{} matched no active UDP client; dropped",
                                     inet_ntoa(sender.sin_addr), ntohs(sender.sin_port));
                    }
                    continue;
                }
            }



            // Switch 2 desktop audio is handled entirely on its own socket/threads
            // (see udp_audio.cpp). It never reaches this input loop, so nothing to
            // do here — keeping audio off this thread is what protects input latency.

            // --- Macro chunk ---
            if (bytes >= 4) {
                uint32_t mmagic = 0;
                memcpy(&mmagic, udp_rx.data(), 4);
                if (mmagic == ns::macro::UDP_CHUNK_MAGIC) {
                    server_macro_handle_chunk_packet({udp_rx.data(), static_cast<size_t>(bytes)}, sender);
                    continue;
                }
                if (mmagic == ns::AMIIBO_DATA_MAGIC) {
                    ns::AmiiboDataPacket ad{};
                    memcpy(&ad, udp_rx.data(), std::min((size_t)bytes, sizeof(ad)));
                    // AmiiboDataPacket is packed for the UDP wire format. Copy
                    // multi-byte members before formatting/using them so C++ does
                    // not try to bind a reference to a potentially unaligned field.
                    const uint16_t amiibo_data_len = ad.data_len;
                    constexpr size_t amiibo_packet_header = offsetof(ns::AmiiboDataPacket, data);
                    const bool amiibo_size_supported = amiibo_data_len == ns::AMIIBO_RAW_DUMP_SIZE
                        || amiibo_data_len == ns::AMIIBO_EXTENDED_DUMP_SIZE;
                    const bool amiibo_packet_complete = bytes >= 0
                        && static_cast<size_t>(bytes) >= amiibo_packet_header + amiibo_data_len;
                    if (g_ctx.verbose) {
                        char addrbuf[INET_ADDRSTRLEN] = {};
                        inet_ntop(AF_INET, &sender.sin_addr, addrbuf, sizeof(addrbuf));
                        std::println("[s2][nfc][udp-rx] t_us={} from={}:{} packet_bytes={} subpad={} declared_data_len={}",
                                     now_us(), addrbuf, ntohs(sender.sin_port), bytes,
                                     static_cast<unsigned>(ad.subpad),
                                     static_cast<unsigned>(amiibo_data_len));
                    }
                    if (!amiibo_size_supported || !amiibo_packet_complete) {
                        if (g_ctx.verbose) {
                            std::println(stderr,
                                         "[s2][nfc][udp-rx] upload rejected before routing: supported_size={} complete_packet={} header_bytes={} declared_data_len={} packet_bytes={}",
                                         amiibo_size_supported, amiibo_packet_complete,
                                         amiibo_packet_header, static_cast<unsigned>(amiibo_data_len), bytes);
                        }
                        continue;
                    }
                    int client_idx = -1;
                    for (int i = 0; i < MAX_CLIENTS; ++i) {
                        std::lock_guard<std::mutex> lk(g_ctx.mtx[i]);
                        if (g_ctx.clients[i].active
                                && g_ctx.clients[i].source == InputSource::Udp
                                && g_ctx.clients[i].addr.sin_addr.s_addr == sender.sin_addr.s_addr
                                && g_ctx.clients[i].addr.sin_port == sender.sin_port) {
                            client_idx = i;
                            break;
                        }
                    }
                    int port_for_source = client_idx >= 0
                        ? console_port_for_client_subpad(client_idx, ad.subpad)
                        : -1;
                    if (g_ctx.verbose)
                        std::println("[s2][nfc][udp-rx] resolved client={} subpad={} initial_port={}",
                                     client_idx, ad.subpad, port_for_source);
                    // Joy-Con L+R pair exposes NFC on the right virtual port.
                    // If the primary assignment is the left port, route the
                    // uploaded tag to any assigned port that actually has NFC.
                    if (client_idx >= 0 && (port_for_source < 0 || !controller_port_supports_amiibo(port_for_source))) {
                        uint8_t mask = 0;
                        {
                            std::lock_guard<std::mutex> lk(g_ctx.mtx[client_idx]);
                            mask = g_ctx.clients[client_idx].client_assignment[ad.subpad].console_port_mask;
                        }
                        for (int port = 0; port < HID_PORT_COUNT; ++port) {
                            if ((mask & (1u << port)) && controller_port_supports_amiibo(port)) {
                                port_for_source = port;
                                break;
                            }
                        }
                        if (g_ctx.verbose)
                            std::println("[s2][nfc][udp-rx] NFC-capable fallback lookup mask=0x{:02x} resolved_port={}",
                                         mask, port_for_source);
                    }
                    if (port_for_source >= 0) {
                        if (g_ctx.verbose)
                            std::println("[s2][nfc][udp-rx] forwarding upload client={} subpad={} -> port={} len={}",
                                         client_idx, static_cast<unsigned>(ad.subpad), port_for_source,
                                         static_cast<unsigned>(amiibo_data_len));
                        set_amiibo_data_for_port(port_for_source, ad.data, amiibo_data_len);
                    } else if (g_ctx.verbose) {
                        std::println(stderr,
                                     "[s2][nfc][udp-rx] upload dropped: no assigned NFC-capable port client={} subpad={}",
                                     client_idx, ad.subpad);
                    }
                    continue;
                }
            }

            // --- Macro text packet ---
            if (bytes >= static_cast<ssize_t>(ns::macro::UDP_HEADER_SIZE + HMAC_TAG_SIZE)) {
                uint32_t mmagic = 0;
                memcpy(&mmagic, udp_rx.data(), 4);
                if (mmagic == ns::macro::UDP_MAGIC) {
                    ns::macro::MacroUdpHeaderWire mh{};
                    memcpy(&mh, udp_rx.data(), sizeof(mh));
                    uint32_t text_len = mh.text_len;
                    if (text_len <= ns::macro::UDP_TEXT_MAX
                            && bytes == static_cast<ssize_t>(
                                ns::macro::UDP_HEADER_SIZE + text_len + HMAC_TAG_SIZE)) {
                        const uint8_t* r_hmac = udp_rx.data() + ns::macro::UDP_HEADER_SIZE + text_len;
                        if (hmac_verify({g_ctx.hmac_key, 32},
                                        {udp_rx.data(), ns::macro::UDP_HEADER_SIZE + text_len},
                                        {r_hmac, HMAC_TAG_SIZE}) == 0) {
                            if (!rate_allow(sender.sin_addr.s_addr)) continue;
                            int cidx = server_macro_client_for_sender(sender);
                            if (cidx >= 0) {
                                {
                                    std::lock_guard<std::mutex> lk(g_ctx.mtx[cidx]);
                                    g_ctx.clients[cidx].uses_pad_presence = true;
                                    int sp = mh.subpad < 4 ? mh.subpad : 0;
                                    g_ctx.clients[cidx].pad_present[sp]         = true;
                                    g_ctx.clients[cidx].pad_last_present_us[sp] = now_us();
                                }
                                std::string text(reinterpret_cast<char*>(udp_rx.data())
                                                     + ns::macro::UDP_HEADER_SIZE,
                                                 text_len);
                                server_macro_start(cidx, mh.subpad < 4 ? mh.subpad : 0, text);
                            }
                        } else if (g_ctx.verbose) {
                            std::println("bad macro HMAC, dropped");
                        }
                    }
                    continue;
                }
            }

            // --- Client source controller names ---
            if (bytes == static_cast<ssize_t>(sizeof(ns::ClientNamesPacket))) {
                uint32_t nmagic = 0;
                memcpy(&nmagic, udp_rx.data(), 4);
                if (nmagic == ns::CLIENT_NAMES_MAGIC) {
                    if (hmac_verify({g_ctx.hmac_key, 32},
                                    {udp_rx.data(), ns::CLIENT_NAMES_AUTH_SIZE},
                                    {udp_rx.data() + ns::CLIENT_NAMES_AUTH_SIZE, HMAC_TAG_SIZE}) == 0) {
                        if (!rate_allow(sender.sin_addr.s_addr)) continue;
                        ns::ClientNamesPacket names{};
                        memcpy(&names, udp_rx.data(), sizeof(names));
                        if (names.version == ns::SERVER_INFO_VERSION) {
                            int cidx = -1;
                            for (int i = 0; i < MAX_CLIENTS; ++i) {
                                std::lock_guard<std::mutex> lk(g_ctx.mtx[i]);
                                if (g_ctx.clients[i].active
                                        && g_ctx.clients[i].source == InputSource::Udp
                                        && g_ctx.clients[i].addr.sin_addr.s_addr == sender.sin_addr.s_addr
                                        && g_ctx.clients[i].addr.sin_port == sender.sin_port) {
                                    cidx = i; break;
                                }
                            }
                            if (cidx >= 0) store_client_source_names(cidx, names);
                        }
                    } else if (g_ctx.verbose) {
                        std::println("bad names HMAC, dropped");
                    }
                    continue;
                }
            }

            // --- Normal controller packet ---
            uint32_t src_ip = sender.sin_addr.s_addr;
            if (!rate_allow(src_ip)) continue;

            if (bytes < static_cast<ssize_t>(20 + HMAC_TAG_SIZE)) {
                if (g_ctx.verbose) std::println("[udp] short packet, dropped");
                continue;
            }

            size_t auth_len = static_cast<size_t>(bytes) - HMAC_TAG_SIZE;
            if (hmac_verify({g_ctx.hmac_key, 32},
                            {udp_rx.data(), auth_len},
                            {udp_rx.data() + auth_len, HMAC_TAG_SIZE}) != 0) {
                if (g_ctx.verbose) std::println("bad HMAC, dropped");
                continue;
            }

            uint8_t     flags = 0;
            uint32_t    seq   = 0;
            MultiReport report;
            bool        pad_present[4] = {};
            if (!parse_client_packet(udp_rx.data(), bytes, flags, seq, report, pad_present)) continue;

            // Native S2 mode has one source/input only. New clients already send
            // P1 exclusively, but enforce it server-side for compatibility.
            if (g_ctx.usb_controller_family == UsbControllerFamily::Switch2) {
                report.p2.reset(); report.p3.reset(); report.p4.reset();
                pad_present[1] = pad_present[2] = pad_present[3] = false;
            }

            // --- Disconnect ---
            if (flags & FLAG_DISCONNECT) {
                int cidx = -1;
                for (int i = 0; i < MAX_CLIENTS; ++i) {
                    std::lock_guard<std::mutex> lk(g_ctx.mtx[i]);
                    if (g_ctx.clients[i].active
                            && g_ctx.clients[i].source == InputSource::Udp
                            && g_ctx.clients[i].addr.sin_addr.s_addr == src_ip
                            && g_ctx.clients[i].addr.sin_port == sender.sin_port) {
                        cidx = i; break;
                    }
                }
                forget_switch2_dormant_udp_endpoint(sender);
                if (g_ctx.usb_controller_family == UsbControllerFamily::Switch2)
                    s2_udp_audio_forget_endpoint(sender);
                if (cidx >= 0) {
                    reset_client_session(cidx);
                    rearm_switch2_wake_after_client_disconnect();
                    std::println("UDP client released from Slot {}", cidx + 1);
                }
                ++g_ctx.pkts_rx;
                continue;
            }

            // --- Find or allocate client slot ---
            int      cidx            = -1;
            uint64_t now             = now_us();
            bool     wake_on_new_client = false;
            const bool sleeping      = switch2_sleep_confirmed(now);

            for (int i = 0; i < MAX_CLIENTS; ++i) {
                std::lock_guard<std::mutex> lk(g_ctx.mtx[i]);
                if (g_ctx.clients[i].active
                        && g_ctx.clients[i].source == InputSource::Udp
                        && g_ctx.clients[i].addr.sin_addr.s_addr == src_ip
                        && g_ctx.clients[i].addr.sin_port == sender.sin_port) {
                    cidx = i; break;
                }
            }

            const bool dormant_endpoint = sleeping && switch2_dormant_udp_endpoint_matches(sender);
            const bool unsupported_s2_pair = report_requests_unsupported_s2_pair(report, pad_present, true);
            if (cidx >= 0 && unsupported_s2_pair) {
                ns::ClientAssignmentPacket unsupported = make_server_profile_unsupported_assignment_packet(
                    static_cast<uint8_t>(std::clamp(active_client_count(now), 0, configured_client_capacity())),
                    static_cast<uint8_t>(std::clamp(free_virtual_slot_count(now), 0, configured_virtual_port_count())),
                    sleeping);
                sendto(sock, &unsupported, sizeof(unsupported), 0, reinterpret_cast<const sockaddr*>(&sender), slen);
                reset_client_session(cidx);
                if (g_ctx.verbose) std::println("[udp] refused Joy-Con L+R: native S2 mode supports one controller only");
                continue;
            }
            if (cidx == -1) {
                if (dormant_endpoint) {
                    // This endpoint belonged to a pre-sleep client. Keep dropping
                    // until the desktop client observes the sleep flag, disconnects,
                    // and reconnects as a fresh wake attempt.
                    ++g_ctx.pkts_rx;
                    continue;
                }

                const int free_slots_now = free_virtual_slot_count(now);
                const int active_now = active_client_count(now);
                // Profile errors take priority over capacity errors so an L+R
                // client always receives the actionable S2-mode explanation.
                if (unsupported_s2_pair) {
                    ns::ClientAssignmentPacket unsupported = make_server_profile_unsupported_assignment_packet(
                        static_cast<uint8_t>(std::clamp(active_now, 0, configured_client_capacity())),
                        static_cast<uint8_t>(std::clamp(free_slots_now, 0, configured_virtual_port_count())),
                        sleeping);
                    sendto(sock, &unsupported, sizeof(unsupported), 0, reinterpret_cast<const sockaddr*>(&sender), slen);
                    if (g_ctx.verbose) std::println("[udp] refused Joy-Con L+R: native S2 mode supports one controller only");
                    continue;
                }
                if (free_slots_now <= 0 || active_now >= configured_client_capacity()) {
                    ns::ClientAssignmentPacket full = make_server_full_assignment_packet(
                        static_cast<uint8_t>(std::clamp(active_now, 0, configured_client_capacity())),
                        static_cast<uint8_t>(std::clamp(free_slots_now, 0, configured_virtual_port_count())),
                        sleeping);
                    sendto(sock, &full, sizeof(full), 0, reinterpret_cast<const sockaddr*>(&sender), slen);
                    if (g_ctx.verbose) std::println("server is full, refused UDP client");
                    continue;
                }
                const int required_slots = requested_virtual_slots_for_report(report, pad_present, true);
                if (required_slots > free_slots_now) {
                    ns::ClientAssignmentPacket full = make_server_full_assignment_packet(
                        static_cast<uint8_t>(std::clamp(active_now, 0, configured_client_capacity())),
                        static_cast<uint8_t>(std::clamp(free_slots_now, 0, configured_virtual_port_count())),
                        sleeping);
                    sendto(sock, &full, sizeof(full), 0, reinterpret_cast<const sockaddr*>(&sender), slen);
                    continue;
                }

                cidx = allocate_client_session(now, &sender, true, InputSource::Udp);
                if (cidx >= 0) {
                    wake_on_new_client = true;
                    forget_switch2_dormant_udp_endpoint(sender);
                    std::println("New UDP client accepted into Slot {}", cidx + 1);
                }
            }

            if (cidx == -1) {
                ns::ClientAssignmentPacket full = make_server_full_assignment_packet(
                    static_cast<uint8_t>(std::clamp(active_client_count(now), 0, configured_client_capacity())),
                    static_cast<uint8_t>(std::clamp(free_virtual_slot_count(now), 0, configured_virtual_port_count())),
                    sleeping);
                sendto(sock, &full, sizeof(full), 0, reinterpret_cast<const sockaddr*>(&sender), slen);
                if (g_ctx.verbose) std::println("server is full, dropped");
                continue;
            }

            // --- Apply packet ---
            bool accepted = false;
            {
                std::lock_guard<std::mutex> lk(g_ctx.mtx[cidx]);
                ClientSession& c = g_ctx.clients[cidx];
                if (c.active) {
                    bool is_reset      = (flags & FLAG_RESET);
                    bool sequence_jump = (c.expected_seq > seq)
                                      && ((c.expected_seq - seq) > 100);
                    if (!c.first_pkt && seq < c.expected_seq && !is_reset && !sequence_jump)
                        continue;

                    c.first_pkt    = false;
                    c.expected_seq = seq + 1;

                    if (is_reset) {
                        reset_client_session_locked(c);
                        c.active     = true;
                        c.source     = InputSource::Udp;
                        c.addr       = sender;
                        c.last_rx_us = now;
                    } else {
                        c.last_rx_us         = now;
                        c.uses_pad_presence  = true;
                        c.udp_rumble_enabled = true;
                        c.report             = report;
                        c.has_new_report     = true;
                        ++c.report_generation;

                        const HIDReport* src_pads[4] = { &report.p1, &report.p2, &report.p3, &report.p4 };
                        HIDReport*       dst_pads[4] = { &c.report.p1, &c.report.p2, &c.report.p3, &c.report.p4 };

                        for (int s = 0; s < 4; ++s) {
                            if (pad_present[s]) {
                                c.pad_present[s]         = true;
                                c.pad_last_present_us[s] = now;
                                *dst_pads[s]             = *src_pads[s];
                            } else {
                                c.pad_present[s] = false;
                                uint64_t last_seen = c.pad_last_present_us[s];
                                if (last_seen == 0 || now - last_seen >= WEB_PAD_ABSENT_RELEASE_US)
                                    dst_pads[s]->reset();
                            }
                        }
                    }
                    accepted = true;
                }
            }

            if (!accepted) continue;
            ++g_ctx.pkts_rx;
            if (wake_on_new_client) {
                maybe_send_switch2_wake_advert("UDP client connected", true);
            }
            flush_feedback_to_udp(sock, cidx);
        }
    }

    // -----------------------------------------------------------------------
    // Shutdown
    // -----------------------------------------------------------------------
    std::cout << "Shutting down" << std::flush;

    // Broadcast every stop condition before waiting for any subsystem. Joins
    // below remain ordered only to protect dependencies (the USB writer must
    // leave before gadget teardown); the workers themselves exit concurrently.
    g_ctx.running.store(false, std::memory_order_relaxed);
    wt.request_stop();
    st.request_stop();
    if (web_thread.joinable()) web_thread.request_stop();
    if (bluetooth_thread.joinable()) {
        bluetooth_thread.request_stop();
        bluetooth_manager_request_stop();
    }

    // Router cleanup can be network-bound and has no dependency on the worker
    // threads. Run it alongside their shutdown, but join it before exit/exec.
    std::thread upnp_cleanup([port] { upnp_remove_mapping(port); });

    close(sock);
    if (g_ctx.usb_controller_family == UsbControllerFamily::Switch2) {
        s2_udp_audio_stop();
        if (audio_sock >= 0) close(audio_sock);
    }
    std::cout << "." << std::flush;

    if (wt.joinable())               { wt.join();               std::cout << "." << std::flush; }
    if (st.joinable())               { st.join();               std::cout << "." << std::flush; }
    if (bluetooth_thread.joinable()) { bluetooth_thread.join(); std::cout << "." << std::flush; }
    if (web_thread.joinable())       { web_thread.join();       std::cout << "." << std::flush; }

    teardown_gadget();
    if (upnp_cleanup.joinable()) upnp_cleanup.join();
    std::cout << ".\n";

    // A WS client (webapp Settings) may have requested the family change; it
    // funnels into the same re-exec path as the HMAC-verified UDP request.
    if (!pending_restart && g_ctx.family_change_requested.load(std::memory_order_relaxed)) {
        pending_restart = true;
        switch (g_ctx.family_change_target.load(std::memory_order_relaxed)) {
            case ns::GADGET_FAMILY_SWITCH2: pending_restart_family = UsbControllerFamily::Switch2; break;
            case ns::GADGET_FAMILY_HORI:    pending_restart_family = UsbControllerFamily::Hori;    break;
            default:                        pending_restart_family = UsbControllerFamily::Switch1; break;
        }
    }

    if (pending_restart) {
        // Re-exec rather than reconfigure in place: same cold-start path that
        // was already exercised above, just with the family flag swapped.
        // Scheduling policy (e.g. `chrt -f 99`) and root privileges are process
        // attributes that execve() preserves, so neither is lost here.
        std::vector<std::string> restart_args;
        restart_args.reserve(cli_args.size() + 1);
        for (const std::string& a : cli_args) {
            if (a != "--hori" && a != "--s2") restart_args.push_back(a);
        }
        if (pending_restart_family == UsbControllerFamily::Hori)        restart_args.push_back("--hori");
        else if (pending_restart_family == UsbControllerFamily::Switch2) restart_args.push_back("--s2");

        std::vector<char*> exec_argv;
        exec_argv.reserve(restart_args.size() + 1);
        for (std::string& a : restart_args) exec_argv.push_back(a.data());
        exec_argv.push_back(nullptr);

        std::println("Restarting ns-backend as {}...", usb_controller_family_name(pending_restart_family));
        execv("/proc/self/exe", exec_argv.data());
        std::println(stderr, "error: failed to restart ns-backend: {}", std::strerror(errno));
        return 1;
    }
    return 0;
}
