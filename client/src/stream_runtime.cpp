#include "stream_runtime.hpp"
#include "audio_client.hpp"
#include "input_settings.hpp"
#include "macro_client.hpp"
#include "mouse_input.hpp"
#include "rumble_client.hpp"
#include "udp_protocol.hpp"
#include "shared/macros.hpp"
#include "shared/sha256.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <print>
#include <iostream>
#include <utility>
#include <functional>
#include <vector>

std::atomic<bool> g_connected{false};
std::atomic<bool> g_connecting{false};
std::atomic<bool> g_amiiboScanPending[4]{};
std::atomic<uint16_t> g_amiiboRequestSequence[4]{};
std::atomic<uint64_t> g_amiiboScanDeadlineUs[4]{};
SOCKET g_sendSock = INVALID_SOCKET;
sockaddr_in g_sendDest{};
static std::mutex g_sendTransportMutex;
static std::mutex g_amiiboPathMutex;
static QString g_amiiboPaths[4];
std::thread g_senderThread;
std::atomic<bool> g_senderRunning{false};
uint8_t g_hmacKey[32]{};
std::atomic<uint32_t> g_packetCount{0};
std::mutex g_statusMutex;
std::string g_statusMessage = "Ready";
std::string g_lastError;
std::mutex g_assignmentMutex;
ServerAssignmentView g_serverAssignment;
std::mutex g_rosterMutex;
RosterView g_roster;

void set_amiibo_path(uint8_t subpad, const QString& path) {
    if (subpad >= 4) return;
    std::lock_guard<std::mutex> lk(g_amiiboPathMutex);
    g_amiiboPaths[subpad] = path;
}

QString amiibo_path_snapshot(uint8_t subpad) {
    if (subpad >= 4) return {};
    std::lock_guard<std::mutex> lk(g_amiiboPathMutex);
    return g_amiiboPaths[subpad];
}

void clear_amiibo_paths() {
    std::lock_guard<std::mutex> lk(g_amiiboPathMutex);
    for (QString& path : g_amiiboPaths) path.clear();
}

