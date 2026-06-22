#pragma once

#include "app_state.hpp"
#include "shared/protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <netinet/in.h>
#include <span>
#include <stop_token>

constexpr int MAX_WS_CLIENTS = 32;

struct WebClient;

#ifndef NS_LOCAL_PACKED
#define NS_LOCAL_PACKED __attribute__((packed))
#endif

struct NS_LOCAL_PACKED ExtendedUdpPacket {
    uint32_t magic;
    uint8_t version;
    uint8_t flags;
    uint16_t reserved;
    uint32_t seq;
    uint64_t timestamp_us;
    ns::ExtendedMultiReport report;
    uint8_t hmac[ns::HMAC_TAG_SIZE];
};

struct NS_LOCAL_PACKED ExtendedUdpPacketPc {
    uint32_t magic;
    uint8_t version;
    uint8_t flags;
    uint16_t reserved;
    uint32_t seq;
    uint64_t timestamp_us;
    ns::ExtendedMultiReport3 report;
    uint8_t hmac[ns::HMAC_TAG_SIZE];
};

constexpr size_t EXT_UDP_PACKET_AUTH_SIZE = 20 + sizeof(ns::ExtendedMultiReport);
constexpr size_t EXT_UDP_PACKET_SIZE = EXT_UDP_PACKET_AUTH_SIZE + ns::HMAC_TAG_SIZE;
constexpr size_t EXT3_UDP_PACKET_AUTH_SIZE = 20 + sizeof(ns::ExtendedMultiReport3);
constexpr size_t EXT3_UDP_PACKET_SIZE = EXT3_UDP_PACKET_AUTH_SIZE + ns::HMAC_TAG_SIZE;
constexpr size_t UDP_RX_MAX_PACKET_SIZE =
    sizeof(ExtendedUdpPacketPc) > sizeof(ns::Packet) ? sizeof(ExtendedUdpPacketPc) : sizeof(ns::Packet);
static_assert(sizeof(ExtendedUdpPacket) == EXT_UDP_PACKET_SIZE, "ExtendedUdpPacket size must match its wire format");
static_assert(sizeof(ExtendedUdpPacketPc) == EXT3_UDP_PACKET_SIZE, "ExtendedUdpPacketPc size must match its wire format");

void flush_rumble_to_udp(int sock, int client_idx);
void web_server_thread(std::stop_token stoken, int web_port, uint16_t udp_port, bool serve_http_webapp);
