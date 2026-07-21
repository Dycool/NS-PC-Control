// feat_mouse.js — Phase 3 + Phase 5: Pointer Lock mouse input (PC page only).
//
// Two modes, both fed by Pointer Lock movementX/Y:
//  - "Mouse mode" (Phase 3): deltas drive the right stick (left stick for a
//    lone Joy-Con L, replicating stream_runtime.cpp:360-366) — client-only.
//  - "Native Joy-Con mouse" (Phase 5): S2 optical-mouse posture. Deltas are
//    sent as JoyconMousePacket frames over the WebSocket (no HMAC — WS is a
//    trusted-network transport, unlike the authenticated UDP equivalent).
'use strict';

(function () {
    const C = NSCore.C, S = NSCore.settings, caps = NSCore.caps;
    let page = 'index';
    let accX = 0, accY = 0;            // stick-mode accumulator (per input frame)
    let jmX = 0, jmY = 0, jmScroll = 0; // native-mouse accumulators
    let jmLeft = false, jmRight = false;
    let jmSeq = 0;
    let hooked = false;

    const available = () => page === 'index' && !caps.isMobile && !caps.isNative && caps.hasPointerLock;
    const stickModeOn = () => S.get('mouseMode') && !S.get('joyconMouse');
    // Native optical mouse: S2 backend + right Joy-Con profile (input_settings.cpp rule).
    const joyconMouseEligible = () =>
        NSCore.state.connected && NSCore.s2Active()
        && S.get('controllerType') === C.TYPE_JOYCON_R;
    const joyconMouseOn = () => S.get('joyconMouse') && joyconMouseEligible();
    const anyModeOn = () => available() && (stickModeOn() || joyconMouseOn());
    const locked = () => document.pointerLockElement === document.body;

    // ── Pointer Lock plumbing ────────────────────────────────────────────────
    function requestLock() {
        try { document.body.requestPointerLock(); } catch (_) {}
    }
    function onMouseMove(e) {
        if (!locked()) return;
        accX += e.movementX; accY += e.movementY;
        jmX += e.movementX; jmY += e.movementY;
    }
    function onMouseDown(e) {
        if (!locked()) return;
        if (e.button === 0) jmLeft = true;
        if (e.button === 2) jmRight = true;
    }
    function onMouseUp(e) {
        if (!locked()) return;
        if (e.button === 0) jmLeft = false;
        if (e.button === 2) jmRight = false;
    }
    function onWheel(e) {
        if (!locked()) return;
        jmScroll += Math.round(-e.deltaY); // wheel-up positive, Qt-style
    }
    function onSurfaceClick(e) {
        if (!anyModeOn() || locked()) return;
        // Do not steal clicks meant for controls.
        const t = e.target;
        if (t.closest && t.closest('button, select, input, label, a, .ns-drawer, #modalOverlay, #macroOverlay')) return;
        requestLock();
    }
    function hook() {
        if (hooked) return;
        hooked = true;
        document.addEventListener('mousemove', onMouseMove);
        document.addEventListener('mousedown', onMouseDown);
        document.addEventListener('mouseup', onMouseUp);
        document.addEventListener('wheel', onWheel, { passive: true });
        document.addEventListener('click', onSurfaceClick);
        document.addEventListener('contextmenu', e => { if (locked()) e.preventDefault(); });
    }

    // ── Native Joy-Con mouse frames (Phase 5) ────────────────────────────────
    function sendJoyconMousePacket() {
        const dx = jmX, dy = jmY, scroll = jmScroll;
        jmX = 0; jmY = 0; jmScroll = 0;
        const buf = new ArrayBuffer(C.JOYCON_MOUSE_SIZE);
        const v = new DataView(buf);
        v.setUint32(0, C.JOYCON_MOUSE_MAGIC, true);
        v.setUint8(4, C.JOYCON_MOUSE_VERSION);
        let flags = C.JOYCON_MOUSE_FLAG_ACTIVE;
        if (jmLeft) flags |= C.JOYCON_MOUSE_FLAG_LEFT_BUTTON;
        if (jmRight) flags |= C.JOYCON_MOUSE_FLAG_RIGHT_BUTTON;
        v.setUint8(5, flags);
        v.setUint8(6, 0); // subpad 0
        v.setUint32(7, jmSeq++ >>> 0, true);
        // Raw pixel deltas, like the desktop client: the server owns the
        // conversion into the Joy-Con 2 report's signed 16-bit fields.
        v.setInt32(11, dx | 0, true);
        v.setInt32(15, dy | 0, true);
        v.setInt32(19, scroll | 0, true);
        v.setBigUint64(23, BigInt(Date.now()) * 1000n, true);
        // bytes 31..46: HMAC field, zeroed (WS path is trusted, see docs/web-app.md)
        NSCore.wsSend(buf);
    }

    // ── Per-input-frame hook (runs from buildAndSendPacket, ~250 Hz) ────────
    NSCore.registerFeature({
        id: 'mouse',
        mountUI(p) { page = p; if (available()) hook(); },
        onDisconnect() { accX = accY = jmX = jmY = jmScroll = 0; jmLeft = jmRight = false; },
        onBuildFrame(frame) {
            if (!available()) return;
            if (joyconMouseOn()) {
                if (locked() || jmX || jmY || jmScroll) sendJoyconMousePacket();
                accX = accY = 0;
                return;
            }
            if (!stickModeOn()) { accX = accY = 0; return; }
            if (!locked()) { accX = accY = 0; return; }
            const sens = Number(S.get('mouseSensitivity')) || 1;
            const gain = 6 * sens; // pixels/frame -> stick deflection
            const sx = Math.max(0, Math.min(255, Math.round(128 + accX * gain)));
            const sy = Math.max(0, Math.min(255, Math.round(128 + accY * gain)));
            accX = accY = 0;
            // A lone Joy-Con (L) exposes its one physical stick in the left-stick
            // field; the server neutralizes rx/ry for that type.
            if (S.get('controllerType') === C.TYPE_JOYCON_L) {
                frame.state.lx = sx; frame.state.ly = sy;
            } else {
                frame.state.rx = sx; frame.state.ry = sy;
            }
            frame.ext.present = true;
        },
        settingsUI(ui, p) {
            if (p !== 'index' || caps.isMobile || caps.isNative) return;
            const sec = ui.section('Mouse');
            if (!caps.hasPointerLock) {
                sec.note('This browser does not support Pointer Lock.', true);
                return;
            }
            sec.toggle('Mouse Mode', 'mouseMode', {
                async beforeChange(enabling) {
                    if (enabling) { S.set('joyconMouse', false); requestLock(); }
                    else if (locked()) document.exitPointerLock();
                    return true;
                }
            });
            const eligible = joyconMouseEligible();
            sec.toggle('Joycon Mouse Mode', 'joyconMouse', {
                disabled: !eligible,
                async beforeChange(enabling) {
                    if (enabling) { S.set('mouseMode', false); requestLock(); }
                    else if (locked()) document.exitPointerLock();
                    return true;
                }
            });
            if (!eligible)
                sec.note('Joycon Mouse Mode needs a Switch 2 backend, an active connection and the Joy-Con (R) controller type.');
            sec.range('Mouse sensitivity', 'mouseSensitivity', 0, 5, 0.05);
            sec.note('Click the page to capture the mouse; press Esc to release.');
        }
    });
})();
