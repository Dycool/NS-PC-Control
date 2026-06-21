#include "writers.hpp"
#include "app_state.hpp"
#include "gadget_wakeup.hpp"
#include "virtual_controller.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <poll.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <thread>

using namespace ns;

// ── Smart Multiplexer HID Writer Thread ───────────────────────────────────────
void legacy_writer_thread(int hz) {
    const auto tick = us(1'000'000 / hz);
    int fds[HID_PORT_COUNT] = {-1, -1, -1, -1};
    std::string devs[HID_PORT_COUNT] = {"/dev/hidg0", "/dev/hidg1", "/dev/hidg2", "/dev/hidg3"};
    bool was_connected = false;

    struct HwSlot { int client_idx = -1; int sub_idx = -1; };
    HwSlot hw_slots[HID_PORT_COUNT];

    while (g_running.load(std::memory_order_relaxed)) {
        bool all_open = true;
        for (int i = 0; i < HID_PORT_COUNT; ++i) {
            if (fds[i] < 0) {
                fds[i] = open(devs[i].c_str(), O_WRONLY | O_NONBLOCK);
                if (fds[i] < 0) all_open = false;
            }
        }

        if (!all_open) {
            clear_switch2_usb_activity();
            for (int i = 0; i < HID_PORT_COUNT; ++i) {
                if (fds[i] >= 0) { close(fds[i]); fds[i] = -1; }
            }
            run_gadget_setup_if_needed(false, "requested legacy /dev/hidg* nodes could not all be opened");
            for (int wait_i = 0; wait_i < 50 && g_running.load(std::memory_order_relaxed); ++wait_i) std::this_thread::sleep_for(ms(10));
            continue;
        }

        if (g_verbose || !was_connected)
            std::printf("%dx legacy /dev/hidg* opened\n", HID_PORT_COUNT);
        // Opening /dev/hidg* only proves the gadget exists, not that the Switch is awake.
        // Mark the host connected only after real USB writes succeed.
        was_connected = true;

        auto next = Clock::now() + tick;
        HIDReport prev[HID_PORT_COUNT];
        for (int i = 0; i < HID_PORT_COUNT; ++i) prev[i].buttons = 0xFFFF;
        bool error_shown = false;
        bool timeout_printed[MAX_CLIENTS] = {};

        while (g_running.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_until(next);
            auto now = Clock::now();
            next = std::max(next + tick, now + tick);

            uint64_t now_stamp = now_us();
            bool active_snap[MAX_CLIENTS] = {};
            bool uses_presence_snap[MAX_CLIENTS] = {};
            bool present_snap[MAX_CLIENTS][4] = {};
            uint64_t last_present_snap[MAX_CLIENTS][4] = {};
            ExtendedHIDReport report_snap[MAX_CLIENTS][4];
            for (int c = 0; c < MAX_CLIENTS; ++c)
                for (int s = 0; s < 4; ++s)
                    report_snap[c][s].reset();

            for (int c = 0; c < MAX_CLIENTS; ++c) {
                std::lock_guard<std::mutex> lk(g_mtx[c]);
                repair_future_client_timestamp(g_clients[c], now_stamp);
                const uint64_t client_idle_us = elapsed_us_saturated(now_stamp, g_clients[c].last_rx_us);
                if (g_clients[c].active && g_clients[c].last_rx_us != 0 && client_idle_us > CLIENT_TIMEOUT_US && !server_macro_running(c,0) && !server_macro_running(c,1) && !server_macro_running(c,2) && !server_macro_running(c,3)) {
                    g_clients[c].active = false;
                    g_clients[c].report.reset();
                    clear_all_motion(g_clients[c]);
                    g_clients[c].uses_pad_presence = false;
                    for (int s = 0; s < 4; ++s) {
                        g_clients[c].pad_present[s] = false;
                        g_clients[c].pad_last_present_us[s] = 0;
                    }
                    if (g_verbose && !timeout_printed[c]) {
                        std::printf("PC %d timed out after %.1fs without UDP input and was disconnected.\n",
                                    c + 1, (double)client_idle_us / 1000000.0);
                        timeout_printed[c] = true;
                    }
                } else if (g_clients[c].active) {
                    timeout_printed[c] = false;
                }

                active_snap[c] = g_clients[c].active;
                uses_presence_snap[c] = g_clients[c].uses_pad_presence;
                const bool input_stream_stale =
                    g_clients[c].active &&
                    g_clients[c].last_rx_us != 0 &&
                    client_idle_us > CLIENT_STALE_NEUTRAL_US;

                for (int s = 0; s < 4; ++s) {
                    present_snap[c][s] = g_clients[c].pad_present[s];
                    last_present_snap[c][s] = g_clients[c].pad_last_present_us[s];
                }

                if (!input_stream_stale) {
                    report_snap[c][0] = g_clients[c].report.p1;
                    report_snap[c][1] = g_clients[c].report.p2;
                    report_snap[c][2] = g_clients[c].report.p3;
                    report_snap[c][3] = g_clients[c].report.p4;
                }
            }

            for (int h = 0; h < HID_PORT_COUNT; ++h) {
                if (hw_slots[h].client_idx == -1) continue;

                int cidx = hw_slots[h].client_idx;
                int sidx = hw_slots[h].sub_idx;
                bool absent_too_long = false;
                if (uses_presence_snap[cidx] && !present_snap[cidx][sidx]) {
                    uint64_t last_seen = last_present_snap[cidx][sidx];
                    absent_too_long = (last_seen == 0) ||
                                      (now_stamp - last_seen >= WEB_PAD_ABSENT_RELEASE_US);
                }

                if ((!active_snap[cidx] || absent_too_long) && !server_macro_running(cidx, sidx)) {
                    hw_slots[h].client_idx = -1;
                    hw_slots[h].sub_idx = -1;
                }
            }

            for (int c = 0; c < MAX_CLIENTS; ++c) {
                if (!active_snap[c]) continue;
                for (int s = 0; s < 4; ++s) {
                    bool mapped = false;
                    for (int h = 0; h < HID_PORT_COUNT; ++h) {
                        if (hw_slots[h].client_idx == c && hw_slots[h].sub_idx == s) {
                            mapped = true;
                            break;
                        }
                    }
                    if (mapped) continue;

                    bool macro_active_for_pad = server_macro_running(c, s);
                    if (uses_presence_snap[c]) {
                        if (!present_snap[c][s] && !macro_active_for_pad) continue;
                    } else if (input_is_neutral(report_snap[c][s].input) && !macro_active_for_pad) {
                        continue;
                    }

                    int chosen = -1;
                    if (s >= 0 && s < HID_PORT_COUNT && hw_slots[s].client_idx == -1) {
                        chosen = s;
                    } else {
                        for (int h = 0; h < HID_PORT_COUNT; ++h) {
                            if (hw_slots[h].client_idx == -1) {
                                chosen = h;
                                break;
                            }
                        }
                    }

                    if (chosen != -1) {
                        hw_slots[chosen].client_idx = c;
                        hw_slots[chosen].sub_idx = s;
                        if (g_verbose)
                            std::printf("Map -> PC %d (Pad %d) took console legacy Port %d\n", c + 1, s + 1, chosen + 1);
                    }
                }
            }

            HIDReport out_reports[HID_PORT_COUNT];
            for (int h = 0; h < HID_PORT_COUNT; ++h) out_reports[h].reset();
            for (int h = 0; h < HID_PORT_COUNT; ++h) {
                if (hw_slots[h].client_idx == -1) continue;
                int cidx = hw_slots[h].client_idx;
                int sidx = hw_slots[h].sub_idx;
                out_reports[h] = report_snap[cidx][sidx].input;
                out_reports[h].vendor = 0;
                server_macro_apply(cidx, sidx, out_reports[h]);
                out_reports[h].vendor = 0;
            }

            bool ok = true;
            static_assert(sizeof(HIDReport) == 8, "HIDReport size mismatch with legacy HID gadget descriptor");
            for (int h = 0; h < HID_PORT_COUNT; ++h) {
                if (out_reports[h] == prev[h]) continue;
                ssize_t w = write(fds[h], &out_reports[h], sizeof(HIDReport));
                if (w < 0) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK) ok = false;
                } else if (w == (ssize_t)sizeof(HIDReport)) {
                    prev[h] = out_reports[h];
                    ++g_hid_writes;
                    mark_switch2_usb_activity(now_stamp);
                } else if (w > 0) {
                    ok = false;
                }
            }

            if (!ok) {
                if (!error_shown) { std::puts("Host disconnected - waiting for reconnect..."); error_shown = true; }
                mark_switch2_usb_host_disconnected();
                for (int i = 0; i < HID_PORT_COUNT; ++i) { close(fds[i]); fds[i] = -1; }
                for (int wait_i = 0; wait_i < 100 && g_running.load(std::memory_order_relaxed); ++wait_i) std::this_thread::sleep_for(ms(10));
                break;
            }
        }
    }

    HIDReport neutral{};
    neutral.reset();
    for (int i = 0; i < HID_PORT_COUNT; ++i) {
        if (fds[i] >= 0) {
            ssize_t unused = write(fds[i], &neutral, sizeof(HIDReport));
            (void)unused;
            close(fds[i]);
        }
    }
}

