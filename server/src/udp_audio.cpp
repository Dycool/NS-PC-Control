#include "udp_audio.hpp"

#include "app_state.hpp"
#include "s2_uac1_audio.hpp"
#include "shared/protocol.hpp"
#include "shared/sha256.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <print>
#include <span>
#include <thread>
#include <vector>

#include <poll.h>
#include <sys/socket.h>

namespace {

constexpr uint64_t AUDIO_CAPABILITY_TIMEOUT_US = 5'000'000ULL;

struct AudioEndpointState {
    std::mutex mutex;
    sockaddr_in endpoint{};
    bool valid = false;
    uint8_t capabilities = 0;
    uint64_t last_seen_us = 0;
    uint32_t output_sequence = 0;
    uint32_t last_capabilities_sequence = 0;
    uint32_t last_microphone_sequence = 0;
    bool have_capabilities_sequence = false;
    bool have_microphone_sequence = false;
};

AudioEndpointState g_audio_endpoint;
std::jthread g_audio_playback_thread;
std::jthread g_audio_capture_thread;
std::atomic<int> g_audio_socket{-1};
std::atomic<uint8_t> g_active_capabilities{0};
std::atomic<uint64_t> g_active_last_seen_us{0};

bool same_endpoint(const sockaddr_in& a, const sockaddr_in& b) {
    return a.sin_family == b.sin_family
        && a.sin_addr.s_addr == b.sin_addr.s_addr
        && a.sin_port == b.sin_port;
}

// Audio arrives on its own socket, so its source port differs from the client's
// input session's source port. Associate the two by source IP only. On a LAN
// each client has a distinct IP; this could only mis-associate two clients that
// shared one address behind NAT, which is not a supported deployment (same
// constraint as the gadget-mode change path).
bool same_ip(const sockaddr_in& a, const sockaddr_in& b) {
    return a.sin_addr.s_addr == b.sin_addr.s_addr;
}

bool endpoint_has_active_udp_session(const sockaddr_in& endpoint, uint64_t now) {
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        std::lock_guard<std::mutex> lk(g_ctx.mtx[i]);
        const ClientSession& client = g_ctx.clients[i];
        if (!client.active || client.source != InputSource::Udp) continue;
        if (elapsed_us_over(now, client.last_rx_us, CLIENT_TIMEOUT_US)) continue;
        if (same_ip(client.addr, endpoint)) return true;
    }
    return false;
}

bool endpoint_has_active_udp_pro_session(const sockaddr_in& endpoint, uint64_t now) {
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        std::lock_guard<std::mutex> lk(g_ctx.mtx[i]);
        const ClientSession& client = g_ctx.clients[i];
        if (!client.active || client.source != InputSource::Udp) continue;
        if (elapsed_us_over(now, client.last_rx_us, CLIENT_TIMEOUT_US)) continue;
        if (!same_ip(client.addr, endpoint)) continue;

        // The desktop client stamps every pad in the live input frame with the
        // requested controller profile. Audio is a Pro Controller 2-only
        // feature, so reject stale/forged capability packets from Joy-Con
        // sessions even if they are otherwise authenticated.
        const uint8_t profile = client.report.p1.reserved[2];
        return profile == ns::CONTROLLER_TYPE_PRO
            || profile == ns::CONTROLLER_TYPE_PRO_S2;
    }
    return false;
}

bool verify_packet(std::span<const uint8_t> data, size_t auth_size) {
    if (data.size() != auth_size + ns::HMAC_TAG_SIZE) return false;
    return hmac_verify(std::span(g_ctx.hmac_key, 32), data.first(auth_size),
                       data.subspan(auth_size, ns::HMAC_TAG_SIZE)) == 0;
}

void clear_endpoint_locked() {
    g_audio_endpoint.endpoint = {};
    g_audio_endpoint.valid = false;
    g_audio_endpoint.capabilities = 0;
    g_audio_endpoint.last_seen_us = 0;
    g_audio_endpoint.output_sequence = 0;
    g_audio_endpoint.last_capabilities_sequence = 0;
    g_audio_endpoint.last_microphone_sequence = 0;
    g_audio_endpoint.have_capabilities_sequence = false;
    g_audio_endpoint.have_microphone_sequence = false;
    g_active_capabilities.store(0, std::memory_order_relaxed);
    g_active_last_seen_us.store(0, std::memory_order_relaxed);
}

