#include "rumble_client.hpp"
#include "audio_client.hpp"
#include "input_settings.hpp"
#include "udp_protocol.hpp"
#include "macro_client.hpp"
#include "qt_helpers.hpp"
#include "stream_runtime.hpp"
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <thread>
#include <span>
#include <QFileInfo>
#include <QSaveFile>


static void apply_server_info_reply(const ns::ServerInfoReply& reply) {
    g_serverLastReplyUs.store(ns::now_us());
    g_switch2ModeEnabled.store((reply.reserved[0] & ns::SERVER_INFO_FLAG_SWITCH2_MODE) != 0,
                               std::memory_order_relaxed);
    g_switch2AudioSupported.store((reply.reserved[0] & ns::SERVER_INFO_FLAG_S2_AUDIO) != 0,
                                  std::memory_order_relaxed);
    g_horiModeEnabled.store((reply.reserved[0] & ns::SERVER_INFO_FLAG_HORI_MODE) != 0,
                            std::memory_order_relaxed);
    if (reply.reserved[0] & (ns::SERVER_INFO_FLAG_SWITCH_ASLEEP
                           | ns::SERVER_INFO_FLAG_SESSION_TERMINATED)) {
        g_serverRequestedDisconnect.store(true, std::memory_order_relaxed);
    }
    if (reply.reserved[0] & ns::SERVER_INFO_FLAG_SERVER_FULL) {
        g_serverProbeFull.store(true, std::memory_order_relaxed);
    }
}

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

void pump_udp_replies(SOCKET sock, RumbleManager& rumble,
                      const uint8_t hmac_key[32], const int controller_for_slot[4]) {
    // Audio has moved to its own socket/thread, so this input-reply pump no
    // longer sees or dispatches audio datagrams.
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
            if (reply.version == ns::SERVER_INFO_VERSION) apply_server_info_reply(reply);
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
        } else if (magic == ns::ROSTER_MAGIC && n == sizeof(ns::RosterPacket)) {
            ns::RosterPacket rp{};
            std::memcpy(&rp, buf, sizeof(rp));
            handle_roster_packet(rp);
        } else if (magic == ns::PRECISION_RUMBLE_MAGIC && n == sizeof(ns::PrecisionRumblePacket)) {
            ns::PrecisionRumblePacket rp{};
            std::memcpy(&rp, buf, sizeof(rp));
            rumble.apply_precision_packet(rp, controller_for_slot);
        } else if (magic == ns::RUMBLE_MAGIC && n == sizeof(ns::RumblePacket)) {
            ns::RumblePacket rp{};
            std::memcpy(&rp, buf, sizeof(rp));
            rumble.apply_packet(rp, controller_for_slot);
        } else if (magic == ns::AMIIBO_REQUEST_MAGIC && n == sizeof(ns::AmiiboRequestPacket)) {
            ns::AmiiboRequestPacket ar{};
            std::memcpy(&ar, buf, sizeof(ar));
            if (ar.subpad < 4) {
                const uint16_t seq = static_cast<uint16_t>(ar.sequence_le[0])
                    | (static_cast<uint16_t>(ar.sequence_le[1]) << 8);
                const uint16_t previous = g_amiiboRequestSequence[ar.subpad].load();
                // Sequence zero is accepted for compatibility with an older
                // backend. Otherwise only a newer event may change the UI.
                if (seq == 0 || previous == 0 || static_cast<int16_t>(seq - previous) > 0) {
                    if (seq != 0) g_amiiboRequestSequence[ar.subpad] = seq;
                    const bool requested = ar.requested != 0;
                    g_amiiboScanDeadlineUs[ar.subpad] = requested
                        ? ns::now_us() + 10'000'000ULL : 0;
                    g_amiiboScanPending[ar.subpad] = requested;
                }
            }
        } else if (magic == ns::AMIIBO_DATA_MAGIC && n >= static_cast<int>(offsetof(ns::AmiiboDataPacket, data))) {
            ns::AmiiboDataPacket ad{};
            std::memcpy(&ad, buf, std::min((size_t)n, sizeof(ad)));
            const uint16_t amiibo_data_len = ad.data_len;
            constexpr size_t amiibo_packet_header = offsetof(ns::AmiiboDataPacket, data);
            const bool supported_size = amiibo_data_len == ns::AMIIBO_RAW_DUMP_SIZE
                || amiibo_data_len == ns::AMIIBO_EXTENDED_DUMP_SIZE;
            const bool complete_packet = static_cast<size_t>(n) >= amiibo_packet_header + amiibo_data_len;
            if (ad.subpad < 4 && supported_size && complete_packet) {
                // Writeback from server (modified Amiibo after NFC 0x14/0x08) -> persist to the selected dump.
                const QString path = amiibo_path_snapshot(ad.subpad);
                if (path.isEmpty()) {
                    set_status_message("Amiibo write completed, but no destination file is selected.");
                } else {
                    QSaveFile file(path);
                    const bool opened = file.open(QIODevice::WriteOnly);
                    const qint64 written = opened
                        ? file.write(reinterpret_cast<const char*>(ad.data), amiibo_data_len)
                        : -1;
                    const bool saved = opened && written == amiibo_data_len && file.commit();
                    if (saved) {
                        set_status_message("Amiibo saved to " + q_to_std(QFileInfo(path).fileName()));
                    } else {
                        if (opened && written != amiibo_data_len) file.cancelWriting();
                        set_status_message("Failed to save Amiibo: " + q_to_std(file.errorString()));
                    }
                }
                g_amiiboScanPending[ad.subpad] = false;
                g_amiiboScanDeadlineUs[ad.subpad] = 0;
            }
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
            apply_server_info_reply(reply);
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
            apply_server_info_reply(reply);
            closesocket(sock);
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    closesocket(sock);
    return false;
}
