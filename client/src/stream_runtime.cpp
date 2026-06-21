#include "stream_runtime.hpp"
#include "input_settings.hpp"
#include "macro_client.hpp"
#include "rumble_client.hpp"
#include "udp_protocol.hpp"
#include "shared/macros.hpp"
#include "shared/sha256.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <utility>

std::atomic<bool> g_connected{false};
std::jthread g_senderThread;
uint8_t g_hmacKey[32]{};
std::atomic<uint32_t> g_packetCount{0};
std::mutex g_statusMutex;
std::string g_statusMessage = "Ready";
std::string g_lastError;

void set_status_message(const std::string& s) {
    std::lock_guard<std::mutex> lk(g_statusMutex);
    g_statusMessage = s;
}

std::string status_message() {
    std::lock_guard<std::mutex> lk(g_statusMutex);
    return g_statusMessage;
}

bool parse_host_port(std::string in, std::string& host, int& port) {
    in = ns::macro::trim(in);
    if (in.empty()) return false;
    port = ns::DEFAULT_PORT;
    size_t colon = in.find(':');
    if (colon != std::string::npos) {
        int p = std::atoi(in.c_str() + colon + 1);
        if (p >= 1 && p <= 65535) port = p;
        in.resize(colon);
    }
    host = ns::macro::trim(in);
    return !host.empty();
}

void raise_process_priority() {
#ifdef _WIN32
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
#else
    setpriority(PRIO_PROCESS, 0, -20);
#endif
}

void raise_sender_priority() {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#endif
}

void ClientFrame::reset() {
        active_count = 0;
        for (int i = 0; i < 4; ++i) {
            reports[i].reset();
            for (int j = 0; j < 3; ++j) motion[i][j].reset();
            present[i] = false;
            has_motion[i] = false;
            controller_for_slot[i] = -1;
        }
}

void build_client_frame(ClientFrame& frame,
                               DigitalReleaseFilter filters[4],
                               bool send_motion,
                               int keyboard_mode) {
    frame.reset();

    auto sdl = g_sdlInput.snapshot();
    const uint64_t filter_now = ns::now_us();

    for (int i = 0; i < 4; ++i) {
        if (!sdl[i].connected) {
            filters[i].reset();
            continue;
        }

        frame.reports[i] = sdl[i].input;
        filters[i].apply(frame.reports[i], filter_now);
        for (int j = 0; j < 3; ++j) frame.motion[i][j] = sdl[i].motion_samples[j];
        frame.present[i] = true;
        frame.has_motion[i] = send_motion && sdl[i].has_motion;
        frame.controller_for_slot[i] = i;
        ++frame.active_count;
    }

    if (keyboard_mode == KB_SINGLE) {
        if (frame.present[0]) {
            int target = -1;
            for (int s = 1; s < 4; ++s) {
                if (!frame.present[s]) { target = s; break; }
            }
            if (target >= 0) {
                frame.reports[target] = frame.reports[0];
                for (int j = 0; j < 3; ++j) frame.motion[target][j] = frame.motion[0][j];
                frame.has_motion[target] = frame.has_motion[0];
                frame.present[target] = true;
                frame.controller_for_slot[target] = frame.controller_for_slot[0];
                ++frame.active_count;
            }
        }

        frame.reports[0].reset();
        for (int j = 0; j < 3; ++j) frame.motion[0][j].reset();
        apply_keyboard_to_report(frame.reports[0], false);
        frame.present[0] = true;
        frame.has_motion[0] = false;
        frame.controller_for_slot[0] = -1;
        frame.active_count = std::max(frame.active_count, 1);
    } else if (keyboard_mode == KB_OVERRIDE) {
        apply_keyboard_to_report(frame.reports[0], true);
        frame.present[0] = true;
        frame.active_count = std::max(frame.active_count, 1);
    }
}

