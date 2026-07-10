#include "udp_feedback.hpp"
#include "app_state.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <print>

using namespace ns;

void flush_feedback_to_udp(int sock, int client_idx) {
    if (sock < 0 || client_idx < 0 || client_idx >= MAX_CLIENTS) return;
    sockaddr_in dest{};
    ns::RumblePacket pending[4]{};
    bool has[4]{};
    ns::ClientAssignmentPacket pending_assignment[4]{};
    bool has_assignment[4]{};
    ns::ControllerStatusPacket pending_status[4]{};
    bool has_status[4]{};
    const uint64_t state_seq = refresh_server_state_seq();
    const uint8_t active_clients = static_cast<uint8_t>(std::clamp(active_client_count(), 0, MAX_CLIENTS));
    const uint8_t free_slots = static_cast<uint8_t>(std::clamp(free_virtual_slot_count(), 0, HID_PORT_COUNT));
    const bool switch_asleep = switch2_sleep_confirmed();
    const uint64_t roster_seq = refresh_roster_seq();
    ns::RosterPacket roster_pkt{};
    get_roster_packet(roster_pkt);
    bool has_roster = false;

    ns::AmiiboRequestPacket pending_amiibo_request[4]{};
    bool has_amiibo_request[4]{};
    ns::AmiiboDataPacket pending_amiibo_data[4]{};
    bool has_amiibo_data[4]{};

    {
        std::lock_guard<std::mutex> lk(g_ctx.mtx[client_idx]);
        ClientSession& c = g_ctx.clients[client_idx];
        if (!c.active || !c.udp_rumble_enabled) return;
        dest = c.addr;
        bool any_assignment_packet = false;
        for (int s = 0; s < 4; ++s) {
            uint32_t assignment_seq = c.client_assignment_seq[s];
            if (assignment_seq != c.udp_last_client_assignment_seq[s]) {
                pending_assignment[s] = ns::ClientAssignmentPacket{};
                pending_assignment[s].flags = ns::CLIENT_ASSIGNMENT_FLAG_ACCEPTED;
                pending_assignment[s].server_slot = static_cast<uint8_t>(client_idx);
                pending_assignment[s].subpad = static_cast<uint8_t>(s);
                pending_assignment[s].console_port_mask = c.client_assignment[s].console_port_mask;
                pending_assignment[s].primary_console_port = c.client_assignment[s].primary_console_port;
                pending_assignment[s].requested_type = c.client_assignment[s].requested_type;
                pending_assignment[s].virtual_type = c.client_assignment[s].virtual_type;
                if (pending_assignment[s].console_port_mask != 0) pending_assignment[s].flags |= ns::CLIENT_ASSIGNMENT_FLAG_ASSIGNMENT_VALID;
                if (switch_asleep) pending_assignment[s].flags |= ns::CLIENT_ASSIGNMENT_FLAG_SWITCH_ASLEEP;
                pending_assignment[s].active_clients = active_clients;
                pending_assignment[s].max_clients = MAX_CLIENTS;
                pending_assignment[s].free_virtual_slots = free_slots;
                c.udp_last_client_assignment_seq[s] = assignment_seq;
                has_assignment[s] = true;
                any_assignment_packet = true;
            }
            uint32_t seq = c.rumble_seq[s];
            if (seq != c.udp_last_rumble_seq[s]) {
                pending[s] = c.rumble[s];
                c.udp_last_rumble_seq[s] = seq;
                has[s] = true;
            }
            uint32_t status_seq = c.controller_status_seq[s];
            if (status_seq != c.udp_last_controller_status_seq[s]) {
                pending_status[s] = ns::ControllerStatusPacket{};
                pending_status[s].subpad = static_cast<uint8_t>(s);
                pending_status[s].player_index = c.controller_status[s].player_index;
                pending_status[s].player_leds = c.controller_status[s].player_leds;
                if (c.controller_status[s].body_rgb_valid) {
                    pending_status[s].reserved[0] = c.controller_status[s].body_rgb[0];
                    pending_status[s].reserved[1] = c.controller_status[s].body_rgb[1];
                    pending_status[s].reserved[2] = c.controller_status[s].body_rgb[2];
                    pending_status[s].reserved[3] |= ns::CONTROLLER_STATUS_FLAG_BODY_RGB_VALID;
                }
                c.udp_last_controller_status_seq[s] = status_seq;
                has_status[s] = true;
            }
        }
        const uint64_t now = ns::now_us();
        const bool periodic_resend = c.udp_last_roster_send_us == 0 || now - c.udp_last_roster_send_us >= 2'000'000ULL;
        if (roster_seq != c.udp_last_roster_seq || periodic_resend) {
            c.udp_last_roster_seq = roster_seq;
            c.udp_last_roster_send_us = now;
            has_roster = true;
        }
        if (state_seq != c.udp_last_server_state_seq) {
            c.udp_last_server_state_seq = state_seq;
            if (!any_assignment_packet) {
                pending_assignment[0] = ns::ClientAssignmentPacket{};
                pending_assignment[0].flags = ns::CLIENT_ASSIGNMENT_FLAG_ACCEPTED;
                pending_assignment[0].server_slot = static_cast<uint8_t>(client_idx);
                pending_assignment[0].subpad = 0;
                pending_assignment[0].console_port_mask = c.client_assignment[0].console_port_mask;
                pending_assignment[0].primary_console_port = c.client_assignment[0].primary_console_port;
                pending_assignment[0].requested_type = c.client_assignment[0].requested_type;
                pending_assignment[0].virtual_type = c.client_assignment[0].virtual_type;
                if (pending_assignment[0].console_port_mask != 0) pending_assignment[0].flags |= ns::CLIENT_ASSIGNMENT_FLAG_ASSIGNMENT_VALID;
                if (switch_asleep) pending_assignment[0].flags |= ns::CLIENT_ASSIGNMENT_FLAG_SWITCH_ASLEEP;
                pending_assignment[0].active_clients = active_clients;
                pending_assignment[0].max_clients = MAX_CLIENTS;
                pending_assignment[0].free_virtual_slots = free_slots;
                has_assignment[0] = true;
            }
        }

        for (int s = 0; s < 4; ++s) {
            if (c.amiibo_request_pending[s]) {
                pending_amiibo_request[s] = ns::AmiiboRequestPacket{};
                pending_amiibo_request[s].subpad = static_cast<uint8_t>(s);
                pending_amiibo_request[s].requested = c.amiibo_requested[s] ? 1 : 0;
                has_amiibo_request[s] = true;
                c.amiibo_request_pending[s] = false;
            }
            if (c.amiibo_writeback_pending[s]) {
                pending_amiibo_data[s] = ns::AmiiboDataPacket{};
                pending_amiibo_data[s].subpad = static_cast<uint8_t>(s);
                pending_amiibo_data[s].data_len = c.amiibo_writeback_len[s];
                std::memcpy(pending_amiibo_data[s].data, c.amiibo_writeback_data[s], std::min<size_t>(c.amiibo_writeback_len[s], 540));
                has_amiibo_data[s] = true;
                c.amiibo_writeback_pending[s] = false;
            }
        }
    }

    // Gameplay feedback first; UI/server-state packets are event-driven and
    // should never delay rumble when both are pending in the same tick.
    for (int s = 0; s < 4; ++s) {
        if (!has[s]) continue;
        ssize_t sent = sendto(sock, &pending[s], sizeof(ns::RumblePacket), 0,
                              reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
        if (g_ctx.verbose && sent != static_cast<ssize_t>(sizeof(ns::RumblePacket)))
            std::println(stderr, "[udp] failed to send rumble packet: {}", std::strerror(errno));
    }
    for (int s = 0; s < 4; ++s) {
        if (has_assignment[s]) {
            ssize_t sent = sendto(sock, &pending_assignment[s], sizeof(ns::ClientAssignmentPacket), 0,
                                  reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
            if (g_ctx.verbose && sent != static_cast<ssize_t>(sizeof(ns::ClientAssignmentPacket)))
                std::println(stderr, "[udp] failed to send assignment packet: {}", std::strerror(errno));
        }
        if (has_status[s]) {
            ssize_t sent = sendto(sock, &pending_status[s], sizeof(ns::ControllerStatusPacket), 0,
                                  reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
            if (g_ctx.verbose && sent != static_cast<ssize_t>(sizeof(ns::ControllerStatusPacket)))
                std::println(stderr, "[udp] failed to send controller status packet: {}", std::strerror(errno));
        }
    }
    if (has_roster) {
        ssize_t sent = sendto(sock, &roster_pkt, sizeof(ns::RosterPacket), 0,
                              reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
        if (g_ctx.verbose && sent != static_cast<ssize_t>(sizeof(ns::RosterPacket)))
            std::println(stderr, "[udp] failed to send roster packet: {}", std::strerror(errno));
    }

    // Amiibo packets
    for (int s = 0; s < 4; ++s) {
        if (has_amiibo_request[s]) {
            ssize_t sent = sendto(sock, &pending_amiibo_request[s], sizeof(ns::AmiiboRequestPacket), 0,
                                  reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
            if (g_ctx.verbose && sent < 0)
                std::println(stderr, "[udp] failed to send amiibo request: {}", std::strerror(errno));
        }
        if (has_amiibo_data[s]) {
            ssize_t sent = sendto(sock, &pending_amiibo_data[s], sizeof(ns::AmiiboDataPacket), 0,
                                  reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
            if (g_ctx.verbose && sent < 0)
                std::println(stderr, "[udp] failed to send amiibo writeback: {}", std::strerror(errno));
        }
    }
}
