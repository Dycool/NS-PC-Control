#pragma once

#include "shared/protocol.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

// Audio bridge for the native S2 gadget's UAC1 interface, backed by
// s2_rawgadget's AS-OUT/AS-IN endpoints.
bool s2_uac1_audio_start();
void s2_uac1_audio_stop();
bool s2_uac1_audio_ready();

bool s2_uac1_wait_console_audio(
    std::array<unsigned char, ns::S2_AUDIO_USB_FRAME_BYTES>& audio_frame,
    std::chrono::milliseconds timeout);

bool s2_uac1_submit_microphone_audio(const uint8_t* data, size_t len);
