#include "bluetooth_input.hpp"
#include "app_state.hpp"

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
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    kill(-pid, SIGTERM);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    kill(-pid, SIGKILL);
    waitpid(pid, &status, 0);
}

static void start_bluetooth_pairing_window() {
    bool expected = false;
    if (!g_bt_pair_window_started.compare_exchange_strong(expected, true)) return;
    g_bt_pair_window_stop.store(false, std::memory_order_relaxed);
    g_bt_pair_window_thread = std::thread([] {
        std::println("[bt] pairing window open for 2 minutes");
        run_pairing_window_script(
            "bluetoothctl power on >/dev/null 2>&1; bluetoothctl pairable on >/dev/null 2>&1; bluetoothctl discoverable on >/dev/null 2>&1; "
            "(printf \"agent NoInputNoOutput\\ndefault-agent\\n\"; sleep 125) | bluetoothctl >/dev/null 2>&1 & agent=$!; "
            "(printf \"scan on\\n\"; sleep 120) | bluetoothctl >/dev/null 2>&1 & scan=$!; "
            "for i in $(seq 1 24); do sleep 5; "
                "bluetoothctl devices | while read -r _ mac name; do "
                    "case \"$name\" in *Wireless*|*Xbox*|*Pro*|*Nintendo*|*Joy-Con*|*8BitDo*) "
                        "bluetoothctl pair \"$mac\" >/dev/null 2>&1; "
                        "bluetoothctl trust \"$mac\" >/dev/null 2>&1; "
                        "bluetoothctl connect \"$mac\" >/dev/null 2>&1;; "
                    "esac; "
                "done; "
            "done; kill $scan $agent 2>/dev/null; bluetoothctl discoverable off >/dev/null 2>&1"
        );
        std::println("[bt] pairing/reconnect helper stopped");
    });
}

static void disconnect_connected_bluetooth_gamepads() {
    int r = std::system("bluetoothctl devices 2>/dev/null | while read -r _ mac name; do "
                        "case \"$name\" in *Wireless*|*Xbox*|*Pro*|*Nintendo*|*Joy-Con*|*8BitDo*) "
                        "bluetoothctl info \"$mac\" 2>/dev/null | grep -q 'Connected: yes' && "
                        "bluetoothctl disconnect \"$mac\" >/dev/null 2>&1;; esac; done");
    (void)r;
}

using namespace ns;

#ifdef NS_ENABLE_SDL_BT
bool bluetooth_input_available() { return true; }

static void publish_bluetooth_state_to_client(int client_idx, const SdlPadState& pad, uint64_t now) {
    std::lock_guard<std::mutex> lk(g_ctx.mtx[client_idx]);
    ClientSession& c = g_ctx.clients[client_idx];
    if (!c.active) return;
    c.uses_pad_presence = true;
    c.udp_rumble_enabled = false;
    c.last_rx_us = now;
    c.report.reset();
    c.report.p1.input = pad.input;
    c.report.p1.has_motion = pad.has_motion ? 1 : 0;
    c.report.p1.motion = pad.has_motion ? pad.motion : ns::MotionReport{};
    c.pad_present[0] = true;
    c.pad_last_present_us[0] = now;
    if (pad.has_motion) set_motion_samples(c, 0, pad.motion_samples);
    else clear_motion(c, 0);
    for (int s = 1; s < 4; ++s) {
        c.pad_present[s] = false;
        c.pad_last_present_us[s] = 0;
        clear_motion(c, s);
    }
}

