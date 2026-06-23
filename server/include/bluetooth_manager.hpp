#pragma once

#include <string>

// RetroPie/Batocera-style Bluetooth lifecycle manager:
// BlueZ owns pairing/reconnect; SDL owns gamepad input once Linux exposes it.
// Performs best-effort one-time runtime OS prep: start BlueZ, unblock rfkill,
// load uhid, and install missing runtime packages on Debian/Raspberry Pi OS.
void bluetooth_manager_runtime_setup(bool verbose);
void bluetooth_manager_start(bool open_pair_window);
void bluetooth_manager_stop();
void bluetooth_manager_disconnect_connected_gamepads();
