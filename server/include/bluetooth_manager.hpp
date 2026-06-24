#pragma once

#include <string>

// RetroPie/Batocera-style Bluetooth lifecycle manager:
// BlueZ owns pairing/reconnect; SDL owns gamepad input once Linux exposes it.
// Performs best-effort one-time runtime OS prep: start BlueZ, unblock rfkill,
// load uhid, and install missing runtime packages on Debian/Raspberry Pi OS.
void bluetooth_manager_runtime_setup(bool verbose);
void bluetooth_manager_start();
void bluetooth_manager_stop();
void bluetooth_manager_disconnect_connected_gamepads();
void bluetooth_manager_set_proactive_reconnect_enabled(bool enabled);
// Ask the running manager to open a 2-minute pairing window (thread-safe; no-op if the
// manager isn't running). Used to auto-pair when the Switch enters its controller-pairing screen.
void bluetooth_manager_request_pairing_window();
bool run_bluetooth_pairing_wizard();
