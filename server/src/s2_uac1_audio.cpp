#include "s2_uac1_audio.hpp"

#include "s2_rawgadget.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

namespace {

float linear_gain_from_usb_volume(int16_t value) {
    const float db = static_cast<float>(value) / 256.0f;
    return std::pow(10.0f, db / 20.0f);
}

int16_t saturate(float v) {
    if (v >= 32767.0f) return 32767;
    if (v <= -32768.0f) return -32768;
    return static_cast<int16_t>(v);
}

void apply_gain_mute(uint8_t* pcm, size_t len, bool muted, int16_t volume_1_256db) {
    if (muted) {
        std::memset(pcm, 0, len);
        return;
    }
    const float gain = linear_gain_from_usb_volume(volume_1_256db);
    if (gain == 1.0f) return;
    for (size_t i = 0; i + 1 < len; i += 2) {
        int16_t sample = static_cast<int16_t>(pcm[i] | (pcm[i + 1] << 8));
        sample = saturate(static_cast<float>(sample) * gain);
        pcm[i] = static_cast<uint8_t>(sample & 0xFF);
        pcm[i + 1] = static_cast<uint8_t>((sample >> 8) & 0xFF);
    }
}

std::atomic<bool> g_ready{false};

} // namespace

bool s2_uac1_audio_start() {
    g_ready.store(true, std::memory_order_release);
    return true;
}

void s2_uac1_audio_stop() {
    g_ready.store(false, std::memory_order_release);
}

bool s2_uac1_audio_ready() {
    return g_ready.load(std::memory_order_acquire);
}

bool s2_uac1_wait_console_audio(
        std::array<unsigned char, ns::S2_AUDIO_USB_FRAME_BYTES>& audio_frame,
        std::chrono::milliseconds timeout) {
    audio_frame.fill(0);
    if (!s2_uac1_audio_ready()) return false;
    if (!s2_rawgadget_pop_console_audio(std::span<uint8_t>(audio_frame), timeout)) {
        return false;
    }
    bool muted = false;
    int16_t volume = 0;
    s2_rawgadget_get_playback_control(muted, volume);
    apply_gain_mute(audio_frame.data(), audio_frame.size(), muted, volume);
    return true;
}

bool s2_uac1_submit_microphone_audio(const uint8_t* data, size_t len) {
    if (!data || len == 0 || !s2_uac1_audio_ready()) return false;
    if (len % ns::S2_AUDIO_USB_FRAME_BYTES != 0) return false;

    bool muted = false;
    int16_t volume = 0;
    s2_rawgadget_get_capture_control(muted, volume);

    std::vector<uint8_t> scratch(data, data + len);
    apply_gain_mute(scratch.data(), scratch.size(), muted, volume);
    return s2_rawgadget_queue_microphone_audio(std::span<const uint8_t>(scratch));
}
