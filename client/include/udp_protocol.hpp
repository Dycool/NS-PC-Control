#pragma once

#include "platform.hpp"
#include "shared/protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <span>

constexpr uint8_t EXT_PAD_PRESENT = 0x01;

void set_pad_present_flag(ns::HIDReport& r, bool present);
void fill_extended_pad(ns::HIDReport& dst, const ns::HoriHIDReport& input,
                       bool present, const ns::MotionReport motion[3] = nullptr,
                       int battery_percent = -1, bool battery_charging = false,
                       bool motion_sample_fresh = false);
bool set_socket_nonblocking(SOCKET sock);
bool socket_would_block();
bool resolve_udp_destination(const std::string& host, int port, sockaddr_in& dest);
int send_all_udp(SOCKET sock, const sockaddr_in& dest, std::span<const uint8_t> data);
void send_udp_disconnect_packet(SOCKET sock, const sockaddr_in& dest,
                                const uint8_t hmac_key[32], uint32_t seq);

// One-off, connectionless request asking the server to switch its emulated USB
// controller family. Opens its own socket and blocks for up to ~1 second
// waiting for a GadgetModeReplyPacket. Returns false only when the server never
// replied (bad host, unreachable, or dropped packet); check out_reply.result
// for the server's actual decision (restarting/unchanged/server full).
bool send_gadget_mode_request_sync(const std::string& host, int port, ns::GadgetFamily family,
                                   const uint8_t hmac_key[32], ns::GadgetModeReplyPacket& out_reply);
