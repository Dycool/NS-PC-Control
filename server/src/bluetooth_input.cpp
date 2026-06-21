#include "bluetooth_input.hpp"
#include "app_state.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
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

static void run_pairing_window_script(const char* script) {
    pid_t pid = fork();
    if (pid < 0) {
        std::perror("[bt] fork pairing helper");
        return;
    }

    if (pid == 0) {
        setpgid(0, 0);
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        execl("/bin/sh", "sh", "-c", script, (char*)nullptr);
        _exit(127);
    }

    setpgid(pid, pid);
    int status = 0;
    while (g_running.load(std::memory_order_relaxed)) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid)
            return;
        if (r < 0)
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    kill(-pid, SIGTERM);
    for (int i = 0; i < 20; ++i) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid)
            break;
        if (r < 0)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (waitpid(pid, &status, WNOHANG) == 0) {
        kill(-pid, SIGKILL);
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    }
}

static void start_bluetooth_pairing_window() {
    bool expected = false;
    if (!g_bt_pair_window_started.compare_exchange_strong(expected, true))
        return;

    g_bt_pair_window_thread = std::thread([] {
        std::puts("[bt] pairing window open for 2 minutes");

        // Service-mode friendly: use bluetoothctl non-interactively, avoid TTY
        // assumptions, and keep the pairing/scanning helper separate from SDL's
        // polling loop. Discovery is intentionally finite so BT mode does not
        // scan forever during gameplay. After that, keep nudging already paired
        // controllers so trusted pads can reconnect at any time.
        const char* script =
            "command -v bluetoothctl >/dev/null 2>&1 || exit 0; "
            "bluetoothctl power on >/dev/null 2>&1 || true; "
            "bluetoothctl pairable on >/dev/null 2>&1 || true; "
            "bluetoothctl discoverable on >/dev/null 2>&1 || true; "
            "(printf \"agent NoInputNoOutput\\ndefault-agent\\n\"; sleep 125) | bluetoothctl >/dev/null 2>&1 & agent=$!; "
            "(printf \"scan on\\n\"; sleep 120; printf \"scan off\\n\") | bluetoothctl >/dev/null 2>&1 & scan=$!; "
            "sleep 3; "
            "end=$(( $(date +%s) + 120 )); "
            "while [ $(date +%s) -lt $end ]; do "
                "bluetoothctl devices | while read -r kind mac name_rest; do "
                    "[ \"$kind\" = \"Device\" ] || continue; "
                    "name=\"$name_rest\"; "
                    "case \"$name\" in "
                        "*Wireless\\ Controller*|*Xbox\\ Wireless\\ Controller*|*Xbox\\ One\\ Wireless\\ Controller*|*Pro\\ Controller*|*Nintendo\\ Switch\\ Pro\\ Controller*|*Joy-Con*|*8BitDo*) "
                            "bluetoothctl pair \"$mac\" >/dev/null 2>&1 || true; "
                            "bluetoothctl trust \"$mac\" >/dev/null 2>&1 || true; "
                            "bluetoothctl connect \"$mac\" >/dev/null 2>&1 || true; "
                            ";; "
                    "esac; "
                "done; "
                "sleep 5; "
            "done; "
            "bluetoothctl scan off >/dev/null 2>&1 || true; "
            "kill $scan >/dev/null 2>&1 || true; wait $scan >/dev/null 2>&1 || true; "
            "kill $agent >/dev/null 2>&1 || true; wait $agent >/dev/null 2>&1 || true; "
            "bluetoothctl discoverable off >/dev/null 2>&1 || true; "
            "while true; do "
                "bluetoothctl paired-devices | while read -r kind mac name_rest; do "
                    "[ \"$kind\" = \"Device\" ] || continue; "
                    "name=\"$name_rest\"; "
                    "case \"$name\" in "
                        "*Wireless\\ Controller*|*Xbox\\ Wireless\\ Controller*|*Xbox\\ One\\ Wireless\\ Controller*|*Pro\\ Controller*|*Nintendo\\ Switch\\ Pro\\ Controller*|*Joy-Con*|*8BitDo*) "
                            "bluetoothctl trust \"$mac\" >/dev/null 2>&1 || true; "
                            "bluetoothctl connect \"$mac\" >/dev/null 2>&1 || true; "
                            ";; "
                    "esac; "
                "done; "
                "sleep 10; "
            "done; ";

        run_pairing_window_script(script);
        std::system("bluetoothctl scan off >/dev/null 2>&1 || true; bluetoothctl discoverable off >/dev/null 2>&1 || true");
        std::puts("[bt] pairing/reconnect helper stopped");
    });
}

using namespace ns;

#ifdef NS_ENABLE_SDL_BT
bool bluetooth_input_available() {
    return true;
}

