#include "../../include/protocol.hpp"

#include <atomic>
#include <thread>
#include <mutex>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <string>
#include <algorithm>

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

using namespace ns;
using Clock = std::chrono::steady_clock;
using us    = std::chrono::microseconds;
using ms    = std::chrono::milliseconds;

// ── Global Flags ──────────────────────────────────────────────────────────────
static std::atomic<bool> g_running{true};
static bool g_verbose = false;

// ── Shared State (Protected by g_mtx) ─────────────────────────────────────────
static std::mutex  g_mtx;
static HIDReport   g_report[MAX_CONTROLLERS]{};
static uint16_t    g_autofire_mask[MAX_CONTROLLERS]{0};

static std::atomic<uint64_t> g_last_rx_us[MAX_CONTROLLERS]{};

// ── Diagnostics ───────────────────────────────────────────────────────────────
static std::atomic<uint64_t> g_pkts_rx{0};
static std::atomic<uint64_t> g_hid_writes{0};

// ── Signal Handler ────────────────────────────────────────────────────────────
static void on_signal(int) { g_running.store(false, std::memory_order_relaxed); }

// ── HID Writer Thread ─────────────────────────────────────────────────────────
static void writer_thread(int hz) {
    const auto tick = us(1'000'000 / hz);
    const int af_period = std::max(1, hz / (AUTOFIRE_HZ * 2));
    
    int      af_tick[MAX_CONTROLLERS]  = {0};
    uint16_t af_state[MAX_CONTROLLERS] = {0};

    int  fds[MAX_CONTROLLERS];
    bool was_connected[MAX_CONTROLLERS] = {false};
    
    for (int i = 0; i < MAX_CONTROLLERS; ++i) {
        fds[i] = -1;
    }

    auto next = Clock::now() + tick;
    HIDReport prev[MAX_CONTROLLERS]{};
    
    for (int i = 0; i < MAX_CONTROLLERS; ++i) {
        prev[i].buttons = 0xFF; // Force initial write after connection
    }

    int reconnect_timer = 0;

    while (g_running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_until(next);
        auto now = Clock::now();
        next = std::max(next + tick, now + tick);

        // Periodically attempt to open disconnected endpoints
        if (++reconnect_timer >= hz) { 
            reconnect_timer = 0;
            for (int i = 0; i < MAX_CONTROLLERS; ++i) {
                if (fds[i] < 0) {
                    std::string dev = "/dev/hidg" + std::to_string(i);
                    fds[i] = open(dev.c_str(), O_WRONLY | O_NONBLOCK);
                    if (fds[i] >= 0 && (g_verbose || !was_connected[i])) {
                        std::printf("[backend] %s opened\n", dev.c_str());
                        was_connected[i] = true;
                    }
                }
            }
        }

        // Snapshot current state to minimize lock contention
        HIDReport r[MAX_CONTROLLERS];
        uint16_t  af_mask[MAX_CONTROLLERS];
        bool      silent[MAX_CONTROLLERS];

        for (int i = 0; i < MAX_CONTROLLERS; ++i) {
            uint64_t last = g_last_rx_us[i].load(std::memory_order_acquire);
            silent[i] = (last != 0) && (now_us() - last > (uint64_t)WATCHDOG_MS * 1000u);
        }

        {
            std::lock_guard<std::mutex> lk(g_mtx);
            for (int i = 0; i < MAX_CONTROLLERS; ++i) {
                if (silent[i]) {
                    g_report[i].reset();
                    g_autofire_mask[i] = 0;
                }
                r[i] = g_report[i];
                af_mask[i] = g_autofire_mask[i];
            }
        }

        // Process logic and perform HID writes
        for (int i = 0; i < MAX_CONTROLLERS; ++i) {
            if (silent[i] && g_last_rx_us[i].load() != 0) {
                if (g_verbose) std::printf("[backend] Watchdog triggered: Zeroing input for P%d\n", i + 1);
                g_last_rx_us[i].store(0, std::memory_order_release);
            }

            // Apply autofire logic
            if (af_mask[i]) {
                if (++af_tick[i] >= af_period) { 
                    af_tick[i] = 0; 
                    af_state[i] ^= af_mask[i]; 
                }
                r[i].buttons = (r[i].buttons & ~af_mask[i]) | (af_state[i] & af_mask[i]);
            } else {
                af_tick[i] = 0; 
                af_state[i] = 0;
            }

            // Write to HID interface if state changed
            if (fds[i] >= 0 && r[i] != prev[i]) {
                ssize_t n = write(fds[i], &r[i], sizeof(r[i]));
                if (n == (ssize_t)sizeof(r[i])) {
                    prev[i] = r[i];
                    ++g_hid_writes;
                } else if (n < 0 && errno != EAGAIN) {
                    if (g_verbose) std::printf("[backend] P%d disconnected\n", i + 1);
                    close(fds[i]);
                    fds[i] = -1;
                }
            }
        }
    }

    // Cleanup: Zero all active controllers on exit
    for (int i = 0; i < MAX_CONTROLLERS; ++i) {
        if (fds[i] >= 0) {
            HIDReport zero{};
            write(fds[i], &zero, sizeof(zero));
            close(fds[i]);
        }
    }
}

