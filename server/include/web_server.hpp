#pragma once

#include "app_state.hpp"
#include "shared/protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <netinet/in.h>

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

struct NS_LOCAL_PACKED ExtendedUdpPacket3 {
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
    sizeof(ExtendedUdpPacket3) > sizeof(ns::Packet) ? sizeof(ExtendedUdpPacket3) : sizeof(ns::Packet);
static_assert(sizeof(ExtendedUdpPacket) == EXT_UDP_PACKET_SIZE, "ExtendedUdpPacket size must match its wire format");
static_assert(sizeof(ExtendedUdpPacket3) == EXT3_UDP_PACKET_SIZE, "ExtendedUdpPacket3 size must match its wire format");

void base64_encode(const uint8_t* in, size_t len, char* out);
void legacy_multi_to_extended(const ns::MultiReport& in, ns::ExtendedMultiReport& out);
bool extended_udp_packet_ok(const ExtendedUdpPacket& p);
bool extended_udp3_packet_ok(const ExtendedUdpPacket3& p);
bool extended_report_pad_present(const ns::ExtendedMultiReport& report, int subpad);
bool extended3_report_pad_present(const ns::ExtendedMultiReport3& report, int subpad);
void extended3_to_extended_latest(const ns::ExtendedHIDReport3& in, ns::ExtendedHIDReport& out);
void clear_udp_rumble_state(ClientSession& c);
void reset_udp_client_session_locked(ClientSession& c);
void enable_udp_rumble_state(ClientSession& c);
void flush_rumble_to_udp(int sock, int client_idx);
bool send_ws_binary_frame(WebClient* c, const uint8_t* payload, size_t len);
void flush_rumble_to_ws(WebClient* c);
size_t process_ws_frame(WebClient* c);
int ws_upgrade(const char* key_line, char* resp, size_t resp_sz);
bool has_header(const char* buf, const char* header);
bool req_match(const char* buf, const char* path);
void web_server_thread(int web_port, uint16_t udp_port, bool serve_http_webapp);
