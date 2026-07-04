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
#include <print>
#include <iostream>
#include <utility>
#include <functional>
#include <vector>

std::atomic<bool> g_connected{false};
std::thread g_senderThread;
std::atomic<bool> g_senderRunning{false};
uint8_t g_hmacKey[32]{};
std::atomic<uint32_t> g_packetCount{0};
std::mutex g_statusMutex;
std::string g_statusMessage = "Ready";
std::string g_lastError;

static void sleep_while_running(std::atomic<bool>& running, std::chrono::milliseconds duration) {
    constexpr auto SLICE = std::chrono::milliseconds(20);
    auto remaining = duration;
    while (running.load(std::memory_order_relaxed) && remaining > std::chrono::milliseconds::zero()) {
        const auto chunk = std::min(remaining, SLICE);
        std::this_thread::sleep_for(chunk);
        remaining -= chunk;
    }
}

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
        battery_percent[i] = -1;
        battery_charging[i] = false;
    }
}

void build_client_frame(ClientFrame& frame, DigitalReleaseFilter filters[4], bool send_motion, int keyboard_mode) {
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
        frame.battery_percent[i] = sdl[i].battery_percent;
        frame.battery_charging[i] = sdl[i].battery_charging;
        ++frame.active_count;
    }

    if (keyboard_mode == KB_SINGLE) {
        if (frame.present[0]) {
            int target = -1;
            for (int s = 1; s < 4 && target < 0; ++s) if (!frame.present[s]) target = s;
            if (target >= 0) {
                frame.reports[target] = frame.reports[0];
                std::copy_n(frame.motion[0], 3, frame.motion[target]);
                frame.has_motion[target] = frame.has_motion[0];
                frame.present[target] = true;
                frame.controller_for_slot[target] = frame.controller_for_slot[0];
                frame.battery_percent[target] = frame.battery_percent[0];
                frame.battery_charging[target] = frame.battery_charging[0];
                ++frame.active_count;
            }
        }
        frame.reports[0].reset();
        for (auto& m : frame.motion[0]) m.reset();
        apply_keyboard_to_report(frame.reports[0], false);
        frame.present[0] = true;
        frame.has_motion[0] = false;
        frame.controller_for_slot[0] = -1;
        frame.battery_percent[0] = -1;
        frame.battery_charging[0] = false;
        frame.active_count = std::max(frame.active_count, 1);
    } else if (keyboard_mode == KB_OVERRIDE) {
        apply_keyboard_to_report(frame.reports[0], true);
        frame.present[0] = true;
        frame.active_count = std::max(frame.active_count, 1);
    }
}

void send_client_frame(SOCKET sock, const sockaddr_in& dest, const uint8_t hmac_key[32], uint32_t& seq, const ClientFrame& frame) {
    auto sign_and_send = [&](void* pkt, size_t size, size_t auth_size) {
        uint8_t full_hmac[32];
        hmac_sha256(std::span(hmac_key, 32), std::span(static_cast<const uint8_t*>(pkt), auth_size), std::span<uint8_t, 32>(full_hmac));
        std::memcpy(static_cast<uint8_t*>(pkt) + auth_size, full_hmac, ns::HMAC_TAG_SIZE);
        send_all_udp(sock, dest, std::span(static_cast<const uint8_t*>(pkt), size));
    };

    ns::Packet pkt{.magic = ns::PROTO_MAGIC, .version = ns::WEB_PROTO_VERSION, .flags = ns::FLAG_NONE, .reserved = 0, .seq = seq++, .ts_us = ns::now_us()};
    pkt.report.reset();
    for (int i = 0; i < 4; ++i) {
        ns::HIDReport* dst = (i == 0 ? &pkt.report.p1 : (i == 1 ? &pkt.report.p2 : (i == 2 ? &pkt.report.p3 : &pkt.report.p4)));
        fill_extended_pad(*dst, frame.reports[i], frame.present[i], frame.has_motion[i] ? frame.motion[i] : nullptr,
                          frame.battery_percent[i], frame.battery_charging[i]);
        dst->reserved[2] = static_cast<uint8_t>(g_controllerType.load(std::memory_order_relaxed));
    }
    sign_and_send(&pkt, ns::PACKET_SIZE, ns::PACKET_AUTH_SIZE);
}

