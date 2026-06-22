#include "rumble_client.hpp"
#include "input_settings.hpp"
#include "udp_protocol.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

void RumbleManager::apply_precision_packet(const ns::PrecisionRumblePacket& rp, const int controller_for_slot[4]) {
    if (rp.subpad >= 4) return;
    ns::RumblePacket fallback{.magic = ns::RUMBLE_MAGIC, .subpad = rp.subpad,
                              .low_freq = rp.low_freq, .high_freq = rp.high_freq,
                              .duration_10ms = rp.duration_10ms};
    apply_packet(fallback, controller_for_slot);
    states[rp.subpad].suppress_classic_until_us = ns::now_us() + 20000ULL;
}

void RumbleManager::apply_packet(const ns::RumblePacket& rp, const int controller_for_slot[4]) {
    if (rp.subpad >= 4 || !g_rumbleEnabled.load()) return;
    auto& s = states[rp.subpad];
    const uint64_t now = ns::now_us();
    if (now < s.suppress_classic_until_us) return;
    bool neutral = (rp.low_freq == 0 && rp.high_freq == 0) || rp.duration_10ms == 0;
    uint32_t dur_ms = neutral ? 0 : std::max(40u, (uint32_t)rp.duration_10ms * 10);
    uint64_t dur_us = (uint64_t)dur_ms * 1000;
    if (!neutral && s.low == rp.low_freq && s.high == rp.high_freq && now - s.last_set_us < 100000ULL) {
        s.until_us = now + dur_us;
        return;
    }
    s.low = rp.low_freq; s.high = rp.high_freq;
    s.until_us = neutral ? 0 : now + dur_us;
    s.last_set_us = now;
    set_output(rp.subpad, neutral ? 0 : rp.low_freq, neutral ? 0 : rp.high_freq, dur_ms, controller_for_slot[rp.subpad]);
}

void RumbleManager::update_timeouts(const int controller_for_slot[4]) {
    const uint64_t now = ns::now_us();
    for (int i = 0; i < 4; ++i) {
        if (states[i].until_us != 0 && now > states[i].until_us) {
            states[i].until_us = 0;
            states[i].low = states[i].high = 0;
            set_output(i, 0, 0, 0, controller_for_slot[i]);
        }
    }
}

void RumbleManager::stop_all() {
    int none[4] = {-1, -1, -1, -1};
    for (int i = 0; i < 4; ++i) set_output(i, 0, 0, 0, none[i]);
    g_sdlInput.stop_all_rumble();
}

void RumbleManager::set_output(int slot, uint8_t low, uint8_t high, uint32_t duration_ms, int pad_idx) {
    if (states[slot].last_controller != -1 && states[slot].last_controller != pad_idx)
        g_sdlInput.set_rumble(states[slot].last_controller, 0, 0, 0);
    if (pad_idx >= 0)
        g_sdlInput.set_rumble(pad_idx, low, high, (low || high) ? duration_ms : 0);
    states[slot].last_controller = pad_idx;
}

void pump_udp_replies(SOCKET sock, RumbleManager& rumble, const int controller_for_slot[4]) {
    uint8_t buf[64];
    for (;;) {
        int n = (int)recvfrom(sock, reinterpret_cast<char*>(buf), sizeof(buf), 0, nullptr, nullptr);
        if (n < 0) break;
        if (n == sizeof(ns::ServerInfoReply)) {
            ns::ServerInfoReply reply{};
            std::memcpy(&reply, buf, sizeof(reply));
            if (reply.magic == ns::SERVER_INFO_MAGIC && reply.version == ns::SERVER_INFO_VERSION) {
                g_serverLastReplyUs.store(ns::now_us());
            }
        } else if (n == sizeof(ns::PrecisionRumblePacket)) {
            ns::PrecisionRumblePacket rp{};
            std::memcpy(&rp, buf, sizeof(rp));
            if (rp.magic == ns::PRECISION_RUMBLE_MAGIC) rumble.apply_precision_packet(rp, controller_for_slot);
        } else if (n == sizeof(ns::RumblePacket)) {
            ns::RumblePacket rp{};
            std::memcpy(&rp, buf, sizeof(rp));
            if (rp.magic == ns::RUMBLE_MAGIC) rumble.apply_packet(rp, controller_for_slot);
        }
    }
}

bool detect_server_is_legacy(SOCKET sock, const sockaddr_in& dest) {
    ns::ServerInfoProbe probe{};
    send_all_udp(sock, dest, std::span(reinterpret_cast<const uint8_t*>(&probe), sizeof(probe)));
    const uint64_t deadline = ns::now_us() + 150000ULL;
    while (ns::now_us() < deadline) {
        ns::ServerInfoReply reply{};
        int n = (int)recvfrom(sock, reinterpret_cast<char*>(&reply), sizeof(reply), 0, nullptr, nullptr);
        if (n == sizeof(reply) && reply.magic == ns::SERVER_INFO_MAGIC && reply.version == ns::SERVER_INFO_VERSION) {
            g_serverLastReplyUs.store(ns::now_us());
            return reply.backend == ns::SERVER_BACKEND_LEGACY;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

bool probe_server_sync(const std::string& host, int port) {
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in dest{};
    if (sock == INVALID_SOCKET || !resolve_udp_destination(host, port, dest)) {
        if (sock != INVALID_SOCKET) closesocket(sock);
        return false;
    }
    set_socket_nonblocking(sock);
    ns::ServerInfoProbe probe{};
    uint64_t deadline = ns::now_us() + 1000000ULL;
    for (int i = 0; ns::now_us() < deadline; ++i) {
        if (i < 3) send_all_udp(sock, dest, std::span(reinterpret_cast<const uint8_t*>(&probe), sizeof(probe)));
        ns::ServerInfoReply reply{};
        int n = (int)recvfrom(sock, reinterpret_cast<char*>(&reply), sizeof(reply), 0, nullptr, nullptr);
        if (n == sizeof(reply) && reply.magic == ns::SERVER_INFO_MAGIC && reply.version == ns::SERVER_INFO_VERSION) {
            closesocket(sock);
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    closesocket(sock);
    return false;
}