static void sleep_while_running(std::atomic<bool>& running, std::chrono::microseconds duration) {
    constexpr auto SLICE = std::chrono::microseconds(20'000);
    auto remaining = duration;
    while (running.load(std::memory_order_relaxed) && remaining > std::chrono::microseconds::zero()) {
        const auto chunk = std::min(remaining, SLICE);
        std::this_thread::sleep_for(chunk);
        remaining -= chunk;
    }
}

void set_status_message(const std::string& s) {
    std::lock_guard<std::mutex> lk(g_statusMutex);
    g_statusMessage = s;
}

std::string status_message() {
    std::lock_guard<std::mutex> lk(g_statusMutex);
    return g_statusMessage;
}

void reset_server_assignment_state() {
    std::lock_guard<std::mutex> lk(g_assignmentMutex);
    g_serverAssignment = ServerAssignmentView{};
}

void handle_client_assignment_packet(const ns::ClientAssignmentPacket& packet) {
    if (packet.magic != ns::CLIENT_ASSIGNMENT_MAGIC || packet.version != ns::CLIENT_ASSIGNMENT_VERSION) return;
    {
        std::lock_guard<std::mutex> lk(g_assignmentMutex);
        g_serverAssignment.server_full = (packet.flags & ns::CLIENT_ASSIGNMENT_FLAG_SERVER_FULL) != 0;
        if (packet.flags & ns::CLIENT_ASSIGNMENT_FLAG_ACCEPTED) {
            g_serverAssignment.accepted = true;
            g_serverAssignment.server_slot = packet.server_slot;
        }
        g_serverAssignment.active_clients = packet.active_clients;
        g_serverAssignment.max_clients = packet.max_clients;
        g_serverAssignment.free_virtual_slots = packet.free_virtual_slots;
        g_serverAssignment.last_update_us = ns::now_us();
        if (packet.subpad < 4) {
            g_serverAssignment.console_port_mask[packet.subpad] = packet.console_port_mask;
            g_serverAssignment.primary_console_port[packet.subpad] = packet.primary_console_port;
            g_serverAssignment.requested_type[packet.subpad] = packet.requested_type;
            g_serverAssignment.virtual_type[packet.subpad] = packet.virtual_type;
        }
    }
    if (packet.flags & ns::CLIENT_ASSIGNMENT_FLAG_SERVER_FULL) {
        g_serverFullDisconnect.store(true, std::memory_order_relaxed);
        g_serverRequestedDisconnect.store(true, std::memory_order_relaxed);
        set_status_message("Server full");
    }
    if (packet.flags & ns::CLIENT_ASSIGNMENT_FLAG_PROFILE_UNSUPPORTED) {
        g_serverProfileUnsupportedDisconnect.store(true, std::memory_order_relaxed);
        g_serverRequestedDisconnect.store(true, std::memory_order_relaxed);
        set_status_message("Switch 2 mode does not support Joy-Con L + R");
    }
}

ServerAssignmentView server_assignment_snapshot() {
    std::lock_guard<std::mutex> lk(g_assignmentMutex);
    return g_serverAssignment;
}

void reset_roster_state() {
    std::lock_guard<std::mutex> lk(g_rosterMutex);
    g_roster = RosterView{};
}

void handle_roster_packet(const ns::RosterPacket& packet) {
    if (packet.magic != ns::ROSTER_MAGIC || packet.version != ns::SERVER_INFO_VERSION) return;
    std::lock_guard<std::mutex> lk(g_rosterMutex);
    g_roster.valid = true;
    for (int h = 0; h < 4; ++h) {
        g_roster.ports[h] = packet.ports[h];
        g_roster.ports[h].name[ns::ROSTER_NAME_CAP - 1] = '\0';
    }
    g_roster.last_update_us = ns::now_us();
}

RosterView roster_snapshot() {
    std::lock_guard<std::mutex> lk(g_rosterMutex);
    return g_roster;
}

bool parse_host_port(std::string in, std::string& host, int& port) {
    in = ns::macro::trim(in);
    if (in.empty()) return false;
    port = ns::DEFAULT_PORT;
    size_t colon = in.find(':');
    if (colon != std::string::npos) {
        int p = std::atoi(in.c_str() + colon + 1);
        if (p >= 1 && p <= 65535) port = p;
        in.resize(colon);
    }
    host = ns::macro::trim(in);
    return !host.empty();
}

void raise_process_priority() {
#ifdef _WIN32
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
#else
    setpriority(PRIO_PROCESS, 0, -20);
#endif
}

void raise_sender_priority() {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#endif
}


static uint8_t requested_controller_profile_for_frame() {
    if (g_horiModeEnabled.load(std::memory_order_relaxed)) return ns::CONTROLLER_TYPE_HORI;

    int mode = g_controllerType.load(std::memory_order_relaxed);
    const bool s2 = g_switch2ModeEnabled.load(std::memory_order_relaxed);
    const bool single_joycon_selected =
        mode == ns::CONTROLLER_TYPE_JOYCON_L || mode == ns::CONTROLLER_TYPE_JOYCON_R;
    // S2 single-keyboard mode is always one full Pro Controller 2 input on P1;
    // physical SDL controllers and any selected Joy-Con-pair profile are ignored.
    if (s2 && g_keyboardMode.load(std::memory_order_relaxed) == KB_SINGLE
            && !joycon_mouse_mode_active() && !single_joycon_selected)
        return ns::CONTROLLER_TYPE_PRO_S2;
    if (s2) {
        switch (mode) {
            case ns::CONTROLLER_TYPE_PRO:         mode = ns::CONTROLLER_TYPE_PRO_S2; break;
            case ns::CONTROLLER_TYPE_JOYCON_L:    mode = ns::CONTROLLER_TYPE_JOYCON_L_S2; break;
            case ns::CONTROLLER_TYPE_JOYCON_R:    mode = ns::CONTROLLER_TYPE_JOYCON_R_S2; break;
            case ns::CONTROLLER_TYPE_JOYCON_PAIR: mode = ns::CONTROLLER_TYPE_JOYCON_PAIR_S2; break;
            default: break;
        }
    }
    if (mode == ns::CONTROLLER_TYPE_JOYCON_L ||
            mode == ns::CONTROLLER_TYPE_JOYCON_R ||
            mode == ns::CONTROLLER_TYPE_PRO ||
            mode == ns::CONTROLLER_TYPE_JOYCON_PAIR ||
            mode == ns::CONTROLLER_TYPE_PRO_S2 ||
            mode == ns::CONTROLLER_TYPE_JOYCON_L_S2 ||
            mode == ns::CONTROLLER_TYPE_JOYCON_R_S2 ||
            mode == ns::CONTROLLER_TYPE_JOYCON_PAIR_S2) {
        return static_cast<uint8_t>(mode);
    }
    return ns::CONTROLLER_TYPE_PRO;
}

void ClientFrame::reset() {
    active_count = 0;
    for (int i = 0; i < 4; ++i) {
        reports[i].reset();
        for (int j = 0; j < 3; ++j) motion[i][j].reset();
        present[i] = false;
        has_motion[i] = false;
        motion_sample_fresh[i] = false;
        controller_for_slot[i] = -1;
        battery_percent[i] = -1;
        battery_charging[i] = false;
    }
}

void build_client_frame(ClientFrame& frame, DigitalReleaseFilter filters[4], bool send_motion, int keyboard_mode) {
    frame.reset();
    auto sdl = g_sdlInput.snapshot();
    const uint64_t filter_now = ns::now_us();
    const bool s2 = g_switch2ModeEnabled.load(std::memory_order_relaxed);

    if (s2) {
        const bool native_mouse = joycon_mouse_mode_active();
        // Native S2 mode is deliberately one input only. Keyboard single owns
        // P1 completely; otherwise the first connected SDL controller is mapped
        // to P1 and every additional controller is ignored.
        if (keyboard_mode == KB_SINGLE) {
            apply_keyboard_to_report(frame.reports[0], false);
            if (mouse_mode_active()) {
                // A lone Joy-Con (L) exposes its one physical stick in the
                // left-stick field; the server neutralizes rx/ry for that type.
                if (g_controllerType.load(std::memory_order_relaxed) == ns::CONTROLLER_TYPE_JOYCON_L)
                    mouse_apply_right_stick(frame.reports[0].lx, frame.reports[0].ly);
                else
                    mouse_apply_right_stick(frame.reports[0].rx, frame.reports[0].ry);
            }
            frame.present[0] = true;
            frame.active_count = 1;
            return;
        }

        int source = -1;
        for (int i = 0; i < 4; ++i) {
            if (sdl[i].connected) { source = i; break; }
        }
        if (source >= 0) {
            for (int i = 0; i < 4; ++i) {
                if (i != source) filters[i].reset();
            }
            frame.reports[0] = sdl[source].input;
            filters[source].apply(frame.reports[0], filter_now);
            for (int j = 0; j < 3; ++j) frame.motion[0][j] = sdl[source].motion_samples[j];
            frame.present[0] = true;
            frame.has_motion[0] = send_motion && sdl[source].has_motion;
            frame.motion_sample_fresh[0] = frame.has_motion[0] && sdl[source].motion_sample_fresh;
            frame.controller_for_slot[0] = source;
            frame.battery_percent[0] = sdl[source].battery_percent;
            frame.battery_charging[0] = sdl[source].battery_charging;
            frame.active_count = 1;
        } else {
            for (int i = 0; i < 4; ++i) filters[i].reset();
        }

        if (keyboard_mode == KB_OVERRIDE) {
            apply_keyboard_to_report(frame.reports[0], true);
            frame.present[0] = true;
            frame.active_count = 1;
        }
        if (mouse_mode_active()) {
            if (!frame.present[0]) {
                frame.reports[0].reset();
                frame.present[0] = true;
                frame.active_count = 1;
            }
            // A lone Joy-Con (L) exposes its one physical stick in the
            // left-stick field; the server neutralizes rx/ry for that type.
            if (g_controllerType.load(std::memory_order_relaxed) == ns::CONTROLLER_TYPE_JOYCON_L)
                mouse_apply_right_stick(frame.reports[0].lx, frame.reports[0].ly);
            else
                mouse_apply_right_stick(frame.reports[0].rx, frame.reports[0].ry);
        }
        if (g_joyconHorizontalMode.load(std::memory_order_relaxed) && frame.present[0]) {
            const int controller_type = g_controllerType.load(std::memory_order_relaxed);
            apply_joycon_horizontal_transform(frame.reports[0], controller_type);
            if (frame.has_motion[0]) {
                for (auto& sample : frame.motion[0])
                    apply_joycon_horizontal_motion_transform(sample, controller_type);
            }
        }
        if (native_mouse) {
            // Native mouse movement is useful without a physical SDL pad too.
            // Keep a neutral Joy-Con source alive and map PC mouse clicks onto
            // the real Joy-Con 2 mouse-posture shoulder buttons.
            if (!frame.present[0]) {
                frame.reports[0].reset();
                frame.present[0] = true;
                frame.active_count = 1;
            }
        }
        return;
    }

    const bool joycon_pair_mode = !g_horiModeEnabled.load(std::memory_order_relaxed)
        && g_controllerType.load(std::memory_order_relaxed) == ns::CONTROLLER_TYPE_JOYCON_PAIR;
    const int source_slots = joycon_pair_mode ? 2 : 4;

    for (int i = 0; i < 4; ++i) {
        if (i >= source_slots || !sdl[i].connected) {
            filters[i].reset();
            continue;
        }
        frame.reports[i] = sdl[i].input;
        filters[i].apply(frame.reports[i], filter_now);
        for (int j = 0; j < 3; ++j) frame.motion[i][j] = sdl[i].motion_samples[j];
        frame.present[i] = true;
        frame.has_motion[i] = send_motion && sdl[i].has_motion;
        frame.motion_sample_fresh[i] = frame.has_motion[i] && sdl[i].motion_sample_fresh;
        frame.controller_for_slot[i] = i;
        frame.battery_percent[i] = sdl[i].battery_percent;
        frame.battery_charging[i] = sdl[i].battery_charging;
        ++frame.active_count;
    }

    if (keyboard_mode == KB_SINGLE) {
        if (frame.present[0]) {
            int target = -1;
            for (int sidx = 1; sidx < source_slots && target < 0; ++sidx) if (!frame.present[sidx]) target = sidx;
            if (target >= 0) {
                frame.reports[target] = frame.reports[0];
                std::copy_n(frame.motion[0], 3, frame.motion[target]);
                frame.has_motion[target] = frame.has_motion[0];
                frame.motion_sample_fresh[target] = frame.motion_sample_fresh[0];
                frame.present[target] = true;
                frame.controller_for_slot[target] = frame.controller_for_slot[0];
                frame.battery_percent[target] = frame.battery_percent[0];
                frame.battery_charging[target] = frame.battery_charging[0];
                ++frame.active_count;
            }
        }
        frame.reports[0].reset();
        for (auto& m : frame.motion[0]) m.reset();
        apply_keyboard_to_report(frame.reports[0], false);
        frame.present[0] = true;
        frame.has_motion[0] = false;
        frame.motion_sample_fresh[0] = false;
        frame.controller_for_slot[0] = -1;
        frame.battery_percent[0] = -1;
        frame.battery_charging[0] = false;
        frame.active_count = std::max(frame.active_count, 1);
    } else if (keyboard_mode == KB_OVERRIDE) {
        apply_keyboard_to_report(frame.reports[0], true);
        frame.present[0] = true;
        frame.active_count = std::max(frame.active_count, 1);
    }

    if (mouse_mode_active()) {
        if (!frame.present[0]) {
            frame.reports[0].reset();
            frame.present[0] = true;
            frame.active_count = std::max(frame.active_count, 1);
        }
        // A lone Joy-Con (L) exposes its one physical stick in the left-stick
        // field; the server neutralizes rx/ry for that type.
        if (!g_horiModeEnabled.load(std::memory_order_relaxed)
                && g_controllerType.load(std::memory_order_relaxed) == ns::CONTROLLER_TYPE_JOYCON_L)
            mouse_apply_right_stick(frame.reports[0].lx, frame.reports[0].ly);
        else
            mouse_apply_right_stick(frame.reports[0].rx, frame.reports[0].ry);
    }

    if (g_joyconHorizontalMode.load(std::memory_order_relaxed)) {
        const int controller_type = g_controllerType.load(std::memory_order_relaxed);
        for (int i = 0; i < 4; ++i) {
            if (!frame.present[i]) continue;
            apply_joycon_horizontal_transform(frame.reports[i], controller_type);
            if (frame.has_motion[i]) {
                for (auto& sample : frame.motion[i])
                    apply_joycon_horizontal_motion_transform(sample, controller_type);
            }
        }
    }
}

void sendAmiiboData(uint8_t subpad, const QByteArray& data) {
    std::lock_guard<std::mutex> transport_lk(g_sendTransportMutex);
    if (g_sendSock == INVALID_SOCKET) return;
    ns::AmiiboDataPacket pkt{};
    pkt.magic = ns::AMIIBO_DATA_MAGIC;
    pkt.subpad = subpad;
    size_t dl = std::min<size_t>(data.size(), ns::AMIIBO_EXTENDED_DUMP_SIZE);
    pkt.data_len = static_cast<uint16_t>(dl);
    if (dl > 0) std::memcpy(pkt.data, data.constData(), dl);
    // Server accepts directly on magic match (no hmac verification for this control packet)
    send_all_udp(g_sendSock, g_sendDest, std::span(reinterpret_cast<const uint8_t*>(&pkt), sizeof(pkt)));
}

void send_client_frame(SOCKET sock, const sockaddr_in& dest, const uint8_t hmac_key[32], uint32_t& seq, const ClientFrame& frame) {
    auto sign_and_send = [&](void* pkt, size_t size, size_t auth_size) {
        uint8_t full_hmac[32];
        hmac_sha256(std::span(hmac_key, 32), std::span(static_cast<const uint8_t*>(pkt), auth_size), std::span<uint8_t, 32>(full_hmac));
        std::memcpy(static_cast<uint8_t*>(pkt) + auth_size, full_hmac, ns::HMAC_TAG_SIZE);
        send_all_udp(sock, dest, std::span(static_cast<const uint8_t*>(pkt), size));
    };

    ns::Packet pkt{.magic = ns::PROTO_MAGIC, .version = ns::WEB_PROTO_VERSION, .flags = ns::FLAG_NONE, .reserved = 0, .seq = seq++, .ts_us = ns::now_us()};
    pkt.report.reset();
    for (int i = 0; i < 4; ++i) {
        ns::HIDReport* dst = (i == 0 ? &pkt.report.p1 : (i == 1 ? &pkt.report.p2 : (i == 2 ? &pkt.report.p3 : &pkt.report.p4)));
        fill_extended_pad(*dst, frame.reports[i], frame.present[i], frame.has_motion[i] ? frame.motion[i] : nullptr,
                          frame.battery_percent[i], frame.battery_charging[i],
                          frame.motion_sample_fresh[i]);
        // This is a requested controller profile, not necessarily the final
        // USB identity. In Joy-Con L+R mode the server expands one source pad
        // into two virtual ports and chooses L/R per port.
        dst->reserved[2] = requested_controller_profile_for_frame();
    }
    sign_and_send(&pkt, ns::PACKET_SIZE, ns::PACKET_AUTH_SIZE);
}

static void send_joycon_mouse_update(SOCKET sock,
                                     const sockaddr_in& dest,
                                     const uint8_t hmac_key[32],
                                     uint32_t& seq,
                                     uint64_t& last_send_us,
                                     uint8_t& last_flags,
                                     bool force_disable = false) {
    const bool active = !force_disable && joycon_mouse_mode_active();
    int32_t dx = 0, dy = 0, scroll_y = 0;
    bool left = false, right = false;
    if (active) {
        mouse_consume_joycon_input(dx, dy, scroll_y);
        mouse_joycon_button_state(left, right);
    }

    uint8_t flags = active ? ns::JOYCON_MOUSE_FLAG_ACTIVE : 0;
    if (left) flags |= ns::JOYCON_MOUSE_FLAG_LEFT_BUTTON;
    if (right) flags |= ns::JOYCON_MOUSE_FLAG_RIGHT_BUTTON;

    const uint64_t now = ns::now_us();
    constexpr uint64_t KEEPALIVE_US = 50'000ULL;
    const bool state_changed = flags != last_flags;
    if (!state_changed && dx == 0 && dy == 0 && scroll_y == 0
            && last_send_us != 0 && now - last_send_us < KEEPALIVE_US) {
        return;
    }
    if (!active && last_flags == 0 && !force_disable) return;

    ns::JoyconMousePacket pkt{};
    pkt.flags = flags;
    pkt.subpad = 0;
    pkt.seq = seq++;
    pkt.delta_x = dx;
    pkt.delta_y = dy;
    pkt.scroll_y = scroll_y;
    pkt.ts_us = now;

    uint8_t full_hmac[32];
    hmac_sha256(std::span(hmac_key, 32),
                std::span(reinterpret_cast<const uint8_t*>(&pkt), ns::JOYCON_MOUSE_AUTH_SIZE),
                std::span<uint8_t, 32>(full_hmac));
    std::memcpy(pkt.hmac, full_hmac, ns::HMAC_TAG_SIZE);
    send_all_udp(sock, dest,
                 std::span(reinterpret_cast<const uint8_t*>(&pkt), sizeof(pkt)));
    last_send_us = now;
    last_flags = flags;
}

static void set_roster_name(ns::RosterEntry& e, const std::string& name) {
    const size_t n = std::min(name.size(), ns::ROSTER_NAME_CAP - 1);
    std::memcpy(e.name, name.data(), n);
    e.name[n] = '\0';
}

static void build_local_roster_entries(int keyboard_mode, ns::RosterEntry out[4]) {
    for (int i = 0; i < 4; ++i) out[i] = ns::RosterEntry{};
    auto sdl = g_sdlInput.snapshot();
    const bool s2 = g_switch2ModeEnabled.load(std::memory_order_relaxed);
    auto set_entry = [&](int i, bool has_gyro, const std::string& name) {
        out[i].present = 1;
        out[i].has_gyro = has_gyro ? 1 : 0;
        set_roster_name(out[i], name);
    };

    if (s2) {
        if (keyboard_mode == KB_SINGLE) {
            set_entry(0, false, "Keyboard");
            return;
        }
        int source = -1;
        for (int i = 0; i < 4; ++i) if (sdl[i].connected) { source = i; break; }
        if (keyboard_mode == KB_OVERRIDE) {
            if (source >= 0) {
                const std::string base = sdl[source].name.empty() ? std::string("Controller") : sdl[source].name;
                set_entry(0, sdl[source].has_motion, base + " + Keyboard");
            } else {
                set_entry(0, false, "Keyboard");
            }
        } else if (source >= 0) {
            set_entry(0, sdl[source].has_motion, sdl[source].name.empty() ? "Controller" : sdl[source].name);
        }
        return;
    }

    const bool joycon_pair = !g_horiModeEnabled.load(std::memory_order_relaxed)
        && g_controllerType.load(std::memory_order_relaxed) == ns::CONTROLLER_TYPE_JOYCON_PAIR;
    const int source_slots = joycon_pair ? 2 : 4;

    int shifted_p1_target = -1;
    if (keyboard_mode == KB_SINGLE && sdl[0].connected) {
        for (int sidx = 1; sidx < source_slots; ++sidx) {
            if (!sdl[sidx].connected) { shifted_p1_target = sidx; break; }
        }
    }
    for (int i = 0; i < source_slots; ++i) {
        if (i == 0 && keyboard_mode != KB_OFF) {
            if (keyboard_mode == KB_SINGLE) {
                set_entry(0, false, "Keyboard");
            } else if (sdl[0].connected) {
                const std::string base = sdl[0].name.empty() ? std::string("Controller") : sdl[0].name;
                set_entry(0, sdl[0].has_motion, base + " + Keyboard");
            } else {
                set_entry(0, false, "Keyboard");
            }
        } else if (i == shifted_p1_target) {
            set_entry(i, sdl[0].has_motion, sdl[0].name.empty() ? "Controller" : sdl[0].name);
        } else if (sdl[i].connected) {
            set_entry(i, sdl[i].has_motion, sdl[i].name.empty() ? "Controller" : sdl[i].name);
        }
    }
}

static void send_client_names_if_changed(SOCKET sock, const sockaddr_in& dest, const uint8_t hmac_key[32],
                                         int keyboard_mode, ns::ClientNamesPacket& last_sent, uint64_t& last_send_us) {
    ns::ClientNamesPacket pkt{};
    build_local_roster_entries(keyboard_mode, pkt.pads);
    const uint64_t now = ns::now_us();
    const bool changed = std::memcmp(pkt.pads, last_sent.pads, sizeof(pkt.pads)) != 0;
    if (!changed && last_send_us != 0 && now - last_send_us < 2'000'000ULL) return;

    uint8_t full_hmac[32];
    hmac_sha256(std::span(hmac_key, 32),
                std::span(reinterpret_cast<const uint8_t*>(&pkt), ns::CLIENT_NAMES_AUTH_SIZE),
                std::span<uint8_t, 32>(full_hmac));
    std::memcpy(pkt.hmac, full_hmac, ns::HMAC_TAG_SIZE);
    send_all_udp(sock, dest, std::span(reinterpret_cast<const uint8_t*>(&pkt), sizeof(pkt)));
    std::memcpy(last_sent.pads, pkt.pads, sizeof(pkt.pads));
    last_send_us = now;
}

// Switch 2 desktop audio runs on its own UDP socket (input port + offset) and
// its own thread so it never shares the controller-input send loop. This keeps
// the ~200 audio datagrams/sec each way, plus the SDL audio work, entirely off
// the input path — input latency is unaffected by audio activity.
static void s2_audio_client_loop(std::stop_token st, std::atomic<bool>& running,
                                 std::string host, uint16_t audio_port,
                                 const uint8_t* hmac_key) {
    SOCKET asock = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in adest{};
    if (asock == INVALID_SOCKET || !resolve_udp_destination(host, audio_port, adest)) {
        if (asock != INVALID_SOCKET) closesocket(asock);
        return;
    }
    set_socket_nonblocking(asock);

    S2AudioClient audio;
    uint8_t buf[sizeof(ns::S2AudioPcmPacket)];
    while (!st.stop_requested() && running.load(std::memory_order_relaxed)) {
        const bool switch2ProAudio = g_switch2ModeEnabled.load(std::memory_order_relaxed)
            && g_switch2AudioSupported.load(std::memory_order_relaxed)
            && g_controllerType.load(std::memory_order_relaxed) == ns::CONTROLLER_TYPE_PRO;
        const auto [playbackDevice, microphoneDevice] = switch2_audio_device_selections();
        // Sends capability updates and pumps captured microphone audio.
        audio.update(asock, adest, hmac_key, switch2ProAudio,
                     g_switch2AudioEnabled.load(std::memory_order_relaxed),
                     g_switch2MicrophoneEnabled.load(std::memory_order_relaxed),
                     playbackDevice, microphoneDevice);
        // Drain inbound playback datagrams into the jitter buffer.
        for (;;) {
            const int n = static_cast<int>(
                recvfrom(asock, reinterpret_cast<char*>(buf), sizeof(buf), 0, nullptr, nullptr));
            if (n <= 0) break;
            audio.handle_packet(buf, static_cast<size_t>(n), hmac_key);
        }
        sleep_while_running(running, std::chrono::milliseconds(1));
    }
    audio.shutdown(asock, adest, hmac_key);
    closesocket(asock);
}

int run_client_stream(const ClientStreamConfig& cfg, std::atomic<bool>& running, std::string* err_out) {
    g_serverRequestedDisconnect.store(false, std::memory_order_relaxed);
    g_serverFullDisconnect.store(false, std::memory_order_relaxed);
    g_serverProfileUnsupportedDisconnect.store(false, std::memory_order_relaxed);
    if (!cfg.hmac_key) return (err_out ? *err_out = "Missing HMAC key." : ""), 1;
    raise_sender_priority();
    if (!g_sdlInput.start()) return (err_out ? *err_out = "SDL3 input failed: " + g_sdlInput.error() : ""), 1;

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in dest{};
    if (sock == INVALID_SOCKET || !resolve_udp_destination(cfg.host, cfg.port, dest)) {
        if (err_out) *err_out = "Socket or resolve error: " + cfg.host;
        if (sock != INVALID_SOCKET) closesocket(sock);
        return 1;
    }
    set_socket_nonblocking(sock);

    {
        std::lock_guard<std::mutex> transport_lk(g_sendTransportMutex);
        g_sendSock = sock;
        g_sendDest = dest;
    }

    detect_server_is_legacy(sock, dest);
    if (g_serverLastReplyUs.load() == 0) {
        {
            std::lock_guard<std::mutex> transport_lk(g_sendTransportMutex);
            if (g_sendSock == sock) g_sendSock = INVALID_SOCKET;
        }
        if (err_out) *err_out = "Server not reachable.";
        closesocket(sock);
        return 1;
    }
    uint32_t seq = 0;
    uint32_t joycon_mouse_seq = 0;
    uint64_t joycon_mouse_last_send_us = 0;
    uint8_t joycon_mouse_last_flags = 0;
    RumbleManager rumble;
    // Desktop Switch 2 audio runs on its own socket/thread (GUI client only) so
    // it never shares this input loop. Started only after the server is known
    // reachable (above) so we don't spawn it on a failed connect.
    std::jthread audio_thread;
    if (cfg.gui_features) {
        audio_thread = std::jthread(s2_audio_client_loop, std::ref(running), cfg.host,
                                    static_cast<uint16_t>(cfg.port + ns::S2_AUDIO_PORT_OFFSET),
                                    cfg.hmac_key);
    }
    DigitalReleaseFilter sdl_filters[4];
    bool no_controllers_printed = false;
    uint64_t last_probe_us = 0, last_kb_poll_us = 0;
    ns::ClientNamesPacket last_names{};
    uint64_t last_names_send_us = 0;
    ClientFrame frame;
    frame.reset();
    uint64_t next_input_frame_us = 0;

    while (running.load(std::memory_order_relaxed)) {
        const uint64_t loop_start_us = ns::now_us();
        std::string upload;
        if (cfg.gui_features && (loop_start_us - last_kb_poll_us >= 10000ULL)) {
            update_keyboard_state_cache();
            {
                std::lock_guard<std::mutex> lk(g_macro_mtx);
                upload.swap(g_macro_upload_pending);
            }
            poll_macro_entry_hotkeys();
            last_kb_poll_us = loop_start_us;
        }

        if (next_input_frame_us == 0) next_input_frame_us = loop_start_us;
        const bool input_frame_due = loop_start_us >= next_input_frame_us;
        if (input_frame_due) {
            // Keep controller traffic at the established 250 Hz even while the
            // loop polls audio every millisecond. This avoids wasting UDP/CPU
            // budget and leaves headroom under the server's packet-rate limit.
            constexpr uint64_t INPUT_FRAME_US =
                static_cast<uint64_t>(ns::LEGACY_UDP_INTERVAL_MS) * 1000ULL;
            next_input_frame_us += INPUT_FRAME_US;
            if (loop_start_us > next_input_frame_us + 8 * INPUT_FRAME_US)
                next_input_frame_us = loop_start_us + INPUT_FRAME_US;
            g_sdlInput.poll();
            build_client_frame(frame, sdl_filters, true, g_keyboardMode.load());

            if (cfg.gui_features) {
                if (g_macro_recording.load(std::memory_order_relaxed)) macro_record_sample(frame.reports[0]);
                if (g_macro_running.load(std::memory_order_relaxed)) {
                    if (apply_macro_override(frame.reports, frame.present, frame.has_motion)) {
                        frame.active_count = 1;
                        for (int i = 0; i < 4; ++i) {
                            if (!frame.has_motion[i]) frame.motion_sample_fresh[i] = false;
                        }
                    }
                }
            }

            // Keep the packet as a source-controller frame. For Joy-Con L+R mode
            // the client sends one physical pad with CONTROLLER_TYPE_JOYCON_PAIR;
            // the server owns the virtual L/R expansion and USB identities.
            send_client_frame(sock, dest, cfg.hmac_key, seq, frame);
            send_joycon_mouse_update(sock, dest, cfg.hmac_key,
                                     joycon_mouse_seq, joycon_mouse_last_send_us,
                                     joycon_mouse_last_flags);
            ++g_packetCount;
        }
        // Audio (capability negotiation, mic, playback) is handled entirely by
        // the dedicated s2_audio_client_loop on its own socket/thread, so nothing
        // audio-related runs on this input loop.
        send_client_names_if_changed(sock, dest, cfg.hmac_key, g_keyboardMode.load(), last_names, last_names_send_us);
        // Typed uploads are sent after the live input frame so the server has
        // already seen the current controller type.
        if (!upload.empty()) send_macro_udp_packet(sock, dest, cfg.hmac_key, upload, 0);
        pump_udp_replies(sock, rumble, cfg.hmac_key, frame.controller_for_slot);
        if (g_serverRequestedDisconnect.load(std::memory_order_relaxed)) {
            if (g_serverProfileUnsupportedDisconnect.load(std::memory_order_relaxed)) {
                const std::string message = "Switch 2 mode does not support Joy-Con L + R. Use an individual Joy-Con or Pro Controller.";
                set_status_message(message);
                if (err_out) *err_out = message;
            } else {
                set_status_message(g_serverFullDisconnect.load(std::memory_order_relaxed) ? "Server full" : "Disconnected");
            }
            running.store(false, std::memory_order_relaxed);
            break;
        }
        rumble.update_timeouts(frame.controller_for_slot);

        const uint64_t now = ns::now_us();
        if (now - last_probe_us >= 5000000ULL) {
            ns::ServerInfoProbe probe{};
            send_all_udp(sock, dest, std::span(reinterpret_cast<const uint8_t*>(&probe), sizeof(probe)));
            last_probe_us = now;
        }
        if (last_probe_us && now - g_serverLastReplyUs.load(std::memory_order_relaxed) > 15000000ULL) {
            set_status_message("Lost connection to server");
            running.store(false, std::memory_order_relaxed);
            break;
        }

        if (frame.active_count > 0) {
            no_controllers_printed = false;
            // Sleep only for the remainder of the absolute 4 ms frame. Sleeping
            // another full tick after polling/signing made the actual mouse
            // cadence roughly processing-time + 4 ms (commonly 9-10 ms).
            const uint64_t sleep_now_us = ns::now_us();
            if (next_input_frame_us > sleep_now_us) {
                sleep_while_running(running, std::chrono::microseconds(
                    next_input_frame_us - sleep_now_us));
            } else {
                std::this_thread::yield();
            }
        } else {
            if (cfg.print_cli_waiting_messages && !no_controllers_printed) {
                std::println("No controllers detected - waiting for connections...");
                no_controllers_printed = true;
            }
            sleep_while_running(running, std::chrono::milliseconds(cfg.idle_sleep_ms));
        }
    }

    // The audio thread (audio_thread) stops and sends its own capability
    // teardown when this function returns (running is already false here) via its
    // jthread destructor; nothing to do for audio on this path.
    rumble.stop_all();
    if (joycon_mouse_last_flags & ns::JOYCON_MOUSE_FLAG_ACTIVE) {
        send_joycon_mouse_update(sock, dest, cfg.hmac_key,
                                 joycon_mouse_seq, joycon_mouse_last_send_us,
                                 joycon_mouse_last_flags, true);
    }
    send_udp_disconnect_packet(sock, dest, cfg.hmac_key, seq++);
    {
        std::lock_guard<std::mutex> transport_lk(g_sendTransportMutex);
        if (g_sendSock == sock) g_sendSock = INVALID_SOCKET;
    }
    closesocket(sock);
    return g_serverProfileUnsupportedDisconnect.load(std::memory_order_relaxed) ? 1 : 0;
}

void sender_thread_main(std::atomic<bool>& running, std::string host, uint16_t port) {
    set_status_message("Connecting to " + host + ":" + std::to_string(port) + "...");
    if (!probe_server_sync(host, port)) {
        const std::string error = g_serverProbeFull.load(std::memory_order_relaxed)
            ? "Server is full. All virtual controller slots are in use."
            : "Server not reachable. Check the IP address.";
        g_lastError = error;
        set_status_message(error);
        g_connecting.store(false, std::memory_order_relaxed);
        g_connected.store(false, std::memory_order_relaxed);
        g_joyconMouseModeEnabled.store(false, std::memory_order_relaxed);
        mouse_input_reset();
        running.store(false, std::memory_order_relaxed);
        return;
    }
    if (!running.load(std::memory_order_relaxed)) {
        g_connecting.store(false, std::memory_order_relaxed);
        return;
    }
    if (!g_sdlInput.start()) {
        const std::string error = "SDL3 input failed: " + g_sdlInput.error();
        g_lastError = error;
        set_status_message(error);
        g_connecting.store(false, std::memory_order_relaxed);
        g_connected.store(false, std::memory_order_relaxed);
        g_joyconMouseModeEnabled.store(false, std::memory_order_relaxed);
        mouse_input_reset();
        running.store(false, std::memory_order_relaxed);
        return;
    }
    if (!running.load(std::memory_order_relaxed)) {
        g_connecting.store(false, std::memory_order_relaxed);
        return;
    }

    ClientStreamConfig cfg{.host = std::move(host), .port = port,
                           .gui_features = true, .print_cli_waiting_messages = false,
                           .idle_sleep_ms = 50, .hmac_key = g_hmacKey};
    g_connected.store(true, std::memory_order_relaxed);
    g_connecting.store(false, std::memory_order_relaxed);
    set_status_message("Connected to " + cfg.host + ":" + std::to_string(cfg.port));
    std::string err;
    int rc = run_client_stream(cfg, running, &err);
    if (rc != 0 && !err.empty()) {
        g_lastError = err;
        set_status_message(err);
    }
    g_connected.store(false);
    g_connecting.store(false, std::memory_order_relaxed);
    g_joyconMouseModeEnabled.store(false, std::memory_order_relaxed);
    mouse_input_reset();
    running.store(false, std::memory_order_relaxed);
}

std::expected<void, std::string> start_connection(const std::string& target) {
    if (g_connected.load() || g_connecting.load()) return {};
    std::string host;
    int port = ns::DEFAULT_PORT;
    if (!parse_host_port(target, host, port)) return std::unexpected("Please enter a Raspberry Pi IP address.");
    g_serverProbeFull.store(false, std::memory_order_relaxed);
    g_switch2ModeEnabled.store(false, std::memory_order_relaxed);
    g_switch2AudioSupported.store(false, std::memory_order_relaxed);
    g_horiModeEnabled.store(false, std::memory_order_relaxed);
    g_joyconMouseModeEnabled.store(false, std::memory_order_relaxed);
    derive_key(ns::DEFAULT_SECRET, g_hmacKey);
    save_last_ip(target);
    load_macro_entries();
    g_packetCount.store(0);
    g_serverLastReplyUs.store(0);
    g_serverRequestedDisconnect.store(false, std::memory_order_relaxed);
    g_serverFullDisconnect.store(false, std::memory_order_relaxed);
    g_serverProfileUnsupportedDisconnect.store(false, std::memory_order_relaxed);
    reset_server_assignment_state();
    reset_roster_state();
    for (int i = 0; i < 4; ++i) {
        g_amiiboScanPending[i].store(false, std::memory_order_relaxed);
        g_amiiboRequestSequence[i].store(0, std::memory_order_relaxed);
        g_amiiboScanDeadlineUs[i].store(0, std::memory_order_relaxed);
    }
    clear_amiibo_paths();
    mouse_input_reset();
    g_lastError.clear();
    if (g_senderThread.joinable()) {
        g_senderRunning = false;
        g_senderThread.join();
    }
    g_senderRunning = true;
    g_connecting.store(true, std::memory_order_relaxed);
    g_senderThread = std::thread(sender_thread_main, std::ref(g_senderRunning), host, (uint16_t)port);
    return {};
}

void stop_connection() {
    const bool was_connected = g_connected.exchange(false);
    const bool was_connecting = g_connecting.exchange(false, std::memory_order_relaxed);
    if (g_senderThread.joinable()) {
        g_senderRunning = false;
        g_senderThread.join();
    }
    // Release controllers when not connected so detection is scoped to a live
    // session (the sender thread has already been joined, so SDL is now idle).
    g_sdlInput.stop();
    reset_server_assignment_state();
    reset_roster_state();
    g_switch2ModeEnabled.store(false, std::memory_order_relaxed);
    g_switch2AudioSupported.store(false, std::memory_order_relaxed);
    g_horiModeEnabled.store(false, std::memory_order_relaxed);
    g_joyconMouseModeEnabled.store(false, std::memory_order_relaxed);
    mouse_input_reset();
    for (int i = 0; i < 4; ++i) {
        g_amiiboScanPending[i].store(false, std::memory_order_relaxed);
        g_amiiboRequestSequence[i].store(0, std::memory_order_relaxed);
        g_amiiboScanDeadlineUs[i].store(0, std::memory_order_relaxed);
    }
    clear_amiibo_paths();
    if (was_connected || was_connecting) set_status_message("Disconnected");
}

NetworkRuntime::NetworkRuntime() {
#ifdef _WIN32
    timeBeginPeriod(1);
    WSADATA wsa{};
    ok = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
    signal(SIGPIPE, SIG_IGN);
    ok = true;
#endif
}

NetworkRuntime::~NetworkRuntime() {
    stop_connection();
    g_sdlInput.stop();
#ifdef _WIN32
    if (ok) WSACleanup();
    timeEndPeriod(1);
#endif
}

bool NetworkRuntime::good() const { return ok; }
