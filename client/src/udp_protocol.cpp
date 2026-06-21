#include "udp_protocol.hpp"
#include "shared/sha256.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

void set_pad_present_flag(ns::ExtendedHIDReport3& r, bool present) {
    if (present) r.input.vendor |= EXT_PAD_PRESENT;
    else r.input.vendor &= (uint8_t)~EXT_PAD_PRESENT;
}

void fill_extended_pad(ns::ExtendedHIDReport3& dst, const ns::HIDReport& input,
                              bool present, const ns::MotionReport motion[3]) {
    dst.reset();
    dst.input = input;
    set_pad_present_flag(dst, present);
    if (motion) {
        dst.motion[0] = motion[0];
        dst.motion[1] = motion[1];
        dst.motion[2] = motion[2];
        dst.has_motion = 1;
    }
}

bool set_socket_nonblocking(SOCKET sock) {
#ifdef _WIN32
    u_long nonblocking = 1;
    return ioctlsocket(sock, FIONBIO, &nonblocking) == 0;
#else
    int flags = fcntl(sock, F_GETFL, 0);
    return flags >= 0 && fcntl(sock, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

bool socket_would_block() {
#ifdef _WIN32
    int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINTR;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

bool resolve_udp_destination(const std::string& host, int port, sockaddr_in& dest) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    char port_buf[8];
    std::snprintf(port_buf, sizeof(port_buf), "%d", port);
    if (getaddrinfo(host.c_str(), port_buf, &hints, &res) != 0 || !res) return false;
    std::memcpy(&dest, res->ai_addr, sizeof(dest));
    freeaddrinfo(res);
    return true;
}

int send_all_udp(SOCKET sock, const sockaddr_in& dest, const void* data, size_t len) {
    return sendto(sock, reinterpret_cast<const char*>(data), (int)len, 0,
                  reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
}

void send_udp_disconnect_packet(SOCKET sock, const sockaddr_in& dest,
                                       const uint8_t hmac_key[32], uint32_t seq, bool legacy_udp) {
    if (sock == INVALID_SOCKET) return;
    if (legacy_udp) {
        ns::Packet pkt{};
        pkt.magic = ns::PROTO_MAGIC;
        pkt.version = ns::PROTO_VERSION;
        pkt.flags = ns::FLAG_RESET | ns::FLAG_DISCONNECT;
        pkt.seq = seq;
        pkt.ts_us = ns::now_us();
        pkt.report.reset();
        uint8_t full_hmac[32];
        hmac_sha256(hmac_key, 32, reinterpret_cast<const uint8_t*>(&pkt), ns::PACKET_AUTH_SIZE, full_hmac);
        std::memcpy(pkt.hmac, full_hmac, ns::HMAC_TAG_SIZE);
        for (int i = 0; i < 3; ++i) {
            send_all_udp(sock, dest, &pkt, ns::PACKET_SIZE);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return;
    }
    ExtendedUdpPacket pkt{};
    pkt.magic = ns::PROTO_MAGIC;
    pkt.version = ns::WEB_PROTO_VERSION_3;
    pkt.flags = ns::FLAG_RESET | ns::FLAG_DISCONNECT;
    pkt.seq = seq;
    pkt.timestamp_us = ns::now_us();
    pkt.report.reset();
    uint8_t full_hmac[32];
    hmac_sha256(hmac_key, 32, reinterpret_cast<const uint8_t*>(&pkt), EXT_UDP_PACKET_AUTH_SIZE, full_hmac);
    std::memcpy(pkt.hmac, full_hmac, ns::HMAC_TAG_SIZE);
    for (int i = 0; i < 3; ++i) {
        send_all_udp(sock, dest, &pkt, sizeof(pkt));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
