#include "bluetooth_input.hpp"
#include "app_state.hpp"
#include "gadget_wakeup.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <print>
#include <cstdlib>
#include <csignal>
#include <sys/wait.h>
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

static void start_bluetooth_pairing_window() {
    bool expected = false;
    if (!g_bt_pair_window_started.compare_exchange_strong(expected, true)) return;
    if (g_bt_pair_window_thread.joinable()) g_bt_pair_window_thread.join();
    g_bt_pair_window_stop.store(false, std::memory_order_relaxed);
    g_bt_pair_window_thread = std::thread([] {
        std::println("[bt] pairing window open for 2 minutes");
        run_pairing_window_script(R"BT(
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

bt_quiet() {
    secs="$1"
    shift
    bt_timeout "$secs" "$@" >/dev/null 2>&1
}

is_gamepad_name() {
    case "$1" in
        *Wireless*|*Xbox*|*Pro*|*Nintendo*|*Joy-Con*|*8BitDo*|*DualSense*|*DualShock*|*PLAYSTATION*) return 0 ;;
        *) return 1 ;;
    esac
}

bt_quiet 4s bluetoothctl power on
bt_quiet 4s bluetoothctl pairable on
bt_quiet 4s bluetoothctl discoverable on
(printf "agent NoInputNoOutput\ndefault-agent\n"; sleep 125) | bluetoothctl >/dev/null 2>&1 & agent=$!
(printf "scan on\n"; sleep 120) | bluetoothctl >/dev/null 2>&1 & scan=$!

faildir=/tmp/ns-pc-control-bt-pair-fails
mkdir -p "$faildir" 2>/dev/null || true

for i in $(seq 1 24); do
    sleep 5
    bluetoothctl devices 2>/dev/null | while read -r tag mac name; do
        [ "$tag" = "Device" ] || continue
        is_gamepad_name "$name" || continue

        info="$(bt_timeout 3s bluetoothctl info "$mac" 2>/dev/null || true)"
        printf '%s\n' "$info" | grep -q 'Connected: yes' && { rm -f "$faildir/$(printf '%s' "$mac" | tr ':' '_')" 2>/dev/null; continue; }

        # bluetoothctl devices includes old cached devices. Only repair entries that were seen during
        # this scan window; otherwise an off/out-of-range controller could be deleted by accident.
        printf '%s\n' "$info" | grep -q 'RSSI:' || continue

        safe_mac="$(printf '%s' "$mac" | tr ':' '_')"
        fail_file="$faildir/$safe_mac"

        if printf '%s\n' "$info" | grep -q 'Paired: yes'; then
            if bt_quiet 8s bluetoothctl connect "$mac"; then
                bt_quiet 4s bluetoothctl trust "$mac"
                rm -f "$fail_file" 2>/dev/null
                continue
            fi

            fails=0
            if [ -r "$fail_file" ]; then fails="$(cat "$fail_file" 2>/dev/null || echo 0)"; fi
            case "$fails" in ''|*[!0-9]*) fails=0 ;; esac
            fails=$((fails + 1))
            printf '%s\n' "$fails" > "$fail_file" 2>/dev/null || true

            if [ "$fails" -ge 2 ]; then
                echo "[bt] stale pairing suspected for $name ($mac); removing old BlueZ key and keeping pairing open"
                bt_quiet 5s bluetoothctl remove "$mac"
                rm -f "$fail_file" 2>/dev/null
                sleep 1
                if bt_quiet 10s bluetoothctl pair "$mac"; then
                    bt_quiet 4s bluetoothctl trust "$mac"
                    bt_quiet 8s bluetoothctl connect "$mac"
                fi
            else
                echo "[bt] reconnect failed for $name ($mac); retrying before repairing stale pairing"
            fi
            continue
        fi

        rm -f "$fail_file" 2>/dev/null
        if bt_quiet 10s bluetoothctl pair "$mac"; then
            bt_quiet 4s bluetoothctl trust "$mac"
            bt_quiet 8s bluetoothctl connect "$mac"
        fi
    done
done

kill $scan $agent 2>/dev/null
bt_quiet 4s bluetoothctl discoverable off
)BT");
        g_bt_pair_window_started.store(false, std::memory_order_relaxed);
        std::println("[bt] pairing/reconnect helper stopped");
    });
}

using namespace ns;

#ifdef NS_ENABLE_SDL_BT
bool bluetooth_input_available() { return true; }

static bool publish_bluetooth_state_to_client(int client_idx, const SdlPadState& pad, uint64_t now) {
    std::lock_guard<std::mutex> lk(g_ctx.mtx[client_idx]);
    ClientSession& c = g_ctx.clients[client_idx];
    if (!c.active || c.source != InputSource::Bluetooth) return false;
    c.uses_pad_presence = true;
    c.udp_rumble_enabled = false;
    c.last_rx_us = now;
    c.report.reset();
    c.report.p1.input = pad.input;
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
    if (g_ctx.bluetooth_pairing_enabled) {
        start_bluetooth_pairing_window();
    }

    std::array<int, 4> client_for_sdl;
    client_for_sdl.fill(-1);
    std::array<uint32_t, 4> last_rumble_seq{};
    std::array<uint64_t, 4> rumble_until_us{};
    bool waiting_logged = false;
    std::println("[bt] Bluetooth/local controller input enabled");

    while (!stoken.stop_requested()) {
        input.poll();
        auto pads = input.snapshot();
        uint64_t now = now_us();
        bool any_waiting = false;

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
                    rumble_until_us[i] = 0;
                }
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
                    rumble_until_us[i] = 0;
                }
            }

            if (client_for_sdl[i] < 0) {
                client_for_sdl[i] = allocate_client_session(now, nullptr, true, InputSource::Bluetooth, i);
                if (client_for_sdl[i] >= 0) {
                    waiting_logged = false;
                    input.set_rumble(i, 0, 0, 0);
                    rumble_until_us[i] = 0;
                    {
                        std::lock_guard<std::mutex> lk(g_ctx.mtx[client_for_sdl[i]]);
                        last_rumble_seq[i] = g_ctx.clients[client_for_sdl[i]].rumble_seq[0];
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
                rumble_until_us[i] = 0;
                continue;
            }
            apply_bluetooth_rumble(input, i, client_for_sdl[i], last_rumble_seq[i], rumble_until_us[i]);
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
