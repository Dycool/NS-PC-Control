#include "app_state.hpp"
#include "gadget_wakeup.hpp"
#include "shared/sha256.h"

#include <print>
#include <string>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <span>
#include <algorithm>
#include <cerrno>
#include <bit>
#include <sys/socket.h>

using namespace ns;

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
ServerContext g_ctx{
    .usb_serial = "NSBRIDGE000001",
    .switch2_wakeup_config_path = "/etc/ns-pc-control/switch2_wakeup.conf",
    .switch2_wake_hci_dev = "hci0",
};
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif


const char* input_source_name(InputSource source) {
    switch (source) {
        case InputSource::Udp: return "UDP";
        case InputSource::WebSocket: return "WebSocket";
        case InputSource::Bluetooth: return "Bluetooth";
        case InputSource::None: default: return "none";
    }
}

const char* usb_controller_family_name(UsbControllerFamily family) {
    switch (family) {
        case UsbControllerFamily::Switch1: return "switch1";
        case UsbControllerFamily::Switch2: return "switch2";
        case UsbControllerFamily::Hori:    return "hori";
        default:                            return "unknown";
    }
}

uint64_t elapsed_us_saturated(uint64_t now, uint64_t then) {
    return (then == 0 || then > now) ? 0 : now - then;
}

bool elapsed_us_over(uint64_t now, uint64_t then, uint64_t limit) {
    return then != 0 && elapsed_us_saturated(now, then) > limit;
}

void mark_switch2_usb_activity(uint64_t now) {
    if (now == 0) now = now_us();

    if (g_ctx.switch2_usb_lifecycle_seen.load(std::memory_order_relaxed)) {
        g_ctx.switch2_usb_host_suspended.store(false, std::memory_order_relaxed);
        g_ctx.switch2_usb_inactive_since_us.store(0, std::memory_order_relaxed);
        g_ctx.switch2_sleep_confirmed.store(false, std::memory_order_relaxed);
    }

    const uint64_t prev_last = g_ctx.switch2_last_usb_activity_us.exchange(now, std::memory_order_relaxed);
    uint64_t stream_since = g_ctx.switch2_rx_stream_since_us.load(std::memory_order_relaxed);

    // The Switch can produce tiny RX flickers while suspending/re-enumerating.
    // Treat them as a new candidate stream, not as a full awake state. Only a
    // continuous RX stream re-arms suspend handling.
    if (prev_last == 0 || elapsed_us_saturated(now, prev_last) > SWITCH2_USB_ACTIVITY_FRESH_US || stream_since == 0) {
        stream_since = now;
        g_ctx.switch2_rx_stream_since_us.store(stream_since, std::memory_order_relaxed);
        g_ctx.switch2_rx_stream_stable.store(false, std::memory_order_relaxed);
    }

    g_ctx.switch2_usb_host_connected.store(true, std::memory_order_relaxed);

    if (elapsed_us_saturated(now, stream_since) >= SWITCH2_USB_ACTIVITY_STABLE_US) {
        bool was_stable = g_ctx.switch2_rx_stream_stable.exchange(true, std::memory_order_relaxed);
        g_ctx.switch2_sleep_confirmed.store(false, std::memory_order_relaxed);
        if (!was_stable && g_ctx.verbose) {
            std::println("[switch] USB RX stream stable; Switch active");
        }
    }
}

void mark_switch2_usb_host_resumed(uint64_t now) {
    if (now == 0) now = now_us();
    g_ctx.switch2_usb_lifecycle_seen.store(true, std::memory_order_relaxed);
    g_ctx.switch2_usb_host_suspended.store(false, std::memory_order_relaxed);
    g_ctx.switch2_usb_inactive_since_us.store(0, std::memory_order_relaxed);
    g_ctx.switch2_usb_host_connected.store(true, std::memory_order_relaxed);
    g_ctx.switch2_sleep_confirmed.store(false, std::memory_order_relaxed);
    // Treat a resume/configuration event as recent host activity for wake
    // suppression even when the S2 sends no further OUT reports.
    g_ctx.switch2_last_usb_activity_us.store(now, std::memory_order_relaxed);
}

void clear_switch2_usb_activity() {
    g_ctx.switch2_usb_host_connected.store(false, std::memory_order_relaxed);
    g_ctx.switch2_usb_lifecycle_seen.store(false, std::memory_order_relaxed);
    g_ctx.switch2_usb_host_suspended.store(false, std::memory_order_relaxed);
    g_ctx.switch2_usb_inactive_since_us.store(0, std::memory_order_relaxed);
    g_ctx.switch2_last_usb_activity_us.store(0, std::memory_order_relaxed);
    g_ctx.switch2_rx_stream_since_us.store(0, std::memory_order_relaxed);
    g_ctx.switch2_rx_stream_stable.store(false, std::memory_order_relaxed);
    g_ctx.switch2_sleep_confirmed.store(false, std::memory_order_relaxed);
}

void mark_switch2_usb_host_disconnected() {
    // Start a debounce window; poll_switch2_sleep_state() performs the actual
    // transition. This absorbs the disable/enable sequence of a quick gadget
    // re-enumeration without conflating normal S2 command silence with sleep.
    g_ctx.switch2_usb_host_connected.store(false, std::memory_order_relaxed);
    if (g_ctx.switch2_usb_lifecycle_seen.load(std::memory_order_relaxed)) {
        g_ctx.switch2_usb_host_suspended.store(true, std::memory_order_relaxed);
        uint64_t expected = 0;
        g_ctx.switch2_usb_inactive_since_us.compare_exchange_strong(
            expected, now_us(), std::memory_order_relaxed);
    }
}

bool switch2_usb_host_recently_active(uint64_t now) {
    if (g_ctx.usb_controller_family == UsbControllerFamily::Switch2
            && g_ctx.switch2_usb_lifecycle_seen.load(std::memory_order_relaxed)) {
        const bool active = !g_ctx.switch2_usb_host_suspended.load(std::memory_order_relaxed);
        g_ctx.switch2_usb_host_connected.store(active, std::memory_order_relaxed);
        return active;
    }
    uint64_t last = g_ctx.switch2_last_usb_activity_us.load(std::memory_order_relaxed);
    if (last == 0 || elapsed_us_saturated(now, last) > SWITCH2_USB_ACTIVITY_FRESH_US) {
        g_ctx.switch2_usb_host_connected.store(false, std::memory_order_relaxed);
        return false;
    }
    g_ctx.switch2_usb_host_connected.store(true, std::memory_order_relaxed);
    return g_ctx.switch2_rx_stream_stable.load(std::memory_order_relaxed);
}

static bool same_udp_endpoint(const sockaddr_in& a, const sockaddr_in& b) {
    return a.sin_family == b.sin_family &&
           a.sin_addr.s_addr == b.sin_addr.s_addr &&
           a.sin_port == b.sin_port;
}