int run_client_stream(const ClientStreamConfig& cfg, std::atomic<bool>& running, std::string* err_out) {
    if (!cfg.hmac_key) return (err_out ? *err_out = "Missing HMAC key." : ""), 1;
    raise_sender_priority();
    if (!g_sdlInput.start()) return (err_out ? *err_out = "SDL3 input failed: " + g_sdlInput.error() : ""), 1;

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in dest{};
    if (sock == INVALID_SOCKET || !resolve_udp_destination(cfg.host, cfg.port, dest)) {
        if (err_out) *err_out = "Socket or resolve error: " + cfg.host;
        if (sock != INVALID_SOCKET) closesocket(sock);
        return 1;
    }
    set_socket_nonblocking(sock);

    detect_server_is_legacy(sock, dest);
    if (g_serverLastReplyUs.load() == 0) return (err_out ? *err_out = "Server not reachable." : ""), closesocket(sock), 1;
    uint32_t seq = 0;
    RumbleManager rumble;
    DigitalReleaseFilter sdl_filters[4];
    bool no_controllers_printed = false;
    uint64_t last_probe_us = 0, last_kb_poll_us = 0;

    while (running.load(std::memory_order_relaxed)) {
        const uint64_t loop_start_us = ns::now_us();
        std::string upload;
        std::vector<uint8_t> amiibo_upload;
        if (cfg.gui_features && (loop_start_us - last_kb_poll_us >= 10000ULL)) {
            update_keyboard_state_cache();
            {
                std::lock_guard<std::mutex> lk(g_macro_mtx);
                upload.swap(g_macro_upload_pending);
                amiibo_upload.swap(g_amiibo_upload_pending);
            }
            poll_macro_entry_hotkeys();
            last_kb_poll_us = loop_start_us;
        }

        g_sdlInput.poll();
        ClientFrame frame;
        build_client_frame(frame, sdl_filters, true, g_keyboardMode.load());

        if (cfg.gui_features) {
            if (g_macro_recording.load(std::memory_order_relaxed)) macro_record_sample(frame.reports[0]);
            if (g_macro_running.load(std::memory_order_relaxed)) {
                if (apply_macro_override(frame.reports, frame.present, frame.has_motion)) frame.active_count = 1;
            }
        }

        send_client_frame(sock, dest, cfg.hmac_key, seq, frame);
        // Typed uploads are sent after the live input frame so the server has
        // already seen the current controller type (important for Joy-Con R NFC).
        if (!upload.empty()) send_macro_udp_packet(sock, dest, cfg.hmac_key, upload, 0);
        if (!amiibo_upload.empty()) {
            const bool ok = send_amiibo_udp_packet(sock, dest, cfg.hmac_key, amiibo_upload, 0);
            set_status_message(ok ? "Amiibo uploaded" : "Amiibo upload failed");
        }
        std::string amiibo_save_path;
        uint8_t amiibo_save_subpad = 0;
        uint32_t amiibo_save_version = 0;
        if (take_amiibo_save_request(amiibo_save_path, amiibo_save_subpad, amiibo_save_version)) {
            const bool ok = send_amiibo_pull_request(sock, dest, cfg.hmac_key, amiibo_save_subpad, amiibo_save_version);
            set_status_message(ok ? "Pulling updated amiibo dump" : "Amiibo save request failed");
        }
        pump_udp_replies(sock, rumble, frame.controller_for_slot);
        if (g_serverRequestedDisconnect.load(std::memory_order_relaxed)) {
            set_status_message("Disconnected");
            running.store(false, std::memory_order_relaxed);
            break;
        }
        rumble.update_timeouts(frame.controller_for_slot);
        ++g_packetCount;

        const uint64_t now = ns::now_us();
        if (now - last_probe_us >= 5000000ULL) {
            ns::ServerInfoProbe probe{};
            send_all_udp(sock, dest, std::span(reinterpret_cast<const uint8_t*>(&probe), sizeof(probe)));
            last_probe_us = now;
        }
        if (last_probe_us && now - g_serverLastReplyUs.load(std::memory_order_relaxed) > 15000000ULL) {
            set_status_message("Lost connection to server");
            running.store(false, std::memory_order_relaxed);
            break;
        }

        if (frame.active_count > 0) {
            no_controllers_printed = false;
            sleep_while_running(running, std::chrono::milliseconds(ns::LEGACY_UDP_INTERVAL_MS));
        } else {
            if (cfg.print_cli_waiting_messages && !no_controllers_printed) {
                std::println("No controllers detected - waiting for connections...");
                no_controllers_printed = true;
            }
            sleep_while_running(running, std::chrono::milliseconds(cfg.idle_sleep_ms));
        }
    }

    rumble.stop_all();
    send_udp_disconnect_packet(sock, dest, cfg.hmac_key, seq++);
    closesocket(sock);
    return 0;
}

void sender_thread_main(std::atomic<bool>& running, std::string host, uint16_t port) {
    ClientStreamConfig cfg{.host = std::move(host), .port = port,
                           .gui_features = true, .print_cli_waiting_messages = false,
                           .idle_sleep_ms = 50, .hmac_key = g_hmacKey};
    set_status_message("Connected to " + cfg.host + ":" + std::to_string(cfg.port));
    std::string err;
    int rc = run_client_stream(cfg, running, &err);
    if (rc != 0 && !err.empty()) {
        g_lastError = err;
        set_status_message(err);
    }
    g_connected.store(false);
    running.store(false, std::memory_order_relaxed);
}

std::expected<void, std::string> start_connection(const std::string& target) {
    if (g_connected.load()) return {};
    std::string host;
    int port = ns::DEFAULT_PORT;
    if (!parse_host_port(target, host, port)) return std::unexpected("Please enter a Raspberry Pi IP address.");
    if (!probe_server_sync(host, port)) return std::unexpected("Server not reachable. Check the IP address.");
    if (!g_sdlInput.start()) return std::unexpected("SDL3 input failed: " + g_sdlInput.error());
    derive_key(ns::DEFAULT_SECRET, g_hmacKey);
    save_last_ip(target);
    load_macro_entries();
    g_packetCount.store(0);
    g_serverLastReplyUs.store(0);
    g_serverRequestedDisconnect.store(false, std::memory_order_relaxed);
    g_lastError.clear();
    g_connected.store(true);
    if (g_senderThread.joinable()) {
        g_senderRunning = false;
        g_senderThread.join();
    }
    g_senderRunning = true;
    g_senderThread = std::thread(sender_thread_main, std::ref(g_senderRunning), host, (uint16_t)port);
    return {};
}

void stop_connection() {
    const bool was_connected = g_connected.exchange(false);
    if (g_senderThread.joinable()) {
        g_senderRunning = false;
        g_senderThread.join();
    }
    if (was_connected) set_status_message("Disconnected");
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

