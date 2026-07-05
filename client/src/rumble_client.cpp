#include "rumble_client.hpp"
#include "input_settings.hpp"
#include "udp_protocol.hpp"
#include "macro_client.hpp"
#include "stream_runtime.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>
#include <span>

void RumbleManager::apply_precision_packet(const ns::PrecisionRumblePacket& rp, const int controller_for_slot[4]) {
    if (rp.subpad >= 4) return;
    ns::RumblePacket fallback{.magic = ns::RUMBLE_MAGIC, .subpad = rp.subpad,
                              .low_freq = rp.low_freq, .high_freq = rp.high_freq,
                              .duration_10ms = rp.duration_10ms};
    // Precision packets must not be blocked by the suppression window a previous
    // precision packet opened; that window only exists to mute duplicate classic packets.
    states[rp.subpad].suppress_classic_until_us = 0;
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
    g_sdlInput.clear_all_player_status();
}

void RumbleManager::set_output(int slot, uint8_t low, uint8_t high, uint32_t duration_ms, int pad_idx) {
    if (states[slot].last_controller != -1 && states[slot].last_controller != pad_idx)
        g_sdlInput.set_rumble(states[slot].last_controller, 0, 0, 0);
    if (pad_idx >= 0)
        g_sdlInput.set_rumble(pad_idx, low, high, (low || high) ? duration_ms : 0);
    states[slot].last_controller = pad_idx;
}

void pump_udp_replies(SOCKET sock, RumbleManager& rumble, const int controller_for_slot[4]) {
    uint8_t buf[1024];
    for (;;) {
        int n = (int)recvfrom(sock, reinterpret_cast<char*>(buf), sizeof(buf), 0, nullptr, nullptr);
        if (n < 0) break;
        if (n < (int)sizeof(uint32_t)) continue;

        uint32_t magic = 0;
        std::memcpy(&magic, buf, sizeof(magic));

        if (magic == ns::SERVER_INFO_MAGIC && n == sizeof(ns::ServerInfoReply)) {
            ns::ServerInfoReply reply{};
            std::memcpy(&reply, buf, sizeof(reply));
            if (reply.version == ns::SERVER_INFO_VERSION) {
                g_serverLastReplyUs.store(ns::now_us());
                if (reply.reserved[0] & ns::SERVER_INFO_FLAG_SWITCH_ASLEEP) {
                    g_serverRequestedDisconnect.store(true, std::memory_order_relaxed);
                }
                if (reply.reserved[0] & ns::SERVER_INFO_FLAG_SERVER_FULL) {
                    g_serverProbeFull.store(true, std::memory_order_relaxed);
                }
            }
        } else if (magic == ns::CLIENT_ASSIGNMENT_MAGIC && n == sizeof(ns::ClientAssignmentPacket)) {
            ns::ClientAssignmentPacket ap{};
            std::memcpy(&ap, buf, sizeof(ap));
            handle_client_assignment_packet(ap);
        } else if (magic == ns::CONTROLLER_STATUS_MAGIC && n == sizeof(ns::ControllerStatusPacket)) {
            ns::ControllerStatusPacket sp{};
            std::memcpy(&sp, buf, sizeof(sp));
            if (sp.version == ns::SERVER_INFO_VERSION && sp.subpad < 4) {
                int controller = controller_for_slot[sp.subpad];
                if (controller >= 0) {
                    int player_index = (sp.player_index < 4) ? static_cast<int>(sp.player_index) : -1;
                    const uint8_t* body_rgb = (sp.reserved[3] & ns::CONTROLLER_STATUS_FLAG_BODY_RGB_VALID) ? sp.reserved : nullptr;
                    g_sdlInput.set_player_status(controller, player_index, sp.player_leds, body_rgb);
                }
            }
        } else if (magic == ns::AMIIBO_STATUS_MAGIC && n == sizeof(ns::AmiiboStatusPacket)) {
            ns::AmiiboStatusPacket ap{};
            std::memcpy(&ap, buf, sizeof(ap));
            handle_amiibo_status_packet(ap);
        } else if (magic == ns::AMIIBO_CHUNK_MAGIC && n >= (int)sizeof(ns::AmiiboChunkHeader)) {
            handle_amiibo_chunk_packet(std::span<const uint8_t>(buf, static_cast<size_t>(n)));
        } else if (magic == ns::PRECISION_RUMBLE_MAGIC && n == sizeof(ns::PrecisionRumblePacket)) {
            ns::PrecisionRumblePacket rp{};
            std::memcpy(&rp, buf, sizeof(rp));
            rumble.apply_precision_packet(rp, controller_for_slot);
        } else if (magic == ns::RUMBLE_MAGIC && n == sizeof(ns::RumblePacket)) {
            ns::RumblePacket rp{};
            std::memcpy(&rp, buf, sizeof(rp));
            rumble.apply_packet(rp, controller_for_slot);
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
            if (reply.reserved[0] & ns::SERVER_INFO_FLAG_SERVER_FULL) {
                g_serverProbeFull.store(true, std::memory_order_relaxed);
            }
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
            if (reply.reserved[0] & ns::SERVER_INFO_FLAG_SERVER_FULL) {
                g_serverProbeFull.store(true, std::memory_order_relaxed);
                closesocket(sock);
                return true;
            }
            closesocket(sock);
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    closesocket(sock);
    return false;
}