void forget_switch2_dormant_udp_endpoint(const sockaddr_in& addr) {
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (g_ctx.switch2_dormant_udp_valid[i] && same_udp_endpoint(g_ctx.switch2_dormant_udp_addrs[i], addr)) {
            g_ctx.switch2_dormant_udp_valid[i] = false;
        }
    }
}

bool switch2_dormant_udp_endpoint_matches(const sockaddr_in& addr) {
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (g_ctx.switch2_dormant_udp_valid[i] && same_udp_endpoint(g_ctx.switch2_dormant_udp_addrs[i], addr)) {
            return true;
        }
    }
    return false;
}


bool switch2_wake_recent(uint64_t now) {
    if (now == 0) now = now_us();
    if (g_ctx.switch2_wake_adv_running.load(std::memory_order_relaxed)) return true;
    uint64_t last = g_ctx.switch2_last_wake_adv_us.load(std::memory_order_relaxed);
    return last != 0 && elapsed_us_saturated(now, last) <= SWITCH2_WAKE_CLIENT_GRACE_US;
}

bool switch2_sleep_confirmed(uint64_t now) {
    if (now == 0) now = now_us();
    poll_switch2_sleep_state(now);
    return g_ctx.switch2_sleep_confirmed.load(std::memory_order_relaxed);
}

static void confirm_switch2_sleep(uint64_t quiet_us, const char* evidence) {
    bool expected = false;
    if (!g_ctx.switch2_sleep_confirmed.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        return;
    }

    g_ctx.switch2_sleep_seq.fetch_add(1, std::memory_order_relaxed);
    for (int i = 0; i < MAX_CLIENTS; ++i) g_ctx.switch2_dormant_udp_valid[i] = false;

    for (int i = 0; i < MAX_CLIENTS; ++i) {
        bool stop_macros = false;
        {
            std::lock_guard<std::mutex> lk(g_ctx.mtx[i]);
            ClientSession& c = g_ctx.clients[i];
            if (!c.active) continue;
            if (c.source == InputSource::Udp) {
                g_ctx.switch2_dormant_udp_addrs[i] = c.addr;
                g_ctx.switch2_dormant_udp_valid[i] = true;
                reset_client_session_locked(c);
                stop_macros = true;
            } else if (c.source == InputSource::WebSocket || c.source == InputSource::Bluetooth) {
                reset_client_session_locked(c);
                stop_macros = true;
            }
        }
        if (stop_macros) server_macro_stop_all_for_client(i);
    }
    if (g_ctx.verbose) {
        std::println("[switch] confirmed asleep after {:.1f}s {}; input sessions released",
                     static_cast<double>(quiet_us) / 1000000.0, evidence);
    }
}

void poll_switch2_sleep_state(uint64_t now) {
    if (now == 0) now = now_us();

    // The current native S2 transport exposes authoritative bus lifecycle
    // events.  An idle command/RX stream is normal while the console is awake,
    // so only a sustained FunctionFS suspend/disable may release sessions.
    if (g_ctx.usb_controller_family == UsbControllerFamily::Switch2
            && g_ctx.switch2_usb_lifecycle_seen.load(std::memory_order_relaxed)) {
        if (!g_ctx.switch2_usb_host_suspended.load(std::memory_order_relaxed)) {
            g_ctx.switch2_usb_host_connected.store(true, std::memory_order_relaxed);
            return;
        }
        const uint64_t inactive_since = g_ctx.switch2_usb_inactive_since_us.load(std::memory_order_relaxed);
        if (inactive_since == 0
                || elapsed_us_saturated(now, inactive_since) <= SWITCH2_USB_ACTIVITY_FRESH_US
                || switch2_wake_recent(now)) {
            return;
        }
        confirm_switch2_sleep(elapsed_us_saturated(now, inactive_since),
                              "of USB suspend/disconnect");
        return;
    }

    uint64_t last = g_ctx.switch2_last_usb_activity_us.load(std::memory_order_relaxed);
    if (last == 0) {
        g_ctx.switch2_usb_host_connected.store(false, std::memory_order_relaxed);
        g_ctx.switch2_rx_stream_since_us.store(0, std::memory_order_relaxed);
        g_ctx.switch2_rx_stream_stable.store(false, std::memory_order_relaxed);
        return;
    }

    const uint64_t quiet_us = elapsed_us_saturated(now, last);
    if (quiet_us <= SWITCH2_USB_ACTIVITY_FRESH_US) {
        g_ctx.switch2_usb_host_connected.store(true, std::memory_order_relaxed);
        return;
    }

    g_ctx.switch2_usb_host_connected.store(false, std::memory_order_relaxed);

    // If the last RX stream was only a short flicker, ignore it. This happens
    // during the Switch 2 suspend/reconnect dance and should not cause repeated
    // BT disconnects / UDP slot churn / wake spam.
    if (!g_ctx.switch2_rx_stream_stable.exchange(false, std::memory_order_relaxed)) {
        g_ctx.switch2_rx_stream_since_us.store(0, std::memory_order_relaxed);
        return;
    }
    g_ctx.switch2_rx_stream_since_us.store(0, std::memory_order_relaxed);

    confirm_switch2_sleep(quiet_us, "without stable USB RX");
}

void rearm_switch2_wake_after_client_disconnect() {
    // Kept for older call sites. Disconnecting a client no longer forces the next
    // wake; wake is decided from fresh Switch RX activity plus cooldown.
}

bool any_recent_client_active(uint64_t now) {
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        std::lock_guard<std::mutex> lk(g_ctx.mtx[i]);
        if (g_ctx.clients[i].active && g_ctx.clients[i].last_rx_us != 0 && elapsed_us_saturated(now, g_ctx.clients[i].last_rx_us) <= CLIENT_TIMEOUT_US) return true;
    }
    return false;
}

int active_client_count(uint64_t now) {
    if (now == 0) now = now_us();
    int count = 0;
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        std::lock_guard<std::mutex> lk(g_ctx.mtx[i]);
        const ClientSession& c = g_ctx.clients[i];
        if (c.active && c.last_rx_us != 0 && elapsed_us_saturated(now, c.last_rx_us) <= CLIENT_TIMEOUT_US) {
            ++count;
        }
    }
    return count;
}

// The physical "shape" a profile represents, independent of Switch generation.
enum class ProfileShape : uint8_t { Pro, JoyConL, JoyConR, Pair };

static ProfileShape profile_shape(uint8_t profile) {
    switch (profile) {
        case ns::CONTROLLER_TYPE_JOYCON_L:
        case ns::CONTROLLER_TYPE_JOYCON_L_S2:    return ProfileShape::JoyConL;
        case ns::CONTROLLER_TYPE_JOYCON_R:
        case ns::CONTROLLER_TYPE_JOYCON_R_S2:    return ProfileShape::JoyConR;
        case ns::CONTROLLER_TYPE_JOYCON_PAIR:
        case ns::CONTROLLER_TYPE_JOYCON_PAIR_S2: return ProfileShape::Pair;
        default:                                 return ProfileShape::Pro; // Pro/Pro_S2/Hori/default
    }
}

