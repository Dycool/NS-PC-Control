#pragma once

bool load_switch2_wakeup_config(bool quiet_if_missing);
void maybe_send_switch2_wake_advert(const char* reason);
int run_switch2_wakeup_setup();
void teardown_gadget();
void restore_wake_bt_state();
bool run_gadget_setup_if_needed(bool force, const char* reason);
void drain_hid_output_queue(int fd);
