#pragma once

#include "app_state.hpp"
#include "shared/protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <netinet/in.h>

void flush_feedback_to_udp(int sock, int client_idx);