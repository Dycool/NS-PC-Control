#include "bluetooth_input.hpp"
#include "app_state.hpp"
#include "gadget_wakeup.hpp"
#include "virtual_controller.hpp"

#include <atomic>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <print>
#include <cstdlib>
#include <csignal>
#include <sys/wait.h>
#include <string>
#include <thread>
#include <unistd.h>

#ifdef NS_ENABLE_SDL_BT
#include "shared/sdl_input.hpp"
#include <algorithm>
#include <array>
#endif

static std::atomic<bool> g_bt_pair_window_started{false};
static std::thread g_bt_pair_window_thread;
static std::atomic<bool> g_bt_pair_window_stop{false};

static void run_pairing_window_script(const char* script) {
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        setpgid(0, 0);
        execl("/bin/sh", "sh", "-c", script, (char*)nullptr);
        _exit(127);
    }
    int status = 0;
    while (g_ctx.running.load(std::memory_order_relaxed) && !g_bt_pair_window_stop.load(std::memory_order_relaxed)) {
        if (waitpid(pid, &status, WNOHANG) == pid) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    kill(-pid, SIGTERM);
    for (int wait_i = 0; wait_i < 5 && waitpid(pid, &status, WNOHANG) != pid; ++wait_i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    kill(-pid, SIGKILL);
    waitpid(pid, &status, 0);
}

static void disconnect_connected_bluetooth_gamepads() {
    const char* script = R"BT(
set +e
bt_timeout() {
    secs="$1"
    shift
    if command -v timeout >/dev/null 2>&1; then
        timeout "$secs" "$@"
    else
        "$@"
    fi
}

is_gamepad_name() {
    case "$1" in
        *Wireless*|*Xbox*|*Pro*|*Nintendo*|*Joy-Con*|*8BitDo*|*DualSense*|*DualShock*|*PLAYSTATION*) return 0 ;;
        *) return 1 ;;
    esac
}

bluetoothctl devices 2>/dev/null | while read -r tag mac name; do
    [ "$tag" = "Device" ] || continue
    is_gamepad_name "$name" || continue
    info="$(bt_timeout 2s bluetoothctl info "$mac" 2>/dev/null || true)"
    echo "$info" | grep -q 'Connected: yes' || continue
    echo "[bt] disconnecting $name ($mac) because Switch suspended"
    bt_timeout 3s bluetoothctl disconnect "$mac" >/dev/null 2>&1 || true
done
)BT";
    int rc = std::system(script);
    (void)rc;
}

static void start_bluetooth_reconnect_helper() {
    bool expected = false;
    if (!g_bt_pair_window_started.compare_exchange_strong(expected, true)) return;
    if (g_bt_pair_window_thread.joinable()) g_bt_pair_window_thread.join();
    g_bt_pair_window_stop.store(false, std::memory_order_relaxed);

    const char* script = g_ctx.bluetooth_pairing_enabled ? R"BT(
set +e
PAIR_WINDOW=1

bt_timeout() {
    secs="$1"
    shift
    if command -v timeout >/dev/null 2>&1; then
        timeout "$secs" "$@"
    else
        "$@"
    fi
}

bt_quiet() {
    secs="$1"
    shift
    bt_timeout "$secs" "$@" >/dev/null 2>&1
}

is_gamepad_name() {
    case "$1" in
        *Wireless*|*Xbox*|*Pro*|*Nintendo*|*Joy-Con*|*8BitDo*|*DualSense*|*DualShock*|*PLAYSTATION*|*Controller*|*Gamepad*) return 0 ;;
        *) return 1 ;;
    esac
}

cleanup() {
    [ -n "$scan_pid" ] && kill "$scan_pid" 2>/dev/null || true
    [ -n "$agent_pid" ] && kill "$agent_pid" 2>/dev/null || true
    bt_quiet 4s bluetoothctl scan off
    bt_quiet 4s bluetoothctl discoverable off
    bt_quiet 4s bluetoothctl pairable off
}
trap cleanup INT TERM EXIT