bool endpoint_snapshot(sockaddr_in& endpoint, uint8_t& capabilities, uint32_t& sequence) {
    const uint64_t now = ns::now_us();
    uint64_t last_seen = 0;
    {
        std::lock_guard<std::mutex> lk(g_audio_endpoint.mutex);
        if (!g_audio_endpoint.valid
                || elapsed_us_over(now, g_audio_endpoint.last_seen_us, AUDIO_CAPABILITY_TIMEOUT_US)) {
            clear_endpoint_locked();
            return false;
        }
        endpoint = g_audio_endpoint.endpoint;
        capabilities = g_audio_endpoint.capabilities;
        last_seen = g_audio_endpoint.last_seen_us;
    }

    // Do not nest g_audio_endpoint.mutex with the client-session mutexes.
    if (!endpoint_has_active_udp_pro_session(endpoint, now)) {
        std::lock_guard<std::mutex> lk(g_audio_endpoint.mutex);
        if (g_audio_endpoint.valid && same_endpoint(g_audio_endpoint.endpoint, endpoint)) {
            clear_endpoint_locked();
        }
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(g_audio_endpoint.mutex);
        if (!g_audio_endpoint.valid || !same_endpoint(g_audio_endpoint.endpoint, endpoint)) return false;
        sequence = g_audio_endpoint.output_sequence++;
    }
    g_active_capabilities.store(capabilities, std::memory_order_relaxed);
    g_active_last_seen_us.store(last_seen, std::memory_order_relaxed);
    return true;
}

void sign_pcm_packet(ns::S2AudioPcmPacket& packet) {
    uint8_t digest[32];
    hmac_sha256(std::span(g_ctx.hmac_key, 32),
                std::span(reinterpret_cast<const uint8_t*>(&packet), ns::S2_AUDIO_PCM_AUTH_SIZE),
                std::span<uint8_t, 32>(digest));
    std::memcpy(packet.hmac, digest, ns::HMAC_TAG_SIZE);
}

// Console -> client. Batch S2_AUDIO_UDP_FRAMES one-millisecond USB frames into a
// single authenticated datagram, cutting the packet rate (e.g. 1000 -> 200 pps)
// so the audio socket loads the Pi and the network far less.
void audio_playback_loop(std::stop_token stop_token) {
    std::array<unsigned char, ns::S2_AUDIO_USB_FRAME_BYTES> frame{};
    std::array<uint8_t, ns::S2_AUDIO_PCM_BYTES> payload{};
    size_t frames_in_batch = 0;

    while (!stop_token.stop_requested() && g_ctx.running.load(std::memory_order_relaxed)) {
        if (!s2_uac1_wait_console_audio(frame, std::chrono::milliseconds(5))) {
            // Genuine console silence (frames arrive every 1 ms when audio is
            // flowing). Drop any partial batch so a silence gap never emits a
            // stale half-batch when audio resumes.
            frames_in_batch = 0;
            continue;
        }
        std::memcpy(payload.data() + frames_in_batch * ns::S2_AUDIO_USB_FRAME_BYTES,
                    frame.data(), ns::S2_AUDIO_USB_FRAME_BYTES);
        if (++frames_in_batch < ns::S2_AUDIO_UDP_FRAMES) continue;
        frames_in_batch = 0;

        sockaddr_in endpoint{};
        uint8_t capabilities = 0;
        uint32_t sequence = 0;
        if (!endpoint_snapshot(endpoint, capabilities, sequence)
                || (capabilities & ns::S2_AUDIO_CAP_PLAYBACK) == 0) {
            continue;
        }

        ns::S2AudioPcmPacket packet{};
        packet.direction = ns::S2_AUDIO_DIR_CONSOLE_TO_CLIENT;
        packet.payload_bytes = ns::S2_AUDIO_PCM_BYTES;
        packet.seq = sequence;
        packet.ts_us = ns::now_us();
        std::memcpy(packet.pcm, payload.data(), payload.size());
        sign_pcm_packet(packet);
        const int sock = g_audio_socket.load(std::memory_order_relaxed);
        if (sock >= 0) {
            (void)sendto(sock, &packet, sizeof(packet), 0,
                         reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint));
        }
    }
}

