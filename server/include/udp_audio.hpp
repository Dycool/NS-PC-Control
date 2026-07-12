#pragma once

#include <cstdint>
#include <netinet/in.h>
#include <span>

// Desktop ns-client-only Switch 2 audio bridge. Audio shares the established
// UDP socket/port but uses authenticated, independently typed PCM datagrams.
void s2_udp_audio_start(int udp_socket);
void s2_udp_audio_stop();
bool s2_udp_audio_handle_packet(std::span<const uint8_t> data, const sockaddr_in& sender);
void s2_udp_audio_forget_endpoint(const sockaddr_in& sender);
uint8_t s2_udp_audio_headset_state(uint8_t report_timer);
