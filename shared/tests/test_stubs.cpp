#include "app_state.hpp"

#include <cstdint>
#include <mutex>
#include <string>

bool load_switch2_wakeup_config(bool) { return false; }
void enter_switch2_wake_runtime_mode() {}
void enter_bluetooth_runtime_mode() {}
void maybe_send_switch2_wake_advert(const char*) {}
int run_switch2_wakeup_setup() { return 0; }
bool wake_bt_state_was_modified() { return false; }
void teardown_gadget() {}
void restore_wake_bt_state() {}
bool run_gadget_setup_if_needed(bool, const char*) { return true; }
void drain_hid_output_queue(int) {}

static std::mutex g_rate_mtx;

bool rate_allow(uint32_t ip) {
    std::lock_guard<std::mutex> lk(g_rate_mtx);
    uint64_t now = ns::now_us();
    uint32_t base = ip % RATE_TABLE;

    int victim = -1;
    uint64_t oldest_window = UINT64_MAX;
    for (int p = 0; p < RATE_PROBE; ++p) {
        uint32_t idx = (base + p) % RATE_TABLE;
        RateSlot &s = g_ctx.rate_table[idx];
        if (s.ip == ip) {
            if (now - s.window_start > RATE_WINDOW_US) { s.count = 1; s.window_start = now; return true; }
            if (s.count < UINT32_MAX) s.count++;
            return s.count <= RATE_MAX_PKT;
        }
        if (s.window_start < oldest_window) { oldest_window = s.window_start; victim = (int)idx; }
    }
    if (victim < 0) victim = (int)base;
    g_ctx.rate_table[victim] = {ip, 1, now};
    return true;
}

void flush_rumble_to_udp(int, int) {}
