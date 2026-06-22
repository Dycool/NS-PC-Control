#pragma once

#include "platform.hpp"
#include "shared/protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <span>

constexpr uint8_t EXT_PAD_PRESENT = 0x01;

#ifdef _MSC_VER
#pragma pack(push, 1)
#endif
struct ExtendedUdpPacketPc {
    uint32_t magic = ns::PROTO_MAGIC;
    uint8_t version = ns::WEB_PROTO_VERSION_3;
    uint8_t flags = ns::FLAG_NONE;
    uint16_t reserved = 0;
    uint32_t seq = 0;
    uint64_t timestamp_us = 0;
    ns::ExtendedMultiReport3 report{};
    uint8_t hmac[ns::HMAC_TAG_SIZE]{};
} NS_PACKED_ATTR;
#ifdef _MSC_VER
#pragma pack(pop)
#endif

constexpr size_t EXT_UDP_PACKET3_AUTH_SIZE = 20 + sizeof(ns::ExtendedMultiReport3);
constexpr size_t EXT_UDP_PACKET3_SIZE = EXT_UDP_PACKET3_AUTH_SIZE + ns::HMAC_TAG_SIZE;
static_assert(sizeof(ExtendedUdpPacketPc) == EXT_UDP_PACKET3_SIZE, "ExtendedUdpPacketPc wire layout changed");

void set_pad_present_flag(ns::ExtendedHIDReport3& r, bool present);
void fill_extended_pad(ns::ExtendedHIDReport3& dst, const ns::HIDReport& input,
                       bool present, const ns::MotionReport motion[3] = nullptr);
bool set_socket_nonblocking(SOCKET sock);
bool socket_would_block();
bool resolve_udp_destination(const std::string& host, int port, sockaddr_in& dest);
int send_all_udp(SOCKET sock, const sockaddr_in& dest, std::span<const uint8_t> data);
void send_udp_disconnect_packet(SOCKET sock, const sockaddr_in& dest,
                                const uint8_t hmac_key[32], uint32_t seq, bool legacy_udp = false);