bt_quiet 4s bluetoothctl power on
# Keep the agent process alive. A one-shot `bluetoothctl agent ...` exits and unregisters the agent.
(printf "agent NoInputNoOutput\ndefault-agent\n"; while :; do sleep 3600; done) | bluetoothctl >/dev/null 2>&1 & agent_pid=$!

printf '[bt] pairing window open for 2 minutes; trusted controllers may reconnect anytime\n'
bt_quiet 4s bluetoothctl pairable on
bt_quiet 4s bluetoothctl discoverable on
(printf "scan on\n"; sleep 120; printf "scan off\n"; sleep 1) | bluetoothctl >/dev/null 2>&1 & scan_pid=$!
pair_end=$(( $(date +%s) + 120 ))
pair_closed=0

while :; do
    now="$(date +%s)"
    if [ "$pair_closed" = "0" ] && [ "$now" -ge "$pair_end" ]; then
        pair_closed=1
        bt_quiet 4s bluetoothctl pairable off
        bt_quiet 4s bluetoothctl discoverable off
        printf '[bt] pairing window closed; trusted reconnect helper stays active\n'
    fi

    bluetoothctl devices 2>/dev/null | while read -r tag mac name; do
        [ "$tag" = "Device" ] || continue
        is_gamepad_name "$name" || continue

        info="$(bt_timeout 3s bluetoothctl info "$mac" 2>/dev/null || true)"
        if printf '%s\n' "$info" | grep -q 'Connected: yes'; then
            # BlueZ can occasionally keep a controller stuck as Connected while the HID service
            # is not resolved anymore. Kick only that broken state; healthy connected devices are left alone.
            if printf '%s\n' "$info" | grep -q 'ServicesResolved: no'; then
                printf '[bt] stale connected state for %s (%s), reconnecting\n' "$name" "$mac"
                bt_quiet 4s bluetoothctl disconnect "$mac"
                sleep 1
                bt_quiet 5s bluetoothctl connect "$mac"
            fi
            continue
        fi

        if printf '%s\n' "$info" | grep -q 'Paired: yes\|Trusted: yes'; then
            bt_quiet 4s bluetoothctl trust "$mac"
            if bt_quiet 5s bluetoothctl connect "$mac"; then
                printf '[bt] reconnected %s (%s)\n' "$name" "$mac"
            fi
            continue
        fi

        # Only pair new devices during the startup window, and only if they were seen in this scan.
        [ "$pair_closed" = "0" ] || continue
        printf '%s\n' "$info" | grep -q 'RSSI:' || continue
        if bt_quiet 10s bluetoothctl pair "$mac"; then
            bt_quiet 4s bluetoothctl trust "$mac"
            bt_quiet 8s bluetoothctl connect "$mac"
        fi
    done
    sleep 5
done
)BT" : R"BT(
set +e
PAIR_WINDOW=0

bt_timeout() {
    secs="$1"
    shift
    if command -v timeout >/dev/null 2>&1; then
        timeout "$secs" "$@"
    else
        "$@"
    fi
}

bt_quiet() {
    secs="$1"
    shift
    bt_timeout "$secs" "$@" >/dev/null 2>&1
}

is_gamepad_name() {
    case "$1" in
        *Wireless*|*Xbox*|*Pro*|*Nintendo*|*Joy-Con*|*8BitDo*|*DualSense*|*DualShock*|*PLAYSTATION*|*Controller*|*Gamepad*) return 0 ;;
        *) return 1 ;;
    esac
}

cleanup() {
    [ -n "$agent_pid" ] && kill "$agent_pid" 2>/dev/null || true
}
trap cleanup INT TERM EXIT

bt_quiet 4s bluetoothctl power on
# Keep the agent process alive. A one-shot `bluetoothctl agent ...` exits and unregisters the agent.
(printf "agent NoInputNoOutput\ndefault-agent\n"; while :; do sleep 3600; done) | bluetoothctl >/dev/null 2>&1 & agent_pid=$!
printf '[bt] trusted-controller reconnect helper active\n'

