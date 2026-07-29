#pragma once

#include "shared/protocol.hpp"
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

bool load_switch2_wakeup_config(bool quiet_if_missing);
void enter_switch2_wake_runtime_mode();
void enter_bluetooth_runtime_mode();
void maybe_send_switch2_wake_advert(const char* reason, bool force = false);
int run_switch2_wakeup_setup();
bool wake_bt_state_was_modified();
void teardown_gadget();
void emergency_unbind_udc();
void restore_wake_bt_state(bool restart_bluez = true);
bool run_gadget_setup_if_needed(bool force, const char* reason);
void drain_hid_output_queue(int fd);

bool s2_gadget_transport_active();
bool s2_gadget_nodes_ready();
bool s2_gadget_io_ready(int id);
bool s2_gadget_host_enabled(int id);
bool s2_gadget_submit_input_report(int id, const uint8_t* data, size_t len);
bool s2_gadget_poll_control_report(int id, std::vector<unsigned char>& out_report);
bool s2_gadget_poll_output_report(int id, std::vector<unsigned char>& out_report);
void s2_gadget_drain_output(int id);
bool s2_gadget_poll_vendor_report(int id, std::vector<unsigned char>& out_report);
bool s2_gadget_submit_vendor_report(int id, const uint8_t* data, size_t len,
                                    std::span<const uint8_t> request);
