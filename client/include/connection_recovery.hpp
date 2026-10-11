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

// A missed UDP heartbeat is only a health warning, never a reason to close
// the controller-input stream. Keep transmitting while the server is silent.
constexpr bool udp_heartbeat_unresponsive(std::uint64_t now_us,
                                         std::uint64_t last_probe_us,
                                         std::uint64_t last_reply_us) noexcept {
    constexpr std::uint64_t WARNING_AFTER_US = 15'000'000ULL;
    return last_probe_us != 0 && last_reply_us != 0
        && now_us > last_reply_us
        && now_us - last_reply_us > WARNING_AFTER_US;
}

// One-based attempt number, capped without overflowing on large values.
constexpr std::chrono::milliseconds reconnect_backoff(unsigned attempt) {
    constexpr int ms[] = {1000, 2000, 4000, 8000, 15000};
    return std::chrono::milliseconds(ms[std::min(attempt ? attempt - 1 : 0u, 4u)]);
}
