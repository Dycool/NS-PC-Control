#include "writers.hpp"
#include "app_state.hpp"
#include "gadget_wakeup.hpp"
#include "virtual_controller.hpp"
#include "bluetooth_manager.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <print>
#include <span>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <thread>
#include <mutex>
#include <vector>

using namespace ns;

static const HIDReport& get_hid_report(const ClientSession& c, int s) {
    if (s == 0) return c.report.p1;
    if (s == 1) return c.report.p2;
    if (s == 2) return c.report.p3;
    return c.report.p4;
}

void writer_thread(std::stop_token stoken, int hz) {
    const int nports = HID_PORT_COUNT;
    if (!g_ctx.legacy_mode) {
        for (int i = 0; i < nports; ++i) {
            // Default to Pro Controller; the client picks each pad's type per port
            // via HIDReport::reserved[2].
            set_controller_type_for_port(i, NS_TYPE_PRO);
            init_spi_flash(i);
        }
    }

    const auto tick = us(1'000'000 / hz);
    int write_fds[HID_PORT_COUNT] = {-1, -1, -1, -1};
    int read_fds[HID_PORT_COUNT]  = {-1, -1, -1, -1};
    // The controller type (Pro / Joy-Con L/R) the console last read at enumeration.
    // The console only re-reads device info on a fresh USB handshake, so a client
    // changing a pad's type has to re-enumerate the gadget to take effect.
    uint8_t enumerated_type[HID_PORT_COUNT];
    for (int i = 0; i < HID_PORT_COUNT; ++i) enumerated_type[i] = controller_type_for_port(i);

    struct HwSlot { int client_idx = -1; int sub_idx = -1; };
    HwSlot hw_slots[HID_PORT_COUNT];
    ControllerRuntime rt[HID_PORT_COUNT];
    for (int i = 0; i < HID_PORT_COUNT; ++i) rt[i].ctrl = i;

    auto close_port_fds = [&](int i) {
        const int old_write_fd = write_fds[i];
        if (write_fds[i] >= 0) close(write_fds[i]);
        if (read_fds[i] >= 0 && read_fds[i] != old_write_fd) close(read_fds[i]);
        write_fds[i] = read_fds[i] = -1;
        rt[i].fd = -1;
    };
    auto close_all_fds = [&]() {
        for (int i = 0; i < HID_PORT_COUNT; ++i) close_port_fds(i);
    };

    while (!stoken.stop_requested()) {
        bool all_open = true;
        for (int i = 0; i < nports; ++i) {
            if (write_fds[i] < 0 || read_fds[i] < 0) {
                const int old_write_fd = write_fds[i];
                if (write_fds[i] >= 0) close(write_fds[i]);
                if (read_fds[i] >= 0 && read_fds[i] != old_write_fd) close(read_fds[i]);
                write_fds[i] = read_fds[i] = -1;

                if (g_ctx.legacy_mode) {
                    const std::string dev = "/dev/hidg" + std::to_string(i);
                    write_fds[i] = open(dev.c_str(), O_WRONLY | O_NONBLOCK);
                    read_fds[i] = write_fds[i];
                } else {
                    write_fds[i] = open(functionfs_ep_in_path(i).c_str(), O_WRONLY | O_NONBLOCK);
                    read_fds[i]  = open(functionfs_ep_out_path(i).c_str(), O_RDONLY | O_NONBLOCK);
                }

                const bool opened = g_ctx.legacy_mode
                    ? (write_fds[i] >= 0)
                    : (write_fds[i] >= 0 && read_fds[i] >= 0);
                if (opened) {
                    rt[i].fd = write_fds[i];
                    rt[i].timer = 0;
                    rt[i].input_report_mode = RID_INPUT_STANDARD;
                    rt[i].full_report_enabled = false;
                    reset_nfc_runtime(rt[i], true);
                    rt[i].usb_seen_mac = false;
                    rt[i].usb_handshake_done = false;
                    rt[i].usb_baudrate_set = false;
                    rt[i].usb_timeout_disabled = false;
                    rt[i].pending_subcmd_reply = false;
                    rt[i].last_standard_report_us = 0;
                    rt[i].last_idle_neutral_us = 0;
                    rt[i].neutral_burst_until_us = 0;
                    memset(&rt[i].pending_reply, 0, sizeof(rt[i].pending_reply));
                } else {
                    all_open = false;
                }
            }
        }

        if (!all_open) {
            clear_switch2_usb_activity();
            close_all_fds();
            run_gadget_setup_if_needed(false, g_ctx.legacy_mode
                ? "requested /dev/hidg* nodes could not all be opened"
                : "requested FunctionFS endpoints could not all be opened");
            for (int wait_i = 0; wait_i < 50 && !stoken.stop_requested(); ++wait_i) std::this_thread::sleep_for(ms(10));
            continue;
        }

        if (g_ctx.verbose) {
            if (g_ctx.legacy_mode) std::println("{}x legacy /dev/hidg* opened", nports);
            else std::println("{}x Pro FunctionFS endpoints opened", nports);
        }
        auto next = Clock::now() + tick;
        bool error_shown = false;
        bool timeout_printed[MAX_CLIENTS] = {};
        HoriHIDReport prev[HID_PORT_COUNT];
        for (int i = 0; i < HID_PORT_COUNT; ++i) prev[i].buttons = 0xFFFF;
        bool pairing_screen_open[HID_PORT_COUNT] = {}; // edge-trigger for grip/order auto-pair
        uint64_t last_switch_sleep_poll_us = 0;

        while (!stoken.stop_requested()) {
            std::this_thread::sleep_until(next);
            auto now = Clock::now();
            next = std::max(next + tick, now + tick);

            uint64_t now_stamp = now_us();
            if (last_switch_sleep_poll_us == 0 || elapsed_us_saturated(now_stamp, last_switch_sleep_poll_us) >= 100'000ULL) {
                poll_switch2_sleep_state(now_stamp);
                last_switch_sleep_poll_us = now_stamp;
            }

            // FunctionFS advertises the NFC-capable report descriptor from boot,
            // so staging/removing an amiibo no longer rebuilds the gadget. The
            // only remaining modern re-enumeration edge is controller type, which
            // the console reads once during the USB handshake.
            if (!g_ctx.legacy_mode) {
                const bool want_nfc = server_amiibo_any_armed(now_stamp);
                const bool nfc_edge = (want_nfc != g_ctx.nfc_gadget_active.load(std::memory_order_relaxed));
                if (nfc_edge) {
                    g_ctx.nfc_gadget_active.store(want_nfc, std::memory_order_relaxed);
                    if (g_ctx.verbose)
                        std::println("[amiibo] {} NFC session; FunctionFS keeps USB enumerated",
                                     want_nfc ? "arming" : "idling");
                }

                bool type_edge = false;
                for (int h = 0; h < nports; ++h)
                    if (controller_type_for_port(h) != enumerated_type[h]) type_edge = true;

                if (type_edge) {
                    if (g_ctx.verbose)
                        std::println("[gadget] controller type changed; re-enumerating USB gadget");
                    close_all_fds();
                    run_gadget_setup_if_needed(true, "controller type changed");
                    for (int h = 0; h < HID_PORT_COUNT; ++h) enumerated_type[h] = controller_type_for_port(h);
                    break; // reopen the freshly re-enumerated USB endpoints
                }
            }
            ClientSession snap[MAX_CLIENTS];
            bool stale[MAX_CLIENTS] = {};

            for (int c = 0; c < MAX_CLIENTS; ++c) {
                std::lock_guard<std::mutex> lk(g_ctx.mtx[c]);
                repair_future_client_timestamp(g_ctx.clients[c], now_stamp);
                const uint64_t client_idle_us = elapsed_us_saturated(now_stamp, g_ctx.clients[c].last_rx_us);
                if (g_ctx.clients[c].active && g_ctx.clients[c].last_rx_us != 0 && client_idle_us > CLIENT_TIMEOUT_US &&
                    !server_macro_running(c,0) && !server_macro_running(c,1) && !server_macro_running(c,2) && !server_macro_running(c,3)) {
                    g_ctx.clients[c].active = false;
                    g_ctx.clients[c].source = InputSource::None;
                    g_ctx.clients[c].report.reset();
                    clear_all_motion(g_ctx.clients[c]);
                    g_ctx.clients[c].uses_pad_presence = false;
                    for (int s = 0; s < 4; ++s) {
                        g_ctx.clients[c].pad_present[s] = false;
                        g_ctx.clients[c].pad_last_present_us[s] = 0;
                    }
                    if (!timeout_printed[c]) {
                        std::println("UDP client released from Slot {} (timeout)", c + 1);
                        timeout_printed[c] = true;
                    }
                } else if (g_ctx.clients[c].active) {
                    timeout_printed[c] = false;
                }
                snap[c] = g_ctx.clients[c];
                stale[c] = snap[c].active && snap[c].last_rx_us != 0 && client_idle_us > CLIENT_STALE_NEUTRAL_US;
                if (stale[c]) {
                    snap[c].report.reset();
                }
            }

            for (int h = 0; h < nports; ++h) {
                if (hw_slots[h].client_idx == -1) continue;
                int cidx = hw_slots[h].client_idx, sidx = hw_slots[h].sub_idx;
                bool absent_too_long = false;
                if (snap[cidx].uses_pad_presence && !snap[cidx].pad_present[sidx]) {
                    uint64_t last_seen = snap[cidx].pad_last_present_us[sidx];
                    absent_too_long = (last_seen == 0) || (now_stamp - last_seen >= WEB_PAD_ABSENT_RELEASE_US);
                }
                if ((!snap[cidx].active || absent_too_long) && !server_macro_running(cidx, sidx)) {
                    hw_slots[h].client_idx = hw_slots[h].sub_idx = -1;
                    if (!g_ctx.legacy_mode) {
                        rt[h].neutral_burst_until_us = now_stamp + PRO_RELEASE_NEUTRAL_US;
                        drain_hid_output_queue(read_fds[h]);
                    }
                }
            }

            for (int c = 0; c < MAX_CLIENTS; ++c) {
                if (!snap[c].active) continue;
                for (int s = 0; s < 4; ++s) {
                    bool mapped = false;
                    for (int h = 0; h < nports; ++h) {
                        if (hw_slots[h].client_idx == c && hw_slots[h].sub_idx == s) { mapped = true; break; }
                    }
                    if (mapped) continue;
                    bool macro_active_for_pad = server_macro_running(c, s);
                    if (snap[c].uses_pad_presence) {
                        if (!snap[c].pad_present[s] && !macro_active_for_pad) continue;
                    } else if (g_ctx.legacy_mode) {
                        if (input_is_neutral(get_hid_report(snap[c], s).input) && !macro_active_for_pad) continue;
                    } else {
                        if (hid_is_neutral(get_hid_report(snap[c], s)) && !macro_active_for_pad) continue;
                    }

                    int chosen = -1;
                    if (s >= 0 && s < nports && hw_slots[s].client_idx == -1) {
                        chosen = s;
                    } else {
                        for (int h = 0; h < nports; ++h) {
                            if (hw_slots[h].client_idx == -1) { chosen = h; break; }
                        }
                    }

                    if (chosen != -1) {
                        hw_slots[chosen].client_idx = c; hw_slots[chosen].sub_idx = s;
                        if (g_ctx.verbose) std::println("Map -> PC {} (Pad {}) took console Port {}", c + 1, s + 1, chosen + 1);
                        publish_controller_status_event(c, s, g_ctx.console_player_leds[chosen].load(std::memory_order_relaxed), VIRTUAL_BODY_RGB[chosen]);
                    }
                }
            }

            HIDReport out_reports[HID_PORT_COUNT];
            for (int h = 0; h < nports; ++h) {
                if (hw_slots[h].client_idx != -1) {
                    out_reports[h] = get_hid_report(snap[hw_slots[h].client_idx], hw_slots[h].sub_idx);
                    server_macro_apply(hw_slots[h].client_idx, hw_slots[h].sub_idx, out_reports[h].input);
                    uint8_t type = out_reports[h].reserved[2];
                    if (type != NS_TYPE_JOYCON_L && type != NS_TYPE_JOYCON_R && type != NS_TYPE_PRO)
                        type = NS_TYPE_PRO; // default / older clients
                    set_controller_type_for_port(h, type);
                    apply_controller_type_input(type, out_reports[h]);
                }
            }

            bool ok = true;
            if (g_ctx.legacy_mode) {
                for (int h = 0; h < nports; ++h) {
                    HoriHIDReport r = out_reports[h].input;
                    r.vendor = 0;
                    if (r == prev[h]) continue;
                    ssize_t w = write(write_fds[h], &r, sizeof(HoriHIDReport));
                    if (w < 0) {
                        if (errno != EAGAIN && errno != EWOULDBLOCK) ok = false;
                    } else if (w == (ssize_t)sizeof(HoriHIDReport)) {
                        prev[h] = r; ++g_ctx.hid_writes;
                        // A successful write to /dev/hidg* does not prove the Switch USB host is awake.
                        // When the backend starts while the Switch is already suspended, writes can still
                        // succeed briefly before the gadget reports a disconnect. Treat only host-originated
                        // output/handshake reads as proof of an active host, otherwise we would disconnect
                        // freshly connected BT/UDP/WebSocket clients before they can wake the console.
                    } else if (w > 0) ok = false;
                }
            } else {
                for (int h = 0; h < nports; ++h) {
                    const bool port_needed = (hw_slots[h].client_idx != -1);
                    uint8_t write_buf[HIDG_MAX_REPORT_SIZE] = {};
                    size_t write_len = PRO_REPORT_SIZE;
                    bool have_report_to_write = false, wrote_subcmd_reply = false;

                    if (rt[h].pending_subcmd_reply) {
                        rt[h].pending_reply.id = RID_INPUT_SUBCMD;
                        rt[h].pending_reply.timer = pro_timer_from_us(now_stamp);
                        if (port_needed) apply_input_controls_to_pro21(out_reports[h], rt[h].pending_reply);
                        else fill_neutral_controls(rt[h].pending_reply);
                        memcpy(write_buf, &rt[h].pending_reply, sizeof(ProInputReport21));
                        write_len = sizeof(ProInputReport21);
                        have_report_to_write = wrote_subcmd_reply = true;
                    } else if (rt[h].full_report_enabled) {
                        bool release_burst = rt[h].neutral_burst_until_us != 0 && now_stamp < rt[h].neutral_burst_until_us;
                        if (rt[h].neutral_burst_until_us != 0 && now_stamp >= rt[h].neutral_burst_until_us) rt[h].neutral_burst_until_us = 0;

                        bool idle_due = (rt[h].last_idle_neutral_us == 0) || (elapsed_us_saturated(now_stamp, rt[h].last_idle_neutral_us) >= PRO_IDLE_REPORT_INTERVAL_US);
                        bool standard_due = (rt[h].last_standard_report_us == 0) || (elapsed_us_saturated(now_stamp, rt[h].last_standard_report_us) >= PRO_REPORT_INTERVAL_US);

                        if ((port_needed || release_burst) ? standard_due : idle_due) {
                            HIDReport report_for_port;
                            if (port_needed) report_for_port = out_reports[h];
                            const MotionReport* motion_for_port = nullptr;
                            bool has_motion_for_port = false;
                            if (port_needed) {
                                int cidx = hw_slots[h].client_idx, sidx = hw_slots[h].sub_idx;
                                motion_for_port = get_hid_report(snap[cidx], sidx).motion;
                                has_motion_for_port = get_hid_report(snap[cidx], sidx).has_motion != 0;
                            }
                            if (rt[h].input_report_mode == RID_INPUT_NFC_IR
                                    && usb_transport_supports_nfc_reports()) {
                                ProInputReport31 nfc_in{};
                                build_nfc_ir_report(rt[h], report_for_port, motion_for_port, has_motion_for_port, rt[h].imu_enabled, pro_timer_from_us(now_stamp), nfc_in);
                                memcpy(write_buf, &nfc_in, sizeof(ProInputReport31));
                                write_len = sizeof(ProInputReport31);
                            } else {
                                ProInputReport30 std_in{};
                                build_standard_report(report_for_port, motion_for_port, has_motion_for_port, rt[h].imu_enabled, pro_timer_from_us(now_stamp), std_in);
                                memcpy(write_buf, &std_in, sizeof(ProInputReport30));
                                write_len = sizeof(ProInputReport30);
                            }
                            have_report_to_write = true;

                            if (port_needed || release_burst) rt[h].last_standard_report_us = now_stamp;
                            else rt[h].last_idle_neutral_us = now_stamp;
                        }
                    }

                    if (have_report_to_write) {
                        apply_controller_type_report(controller_type_for_port(h), write_buf);
                        ssize_t w = write(write_fds[h], write_buf, write_len);
                        if (w < 0) {
                            if (errno != EAGAIN && errno != EWOULDBLOCK) ok = false;
                        } else if (w == (ssize_t)write_len) {
                            if (wrote_subcmd_reply) rt[h].pending_subcmd_reply = false;
                            ++g_ctx.hid_writes;
                            // A report/subcommand-reply write alone is not reliable host-presence evidence.
                            // Actual Switch activity is recorded when we read host output below.
                        } else if (w > 0) ok = false;
                    }
                }

                auto process_host_output_report = [&](int h, const uint8_t* read_buf, size_t r) {
                    if (r < 2 || (r == 2 && read_buf[0] == 0 && read_buf[1] == 0)) return;

                    mark_switch2_usb_activity(now_stamp);
                    uint8_t id = read_buf[0];
                    if (id == RID_OUTPUT_CMD) {
                        if (r <= 10) return;
                        if (hw_slots[h].client_idx != -1) publish_rumble_event(hw_slots[h].client_idx, hw_slots[h].sub_idx, read_buf, r, false);
                        const uint8_t subcmd = read_buf[10];
                        std::span<const uint8_t> cmd_data(read_buf + 11, r > 11 ? std::min<size_t>(53, r - 11) : 0);
                        if ((subcmd == CMD_SET_PLAYER_LIGHTS || subcmd == 0x33) && !cmd_data.empty()) {
                            const uint8_t player_leds = cmd_data[0];
                            g_ctx.console_player_leds[h].store(player_leds, std::memory_order_relaxed);
                            // The console flashes the player LEDs on its controller-pairing
                            // (Change Grip/Order) screen. Edge-trigger a BT pairing window so a
                            // real controller can be added exactly when the user expects it.
                            const bool pairing_screen = player_leds_indicate_pairing(player_leds);
                            if (!g_ctx.bluetooth_input_disabled
                                    && pairing_screen && !pairing_screen_open[h]) {
                                if (g_ctx.verbose)
                                    std::println("[bt] Switch controller-pairing screen detected (LED {:#04x}); opening pairing window", static_cast<unsigned>(player_leds));
                                bluetooth_manager_request_pairing_window();
                            }
                            pairing_screen_open[h] = pairing_screen;
                            if (hw_slots[h].client_idx != -1) {
                                publish_controller_status_event(hw_slots[h].client_idx, hw_slots[h].sub_idx, player_leds, VIRTUAL_BODY_RGB[h]);
                            }
                        }
                        memset(&rt[h].pending_reply, 0, sizeof(rt[h].pending_reply));
                        int reply_len = handle_subcommand(rt[h], subcmd, cmd_data, &rt[h].pending_reply);
                        rt[h].pending_subcmd_reply = (reply_len >= 0);
                    } else if (id == RID_OUTPUT_RUMBLE) {
                        if (hw_slots[h].client_idx != -1) publish_rumble_event(hw_slots[h].client_idx, hw_slots[h].sub_idx, read_buf, r, true);
                    } else if (id == RID_OUTPUT_NFC_IR) {
                        int cidx = hw_slots[h].client_idx;
                        int sidx = hw_slots[h].sub_idx;
                        uint32_t amiibo_version = 0;
                        const bool amiibo_armed = (cidx >= 0 && sidx >= 0)
                            ? server_amiibo_is_armed(cidx, sidx, now_stamp, &amiibo_version)
                            : false;
                        if (cidx != -1) publish_rumble_event(cidx, sidx, read_buf, r, false);
                        handle_nfc_ir_output_report(rt[h], std::span<const uint8_t>(read_buf, r),
                                                    cidx, sidx, amiibo_armed, amiibo_version);
                    } else if (id == 0x80) {
                        uint8_t resp_81[PRO_REPORT_SIZE] = {};
                        build_usb_81_response(resp_81, read_buf[1], h);
                        switch (read_buf[1]) {
                            case 0x01: rt[h].usb_seen_mac = true; break;
                            case 0x02: rt[h].usb_handshake_done = true; break;
                            case 0x03: rt[h].usb_baudrate_set = true; break;
                            case 0x04: rt[h].usb_timeout_disabled = true; break;
                            case 0x05: rt[h].usb_timeout_disabled = false; break;
                        }
                        if (write(write_fds[h], resp_81, PRO_REPORT_SIZE) == (ssize_t)PRO_REPORT_SIZE) mark_switch2_usb_activity(now_stamp);
                        else ok = false;
                    }
                };

                for (int h = 0; h < nports; ++h) {
                    // FunctionFS can deliver HID SET_REPORT on ep0 rather than
                    // the interrupt OUT endpoint. Feed those reports into the
                    // exact same Pro Controller command path.
                    std::vector<unsigned char> ctrl_report;
                    for (int control_reads = 0; control_reads < 8 && functionfs_poll_control_report(h, ctrl_report); ++control_reads) {
                        process_host_output_report(h, ctrl_report.data(), ctrl_report.size());
                    }

                    for (int output_reads = 0; output_reads < 8; ++output_reads) {
                        struct pollfd pfd = {read_fds[h], POLLIN, 0};
                        uint8_t read_buf[HIDG_MAX_REPORT_SIZE];
                        if (poll(&pfd, 1, 0) <= 0 || !(pfd.revents & POLLIN)) break;
                        ssize_t r = read(read_fds[h], read_buf, HIDG_MAX_REPORT_SIZE);
                        if (r <= 0) continue;
                        process_host_output_report(h, read_buf, static_cast<size_t>(r));
                    }
                }
            }

            if (!ok) {
                if (!error_shown && g_ctx.verbose) { std::println("Host USB transport disconnected; waiting for reconnect..."); error_shown = true; }
                mark_switch2_usb_host_disconnected();
                close_all_fds();
                for (int wait_i = 0; wait_i < 100 && !stoken.stop_requested(); ++wait_i) std::this_thread::sleep_for(ms(10));
                break;
            }
        }
    }

    if (g_ctx.legacy_mode) {
        HoriHIDReport neutral{}; neutral.reset();
        for (int i = 0; i < HID_PORT_COUNT; ++i) {
            if (write_fds[i] >= 0) {
                ssize_t unused = write(write_fds[i], &neutral, sizeof(HoriHIDReport)); (void)unused;
            }
        }
    }
    close_all_fds();
}

std::mutex g_rate_mtx;

void stats_thread(std::stop_token stoken) {
    uint64_t last_cleanup = 0;
    while (!stoken.stop_requested()) {
        for (int wait_i = 0; wait_i < 500 && !stoken.stop_requested(); ++wait_i) std::this_thread::sleep_for(ms(10));
        if (stoken.stop_requested()) break;

        uint64_t now = now_us();
        if (now - last_cleanup > 60000000) {
            last_cleanup = now;
            std::lock_guard<std::mutex> lk(g_rate_mtx);
            for (int i = 0; i < RATE_TABLE; ++i) {
                if (g_ctx.rate_table[i].ip != 0 && now - g_ctx.rate_table[i].window_start > RATE_WINDOW_US * 2) g_ctx.rate_table[i].ip = 0;
            }
        }
        poll_switch2_sleep_state(now);
        if (g_ctx.verbose) {
            std::println("pkts_rx={:<8}  hid_writes={:<8}", (unsigned long long)g_ctx.pkts_rx.load(), (unsigned long long)g_ctx.hid_writes.load());
        }
    }
}

bool rate_allow(uint32_t ip) {
    std::lock_guard<std::mutex> lk(g_rate_mtx);
    uint64_t now = now_us();
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
