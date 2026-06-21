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

void print_cli_usage(const char* exe) {
    std::println(stderr,
                 "Usage: {} --cli <RASPBERRY_PI_IP[:PORT]> [--hori] [-m MACRO] [-k [single|override]]",
                 exe ? exe : "ns-client");
}

#ifdef _WIN32
BOOL WINAPI cli_console_ctrl_handler(DWORD ctrl_type) {
    if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_CLOSE_EVENT ||
        ctrl_type == CTRL_BREAK_EVENT || ctrl_type == CTRL_SHUTDOWN_EVENT) {
        g_cliRunning.store(false);
        return TRUE;
    }
    return FALSE;
}
#else
void cli_signal_handler(int) {
    g_cliRunning.store(false);
}
#endif



int cli_main(const std::vector<std::string>& original_args) {
    NetworkRuntime net;
    if (!net.good()) {
        std::println(stderr, "ERROR: network startup failed");
        return 1;
    }
    raise_process_priority();
    g_cliRunning.store(true);
#ifdef _WIN32
    SetConsoleCtrlHandler(cli_console_ctrl_handler, TRUE);
#else
    signal(SIGINT, cli_signal_handler);
    signal(SIGTERM, cli_signal_handler);
#endif

    std::vector<std::string> args;
    args.reserve(original_args.size());
    for (const std::string& a : original_args) if (a != "--cli") args.push_back(a);
    if (args.empty()) args.push_back("ns-client");
    if (args.size() < 2) {
        print_cli_usage(args[0].c_str());
        return 1;
    }

    std::string host;
    int port = ns::DEFAULT_PORT;
    bool legacy_udp = false;
    bool macro_mode = false;
    std::string macro_path;
    int cli_keyboard_mode = KB_OFF;

    CLI::App app{"ns-client --cli\nRun the terminal client from this unified executable"};

    std::string host_arg;
    app.add_option("host", host_arg, "Target RASPBERRY_PI_IP[:PORT]");
    
    app.add_flag("--hori", legacy_udp, "Send old input-only HORI-compatible UDP packets; disables UDP rumble/gyro");
    
    auto opt_m = app.add_option("-m,--macro,--upload-macro,--server-macro", macro_path, "Upload a P1 server-side macro JSON/string, wait for it, then exit");
    
    std::string k_val = "single";
    auto opt_k = app.add_option("-k,--keyboard", k_val, "Enable keyboard mode where supported (single|override)")->expected(0, 1);

    std::vector<const char*> argv_ptrs;
    for (const auto& a : args) argv_ptrs.push_back(a.c_str());

    try {
        app.parse(argv_ptrs.size(), argv_ptrs.data());
    } catch (const CLI::ParseError &e) {
        return app.exit(e);
    }

    if (host_arg.empty()) {
        std::print(stderr, "{}", app.help());
        return 1;
    }
    
    if (!parse_host_port(host_arg, host, port)) {
        std::println(stderr, "Invalid host: {}", host_arg);
        return 1;
    }
    
    if (*opt_m) {
        macro_mode = true;
    }
    
    if (*opt_k) {
        if (k_val == "single") cli_keyboard_mode = KB_SINGLE;
        else if (k_val == "override") cli_keyboard_mode = KB_OVERRIDE;
        else {
            std::println(stderr, "Unknown keyboard mode: {}", k_val);
            return 1;
        }
    }

    load_saved_bindings();
    load_saved_feature_toggles();
    g_keyboardMode.store(cli_keyboard_mode);
    if (cli_keyboard_mode != KB_OFF) {
        std::println("Keyboard mode enabled ({}) - {} Player 1",
            cli_keyboard_mode == KB_SINGLE ? "single" : "override",
            cli_keyboard_mode == KB_SINGLE ? "replaces" : "augments");
    }
    std::print("{}", legacy_udp ? "Hori UDP mode: input only. UDP rumble and gyro are disabled.\n"
                                 : "Extended UDP mode: SDL3 rumble replies + gyro/motion enabled where supported.\n");

    uint8_t hmac_key[32];
    derive_key(ns::DEFAULT_SECRET, hmac_key);

    if (macro_mode) {
        SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock == INVALID_SOCKET) {
            std::println(stderr, "ERROR: socket() failed");
            return 1;
        }
        set_socket_nonblocking(sock);

        sockaddr_in dest{};
        if (!resolve_udp_destination(host, port, dest)) {
            std::println(stderr, "ERROR: Unable to resolve IP/host: {}", host);
            closesocket(sock);
            return 1;
        }

        std::string err;
        std::string macro_raw = ns::macro::read_text_file_limited(macro_path, &err);
        if (macro_raw.empty()) {
            std::println(stderr, "Macro file is empty or cannot be read: {}", macro_path);
            closesocket(sock);
            return 1;
        }
        auto steps = ns::macro::parse_text(macro_raw);
        if (steps.empty()) {
            std::println(stderr, "Macro file has no usable commands: {}", macro_path);
            closesocket(sock);
            return 1;
        }
        bool sent = send_macro_udp_packet(sock, dest, hmac_key, macro_raw, 0);
        std::println("{}", sent ? "Uploaded server-side macro to P1." : "Failed to upload server-side macro.");
        uint64_t wait_ms = std::min<uint64_t>(ns::macro::total_ms(steps), 600000ULL);
        std::this_thread::sleep_for(std::chrono::milliseconds((int)wait_ms + 180));
        closesocket(sock);
        return sent ? 0 : 1;
    }

    ClientStreamConfig cfg{};
    cfg.host = host;
    cfg.port = port;
    cfg.force_legacy_udp = legacy_udp;
    cfg.gui_features = false;
    cfg.print_cli_waiting_messages = true;
    cfg.idle_sleep_ms = 500;
    cfg.hmac_key = hmac_key;

    std::println("Started unified CLI client. Press Ctrl+C to stop.");
    std::string err;
    
    int rc = run_client_stream(cfg, g_cliRunning, &err);

    if (rc != 0 && !err.empty()) std::println(stderr, "ERROR: {}", err);
    std::println("\nShutting down...");
    return rc;
}