void writer_thread(int hz) {
    if (g_legacy_mode) {
        legacy_writer_thread(hz);
        return;
    }

    for (int i = 0; i < HID_PORT_COUNT; ++i) init_spi_flash(i);

    const auto tick = us(1'000'000 / hz);
    int fds[4] = {-1, -1, -1, -1};
    std::string devs[4] = {"/dev/hidg0", "/dev/hidg1", "/dev/hidg2", "/dev/hidg3"};
    bool was_connected = false;

    struct HwSlot { int client_idx = -1; int sub_idx = -1; };
    HwSlot hw_slots[4];
    ControllerRuntime rt[4];
    for (int i = 0; i < HID_PORT_COUNT; ++i) rt[i].ctrl = i;

    while (g_running.load(std::memory_order_relaxed)) {
        bool all_open = true;
        for (int i = 0; i < HID_PORT_COUNT; ++i) {
            if (fds[i] < 0) {
                fds[i] = open(devs[i].c_str(), O_RDWR | O_NONBLOCK);
                if (fds[i] >= 0) {
                    rt[i].fd = fds[i];
                    rt[i].timer = 0;
                    rt[i].full_report_enabled = false;
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
            // Do not keep a partial set of opened endpoints around while the
            // gadget is being recreated/rebound.  Retry with a clean fd set.
            for (int i = 0; i < HID_PORT_COUNT; ++i) {
                if (fds[i] >= 0) { close(fds[i]); fds[i] = -1; rt[i].fd = -1; }
            }
            run_gadget_setup_if_needed(false, "requested /dev/hidg* nodes could not all be opened");
            for (int wait_i = 0; wait_i < 50 && g_running.load(std::memory_order_relaxed); ++wait_i) std::this_thread::sleep_for(ms(10));
            continue;
        }

        if (g_verbose || !was_connected)
            std::printf("%dx USB gamepad /dev/hidg* opened\n", HID_PORT_COUNT);
        // Opening /dev/hidg* only proves the gadget exists, not that the Switch is awake.
        // Mark the host connected only after real USB output/handshake activity succeeds.
        was_connected = true;

        auto next = Clock::now() + tick;
        bool error_shown = false;
        bool timeout_printed[MAX_CLIENTS] = {};
        uint64_t writes_this_second = 0;
        auto last_rate_log = Clock::now();

        while (g_running.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_until(next);
            auto now = Clock::now();
            next = std::max(next + tick, now + tick);

            uint64_t now_stamp = now_us();
            bool active_snap[MAX_CLIENTS] = {};
            bool uses_presence_snap[MAX_CLIENTS] = {};
            bool present_snap[MAX_CLIENTS][4] = {};
            uint64_t last_present_snap[MAX_CLIENTS][4] = {};
            ExtendedHIDReport report_snap[MAX_CLIENTS][4];
            MotionReport motion_snap[MAX_CLIENTS][4][3];
            bool has_motion_snap[MAX_CLIENTS][4] = {};
            for (int c = 0; c < MAX_CLIENTS; ++c) {
                for (int s = 0; s < 4; ++s) {
                    report_snap[c][s].reset();
                    for (int i = 0; i < 3; ++i) motion_snap[c][s][i].reset();
                }
            }

            for (int c = 0; c < MAX_CLIENTS; ++c) {
                std::lock_guard<std::mutex> lk(g_mtx[c]);
                repair_future_client_timestamp(g_clients[c], now_stamp);
                const uint64_t client_idle_us = elapsed_us_saturated(now_stamp, g_clients[c].last_rx_us);
                if (g_clients[c].active && g_clients[c].last_rx_us != 0 && client_idle_us > CLIENT_TIMEOUT_US && !server_macro_running(c,0) && !server_macro_running(c,1) && !server_macro_running(c,2) && !server_macro_running(c,3)) {
                    g_clients[c].active = false;
                    g_clients[c].report.reset();
                    clear_all_motion(g_clients[c]);
                    g_clients[c].uses_pad_presence = false;
                    for (int s = 0; s < 4; ++s) {
                        g_clients[c].pad_present[s] = false;
                        g_clients[c].pad_last_present_us[s] = 0;
                    }
                    if (g_verbose && !timeout_printed[c]) {
                        std::printf("PC %d timed out after %.1fs without UDP input and was disconnected.\n",
                                    c + 1, (double)elapsed_us_saturated(now_stamp, g_clients[c].last_rx_us) / 1000000.0);
                        timeout_printed[c] = true;
                    }
                } else if (g_clients[c].active) {
                    timeout_printed[c] = false;
                }
                active_snap[c] = g_clients[c].active;
                uses_presence_snap[c] = g_clients[c].uses_pad_presence;
                const bool input_stream_stale =
                    g_clients[c].active &&
                    g_clients[c].last_rx_us != 0 &&
                    client_idle_us > CLIENT_STALE_NEUTRAL_US;

                for (int s = 0; s < 4; ++s) {
                    present_snap[c][s] = g_clients[c].pad_present[s];
                    last_present_snap[c][s] = g_clients[c].pad_last_present_us[s];
                }

                if (input_stream_stale) {
                    // Keep the session/port alive through a short Windows UDP stall,
                    // but release all controls quickly so held R/ZR/sticks do not get
                    // stuck.  A real disconnect is handled later by CLIENT_TIMEOUT_US.
                    report_snap[c][0].reset();
                    report_snap[c][1].reset();
                    report_snap[c][2].reset();
                    report_snap[c][3].reset();
                    for (int s = 0; s < 4; ++s)
                        has_motion_snap[c][s] = false;
                } else {
                    report_snap[c][0] = g_clients[c].report.p1;
                    report_snap[c][1] = g_clients[c].report.p2;
                    report_snap[c][2] = g_clients[c].report.p3;
                    report_snap[c][3] = g_clients[c].report.p4;
                    for (int s = 0; s < 4; ++s) {
                        for (int i = 0; i < 3; ++i)
                            motion_snap[c][s][i] = g_clients[c].motion_samples[s][i];
                        has_motion_snap[c][s] = g_clients[c].has_motion[s];
                    }
                }
            }

            for (int h = 0; h < HID_PORT_COUNT; ++h) {
                if (hw_slots[h].client_idx == -1) continue;

                int cidx = hw_slots[h].client_idx;
                int sidx = hw_slots[h].sub_idx;
                bool absent_too_long = false;
                if (uses_presence_snap[cidx] && !present_snap[cidx][sidx]) {
                    uint64_t last_seen = last_present_snap[cidx][sidx];
                    absent_too_long = (last_seen == 0) ||
                                      (now_stamp - last_seen >= WEB_PAD_ABSENT_RELEASE_US);
                }

                if ((!active_snap[cidx] || absent_too_long) && !server_macro_running(cidx, sidx)) {
                    hw_slots[h].client_idx = -1;
                    hw_slots[h].sub_idx = -1;

                    // The USB interface stays alive so the console can keep talking to
                    // it, but the player assignment is gone.  Send a short neutral
                    // release burst to clear any held buttons, drain stale output, then
                    // fall back to the low-rate idle heartbeat.
                    rt[h].neutral_burst_until_us = now_stamp + PRO_RELEASE_NEUTRAL_US;
                    drain_hid_output_queue(fds[h]);
                }
            }

            for (int c = 0; c < MAX_CLIENTS; ++c) {
                if (!active_snap[c]) continue;
                for (int s = 0; s < 4; ++s) {
                    bool mapped = false;
                    for (int h = 0; h < HID_PORT_COUNT; ++h) {
                        if (hw_slots[h].client_idx == c && hw_slots[h].sub_idx == s) {
                            mapped = true;
                            break;
                        }
                    }

                    // A browser/mobile pad that is connected but currently neutral still
                    // needs to claim its console port so rumble has a target immediately.
                    if (mapped) continue;
                    bool macro_active_for_pad = server_macro_running(c, s);
                    if (uses_presence_snap[c]) {
                        if (!present_snap[c][s] && !macro_active_for_pad) continue;
                    } else if (extended_is_neutral(report_snap[c][s]) && !macro_active_for_pad) {
                        continue;
                    }

                    // Preserve logical pad order.  The previous "first free port" mapper
                    // let Pad 2 steal console port 1 whenever keyboard/mobile Pad 1 was
                    // neutral, which made keyboard mode and mobile mode look broken.
                    int chosen = -1;
                    if (s >= 0 && s < HID_PORT_COUNT && hw_slots[s].client_idx == -1) {
                        chosen = s;
                    } else {
                        // Fallback only for multi-client cases where the preferred port
                        // is already occupied.
                        for (int h = 0; h < HID_PORT_COUNT; ++h) {
                            if (hw_slots[h].client_idx == -1) {
                                chosen = h;
                                break;
                            }
                        }
                    }

                    if (chosen != -1) {
                        hw_slots[chosen].client_idx = c;
                        hw_slots[chosen].sub_idx = s;
                        if (g_verbose)
                            std::printf("Map -> PC %d (Pad %d) took console Pro Port %d\n", c + 1, s + 1, chosen + 1);
                    }
                }
            }

            ExtendedHIDReport out_reports[4];
            for (int h = 0; h < HID_PORT_COUNT; ++h) out_reports[h].reset();
            for (int h = 0; h < HID_PORT_COUNT; ++h) {
                if (hw_slots[h].client_idx != -1) {
                    out_reports[h] = report_snap[hw_slots[h].client_idx][hw_slots[h].sub_idx];
                    server_macro_apply(hw_slots[h].client_idx, hw_slots[h].sub_idx, out_reports[h].input);
                }
            }

            bool ok = true;
            for (int h = 0; h < HID_PORT_COUNT; ++h) {
                const bool port_needed = (hw_slots[h].client_idx != -1);

                uint8_t write_buf[PRO_REPORT_SIZE] = {};
                bool have_report_to_write = false;
                bool wrote_subcmd_reply = false;

                if (rt[h].pending_subcmd_reply) {
                    rt[h].pending_reply.id = RID_INPUT_SUBCMD;
                    rt[h].pending_reply.timer = pro_timer_from_us(now_stamp);
                    if (port_needed)
                        apply_input_controls_to_pro21(out_reports[h], rt[h].pending_reply);
                    else
                        fill_neutral_controls(rt[h].pending_reply);
                    memcpy(write_buf, &rt[h].pending_reply, sizeof(ProInputReport21));
                    have_report_to_write = true;
                    wrote_subcmd_reply = true;
                } else if (rt[h].full_report_enabled) {
                    // Active/player-assigned ports run at the shared boring
                    // 250Hz / 4ms cadence used by UDP clients and servers.
                    // Unassigned ports still send neutral keepalive reports,
                    // but only at a low heartbeat rate so we do not spam neutral data.
                    bool release_burst = rt[h].neutral_burst_until_us != 0 &&
                                         now_stamp < rt[h].neutral_burst_until_us;
                    if (rt[h].neutral_burst_until_us != 0 && now_stamp >= rt[h].neutral_burst_until_us)
                        rt[h].neutral_burst_until_us = 0;

                    bool idle_due = (rt[h].last_idle_neutral_us == 0) ||
                                    (elapsed_us_saturated(now_stamp, rt[h].last_idle_neutral_us) >= PRO_IDLE_REPORT_INTERVAL_US);
                    bool standard_due = (rt[h].last_standard_report_us == 0) ||
                                        (elapsed_us_saturated(now_stamp, rt[h].last_standard_report_us) >= PRO_REPORT_INTERVAL_US);

                    // Standard 0x30 reports use the same 250Hz cadence as the
                    // UDP input stream so reconnects and backend switching do
                    // not change timing behavior.
                    bool should_write_standard = false;
                    if (port_needed || release_burst) should_write_standard = standard_due;
                    else should_write_standard = idle_due;

                    if (should_write_standard) {
                        ExtendedHIDReport report_for_port;
                        report_for_port.reset();
                        if (port_needed) report_for_port = out_reports[h];

                        const MotionReport* motion_for_port = nullptr;
                        bool has_motion_for_port = false;
                        if (port_needed) {
                            int cidx = hw_slots[h].client_idx;
                            int sidx = hw_slots[h].sub_idx;
                            motion_for_port = motion_snap[cidx][sidx];
                            has_motion_for_port = has_motion_snap[cidx][sidx];
                        }

                        ProInputReport30 std_in{};
                        build_standard_report(report_for_port,
                                              motion_for_port,
                                              has_motion_for_port,
                                              rt[h].imu_enabled,
                                              pro_timer_from_us(now_stamp),
                                              std_in);
                        memcpy(write_buf, &std_in, sizeof(ProInputReport30));
                        have_report_to_write = true;

                        if (port_needed || release_burst)
                            rt[h].last_standard_report_us = now_stamp;
                        if (!port_needed && !release_burst)
                            rt[h].last_idle_neutral_us = now_stamp;
                    }
                }

                if (!have_report_to_write) continue;

                ssize_t w = write(fds[h], write_buf, PRO_REPORT_SIZE);
                if (w < 0) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK) ok = false;
                    // If a subcommand reply could not be written, keep it pending.
                    // Dropping it makes the console repeat commands such as 0x02 forever.
                } else if (w == (ssize_t)PRO_REPORT_SIZE) {
                    if (wrote_subcmd_reply) rt[h].pending_subcmd_reply = false;
                    writes_this_second++;
                    mark_switch2_usb_activity(now_stamp);
                } else if (w > 0) {
                    // Partial HID report writes should not happen.  Treat as an error so
                    // we reconnect cleanly rather than sending malformed controller data.
                    ok = false;
                }
            }

            for (int h = 0; h < HID_PORT_COUNT; ++h) {
                // Always serve the HID control/output side for every exposed Pro
                // Controller interface.  HID gadgets are not lazily created: once
                // setup_gadget.sh exposes hidg0..hidg3, the the host may send init
                // commands to any of them.  Ignoring those commands until a pad maps
                // to the port breaks keyboard/mobile/web input and leaves stale output
                // reports queued in /dev/hidgX.
                for (int output_reads = 0; output_reads < 8; ++output_reads) {
                    struct pollfd pfd = {fds[h], POLLIN, 0};
                    uint8_t read_buf[PRO_REPORT_SIZE];
                    if (poll(&pfd, 1, 0) <= 0 || !(pfd.revents & POLLIN))
                        break;

                    ssize_t r = read(fds[h], read_buf, PRO_REPORT_SIZE);
                    if (r <= 0) continue;

                    // USB gadget reads may occasionally return 2-byte 00 00
                    // frames from /dev/hidg0. They are not valid controller output
                    // reports and forwarding them to a real USB gamepad caused
                    // WriteFile(ERROR_INVALID_PARAMETER).  Ignore them here too.
                    if (r < 2 || (r == 2 && read_buf[0] == 0x00 && read_buf[1] == 0x00))
                        continue;

                    mark_switch2_usb_activity(now_stamp);
                    uint8_t id = read_buf[0];
                    if (id == RID_OUTPUT_CMD) {
                        if (hw_slots[h].client_idx != -1)
                            publish_rumble_event(hw_slots[h].client_idx, hw_slots[h].sub_idx, read_buf, r, false);

                        uint8_t subcmd_id = read_buf[10];
                        size_t subcmd_data_len = r > 11 ? std::min((size_t)53, (size_t)(r - 11)) : 0;
                        memset(&rt[h].pending_reply, 0, sizeof(rt[h].pending_reply));
                        int reply_len = handle_subcommand(
                            rt[h], subcmd_id,
                            subcmd_data_len > 0 ? read_buf + 11 : nullptr,
                            subcmd_data_len,
                            &rt[h].pending_reply
                        );
                        rt[h].pending_subcmd_reply = (reply_len >= 0);
                        if (g_verbose) {
                            std::printf("[pro%d] subcmd 0x%02X reply=0x%02X 0x%02X",
                                        h + 1, subcmd_id, rt[h].pending_reply.ack, rt[h].pending_reply.subcmd_id);
                            if ((subcmd_id == CMD_SET_DATA_FORMAT || subcmd_id == CMD_ENABLE_IMU ||
                                 subcmd_id == CMD_ENABLE_VIBRATION) &&
                                subcmd_data_len > 0) {
                                std::printf(" data=");
                                for (size_t bi = 0; bi < subcmd_data_len && bi < 8; ++bi)
                                    std::printf("%s%02X", bi ? " " : "", read_buf[11 + bi]);
                            }
                            std::printf("\n");
                        }
                    } else if (id == RID_OUTPUT_RUMBLE) {
                        if (hw_slots[h].client_idx != -1)
                            publish_rumble_event(hw_slots[h].client_idx, hw_slots[h].sub_idx, read_buf, r, true);
                    } else if (id == 0x80) {
                        const uint8_t usb_cmd = read_buf[1];
                        uint8_t resp_81[PRO_REPORT_SIZE] = {};
                        build_usb_81_response(resp_81, usb_cmd, h);

                        switch (usb_cmd) {
                        case 0x01: rt[h].usb_seen_mac = true; break;
                        case 0x02: rt[h].usb_handshake_done = true; break;
                        case 0x03: rt[h].usb_baudrate_set = true; break;
                        case 0x04: rt[h].usb_timeout_disabled = true; break;
                        case 0x05: rt[h].usb_timeout_disabled = false; break;
                        default: break;
                        }

                        ssize_t w = write(fds[h], resp_81, PRO_REPORT_SIZE);
                        if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) ok = false;
                        else if (w == (ssize_t)PRO_REPORT_SIZE) mark_switch2_usb_activity(now_stamp);
                        if (g_verbose) {
                            std::printf("[pro%d] usb 0x80 cmd=0x%02X -> 0x81 subtype=0x%02X mac=%02X:%02X:%02X:%02X:%02X:%02X timeout=%s\n",
                                        h + 1, usb_cmd, resp_81[1],
                                        CTRL_MAC_BE[h][0], CTRL_MAC_BE[h][1], CTRL_MAC_BE[h][2],
                                        CTRL_MAC_BE[h][3], CTRL_MAC_BE[h][4], CTRL_MAC_BE[h][5],
                                        rt[h].usb_timeout_disabled ? "disabled" : "enabled");
                        }
                    } else {
                        if (g_verbose && id != 0x00)
                            std::printf("[pro%d] unknown output report id=0x%02X len=%zd\n", h + 1, id, r);
                    }
                }
            }

            if (!ok) {
                if (!error_shown) { std::puts("Host disconnected — waiting for reconnect..."); error_shown = true; }
                mark_switch2_usb_host_disconnected();
                for (int i = 0; i < 4; ++i) { close(fds[i]); fds[i] = -1; rt[i].fd = -1; }
                for (int wait_i = 0; wait_i < 100 && g_running.load(std::memory_order_relaxed); ++wait_i) std::this_thread::sleep_for(ms(10));
                break;
            }

            auto now_log = Clock::now();
            if (g_verbose && now_log - last_rate_log >= ms(1000)) {
                std::printf("pro_hid_writes/sec=%llu\n", (unsigned long long)writes_this_second);
                writes_this_second = 0;
                last_rate_log = now_log;
            }
            if (writes_this_second) ++g_hid_writes;
        }
    }

    for (int i = 0; i < 4; ++i) {
        if (fds[i] >= 0) close(fds[i]);
    }
}

