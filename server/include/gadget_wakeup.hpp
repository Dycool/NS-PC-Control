#pragma once

#include <string>
#include <vector>

bool load_switch2_wakeup_config(bool quiet_if_missing);
void enter_switch2_wake_runtime_mode();
void enter_bluetooth_runtime_mode();
void maybe_send_switch2_wake_advert(const char* reason);
int run_switch2_wakeup_setup();
bool wake_bt_state_was_modified();
void teardown_gadget();
void emergency_unbind_udc();
void restore_wake_bt_state(bool restart_bluez = true);
bool run_gadget_setup_if_needed(bool force, const char* reason);
bool run_revert_gadget_host();
void drain_hid_output_queue(int fd);

// Modern FunctionFS transport helpers. Legacy mode still uses /dev/hidg*.
bool functionfs_transport_active();
bool functionfs_nodes_ready();
std::string functionfs_ep_in_path(int id);
std::string functionfs_ep_out_path(int id);
bool functionfs_poll_control_report(int id, std::vector<unsigned char>& out_report);
bool usb_transport_supports_nfc_reports();

// FunctionFS data-endpoint transport (dedicated blocking I/O threads own ep1/ep2).
bool functionfs_io_ready(int id);
bool functionfs_host_enabled(int id);
bool functionfs_submit_input_report(int id, const uint8_t* data, size_t len);
bool functionfs_poll_output_report(int id, std::vector<unsigned char>& out_report);
void functionfs_drain_output(int id);
