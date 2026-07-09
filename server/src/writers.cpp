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


// Profile actually used for mapping/reports: coerced onto the active family so a
// mismatched client is adapted rather than dropped.
static uint8_t requested_controller_profile_from_report(const HIDReport& report) {
    return coerce_profile_to_family(report.reserved[2], g_ctx.usb_controller_family);
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

        const auto idle_virtual_type = [] {
            return g_ctx.usb_controller_family == UsbControllerFamily::Hori ? NS_TYPE_HORI : NS_TYPE_PRO;
        };
        for (int h = 0; h < nports; ++h) hw_slots[h].virtual_type = idle_virtual_type();

        auto protocol_for_slot = [](uint8_t profile, bool pair_right) -> uint8_t {
            if (!profile_is_pair(profile)) return profile;
            const bool is_s2 = profile == ns::CONTROLLER_TYPE_JOYCON_PAIR_S2;
            if (pair_right) return is_s2 ? ns::CONTROLLER_TYPE_JOYCON_R_S2
                                         : ns::CONTROLLER_TYPE_JOYCON_R;
            return is_s2 ? ns::CONTROLLER_TYPE_JOYCON_L_S2
                         : ns::CONTROLLER_TYPE_JOYCON_L;
        };

        struct SourceRequest {
            int client_idx = -1;
            int sub_idx = -1;
            uint8_t profile = ns::CONTROLLER_TYPE_PRO;
        };

        auto same_slot = [](const HwSlot& a, const HwSlot& b) {
            return a.client_idx == b.client_idx
                && a.sub_idx == b.sub_idx
                && a.virtual_type == b.virtual_type
                && a.pair_member == b.pair_member
                && a.pair_right == b.pair_right;
        };

        // Reconcile the complete source-pad layout in one pass. Allocating pairs
        // first makes [0,1]/[2,3] atomic and allows active single pads to be
        // compacted instead of permanently fragmenting the only pair groups.
        auto reconcile_hw_slots = [&](const ClientSession snaps[MAX_CLIENTS], uint64_t stamp) {
            std::vector<SourceRequest> pairs;
            std::vector<SourceRequest> singles;
            for (int c = 0; c < MAX_CLIENTS; ++c) {
                if (!snaps[c].active) continue;
                for (int s = 0; s < 4; ++s) {
                    const uint8_t profile = requested_controller_profile_from_report(get_hid_report(snaps[c], s));
                    if (!controller_profile_supported_by_usb_family(profile)) continue;
                    const bool macro_active = server_macro_running(c, s);
                    const bool active = snaps[c].uses_pad_presence
                        ? (snaps[c].pad_present[s] || macro_active)
                        : (!hid_is_neutral(get_hid_report(snaps[c], s)) || macro_active);
                    if (!active) continue;
                    (profile_is_pair(profile) ? pairs : singles).push_back({c, s, profile});
                }
            }

            auto existing_pair_base = [&](const SourceRequest& req) {
                for (int base = 0; base + 1 < nports; base += 2) {
                    const HwSlot& left = hw_slots[base];
                    const HwSlot& right = hw_slots[base + 1];
                    if (left.client_idx == req.client_idx && left.sub_idx == req.sub_idx
                            && left.pair_member && !left.pair_right
                            && right.client_idx == req.client_idx && right.sub_idx == req.sub_idx
                            && right.pair_member && right.pair_right) {
                        return base;
                    }
                }
                return -1;
            };
            auto existing_single_port = [&](const SourceRequest& req) {
                for (int h = 0; h < nports; ++h) {
                    if (hw_slots[h].client_idx == req.client_idx && hw_slots[h].sub_idx == req.sub_idx
                            && !hw_slots[h].pair_member) return h;
                }
                return -1;
            };
            const int requested_slots = static_cast<int>(pairs.size() * 2 + singles.size());
            if (requested_slots > nports) {
                // A live session is allowed to change its request, but that
                // change must not evict unrelated already-mapped players. Keep
                // only requests with an exact committed layout; the excess
                // request remains visibly unassigned in feedback until space
                // is available.
                pairs.erase(std::remove_if(pairs.begin(), pairs.end(), [&](const SourceRequest& req) {
                    return existing_pair_base(req) < 0;
                }), pairs.end());
                singles.erase(std::remove_if(singles.begin(), singles.end(), [&](const SourceRequest& req) {
                    return existing_single_port(req) < 0;
                }), singles.end());
            }
            std::stable_sort(pairs.begin(), pairs.end(), [&](const SourceRequest& a, const SourceRequest& b) {
                return (existing_pair_base(a) >= 0) > (existing_pair_base(b) >= 0);
            });
            std::stable_sort(singles.begin(), singles.end(), [&](const SourceRequest& a, const SourceRequest& b) {
                return (existing_single_port(a) >= 0) > (existing_single_port(b) >= 0);
            });

            HwSlot next_slots[HID_PORT_COUNT];
            for (int h = 0; h < nports; ++h) next_slots[h].virtual_type = idle_virtual_type();
            auto pair_base_free = [&](int base) {
                return base >= 0 && base + 1 < nports
                    && next_slots[base].client_idx == -1
                    && next_slots[base + 1].client_idx == -1;
            };
            auto first_free_pair_base = [&] {
                for (int base = 0; base + 1 < nports; base += 2) {
                    if (pair_base_free(base)) return base;
                }
                return -1;
            };
            auto first_free_port = [&] {
                for (int h = 0; h < nports; ++h) {
                    if (next_slots[h].client_idx == -1) return h;
                }
                return -1;
            };

            for (const SourceRequest& req : pairs) {
                int base = existing_pair_base(req);
                if (!pair_base_free(base)) base = -1;
                if (base < 0 && req.sub_idx < 2 && pair_base_free(req.sub_idx * 2)) base = req.sub_idx * 2;
                if (base < 0) base = first_free_pair_base();
                if (base < 0) continue;
                next_slots[base] = {req.client_idx, req.sub_idx, NS_TYPE_JOYCON_L, true, false};
                next_slots[base + 1] = {req.client_idx, req.sub_idx, NS_TYPE_JOYCON_R, true, true};
            }
            for (const SourceRequest& req : singles) {
                int port = existing_single_port(req);
                if (port < 0 || next_slots[port].client_idx != -1) port = -1;
                if (port < 0 && req.sub_idx < nports && next_slots[req.sub_idx].client_idx == -1) port = req.sub_idx;
                if (port < 0) port = first_free_port();
                if (port < 0) continue;
                next_slots[port] = {req.client_idx, req.sub_idx, virtual_type_for_profile(req.profile), false, false};
            }

            for (int h = 0; h < nports; ++h) {
                if (same_slot(hw_slots[h], next_slots[h])) continue;
                const HwSlot old = hw_slots[h];
                if (old.client_idx != -1) {
                    rt[h].neutral_burst_until_us = stamp + PRO_RELEASE_NEUTRAL_US;
                    functionfs_drain_output(h);
                    if (old.virtual_type == NS_TYPE_HORI && next_slots[h].client_idx == -1) {
                        HoriHIDReport neutral{};
                        neutral.reset();
                        neutral.vendor = 0;
                        functionfs_submit_input_report(h, reinterpret_cast<const uint8_t*>(&neutral), sizeof(neutral));
                        prev[h] = neutral;
                    }
                }
                hw_slots[h] = next_slots[h];
                if (hw_slots[h].client_idx != -1) {
                    const int c = hw_slots[h].client_idx;
                    const int s = hw_slots[h].sub_idx;
                    const uint8_t profile = requested_controller_profile_from_report(get_hid_report(snaps[c], s));
                    set_controller_type_for_port(h, protocol_for_slot(profile, hw_slots[h].pair_right));
                    if (old.client_idx != c || old.sub_idx != s) {
                        publish_controller_status_event(c, s,
                                                        g_ctx.console_player_leds[h].load(std::memory_order_relaxed),
                                                        VIRTUAL_BODY_RGB[h]);
                    }
                } else {
                    const uint8_t idle_profile = g_ctx.usb_controller_family == UsbControllerFamily::Switch2
                        ? ns::CONTROLLER_TYPE_PRO_S2
                        : (g_ctx.usb_controller_family == UsbControllerFamily::Hori
                            ? ns::CONTROLLER_TYPE_HORI
                            : ns::CONTROLLER_TYPE_PRO);
                    set_controller_type_for_port(h, idle_profile);
                }
            }
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
                    // Neutralize stale controls without erasing the requested
                    // controller profile. Identity is configuration, not live
                    // input; losing it used to remap quiet Joy-Cons/Hori pads
                    // as Pro controllers after only 350 ms.
                    HIDReport* pads[4] = {&snap[c].report.p1, &snap[c].report.p2,
                                          &snap[c].report.p3, &snap[c].report.p4};
                    uint8_t profiles[4] = {pads[0]->reserved[2], pads[1]->reserved[2],
                                           pads[2]->reserved[2], pads[3]->reserved[2]};
                    snap[c].report.reset();
                    for (int s = 0; s < 4; ++s) pads[s]->reserved[2] = profiles[s];
                }
            }

            // The USB controller family is now a server startup choice (--s2/--hori),
            // not a client request. Keep the gadget identity stable and coerce every
            // client profile onto g_ctx.usb_controller_family in app_state.cpp.

            reconcile_hw_slots(snap, now_stamp);

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
                if (hw_slots[h].pair_member) {
                    assignment_virtual[c][s] = s2_pair ? ns::CONTROLLER_TYPE_JOYCON_PAIR_S2
                                                       : ns::CONTROLLER_TYPE_JOYCON_PAIR;
                } else if (hw_slots[h].virtual_type == NS_TYPE_HORI) {
                    assignment_virtual[c][s] = ns::CONTROLLER_TYPE_HORI;
                } else if (hw_slots[h].virtual_type == NS_TYPE_JOYCON_L) {
                    assignment_virtual[c][s] = s2 ? ns::CONTROLLER_TYPE_JOYCON_L_S2
                                                   : ns::CONTROLLER_TYPE_JOYCON_L;
                } else if (hw_slots[h].virtual_type == NS_TYPE_JOYCON_R) {
                    assignment_virtual[c][s] = s2 ? ns::CONTROLLER_TYPE_JOYCON_R_S2
                                                   : ns::CONTROLLER_TYPE_JOYCON_R;
                } else {
                    assignment_virtual[c][s] = s2 ? ns::CONTROLLER_TYPE_PRO_S2
                                                   : ns::CONTROLLER_TYPE_PRO;
                }
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
                        if (port_needed) {
                            HoriHIDReport r = out_reports[h].input;
                            r.vendor = 0;
                            if (r != prev[h]) {
                                memcpy(write_buf, &r, sizeof(HoriHIDReport));
                                write_len = sizeof(HoriHIDReport);
                                have_report_to_write = true;
                                prev[h] = r;
                            }
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
                                    if (hw_slots[h].pair_member && !hw_slots[h].pair_right && !g_port_switch2[h]) {
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
                        const uint8_t expected_report_id = switch2_output_report_id_for_port(h);
                        const bool s2_output_report_id = (id == expected_report_id || id == 0x01 || id == 0x02);
                        size_t cmd_off = s2_output_report_id ? 1 : 0;
                        if (r >= cmd_off + 8 && read_buf[cmd_off + 1] == 0x91) {
                            const uint8_t cmd_id = read_buf[cmd_off + 0];
                            const uint8_t transport = read_buf[cmd_off + 2];
                            const uint8_t subcmd = read_buf[cmd_off + 3];
                            const size_t declared_len = read_buf[cmd_off + 5];
                            const size_t available_len = r > cmd_off + 8 ? (r - cmd_off - 8) : 0;
                            const size_t data_len = std::min({declared_len, available_len, static_cast<size_t>(56)});
                            std::span<const uint8_t> cmd_data(read_buf + cmd_off + 8, data_len);

                            uint8_t* cr = rt[h].cmd_response_buf;
                            std::memset(cr, 0, sizeof(rt[h].cmd_response_buf));
                            cr[0] = 0x05; // S2 common command-response input report
                            cr[1] = cmd_id;
                            cr[2] = 0x01;
                            cr[3] = transport;
                            cr[4] = subcmd;
                            // Response status bytes follow the captures/docs:
                            //   NFC command 0x01 commonly replies with 00 f8, while the
                            //   initialisation/feature/LED/rumble/battery paths use 10 78.
                            if (cmd_id == 0x01) {
                                cr[5] = 0x00;
                                cr[6] = 0xF8;
                            } else {
                                cr[5] = 0x10;
                                cr[6] = 0x78;
                            }
                            cr[7] = 0x00;
                            cr[8] = 0x00;
                            size_t payload_len = 0;
                            constexpr size_t payload_off = 9;

                            if (cmd_id == 0x03) {
                                if (subcmd == 0x03) {
                                    rt[h].full_report_enabled = cmd_data.empty() || cmd_data[0] != 0;
                                    cr[payload_off] = rt[h].full_report_enabled ? 0x01 : 0x00;
                                    payload_len = 4;
                                } else if (subcmd == 0x0A) {
                                    if (!cmd_data.empty()) rt[h].input_report_mode = cmd_data[0];
                                } else if (subcmd == 0x0D) {
                                    rt[h].full_report_enabled = true;
                                    cr[payload_off] = 0x01;
                                    payload_len = 4;
                                }
                            } else if (cmd_id == 0x0C) {
                                // Switch 2 feature selection. Motion is feature bit 2
                                // in the captured PC2 init path; reply shapes mirror the
                                // PicoSwitch2/ns2-testing handler so the console can finish
                                // feature setup before reading report 0x09 IMU data.
                                if (subcmd == 0x01) { // get feature info
                                    const uint8_t f = !cmd_data.empty() ? cmd_data[0] : 0;
                                    const bool joycon = controller_type_for_port(h) == NS_TYPE_JOYCON_L || controller_type_for_port(h) == NS_TYPE_JOYCON_R;
                                    const uint8_t motion_like = joycon ? 0x03 : 0x01;
                                    cr[payload_off + 4] = (f & 0x01) ? 0x07 : 0x00;
                                    cr[payload_off + 5] = (f & 0x02) ? 0x07 : 0x00;
                                    cr[payload_off + 6] = (f & 0x04) ? motion_like : 0x00;
                                    cr[payload_off + 7] = (f & 0x80) ? motion_like : 0x00;
                                    cr[payload_off + 8] = (f & 0x10) ? motion_like : 0x00;
                                    cr[payload_off + 9] = (f & 0x20) ? 0x03 : 0x00;
                                    payload_len = 12;
                                } else if (subcmd == 0x06) { // configure feature
                                    std::memset(cr + payload_off, 0, 40);
                                    const uint8_t f = !cmd_data.empty() ? cmd_data[0] : 0;
                                    const bool mentions_motion = (f & 0x04) != 0;
                                    if (mentions_motion) {
                                        // Capture/docs example for IMU configure returns 4 zero bytes,
                                        // length 2, then 00 50 followed by padding.
                                        cr[payload_off + 4] = 0x02;
                                        cr[payload_off + 8] = 0x00;
                                        cr[payload_off + 9] = 0x50;
                                        rt[h].imu_enabled = true;
                                    } else if (cmd_data.size() > 4) {
                                        cr[payload_off + 4] = cmd_data[4];
                                    }
                                    payload_len = 40;
                                } else { // set/clear/enable/disable mask
                                    if (subcmd == 0x03) {
                                        // Clear feature mask disables report features until a new mask is set.
                                        rt[h].imu_enabled = false;
                                    } else if (!cmd_data.empty()) {
                                        const bool mentions_motion = (cmd_data[0] & 0x04) != 0;
                                        if (subcmd == 0x04 && mentions_motion) rt[h].imu_enabled = true;
                                        else if (subcmd == 0x05 && mentions_motion) rt[h].imu_enabled = false;
                                        else if (subcmd == 0x02 && !mentions_motion) rt[h].imu_enabled = false;
                                    }
                                    payload_len = 4;
                                }
                            } else if (cmd_id == 0x01 && controller_port_supports_amiibo(h)) {
                                payload_len = fill_nfc_response_payload(subcmd, cmd_data, cr + payload_off, h);
                            }

                            rt[h].cmd_response_len = payload_off + payload_len;
                            rt[h].pending_cmd_response = true;
                            return;
                        }
                        if (s2_output_report_id) {
                            if (hw_slots[h].client_idx != -1) publish_s2_rumble_event(hw_slots[h].client_idx, hw_slots[h].sub_idx, read_buf, r, true);
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
