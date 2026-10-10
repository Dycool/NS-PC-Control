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
}
