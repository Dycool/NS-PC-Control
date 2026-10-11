#include "connection_recovery.hpp"
#include <cassert>
#include <chrono>
int main() {
    using namespace std::chrono_literals;
    assert(reconnect_backoff(0) == 1s);
    assert(reconnect_backoff(1) == 1s);
    assert(reconnect_backoff(2) == 2s);
    assert(reconnect_backoff(3) == 4s);
    assert(reconnect_backoff(4) == 8s);
    assert(reconnect_backoff(5) == 15s);
    assert(reconnect_backoff(1000000) == 15s);
    assert(should_reconnect(ClientDisconnectReason::Timeout));
    assert(should_reconnect(ClientDisconnectReason::Unreachable));
    assert(should_reconnect(ClientDisconnectReason::SocketError));
    assert(!should_reconnect(ClientDisconnectReason::None));
    assert(!should_reconnect(ClientDisconnectReason::ServerFull));
    assert(!should_reconnect(ClientDisconnectReason::UnsupportedProfile));
    assert(!should_reconnect(ClientDisconnectReason::SessionTerminated));
    assert(!should_reconnect(ClientDisconnectReason::SwitchAsleep));
    assert(!should_reconnect(ClientDisconnectReason::InputError));

    // A heartbeat gap changes diagnostic state, not the stream's lifetime.
    constexpr std::uint64_t probe = 10'000'000ULL;
    constexpr std::uint64_t reply = 11'000'000ULL;
    assert(!udp_heartbeat_unresponsive(reply + 15'000'000ULL, probe, reply));
    assert(udp_heartbeat_unresponsive(reply + 15'000'001ULL, probe, reply));
    assert(!udp_heartbeat_unresponsive(reply + 60'000'000ULL, 0, reply));
    assert(!udp_heartbeat_unresponsive(reply + 60'000'000ULL, probe, 0));
    assert(!udp_heartbeat_unresponsive(reply - 1, probe, reply));
    // One later server reply immediately clears the warning.
    assert(!udp_heartbeat_unresponsive(reply + 60'000'000ULL, probe,
                                       reply + 59'000'000ULL));
}