while :; do
    bluetoothctl devices 2>/dev/null | while read -r tag mac name; do
        [ "$tag" = "Device" ] || continue
        is_gamepad_name "$name" || continue

        info="$(bt_timeout 3s bluetoothctl info "$mac" 2>/dev/null || true)"
        if printf '%s\n' "$info" | grep -q 'Connected: yes'; then
            # BlueZ can occasionally keep a controller stuck as Connected while the HID service
            # is not resolved anymore. Kick only that broken state; healthy connected devices are left alone.
            if printf '%s\n' "$info" | grep -q 'ServicesResolved: no'; then
                printf '[bt] stale connected state for %s (%s), reconnecting\n' "$name" "$mac"
                bt_quiet 4s bluetoothctl disconnect "$mac"
                sleep 1
                bt_quiet 5s bluetoothctl connect "$mac"
            fi
            continue
        fi

        if printf '%s\n' "$info" | grep -q 'Paired: yes\|Trusted: yes'; then
            bt_quiet 4s bluetoothctl trust "$mac"
            if bt_quiet 5s bluetoothctl connect "$mac"; then
                printf '[bt] reconnected %s (%s)\n' "$name" "$mac"
            fi
        fi
    done
    sleep 5
done
)BT";

    g_bt_pair_window_thread = std::thread([script] {
        run_pairing_window_script(script);
        g_bt_pair_window_started.store(false, std::memory_order_relaxed);
        std::println("[bt] reconnect helper stopped");
    });
}

using namespace ns;

#ifdef NS_ENABLE_SDL_BT
bool bluetooth_input_available() { return true; }

static bool bt_contains_case_insensitive(const std::string& haystack, const char* needle) {
    if (!needle || !*needle) return false;
    std::string h;
    std::string n;
    h.reserve(haystack.size());
    for (unsigned char c : haystack) h.push_back(static_cast<char>(std::tolower(c)));
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(needle); *p; ++p) {
        n.push_back(static_cast<char>(std::tolower(*p)));
    }
    return h.find(n) != std::string::npos;
}

static bool bt_is_playstation_controller(const SdlPadState& pad) {
    return pad.vid == 0x054c ||
           bt_contains_case_insensitive(pad.name, "playstation") ||
           bt_contains_case_insensitive(pad.name, "dualsense") ||
           bt_contains_case_insensitive(pad.name, "dualshock") ||
           bt_contains_case_insensitive(pad.name, "wireless controller") ||
           bt_contains_case_insensitive(pad.name, "wirless controller");
}

static bool publish_bluetooth_state_to_client(int client_idx, const SdlPadState& pad, uint64_t now) {
    std::lock_guard<std::mutex> lk(g_ctx.mtx[client_idx]);
    ClientSession& c = g_ctx.clients[client_idx];
    if (!c.active || c.source != InputSource::Bluetooth) return false;
    c.uses_pad_presence = true;
    c.udp_rumble_enabled = false;
    c.last_rx_us = now;
    c.report.reset();
    c.report.p1.input = pad.input;
    if (pad.battery_percent >= 0 && pad.battery_percent <= 100) {
        c.report.p1.reserved[0] = static_cast<uint8_t>(pad.battery_percent);
        c.report.p1.reserved[1] |= EXT_STATUS_BATTERY_VALID;
        if (pad.battery_charging) c.report.p1.reserved[1] |= EXT_STATUS_BATTERY_CHARGING;
    }
    c.report.p1.has_motion = pad.has_motion ? 1 : 0;
    if (pad.has_motion) {
        c.report.p1.motion[0] = pad.motion_samples[0];
        c.report.p1.motion[1] = pad.motion_samples[1];
        c.report.p1.motion[2] = pad.motion_samples[2];
    } else {
        for (int j = 0; j < 3; ++j) c.report.p1.motion[j].reset();
    }
    c.pad_present[0] = true;
    c.pad_last_present_us[0] = now;
    for (int s = 1; s < 4; ++s) {
        c.pad_present[s] = false;
        c.pad_last_present_us[s] = 0;
        clear_motion(c, s);
    }
    return true;
}

