#pragma once

#include "platform.hpp"
#include "shared/protocol.hpp"

#include <cstdint>
#include <string>

class RumbleManager {
public:
    void apply_precision_packet(const ns::PrecisionRumblePacket& rp, const int controller_for_slot[4]);
    void apply_packet(const ns::RumblePacket& rp, const int controller_for_slot[4]);
    void update_timeouts(const int controller_for_slot[4]);
    void stop_all();

private:
    struct SlotState {
        uint8_t low = 0, high = 0;
        uint64_t until_us = 0;
        uint64_t last_set_us = 0;
        int last_controller = -1;
        uint64_t suppress_classic_until_us = 0;
    } states[4];

    void set_output(int slot, uint8_t low, uint8_t high, uint32_t duration_ms, int pad_idx);
};

void pump_udp_replies(SOCKET sock, RumbleManager& rumble,
                      const uint8_t hmac_key[32], const int controller_for_slot[4]);
bool detect_server_is_legacy(SOCKET sock, const sockaddr_in& dest);
bool probe_server_sync(const std::string& host, int port);