// ── Per-IP rate limiter ──────────────────────────────────────────────────────
std::mutex g_rate_mtx;

// ── Stats thread ──────────────────────────────────────────────────────────────
void stats_thread() {
    uint64_t last_cleanup = 0;
    while (g_running.load(std::memory_order_relaxed)) {
        for (int wait_i = 0; wait_i < 50 && g_running.load(std::memory_order_relaxed); ++wait_i)
            std::this_thread::sleep_for(ms(100));
        if (!g_running.load(std::memory_order_relaxed)) break;

        // Periodic rate limiter cleanup (every 60s)
        uint64_t now = now_us();
        if (now - last_cleanup > 60000000) {
            last_cleanup = now;
            std::lock_guard<std::mutex> lk(g_rate_mtx);
            for (int i = 0; i < RATE_TABLE; ++i) {
                if (g_rate_table[i].ip != 0 &&
                    now - g_rate_table[i].window_start > RATE_WINDOW_US * 2)
                    g_rate_table[i].ip = 0;
            }
        }

        if (!g_verbose) continue;
        std::printf("pkts_rx=%-8llu  hid_writes=%-8llu\n",
            (unsigned long long)g_pkts_rx.load(),
            (unsigned long long)g_hid_writes.load());
    }
}

// ── Per-IP rate limiter ──────────────────────────────────────────────────────
bool rate_allow(uint32_t ip) {
    std::lock_guard<std::mutex> lk(g_rate_mtx);
    uint64_t now = now_us();
    uint32_t idx = ip % RATE_TABLE;
    RateSlot &s = g_rate_table[idx];
    if (s.ip != ip) {
        s.ip = ip; s.count = 1; s.window_start = now; return true;
    }
    if (now - s.window_start > RATE_WINDOW_US) {
        s.count = 1; s.window_start = now; return true;
    }
    if (s.count < UINT32_MAX) s.count++;
    return s.count <= RATE_MAX_PKT;
}
