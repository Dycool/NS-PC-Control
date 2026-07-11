#include "audio_client.hpp"

#include "shared/sha256.h"
#include "udp_protocol.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <print>
#include <span>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace {

// Each network block is exactly one 1 ms USB audio frame. Two blocks are the
// lowest practical startup target that still tolerates one late LAN/scheduler
// tick without an immediate underrun.
constexpr int PLAYBACK_START_BLOCKS = 2;      // 2 ms startup jitter buffer
constexpr int PLAYBACK_TARGET_BLOCKS = 2;     // 2 ms steady-state queue target
constexpr int PLAYBACK_MAX_BLOCKS = 12;       // 12 ms hard latency cap
constexpr int MICROPHONE_MAX_BLOCKS = 8;      // 8 ms hard capture cap
constexpr float PLAYBACK_MIN_RATIO = 0.9975f;
constexpr float PLAYBACK_MAX_RATIO = 1.0025f;
constexpr uint64_t CAPS_INTERVAL_US = 500'000ULL;
constexpr uint64_t AUDIO_RETRY_US = 5'000'000ULL;
constexpr uint64_t PLAYBACK_IDLE_RESET_US = 500'000ULL;

void sign_packet(void* packet, size_t auth_size, uint8_t* tag, const uint8_t hmac_key[32]) {
    uint8_t digest[32];
    hmac_sha256(std::span(hmac_key, 32),
                std::span(static_cast<const uint8_t*>(packet), auth_size),
                std::span<uint8_t, 32>(digest));
    std::memcpy(tag, digest, ns::HMAC_TAG_SIZE);
}

bool verify_packet(const uint8_t* data, size_t len, size_t auth_size, const uint8_t hmac_key[32]) {
    if (!data || len != auth_size + ns::HMAC_TAG_SIZE) return false;
    return hmac_verify(std::span(hmac_key, 32), std::span(data, auth_size),
                       std::span(data + auth_size, ns::HMAC_TAG_SIZE)) == 0;
}


SDL_AudioDeviceID resolve_audio_device(const std::string& requested, bool recording) {
    if (requested.empty() || requested == "@default") {
        return recording ? SDL_AUDIO_DEVICE_DEFAULT_RECORDING
                         : SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    }

    int count = 0;
    SDL_AudioDeviceID* devices = recording
        ? SDL_GetAudioRecordingDevices(&count)
        : SDL_GetAudioPlaybackDevices(&count);
    if (!devices) return 0;

    SDL_AudioDeviceID selected = 0;
    for (int i = 0; i < count; ++i) {
        const char* name = SDL_GetAudioDeviceName(devices[i]);
        if (name && requested == name) {
            selected = devices[i];
            break;
        }
    }
    SDL_free(devices);
    return selected;
}

} // namespace

std::vector<std::string> enumerate_s2_audio_devices(bool recording) {
    std::vector<std::string> names;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return names;

    int count = 0;
    SDL_AudioDeviceID* devices = recording
        ? SDL_GetAudioRecordingDevices(&count)
        : SDL_GetAudioPlaybackDevices(&count);
    std::unordered_set<std::string> seen;
    if (devices) {
        for (int i = 0; i < count; ++i) {
            const char* name = SDL_GetAudioDeviceName(devices[i]);
            if (name && *name && seen.insert(name).second) names.emplace_back(name);
        }
        SDL_free(devices);
    }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    std::sort(names.begin(), names.end());
    return names;
}

S2AudioClient::~S2AudioClient() {
    close_streams();
}

void S2AudioClient::close_streams() {
    if (playback_stream) {
        SDL_DestroyAudioStream(playback_stream);
        playback_stream = nullptr;
    }
    if (microphone_stream) {
        SDL_DestroyAudioStream(microphone_stream);
        microphone_stream = nullptr;
    }
    if (audio_subsystem_ref) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        audio_subsystem_ref = false;
    }
    playback_started = false;
    active_flags = 0;
    have_playback_sequence = false;
    last_playback_receive_us = 0;
}

