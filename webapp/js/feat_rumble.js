// feat_rumble.js — Phase 2: server rumble -> connected gamepad actuators.
//
// Decodes RumblePacket / PrecisionRumblePacket from the WebSocket (the server
// already pushes them to WS clients) and drives the Gamepad vibration
// actuator. Deliberately NO phone-motor vibration. The native app keeps its
// own haptics path, so everything here is skipped under window.NSBridge.
'use strict';

(function () {
    const C = NSCore.C, S = NSCore.settings, caps = NSCore.caps;

    // Map a server subpad back to the gamepad that feeds it, mirroring the
    // slot layout used by buildAndSendPacket() (kbMode 1 shifts pads by one).
    function gamepadForSubpad(subpad) {
        if (!navigator.getGamepads) return null;
        const raw = navigator.getGamepads();
        const active = [];
        for (let i = 0; i < raw.length; i++) if (raw[i]) active.push(raw[i]);
        const kbModeEl = document.getElementById('kbMode');
        const mode = kbModeEl ? parseInt(kbModeEl.value, 10) || 0 : 0;
        let idx = subpad;
        if (mode === 1) idx = subpad - 1;      // slot 0 is the keyboard
        if (idx < 0 || idx >= active.length) return null;
        return active[idx];
    }

    function applyRumble(r) {
        if (caps.isNative) return;              // native app owns haptics
        if (!S.get('rumble')) return;
        const pad = gamepadForSubpad(r.subpad);
        if (!pad) return;
        const actuator = pad.vibrationActuator
            || (pad.hapticActuators && pad.hapticActuators[0]);
        if (!actuator) return;
        const strong = Math.max(0, Math.min(1, r.high / 255));
        const weak = Math.max(0, Math.min(1, r.low / 255));
        const durationMs = r.duration10ms * 10;
        try {
            if ((strong === 0 && weak === 0) || durationMs === 0) {
                if (typeof actuator.reset === 'function') actuator.reset();
                else if (typeof actuator.playEffect === 'function')
                    actuator.playEffect('dual-rumble', { duration: 1, strongMagnitude: 0, weakMagnitude: 0 });
                return;
            }
            if (typeof actuator.playEffect === 'function') {
                actuator.playEffect('dual-rumble', {
                    duration: durationMs,
                    strongMagnitude: strong,
                    weakMagnitude: weak
                }).catch(() => {});
            } else if (typeof actuator.pulse === 'function') {
                actuator.pulse(Math.max(strong, weak), durationMs).catch(() => {});
            }
        } catch (_) {}
    }

    NSCore.registerFeature({
        id: 'rumble',
        onWsMessage(magic, view) {
            if (magic === C.RUMBLE_MAGIC && view.byteLength === C.RUMBLE_SIZE) {
                applyRumble(NSCore.parseRumble(view));
                return true;
            }
            if (magic === C.PRECISION_RUMBLE_MAGIC && view.byteLength === C.PRECISION_RUMBLE_SIZE) {
                // Web actuators cannot play raw HD rumble; use the classic
                // fallback fields the packet carries for exactly this purpose.
                applyRumble(NSCore.parsePrecisionRumble(view));
                return true;
            }
            return false;
        },
        settingsUI(ui, page) {
            if (caps.isNative) return;
            const sec = ui.section('Feedback');
            sec.toggle('Gamepad rumble', 'rumble');
            if (page === 'mobile')
                sec.note('Rumble drives connected gamepads only — the phone motor is never used.');
            else if (!('getGamepads' in navigator))
                sec.note('This browser does not expose gamepads.', true);
        }
    });
})();
