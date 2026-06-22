#pragma once

bool load_switch2_wakeup_config(bool quiet_if_missing);
void enter_switch2_wake_runtime_mode();
void enter_bluetooth_runtime_mode();
void maybe_send_switch2_wake_advert(const char* reason);
int run_switch2_wakeup_setup();
bool wake_bt_state_was_modified();
void teardown_gadget();
void restore_wake_bt_state();
bool run_gadget_setup_if_needed(bool force, const char* reason);
bool run_revert_gadget_host();
void drain_hid_output_queue(int fd);
