// ns_core.js — shared core for the NS PC Control web clients.
//
// Single source of truth for protocol constants, packet build/parse helpers,
// capability detection, the persisted settings store and the feature registry
// consumed by index.js (PC), mobile.js (touch) and the feat_*.js modules.
//
// Loaded before every other webapp script (after bridge.js). Everything lives
// under the window.NSCore namespace; no top-level identifiers are exported so
// index.js/mobile.js keep their own local names.
'use strict';

window.NSCore = (function () {

    // ── Protocol constants (mirror shared/protocol.hpp) ──────────────────────
    const C = {
        PROTO_MAGIC: 0x4E535743,              // 'NSWC'
        PROTO_VERSION: 5,                     // WEB_PROTO_VERSION
        PROTO_VERSION_3: 6,                   // WEB_PROTO_VERSION_3 (3 motion samples)
        RUMBLE_MAGIC: 0x4E535652,             // 'NSVR'
        PRECISION_RUMBLE_MAGIC: 0x4E535648,   // 'NSVH'
        CONTROLLER_STATUS_MAGIC: 0x4E534353,  // 'NSCS'
        CLIENT_ASSIGNMENT_MAGIC: 0x4E534341,  // 'NSCA'
        CLIENT_NAMES_MAGIC: 0x4E53434E,       // 'NSCN'
        ROSTER_MAGIC: 0x4E53524F,             // 'NSRO'
        JOYCON_MOUSE_MAGIC: 0x4E534A4D,       // 'NSJM'
        AMIIBO_REQUEST_MAGIC: 0x4E534152,     // 'NSAR'
        AMIIBO_DATA_MAGIC: 0x4E534144,        // 'NSAD'
        S2_AUDIO_CAPS_MAGIC: 0x4E534143,      // 'NSAC'
        S2_AUDIO_PCM_MAGIC: 0x4E534155,       // 'NSAU'
        MACRO_CHUNK_MAGIC: 0x4E534D4B,        // 'NSMK'

        SERVER_INFO_VERSION: 1,
        JOYCON_MOUSE_VERSION: 3,
        S2_AUDIO_VERSION: 1,

        PACKET_SIZE: 212,                     // WEB_PACKET_SIZE (no HMAC on WS)
        EXT_REPORT_SIZE: 48,
        RUMBLE_SIZE: 8,
        PRECISION_RUMBLE_SIZE: 20,
        CONTROLLER_STATUS_SIZE: 12,
        CLIENT_ASSIGNMENT_SIZE: 16,
        ROSTER_SIZE: 208,
        ROSTER_ENTRY_SIZE: 50,
        ROSTER_NAME_CAP: 48,
        CLIENT_NAMES_SIZE: 224,
        AMIIBO_REQUEST_SIZE: 8,
        AMIIBO_RAW_DUMP_SIZE: 540,
        AMIIBO_EXTENDED_DUMP_SIZE: 572,
        AMIIBO_DATA_SIZE: 579,                // 4+1+2+572 packed
        AMIIBO_DATA_HEADER: 7,                // offsetof(AmiiboDataPacket, data)
        JOYCON_MOUSE_SIZE: 47,
        S2_AUDIO_CAPS_SIZE: 36,
        S2_AUDIO_PCM_SIZE: 996,               // 36 + 960
        S2_AUDIO_PCM_BYTES: 960,              // 5 ms stereo S16LE @ 48 kHz
        S2_AUDIO_SAMPLE_RATE: 48000,
        S2_AUDIO_CHANNELS: 2,
        S2_AUDIO_CAP_PLAYBACK: 1,
        S2_AUDIO_CAP_MICROPHONE: 2,
        S2_AUDIO_DIR_CONSOLE_TO_CLIENT: 0,
        S2_AUDIO_DIR_CLIENT_TO_CONSOLE: 1,

        PAD_PRESENT: 1,
        FLAG_RESET: 0x01,
        FLAG_SINGLE_PAD: 0x04,

        BTN_Y: 1 << 0, BTN_B: 1 << 1, BTN_A: 1 << 2, BTN_X: 1 << 3,
        BTN_L: 1 << 4, BTN_R: 1 << 5, BTN_ZL: 1 << 6, BTN_ZR: 1 << 7,
        BTN_MINUS: 1 << 8, BTN_PLUS: 1 << 9, BTN_LSTICK: 1 << 10, BTN_RSTICK: 1 << 11,
        BTN_HOME: 1 << 12, BTN_CAPTURE: 1 << 13,

        HAT_N: 0, HAT_NE: 1, HAT_E: 2, HAT_SE: 3,
        HAT_S: 4, HAT_SW: 5, HAT_W: 6, HAT_NW: 7, HAT_NEUTRAL: 8,

        EXT_PAD_PRESENT: 0x01,
        EXT_BUTTON_SL: 0x10,
        EXT_BUTTON_SR: 0x20,
        EXT_STATUS_BATTERY_VALID: 0x01,
        EXT_STATUS_BATTERY_CHARGING: 0x02,
        EXT_STATUS_MOTION_FRESH: 0x04,
        EXT_STATUS_MOTION_FRESH_VALID: 0x08,

        CLIENT_ASSIGNMENT_FLAG_ACCEPTED: 0x01,
        CLIENT_ASSIGNMENT_FLAG_SERVER_FULL: 0x02,
        CLIENT_ASSIGNMENT_FLAG_SWITCH_ASLEEP: 0x04,
        CLIENT_ASSIGNMENT_FLAG_ASSIGNMENT_VALID: 0x08,
        CLIENT_ASSIGNMENT_FLAG_PROFILE_UNSUPPORTED: 0x10,

        CONTROLLER_STATUS_FLAG_BODY_RGB_VALID: 0x01,

        JOYCON_MOUSE_FLAG_ACTIVE: 0x01,
        JOYCON_MOUSE_FLAG_LEFT_BUTTON: 0x02,
        JOYCON_MOUSE_FLAG_RIGHT_BUTTON: 0x04,

        // ControllerType (HIDReport reserved[2], ext-pad offset 47)
        TYPE_DEFAULT: 0,
        TYPE_JOYCON_L: 1,
        TYPE_JOYCON_R: 2,
        TYPE_PRO: 3,
        TYPE_JOYCON_PAIR: 4,
        TYPE_HORI: 5,
        TYPE_PRO_S2: 6,
        TYPE_JOYCON_L_S2: 7,
        TYPE_JOYCON_R_S2: 8,
        TYPE_JOYCON_PAIR_S2: 9
    };

    // ── Capability detection ─────────────────────────────────────────────────
    const isNative = !!(window.NSBridge
        || (window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.nsBridge));
    const caps = {
        isNative,
        isMobile: /Mobi|Android|iPhone|iPad|iPod|BlackBerry|IEMobile|Opera Mini/i.test(navigator.userAgent)
            || ('ontouchstart' in window && navigator.maxTouchPoints > 0),
        isSecureContext: !!window.isSecureContext,
        hasGamepadHaptics: false, // refreshed lazily; actuator lives on Gamepad objects
        hasDeviceMotion: typeof DeviceMotionEvent !== 'undefined',
        needsMotionPermission: typeof DeviceMotionEvent !== 'undefined'
            && typeof DeviceMotionEvent.requestPermission === 'function',
        hasPointerLock: 'pointerLockElement' in document || 'webkitPointerLockElement' in document,
        hasGetUserMedia: !!(navigator.mediaDevices && navigator.mediaDevices.getUserMedia),
        hasAudioWorklet: !!(window.AudioContext && AudioContext.prototype
            && typeof AudioWorkletNode !== 'undefined')
    };

    // ── Settings store (localStorage-backed, observable) ─────────────────────
    const SETTINGS_KEY = 'nswc_settings';
    const settingsDefaults = {
        controllerType: C.TYPE_PRO,     // Pro / Joy-Con L / Joy-Con R / Pair
        gyro: true,                     // browser DeviceMotion -> motion samples
        rumble: true,                   // server rumble -> gamepad actuators
        mouseMode: false,               // Pointer Lock -> stick
        mouseSensitivity: 1.0,          // 0.1 .. 3.0
        joyconMouse: false,             // S2 native optical mouse over WS
        homeShortcut: true,             // LS+RS -> HOME combo
        captureShortcut: true,          // MINUS+PLUS -> CAPTURE combo
        audioPlayback: false,           // S2 console audio -> browser
        audioMicrophone: false,         // browser mic -> console
        audioOutputDevice: '',          // sinkId ('' = default)
        audioInputDevice: ''            // getUserMedia deviceId ('' = default)
    };
    let settingsData = {};
    try { settingsData = JSON.parse(localStorage.getItem(SETTINGS_KEY) || '{}') || {}; }
    catch (_) { settingsData = {}; }
    const settingsListeners = {};
    const settings = {
        get(key) {
            return Object.prototype.hasOwnProperty.call(settingsData, key)
                ? settingsData[key] : settingsDefaults[key];
        },
        set(key, value) {
            if (settings.get(key) === value) return;
            settingsData[key] = value;
            try { localStorage.setItem(SETTINGS_KEY, JSON.stringify(settingsData)); } catch (_) {}
            (settingsListeners[key] || []).forEach(fn => { try { fn(value); } catch (_) {} });
            (settingsListeners['*'] || []).forEach(fn => { try { fn(key, value); } catch (_) {} });
        },
        onChange(key, fn) { (settingsListeners[key] = settingsListeners[key] || []).push(fn); },
        defaults: settingsDefaults
    };

    // ── Live session state (fed by the packet parsers below) ─────────────────
    function emptyAssignment() {
        return {
            accepted: false, serverFull: false, serverSlot: 255,
            activeClients: 0, maxClients: 4, freeSlots: 0, switchAsleep: false,
            subpads: [0, 1, 2, 3].map(() => ({
                valid: false, mask: 0, primary: 255,
                requestedType: C.TYPE_DEFAULT, virtualType: C.TYPE_DEFAULT
            }))
        };
    }
    function emptyStatus() {
        return [0, 1, 2, 3].map(() => ({
            playerIndex: 255, playerLeds: 0, rgb: null
        }));
    }
    function emptyRoster() {
        return { valid: false, ports: [0, 1, 2, 3].map(() => ({ present: 0, gyro: 0, name: '' })) };
    }
    const state = {
        connected: false,
        assignment: emptyAssignment(),
        status: emptyStatus(),
        roster: emptyRoster(),
        battery: { percent: null, charging: false }
    };
    function resetState() {
        state.connected = false;
        state.assignment = emptyAssignment();
        state.status = emptyStatus();
        state.roster = emptyRoster();
        emitStateChanged();
    }
    const stateListeners = [];
    function onStateChanged(fn) { stateListeners.push(fn); }
    function emitStateChanged() {
        stateListeners.forEach(fn => { try { fn(state); } catch (_) {} });
    }
    const S2_TYPES = [C.TYPE_PRO_S2, C.TYPE_JOYCON_L_S2, C.TYPE_JOYCON_R_S2, C.TYPE_JOYCON_PAIR_S2];
    function s2Active() {
        return state.assignment.accepted
            && state.assignment.subpads.some(sp => sp.valid && S2_TYPES.includes(sp.virtualType));
    }
    // NFC lives on Pro 2 / right Joy-Con 2 virtual controllers (main_window.cpp rule).
    function s2NfcAssigned() {
        return state.assignment.accepted && state.assignment.subpads.some(sp =>
            sp.valid && (sp.mask & 0x01)
            && (sp.virtualType === C.TYPE_PRO_S2
                || sp.virtualType === C.TYPE_JOYCON_R_S2
                || sp.virtualType === C.TYPE_JOYCON_PAIR_S2));
    }
    function s2AudioEligible() {
        return state.assignment.accepted && state.assignment.subpads.some(sp =>
            sp.valid && sp.virtualType === C.TYPE_PRO_S2);
    }

    // ── Feedback packet parsers (server -> client) ───────────────────────────
    function parseAssignment(view) {
        const a = state.assignment;
        const flags = view.getUint8(5);
        const subpad = view.getUint8(7);
        a.serverFull = !!(flags & C.CLIENT_ASSIGNMENT_FLAG_SERVER_FULL);
        a.switchAsleep = !!(flags & C.CLIENT_ASSIGNMENT_FLAG_SWITCH_ASLEEP);
        if (flags & C.CLIENT_ASSIGNMENT_FLAG_ACCEPTED) {
            a.accepted = true;
            a.serverSlot = view.getUint8(6);
        }
        a.activeClients = view.getUint8(12);
        a.maxClients = view.getUint8(13);
        a.freeSlots = view.getUint8(14);
        if (subpad < 4) {
            const sp = a.subpads[subpad];
            sp.mask = view.getUint8(8);
            sp.primary = view.getUint8(9);
            sp.requestedType = view.getUint8(10);
            sp.virtualType = view.getUint8(11);
            sp.valid = !!(flags & C.CLIENT_ASSIGNMENT_FLAG_ASSIGNMENT_VALID);
        }
        emitStateChanged();
        return { flags, subpad };
    }
    function parseControllerStatus(view) {
        const subpad = view.getUint8(5);
        if (subpad >= 4) return null;
        const st = state.status[subpad];
        st.playerIndex = view.getUint8(6);
        st.playerLeds = view.getUint8(7);
        const sflags = view.getUint8(11);
        st.rgb = (sflags & C.CONTROLLER_STATUS_FLAG_BODY_RGB_VALID)
            ? [view.getUint8(8), view.getUint8(9), view.getUint8(10)] : null;
        emitStateChanged();
        return st;
    }
    function parseRoster(view) {
        const ports = [];
        for (let i = 0; i < 4; i++) {
            const off = 8 + i * C.ROSTER_ENTRY_SIZE;
            const present = view.getUint8(off);
            const gyro = view.getUint8(off + 1);
            let name = '';
            for (let k = 0; k < C.ROSTER_NAME_CAP; k++) {
                const ch = view.getUint8(off + 2 + k);
                if (ch === 0) break;
                name += String.fromCharCode(ch);
            }
            ports.push({ present, gyro, name });
        }
        state.roster = { valid: true, ports };
        emitStateChanged();
        return state.roster;
    }
    function parseRumble(view) {
        return {
            subpad: view.getUint8(4),
            low: view.getUint8(5),
            high: view.getUint8(6),
            duration10ms: view.getUint8(7)
        };
    }
    function parsePrecisionRumble(view) {
        const r = parseRumble(view);
        r.precision = [];
        for (let i = 0; i < 8; i++) r.precision.push(view.getUint8(8 + i));
        return r;
    }
    function parseAmiiboRequest(view) {
        return {
            subpad: view.getUint8(4),
            requested: view.getUint8(5) !== 0,
            sequence: view.getUint8(6) | (view.getUint8(7) << 8)
        };
    }

    // ── Input packet building (client -> server) ─────────────────────────────
    function clampI16(v) { v = Math.round(v || 0); return Math.max(-32768, Math.min(32767, v)); }
    function clampU8(v) { v = Math.round(v || 0); return Math.max(0, Math.min(255, v)); }

    // Writes one full 48-byte extended pad report at `off`.
    //   s : {buttons, hat, lx, ly, rx, ry}
    //   ex: {present, extraBits, controllerType, motionSamples (3x[6]),
    //        motionFresh, batteryPercent, batteryCharging}
    function buildExtPad(view, off, s, ex) {
        ex = ex || {};
        for (let k = 0; k < C.EXT_REPORT_SIZE; k++) view.setUint8(off + k, 0);
        view.setUint16(off, (s.buttons || 0) & 0xFFFF, true);
        view.setUint8(off + 2, s.hat === undefined ? C.HAT_NEUTRAL : s.hat);
        view.setUint8(off + 3, clampU8(s.lx === undefined ? 128 : s.lx));
        view.setUint8(off + 4, clampU8(s.ly === undefined ? 128 : s.ly));
        view.setUint8(off + 5, clampU8(s.rx === undefined ? 128 : s.rx));
        view.setUint8(off + 6, clampU8(s.ry === undefined ? 128 : s.ry));
        view.setUint8(off + 7, (ex.present ? C.PAD_PRESENT : 0) | ((ex.extraBits || 0) & 0xFE));
        let statusFlags = 0;
        if (ex.motionSamples && ex.motionSamples.length === 3) {
            for (let m = 0; m < 3; m++)
                for (let v = 0; v < 6; v++)
                    view.setInt16(off + 8 + m * 12 + v * 2, clampI16(ex.motionSamples[m][v]), true);
            view.setUint8(off + 44, 1); // has_motion
            statusFlags |= C.EXT_STATUS_MOTION_FRESH_VALID;
            if (ex.motionFresh) statusFlags |= C.EXT_STATUS_MOTION_FRESH;
        }
        if (ex.batteryPercent !== null && ex.batteryPercent !== undefined) {
            view.setUint8(off + 45, clampU8(ex.batteryPercent));
            statusFlags |= C.EXT_STATUS_BATTERY_VALID;
            if (ex.batteryCharging) statusFlags |= C.EXT_STATUS_BATTERY_CHARGING;
        }
        view.setUint8(off + 46, statusFlags);
        view.setUint8(off + 47, ex.controllerType || 0);
    }

    // HOME / CAPTURE stick+system combos, honoring the settings toggles.
    function normalizeSystemShortcuts(buttons) {
        const captureCombo = settings.get('captureShortcut')
            && (buttons & C.BTN_MINUS) && (buttons & C.BTN_PLUS);
        const homeCombo = settings.get('homeShortcut')
            && (buttons & C.BTN_LSTICK) && (buttons & C.BTN_RSTICK);
        if (captureCombo) {
            buttons |= C.BTN_CAPTURE;
            buttons &= ~(C.BTN_MINUS | C.BTN_PLUS | C.BTN_HOME);
            if (homeCombo) buttons &= ~(C.BTN_LSTICK | C.BTN_RSTICK);
        } else if (homeCombo) {
            buttons |= C.BTN_HOME;
            buttons &= ~(C.BTN_LSTICK | C.BTN_RSTICK | C.BTN_CAPTURE);
        }
        return buttons;
    }

    function makeWsUrl() {
        const proto = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
        return proto + '//' + window.location.host + '/';
    }

    // ── Motion (browser DeviceMotion -> Switch motion samples) ───────────────
    // Same math as the historical mobile.js path; shared by both pages now.
    const motion = {
        enabled: false,
        samples: [],
        revision: 0,
        _sentRevision: -1,
        _listener: null,
        get supported() { return caps.hasDeviceMotion && !caps.isNative; },
        // iOS requires a user-gesture-initiated permission request.
        async enable() {
            if (motion.enabled || !motion.supported) return motion.enabled;
            try {
                if (caps.needsMotionPermission
                        && await DeviceMotionEvent.requestPermission() !== 'granted')
                    return false;
                motion._listener = onDeviceMotion;
                window.addEventListener('devicemotion', motion._listener);
                motion.enabled = true;
            } catch (_) { return false; }
            return true;
        },
        disable() {
            if (!motion.enabled) return;
            window.removeEventListener('devicemotion', motion._listener);
            motion._listener = null;
            motion.enabled = false;
            motion.samples = [];
        },
        // Consume for one outgoing packet: marks freshness exactly once.
        consume() {
            if (!motion.enabled || motion.samples.length !== 3) return null;
            const fresh = motion.revision !== motion._sentRevision;
            motion._sentRevision = motion.revision;
            return { samples: motion.samples, fresh };
        }
    };
    function screenRemap(x, y, z) {
        const angle = ((screen.orientation && screen.orientation.angle) || window.orientation || 0) % 360;
        if (angle === 90 || angle === -270) return [-y, x, z];
        if (angle === 180 || angle === -180) return [-x, -y, z];
        if (angle === 270 || angle === -90) return [y, -x, z];
        return [x, y, z];
    }
    function onDeviceMotion(e) {
        const a0 = e.accelerationIncludingGravity || e.acceleration;
        const r0 = e.rotationRate;
        if (!a0 || !r0) return;
        const a = screenRemap(a0.x || 0, a0.y || 0, a0.z || 0);
        // DeviceMotion rotationRate is degrees/sec; native gyro APIs use rad/s.
        const g = screenRemap(r0.beta || 0, r0.gamma || 0, r0.alpha || 0);
        const accelScale = 4096 / 9.80665, gyroScale = 16.384;
        const sample = [
            clampI16(-a[2] * accelScale), clampI16(-a[0] * accelScale), clampI16(a[1] * accelScale),
            clampI16(-g[2] * gyroScale), clampI16(-g[0] * gyroScale), clampI16(g[1] * gyroScale)
        ];
        for (let i = 3; i < 6; i++) if (Math.abs(sample[i]) <= 32) sample[i] = 0;
        motion.samples = [sample, sample, sample];
        motion.revision++;
    }

    // ── Battery (shared; previously mobile-only) ─────────────────────────────
    if (navigator.getBattery) {
        navigator.getBattery().then(b => {
            const update = () => {
                state.battery.percent = Number.isFinite(b.level)
                    ? Math.max(0, Math.min(100, Math.round(b.level * 100))) : null;
                state.battery.charging = !!b.charging;
            };
            update();
            b.addEventListener('levelchange', update);
            b.addEventListener('chargingchange', update);
        }).catch(() => { state.battery.percent = null; });
    }

    // ── Feature registry ─────────────────────────────────────────────────────
    // feat_*.js modules call NSCore.registerFeature({...hooks}). Hooks:
    //   id            : string
    //   onWsMessage(magic, view)   -> return true when the message was consumed
    //   onBuildFrame(frame)        -> mutate {state, ext} for pad 0 before send
    //   onConnect(ws) / onDisconnect()
    //   mountUI(page)              -> called on DOM ready ('index' | 'mobile')
    const features = [];
    function registerFeature(f) { features.push(f); }
    let activeWs = null;
    const dispatch = {
        wsMessage(magic, view) {
            for (const f of features) {
                try { if (f.onWsMessage && f.onWsMessage(magic, view)) return true; }
                catch (err) { console.error('[ns-core] feature ' + f.id + ' onWsMessage failed', err); }
            }
            return false;
        },
        buildFrame(frame) {
            for (const f of features) {
                try { if (f.onBuildFrame) f.onBuildFrame(frame); }
                catch (err) { console.error('[ns-core] feature ' + f.id + ' onBuildFrame failed', err); }
            }
        },
        connect(ws) {
            activeWs = ws;
            state.connected = true;
            emitStateChanged();
            for (const f of features) { try { if (f.onConnect) f.onConnect(ws); } catch (_) {} }
        },
        disconnect() {
            activeWs = null;
            for (const f of features) { try { if (f.onDisconnect) f.onDisconnect(); } catch (_) {} }
            resetState();
        },
        mountUI(page) {
            for (const f of features) {
                try { if (f.mountUI) f.mountUI(page); }
                catch (err) { console.error('[ns-core] feature ' + f.id + ' mountUI failed', err); }
            }
        },
        // Lets every feature contribute rows to the Settings drawer
        // (feat_settings owns the drawer shell and calls this when building).
        settingsUI(ui, page) {
            for (const f of features) {
                try { if (f.settingsUI) f.settingsUI(ui, page); }
                catch (err) { console.error('[ns-core] feature ' + f.id + ' settingsUI failed', err); }
            }
        }
    };
    function ws() { return activeWs; }
    function wsSend(buffer) {
        if (activeWs && activeWs.readyState === WebSocket.OPEN) { activeWs.send(buffer); return true; }
        return false;
    }

    // ── Small DOM helpers used by the feature UIs ────────────────────────────
    function el(tag, attrs, children) {
        const node = document.createElement(tag);
        if (attrs) for (const [k, v] of Object.entries(attrs)) {
            if (k === 'class') node.className = v;
            else if (k === 'text') node.textContent = v;
            else if (k === 'html') node.innerHTML = v;
            else if (k.startsWith('on')) node[k] = v;
            else node.setAttribute(k, v);
        }
        (children || []).forEach(ch => node.appendChild(ch));
        return node;
    }

    return {
        C, caps, settings, state, motion,
        onStateChanged, emitStateChanged, resetState,
        s2Active, s2NfcAssigned, s2AudioEligible,
        parseAssignment, parseControllerStatus, parseRoster,
        parseRumble, parsePrecisionRumble, parseAmiiboRequest,
        buildExtPad, normalizeSystemShortcuts, makeWsUrl,
        clampI16, clampU8, screenRemap,
        registerFeature, dispatch, ws, wsSend, el
    };
})();
