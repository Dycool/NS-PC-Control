#include "writers.hpp"
#include "app_state.hpp"
#include "gadget_wakeup.hpp"
#include "virtual_controller.hpp"
#include "bluetooth_manager.hpp"
#include "switch2_native.hpp"

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


static uint8_t raw_controller_profile_from_report(const HIDReport& report) {
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

// Profile actually requested by the source pad, coerced onto the active USB
// family. In --s2 the native port follows the request too: Joy-Con 2 identity
// is the same split-identity trick as S1 — USB PID stays Pro2 (0x2069) while
// factory memory 0x13014 / the ep0 identity block claim the Joy-Con 2 PID.
static uint8_t requested_controller_profile_from_report(const HIDReport& report) {
    return coerce_profile_to_family(raw_controller_profile_from_report(report), g_ctx.usb_controller_family);
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

static bool port_uses_s2_functionfs(int port) {
    return g_ctx.usb_controller_family == UsbControllerFamily::Switch2 && port == 0;
}

static int legacy_hidg_index_for_port(int port) {
    return port;
}

static bool port_uses_hori_hidg(int port) {
    return !port_uses_s2_functionfs(port)
        && g_ctx.usb_controller_family == UsbControllerFamily::Hori;
}

static bool write_all_nonblock_drop(int fd, const uint8_t* data, size_t len) {
    if (fd < 0 || !data || len == 0) return false;
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, data + off, len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return false;
            return false;
        }
        if (w == 0) return false;
        off += static_cast<size_t>(w);
    }
    return true;
}