// Map any requested profile onto the equivalent profile in the given family.
// The USB device can only be one family at a time, so a mismatched client is
// adapted to the active family (keeping its L/R/pair shape) instead of rejected.
uint8_t coerce_profile_to_family(uint8_t profile, UsbControllerFamily family) {
    const ProfileShape shape = profile_shape(profile);
    switch (family) {
        case UsbControllerFamily::Switch2:
            switch (shape) {
                case ProfileShape::JoyConL: return ns::CONTROLLER_TYPE_JOYCON_L_S2;
                case ProfileShape::JoyConR: return ns::CONTROLLER_TYPE_JOYCON_R_S2;
                case ProfileShape::Pair:    return ns::CONTROLLER_TYPE_JOYCON_PAIR_S2;
                default:                    return ns::CONTROLLER_TYPE_PRO_S2;
            }
        case UsbControllerFamily::Hori:
            return ns::CONTROLLER_TYPE_HORI; // Hori exposes a single Pro-like pad
        case UsbControllerFamily::Switch1:
        default:
            switch (shape) {
                case ProfileShape::JoyConL: return ns::CONTROLLER_TYPE_JOYCON_L;
                case ProfileShape::JoyConR: return ns::CONTROLLER_TYPE_JOYCON_R;
                case ProfileShape::Pair:    return ns::CONTROLLER_TYPE_JOYCON_PAIR;
                default:                    return ns::CONTROLLER_TYPE_PRO;
            }
    }
}

static uint8_t raw_profile_from_wire_report(const ns::HIDReport& report) {
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

static uint8_t requested_profile_from_wire_report(const ns::HIDReport& report) {
    return coerce_profile_to_family(raw_profile_from_wire_report(report), g_ctx.usb_controller_family);
}

UsbControllerFamily usb_family_for_profile(uint8_t profile) {
    switch (profile) {
        case ns::CONTROLLER_TYPE_PRO_S2:
        case ns::CONTROLLER_TYPE_JOYCON_L_S2:
        case ns::CONTROLLER_TYPE_JOYCON_R_S2:
        case ns::CONTROLLER_TYPE_JOYCON_PAIR_S2:
            return UsbControllerFamily::Switch2;
        case ns::CONTROLLER_TYPE_HORI:
            return UsbControllerFamily::Hori;
        default:
            return UsbControllerFamily::Switch1;
    }
}

bool controller_profile_supported_by_usb_family(uint8_t /*profile*/) {
    // Every requested profile is now coerced onto the active family
    // (coerce_profile_to_family), so a client is never rejected for a family
    // mismatch — it is adapted to the current family instead. Kept as an always
    // -true hook so slot accounting and the old reject call sites stay valid.
    return true;
}

bool report_requests_unsupported_s2_pair(const ns::MultiReport& report, const bool pad_present[4], bool reserve_when_idle) {
    if (g_ctx.usb_controller_family != UsbControllerFamily::Switch2) return false;
    const ns::HIDReport* pads[4] = {&report.p1, &report.p2, &report.p3, &report.p4};
    bool any_present = false;
    for (int s = 0; s < 4; ++s) {
        if (pad_present && !pad_present[s]) continue;
        any_present = true;
        const uint8_t raw = raw_profile_from_wire_report(*pads[s]);
        if (raw == ns::CONTROLLER_TYPE_JOYCON_PAIR || raw == ns::CONTROLLER_TYPE_JOYCON_PAIR_S2) return true;
    }
    if (!any_present && reserve_when_idle) {
        const uint8_t raw = raw_profile_from_wire_report(report.p1);
        return raw == ns::CONTROLLER_TYPE_JOYCON_PAIR || raw == ns::CONTROLLER_TYPE_JOYCON_PAIR_S2;
    }
    return false;
}

int requested_virtual_slots_for_report(const ns::MultiReport& report, const bool pad_present[4], bool reserve_when_idle) {
    const ns::HIDReport* pads[4] = {&report.p1, &report.p2, &report.p3, &report.p4};
    int needed = 0;
    for (int s = 0; s < 4; ++s) {
        if (pad_present && !pad_present[s]) continue;
        const uint8_t profile = requested_profile_from_wire_report(*pads[s]);
        if (!controller_profile_supported_by_usb_family(profile)) continue;
        if (g_ctx.usb_controller_family == UsbControllerFamily::Switch2) {
            // A pair still requests two logical halves, which deliberately exceeds
            // the single native S2 slot and is rejected by the connection layer.
            needed += profile == ns::CONTROLLER_TYPE_JOYCON_PAIR_S2 ? 2 : 1;
        } else {
            needed += (profile == ns::CONTROLLER_TYPE_JOYCON_PAIR || profile == ns::CONTROLLER_TYPE_JOYCON_PAIR_S2) ? 2 : 1;
        }
    }
    if (needed == 0 && reserve_when_idle) {
        const uint8_t profile = requested_profile_from_wire_report(report.p1);
        if (controller_profile_supported_by_usb_family(profile)) {
            needed = (g_ctx.usb_controller_family == UsbControllerFamily::Switch2)
                ? (profile == ns::CONTROLLER_TYPE_JOYCON_PAIR_S2 ? 2 : 1)
                : ((profile == ns::CONTROLLER_TYPE_JOYCON_PAIR || profile == ns::CONTROLLER_TYPE_JOYCON_PAIR_S2) ? 2 : 1);
        }
    }
    // Do not clamp here. Callers use a value larger than the configured
    // capacity to reject requests that cannot be mapped completely.
    return needed;
}


int configured_virtual_port_count() {
    return g_ctx.usb_controller_family == UsbControllerFamily::Switch2 ? 1 : HID_PORT_COUNT;
}

int configured_client_capacity() {
    return g_ctx.usb_controller_family == UsbControllerFamily::Switch2 ? 1 : MAX_CLIENTS;
}

int active_requested_virtual_slots(uint64_t now, int ignore_client_idx) {
    if (now == 0) now = now_us();
    int used = 0;
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (i == ignore_client_idx) continue;
        std::lock_guard<std::mutex> lk(g_ctx.mtx[i]);
        ClientSession& c = g_ctx.clients[i];
        repair_future_client_timestamp(c, now);
        if (!c.active || c.last_rx_us == 0 || elapsed_us_saturated(now, c.last_rx_us) > CLIENT_TIMEOUT_US) continue;
        uint8_t assigned_mask = 0;
        for (int s = 0; s < 4; ++s) {
            assigned_mask = static_cast<uint8_t>(assigned_mask | c.client_assignment[s].console_port_mask);
        }
        // An existing assignment does not cover source pads which appeared
        // later in the same session. Reserve for the larger of the committed
        // layout and the current request so another client cannot be admitted
        // into those pending ports in the gap before the writer reconciles it.
        const int assigned = std::popcount(static_cast<unsigned>(assigned_mask));
        const int requested = requested_virtual_slots_for_report(c.report, c.pad_present, true);
        used += std::max(assigned, requested);
    }
    return std::min(used, configured_virtual_port_count());
}