void send_client_frame(SOCKET sock,
                              const sockaddr_in& dest,
                              const uint8_t hmac_key[32],
                              uint32_t& seq,
                              bool legacy_packet,
                              const ClientFrame& frame) {
    if (legacy_packet) {
        ns::Packet pkt{};
        pkt.magic = ns::PROTO_MAGIC;
        pkt.version = ns::PROTO_VERSION;
        pkt.flags = ns::FLAG_NONE;
        pkt.seq = seq++;
        pkt.ts_us = ns::now_us();
        pkt.report.reset();

        ns::HIDReport* pads[4] = {&pkt.report.p1, &pkt.report.p2, &pkt.report.p3, &pkt.report.p4};
        for (int i = 0; i < 4; ++i) *pads[i] = frame.reports[i];

        uint8_t full_hmac[32];
        hmac_sha256(std::span<const uint8_t>(hmac_key, 32), std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&pkt), ns::PACKET_AUTH_SIZE), std::span<uint8_t, 32>(full_hmac));
        std::memcpy(pkt.hmac, full_hmac, ns::HMAC_TAG_SIZE);
        send_all_udp(sock, dest, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&pkt), ns::PACKET_SIZE));
        return;
    }

    ExtendedUdpPacket pkt{};
    pkt.magic = ns::PROTO_MAGIC;
    pkt.version = ns::WEB_PROTO_VERSION_3;
    pkt.flags = ns::FLAG_NONE;
    pkt.seq = seq++;
    pkt.timestamp_us = ns::now_us();
    pkt.report.reset();

    ns::ExtendedHIDReport3* pads[4] = {&pkt.report.p1, &pkt.report.p2, &pkt.report.p3, &pkt.report.p4};
    for (int i = 0; i < 4; ++i) {
        fill_extended_pad(*pads[i], frame.reports[i], frame.present[i],
                          frame.has_motion[i] ? frame.motion[i] : nullptr);
    }

    uint8_t full_hmac[32];
    hmac_sha256(std::span<const uint8_t>(hmac_key, 32), std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&pkt), EXT_UDP_PACKET_AUTH_SIZE), std::span<uint8_t, 32>(full_hmac));
    std::memcpy(pkt.hmac, full_hmac, ns::HMAC_TAG_SIZE);
    send_all_udp(sock, dest, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&pkt), sizeof(pkt)));
}

int run_client_stream(const ClientStreamConfig& cfg,
                             std::stop_token stoken,
                             std::string* err_out) {
    if (!cfg.hmac_key) {
        if (err_out) *err_out = "Missing HMAC key.";
        return 1;
    }

    raise_sender_priority();

    if (!g_sdlInput.start()) {
        if (err_out) *err_out = "SDL3 input failed: " + g_sdlInput.error();
        return 1;
    }

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == INVALID_SOCKET) {
        if (err_out) *err_out = "Failed to create UDP socket.";
        return 1;
    }
    set_socket_nonblocking(sock);

    sockaddr_in dest{};
    if (!resolve_udp_destination(cfg.host, cfg.port, dest)) {
        if (err_out) *err_out = "Cannot resolve address: " + cfg.host;
        closesocket(sock);
        return 1;
    }

    const bool server_is_legacy = detect_server_is_legacy(sock, dest);
    if (g_serverLastReplyUs.load() == 0) {
        if (err_out) *err_out = "Server not reachable. Check the IP address.";
        closesocket(sock);
        return 1;
    }
    const bool legacy_packet = cfg.force_legacy_udp || server_is_legacy;
    const bool send_motion = !legacy_packet;
    const int active_send_interval_ms = ns::LEGACY_UDP_INTERVAL_MS; // always 250 Hz

    uint32_t seq = 0;
    RumbleManager rumble;
    DigitalReleaseFilter sdl_filters[4];
    bool no_controllers_printed = false;

    uint64_t last_probe_us = 0;

    while (!stoken.stop_requested()) {
        if (cfg.gui_features) {
            std::string upload;
            {
                std::lock_guard<std::mutex> lk(g_macro_mtx);
                upload.swap(g_macro_upload_pending);
            }
            if (!upload.empty()) send_macro_udp_packet(sock, dest, cfg.hmac_key, upload, 0);

            poll_macro_entry_hotkeys();
        }

        g_sdlInput.poll();

        ClientFrame frame;
        build_client_frame(frame, sdl_filters, send_motion, g_keyboardMode.load());

        if (cfg.gui_features) {
            macro_record_sample(frame.reports[0]);
            if (apply_macro_override(frame.reports, frame.present, frame.has_motion)) {
                frame.active_count = 1;
            }
        }

        send_client_frame(sock, dest, cfg.hmac_key, seq, legacy_packet, frame);

        pump_udp_replies(sock, rumble, frame.controller_for_slot);
        if (!legacy_packet) {
            rumble.update_timeouts(frame.controller_for_slot);
        }

        ++g_packetCount;

        const uint64_t now = ns::now_us();
        if (now - last_probe_us >= 5000000ULL) {
            ns::ServerInfoProbe probe{};
            send_all_udp(sock, dest, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&probe), sizeof(probe)));
            last_probe_us = now;
        }
        if (last_probe_us != 0 && now - g_serverLastReplyUs.load(std::memory_order_relaxed) > 15000000ULL) {
            set_status_message("Lost connection to server");
            break;
        }

        if (frame.active_count > 0) {
            no_controllers_printed = false;
            std::this_thread::sleep_for(std::chrono::milliseconds(active_send_interval_ms));
        } else {
            if (cfg.print_cli_waiting_messages && !no_controllers_printed) {
                std::cout << "No controllers detected - waiting for connections...\n";
                no_controllers_printed = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(cfg.idle_sleep_ms));
        }
    }

    rumble.stop_all();
    send_udp_disconnect_packet(sock, dest, cfg.hmac_key, seq++, legacy_packet);
    closesocket(sock);
    return 0;
}

