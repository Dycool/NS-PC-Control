#pragma once

#include <string>

// RetroPie/Batocera-style Bluetooth lifecycle manager:
// BlueZ owns pairing/reconnect; SDL owns gamepad input once Linux exposes it.
// Performs best-effort one-time runtime OS prep: start BlueZ, unblock rfkill,
// load uhid, and install missing runtime packages on Debian/Raspberry Pi OS.
void bluetooth_manager_runtime_setup(bool verbose);
void bluetooth_manager_start();
void bluetooth_manager_stop();
// expect_reply=false is fire-and-forget (BlueZ still processes the request,
// but the call returns immediately instead of blocking up to DBUS_CONNECT_TIMEOUT
// per connected gamepad). Use false only when about to exit anyway (shutdown);
// keep the default true where the caller needs the disconnect confirmed.
void bluetooth_manager_disconnect_connected_gamepads(bool expect_reply = true);
void bluetooth_manager_set_proactive_reconnect_enabled(bool enabled);
// Ask the running manager to open a 2-minute pairing window (thread-safe; no-op if the
// manager isn't running). Used to auto-pair when the Switch enters its controller-pairing screen.
void bluetooth_manager_request_pairing_window();
bool run_bluetooth_pairing_wizard();
