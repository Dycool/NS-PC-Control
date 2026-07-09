#include "cli.hpp"
#include "input_settings.hpp"
#include "macro_client.hpp"
#include "stream_runtime.hpp"
#include "udp_protocol.hpp"
#include "shared/sha256.h"
#include <algorithm>
#include <chrono>
#include <print>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <CLI/CLI.hpp>

std::atomic<bool> g_cliRunning{true};

#ifdef _WIN32
BOOL WINAPI cli_handler(DWORD t) { if (t == CTRL_C_EVENT || t == CTRL_CLOSE_EVENT) g_cliRunning = false; return TRUE; }
#else
void cli_handler(int) { g_cliRunning = false; }
#endif

static void cli_sleep_while_running(std::chrono::milliseconds duration) {
    constexpr auto SLICE = std::chrono::milliseconds(20);
    auto remaining = duration;
    while (g_cliRunning.load(std::memory_order_relaxed) && remaining > std::chrono::milliseconds::zero()) {
        const auto chunk = std::min(remaining, SLICE);
        std::this_thread::sleep_for(chunk);
        remaining -= chunk;
    }
}

int cli_main(const std::vector<std::string>& original_args) {
    NetworkRuntime net;
    if (!net.good()) return std::println(stderr, "ERROR: network startup failed"), 1;
    raise_process_priority();
    g_cliRunning.store(true);
#ifdef _WIN32
    SetConsoleCtrlHandler(cli_handler, TRUE);
#else
    signal(SIGINT, cli_handler); signal(SIGTERM, cli_handler);
#endif

    std::vector<std::string> args;
    for (const auto& a : original_args) if (a != "--cli") args.push_back(a);
    if (args.empty()) args.push_back("ns-client");
    if (args.size() < 2) {
        std::println(stderr, "Usage: {} --cli <RASPBERRY_PI_IP[:PORT]> [-m MACRO] [-k [single|override]] [-c pro|joycon-l|joycon-r|joycon-lr]", args[0]);
        return 1;
    }

    std::string host_arg, macro_path, k_val = "single", controller_type = "pro";
    CLI::App app{"ns-client --cli"};
    app.add_option("host", host_arg, "Target IP[:PORT]")->required();
    auto* opt_m = app.add_option("-m,--macro", macro_path);
    auto* opt_k = app.add_option("-k,--keyboard", k_val)->expected(0, 1);
    app.add_option("-c,--controller", controller_type, "Emulated controller: pro, joycon-l, joycon-r, or joycon-lr");

    std::vector<const char*> argv_ptrs;
    for (const auto& a : args) argv_ptrs.push_back(a.c_str());
    try { app.parse((int)argv_ptrs.size(), argv_ptrs.data()); }
    catch (const CLI::ParseError &e) { return app.exit(e); }

    std::string host;
    int port = ns::DEFAULT_PORT;
    if (!parse_host_port(host_arg, host, port)) return std::println(stderr, "Invalid host: {}", host_arg), 1;

    int cli_kb = KB_OFF;
    if (*opt_k) {
        if (k_val == "single") cli_kb = KB_SINGLE;
        else if (k_val == "override") cli_kb = KB_OVERRIDE;
        else return std::println(stderr, "Unknown keyboard mode: {}", k_val), 1;
    }

    load_saved_bindings();
    load_saved_feature_toggles();
    if (controller_type == "pro") g_controllerType.store(ns::CONTROLLER_TYPE_PRO);
    else if (controller_type == "joycon-l") g_controllerType.store(ns::CONTROLLER_TYPE_JOYCON_L);
    else if (controller_type == "joycon-r") g_controllerType.store(ns::CONTROLLER_TYPE_JOYCON_R);
    else if (controller_type == "joycon-lr" || controller_type == "joycon-pair" || controller_type == "joycon-l+r") g_controllerType.store(ns::CONTROLLER_TYPE_JOYCON_PAIR);
    else return std::println(stderr, "Unknown controller type: {}", controller_type), 1;
    g_keyboardMode.store(cli_kb);
    if (cli_kb != KB_OFF) {
        std::println("Keyboard mode enabled ({}) - {} Player 1",
            k_val, cli_kb == KB_SINGLE ? "replaces" : "augments");
    }
    std::println("Extended UDP mode: rumble + motion.");

    uint8_t hmac_key[32];
    derive_key(ns::DEFAULT_SECRET, hmac_key);

    if (*opt_m) {
        SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in dest{};
        if (sock == INVALID_SOCKET || !resolve_udp_destination(host, port, dest)) {
            std::println(stderr, "ERROR: socket setup/resolve failed");
            if (sock != INVALID_SOCKET) closesocket(sock);
            return 1;
        }
        set_socket_nonblocking(sock);
        std::string err, raw = ns::macro::read_text_file_limited(macro_path, &err);
        auto steps = ns::macro::parse_text(raw);
        if (raw.empty() || steps.empty()) {
            std::println(stderr, "Macro error: {}", err.empty() ? "empty or invalid" : err);
            closesocket(sock);
            return 1;
        }
        bool sent = send_macro_udp_packet(sock, dest, hmac_key, raw, 0);
        std::println("{}", sent ? "Uploaded macro." : "Upload failed.");
        cli_sleep_while_running(std::chrono::milliseconds(std::min<uint64_t>(ns::macro::total_ms(steps), 600000) + 180));
        closesocket(sock);
        return sent && g_cliRunning.load(std::memory_order_relaxed) ? 0 : 1;
    }

    ClientStreamConfig cfg{.host = host, .port = port,
                           .gui_features = false, .print_cli_waiting_messages = true,
                           .idle_sleep_ms = 500, .hmac_key = hmac_key};

    std::println("Started unified CLI client. Press Ctrl+C to stop.");
    std::string err;
    int rc = run_client_stream(cfg, g_cliRunning, &err);
    if (rc != 0 && !err.empty()) std::println(stderr, "ERROR: {}", err);
    std::println("\nShutting down...");
    return rc;
}

