#pragma once

#include "platform.hpp"
#include "shared/protocol.hpp"

#include <SDL3/SDL.h>

#include <string>
#include <vector>

#include <cstddef>
#include <cstdint>

class S2AudioClient {
public:
    S2AudioClient() = default;
    ~S2AudioClient();
    S2AudioClient(const S2AudioClient&) = delete;
    S2AudioClient& operator=(const S2AudioClient&) = delete;

    void update(SOCKET sock, const sockaddr_in& destination, const uint8_t hmac_key[32],
                bool switch2_mode, bool playback_requested, bool microphone_requested,
                const std::string& playback_device, const std::string& microphone_device);
    bool handle_packet(const uint8_t* data, size_t len, const uint8_t hmac_key[32]);
    void shutdown(SOCKET sock, const sockaddr_in& destination, const uint8_t hmac_key[32]);

    bool active() const noexcept { return active_flags != 0; }

private:
    SDL_AudioStream* playback_stream = nullptr;
    SDL_AudioStream* microphone_stream = nullptr;
    bool audio_subsystem_ref = false;
    bool playback_started = false;
    uint8_t requested_flags = 0;
    std::string requested_playback_device;
    std::string requested_microphone_device;
    uint8_t active_flags = 0;
    uint8_t last_announced_flags = 0xFF;
    uint32_t capabilities_sequence = 0;
    uint32_t microphone_sequence = 0;
    uint32_t last_playback_sequence = 0;
    bool have_playback_sequence = false;
    uint64_t last_capabilities_send_us = 0;
    uint64_t last_playback_receive_us = 0;
    uint64_t next_open_retry_us = 0;

    void reconfigure(uint8_t flags, const std::string& playback_device,
                     const std::string& microphone_device);
    void close_streams();
    void send_capabilities(SOCKET sock, const sockaddr_in& destination,
                           const uint8_t hmac_key[32], bool force);
    void pump_microphone(SOCKET sock, const sockaddr_in& destination,
                         const uint8_t hmac_key[32]);
    void maintain_playback();
};

std::vector<std::string> enumerate_s2_audio_devices(bool recording);