static void apply_bluetooth_rumble(SDLInputManager& input, int sdl_slot, int client_idx, uint32_t& last_seq, uint64_t& rumble_until_us) {
    RumblePacket ev{};
    uint32_t seq = 0;
    {
        std::lock_guard<std::mutex> lk(g_ctx.mtx[client_idx]);
        if (!g_ctx.clients[client_idx].active || g_ctx.clients[client_idx].source != InputSource::Bluetooth) return;
        seq = g_ctx.clients[client_idx].rumble_seq[0];
        if (seq == last_seq) {
            if (rumble_until_us && now_us() > rumble_until_us) {
                rumble_until_us = 0;
                input.set_rumble(sdl_slot, 0, 0, 0);
            }
            return;
        }
        ev = g_ctx.clients[client_idx].rumble[0];
    }
    last_seq = seq;
    bool neutral = (ev.low_freq == 0 && ev.high_freq == 0) || ev.duration_10ms == 0;
    uint32_t duration_ms = neutral ? 0 : std::max<uint32_t>(RUMBLE_BT_MIN_DURATION_MS, (uint32_t)ev.duration_10ms * 10U);
    rumble_until_us = neutral ? 0 : now_us() + (uint64_t)duration_ms * 1000ULL;
    input.set_rumble(sdl_slot, neutral ? 0 : ev.low_freq, neutral ? 0 : ev.high_freq, duration_ms);
}

static void apply_bluetooth_controller_status(SDLInputManager& input, int sdl_slot, int client_idx,
                                              uint32_t& last_seq, uint64_t& last_apply_us) {
    uint32_t seq = 0;
    ControllerStatusPacket sp{};
    if (!get_controller_status_packet(client_idx, 0, seq, sp)) return;
    const uint64_t now = now_us();
    const bool periodic_refresh = (last_apply_us == 0 || now - last_apply_us >= 2'000'000ULL);
    if (seq == last_seq && !periodic_refresh) return;
    last_seq = seq;
    last_apply_us = now;
    int player_index = (sp.player_index < 4) ? static_cast<int>(sp.player_index) : -1;
    input.set_player_status(sdl_slot, player_index, sp.player_leds, nullptr);
}

