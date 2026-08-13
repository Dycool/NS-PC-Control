#pragma once

#include "virtual_controller.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

struct usb_ctrlrequest;

std::string s2_hex(std::span<const uint8_t> data);

// Native Switch 2 Pro Controller USB backend: HID interface for report 0x09/rumble 0x02 plus a
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

// Split-identity trick, S2 edition: the USB device keeps enumerating as a Pro
// Controller 2 (0x2069), but the PID in factory memory 0x13014 and the ep0
// identity block decide what the console treats the controller as.
// Pro2 = 0x69, Joy-Con 2 (R) = 0x66, (L) = 0x67 (low byte, high byte 0x20).
void switch2_native_set_port_pid(int port, uint8_t pid_lo);

bool switch2_native_streaming_enabled(int port);
uint8_t switch2_native_selected_report(int port);
uint32_t switch2_native_enabled_features(int port);
void switch2_native_note_hid_out(int port, std::span<const uint8_t> report);