void S2AudioClient::reconfigure(uint8_t flags, const std::string& playback_device,
                                const std::string& microphone_device) {
    close_streams();
    requested_flags = flags;
    requested_playback_device = playback_device;
    requested_microphone_device = microphone_device;
    next_open_retry_us = 0;
    if (flags == 0) return;

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        std::println(stderr, "[s2][audio] SDL audio init failed: {}", SDL_GetError());
        next_open_retry_us = ns::now_us() + AUDIO_RETRY_US;
        return;
    }
    audio_subsystem_ref = true;

    // Ask SDL for the smallest sensible device period for this 1 ms tunnel.
    // This remains a hint: the OS/backend may round or ignore it. At 48 kHz,
    // 64 sample frames are about 1.33 ms.
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "64");

    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_S16LE;
    spec.channels = ns::S2_AUDIO_CHANNELS;
    spec.freq = ns::S2_AUDIO_SAMPLE_RATE;

    if ((flags & ns::S2_AUDIO_CAP_PLAYBACK) != 0) {
        const SDL_AudioDeviceID playback_id = resolve_audio_device(playback_device, false);
        if (playback_id == 0) {
            std::println(stderr, "[s2][audio] selected playback device is unavailable: {}", playback_device);
        } else {
            playback_stream = SDL_OpenAudioDeviceStream(
                playback_id, &spec, nullptr, nullptr);
        }
        if (playback_stream) {
            // Keep the device paused until a small network jitter buffer exists.
            active_flags |= ns::S2_AUDIO_CAP_PLAYBACK;
        } else {
            std::println(stderr, "[s2][audio] playback device failed ({}): {}", playback_device, SDL_GetError());
        }
    }

    if ((flags & ns::S2_AUDIO_CAP_MICROPHONE) != 0
            && (active_flags & ns::S2_AUDIO_CAP_PLAYBACK) != 0) {
        const SDL_AudioDeviceID microphone_id = resolve_audio_device(microphone_device, true);
        if (microphone_id == 0) {
            std::println(stderr, "[s2][audio] selected microphone is unavailable: {}", microphone_device);
        } else {
            microphone_stream = SDL_OpenAudioDeviceStream(
                microphone_id, &spec, nullptr, nullptr);
        }
        if (microphone_stream && SDL_ResumeAudioStreamDevice(microphone_stream)) {
            active_flags |= ns::S2_AUDIO_CAP_MICROPHONE;
        } else {
            std::println(stderr, "[s2][audio] microphone failed ({}): {}", microphone_device, SDL_GetError());
            if (microphone_stream) {
                SDL_DestroyAudioStream(microphone_stream);
                microphone_stream = nullptr;
            }
        }
    }

    if (active_flags != flags) next_open_retry_us = ns::now_us() + AUDIO_RETRY_US;
    last_announced_flags = 0xFF; // force immediate capability refresh
}

void S2AudioClient::send_capabilities(SOCKET sock, const sockaddr_in& destination,
                                      const uint8_t hmac_key[32], bool force) {
    if (sock == INVALID_SOCKET) return;
    const uint64_t now = ns::now_us();
    if (!force && active_flags == last_announced_flags
            && now - last_capabilities_send_us < CAPS_INTERVAL_US) return;

    ns::S2AudioCapabilitiesPacket packet{};
    packet.flags = active_flags;
    packet.seq = capabilities_sequence++;
    packet.ts_us = now;
    sign_packet(&packet, ns::S2_AUDIO_CAPS_AUTH_SIZE, packet.hmac, hmac_key);
    (void)send_all_udp(sock, destination,
                       std::span(reinterpret_cast<const uint8_t*>(&packet), sizeof(packet)));
    last_capabilities_send_us = now;
    last_announced_flags = active_flags;
}

