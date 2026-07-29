#pragma once

#include <cstdint>
#include <string_view>

enum class S2EnumerationState : uint8_t {
    WaitingForHost,
    UsbConfigured,
    NativeHandshake,
    Streaming,
    Recovering,
};

constexpr unsigned SWITCH2_MAX_ENUMERATION_RETRIES = 3;

S2EnumerationState switch2_enumeration_state();
unsigned switch2_enumeration_failure_count();

void switch2_enumeration_gadget_started();
void switch2_enumeration_usb_configured();
void switch2_enumeration_native_handshake();
void switch2_enumeration_streaming_validated(uint8_t report_id);
void switch2_enumeration_bus_reset();
void switch2_enumeration_client_connected();
void switch2_enumeration_client_disconnected();

// Records one concrete enumeration/mandatory-handshake failure. Duplicate
// requests are coalesced until the writer services the pending recovery.
void request_switch2_reenumeration(std::string_view reason);

// Called only by the USB writer, which owns gadget teardown/recreation.
// Returns true when it consumed a pending request.
bool service_switch2_reenumeration();

