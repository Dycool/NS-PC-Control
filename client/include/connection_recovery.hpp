#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>

enum class ClientConnectionState : std::uint8_t { Disconnected, Connecting, Connected, Reconnecting };
enum class ClientDisconnectReason : std::uint8_t {
    None, Timeout, Unreachable, SocketError, ServerFull,
    UnsupportedProfile, SessionTerminated, SwitchAsleep, InputError,
};

// Explicit server lifecycle decisions are terminal; transport losses are not.
constexpr bool should_reconnect(ClientDisconnectReason reason) {
    return reason == ClientDisconnectReason::Timeout
        || reason == ClientDisconnectReason::Unreachable
        || reason == ClientDisconnectReason::SocketError;
}

// One-based attempt number, capped without overflowing on large values.
constexpr std::chrono::milliseconds reconnect_backoff(unsigned attempt) {
    constexpr int ms[] = {1000, 2000, 4000, 8000, 15000};
    return std::chrono::milliseconds(ms[std::min(attempt ? attempt - 1 : 0u, 4u)]);
}