// Client -> console (mic) and capability updates. Owns all inbound audio traffic
// on the dedicated socket so the controller-input loop never touches it.
void audio_capture_loop(std::stop_token stop_token) {
    std::vector<uint8_t> buf(sizeof(ns::S2AudioPcmPacket));
    while (!stop_token.stop_requested() && g_ctx.running.load(std::memory_order_relaxed)) {
        const int sock = g_audio_socket.load(std::memory_order_relaxed);
        if (sock < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        pollfd pfd{.fd = sock, .events = POLLIN, .revents = 0};
        const int n = poll(&pfd, 1, 5);
        if (n <= 0 || (pfd.revents & POLLIN) == 0) continue;
        for (;;) {
            sockaddr_in sender{};
            socklen_t slen = sizeof(sender);
            const ssize_t r = recvfrom(sock, buf.data(), buf.size(), 0,
                                       reinterpret_cast<sockaddr*>(&sender), &slen);
            if (r <= 0) break;
            (void)s2_udp_audio_handle_packet(
                std::span<const uint8_t>(buf.data(), static_cast<size_t>(r)), sender);
        }
    }
}

} // namespace

void s2_udp_audio_start(int udp_socket) {
    s2_udp_audio_stop();
    g_audio_socket.store(udp_socket, std::memory_order_relaxed);
    if (g_ctx.usb_controller_family != UsbControllerFamily::Switch2) return;
    g_audio_playback_thread = std::jthread(audio_playback_loop);
    g_audio_capture_thread = std::jthread(audio_capture_loop);
    if (g_ctx.verbose) {
        std::println("[s2][audio] dedicated UDP bridge ready on input port + {}: "
                     "PCM S16LE stereo 48 kHz, {} ms/datagram",
                     ns::S2_AUDIO_PORT_OFFSET, ns::S2_AUDIO_UDP_FRAMES);
    }
}

void s2_udp_audio_stop() {
    // Stop publication first, then broadcast to both directions before waiting
    // for either. Each loop has at most a 5 ms blocking poll/wait, so they now
    // drain concurrently instead of serially.
    g_audio_socket.store(-1, std::memory_order_relaxed);
    if (g_audio_playback_thread.joinable()) g_audio_playback_thread.request_stop();
    if (g_audio_capture_thread.joinable()) g_audio_capture_thread.request_stop();
    if (g_audio_playback_thread.joinable()) g_audio_playback_thread.join();
    if (g_audio_capture_thread.joinable()) g_audio_capture_thread.join();
    std::lock_guard<std::mutex> lk(g_audio_endpoint.mutex);
    clear_endpoint_locked();
}

