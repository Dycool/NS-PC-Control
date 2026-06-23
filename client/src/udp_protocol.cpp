#include "udp_protocol.hpp"
#include "shared/sha256.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

void set_pad_present_flag(ns::HIDReport& r, bool present) {
    if (present) r.input.vendor |= EXT_PAD_PRESENT;
    else r.input.vendor &= (uint8_t)~EXT_PAD_PRESENT;
}

void fill_extended_pad(ns::HIDReport& dst, const ns::HoriHIDReport& input,
                              bool present, const ns::MotionReport motion[3], int battery_percent, bool battery_charging) {
    dst.reset();
    dst.input = input;
    set_pad_present_flag(dst, present);
    if (battery_percent >= 0 && battery_percent <= 100) {
        dst.reserved[0] = static_cast<uint8_t>(battery_percent);
        dst.reserved[1] |= ns::EXT_STATUS_BATTERY_VALID;
        if (battery_charging) dst.reserved[1] |= ns::EXT_STATUS_BATTERY_CHARGING;
    }
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
    std::string port_str = std::to_string(port);
    if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0 || !res) return false;
    std::memcpy(&dest, res->ai_addr, sizeof(dest));
    freeaddrinfo(res);
    return true;
}

int send_all_udp(SOCKET sock, const sockaddr_in& dest, std::span<const uint8_t> data) {
    return sendto(sock, reinterpret_cast<const char*>(data.data()), (int)data.size(), 0,
                  reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
}

void send_udp_disconnect_packet(SOCKET sock, const sockaddr_in& dest,
                                const uint8_t hmac_key[32], uint32_t seq) {
    if (sock == INVALID_SOCKET) return;
    auto send_one = [&](void* pkt, size_t size, size_t auth_size) {
        uint8_t full_hmac[32];
        hmac_sha256(std::span(hmac_key, 32), std::span(static_cast<const uint8_t*>(pkt), auth_size), std::span<uint8_t, 32>(full_hmac));
        std::memcpy(static_cast<uint8_t*>(pkt) + auth_size, full_hmac, ns::HMAC_TAG_SIZE);
        for (int i = 0; i < 3; ++i) {
            send_all_udp(sock, dest, std::span(static_cast<const uint8_t*>(pkt), size));
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    };
    ns::Packet pkt{};
    pkt.magic = ns::PROTO_MAGIC; pkt.version = ns::WEB_PROTO_VERSION;
    pkt.flags = ns::FLAG_RESET | ns::FLAG_DISCONNECT; pkt.seq = seq;
    pkt.ts_us = ns::now_us(); pkt.report.reset();
    send_one(&pkt, ns::PACKET_SIZE, ns::PACKET_AUTH_SIZE);
}

