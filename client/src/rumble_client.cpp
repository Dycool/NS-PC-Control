#include "rumble_client.hpp"
#include "input_settings.hpp"
#include "udp_protocol.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

void RumbleManager::apply_precision_packet(const ns::PrecisionRumblePacket& rp, const int controller_for_slot[4]) {
        if (rp.subpad >= 4) return;
        ns::RumblePacket fallback{};
        fallback.magic = ns::RUMBLE_MAGIC;
        fallback.subpad = rp.subpad;
        fallback.low_freq = rp.low_freq;
        fallback.high_freq = rp.high_freq;
        fallback.duration_10ms = rp.duration_10ms;
        apply_packet(fallback, controller_for_slot);
        states[rp.subpad].suppress_classic_until_us = ns::now_us() + 20000ULL;
    }

void RumbleManager::apply_packet(const ns::RumblePacket& rp, const int controller_for_slot[4]) {
        if (rp.subpad >= 4) return;
        if (!g_rumbleEnabled.load()) return;
        const int slot = rp.subpad;
        if (ns::now_us() < states[slot].suppress_classic_until_us) return;
        const uint8_t low = rp.low_freq;
        const uint8_t high = rp.high_freq;
        const bool neutral = (low == 0 && high == 0) || rp.duration_10ms == 0;
        const uint64_t now = ns::now_us();
        const uint64_t dur_us = neutral ? 0ULL : std::max<uint64_t>(250000ULL, (uint64_t)rp.duration_10ms * 10000ULL);
        if (!neutral && states[slot].low == low && states[slot].high == high &&
            now - states[slot].last_set_us < 100000ULL) {
            states[slot].until_us = now + dur_us;
            return;
        }
        states[slot].low = low;
        states[slot].high = high;
        states[slot].until_us = neutral ? 0ULL : now + dur_us;
        states[slot].last_set_us = now;
        set_output(slot, neutral ? 0 : low, neutral ? 0 : high, controller_for_slot[slot]);
    }

void RumbleManager::update_timeouts(const int controller_for_slot[4]) {
        const uint64_t now = ns::now_us();
        for (int i = 0; i < 4; ++i) {
            if (states[i].until_us != 0 && now > states[i].until_us) {
                states[i].until_us = 0;
                states[i].low = states[i].high = 0;
                set_output(i, 0, 0, controller_for_slot[i]);
            }
        }
    }

void RumbleManager::stop_all() {
        int none[4] = {-1, -1, -1, -1};
        for (int i = 0; i < 4; ++i) set_output(i, 0, 0, none[i]);
        g_sdlInput.stop_all_rumble();
    }

void RumbleManager::set_output(int slot, uint8_t low, uint8_t high, int pad_idx) {
        if (states[slot].last_controller != -1 && states[slot].last_controller != pad_idx)
            g_sdlInput.set_rumble(states[slot].last_controller, 0, 0, 0);
        if (pad_idx >= 0)
            g_sdlInput.set_rumble(pad_idx, low, high, (low || high) ? 250 : 0);
        states[slot].last_controller = pad_idx;
    }

void pump_udp_replies(SOCKET sock, RumbleManager& rumble, const int controller_for_slot[4]) {
    uint8_t buf[64];
    for (;;) {
        sockaddr_in from{};
#ifdef _WIN32
        int from_len = sizeof(from);
        int n = recvfrom(sock, reinterpret_cast<char*>(buf), (int)sizeof(buf), 0,
                         reinterpret_cast<sockaddr*>(&from), &from_len);
#else
        socklen_t from_len = sizeof(from);
        ssize_t n = recvfrom(sock, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &from_len);
#endif
        if (n < 0) {
            if (!socket_would_block()) {
                // Keep the sender alive; opportunistic reads.
            }
            break;
        }
        if (n == (int)sizeof(ns::ServerInfoReply)) {
            ns::ServerInfoReply reply{};
            std::memcpy(&reply, buf, sizeof(reply));
            if (reply.magic == ns::SERVER_INFO_MAGIC && reply.version == ns::SERVER_INFO_VERSION) {
                g_serverLastReplyUs.store(ns::now_us());
            }
        } else if (n == (int)sizeof(ns::PrecisionRumblePacket)) {
            ns::PrecisionRumblePacket rp{};
            std::memcpy(&rp, buf, sizeof(rp));
            if (rp.magic == ns::PRECISION_RUMBLE_MAGIC) rumble.apply_precision_packet(rp, controller_for_slot);
        } else if (n == (int)sizeof(ns::RumblePacket)) {
            ns::RumblePacket rp{};
            std::memcpy(&rp, buf, sizeof(rp));
            if (rp.magic == ns::RUMBLE_MAGIC) rumble.apply_packet(rp, controller_for_slot);
        }
    }
}

bool detect_server_is_legacy(SOCKET sock, const sockaddr_in& dest) {
    ns::ServerInfoProbe probe{};
    send_all_udp(sock, dest, &probe, sizeof(probe));
    const uint64_t deadline = ns::now_us() + 150000ULL;
    while (ns::now_us() < deadline) {
        ns::ServerInfoReply reply{};
        sockaddr_in from{};
#ifdef _WIN32
        int from_len = sizeof(from);
        int n = recvfrom(sock, reinterpret_cast<char*>(&reply), (int)sizeof(reply), 0,
                         reinterpret_cast<sockaddr*>(&from), &from_len);
#else
        socklen_t from_len = sizeof(from);
        ssize_t n = recvfrom(sock, &reply, sizeof(reply), 0, reinterpret_cast<sockaddr*>(&from), &from_len);
#endif
        if (n == (int)sizeof(reply) &&
            reply.magic == ns::SERVER_INFO_MAGIC &&
            reply.version == ns::SERVER_INFO_VERSION) {
            g_serverLastReplyUs.store(ns::now_us());
            return reply.backend == ns::SERVER_BACKEND_LEGACY;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // No reply: assume modern mode unless --hori was explicitly given.
    return false;
}

bool probe_server_sync(const std::string& host, int port) {
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == INVALID_SOCKET) return false;
    set_socket_nonblocking(sock);

    sockaddr_in dest{};
    if (!resolve_udp_destination(host, port, dest)) {
        closesocket(sock);
        return false;
    }

    ns::ServerInfoProbe probe{};
    const uint64_t deadline = ns::now_us() + 1000000ULL;
    int send_count = 0;
    int max_sends = 3;

    while (ns::now_us() < deadline) {
        // Retransmit probe periodically in case of packet loss
        if (send_count < max_sends) {
            send_all_udp(sock, dest, &probe, sizeof(probe));
            send_count++;
        }

        ns::ServerInfoReply reply{};
        sockaddr_in from{};
#ifdef _WIN32
        int from_len = sizeof(from);
        int n = recvfrom(sock, reinterpret_cast<char*>(&reply), (int)sizeof(reply), 0,
                         reinterpret_cast<sockaddr*>(&from), &from_len);
#else
        socklen_t from_len = sizeof(from);
        ssize_t n = recvfrom(sock, &reply, sizeof(reply), 0, reinterpret_cast<sockaddr*>(&from), &from_len);
#endif
        if (n == (int)sizeof(reply) &&
            reply.magic == ns::SERVER_INFO_MAGIC &&
            reply.version == ns::SERVER_INFO_VERSION) {
            closesocket(sock);
            return true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    closesocket(sock);
    return false;
}