static void clear_bluetooth_slot_rumble_state(ClientSession& c) {
    for (int s = 0; s < 4; ++s) {
        c.rumble[s] = RumblePacket{};
        c.precision_rumble[s] = PrecisionRumblePacket{};
        c.rumble_active[s] = false;
        c.rumble_seq[s]++;
        c.udp_last_rumble_seq[s] = c.rumble_seq[s];
    }
}

static void reset_bluetooth_client_slot(int client_idx) {
    if (client_idx < 0 || client_idx >= MAX_CLIENTS) return;
    std::lock_guard<std::mutex> lk(g_mtx[client_idx]);
    g_clients[client_idx].active = false;
    g_clients[client_idx].first_pkt = true;
    g_clients[client_idx].expected_seq = 0;
    g_clients[client_idx].last_rx_us = 0;
    g_clients[client_idx].report.reset();
    clear_all_motion(g_clients[client_idx]);
    g_clients[client_idx].uses_pad_presence = false;
    g_clients[client_idx].udp_rumble_enabled = false;
    clear_bluetooth_slot_rumble_state(g_clients[client_idx]);
    for (int s = 0; s < 4; ++s) {
        g_clients[client_idx].pad_present[s] = false;
        g_clients[client_idx].pad_last_present_us[s] = 0;
    }
    server_macro_stop_all_for_client(client_idx);
}

static int find_free_bluetooth_client_slot(uint64_t now) {
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        std::lock_guard<std::mutex> lk(g_mtx[i]);
        repair_future_client_timestamp(g_clients[i], now);
        if (!g_clients[i].active || elapsed_us_over(now, g_clients[i].last_rx_us, CLIENT_TIMEOUT_US)) {
            g_clients[i].active = true;
            g_clients[i].first_pkt = true;
            g_clients[i].expected_seq = 0;
            g_clients[i].last_rx_us = now;
            g_clients[i].addr = sockaddr_in{};
            g_clients[i].report.reset();
            clear_all_motion(g_clients[i]);
            g_clients[i].uses_pad_presence = true;
            g_clients[i].udp_rumble_enabled = false;
            clear_bluetooth_slot_rumble_state(g_clients[i]);
            for (int s = 0; s < 4; ++s) {
                g_clients[i].pad_present[s] = false;
                g_clients[i].pad_last_present_us[s] = 0;
            }
            return i;
        }
    }
    return -1;
}

static void publish_bluetooth_state_to_client(int client_idx, const SdlPadState& pad, uint64_t now) {
    std::lock_guard<std::mutex> lk(g_mtx[client_idx]);
    if (!g_clients[client_idx].active)
        return;

    g_clients[client_idx].uses_pad_presence = true;
    g_clients[client_idx].udp_rumble_enabled = false;
    g_clients[client_idx].last_rx_us = now;

    g_clients[client_idx].report.reset();
    g_clients[client_idx].report.p1.input = pad.input;
    g_clients[client_idx].report.p1.has_motion = pad.has_motion ? 1 : 0;
    g_clients[client_idx].report.p1.motion = pad.has_motion ? pad.motion : ns::MotionReport{};
    g_clients[client_idx].pad_present[0] = true;
    g_clients[client_idx].pad_last_present_us[0] = now;

    if (pad.has_motion) {
        set_motion_samples(g_clients[client_idx], 0, pad.motion_samples);
    } else {
        clear_motion(g_clients[client_idx], 0);
    }

    for (int s = 1; s < 4; ++s) {
        g_clients[client_idx].pad_present[s] = false;
        g_clients[client_idx].pad_last_present_us[s] = 0;
        clear_motion(g_clients[client_idx], s);
    }
}

static void apply_bluetooth_rumble(SDLInputManager& input,
                                   int sdl_slot,
                                   int client_idx,
                                   uint32_t& last_seq,
                                   uint64_t& rumble_until_us) {
    if (sdl_slot < 0 || sdl_slot >= 4 || client_idx < 0 || client_idx >= MAX_CLIENTS)
        return;

    RumblePacket ev{};
    uint32_t seq = 0;
    {
        std::lock_guard<std::mutex> lk(g_mtx[client_idx]);
        if (!g_clients[client_idx].active) return;
        seq = g_clients[client_idx].rumble_seq[0];
        if (seq == last_seq) {
            if (rumble_until_us != 0 && now_us() > rumble_until_us) {
                rumble_until_us = 0;
                input.set_rumble(sdl_slot, 0, 0, 0);
            }
            return;
        }
        ev = g_clients[client_idx].rumble[0];
    }

    last_seq = seq;
    const bool neutral = (ev.low_freq == 0 && ev.high_freq == 0) || ev.duration_10ms == 0;
    auto scale_bt_motor = [](uint8_t v) -> uint8_t {
        int scaled = ((int)v * RUMBLE_BT_GAIN_PERCENT) / 100;
        if (scaled == 0 && v != 0) scaled = 1;
        return (uint8_t)std::clamp(scaled, 0, 255);
    };
    const uint32_t duration_ms = neutral ? 0 : std::max<uint32_t>(RUMBLE_BT_MIN_DURATION_MS, (uint32_t)ev.duration_10ms * 10U);
    const uint8_t low = neutral ? 0 : scale_bt_motor(ev.low_freq);
    const uint8_t high = neutral ? 0 : scale_bt_motor(ev.high_freq);
    rumble_until_us = neutral ? 0 : now_us() + (uint64_t)duration_ms * 1000ULL;
    input.set_rumble(sdl_slot, low, high, duration_ms);
}

