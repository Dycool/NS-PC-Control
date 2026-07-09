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


static uint8_t requested_controller_profile_from_report(const HIDReport& report) {
    switch (report.reserved[2]) {
        case ns::CONTROLLER_TYPE_JOYCON_L:
        case ns::CONTROLLER_TYPE_JOYCON_R:
        case ns::CONTROLLER_TYPE_PRO:
        case ns::CONTROLLER_TYPE_JOYCON_PAIR:
        case ns::CONTROLLER_TYPE_HORI:
        case ns::CONTROLLER_TYPE_PRO_S2:
        case ns::CONTROLLER_TYPE_JOYCON_L_S2:
        case ns::CONTROLLER_TYPE_JOYCON_R_S2:
        case ns::CONTROLLER_TYPE_JOYCON_PAIR_S2:
            return report.reserved[2];
        case ns::CONTROLLER_TYPE_DEFAULT:
        default:
            return ns::CONTROLLER_TYPE_PRO;
    }
}

static uint8_t virtual_type_for_profile(uint8_t profile, bool pair_right_side = false) {
    switch (profile) {
        case ns::CONTROLLER_TYPE_JOYCON_L:
        case ns::CONTROLLER_TYPE_JOYCON_L_S2:
            return NS_TYPE_JOYCON_L;
        case ns::CONTROLLER_TYPE_JOYCON_R:
        case ns::CONTROLLER_TYPE_JOYCON_R_S2:
            return NS_TYPE_JOYCON_R;
        case ns::CONTROLLER_TYPE_JOYCON_PAIR:
        case ns::CONTROLLER_TYPE_JOYCON_PAIR_S2:
            return pair_right_side ? NS_TYPE_JOYCON_R : NS_TYPE_JOYCON_L;
        case ns::CONTROLLER_TYPE_HORI:
            return NS_TYPE_HORI;
        case ns::CONTROLLER_TYPE_PRO:
        case ns::CONTROLLER_TYPE_PRO_S2:
        default:
            return NS_TYPE_PRO;
    }
}

static bool profile_is_pair(uint8_t profile) {
    return profile == ns::CONTROLLER_TYPE_JOYCON_PAIR ||
           profile == ns::CONTROLLER_TYPE_JOYCON_PAIR_S2;
}

static const HIDReport& get_hid_report(const ClientSession& c, int s) {
    if (s == 0) return c.report.p1;
    if (s == 1) return c.report.p2;
    if (s == 2) return c.report.p3;
    return c.report.p4;
}