void sender_thread_main(std::stop_token stoken, std::string host, uint16_t port, bool legacy_udp) {
    ClientStreamConfig cfg{};
    cfg.host = std::move(host);
    cfg.port = port;
    cfg.force_legacy_udp = legacy_udp;
    cfg.gui_features = true;
    cfg.print_cli_waiting_messages = false;
    cfg.idle_sleep_ms = 50;
    cfg.hmac_key = g_hmacKey;

    set_status_message("Connected to " + cfg.host + ":" + std::to_string(cfg.port));

    std::string err;
    int rc = run_client_stream(cfg, stoken, &err);
    if (rc != 0 && !err.empty()) {
        g_lastError = err;
        set_status_message(err);
    }

    g_connected.store(false);
}

bool start_connection(const std::string& target, std::string* err_out) {
    if (g_connected.load()) return true;
    std::string host;
    int port = ns::DEFAULT_PORT;
    if (!parse_host_port(target, host, port)) {
        if (err_out) *err_out = "Please enter a Raspberry Pi IP address.";
        return false;
    }
    if (!probe_server_sync(host, port)) {
        if (err_out) *err_out = "Server not reachable. Check the IP address.";
        return false;
    }
    if (!g_sdlInput.start()) {
        if (err_out) *err_out = "SDL3 input failed: " + g_sdlInput.error();
        return false;
    }
    derive_key(ns::DEFAULT_SECRET, g_hmacKey);
    save_last_ip(target);
    load_macro_entries();
    g_packetCount.store(0);
    g_serverLastReplyUs.store(0);
    g_lastError.clear();
    g_connected.store(true);
    if (g_senderThread.joinable()) {
        g_senderThread.request_stop();
        g_senderThread.join();
    }
    g_senderThread = std::jthread(sender_thread_main, host, (uint16_t)port, false);
    return true;
}

void stop_connection() {
    if (!g_connected.load()) return;
    g_connected.store(false);
    if (g_senderThread.joinable()) {
        g_senderThread.request_stop();
        g_senderThread.join();
    }
    set_status_message("Disconnected");
}

NetworkRuntime::NetworkRuntime() {
#ifdef _WIN32
        timeBeginPeriod(1);
        WSADATA wsa{};
        ok = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
        signal(SIGPIPE, SIG_IGN);
        ok = true;
#endif
    }
NetworkRuntime::~NetworkRuntime() {
        stop_connection();
        g_sdlInput.stop();
#ifdef _WIN32
        if (ok) WSACleanup();
        timeEndPeriod(1);
#endif
    }
bool NetworkRuntime::good() const { return ok; }