int free_virtual_slot_count(uint64_t now, int ignore_client_idx) {
    const int capacity = configured_virtual_port_count();
    const int used = active_requested_virtual_slots(now, ignore_client_idx);
    return std::max(0, capacity - used);
}

uint32_t pack_server_state(uint8_t active_clients, uint8_t free_virtual_slots, bool switch_asleep) {
    return static_cast<uint32_t>(active_clients)
        | (static_cast<uint32_t>(free_virtual_slots) << 8)
        | (switch_asleep ? (1u << 16) : 0u);
}

uint64_t refresh_server_state_seq(uint64_t now, bool force) {
    if (now == 0) now = now_us();
    const uint64_t last_refresh = g_ctx.server_state_last_refresh_us.load(std::memory_order_relaxed);
    if (!force && last_refresh != 0 && elapsed_us_saturated(now, last_refresh) < SERVER_STATE_REFRESH_MIN_US) {
        return g_ctx.server_state_seq.load(std::memory_order_relaxed);
    }
    g_ctx.server_state_last_refresh_us.store(now, std::memory_order_relaxed);
    const uint8_t active_clients = static_cast<uint8_t>(std::clamp(active_client_count(now), 0, configured_client_capacity()));
    const uint8_t free_slots = static_cast<uint8_t>(std::clamp(free_virtual_slot_count(now), 0, configured_virtual_port_count()));
    const bool asleep = switch2_sleep_confirmed(now);
    const uint32_t packed = pack_server_state(active_clients, free_slots, asleep);
    uint32_t old = g_ctx.server_state_packed.load(std::memory_order_relaxed);
    while (old != packed) {
        if (g_ctx.server_state_packed.compare_exchange_weak(old, packed, std::memory_order_relaxed)) {
            return g_ctx.server_state_seq.fetch_add(1, std::memory_order_relaxed) + 1;
        }
    }
    return g_ctx.server_state_seq.load(std::memory_order_relaxed);
}

uint8_t switch_player_index_from_leds(uint8_t player_leds) {
    const uint8_t solid = static_cast<uint8_t>(player_leds & 0x0F);
    const uint8_t flashing = static_cast<uint8_t>((player_leds >> 4) & 0x0F);
    const uint8_t bits = solid ? solid : flashing;
    switch (bits) {
        case 0x01: return 0;
        case 0x02: return 1;
        case 0x04: return 2;
        case 0x08: return 3;
        default: return ns::CONTROLLER_PLAYER_INDEX_UNKNOWN;
    }
}

void publish_controller_status_event(int client_idx, int sub_idx, uint8_t player_leds, const uint8_t* body_rgb) {
    if (client_idx < 0 || client_idx >= MAX_CLIENTS || sub_idx < 0 || sub_idx >= 4) return;
    std::lock_guard<std::mutex> lk(g_ctx.mtx[client_idx]);
    ClientSession& c = g_ctx.clients[client_idx];
    if (!c.active) return;
    ControllerStatusState& st = c.controller_status[sub_idx];
    const uint8_t player_index = switch_player_index_from_leds(player_leds);
    bool changed = st.player_leds != player_leds || st.player_index != player_index;
    st.player_leds = player_leds;
    st.player_index = player_index;
    if (body_rgb) {
        if (!st.body_rgb_valid || st.body_rgb[0] != body_rgb[0] || st.body_rgb[1] != body_rgb[1] || st.body_rgb[2] != body_rgb[2]) changed = true;
        st.body_rgb[0] = body_rgb[0];
        st.body_rgb[1] = body_rgb[1];
        st.body_rgb[2] = body_rgb[2];
        st.body_rgb_valid = true;
    }
    if (changed) c.controller_status_seq[sub_idx]++;
}

void publish_client_assignment_event(int client_idx, int sub_idx, uint8_t console_port_mask,
                                     uint8_t primary_console_port, uint8_t requested_type,
                                     uint8_t virtual_type) {
    if (client_idx < 0 || client_idx >= MAX_CLIENTS || sub_idx < 0 || sub_idx >= 4) return;
    std::lock_guard<std::mutex> lk(g_ctx.mtx[client_idx]);
    ClientSession& c = g_ctx.clients[client_idx];
    if (!c.active) return;
    ClientAssignmentState& st = c.client_assignment[sub_idx];
    if (primary_console_port >= HID_PORT_COUNT) primary_console_port = ns::CONTROLLER_CONSOLE_PORT_NONE;
    console_port_mask = static_cast<uint8_t>(console_port_mask & ((1u << HID_PORT_COUNT) - 1u));
    const bool changed = st.console_port_mask != console_port_mask
        || st.primary_console_port != primary_console_port
        || st.requested_type != requested_type
        || st.virtual_type != virtual_type;
    st.console_port_mask = console_port_mask;
    st.primary_console_port = primary_console_port;
    st.requested_type = requested_type;
    st.virtual_type = virtual_type;
    if (changed) c.client_assignment_seq[sub_idx]++;
}

bool get_controller_status_packet(int client_idx, int sub_idx, uint32_t& seq, ns::ControllerStatusPacket& packet) {
    if (client_idx < 0 || client_idx >= MAX_CLIENTS || sub_idx < 0 || sub_idx >= 4) return false;
    std::lock_guard<std::mutex> lk(g_ctx.mtx[client_idx]);
    const ClientSession& c = g_ctx.clients[client_idx];
    if (!c.active) return false;
    seq = c.controller_status_seq[sub_idx];
    packet = ns::ControllerStatusPacket{};
    packet.subpad = static_cast<uint8_t>(sub_idx);
    packet.player_index = c.controller_status[sub_idx].player_index;
    packet.player_leds = c.controller_status[sub_idx].player_leds;
    if (c.controller_status[sub_idx].body_rgb_valid) {
        packet.reserved[0] = c.controller_status[sub_idx].body_rgb[0];
        packet.reserved[1] = c.controller_status[sub_idx].body_rgb[1];
        packet.reserved[2] = c.controller_status[sub_idx].body_rgb[2];
        packet.reserved[3] |= ns::CONTROLLER_STATUS_FLAG_BODY_RGB_VALID;
    }
    return true;
}

