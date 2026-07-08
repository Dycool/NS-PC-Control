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

constexpr size_t UDP_RX_MAX_PACKET_SIZE = ns::PACKET_SIZE;

void web_server_thread(std::stop_token stoken, int web_port, uint16_t udp_port, bool serve_http_webapp);