void bluetooth_input_thread() {
    SDLInputManager input;
    input.set_motion_enabled(true);
    input.set_home_shortcut_enabled(true);
    input.set_capture_shortcut_enabled(true);

    if (!input.start()) {
        std::fprintf(stderr, "[bt] SDL3 input failed: %s\n", input.error().c_str());
        return;
    }

    start_bluetooth_pairing_window();

    std::array<int, 4> client_for_sdl{};
    client_for_sdl.fill(-1);
    std::array<uint32_t, 4> last_rumble_seq{};
    std::array<uint64_t, 4> rumble_until_us{};
    bool waiting_logged = false;

    std::puts("[bt] Bluetooth/local controller input enabled");

    while (g_running.load(std::memory_order_relaxed)) {
        input.poll();
        auto pads = input.snapshot();
        const uint64_t now = now_us();
        bool any_waiting = false;

        for (int i = 0; i < 4; ++i) {
            if (!pads[i].connected) {
                if (client_for_sdl[i] >= 0) {
                    input.set_rumble(i, 0, 0, 0);
                    reset_bluetooth_client_slot(client_for_sdl[i]);
                    if (g_verbose)
                        std::printf("[bt] controller %d disconnected from server slot %d\n", i + 1, client_for_sdl[i] + 1);
                    client_for_sdl[i] = -1;
                    last_rumble_seq[i] = 0;
                    rumble_until_us[i] = 0;
                }
                continue;
            }

            if (client_for_sdl[i] >= 0) {
                bool still_active = false;
                {
                    std::lock_guard<std::mutex> lk(g_mtx[client_for_sdl[i]]);
                    still_active = g_clients[client_for_sdl[i]].active;
                }
                if (!still_active) {
                    client_for_sdl[i] = -1;
                    last_rumble_seq[i] = 0;
                    rumble_until_us[i] = 0;
                }
            }

            if (client_for_sdl[i] < 0) {
                client_for_sdl[i] = find_free_bluetooth_client_slot(now);
                if (client_for_sdl[i] >= 0) {
                    waiting_logged = false;
                    input.set_rumble(i, 0, 0, 0);
                    rumble_until_us[i] = 0;
                    {
                        std::lock_guard<std::mutex> lk(g_mtx[client_for_sdl[i]]);
                        last_rumble_seq[i] = g_clients[client_for_sdl[i]].rumble_seq[0];
                    }
                    if (g_verbose)
                        std::printf("[bt] controller %d (%s) assigned to server slot %d\n",
                                    i + 1,
                                    pads[i].name.empty() ? "SDL3 Gamepad" : pads[i].name.c_str(),
                                    client_for_sdl[i] + 1);
                }
            }

            if (client_for_sdl[i] < 0) {
                any_waiting = true;
                continue;
            }

            publish_bluetooth_state_to_client(client_for_sdl[i], pads[i], now);
            apply_bluetooth_rumble(input, i, client_for_sdl[i], last_rumble_seq[i], rumble_until_us[i]);
        }

        if (any_waiting && !waiting_logged) {
            std::puts("[bt] controller connected, but all server slots are in use; waiting for UDP/WebSocket/BT slot to free");
            waiting_logged = true;
        } else if (!any_waiting) {
            waiting_logged = false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(PRO_UDP_INTERVAL_MS));
    }

    for (int i = 0; i < 4; ++i) {
        input.set_rumble(i, 0, 0, 0);
        if (client_for_sdl[i] >= 0)
            reset_bluetooth_client_slot(client_for_sdl[i]);
    }
    if (g_bt_pair_window_thread.joinable())
        g_bt_pair_window_thread.join();
    input.stop();
    std::puts("[bt] Bluetooth/local SDL controller input stopped");
}
#else
bool bluetooth_input_available() {
    return false;
}

void bluetooth_input_thread() {
    std::fprintf(stderr, "[bt] ns-backend was built without SDL3 Bluetooth/local controller support\n");
}
#endif