bool get_client_assignment_packet(int client_idx, int sub_idx, uint8_t active_clients,
                                  uint8_t free_virtual_slots, uint32_t& seq,
                                  ns::ClientAssignmentPacket& packet) {
    if (client_idx < 0 || client_idx >= MAX_CLIENTS || sub_idx < 0 || sub_idx >= 4) return false;
    const bool switch_asleep = switch2_sleep_confirmed();
    std::lock_guard<std::mutex> lk(g_ctx.mtx[client_idx]);
    const ClientSession& c = g_ctx.clients[client_idx];
    if (!c.active) return false;
    seq = c.client_assignment_seq[sub_idx];
    packet = ns::ClientAssignmentPacket{};
    packet.flags = ns::CLIENT_ASSIGNMENT_FLAG_ACCEPTED;
    packet.server_slot = static_cast<uint8_t>(client_idx);
    packet.subpad = static_cast<uint8_t>(sub_idx);
    packet.console_port_mask = c.client_assignment[sub_idx].console_port_mask;
    packet.primary_console_port = c.client_assignment[sub_idx].primary_console_port;
    packet.requested_type = c.client_assignment[sub_idx].requested_type;
    packet.virtual_type = c.client_assignment[sub_idx].virtual_type;
    if (packet.console_port_mask != 0) packet.flags |= ns::CLIENT_ASSIGNMENT_FLAG_ASSIGNMENT_VALID;
    if (switch_asleep) packet.flags |= ns::CLIENT_ASSIGNMENT_FLAG_SWITCH_ASLEEP;
    packet.active_clients = active_clients;
    packet.max_clients = static_cast<uint8_t>(configured_client_capacity());
    packet.free_virtual_slots = free_virtual_slots;
    return true;
}

int console_port_for_client_subpad(int client_idx, int sub_idx) {
    if (client_idx < 0 || client_idx >= MAX_CLIENTS || sub_idx < 0 || sub_idx >= 4) return -1;
    std::lock_guard<std::mutex> lk(g_ctx.mtx[client_idx]);
    const ClientSession& c = g_ctx.clients[client_idx];
    if (!c.active) return -1;
    const uint8_t primary = c.client_assignment[sub_idx].primary_console_port;
    if (primary < HID_PORT_COUNT) return primary;
    const uint8_t mask = c.client_assignment[sub_idx].console_port_mask;
    for (int port = 0; port < HID_PORT_COUNT; ++port) {
        if (mask & (1u << port)) return port;
    }
    return -1;
}

bool client_subpad_for_console_port(int console_port, int& client_idx, int& sub_idx) {
    client_idx = -1;
    sub_idx = -1;
    if (console_port < 0 || console_port >= HID_PORT_COUNT) return false;
    const uint8_t bit = static_cast<uint8_t>(1u << console_port);
    for (int cidx = 0; cidx < MAX_CLIENTS; ++cidx) {
        std::lock_guard<std::mutex> lk(g_ctx.mtx[cidx]);
        const ClientSession& c = g_ctx.clients[cidx];
        if (!c.active) continue;
        for (int sidx = 0; sidx < 4; ++sidx) {
            if (c.client_assignment[sidx].console_port_mask & bit) {
                client_idx = cidx;
                sub_idx = sidx;
                return true;
            }
        }
    }
    return false;
}

void publish_amiibo_request(int client_idx, int sub_idx, bool requested) {
    if (client_idx < 0 || client_idx >= MAX_CLIENTS || sub_idx < 0 || sub_idx >= 4) return;
    std::lock_guard<std::mutex> lk(g_ctx.mtx[client_idx]);
    ClientSession& c = g_ctx.clients[client_idx];
    if (!c.active) return;
    // Only create a new event for an actual state transition. Send the same
    // event a few times because this UI control message travels over UDP.
    if (c.amiibo_request_seq[sub_idx] != 0 && c.amiibo_requested[sub_idx] == requested) return;
    c.amiibo_requested[sub_idx] = requested;
    c.amiibo_request_pending[sub_idx] = true;
    c.amiibo_request_repeats[sub_idx] = 3;
    if (++c.amiibo_request_seq[sub_idx] == 0) ++c.amiibo_request_seq[sub_idx];
}

void publish_amiibo_writeback(int client_idx, int sub_idx, const uint8_t* data, uint16_t len) {
    if (client_idx < 0 || client_idx >= MAX_CLIENTS || sub_idx < 0 || sub_idx >= 4 || !data || len == 0) return;
    std::lock_guard<std::mutex> lk(g_ctx.mtx[client_idx]);
    ClientSession& c = g_ctx.clients[client_idx];
    if (!c.active || c.source != InputSource::Udp) return;
    c.amiibo_writeback_pending[sub_idx] = true;
    c.amiibo_writeback_len[sub_idx] = len;
    std::memcpy(c.amiibo_writeback_data[sub_idx], data, std::min<size_t>(len, 540));
}

void store_client_source_names(int client_idx, const ns::ClientNamesPacket& packet) {
    if (client_idx < 0 || client_idx >= MAX_CLIENTS) return;
    std::lock_guard<std::mutex> lk(g_ctx.mtx[client_idx]);
    ClientSession& c = g_ctx.clients[client_idx];
    if (!c.active) return;
    for (int s = 0; s < 4; ++s) {
        ns::RosterEntry e = packet.pads[s];
        e.present = e.present ? 1 : 0;
        e.name[ns::ROSTER_NAME_CAP - 1] = '\0';
        c.source_pads[s] = e;
    }
    g_ctx.roster_last_refresh_us.store(0, std::memory_order_relaxed);
}

static bool roster_entry_equal(const ns::RosterEntry& a, const ns::RosterEntry& b) {
    return a.present == b.present && a.has_gyro == b.has_gyro
        && std::memcmp(a.name, b.name, ns::ROSTER_NAME_CAP) == 0;
}

uint64_t refresh_roster_seq(uint64_t now, bool force) {
    if (now == 0) now = now_us();
    const uint64_t last_refresh = g_ctx.roster_last_refresh_us.load(std::memory_order_relaxed);
    if (!force && last_refresh != 0 && elapsed_us_saturated(now, last_refresh) < SERVER_STATE_REFRESH_MIN_US) {
        return g_ctx.roster_seq.load(std::memory_order_relaxed);
    }
    g_ctx.roster_last_refresh_us.store(now, std::memory_order_relaxed);

    ns::RosterEntry built[HID_PORT_COUNT]{};
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        std::lock_guard<std::mutex> lk(g_ctx.mtx[i]);
        const ClientSession& c = g_ctx.clients[i];
        if (!c.active) continue;
        for (int s = 0; s < 4; ++s) {
            const uint8_t port = c.client_assignment[s].primary_console_port;
            if (port >= HID_PORT_COUNT) continue;
            if (built[port].present != 1) {
                if (c.source_pads[s].present && c.source_pads[s].name[0] != '\0') {
                    built[port] = c.source_pads[s];
                    built[port].present = 1;
                } else {
                    built[port].present = 1;
                    const char* default_name = (c.source == InputSource::WebSocket) ? "Mobile" : "Controller";
                    std::strncpy(built[port].name, default_name, sizeof(built[port].name) - 1);
                    built[port].name[sizeof(built[port].name) - 1] = '\0';
                }
            }
            const uint8_t mask = c.client_assignment[s].console_port_mask;
            for (int h = 0; h < HID_PORT_COUNT; ++h) {
                if (h != port && (mask & (1 << h))) {
                    built[h].present = 2;
                    built[h].has_gyro = 0;
                    std::memset(built[h].name, 0, sizeof(built[h].name));
                }
            }
        }
    }

    std::lock_guard<std::mutex> lk(g_ctx.roster_mtx);
    bool changed = false;
    for (int h = 0; h < HID_PORT_COUNT; ++h) {
        if (!roster_entry_equal(g_ctx.roster[h], built[h])) { changed = true; break; }
    }
    if (changed) {
        for (int h = 0; h < HID_PORT_COUNT; ++h) g_ctx.roster[h] = built[h];
        return g_ctx.roster_seq.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    return g_ctx.roster_seq.load(std::memory_order_relaxed);
}

