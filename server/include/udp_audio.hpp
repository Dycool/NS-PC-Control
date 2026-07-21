#pragma once

#include <cstdint>
#include <netinet/in.h>
#include <span>

// Switch 2 audio bridge. The desktop ns-client uses authenticated UDP PCM
// datagrams on a dedicated socket; web clients use the trusted WebSocket
// transport (web_server.cpp registers its sink through the s2_ws_audio_*
// hooks below). Either transport can be the active audio consumer.
void s2_udp_audio_start(int udp_socket);
void s2_udp_audio_stop();
bool s2_udp_audio_handle_packet(std::span<const uint8_t> data, const sockaddr_in& sender);
void s2_udp_audio_forget_endpoint(const sockaddr_in& sender);
uint8_t s2_udp_audio_headset_state(uint8_t report_timer);

// WebSocket audio sink registration (called from web_server.cpp on the lws
// service thread). caps uses ns::S2_AUDIO_CAP_*; 0 unregisters. The
// mic-requires-playback invariant is enforced here, like the UDP path.
void s2_ws_audio_set_capabilities(uint8_t caps);
// Heartbeat: refreshes the WS capability timeout (e.g. on mic PCM ingress).
void s2_ws_audio_touch();