void S2AudioClient::pump_microphone(SOCKET sock, const sockaddr_in& destination,
                                    const uint8_t hmac_key[32]) {
    if (!microphone_stream || (active_flags & ns::S2_AUDIO_CAP_MICROPHONE) == 0) return;

    int available = SDL_GetAudioStreamAvailable(microphone_stream);
    if (available > static_cast<int>(ns::S2_AUDIO_PCM_BYTES) * MICROPHONE_MAX_BLOCKS) {
        // Prefer current microphone audio over sending a delayed backlog.
        SDL_ClearAudioStream(microphone_stream);
        return;
    }

    for (int i = 0; i < 4 && available >= static_cast<int>(ns::S2_AUDIO_PCM_BYTES); ++i) {
        ns::S2AudioPcmPacket packet{};
        packet.direction = ns::S2_AUDIO_DIR_CLIENT_TO_CONSOLE;
        packet.payload_bytes = ns::S2_AUDIO_PCM_BYTES;
        packet.seq = microphone_sequence++;
        packet.ts_us = ns::now_us();
        const int received = SDL_GetAudioStreamData(
            microphone_stream, packet.pcm, static_cast<int>(ns::S2_AUDIO_PCM_BYTES));
        if (received != static_cast<int>(ns::S2_AUDIO_PCM_BYTES)) break;
        sign_packet(&packet, ns::S2_AUDIO_PCM_AUTH_SIZE, packet.hmac, hmac_key);
        (void)send_all_udp(sock, destination,
                           std::span(reinterpret_cast<const uint8_t*>(&packet), sizeof(packet)));
        available = SDL_GetAudioStreamAvailable(microphone_stream);
    }
}

void S2AudioClient::maintain_playback() {
    if (!playback_stream) return;
    const uint64_t now = ns::now_us();
    if (playback_started && last_playback_receive_us != 0
            && now - last_playback_receive_us > PLAYBACK_IDLE_RESET_US) {
        SDL_PauseAudioStreamDevice(playback_stream);
        SDL_ClearAudioStream(playback_stream);
        SDL_SetAudioStreamFrequencyRatio(playback_stream, 1.0f);
        playback_started = false;
        have_playback_sequence = false;
    }
}

void S2AudioClient::update(SOCKET sock, const sockaddr_in& destination,
                           const uint8_t hmac_key[32], bool switch2_mode,
                           bool playback_requested, bool microphone_requested,
                           const std::string& playback_device,
                           const std::string& microphone_device) {
    // Remember whether the server was previously told that a headset existed.
    // If the runtime becomes ineligible (non-S2, unsupported server, or a
    // Joy-Con profile), send an immediate zero-capability update instead of
    // leaving the old headset state alive until the server timeout.
    const bool had_announced_capabilities =
        last_announced_flags != 0xFF && last_announced_flags != 0;

    uint8_t desired = 0;
    if (switch2_mode && playback_requested) desired |= ns::S2_AUDIO_CAP_PLAYBACK;
    // A microphone is physically part of a headset. Never advertise a
    // microphone when the emulated controller reports no headphones.
    if (switch2_mode && playback_requested && microphone_requested) {
        desired |= ns::S2_AUDIO_CAP_MICROPHONE;
    }

    const uint64_t now = ns::now_us();
    const bool device_changed = playback_device != requested_playback_device
        || microphone_device != requested_microphone_device;
    if (desired != requested_flags || device_changed) {
        reconfigure(desired, playback_device, microphone_device);
    } else if (desired != 0 && active_flags != desired
               && next_open_retry_us != 0 && now >= next_open_retry_us) {
        reconfigure(desired, playback_device, microphone_device);
    }

    if (switch2_mode) {
        send_capabilities(sock, destination, hmac_key, false);
    } else if (had_announced_capabilities) {
        // reconfigure(0, ...) has already closed the local streams and forced
        // last_announced_flags to 0xFF, so this emits one authenticated NSAC
        // packet with flags=0.
        send_capabilities(sock, destination, hmac_key, true);
    }
    pump_microphone(sock, destination, hmac_key);
    maintain_playback();
}