void writer_thread(std::stop_token stoken, int hz) {
    // S2 exposes exactly one native FunctionFS controller. S1/HORI retain
    // the established four-port legacy layout.
    const int nports = configured_virtual_port_count();
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

    int hidg_fd[HID_PORT_COUNT] = {-1, -1, -1, -1};
    auto close_all_fds = [&]() {
        for (int i = 0; i < HID_PORT_COUNT; ++i) {
            if (hidg_fd[i] >= 0) { close(hidg_fd[i]); hidg_fd[i] = -1; }
        }
    };

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
            if (port_uses_s2_functionfs(i)) {
                const bool live = functionfs_transport_active() && functionfs_io_ready(i);
                if (!live) {
                    ffs_live[i] = false;
                    all_open = false;
                } else if (!ffs_live[i]) {
                    ffs_live[i] = true;
                    reset_port_runtime(i);
                }
                continue;
            }

            const int hidg_idx = legacy_hidg_index_for_port(i);
            if (hidg_fd[i] < 0) {
                const std::string path = "/dev/hidg" + std::to_string(hidg_idx);
                const int mode = port_uses_hori_hidg(i) ? O_WRONLY : O_RDWR;
                hidg_fd[i] = open(path.c_str(), mode | O_NONBLOCK);
                if (hidg_fd[i] >= 0) {
                    reset_port_runtime(i);
                    if (!port_uses_hori_hidg(i)) drain_hid_output_queue(hidg_fd[i]);
                }
            }
            if (hidg_fd[i] < 0) all_open = false;
        }

        if (!all_open) {
            clear_switch2_usb_activity();
            close_all_fds();
            run_gadget_setup_if_needed(false, "requested USB gadget endpoints could not all be opened");
            for (int wait_i = 0; wait_i < 50 && !stoken.stop_requested(); ++wait_i) std::this_thread::sleep_for(ms(10));
            continue;
        }

        if (g_ctx.verbose) {
            if (g_ctx.usb_controller_family == UsbControllerFamily::Switch2)
                std::println("1 native S2 FunctionFS port opened");
            else
                std::println("{}x legacy /dev/hidg* opened", nports);
        }
        // The identity live right now is what the console reads in the
        // handshake that follows this (re)connect.
        mark_s1_identity_enumerated();
        auto next = Clock::now() + tick;
        bool error_shown = false;
        bool timeout_printed[MAX_CLIENTS] = {};
        HoriHIDReport prev[HID_PORT_COUNT];
        for (int i = 0; i < HID_PORT_COUNT; ++i) prev[i].buttons = 0xFFFF;
        bool pairing_screen_open[HID_PORT_COUNT] = {}; // edge-trigger for grip/order auto-pair
        uint64_t last_switch_sleep_poll_us = 0;

        auto port_submit_input = [&](int h, const uint8_t* data, size_t len) -> bool {
            if (port_uses_s2_functionfs(h)) return functionfs_submit_input_report(h, data, len);
            return write_all_nonblock_drop(hidg_fd[h], data, len);
        };

        auto port_drain_output = [&](int h) {
            if (port_uses_s2_functionfs(h)) functionfs_drain_output(h);
            else if (!port_uses_hori_hidg(h)) drain_hid_output_queue(hidg_fd[h]);
        };

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

        // Reconcile the complete source-pad layout in one pass. In S2 mode
        // only the first single-controller request can own native port 0.
        // Joy-Con L+R is rejected by the connection layer before it reaches here.
        auto reconcile_hw_slots = [&](const ClientSession snaps[MAX_CLIENTS], uint64_t stamp) {
            std::vector<SourceRequest> ordered;
            std::vector<SourceRequest> pairs;
            std::vector<SourceRequest> singles;
            for (int c = 0; c < MAX_CLIENTS; ++c) {
                if (!snaps[c].active) continue;

                bool any_request_for_client = false;
                for (int sidx = 0; sidx < 4; ++sidx) {
                    const uint8_t profile = requested_controller_profile_from_report(get_hid_report(snaps[c], sidx));
                    if (!controller_profile_supported_by_usb_family(profile)) continue;
                    const bool macro_active = server_macro_running(c, sidx);
                    const bool active = snaps[c].uses_pad_presence
                        ? (snaps[c].pad_present[sidx] || macro_active)
                        : (!hid_is_neutral(get_hid_report(snaps[c], sidx)) || macro_active);
                    if (!active) continue;
                    const SourceRequest req{c, sidx, profile};
                    ordered.push_back(req);
                    (profile_is_pair(profile) ? pairs : singles).push_back(req);
                    any_request_for_client = true;
                }

                if (!any_request_for_client) {
                    // Reserve pad 1's configured identity while idle so the
                    // console sees the intended controller during enumeration.
                    const uint8_t idle_profile = requested_controller_profile_from_report(get_hid_report(snaps[c], 0));
                    if (controller_profile_supported_by_usb_family(idle_profile)) {
                        const SourceRequest req{c, 0, idle_profile};
                        ordered.push_back(req);
                        (profile_is_pair(idle_profile) ? pairs : singles).push_back(req);
                    }
                }
            }

            HwSlot next_slots[HID_PORT_COUNT];
            for (int h = 0; h < nports; ++h) next_slots[h].virtual_type = idle_virtual_type();

            auto free_port = [&](int h) {
                return h >= 0 && h < nports && next_slots[h].client_idx == -1;
            };
            auto first_free_port = [&](int begin) {
                for (int h = begin; h < nports; ++h) if (free_port(h)) return h;
                return -1;
            };
            auto existing_single_port = [&](const SourceRequest& req, int begin) {
                for (int h = begin; h < nports; ++h) {
                    if (hw_slots[h].client_idx == req.client_idx
                            && hw_slots[h].sub_idx == req.sub_idx
                            && !hw_slots[h].pair_member
                            && free_port(h)) return h;
                }
                return -1;
            };
            auto assign_single = [&](const SourceRequest& req, int port) {
                if (!free_port(port)) return false;
                next_slots[port] = {req.client_idx, req.sub_idx,
                                    virtual_type_for_profile(req.profile), false, false};
                return true;
            };
            auto assign_pair = [&](const SourceRequest& req, int left_port, int right_port) {
                if (!free_port(left_port) || !free_port(right_port) || left_port == right_port) return false;
                next_slots[left_port] = {req.client_idx, req.sub_idx, NS_TYPE_JOYCON_L, true, false};
                next_slots[right_port] = {req.client_idx, req.sub_idx, NS_TYPE_JOYCON_R, true, true};
                return true;
            };

            if (g_ctx.usb_controller_family == UsbControllerFamily::Switch2) {
                if (!ordered.empty() && !profile_is_pair(ordered.front().profile)) {
                    assign_single(ordered.front(), 0);
                }
            } else {
                // Preserve the established all-S1/HORI behavior: pairs consume
                // contiguous [0,1] / [2,3] groups and are allocated first.
                auto pair_base_free = [&](int base) {
                    return base >= 0 && base + 1 < nports && free_port(base) && free_port(base + 1);
                };
                auto existing_pair_base = [&](const SourceRequest& req) {
                    for (int base = 0; base + 1 < nports; base += 2) {
                        const HwSlot& left = hw_slots[base];
                        const HwSlot& right = hw_slots[base + 1];
                        if (left.client_idx == req.client_idx && left.sub_idx == req.sub_idx
                                && left.pair_member && !left.pair_right
                                && right.client_idx == req.client_idx && right.sub_idx == req.sub_idx
                                && right.pair_member && right.pair_right
                                && pair_base_free(base)) return base;
                    }
                    return -1;
                };
                for (const SourceRequest& req : pairs) {
                    int base = existing_pair_base(req);
                    if (base < 0) {
                        const int preferred = req.sub_idx * 2;
                        if (pair_base_free(preferred)) base = preferred;
                    }
                    if (base < 0) {
                        for (int candidate = 0; candidate + 1 < nports; candidate += 2) {
                            if (pair_base_free(candidate)) { base = candidate; break; }
                        }
                    }
                    if (base >= 0) assign_pair(req, base, base + 1);
                }
                for (const SourceRequest& req : singles) {
                    int port = existing_single_port(req, 0);
                    if (port < 0 && req.sub_idx < nports && free_port(req.sub_idx)) port = req.sub_idx;
                    if (port < 0) port = first_free_port(0);
                    if (port >= 0) assign_single(req, port);
                }
            }

            for (int h = 0; h < nports; ++h) {
                if (same_slot(hw_slots[h], next_slots[h])) continue;
                const HwSlot old = hw_slots[h];
                if (old.client_idx != -1) {
                    if (controller_port_supports_amiibo(h))
                        publish_amiibo_request(old.client_idx, old.sub_idx, false);
                    rt[h].neutral_burst_until_us = stamp + PRO_RELEASE_NEUTRAL_US;
                    port_drain_output(h);
                    if (old.virtual_type == NS_TYPE_HORI && next_slots[h].client_idx == -1) {
                        HoriHIDReport neutral{};
                        neutral.reset();
                        neutral.vendor = 0;
                        port_submit_input(h, reinterpret_cast<const uint8_t*>(&neutral), sizeof(neutral));
                        prev[h] = neutral;
                    }
                }
                hw_slots[h] = next_slots[h];
                if (hw_slots[h].client_idx != -1) {
                    const int c = hw_slots[h].client_idx;
                    const int s = hw_slots[h].sub_idx;
                    uint8_t profile = requested_controller_profile_from_report(get_hid_report(snaps[c], s));
                    if (profile_is_pair(profile))
                        profile = protocol_for_slot(profile, hw_slots[h].pair_right);
                    set_controller_type_for_port(h, profile);
                    if (old.client_idx != c || old.sub_idx != s) {
                        publish_controller_status_event(c, s,
                                                        g_ctx.console_player_leds[h].load(std::memory_order_relaxed),
                                                        VIRTUAL_BODY_RGB[h]);
                    }
                } else {
                    const uint8_t idle_profile = port_uses_s2_functionfs(h)
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

            // The console latches each port's type (device info/SPI) once per
            // USB session, so a changed controller identity needs one re-enumeration
            // to become visible — otherwise Joy-Con profiles keep showing up
            // as the Pro identity latched at plug-in. Debounced in
            // s1_identity_reenumeration_due() so a quick profile flip costs a
            // single reconnect blip. In --s2 this is also how the console is
            // made to re-read the native Pro2/Joy-Con2 split identity.
            if (s1_identity_reenumeration_due(now_stamp)) {
                if (g_ctx.verbose)
                    std::println("Controller type changed; re-enumerating USB gadget so the console re-reads identity");
                mark_s1_identity_enumerated();
                clear_switch2_usb_activity();
                close_all_fds();
                run_gadget_setup_if_needed(true, "controller type changed; console must re-read device identity");
                break;
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
                        assignment_virtual[c][s] = ns::CONTROLLER_TYPE_DEFAULT;
                    }
                }
            }
            for (int h = 0; h < nports; ++h) {
                if (hw_slots[h].client_idx < 0 || hw_slots[h].sub_idx < 0) continue;
                const int c = hw_slots[h].client_idx;
                const int s = hw_slots[h].sub_idx;
                assignment_masks[c][s] = static_cast<uint8_t>(assignment_masks[c][s] | (1u << h));
                if (assignment_primary[c][s] == ns::CONTROLLER_CONSOLE_PORT_NONE) assignment_primary[c][s] = static_cast<uint8_t>(h);
                const bool actual_s2 = port_uses_s2_functionfs(h);
                if (hw_slots[h].pair_member) {
                    assignment_virtual[c][s] = actual_s2 ? ns::CONTROLLER_TYPE_JOYCON_PAIR_S2
                                                         : ns::CONTROLLER_TYPE_JOYCON_PAIR;
                } else if (hw_slots[h].virtual_type == NS_TYPE_HORI) {
                    assignment_virtual[c][s] = ns::CONTROLLER_TYPE_HORI;
                } else if (hw_slots[h].virtual_type == NS_TYPE_JOYCON_L) {
                    assignment_virtual[c][s] = actual_s2 ? ns::CONTROLLER_TYPE_JOYCON_L_S2
                                                         : ns::CONTROLLER_TYPE_JOYCON_L;
                } else if (hw_slots[h].virtual_type == NS_TYPE_JOYCON_R) {
                    assignment_virtual[c][s] = actual_s2 ? ns::CONTROLLER_TYPE_JOYCON_R_S2
                                                         : ns::CONTROLLER_TYPE_JOYCON_R;
                } else {
                    assignment_virtual[c][s] = actual_s2 ? ns::CONTROLLER_TYPE_PRO_S2
                                                         : ns::CONTROLLER_TYPE_PRO;
                }
            }
            for (int c = 0; c < MAX_CLIENTS; ++c) {
                if (!snap[c].active) continue;
                for (int s = 0; s < 4; ++s) {
                    publish_client_assignment_event(c, s, assignment_masks[c][s], assignment_primary[c][s],
                                                    assignment_requested[c][s], assignment_virtual[c][s]);
                    bool has_native_nfc = false;
                    for (int h = 0; h < nports; ++h) {
                        if ((assignment_masks[c][s] & (1u << h)) && controller_port_supports_amiibo(h)) {
                            has_native_nfc = true;
                            break;
                        }
                    }
                    if (!has_native_nfc) publish_amiibo_request(c, s, false);
                }
            }

            HIDReport out_reports[HID_PORT_COUNT];
            for (int h = 0; h < nports; ++h) {
                if (hw_slots[h].client_idx != -1) {
                    out_reports[h] = get_hid_report(snap[hw_slots[h].client_idx], hw_slots[h].sub_idx);
                    server_macro_apply(hw_slots[h].client_idx, hw_slots[h].sub_idx, out_reports[h].input);
                    const uint8_t profile = requested_controller_profile_from_report(out_reports[h]);
                    uint8_t eff_profile = profile;
                    if (profile_is_pair(eff_profile))
                        eff_profile = protocol_for_slot(eff_profile, hw_slots[h].pair_right);
                    set_controller_type_for_port(h, eff_profile);
                    apply_controller_type_input(hw_slots[h].virtual_type, out_reports[h], hw_slots[h].pair_member);
                }
            }

            bool ok = true;
            for (int h = 0; h < nports; ++h) {
                const bool port_needed = (hw_slots[h].client_idx != -1);
                    if (port_uses_s2_functionfs(h)) {
                        // The only logical S2 port is native FunctionFS port 0.
                        // Its vendor channel owns the streaming/feature state.
                        rt[h].full_report_enabled = switch2_native_streaming_enabled(h);
                        rt[h].input_report_mode = switch2_native_selected_report(h);
                        const uint32_t enabled_features = switch2_native_enabled_features(h);
                        rt[h].imu_enabled = (enabled_features & 0x04u) != 0;
                        rt[h].vibration_enabled = (enabled_features & 0x20u) != 0;
                    }
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
                                bool motion_fresh_for_port = false;
                                MotionReport joycon_motion[3]{};
                                if (port_needed) {
                                    int cidx = hw_slots[h].client_idx, sidx = hw_slots[h].sub_idx;
                                    motion_for_port = get_hid_report(snap[cidx], sidx).motion;
                                    has_motion_for_port = get_hid_report(snap[cidx], sidx).has_motion != 0;
                                    const uint8_t motion_status = get_hid_report(snap[cidx], sidx).reserved[1];
                                    motion_fresh_for_port = has_motion_for_port &&
                                        ((motion_status & ns::EXT_STATUS_MOTION_FRESH_VALID)
                                            ? (motion_status & ns::EXT_STATUS_MOTION_FRESH) != 0
                                            : true);
                                    if (hw_slots[h].pair_member && !hw_slots[h].pair_right && !g_port_switch2[h]) {
                                        motion_for_port = nullptr;
                                        has_motion_for_port = false;
                                        motion_fresh_for_port = false;
                                    }
                                    if (motion_for_port && hw_slots[h].virtual_type == NS_TYPE_JOYCON_R
                                            && !g_port_switch2[h]) {
                                        // A Switch 1 Joy-Con R mounts its IMU with raw Y/Z axes
                                        // reversed relative to Joy-Con L and Pro Controller. The
                                        // client sends Pro-normalized motion, so convert it back to
                                        // the raw Joy-Con R convention expected in report 0x30.
                                        for (int idx = 0; idx < 3; ++idx) {
                                            joycon_motion[idx].ax = motion_for_port[idx].ax;
                                            joycon_motion[idx].ay = -motion_for_port[idx].ay;
                                            joycon_motion[idx].az = -motion_for_port[idx].az;
                                            joycon_motion[idx].gx = motion_for_port[idx].gx;
                                            joycon_motion[idx].gy = -motion_for_port[idx].gy;
                                            joycon_motion[idx].gz = -motion_for_port[idx].gz;
                                        }
                                        motion_for_port = joycon_motion;
                                    }
                                    // Synchronized Joy-Con 2 captures in the official grip show
                                    // same-axis, same-sign correspondence between upright Left and
                                    // Right units. Do not apply the old experimental S2-L inversion.
                                }
                                ProInputReport30 std_in{};
                                bool is_s2 = g_port_switch2[h];
                                check_amiibo_expiry(h);
                                if (is_s2) {
                                    if (!switch2_native_streaming_enabled(h)) {
                                        have_report_to_write = false;
                                        continue;
                                    }
                                    const bool imu_on = (switch2_native_enabled_features(h) & 0x04u) != 0;
                                    build_s2_pro_report(report_for_port, motion_for_port,
                                                        motion_fresh_for_port, imu_on,
                                                        pro_timer_from_us(now_stamp), now_stamp,
                                                        h, write_buf);
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
                                const uint8_t extra_buttons = port_needed ? out_reports[h].input.vendor : 0;
                                apply_controller_type_report(controller_type_for_port(h), extra_buttons, write_buf);
                            }
                        }
                        if (port_submit_input(h, write_buf, write_len)) {
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
                        // Native S2 HID OUT is rumble: report 0x02 for Pro Controller 2,
                        // report 0x01 for either Joy-Con 2 half.
                        // Init/pairing/feature/memory commands arrive on the vendor-bulk
                        // endpoint and are processed below via functionfs_poll_vendor_report().
                        if (id == switch2_output_report_id_for_port(h)) {
                            switch2_native_note_hid_out(h, std::span<const uint8_t>(read_buf, r));
                            if (hw_slots[h].client_idx != -1)
                                publish_s2_rumble_event(hw_slots[h].client_idx, hw_slots[h].sub_idx, read_buf, r, true);
                            return;
                        }
                        return;
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
                        if (port_submit_input(h, resp_81, PRO_REPORT_SIZE)) mark_switch2_usb_activity(now_stamp);
                    }
                };

                for (int h = 0; h < nports; ++h) {
                    if (port_uses_s2_functionfs(h)) {
                        std::vector<unsigned char> ctrl_report;
                        for (int control_reads = 0; control_reads < 8 && functionfs_poll_control_report(h, ctrl_report); ++control_reads) {
                            process_host_output_report(h, ctrl_report.data(), ctrl_report.size());
                        }

                        std::vector<unsigned char> out_report;
                        for (int output_reads = 0; output_reads < 16 && functionfs_poll_output_report(h, out_report); ++output_reads) {
                            process_host_output_report(h, out_report.data(), out_report.size());
                        }

                        std::vector<unsigned char> vendor_cmd;
                        for (int vendor_reads = 0; vendor_reads < 16 && functionfs_poll_vendor_report(h, vendor_cmd); ++vendor_reads) {
                            ++g_ctx.host_out_reports;
                            mark_switch2_usb_activity(now_stamp);
                            if (g_ctx.verbose && vendor_cmd.size() >= 4)
                                std::println("[s2] vendor cmd port {}: id={:#04x} sub={:#04x} len={}",
                                             h, vendor_cmd[0], vendor_cmd[3], vendor_cmd.size());
                            std::vector<uint8_t> vendor_resp;
                            if (switch2_native_handle_vendor_command(h, std::span<const uint8_t>(vendor_cmd.data(), vendor_cmd.size()), vendor_resp, rt[h])
                                    && !vendor_resp.empty()) {
                                const bool queued = functionfs_submit_vendor_report(h, vendor_resp.data(), vendor_resp.size());
                                if (g_ctx.verbose && vendor_cmd.size() >= 4 && vendor_cmd[0] == 0x01) {
                                    std::println("[s2][nfc][tx-queue] t_us={} port={} sub=0x{:02x} response_len={} queued={}",
                                                 now_us(), h, vendor_cmd[3], vendor_resp.size(), queued);
                                }
                            } else if (g_ctx.verbose && vendor_cmd.size() >= 4 && vendor_cmd[0] == 0x01) {
                                std::println(stderr,
                                             "[s2][nfc][tx-queue] port={} sub=0x{:02x} no response produced or handler rejected command",
                                             h, vendor_cmd[3]);
                            }
                        }
                    } else if (!port_uses_hori_hidg(h)) {
                        uint8_t read_buf[HIDG_MAX_REPORT_SIZE];
                        for (int output_reads = 0; output_reads < 16; ++output_reads) {
                            ssize_t r = read(hidg_fd[h], read_buf, sizeof(read_buf));
                            if (r < 0) {
                                if (errno == EINTR) continue;
                                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                                ok = false;
                                break;
                            }
                            if (r == 0) break;
                            process_host_output_report(h, read_buf, static_cast<size_t>(r));
                        }
                    }
                }

                if (g_ctx.usb_controller_family == UsbControllerFamily::Switch2 && functionfs_transport_active() == false) ok = false;

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
