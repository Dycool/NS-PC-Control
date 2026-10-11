#pragma once

// USB host silence or suspend must not evict remote clients when the server
// has no usable wake configuration. Network inactivity and explicit client
// disconnects remain independent of this policy.
constexpr bool usb_sleep_may_release_clients(bool wake_configured) noexcept {
    return wake_configured;
}