uint64_t get_roster_packet(ns::RosterPacket& packet) {
    packet = ns::RosterPacket{};
    std::lock_guard<std::mutex> lk(g_ctx.roster_mtx);
    for (int h = 0; h < HID_PORT_COUNT; ++h) packet.ports[h] = g_ctx.roster[h];
    return g_ctx.roster_seq.load(std::memory_order_relaxed);
}

ns::ClientAssignmentPacket make_server_full_assignment_packet(uint8_t active_clients,
                                                              uint8_t free_virtual_slots,
                                                              bool switch_asleep) {
    ns::ClientAssignmentPacket packet{};
    packet.flags = ns::CLIENT_ASSIGNMENT_FLAG_SERVER_FULL;
    if (switch_asleep) packet.flags |= ns::CLIENT_ASSIGNMENT_FLAG_SWITCH_ASLEEP;
    packet.server_slot = ns::CONTROLLER_PLAYER_INDEX_UNKNOWN;
    packet.subpad = 0;
    packet.console_port_mask = 0;
    packet.primary_console_port = ns::CONTROLLER_CONSOLE_PORT_NONE;
    packet.active_clients = active_clients;
    packet.max_clients = static_cast<uint8_t>(configured_client_capacity());
    packet.free_virtual_slots = free_virtual_slots;
    return packet;
}

ns::ClientAssignmentPacket make_server_profile_unsupported_assignment_packet(uint8_t active_clients,
                                                                              uint8_t free_virtual_slots,
                                                                              bool switch_asleep) {
    ns::ClientAssignmentPacket packet{};
    packet.flags = ns::CLIENT_ASSIGNMENT_FLAG_PROFILE_UNSUPPORTED;
    if (switch_asleep) packet.flags |= ns::CLIENT_ASSIGNMENT_FLAG_SWITCH_ASLEEP;
    packet.server_slot = ns::CONTROLLER_PLAYER_INDEX_UNKNOWN;
    packet.subpad = 0;
    packet.console_port_mask = 0;
    packet.primary_console_port = ns::CONTROLLER_CONSOLE_PORT_NONE;
    packet.active_clients = active_clients;
    packet.max_clients = static_cast<uint8_t>(configured_client_capacity());
    packet.free_virtual_slots = free_virtual_slots;
    return packet;
}

bool any_client_source_active(InputSource source, uint64_t now) {
    if (now == 0) now = now_us();
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        std::lock_guard<std::mutex> lk(g_ctx.mtx[i]);
        const ClientSession& c = g_ctx.clients[i];
        if (c.active && c.source == source && c.last_rx_us != 0 && elapsed_us_saturated(now, c.last_rx_us) <= CLIENT_TIMEOUT_US) return true;
    }
    return false;
}

void repair_future_client_timestamp(ClientSession& c, uint64_t now) {
    if (c.active && (c.last_rx_us == 0 || c.last_rx_us > now)) c.last_rx_us = now;
}


static HIDReport* get_pad_report(ClientSession& c, int subpad) {
    if (subpad == 0) return &c.report.p1;
    if (subpad == 1) return &c.report.p2;
    if (subpad == 2) return &c.report.p3;
    if (subpad == 3) return &c.report.p4;
    return nullptr;
}




void clear_motion(ClientSession& c, int subpad) {
    auto pad = get_pad_report(c, subpad);
    if (!pad) return;
    for (int i = 0; i < 3; ++i) pad->motion[i].reset();
    pad->has_motion = 0;
}

void clear_all_motion(ClientSession& c) { for (int s = 0; s < 4; ++s) clear_motion(c, s); }

void set_motion(ClientSession& c, int subpad, const MotionReport& motion) {
    auto pad = get_pad_report(c, subpad);
    if (!pad) return;
    if (!pad->has_motion) {
        for (int i = 0; i < 3; ++i) pad->motion[i] = motion;
    } else {
        pad->motion[0] = pad->motion[1];
        pad->motion[1] = pad->motion[2];
        pad->motion[2] = motion;
    }
    pad->has_motion = 1;
}

void set_motion_samples(ClientSession& c, int subpad, const MotionReport samples[3]) {
    auto pad = get_pad_report(c, subpad);
    if (!pad || !samples) return;
    pad->motion[0] = samples[0];
    pad->motion[1] = samples[1];
    pad->motion[2] = samples[2];
    pad->has_motion = 1;
}

bool rate_allow(uint32_t ip);
bool server_macro_start(int client_idx, int subpad, const std::string& json_or_commands);

static bool handle_macro_chunk(int client_idx, uint32_t upload_id, uint8_t subpad, ns::macro::UploadKind kind, uint32_t total_len, uint32_t chunk_count, uint32_t chunk_index, std::span<const uint8_t> payload) {
    if (chunk_count == 0 || chunk_count > ns::macro::UDP_CHUNK_COUNT_MAX
            || chunk_index >= chunk_count || total_len > ns::macro::UDP_TEXT_MAX) {
        return true;
    }
    uint64_t now = now_us();
    std::string completed;
    {
        std::lock_guard<std::mutex> lk(g_ctx.server_macro_upload_mtx);
        ServerMacroUploadRuntime& up = g_ctx.server_macro_uploads[client_idx];
        if (!up.active || up.upload_id != upload_id || up.kind != kind) {
            up = ServerMacroUploadRuntime{};
            up.active = true;
            up.upload_id = upload_id;
            up.subpad = (uint8_t)(subpad < 4 ? subpad : 0);
            up.kind = kind;
            up.total_len = total_len;
            up.chunk_count = chunk_count;
            up.chunks.assign(chunk_count, {}); up.got.assign(chunk_count, 0);
        }
        if (up.total_len != total_len || up.chunk_count != chunk_count) return true;
        up.last_rx_us = now;
        if (!up.got[chunk_index]) {
            up.chunks[chunk_index].assign((const char*)payload.data(), payload.size());
            up.got[chunk_index] = 1; up.received_count++;
        }
        if (up.received_count == up.chunk_count) {
            size_t total = 0; for (const auto& c : up.chunks) total += c.size();
            if (total != up.total_len) { up = ServerMacroUploadRuntime{}; return true; }
            completed.reserve(total); for (const auto& c : up.chunks) completed += c;
            subpad = up.subpad; up = ServerMacroUploadRuntime{};
        }
    }
    if (!completed.empty()) {
        if (kind == ns::macro::UploadKind::Macro) {
            if (g_ctx.verbose) std::println("[macro] received chunked macro {} bytes", completed.size());
            server_macro_start(client_idx, subpad, completed);
        }
    }
    return true;
}

