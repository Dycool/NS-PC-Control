#include "usb_sleep_policy.hpp"

#include <cassert>

// A Pi without wake configuration must keep its client sessions, regardless of
// whether sleep was inferred from quiet USB reports or a USB suspend event.
static_assert(!usb_sleep_may_release_clients(false));
static_assert(usb_sleep_may_release_clients(true));

int main() {
    assert(!usb_sleep_may_release_clients(false));
    assert(usb_sleep_may_release_clients(true));
}
