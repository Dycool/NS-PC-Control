#pragma once

#include "shared/protocol.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

// ALSA bridge for the kernel usb_f_uac1 function used only by the native S2
// gadget. The USB OUT stream (console audio) is exposed as an ALSA capture
// stream; the USB IN stream (headset microphone) is exposed as ALSA playback.
bool s2_uac1_audio_compiled();
bool s2_uac1_audio_start();
void s2_uac1_audio_stop();
bool s2_uac1_audio_ready();

bool s2_uac1_wait_console_audio(
    std::array<unsigned char, ns::S2_AUDIO_USB_FRAME_BYTES>& audio_frame,
    std::chrono::milliseconds timeout);

bool s2_uac1_submit_microphone_audio(const uint8_t* data, size_t len);