void writer_thread(std::stop_token stoken, int hz) {
    const int nports = HID_PORT_COUNT;
    for (int i = 0; i < nports; ++i) init_spi_flash(i);

    const auto tick = us(1'000'000 / hz);
    struct HwSlot {
        int client_idx = -1;
        int sub_idx = -1;
        uint8_t virtual_type = NS_TYPE_PRO;
        bool pair_member = false;
        bool pair_right = false;
    };
    HwSlot hw_slots[HID_PORT_COUNT];
    ControllerRuntime rt[HID_PORT_COUNT];
    for (int i = 0; i < HID_PORT_COUNT; ++i) rt[i].ctrl = i;

    auto close_all_fds = [&]() {};

    auto reset_port_runtime = [&](int i) {
        rt[i].fd = -1;
        rt[i].timer = 0;
        rt[i].input_report_mode = RID_INPUT_STANDARD;
        rt[i].full_report_enabled = false;
        rt[i].usb_seen_mac = false;
        rt[i].usb_handshake_done = false;
        rt[i].usb_baudrate_set = false;
        rt[i].usb_timeout_disabled = false;
        rt[i].pending_subcmd_reply = false;
        rt[i].pending_cmd_response = false;
        rt[i].cmd_response_len = 0;
        rt[i].last_standard_report_us = 0;
        rt[i].last_idle_neutral_us = 0;
        rt[i].neutral_burst_until_us = 0;
        memset(&rt[i].pending_reply, 0, sizeof(rt[i].pending_reply));
        memset(rt[i].cmd_response_buf, 0, sizeof(rt[i].cmd_response_buf));
    };
    bool ffs_live[HID_PORT_COUNT] = {};

    while (!stoken.stop_requested()) {
        bool all_open = true;
        for (int i = 0; i < nports; ++i) {
            const bool live = functionfs_transport_active() && functionfs_io_ready(i);
            if (!live) {
                ffs_live[i] = false;
                all_open = false;
            } else if (!ffs_live[i]) {
                ffs_live[i] = true;
                reset_port_runtime(i);
            }
        }

        if (!all_open) {
            clear_switch2_usb_activity();
            close_all_fds();
            run_gadget_setup_if_needed(false, "requested FunctionFS endpoints could not all be opened");
            for (int wait_i = 0; wait_i < 50 && !stoken.stop_requested(); ++wait_i) std::this_thread::sleep_for(ms(10));
            continue;
        }

        if (g_ctx.verbose) {
            std::println("{}x Pro FunctionFS endpoints opened", nports);
        }
        auto next = Clock::now() + tick;
        bool error_shown = false;
        bool timeout_printed[MAX_CLIENTS] = {};
        HoriHIDReport prev[HID_PORT_COUNT];
        for (int i = 0; i < HID_PORT_COUNT; ++i) prev[i].buttons = 0xFFFF;
        bool pairing_screen_open[HID_PORT_COUNT] = {}; // edge-trigger for grip/order auto-pair
        uint64_t last_switch_sleep_poll_us = 0;

        auto release_hw_slot = [&](int h, uint64_t stamp) {
            hw_slots[h].client_idx = -1;
            hw_slots[h].sub_idx = -1;
            hw_slots[h].pair_member = false;
            hw_slots[h].pair_right = false;
            rt[h].neutral_burst_until_us = stamp + PRO_RELEASE_NEUTRAL_US;
            functionfs_drain_output(h);
        };

        auto find_free_pair_base = [&](int source_subpad) -> int {
            if (source_subpad >= 0 && source_subpad < 2) {
                const int preferred = source_subpad * 2;
                if (preferred + 1 < nports
                        && hw_slots[preferred].client_idx == -1
                        && hw_slots[preferred + 1].client_idx == -1) {
                    return preferred;
                }
            }
            for (int base = 0; base + 1 < nports; base += 2) {
                if (hw_slots[base].client_idx == -1 && hw_slots[base + 1].client_idx == -1)
                    return base;
            }
            return -1;
        };

        while (!stoken.stop_requested()) {
            std::this_thread::sleep_until(next);
            auto now = Clock::now();
            next = std::max(next + tick, now + tick);

            uint64_t now_stamp = now_us();
            if (last_switch_sleep_poll_us == 0 || elapsed_us_saturated(now_stamp, last_switch_sleep_poll_us) >= 100'000ULL) {
                poll_switch2_sleep_state(now_stamp);
                last_switch_sleep_poll_us = now_stamp;
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
                bool profile_changed = false;
                if (snap[cidx].active && !absent_too_long) {
                    const uint8_t profile = requested_controller_profile_from_report(get_hid_report(snap[cidx], sidx));
                    if (profile_is_pair(profile)) {
                        const uint8_t expected = virtual_type_for_profile(profile, hw_slots[h].pair_right);
                        profile_changed = !hw_slots[h].pair_member || hw_slots[h].virtual_type != expected;
                    } else {
                        const uint8_t expected = virtual_type_for_profile(profile);
                        profile_changed = hw_slots[h].pair_member || hw_slots[h].virtual_type != expected;
                    }
                }
                if (((!snap[cidx].active || absent_too_long) && !server_macro_running(cidx, sidx)) || profile_changed) {
                    release_hw_slot(h, now_stamp);
                }
            }

            for (int c = 0; c < MAX_CLIENTS; ++c) {
                if (!snap[c].active || !snap[c].uses_pad_presence) continue;
                for (int s = 0; s < nports; ++s) {
                    if (!snap[c].pad_present[s]) continue;
                    const uint8_t profile = requested_controller_profile_from_report(get_hid_report(snap[c], s));
                    if (profile_is_pair(profile)) {
                        if (s < 2) {
                            const int base = s * 2;
                            if (base + 1 < nports
                                    && hw_slots[base].client_idx == -1
                                    && hw_slots[base + 1].client_idx == -1) {
                                bool is_s2 = (profile == ns::CONTROLLER_TYPE_JOYCON_PAIR_S2);
                                uint8_t lp = is_s2 ? ns::CONTROLLER_TYPE_JOYCON_L_S2 : ns::CONTROLLER_TYPE_JOYCON_L;
                                uint8_t rp = is_s2 ? ns::CONTROLLER_TYPE_JOYCON_R_S2 : ns::CONTROLLER_TYPE_JOYCON_R;
                                set_controller_type_for_port(base, lp);
                                set_controller_type_for_port(base + 1, rp);
                            }
                        }
                    } else if (s >= 0 && s < nports && hw_slots[s].client_idx == -1) {
                        set_controller_type_for_port(s, profile);
                    }
                }
            }

            for (int c = 0; c < MAX_CLIENTS; ++c) {
                if (!snap[c].active) continue;
                for (int s = 0; s < 4; ++s) {
                    const uint8_t profile = requested_controller_profile_from_report(get_hid_report(snap[c], s));
                    const bool wants_pair = profile_is_pair(profile);

                    int mapped_count = 0;
                    bool has_pair_l = false, has_pair_r = false;
                    for (int h = 0; h < nports; ++h) {
                        if (hw_slots[h].client_idx == c && hw_slots[h].sub_idx == s) {
                            ++mapped_count;
                            if (hw_slots[h].pair_member && !hw_slots[h].pair_right && hw_slots[h].virtual_type == NS_TYPE_JOYCON_L) has_pair_l = true;
                            if (hw_slots[h].pair_member &&  hw_slots[h].pair_right && hw_slots[h].virtual_type == NS_TYPE_JOYCON_R) has_pair_r = true;
                        }
                    }
                    if (wants_pair ? (has_pair_l && has_pair_r) : (mapped_count > 0)) continue;

                    bool macro_active_for_pad = server_macro_running(c, s);
                    if (snap[c].uses_pad_presence) {
                        if (!snap[c].pad_present[s] && !macro_active_for_pad) continue;
                    } else {
                        if (hid_is_neutral(get_hid_report(snap[c], s)) && !macro_active_for_pad) continue;
                    }

                    if (wants_pair) {
                        const int base = find_free_pair_base(s);
                        if (base != -1) {
                            bool is_s2 = (profile == ns::CONTROLLER_TYPE_JOYCON_PAIR_S2);
                            uint8_t lp = is_s2 ? ns::CONTROLLER_TYPE_JOYCON_L_S2 : ns::CONTROLLER_TYPE_JOYCON_L;
                            uint8_t rp = is_s2 ? ns::CONTROLLER_TYPE_JOYCON_R_S2 : ns::CONTROLLER_TYPE_JOYCON_R;

                            hw_slots[base].client_idx = c;
                            hw_slots[base].sub_idx = s;
                            hw_slots[base].virtual_type = NS_TYPE_JOYCON_L;
                            hw_slots[base].pair_member = true;
                            hw_slots[base].pair_right = false;
                            set_controller_type_for_port(base, lp);

                            hw_slots[base + 1].client_idx = c;
                            hw_slots[base + 1].sub_idx = s;
                            hw_slots[base + 1].virtual_type = NS_TYPE_JOYCON_R;
                            hw_slots[base + 1].pair_member = true;
                            hw_slots[base + 1].pair_right = true;
                            set_controller_type_for_port(base + 1, rp);

                            if (g_ctx.verbose) std::println("Map -> PC {} (Pad {}) took console Ports {}+{} as Joy-Con L+R",
                                                          c + 1, s + 1, base + 1, base + 2);
                            publish_controller_status_event(c, s, g_ctx.console_player_leds[base].load(std::memory_order_relaxed), VIRTUAL_BODY_RGB[base]);
                            publish_controller_status_event(c, s, g_ctx.console_player_leds[base + 1].load(std::memory_order_relaxed), VIRTUAL_BODY_RGB[base + 1]);
                        }
                        continue;
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
                        const uint8_t virtual_type = virtual_type_for_profile(profile);
                        hw_slots[chosen].client_idx = c;
                        hw_slots[chosen].sub_idx = s;
                        hw_slots[chosen].virtual_type = virtual_type;
                        hw_slots[chosen].pair_member = false;
                        hw_slots[chosen].pair_right = false;
                        set_controller_type_for_port(chosen, profile);
                        if (g_ctx.verbose) std::println("Map -> PC {} (Pad {}) took console Port {}", c + 1, s + 1, chosen + 1);
                        publish_controller_status_event(c, s, g_ctx.console_player_leds[chosen].load(std::memory_order_relaxed), VIRTUAL_BODY_RGB[chosen]);
                    }
                }
            }

            uint8_t assignment_masks[MAX_CLIENTS][4] = {};
            uint8_t assignment_primary[MAX_CLIENTS][4];
            uint8_t assignment_requested[MAX_CLIENTS][4] = {};
            uint8_t assignment_virtual[MAX_CLIENTS][4] = {};
            for (int c = 0; c < MAX_CLIENTS; ++c) {
                for (int s = 0; s < 4; ++s) {
                    assignment_primary[c][s] = ns::CONTROLLER_CONSOLE_PORT_NONE;
                    if (snap[c].active) {
                        const uint8_t profile = requested_controller_profile_from_report(get_hid_report(snap[c], s));
                        assignment_requested[c][s] = profile;
                        assignment_virtual[c][s] = profile;
                    }
                }
            }
            for (int h = 0; h < nports; ++h) {
                if (hw_slots[h].client_idx < 0 || hw_slots[h].sub_idx < 0) continue;
                const int c = hw_slots[h].client_idx;
                const int s = hw_slots[h].sub_idx;
                assignment_masks[c][s] = static_cast<uint8_t>(assignment_masks[c][s] | (1u << h));
                if (assignment_primary[c][s] == ns::CONTROLLER_CONSOLE_PORT_NONE) assignment_primary[c][s] = static_cast<uint8_t>(h);
                const uint8_t req = assignment_requested[c][s];
                const bool s2_pair = (req == ns::CONTROLLER_TYPE_JOYCON_PAIR_S2);
                const bool s2 = (req == ns::CONTROLLER_TYPE_PRO_S2 || req == ns::CONTROLLER_TYPE_JOYCON_L_S2 || req == ns::CONTROLLER_TYPE_JOYCON_R_S2 || s2_pair);
                assignment_virtual[c][s] = hw_slots[h].pair_member
                    ? (s2_pair ? ns::CONTROLLER_TYPE_JOYCON_PAIR_S2 : ns::CONTROLLER_TYPE_JOYCON_PAIR)
                    : (hw_slots[h].virtual_type == NS_TYPE_JOYCON_L ? (s2 ? ns::CONTROLLER_TYPE_JOYCON_L_S2 : ns::CONTROLLER_TYPE_JOYCON_L)
                        : (hw_slots[h].virtual_type == NS_TYPE_JOYCON_R ? (s2 ? ns::CONTROLLER_TYPE_JOYCON_R_S2 : ns::CONTROLLER_TYPE_JOYCON_R)
                            : (s2 ? ns::CONTROLLER_TYPE_PRO_S2 : ns::CONTROLLER_TYPE_PRO)));
            }
            for (int c = 0; c < MAX_CLIENTS; ++c) {
                if (!snap[c].active) continue;
                for (int s = 0; s < 4; ++s) {
                    publish_client_assignment_event(c, s, assignment_masks[c][s], assignment_primary[c][s],
                                                    assignment_requested[c][s], assignment_virtual[c][s]);
                }
            }

            HIDReport out_reports[HID_PORT_COUNT];
            for (int h = 0; h < nports; ++h) {
                if (hw_slots[h].client_idx != -1) {
                    out_reports[h] = get_hid_report(snap[hw_slots[h].client_idx], hw_slots[h].sub_idx);
                    server_macro_apply(hw_slots[h].client_idx, hw_slots[h].sub_idx, out_reports[h].input);
                    const uint8_t profile = requested_controller_profile_from_report(out_reports[h]);
                    uint8_t eff_profile = profile;
                    if (profile_is_pair(profile)) {
                        bool is_s2 = (profile == ns::CONTROLLER_TYPE_JOYCON_PAIR_S2);
                        eff_profile = hw_slots[h].pair_right ? (is_s2 ? ns::CONTROLLER_TYPE_JOYCON_R_S2 : ns::CONTROLLER_TYPE_JOYCON_R) : (is_s2 ? ns::CONTROLLER_TYPE_JOYCON_L_S2 : ns::CONTROLLER_TYPE_JOYCON_L);
                    }
                    set_controller_type_for_port(h, eff_profile);
                    apply_controller_type_input(hw_slots[h].virtual_type, out_reports[h], hw_slots[h].pair_member);
                }
            }

            bool ok = true;
            for (int h = 0; h < nports; ++h) {
                const bool port_needed = (hw_slots[h].client_idx != -1);
                    uint8_t write_buf[HIDG_MAX_REPORT_SIZE] = {};
                    size_t write_len = PRO_REPORT_SIZE;
                    bool have_report_to_write = false, wrote_subcmd_reply = false, wrote_cmd_response = false;

                    if (hw_slots[h].virtual_type == NS_TYPE_HORI) {
                        HoriHIDReport r = out_reports[h].input;
                        r.vendor = 0;
                        if (port_needed && r != prev[h]) {
                            memcpy(write_buf, &r, sizeof(HoriHIDReport));
                            write_len = sizeof(HoriHIDReport);
                            have_report_to_write = true;
                            prev[h] = r;
                        }
                    } else {
                        if (rt[h].pending_cmd_response && g_port_switch2[h]) {
                            // Accurate S2 command responses (e.g. NFC 0x01) use observed header format, not 0x21 subcmd wrapper
                            size_t clen = std::min(rt[h].cmd_response_len, sizeof(write_buf));
                            if (clen == 0) clen = 64; // pad
                            std::memcpy(write_buf, rt[h].cmd_response_buf, clen);
                            if (clen < sizeof(write_buf)) std::memset(write_buf + clen, 0, sizeof(write_buf) - clen);
                            write_len = sizeof(write_buf);
                            have_report_to_write = true;
                            wrote_cmd_response = true;
                        } else if (rt[h].pending_subcmd_reply) {
                            rt[h].pending_reply.id = RID_INPUT_SUBCMD; // subcmd reply ID; research uses similar for S2
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
                                MotionReport jc_l_motion[3]{};
                                if (port_needed) {
                                    int cidx = hw_slots[h].client_idx, sidx = hw_slots[h].sub_idx;
                                    motion_for_port = get_hid_report(snap[cidx], sidx).motion;
                                    has_motion_for_port = get_hid_report(snap[cidx], sidx).has_motion != 0;
                                    if (hw_slots[h].pair_member && !hw_slots[h].pair_right) {
                                        motion_for_port = nullptr;
                                        has_motion_for_port = false;
                                    }
                                    if (motion_for_port && hw_slots[h].virtual_type == NS_TYPE_JOYCON_L) {
                                        for (int idx = 0; idx < 3; ++idx) {
                                            jc_l_motion[idx].ax = -motion_for_port[idx].ax;
                                            jc_l_motion[idx].ay = -motion_for_port[idx].ay;
                                            jc_l_motion[idx].az = motion_for_port[idx].az;
                                            jc_l_motion[idx].gx = -motion_for_port[idx].gx;
                                            jc_l_motion[idx].gy = -motion_for_port[idx].gy;
                                            jc_l_motion[idx].gz = motion_for_port[idx].gz;
                                        }
                                        motion_for_port = jc_l_motion;
                                    }
                                }
                                ProInputReport30 std_in{};
                                bool is_s2 = g_port_switch2[h];
                                check_amiibo_expiry(h);
                                if (is_s2) {
                                    // Over USB, S2 Joy-Cons must use the Pro Controller 2 format (0x09)
                                    build_s2_pro_report(report_for_port, motion_for_port, has_motion_for_port, rt[h].imu_enabled, pro_timer_from_us(now_stamp), h, write_buf);
                                    write_len = PRO_REPORT_SIZE;
                                } else {
                                    build_standard_report(report_for_port, motion_for_port, has_motion_for_port, rt[h].imu_enabled, pro_timer_from_us(now_stamp), std_in, is_s2);
                                    memcpy(write_buf, &std_in, sizeof(ProInputReport30));
                                    write_len = sizeof(ProInputReport30);
                                }
                                have_report_to_write = true;

                                if (port_needed || release_burst) rt[h].last_standard_report_us = now_stamp;
                                else rt[h].last_idle_neutral_us = now_stamp;
                            }
                        }
                    }

                    if (have_report_to_write) {
                        if (hw_slots[h].virtual_type != NS_TYPE_HORI) {
                            if (g_port_switch2[h]) {
                                apply_s2_controller_type_report(controller_type_for_port(h), write_buf);
                            } else {
                                apply_controller_type_report(controller_type_for_port(h), write_buf);
                            }
                        }
                        if (functionfs_submit_input_report(h, write_buf, write_len)) {
                            if (wrote_subcmd_reply) rt[h].pending_subcmd_reply = false;
                            if (wrote_cmd_response) rt[h].pending_cmd_response = false;
                            ++g_ctx.hid_writes;
                        }
                    }
                }

                auto process_host_output_report = [&](int h, const uint8_t* read_buf, size_t r) {
                    if (r < 2 || (r == 2 && read_buf[0] == 0 && read_buf[1] == 0)) return;

                    ++g_ctx.host_out_reports;
                    mark_switch2_usb_activity(now_stamp);
                    uint8_t id = read_buf[0];
                    bool is_s2 = g_port_switch2[h];
                    if (is_s2) {
                        // S2 command output from research: the report may contain header for command or rumble+cmd combined
                        // For S2, parse header if looks like command (cmd ID, 0x91 or data)
                        if (r >= 8) {
                            uint8_t cmd_id = read_buf[0];
                            uint8_t dir = read_buf[1];
                            if (dir == 0x91 || dir == 0x01) {
                                // S2 command header per research
                                uint8_t subcmd = read_buf[3];
                                std::span<const uint8_t> cmd_data(read_buf + 8, r > 8 ? std::min<size_t>(56, r - 8) : 0);
                                if (cmd_id == 0x01) {
                                    // NFC (and other 0x01 subs) - use accurate raw response format from commands.md
                                    uint8_t* cr = rt[h].cmd_response_buf;
                                    uint8_t transport = (r > 2 ? read_buf[2] : 0x00);
                                    cr[0] = 0x01;
                                    cr[1] = 0x01;
                                    cr[2] = transport;
                                    cr[3] = subcmd;
                                    cr[4] = 0x00;
                                    cr[5] = (subcmd == 0x15 ? 0x10 : 0x00);
                                    cr[6] = (subcmd == 0x15 ? 0x78 : 0xf8);
                                    cr[7] = 0x00;
                                    size_t pay = fill_nfc_response_payload(subcmd, cmd_data, cr + 8, h);
                                    rt[h].cmd_response_len = 8 + pay;
                                    if (subcmd == 0x15 && pay > 0) {
                                        // match ex response header more
                                        cr[2] = 0x01;
                                    }
                                    rt[h].pending_cmd_response = true;
                                    return;
                                }
                                // non-NFC commands still use subcmd reply path (may evolve)
                                if (hw_slots[h].client_idx != -1) {
                                    // rumble may be in other reports or combined
                                }
                                memset(&rt[h].pending_reply, 0, sizeof(rt[h].pending_reply));
                                int reply_len = handle_subcommand(rt[h], subcmd, cmd_data, &rt[h].pending_reply);
                                rt[h].pending_subcmd_reply = (reply_len >= 0);
                                return; // handled as S2 command
                            }
                        }
                        // fallback for rumble
                        if (id == 0x01 || id == 0x02 || id == RID_OUTPUT_RUMBLE) {
                            if (hw_slots[h].client_idx != -1) publish_rumble_event(hw_slots[h].client_idx, hw_slots[h].sub_idx, read_buf, r, true);
                            return;
                        }
                    }
                    if (id == RID_OUTPUT_CMD) {
                        if (r <= 10) return;
                        if (hw_slots[h].client_idx != -1) publish_rumble_event(hw_slots[h].client_idx, hw_slots[h].sub_idx, read_buf, r, false);
                        const uint8_t subcmd = read_buf[10];
                        std::span<const uint8_t> cmd_data(read_buf + 11, r > 11 ? std::min<size_t>(53, r - 11) : 0);
                        if ((subcmd == CMD_SET_PLAYER_LIGHTS || subcmd == 0x33) && !cmd_data.empty()) {
                            const uint8_t player_leds = cmd_data[0];
                            g_ctx.console_player_leds[h].store(player_leds, std::memory_order_relaxed);
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
                        if (functionfs_submit_input_report(h, resp_81, PRO_REPORT_SIZE)) mark_switch2_usb_activity(now_stamp);
                    }
                };

                for (int h = 0; h < nports; ++h) {
                    std::vector<unsigned char> ctrl_report;
                    for (int control_reads = 0; control_reads < 8 && functionfs_poll_control_report(h, ctrl_report); ++control_reads) {
                        process_host_output_report(h, ctrl_report.data(), ctrl_report.size());
                    }

                    std::vector<unsigned char> out_report;
                    for (int output_reads = 0; output_reads < 16 && functionfs_poll_output_report(h, out_report); ++output_reads) {
                        process_host_output_report(h, out_report.data(), out_report.size());
                    }
                }

                if (functionfs_transport_active() == false) ok = false;

            if (!ok) {
                if (!error_shown && g_ctx.verbose) { std::println("Host USB transport disconnected; waiting for reconnect..."); error_shown = true; }
                mark_switch2_usb_host_disconnected();
                close_all_fds();
                for (int wait_i = 0; wait_i < 100 && !stoken.stop_requested(); ++wait_i) std::this_thread::sleep_for(ms(10));
                break;
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
            std::println("pkts_rx={:<8}  hid_writes={:<8}  host_out={:<8}", (unsigned long long)g_ctx.pkts_rx.load(), (unsigned long long)g_ctx.hid_writes.load(), (unsigned long long)g_ctx.host_out_reports.load());
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
