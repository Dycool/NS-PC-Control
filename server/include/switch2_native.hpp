#pragma once

#include "virtual_controller.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

struct usb_ctrlrequest;

// Native Switch 2 Pro Controller USB backend, ported from PicoSwitch2's
// ns2-testing model: HID interface for report 0x09/rumble 0x02 plus a
// vendor-bulk command channel for init, pairing, feature select and memory.
void switch2_native_init();
void switch2_native_reset_port(int port);

bool switch2_native_handle_ep0_request(int port,
                                      const usb_ctrlrequest& ctrl,
                                      std::vector<uint8_t>& response,
                                      bool& status_only);

bool switch2_native_handle_vendor_command(int port,
                                          std::span<const uint8_t> command,
                                          std::vector<uint8_t>& response,
                                          ControllerRuntime& rt);

bool switch2_native_streaming_enabled(int port);
uint8_t switch2_native_selected_report(int port);
uint32_t switch2_native_enabled_features(int port);
void switch2_native_note_hid_out(int port, std::span<const uint8_t> report);