bool S2AudioClient::handle_packet(const uint8_t* data, size_t len, const uint8_t hmac_key[32]) {
    if (!data || len < sizeof(uint32_t)) return false;
    uint32_t magic = 0;
    std::memcpy(&magic, data, sizeof(magic));
    if (magic != ns::S2_AUDIO_PCM_MAGIC) return false;

    if (len != sizeof(ns::S2AudioPcmPacket)
            || !verify_packet(data, len, ns::S2_AUDIO_PCM_AUTH_SIZE, hmac_key)) return true;
    ns::S2AudioPcmPacket packet{};
    std::memcpy(&packet, data, sizeof(packet));
    if (packet.version != ns::S2_AUDIO_VERSION
            || packet.direction != ns::S2_AUDIO_DIR_CONSOLE_TO_CLIENT
            || packet.payload_bytes != ns::S2_AUDIO_PCM_BYTES) return true;
    if (!playback_stream || (active_flags & ns::S2_AUDIO_CAP_PLAYBACK) == 0) return true;

    if (have_playback_sequence) {
        const int32_t delta = static_cast<int32_t>(packet.seq - last_playback_sequence);
        if (delta <= 0) return true; // duplicate or late datagram
        const int missing = std::min(delta - 1, 2);
        if (missing > 0) {
            static constexpr std::array<uint8_t, ns::S2_AUDIO_PCM_BYTES> silence{};
            for (int i = 0; i < missing; ++i) {
                SDL_PutAudioStreamData(
                    playback_stream, silence.data(), static_cast<int>(silence.size()));
            }
        }
    }
    last_playback_sequence = packet.seq;
    have_playback_sequence = true;
    last_playback_receive_us = ns::now_us();

    int queued = SDL_GetAudioStreamQueued(playback_stream);
    if (queued < 0) queued = 0;
    if (queued > static_cast<int>(ns::S2_AUDIO_PCM_BYTES) * PLAYBACK_MAX_BLOCKS) {
        // Never preserve a stale backlog. Restart from the newest packet with a
        // short pre-roll instead of allowing latency to climb toward 100+ ms.
        SDL_PauseAudioStreamDevice(playback_stream);
        SDL_ClearAudioStream(playback_stream);
        SDL_SetAudioStreamFrequencyRatio(playback_stream, 1.0f);
        playback_started = false;
        have_playback_sequence = false;
        queued = 0;
    }

    if (!SDL_PutAudioStreamData(
            playback_stream, packet.pcm, static_cast<int>(packet.payload_bytes))) {
        std::println(stderr, "[s2][audio] playback queue failed: {}", SDL_GetError());
        return true;
    }

    queued = SDL_GetAudioStreamQueued(playback_stream);
    if (!playback_started
            && queued >= static_cast<int>(ns::S2_AUDIO_PCM_BYTES) * PLAYBACK_START_BLOCKS) {
        if (SDL_ResumeAudioStreamDevice(playback_stream)) playback_started = true;
    }

    if (playback_started && queued >= 0) {
        // The Switch and the PC audio device have independent clocks. A tiny
        // +/-0.25% resampling trim keeps the queue around two milliseconds and
        // avoids periodic clear/restart glitches without audible pitch change.
        const float blocks = static_cast<float>(queued)
            / static_cast<float>(ns::S2_AUDIO_PCM_BYTES);
        const float error = blocks - static_cast<float>(PLAYBACK_TARGET_BLOCKS);
        const float ratio = std::clamp(1.0f + error * 0.0005f,
                                       PLAYBACK_MIN_RATIO, PLAYBACK_MAX_RATIO);
        (void)SDL_SetAudioStreamFrequencyRatio(playback_stream, ratio);
    }
    return true;
}

void S2AudioClient::shutdown(SOCKET sock, const sockaddr_in& destination,
                             const uint8_t hmac_key[32]) {
    const uint8_t previous = active_flags;
    active_flags = 0;
    if (previous != 0 && sock != INVALID_SOCKET) send_capabilities(sock, destination, hmac_key, true);
    requested_flags = 0;
    close_streams();
}