static void apply_bluetooth_rumble(SDLInputManager& input, int sdl_slot, int client_idx, uint32_t& last_seq, uint64_t& rumble_until_us) {
    RumblePacket ev{};
    uint32_t seq = 0;
    {
        std::lock_guard<std::mutex> lk(g_ctx.mtx[client_idx]);
        if (!g_ctx.clients[client_idx].active) return;
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
    if (!input.start()) {
        std::println(stderr, "[bt] SDL3 input failed: {}", input.error());
        return;
    }
    start_bluetooth_pairing_window();

    std::array<int, 4> client_for_sdl;
    client_for_sdl.fill(-1);
    std::array<uint32_t, 4> last_rumble_seq{};
    std::array<uint64_t, 4> rumble_until_us{};
    bool waiting_logged = false;
    uint64_t seen_suspend_disconnect_seq = g_ctx.switch2_suspend_disconnect_seq.load(std::memory_order_relaxed);
    bool switch_suspend_disconnect_active = false;
    uint64_t last_bt_disconnect_us = 0;

    std::println("[bt] Bluetooth/local controller input enabled");

    while (!stoken.stop_requested()) {
        input.poll();
        auto pads = input.snapshot();
        uint64_t now = now_us();
        bool any_waiting = false;

        uint64_t suspend_seq = g_ctx.switch2_suspend_disconnect_seq.load(std::memory_order_relaxed);
        if (suspend_seq != seen_suspend_disconnect_seq) {
            seen_suspend_disconnect_seq = suspend_seq;
            switch_suspend_disconnect_active = true;
            last_bt_disconnect_us = 0;
            std::println("[bt] Switch USB host suspended/disconnected; disconnecting local Bluetooth controllers");
        }
        if (switch_suspend_disconnect_active && g_ctx.switch2_usb_host_connected.load(std::memory_order_relaxed)) {
            switch_suspend_disconnect_active = false;
        }

        for (int i = 0; i < 4; ++i) {
            if (switch_suspend_disconnect_active) {
                if (pads[i].connected || client_for_sdl[i] >= 0) {
                    input.set_rumble(i, 0, 0, 0);
                    if (client_for_sdl[i] >= 0) reset_client_session(client_for_sdl[i]);
                    client_for_sdl[i] = -1;
                    last_rumble_seq[i] = 0;
                    rumble_until_us[i] = 0;
                }
                continue;
            }

            if (!pads[i].connected) {
                if (client_for_sdl[i] >= 0) {
                    input.set_rumble(i, 0, 0, 0);
                    reset_client_session(client_for_sdl[i]);
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
                    still_active = g_ctx.clients[client_for_sdl[i]].active;
                }
                if (!still_active) {
                    client_for_sdl[i] = -1;
                    last_rumble_seq[i] = 0;
                    rumble_until_us[i] = 0;
                }
            }

            if (client_for_sdl[i] < 0) {
                client_for_sdl[i] = allocate_client_session(now, nullptr, true);
                if (client_for_sdl[i] >= 0) {
                    waiting_logged = false;
                    input.set_rumble(i, 0, 0, 0);
                    rumble_until_us[i] = 0;
                    {
                        std::lock_guard<std::mutex> lk(g_ctx.mtx[client_for_sdl[i]]);
                        last_rumble_seq[i] = g_ctx.clients[client_for_sdl[i]].rumble_seq[0];
                    }
                }
            }

            if (client_for_sdl[i] < 0) {
                any_waiting = true;
                continue;
            }

            publish_bluetooth_state_to_client(client_for_sdl[i], pads[i], now);
            apply_bluetooth_rumble(input, i, client_for_sdl[i], last_rumble_seq[i], rumble_until_us[i]);
        }

        if (switch_suspend_disconnect_active && (last_bt_disconnect_us == 0 || now - last_bt_disconnect_us > 1'000'000ULL)) {
            disconnect_connected_bluetooth_gamepads();
            last_bt_disconnect_us = now;
        }

        if (any_waiting && !waiting_logged) {
            std::println("[bt] controller connected, but all server slots are in use");
            waiting_logged = true;
        } else if (!any_waiting) {
            waiting_logged = false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(PRO_UDP_INTERVAL_MS));
    }

    for (int i = 0; i < 4; ++i) {
        input.set_rumble(i, 0, 0, 0);
        if (client_for_sdl[i] >= 0) reset_client_session(client_for_sdl[i]);
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
