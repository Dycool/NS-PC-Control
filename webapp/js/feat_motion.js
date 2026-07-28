// feat_motion.js — Phase 3: browser gyro (DeviceMotion) -> motion samples.
//
// The sampling/scale math lives in NSCore.motion (ported from the historical
// mobile.js path). This module owns the settings toggle, the iOS permission
// flow (must run from a user gesture — the toggle click) and, on the PC page,
// attaching the samples to the outgoing ext-pad. The touch page keeps its own
// motion wiring inside mobile.js/sendPacket; the native app uses its sensors.
'use strict';

(function () {
    const S = NSCore.settings, caps = NSCore.caps;
    let page = 'index';

    NSCore.registerFeature({
        id: 'motion',
        mountUI(p) {
            page = p;
            // Preserve historical behavior: nothing auto-starts here. mobile.js
            // calls NSCore.motion.enable() on Connect when the toggle is on.
        },
        onBuildFrame(frame) {
            // Only the PC page feeds motion through this hook; mobile.js already
            // consumed the samples for this packet.
            if (page !== 'index' || caps.isNative) return;
            if (!S.get('gyro') || !NSCore.motion.enabled) return;
            const m = NSCore.motion.consume();
            if (!m) return;
            frame.ext.motionSamples = m.samples;
            frame.ext.motionFresh = m.fresh;
        },
        settingsUI(ui) {
            if (caps.isNative) return; // native app has its own sensor pipeline
            const sec = ui.section('Motion');
            const hasSensors = caps.hasDeviceMotion;
            const secure = caps.isSecureContext;
            const hasGamepadMotion = NSCore.gamepadMotion.anySupported();
            sec.toggle('Gyro / motion', 'gyro', {
                disabled: !NSCore.motion.supported && !hasGamepadMotion,
                async beforeChange(enabling) {
                    if (!enabling) { NSCore.motion.disable(); return true; }
                    if (NSCore.motion.supported && await NSCore.motion.enable()) return true;
                    return NSCore.gamepadMotion.anySupported();
                }
            });
            if (hasGamepadMotion)
                sec.note('Physical gamepad motion detected through the optional GamepadPose API.');
            else if (!hasSensors)
                sec.note('This browser does not currently expose phone or physical gamepad motion sensors.');
            else if (!secure)
                sec.note('Browsers only expose motion sensors on secure origins. At home: use the native mobile app (native gyro, no restrictions), or in Chrome add this address under chrome://flags/#unsafely-treat-insecure-origin-as-secure. HTTPS (e.g. Tailscale/Caddy) also works.', true);
            else if (caps.needsMotionPermission)
                sec.note('iOS asks for permission when you enable this.');
            if (!hasGamepadMotion)
                sec.note('Physical controller gyro requires browser support for the experimental GamepadPose extension.');
        }
    });
})();