bool s2_udp_audio_handle_packet(std::span<const uint8_t> data, const sockaddr_in& sender) {
    if (data.size() < sizeof(uint32_t)) return false;
    uint32_t magic = 0;
    std::memcpy(&magic, data.data(), sizeof(magic));
    if (magic != ns::S2_AUDIO_CAPS_MAGIC && magic != ns::S2_AUDIO_PCM_MAGIC) return false;

    // Recognise and consume the namespace outside S2 mode so an audio packet can
    // never fall through to the legacy input parser.
    if (g_ctx.usb_controller_family != UsbControllerFamily::Switch2) return true;
    if (!rate_allow(sender.sin_addr.s_addr)) return true;

    const uint64_t now = ns::now_us();
    if (magic == ns::S2_AUDIO_CAPS_MAGIC) {
        if (data.size() != sizeof(ns::S2AudioCapabilitiesPacket)
                || !verify_packet(data, ns::S2_AUDIO_CAPS_AUTH_SIZE)) return true;
        ns::S2AudioCapabilitiesPacket packet{};
        std::memcpy(&packet, data.data(), sizeof(packet));
        if (packet.version != ns::S2_AUDIO_VERSION
                || !endpoint_has_active_udp_session(sender, now)
                || !endpoint_has_active_udp_pro_session(sender, now))
            return true;

        uint8_t caps = packet.flags
            & static_cast<uint8_t>(ns::S2_AUDIO_CAP_PLAYBACK | ns::S2_AUDIO_CAP_MICROPHONE);
        // A microphone is only valid as part of a headset. Enforce the same
        // physical invariant as the desktop UI/runtime even for malformed or
        // older authenticated clients: mic-only must behave as nothing attached.
        if ((caps & ns::S2_AUDIO_CAP_PLAYBACK) == 0) {
            caps &= static_cast<uint8_t>(~ns::S2_AUDIO_CAP_MICROPHONE);
        }
        {
            std::lock_guard<std::mutex> lk(g_audio_endpoint.mutex);
            const bool new_endpoint = !g_audio_endpoint.valid
                || !same_endpoint(g_audio_endpoint.endpoint, sender);
            if (!new_endpoint && g_audio_endpoint.have_capabilities_sequence
                    && static_cast<int32_t>(packet.seq
                        - g_audio_endpoint.last_capabilities_sequence) <= 0) {
                return true; // duplicate/reordered capability update
            }
            if (new_endpoint) {
                g_audio_endpoint.output_sequence = 0;
                g_audio_endpoint.have_microphone_sequence = false;
                g_audio_endpoint.last_microphone_sequence = 0;
            }
            g_audio_endpoint.endpoint = sender;
            g_audio_endpoint.valid = true;
            g_audio_endpoint.capabilities = caps;
            g_audio_endpoint.last_seen_us = now;
            g_audio_endpoint.last_capabilities_sequence = packet.seq;
            g_audio_endpoint.have_capabilities_sequence = true;
            g_active_capabilities.store(caps, std::memory_order_relaxed);
            g_active_last_seen_us.store(now, std::memory_order_relaxed);
        }
        return true;
    }

    if (data.size() != sizeof(ns::S2AudioPcmPacket)
            || !verify_packet(data, ns::S2_AUDIO_PCM_AUTH_SIZE)) return true;
    ns::S2AudioPcmPacket packet{};
    std::memcpy(&packet, data.data(), sizeof(packet));
    if (packet.version != ns::S2_AUDIO_VERSION
            || packet.direction != ns::S2_AUDIO_DIR_CLIENT_TO_CONSOLE
            || packet.payload_bytes != ns::S2_AUDIO_PCM_BYTES) return true;

    if (!endpoint_has_active_udp_session(sender, now)
            || !endpoint_has_active_udp_pro_session(sender, now)) return true;
    {
        std::lock_guard<std::mutex> lk(g_audio_endpoint.mutex);
        if (!g_audio_endpoint.valid || !same_endpoint(g_audio_endpoint.endpoint, sender)
                || (g_audio_endpoint.capabilities & ns::S2_AUDIO_CAP_MICROPHONE) == 0
                || elapsed_us_over(now, g_audio_endpoint.last_seen_us, AUDIO_CAPABILITY_TIMEOUT_US)) {
            return true;
        }
        if (g_audio_endpoint.have_microphone_sequence
                && static_cast<int32_t>(packet.seq
                    - g_audio_endpoint.last_microphone_sequence) <= 0) {
            return true; // duplicate/reordered PCM would create clicks or replay speech
        }
        g_audio_endpoint.last_microphone_sequence = packet.seq;
        g_audio_endpoint.have_microphone_sequence = true;
        g_audio_endpoint.last_seen_us = now;
        g_active_last_seen_us.store(now, std::memory_order_relaxed);
    }
    (void)s2_uac1_submit_microphone_audio(packet.pcm, packet.payload_bytes);
    return true;
}

void s2_udp_audio_forget_endpoint(const sockaddr_in& sender) {
    std::lock_guard<std::mutex> lk(g_audio_endpoint.mutex);
    if (g_audio_endpoint.valid && same_endpoint(g_audio_endpoint.endpoint, sender)) clear_endpoint_locked();
}

uint8_t s2_udp_audio_headset_state(uint8_t report_timer) {
    const uint64_t last_seen = g_active_last_seen_us.load(std::memory_order_relaxed);
    if (last_seen == 0
            || elapsed_us_over(ns::now_us(), last_seen, AUDIO_CAPABILITY_TIMEOUT_US)) {
        g_active_capabilities.store(0, std::memory_order_relaxed);
        return 0;
    }
    const uint8_t caps = g_active_capabilities.load(std::memory_order_relaxed);
    if ((caps & ns::S2_AUDIO_CAP_MICROPHONE) != 0) {
        return static_cast<uint8_t>(0x07u | ((report_timer & 0x08u) ? 0x08u : 0x00u));
    }
    if ((caps & ns::S2_AUDIO_CAP_PLAYBACK) != 0) {
        return static_cast<uint8_t>(0x05u | ((report_timer & 0x08u) ? 0x08u : 0x00u));
    }
    return 0;
}