int server_macro_client_for_sender(const sockaddr_in& sender) {
    uint64_t now = now_us();
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        std::lock_guard<std::mutex> lk(g_ctx.mtx[i]);
        if (g_ctx.clients[i].active && g_ctx.clients[i].source == InputSource::Udp && g_ctx.clients[i].addr.sin_addr.s_addr == sender.sin_addr.s_addr && g_ctx.clients[i].addr.sin_port == sender.sin_port) {
            g_ctx.clients[i].last_rx_us = now; return i;
        }
    }
    if (active_client_count(now) >= configured_client_capacity() || free_virtual_slot_count(now) <= 0) return -1;
    return allocate_client_session(now, &sender, false, InputSource::Udp);
}

bool server_macro_handle_chunk_packet(std::span<const uint8_t> data, const sockaddr_in& sender) {
    if (data.size() < ns::macro::CHUNK_HEADER_SIZE + HMAC_TAG_SIZE) return false;
    ns::macro::MacroUdpChunkHeaderWire h{};
    std::ranges::copy(data.subspan(0, sizeof(h)), (uint8_t*)&h);
    if (h.magic != ns::macro::UDP_CHUNK_MAGIC) return false;
    if (h.version != PROTO_VERSION || h.total_len > ns::macro::UDP_TEXT_MAX || h.chunk_len > ns::macro::UDP_CHUNK_MAX || h.chunk_count == 0 || h.chunk_count > ns::macro::UDP_CHUNK_COUNT_MAX || h.chunk_index >= h.chunk_count || data.size() != ns::macro::CHUNK_HEADER_SIZE + h.chunk_len + HMAC_TAG_SIZE) return true;
    if (h.reserved != static_cast<uint8_t>(ns::macro::UploadKind::Macro)) return true;
    const auto kind = static_cast<ns::macro::UploadKind>(h.reserved);
    if (hmac_verify({g_ctx.hmac_key, 32}, data.subspan(0, ns::macro::CHUNK_HEADER_SIZE + h.chunk_len), data.subspan(ns::macro::CHUNK_HEADER_SIZE + h.chunk_len, HMAC_TAG_SIZE)) != 0 || !rate_allow(sender.sin_addr.s_addr)) return true;
    int cidx = server_macro_client_for_sender(sender);
    if (cidx >= 0) handle_macro_chunk(cidx, h.upload_id, h.subpad, kind, h.total_len, h.chunk_count, h.chunk_index, data.subspan(ns::macro::CHUNK_HEADER_SIZE, h.chunk_len));
    return true;
}

bool server_macro_handle_ws_chunk_packet(int client_idx, std::span<const uint8_t> data) {
    if (client_idx < 0 || client_idx >= MAX_CLIENTS || data.size() < ns::macro::CHUNK_HEADER_SIZE) return false;
    ns::macro::MacroUdpChunkHeaderWire h{};
    std::ranges::copy(data.subspan(0, sizeof(h)), (uint8_t*)&h);
    if (h.magic != ns::macro::UDP_CHUNK_MAGIC) return false;
    if (h.version != PROTO_VERSION && h.version != WEB_PROTO_VERSION) return true;
    if (h.reserved != static_cast<uint8_t>(ns::macro::UploadKind::Macro)) return true;
    if (h.total_len > ns::macro::UDP_TEXT_MAX || h.chunk_len > ns::macro::UDP_CHUNK_MAX || h.chunk_count == 0 || h.chunk_count > ns::macro::UDP_CHUNK_COUNT_MAX || h.chunk_index >= h.chunk_count || data.size() != ns::macro::CHUNK_HEADER_SIZE + h.chunk_len) return true;
    return handle_macro_chunk(client_idx, h.upload_id, h.subpad, ns::macro::UploadKind::Macro, h.total_len, h.chunk_count, h.chunk_index, data.subspan(ns::macro::CHUNK_HEADER_SIZE, h.chunk_len));
}

bool server_macro_running(int client_idx, int subpad) {
    if (client_idx < 0 || client_idx >= MAX_CLIENTS || subpad < 0 || subpad >= 4) return false;
    std::lock_guard<std::mutex> lk(g_ctx.server_macro_mtx);
    ServerMacroRuntime& rt = g_ctx.server_macros[client_idx][subpad];
    if (!rt.running) return false;
    if ((now_us() - rt.start_us) / 1000ULL > ns::macro::total_ms(rt.steps) + 120) { rt.running = false; return false; }
    return true;
}

void server_macro_apply(int client_idx, int subpad, HoriHIDReport& live) {
    if (client_idx < 0 || client_idx >= MAX_CLIENTS || subpad < 0 || subpad >= 4) return;
    std::lock_guard<std::mutex> lk(g_ctx.server_macro_mtx);
    ServerMacroRuntime& rt = g_ctx.server_macros[client_idx][subpad];
    if (!rt.running) return;
    ns::macro::Step step{};
    if (!ns::macro::step_at(rt.steps, (now_us() - rt.start_us) / 1000ULL, step)) { rt.running = false; return; }
    live.buttons |= step.buttons;
    live.vendor |= step.extra_buttons;
    if (step.hat != HAT_NEUTRAL && live.hat == HAT_NEUTRAL) live.hat = step.hat;
    if (step.has_lstick) { live.lx = step.lx; live.ly = step.ly; }
    if (step.has_rstick) { live.rx = step.rx; live.ry = step.ry; }
}

bool server_macro_start(int client_idx, int subpad, const std::string& json_or_commands) {
    if (client_idx < 0 || client_idx >= MAX_CLIENTS) return false;
    if (subpad < 0 || subpad >= 4) subpad = 0;
    std::vector<ns::macro::Step> steps;
    if (!ns::macro::validate_text(json_or_commands, steps, nullptr)) {
        if (g_ctx.verbose) std::println("[macro] rejected: {}", ns::macro::last_error());
        return false;
    }
    std::lock_guard<std::mutex> lk(g_ctx.server_macro_mtx);
    ServerMacroRuntime& rt = g_ctx.server_macros[client_idx][subpad];
    rt.steps = std::move(steps); rt.running = true; rt.start_us = now_us();
    if (g_ctx.verbose) std::println("[macro] started server macro slot={} pad={}", client_idx + 1, subpad + 1);
    return true;
}

void server_macro_stop_all_for_client(int client_idx) {
    if (client_idx < 0 || client_idx >= MAX_CLIENTS) return;
    std::lock_guard<std::mutex> lk(g_ctx.server_macro_mtx);
    for (int s = 0; s < 4; ++s) g_ctx.server_macros[client_idx][s].running = false;
}