// ── Stats Thread ──────────────────────────────────────────────────────────────
static void stats_thread() {
    while (g_running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(ms(5000));
        if (!g_verbose) continue;
        std::printf("[backend] pkts_rx=%-8llu  hid_writes=%-8llu\n",
            (unsigned long long)g_pkts_rx.load(),
            (unsigned long long)g_hid_writes.load());
    }
}

// ── UDP Receive Loop (Main Thread) ────────────────────────────────────────────
int main(int argc, char** argv) {
    uint16_t    port      = DEFAULT_PORT;
    int         rate_hz   = WRITER_HZ;
    std::string bind_addr = "0.0.0.0";

    for (int i = 1; i < argc; ++i) {
        std::string a(argv[i]);
        if      (a == "-p" && i+1 < argc) port      = (uint16_t)std::atoi(argv[++i]);
        else if (a == "-r" && i+1 < argc) rate_hz   = std::atoi(argv[++i]);
        else if (a == "-b" && i+1 < argc) bind_addr = argv[++i];
        else if (a == "-v")               g_verbose = true;
        else if (a == "-h") {
            std::puts("ns-backend [-p PORT] [-r HZ] [-b ADDR] [-v]");
            return 0;
        }
    }
    rate_hz = std::clamp(rate_hz, 1, 1000);

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    int sock = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (sock < 0) { perror("socket"); return 1; }

    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    
    int rbuf = 256 * 1024; // Large receive buffer for bursts
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rbuf, sizeof(rbuf));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = inet_addr(bind_addr.c_str());
    
    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(sock); return 1;
    }
    std::printf("[backend] UDP %s:%u | Max Controllers=%d | Writer=%d Hz\n",
                bind_addr.c_str(), port, MAX_CONTROLLERS, rate_hz);

    std::thread wt(writer_thread, rate_hz);
    std::thread st(stats_thread);

    int ep = epoll_create1(0);
    epoll_event ev{};
    ev.events  = EPOLLIN;
    ev.data.fd = sock;
    epoll_ctl(ep, EPOLL_CTL_ADD, sock, &ev);

    Packet pkt{};
    uint32_t expected_seq[MAX_CONTROLLERS] = {0};
    bool first_pkt[MAX_CONTROLLERS];
    for (int i = 0; i < MAX_CONTROLLERS; ++i) first_pkt[i] = true;
    
    epoll_event evs[4];

    while (g_running.load(std::memory_order_relaxed)) {
        int n = epoll_wait(ep, evs, 4, 200);
        if (n <= 0) continue;

        sockaddr_in sender{};
        socklen_t slen = sizeof(sender);
        ssize_t bytes = recvfrom(sock, &pkt, sizeof(pkt), 0, (sockaddr*)&sender, &slen);

        if (bytes != (ssize_t)PACKET_SIZE) continue;
        
        if (!packet_ok(pkt)) {
            if (g_verbose) std::puts("[backend] Bad magic/version/id, packet dropped");
            continue;
        }

        uint8_t cid = pkt.controller_id;
        bool is_reset = (pkt.flags & FLAG_RESET);
        bool sequence_jump = (expected_seq[cid] > pkt.seq) && ((expected_seq[cid] - pkt.seq) > 100);

        // Discard out-of-order packets (allow first packet and explicit resets through)
        if (!first_pkt[cid] && pkt.seq < expected_seq[cid] && !is_reset && !sequence_jump) {
            continue; 
        }
        
        first_pkt[cid]    = false;
        expected_seq[cid] = pkt.seq + 1;

        // Apply packet data to shared state
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            if (pkt.flags & FLAG_RESET) {
                g_report[cid].reset();
                g_autofire_mask[cid] = 0;
            } else {
                g_report[cid] = pkt.report;
                g_autofire_mask[cid] = (pkt.flags & FLAG_AUTOFIRE) ? pkt.autofire_mask : 0;
            }
        }
        
        g_last_rx_us[cid].store(now_us(), std::memory_order_release);
        ++g_pkts_rx;

        if (g_verbose) {
            std::printf("[P%d] seq=%-6u btns=%04X hat=%u L(%u,%u) R(%u,%u)\n",
                        cid + 1, pkt.seq, pkt.report.buttons, pkt.report.hat,
                        pkt.report.lx, pkt.report.ly, pkt.report.rx, pkt.report.ry);
        }
    }

    std::puts("[backend] Shutting down cleanly...");
    close(ep);
    close(sock);
    wt.join();
    st.join();
    return 0;
}