void bluetooth_input_thread(std::stop_token stoken) {
    SDLInputManager input;
    input.set_motion_enabled(true);
    input.set_home_shortcut_enabled(true);
    input.set_capture_shortcut_enabled(true);
    input.set_connection_callback([](int slot, bool connected) {
        if (slot < 0 || slot >= MAX_CLIENTS) return;
        const uint8_t bit = static_cast<uint8_t>(1u << slot);
        uint8_t old_mask = g_ctx.bluetooth_reserved_client_slots_mask.load(std::memory_order_relaxed);
        uint8_t new_mask = 0;
        do {
            new_mask = connected ? static_cast<uint8_t>(old_mask | bit)
                                 : static_cast<uint8_t>(old_mask & static_cast<uint8_t>(~bit));
        } while (!g_ctx.bluetooth_reserved_client_slots_mask.compare_exchange_weak(
            old_mask, new_mask, std::memory_order_relaxed, std::memory_order_relaxed));
    });
    if (!input.start()) {
        std::println(stderr, "[bt] SDL3 input failed: {}", input.error());
        return;
    }
    start_bluetooth_reconnect_helper();

    std::array<int, 4> client_for_sdl;
    client_for_sdl.fill(-1);
    std::array<uint32_t, 4> last_rumble_seq{};
    std::array<uint32_t, 4> last_status_seq{};
    std::array<uint64_t, 4> last_status_apply_us{};
    std::array<uint64_t, 4> rumble_until_us{};
    std::array<uint64_t, 4> last_live_input_or_motion_us{};
    std::array<bool, 4> motion_seen{};
    std::array<bool, 4> dormant_until_input{};
    uint64_t seen_sleep_seq = g_ctx.switch2_sleep_seq.load(std::memory_order_relaxed);
    bool waiting_logged = false;
    std::println("[bt] Bluetooth/local controller input enabled");

    while (!stoken.stop_requested()) {
        input.poll();
        auto pads = input.snapshot();
        uint64_t now = now_us();
        bool any_waiting = false;
        const bool switch_sleeping = switch2_sleep_confirmed(now);
        const uint64_t sleep_seq = g_ctx.switch2_sleep_seq.load(std::memory_order_relaxed);
        const bool new_sleep_transition = switch_sleeping && sleep_seq != seen_sleep_seq;
        if (new_sleep_transition) {
            seen_sleep_seq = sleep_seq;
            input.stop_all_rumble();
            for (int i = 0; i < 4; ++i) {
                if (client_for_sdl[i] >= 0) reset_client_session_if_source(client_for_sdl[i], InputSource::Bluetooth);
                client_for_sdl[i] = -1;
                last_rumble_seq[i] = 0;
                last_status_seq[i] = 0;
                last_status_apply_us[i] = 0;
                rumble_until_us[i] = 0;
                last_live_input_or_motion_us[i] = 0;
                motion_seen[i] = false;
                input.clear_player_status(i);
            }
            dormant_until_input.fill(true);
            g_ctx.bluetooth_reserved_client_slots_mask.store(0, std::memory_order_relaxed);
            input.disconnect_all();
            disconnect_connected_bluetooth_gamepads();
            if (g_ctx.verbose) std::println("[bt] Switch suspended; local Bluetooth controllers were disconnected");
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }
        if (!switch_sleeping) dormant_until_input.fill(false);

        uint8_t bt_reserved_mask = 0;
        for (int i = 0; i < 4; ++i) {
            if (pads[i].connected) bt_reserved_mask |= static_cast<uint8_t>(1u << i);
        }
        g_ctx.bluetooth_reserved_client_slots_mask.store(bt_reserved_mask, std::memory_order_relaxed);

        for (int i = 0; i < 4; ++i) {
            if (!pads[i].connected) {
                if (client_for_sdl[i] >= 0) {
                    input.set_rumble(i, 0, 0, 0);
                    reset_client_session_if_source(client_for_sdl[i], InputSource::Bluetooth);
                    client_for_sdl[i] = -1;
                    last_rumble_seq[i] = 0;
                    last_status_seq[i] = 0;
                    last_status_apply_us[i] = 0;
                    rumble_until_us[i] = 0;
                    last_live_input_or_motion_us[i] = 0;
                    motion_seen[i] = false;
                    input.clear_player_status(i);
                }
                if (!switch_sleeping) dormant_until_input[i] = false;
                last_live_input_or_motion_us[i] = 0;
                motion_seen[i] = false;
                continue;
            }

            if (client_for_sdl[i] >= 0) {
                bool still_active = false;
                {
                    std::lock_guard<std::mutex> lk(g_ctx.mtx[client_for_sdl[i]]);
                    still_active = g_ctx.clients[client_for_sdl[i]].active && g_ctx.clients[client_for_sdl[i]].source == InputSource::Bluetooth;
                }
                if (!still_active) {
                    client_for_sdl[i] = -1;
                    last_rumble_seq[i] = 0;
                    last_status_seq[i] = 0;
                    last_status_apply_us[i] = 0;
                    rumble_until_us[i] = 0;
                    last_live_input_or_motion_us[i] = 0;
                    motion_seen[i] = false;
                    input.clear_player_status(i);
                }
            }

            const bool real_bt_input = !input_is_neutral(pads[i].input);
            if (pads[i].has_motion) motion_seen[i] = true;
            if (last_live_input_or_motion_us[i] == 0 || real_bt_input || pads[i].has_motion) {
                last_live_input_or_motion_us[i] = now;
            }

            // RPi/BlueZ can leave a DualShock/DualSense in a zombie state: SDL still says
            // connected, but motion/input stops and BlueZ also still says Connected. For PS pads
            // that previously delivered motion, no motion for a few seconds is a good stale-link
            // signal. Do not apply this to Xbox/default controllers, because idle pads can be
            // completely silent there.
            if (client_for_sdl[i] >= 0 && motion_seen[i] && bt_is_playstation_controller(pads[i]) &&
                last_live_input_or_motion_us[i] != 0 && now - last_live_input_or_motion_us[i] > 5'000'000ULL) {
                if (g_ctx.verbose) {
                    std::println("[bt] {} appears connected but stopped sending input/motion; forcing reconnect", pads[i].name);
                }
                reset_client_session_if_source(client_for_sdl[i], InputSource::Bluetooth);
                client_for_sdl[i] = -1;
                last_rumble_seq[i] = 0;
                last_status_seq[i] = 0;
                last_status_apply_us[i] = 0;
                rumble_until_us[i] = 0;
                last_live_input_or_motion_us[i] = 0;
                motion_seen[i] = false;
                input.clear_player_status(i);
                g_ctx.bluetooth_reserved_client_slots_mask.store(0, std::memory_order_relaxed);
                input.disconnect_all();
                disconnect_connected_bluetooth_gamepads();
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                continue;
            }

            if (client_for_sdl[i] < 0) {
                if (dormant_until_input[i] && switch_sleeping && !real_bt_input) {
                    continue;
                }
                client_for_sdl[i] = allocate_client_session(now, nullptr, true, InputSource::Bluetooth, i);
                if (client_for_sdl[i] >= 0) {
                    waiting_logged = false;
                    dormant_until_input[i] = false;
                    input.set_rumble(i, 0, 0, 0);
                    rumble_until_us[i] = 0;
                    {
                        std::lock_guard<std::mutex> lk(g_ctx.mtx[client_for_sdl[i]]);
                        last_rumble_seq[i] = g_ctx.clients[client_for_sdl[i]].rumble_seq[0];
                        last_status_seq[i] = g_ctx.clients[client_for_sdl[i]].controller_status_seq[0];
                        last_status_apply_us[i] = 0;
                    }
                    maybe_send_switch2_wake_advert("Bluetooth controller connected");
                }
            }

            if (client_for_sdl[i] < 0) {
                any_waiting = true;
                continue;
            }

            if (!publish_bluetooth_state_to_client(client_for_sdl[i], pads[i], now)) {
                client_for_sdl[i] = -1;
                last_rumble_seq[i] = 0;
                last_status_seq[i] = 0;
                last_status_apply_us[i] = 0;
                rumble_until_us[i] = 0;
                last_live_input_or_motion_us[i] = 0;
                motion_seen[i] = false;
                input.clear_player_status(i);
                continue;
            }
            apply_bluetooth_rumble(input, i, client_for_sdl[i], last_rumble_seq[i], rumble_until_us[i]);
            apply_bluetooth_controller_status(input, i, client_for_sdl[i], last_status_seq[i], last_status_apply_us[i]);
        }

        if (any_waiting && !waiting_logged) {
            std::println("[bt] controller connected, but all server slots are in use");
            waiting_logged = true;
        } else if (!any_waiting) {
            waiting_logged = false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(PRO_UDP_INTERVAL_MS));
    }

    g_ctx.bluetooth_reserved_client_slots_mask.store(0, std::memory_order_relaxed);
    for (int i = 0; i < 4; ++i) {
        input.set_rumble(i, 0, 0, 0);
        if (client_for_sdl[i] >= 0) reset_client_session_if_source(client_for_sdl[i], InputSource::Bluetooth);
    }
    g_bt_pair_window_stop.store(true, std::memory_order_relaxed);
    if (g_bt_pair_window_thread.joinable()) g_bt_pair_window_thread.join();
    input.stop();
    std::println("[bt] Bluetooth/local SDL controller input stopped");
}
#else
bool bluetooth_input_available() { return false; }
void bluetooth_input_thread(std::stop_token) {
    std::println(stderr, "[bt] built without SDL3 Bluetooth controller support");
}
#endif