// Reset the per-pad rumble/controller-status/presence state for all 4 sub-pads.
// Shared by session reset and allocation; callers must already hold g_ctx.mtx[idx].
static void reset_client_slot_streams_locked(ClientSession& c) {
    for (int s = 0; s < 4; ++s) {
        c.rumble[s] = RumblePacket{}; c.precision_rumble[s] = PrecisionRumblePacket{};
        c.rumble_active[s] = false; c.rumble_seq[s]++;
        c.controller_status[s] = ControllerStatusState{};
        c.controller_status_seq[s]++;
        c.client_assignment[s] = ClientAssignmentState{};
        c.client_assignment_seq[s]++;
        c.udp_last_rumble_seq[s] = c.rumble_seq[s];
        c.udp_last_controller_status_seq[s] = c.controller_status_seq[s];
        c.udp_last_client_assignment_seq[s] = 0;
        c.pad_present[s] = false; c.pad_last_present_us[s] = 0;
        c.amiibo_request_pending[s] = false;
        c.amiibo_requested[s] = false;
        c.amiibo_request_repeats[s] = 0;
        c.amiibo_request_seq[s] = 0;
        c.amiibo_writeback_pending[s] = false;
        c.amiibo_writeback_len[s] = 0;
    }
    c.udp_last_server_state_seq = 0;
}

void reset_client_session_locked(ClientSession& c) {
    c.active = false; c.source = InputSource::None; c.first_pkt = true; c.expected_seq = 0; c.last_rx_us = 0;
    c.report.reset(); c.has_new_report = false;
    clear_all_motion(c);
    c.uses_pad_presence = c.udp_rumble_enabled = false;
    for (int s = 0; s < 4; ++s) c.source_pads[s] = ns::RosterEntry{};
    c.udp_last_roster_seq = 0;
    c.udp_last_roster_send_us = 0;
    reset_client_slot_streams_locked(c);
}

void reset_client_session(int client_idx) {
    if (client_idx < 0 || client_idx >= MAX_CLIENTS) return;
    {
        std::lock_guard<std::mutex> lk(g_ctx.mtx[client_idx]);
        reset_client_session_locked(g_ctx.clients[client_idx]);
    }
    server_macro_stop_all_for_client(client_idx);
}

bool reset_client_session_if_source(int client_idx, InputSource source) {
    if (client_idx < 0 || client_idx >= MAX_CLIENTS) return false;
    bool reset = false;
    {
        std::lock_guard<std::mutex> lk(g_ctx.mtx[client_idx]);
        ClientSession& c = g_ctx.clients[client_idx];
        if (c.source == source) {
            reset_client_session_locked(c);
            reset = true;
        }
    }
    if (reset) {
        server_macro_stop_all_for_client(client_idx);
    }
    return reset;
}

bool client_session_is_source(int client_idx, InputSource source) {
    if (client_idx < 0 || client_idx >= MAX_CLIENTS) return false;
    std::lock_guard<std::mutex> lk(g_ctx.mtx[client_idx]);
    return g_ctx.clients[client_idx].active && g_ctx.clients[client_idx].source == source;
}

int allocate_client_session(uint64_t now, const sockaddr_in* addr, bool uses_pad_presence,
                            InputSource source, int preferred_client_idx) {
    const int capacity = configured_client_capacity();
    auto try_slot = [&](int i) -> bool {
        if (i < 0 || i >= capacity) return false;

        std::lock_guard<std::mutex> lk(g_ctx.mtx[i]);
        repair_future_client_timestamp(g_ctx.clients[i], now);
        if (g_ctx.clients[i].active && !elapsed_us_over(now, g_ctx.clients[i].last_rx_us, CLIENT_TIMEOUT_US)) {
            return false;
        }

        g_ctx.clients[i].active = true;
        g_ctx.clients[i].source = source;
        g_ctx.clients[i].first_pkt = true;
        g_ctx.clients[i].expected_seq = 0;
        g_ctx.clients[i].last_rx_us = now;
        g_ctx.clients[i].addr = addr ? *addr : sockaddr_in{};
        g_ctx.clients[i].report.reset();
        clear_all_motion(g_ctx.clients[i]);
        g_ctx.clients[i].uses_pad_presence = uses_pad_presence;
        g_ctx.clients[i].udp_rumble_enabled = false;
        reset_client_slot_streams_locked(g_ctx.clients[i]);
        return true;
    };

    if (source == InputSource::Bluetooth && preferred_client_idx >= 0 && preferred_client_idx < capacity) {
        if (try_slot(preferred_client_idx)) return preferred_client_idx;
    }

    for (int i = 0; i < capacity; ++i) {
        if (i == preferred_client_idx) continue;
        if (try_slot(i)) return i;
    }
    return -1;
}

bool input_is_neutral(const HoriHIDReport& r) {
    return r.buttons == 0 && r.hat == HAT_NEUTRAL && r.lx == 128 && r.ly == 128 && r.rx == 128 && r.ry == 128;
}

bool motion_is_neutral(const MotionReport& m) {
    return std::abs((int)m.ax) < 64 && std::abs((int)m.ay) < 64 && std::abs((int)m.az) < 64 &&
           std::abs((int)m.gx) < 64 && std::abs((int)m.gy) < 64 && std::abs((int)m.gz) < 64;
}

bool hid_is_neutral(const HIDReport& r) {
    return input_is_neutral(r.input) && (!r.has_motion || (motion_is_neutral(r.motion[0]) && motion_is_neutral(r.motion[1]) && motion_is_neutral(r.motion[2])));
}

bool multi_report_has_real_input(const MultiReport& report, const bool pad_present[4], bool uses_pad_presence) {
    const HIDReport* pads[4] = {&report.p1, &report.p2, &report.p3, &report.p4};
    for (int i = 0; i < 4; ++i) {
        if (uses_pad_presence && pad_present && !pad_present[i]) continue;
        if (!hid_is_neutral(*pads[i])) return true;
    }
    return false;
}

bool parse_client_packet(const uint8_t* data, size_t len,
                         uint8_t& flags, uint32_t& seq,
                         ns::MultiReport& report,
                         bool pad_present[4]) {
    if (len < 20) return false;
    uint32_t magic; std::memcpy(&magic, data, 4);
    if (magic != PROTO_MAGIC) return false;

    uint8_t ver = data[4]; flags = data[5];
    std::memcpy(&seq, data + 8, 4);
    report.reset();
    std::fill(pad_present, pad_present + 4, false);

    if ((ver == WEB_PROTO_VERSION || ver == WEB_PROTO_VERSION_3) && (len == WEB_PACKET_SIZE || len == PACKET_SIZE)) {
        std::memcpy(&report, data + 20, sizeof(ns::MultiReport));
        for (int s = 0; s < 4; ++s) {
            pad_present[s] = (data[20 + s * sizeof(ns::HIDReport) + 7] & 0x01) != 0;
        }
        if (flags & FLAG_SINGLE_PAD) {
            report.p2.reset(); report.p3.reset(); report.p4.reset();
            pad_present[0] = true; pad_present[1] = pad_present[2] = pad_present[3] = false;
        }
        return true;
    }
    return false;
}
