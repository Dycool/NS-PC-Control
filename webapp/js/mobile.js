const CONTROLLER_JOYCON_L = 1, CONTROLLER_JOYCON_R = 2, CONTROLLER_PRO = 3;
// Joy-Con selection is a native-app feature. A normal browser is a generic
// Pro source and lets the server choose S1 Pro, S2 Pro, or HORI.
const nativeMobileHost = !!(window.NSBridge && typeof NSBridge.onTouchState === 'function');
let controllerType = parseInt(localStorage.getItem('nswc_controller_type') || String(CONTROLLER_PRO));
if (![CONTROLLER_JOYCON_L, CONTROLLER_JOYCON_R, CONTROLLER_PRO].includes(controllerType)) controllerType = CONTROLLER_PRO;
if (!nativeMobileHost && !isMobileClient) controllerType = CONTROLLER_PRO;
const defLayout = NSControllerLayouts[controllerType];
let layout = nsLoadControllerLayout(controllerType);
const joyconLeftOnly = new Set(['btn-zl','btn-l','btn-minus','btn-capture','lstick','btn-ls','dpad','btn-sl','btn-sr']);
const joyconRightOnly = new Set(['btn-zr','btn-r','btn-plus','btn-home','rstick','btn-rs','abxy','btn-sl','btn-sr']);
function allowedForController(id) {
    if (controllerType === CONTROLLER_PRO) return id !== 'btn-sl' && id !== 'btn-sr';
    return controllerType === CONTROLLER_JOYCON_L ? joyconLeftOnly.has(id) : joyconRightOnly.has(id);
}
function applyLayout() {
    nsApplyControllerSkin(document.getElementById('gamepad'), controllerType);
    for(let id of NSControllerControlIds) {
        let el = document.getElementById(id);
        if(!el) continue;
        let conf = layout[id] || defLayout[id] || NSControllerLayouts[CONTROLLER_PRO][id];
        if(conf.hide || !allowedForController(id)) { el.style.display = 'none'; }
        else {
            if(id === 'dpad' || id === 'abxy') el.style.display = 'grid';
            else el.style.display = 'flex';
            el.style.left = conf.l + '%'; el.style.top = conf.t + '%'; el.style.width = conf.w + '%';
            if(conf.h) el.style.height = conf.h + '%';
            else { el.style.aspectRatio = '1 / 1'; el.style.height = 'auto'; }
        }
    }
}
applyLayout();
const PROTO_MAGIC = 0x4E535743, PROTO_VERSION = 6;
const CLIENT_ASSIGNMENT_MAGIC = 0x4E534341, CLIENT_ASSIGNMENT_SIZE = 16;
const CLIENT_ASSIGNMENT_FLAG_ACCEPTED = 0x01, CLIENT_ASSIGNMENT_FLAG_SERVER_FULL = 0x02, CLIENT_ASSIGNMENT_FLAG_PROFILE_UNSUPPORTED = 0x10;
const CLIENT_NAMES_MAGIC = 0x4E53434E, SERVER_INFO_VERSION = 1;
const ROSTER_NAME_CAP = 48, ROSTER_ENTRY_SIZE = 50, CLIENT_NAMES_SIZE = 224;
const PAD_PRESENT = 1;
const FLAG_SINGLE_PAD = 0x04;
const EXT_STATUS_BATTERY_VALID = 0x01;
const EXT_STATUS_BATTERY_CHARGING = 0x02;
const EXT_STATUS_MOTION_FRESH = 0x04;
const EXT_STATUS_MOTION_FRESH_VALID = 0x08;
const EXT_BUTTON_SL = 0x10, EXT_BUTTON_SR = 0x20;
const EXT_REPORT_SIZE = 48, PACKET_SIZE = 212;
const BTN_MINUS = 1<<8, BTN_PLUS = 1<<9, BTN_LSTICK = 1<<10, BTN_RSTICK = 1<<11;
const BTN_HOME = 1<<12, BTN_CAPTURE = 1<<13;
let ws = null, loopId = null, seqCounter = 0, isConnected = false, connectTimeout = null;
let serverSlot = 255, serverFull = false, lastNamesSentMs = 0;
function sendTouchName() {
    if (!ws || ws.readyState !== WebSocket.OPEN) return;
    lastNamesSentMs = Date.now();
    const buf = new ArrayBuffer(CLIENT_NAMES_SIZE), v = new DataView(buf);
    v.setUint32(0, CLIENT_NAMES_MAGIC, true);
    v.setUint8(4, SERVER_INFO_VERSION);
    v.setUint8(8, 1); // pad 0 present
    v.setUint8(9, 0); // no gyro flag
    let touchName = 'Mobile';
    if (/iPhone|iPad|iPod/i.test(navigator.userAgent)) {
        touchName = 'iOS Controller';
    } else if (/Android/i.test(navigator.userAgent)) {
        touchName = 'Android Controller';
    }
    const name = touchName.slice(0, ROSTER_NAME_CAP - 1);
    for (let k = 0; k < name.length; k++) v.setUint8(8 + 2 + k, name.charCodeAt(k) & 0xff);
    ws.send(buf);
}
function handleTouchWsBinaryMessage(ev) {
    if (!(ev.data instanceof ArrayBuffer)) return;
    const view = new DataView(ev.data);
    if (view.byteLength !== CLIENT_ASSIGNMENT_SIZE || view.getUint32(0, true) !== CLIENT_ASSIGNMENT_MAGIC) return;
    const flags = view.getUint8(5);
    if (flags & CLIENT_ASSIGNMENT_FLAG_PROFILE_UNSUPPORTED) {
        resetTouchConnectionUi('Switch 2 mode does not support Joy-Con L + R');
        try { if (ws) ws.close(); } catch (_) {}
        alert('Switch 2 mode supports one controller only; Joy-Con L + R is not supported.');
        return;
    }
    if (flags & CLIENT_ASSIGNMENT_FLAG_SERVER_FULL) {
        serverFull = true;
        resetTouchConnectionUi('Server full');
        try { if (ws) ws.close(); } catch (_) {}
        alert('Server full: all virtual controller slots are in use.');
        return;
    }
    if (flags & CLIENT_ASSIGNMENT_FLAG_ACCEPTED) {
        serverSlot = view.getUint8(6);
        const subpad = view.getUint8(7);
        const mask = view.getUint8(8);
        const btn = document.getElementById('btnConnect');
        if (btn && btn.style.display !== 'none' && subpad === 0) {
            const ports = [];
            for (let h=0; h<4; h++) if (mask & (1 << h)) ports.push(`P${h+1}`);
            btn.innerText = ports.length ? `Connected ${ports.join('+')}` : 'Connected';
        }
    }
}

let touchBatteryPercent = null;
let touchBatteryCharging = false;
let motionSamples = [];
let motionEnabled = false;
let motionRevision = 0, sentMotionRevision = -1;
const clampI16 = v => Math.max(-32768, Math.min(32767, Math.round(v || 0)));
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
    // DeviceMotion rotationRate is degrees/sec; native mobile gyro APIs use radians/sec.
    const g = screenRemap(r0.beta || 0, r0.gamma || 0, r0.alpha || 0);
    const accelScale = 4096 / 9.80665, gyroScale = 16.384;
    const sample = [
        clampI16(-a[2] * accelScale), clampI16(-a[0] * accelScale), clampI16(a[1] * accelScale),
        clampI16(-g[2] * gyroScale), clampI16(-g[0] * gyroScale), clampI16(g[1] * gyroScale)
    ];
    for (let i=3; i<6; i++) if (Math.abs(sample[i]) <= 32) sample[i] = 0;
    motionSamples = [sample, sample, sample];
    motionRevision++;
}
async function enableMotion() {
    if (motionEnabled || typeof DeviceMotionEvent === 'undefined') return;
    try {
        if (typeof DeviceMotionEvent.requestPermission === 'function' && await DeviceMotionEvent.requestPermission() !== 'granted') return;
        window.addEventListener('devicemotion', onDeviceMotion);
        motionEnabled = true;
    } catch (_) {}
}
if (navigator.getBattery) {
    navigator.getBattery().then(b => {
        const update = () => {
            touchBatteryPercent = Number.isFinite(b.level) ? Math.max(0, Math.min(100, Math.round(b.level * 100))) : null;
            touchBatteryCharging = !!b.charging;
        };
        update();
        b.addEventListener('levelchange', update);
        b.addEventListener('chargingchange', update);
    }).catch(() => { touchBatteryPercent = null; });
}
function resetTouchConnectionUi(text) {
    isConnected = false;
    if (loopId) { clearInterval(loopId); loopId = null; }
    if (connectTimeout) { clearTimeout(connectTimeout); connectTimeout = null; }
    const btn = document.getElementById('btnConnect');
    if (btn) {
        btn.style.display = 'block';
        btn.innerText = 'Connect';
        btn.classList.remove('connected');
    }
    const dot = document.getElementById('statusDot');
    if (dot) dot.style.display = 'none';
    if (text) window._connectionFailed = (text === 'Connection failed' || text === 'Server full');
}
window.__nsTouchDisconnected = resetTouchConnectionUi;
let state = { buttons: 0, extraButtons: 0, hat: 8, lx: 128, ly: 128, rx: 128, ry: 128 };
const buttonControls = Array.from(document.querySelectorAll('.btn-map,.btn-extra,.btn-dpad'));
const activeTouchControls = new Map();
const allTouchButtonBits = buttonControls
    .filter(el => el.classList.contains('btn-map'))
    .reduce((mask, el) => mask | parseInt(el.dataset.btn), 0);
const allTouchExtraBits = buttonControls
    .filter(el => el.classList.contains('btn-extra'))
    .reduce((mask, el) => mask | parseInt(el.dataset.extra), 0);
const dpad = { u:false, d:false, l:false, r:false };
function updateHat() {
    if (dpad.u && dpad.r) state.hat = 1; else if (dpad.u && dpad.l) state.hat = 7;
    else if (dpad.d && dpad.r) state.hat = 3; else if (dpad.d && dpad.l) state.hat = 5;
    else if (dpad.u) state.hat = 0; else if (dpad.d) state.hat = 4;
    else if (dpad.l) state.hat = 6; else if (dpad.r) state.hat = 2; else state.hat = 8;
}
function controlForElement(el) {
    if (!el) return null;
    if (el.classList.contains('btn-map')) return { el, kind:'button', bit:parseInt(el.dataset.btn) };
    if (el.classList.contains('btn-extra')) return { el, kind:'extra', bit:parseInt(el.dataset.extra) };
    if (el.classList.contains('btn-dpad')) return { el, kind:'dpad', dir:el.dataset.dir };
    return null;
}
function sameControl(a, b) {
    return !!a && !!b && a.el === b.el && a.kind === b.kind && a.bit === b.bit && a.dir === b.dir;
}
function pointInRect(x, y, r) { return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom; }
function pointInElementId(id, x, y) {
    const el = document.getElementById(id);
    return !!el && el.offsetParent !== null && pointInRect(x, y, el.getBoundingClientRect());
}
function getControlAt(x, y) {
    const stack = document.elementsFromPoint ? document.elementsFromPoint(x, y) : [];
    for (const node of stack) {
        const el = node.closest && node.closest('.btn-map,.btn-extra,.btn-dpad');
        if (el) return controlForElement(el);
    }
    if (pointInElementId('lstick', x, y) || pointInElementId('rstick', x, y)) return null;

    // Forgiving fallback: treat the whole visible button area, plus a small
    // finger-radius margin, as pressable. This avoids the "only the exact
    // middle works" feeling on iPhone Safari.
    const hitSlop = Math.max(14, Math.min(window.innerWidth, window.innerHeight) * 0.025);
    let best = null, bestScore = Infinity;
    for (const el of buttonControls) {
        if (el.offsetParent === null) continue;
        const r = el.getBoundingClientRect();
        if (x < r.left - hitSlop || x > r.right + hitSlop || y < r.top - hitSlop || y > r.bottom + hitSlop) continue;
        const cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
        const score = (x - cx) * (x - cx) + (y - cy) * (y - cy);
        if (score < bestScore) { bestScore = score; best = controlForElement(el); }
    }
    return best;
}
function recomputeTouchControls() {
    state.buttons &= ~allTouchButtonBits;
    state.extraButtons &= ~allTouchExtraBits;
    dpad.u = dpad.d = dpad.l = dpad.r = false;
    buttonControls.forEach(el => el.classList.remove('active'));
    for (const info of activeTouchControls.values()) {
        if (!info) continue;
        info.el.classList.add('active');
        if (info.kind === 'button') state.buttons |= info.bit;
        else if (info.kind === 'extra') state.extraButtons |= info.bit;
        else if (info.kind === 'dpad') dpad[info.dir] = true;
    }
    updateHat();
    publishTouchState();
}
function pressControl(touchId, info) {
    if (!info) return false;
    const prev = activeTouchControls.get(touchId);
    if (sameControl(prev, info)) return true;
    activeTouchControls.set(touchId, info);
    recomputeTouchControls();
    return true;
}
function releaseControl(touchId) {
    if (!activeTouchControls.has(touchId)) return false;
    activeTouchControls.delete(touchId);
    recomputeTouchControls();
    return true;
}
const padSurface = document.getElementById('gamepad');
padSurface.addEventListener('touchstart', e => {
    let handled = false;
    for (const t of e.changedTouches) {
        const info = getControlAt(t.clientX, t.clientY);
        if (info) handled = pressControl(t.identifier, info) || handled;
    }
    if (handled) e.preventDefault();
}, {passive:false});
padSurface.addEventListener('touchmove', e => {
    let handled = false;
    for (const t of e.changedTouches) {
        if (!activeTouchControls.has(t.identifier)) continue;
        const info = getControlAt(t.clientX, t.clientY);
        const prev = activeTouchControls.get(t.identifier);
        if (info && sameControl(prev, info)) { handled = true; continue; }
        if (info) pressControl(t.identifier, info);
        else releaseControl(t.identifier);
        handled = true;
    }
    if (handled) e.preventDefault();
}, {passive:false});
padSurface.addEventListener('touchend', e => {
    let handled = false;
    for (const t of e.changedTouches) handled = releaseControl(t.identifier) || handled;
    if (handled) e.preventDefault();
}, {passive:false});
padSurface.addEventListener('touchcancel', e => {
    let handled = false;
    for (const t of e.changedTouches) handled = releaseControl(t.identifier) || handled;
    if (handled) e.preventDefault();
}, {passive:false});
function makeWsUrl() {
    const proto = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
    return `${proto}//${window.location.host}/`;
}
function setupJoystick(baseId, knobId, axisX, axisY) {
    const base = document.getElementById(baseId), knob = document.getElementById(knobId);
    if(!base) return;
    let activeTouch = null;
    const reset = () => { state[axisX] = 128; state[axisY] = 128; knob.style.transform = `translate(0px, 0px)`; activeTouch = null; publishTouchState(); };
    base.addEventListener('touchstart', e => { e.preventDefault(); activeTouch = e.changedTouches[0].identifier; updateJoy(e.changedTouches[0]); }, {passive:false});
    base.addEventListener('touchmove', e => {
        e.preventDefault();
        for (let i=0; i<e.changedTouches.length; i++) if (e.changedTouches[i].identifier === activeTouch) updateJoy(e.changedTouches[i]);
    }, {passive:false});
    base.addEventListener('touchend', e => { for (let i=0; i<e.changedTouches.length; i++) if (e.changedTouches[i].identifier === activeTouch) reset(); }, {passive:false});
    base.addEventListener('touchcancel', e => { for (let i=0; i<e.changedTouches.length; i++) if (e.changedTouches[i].identifier === activeTouch) reset(); }, {passive:false});
    function updateJoy(t) {
        const rect = base.getBoundingClientRect(); const maxDist = rect.width / 2;
        let dx = t.clientX - (rect.left + rect.width/2), dy = t.clientY - (rect.top + rect.height/2);
        let dist = Math.sqrt(dx*dx + dy*dy);
        if (dist > maxDist) { dx = (dx/dist)*maxDist; dy = (dy/dist)*maxDist; }
        knob.style.transform = `translate(${dx}px, ${dy}px)`;
        
        let physX = dx, physY = dy;
        if (controllerType === 1) { // JOYCON_L
            physX = -dy; physY = dx;
        } else if (controllerType === 2) { // JOYCON_R
            physX = dy; physY = -dx;
        }
        
        state[axisX] = Math.round(((physX / maxDist) + 1) * 127.5);
        state[axisY] = Math.round(((physY / maxDist) + 1) * 127.5);
        publishTouchState();
    }
}
setupJoystick('lstick', 'lknob', 'lx', 'ly');
setupJoystick('rstick', 'rknob', 'rx', 'ry');
function normalizeSystemShortcuts(buttons) {
    const captureCombo = (buttons & BTN_MINUS) && (buttons & BTN_PLUS);
    const homeCombo = (buttons & BTN_LSTICK) && (buttons & BTN_RSTICK);
    if (captureCombo) {
        buttons |= BTN_CAPTURE;
        buttons &= ~(BTN_MINUS | BTN_PLUS | BTN_HOME);
        if (homeCombo) buttons &= ~(BTN_LSTICK | BTN_RSTICK);
    } else if (homeCombo) {
        buttons |= BTN_HOME;
        buttons &= ~(BTN_LSTICK | BTN_RSTICK | BTN_CAPTURE);
    }
    return buttons;
}
let publishedControllerType = null;
let publishedExtraButtons = null;
function publishControllerType() {
    if (publishedControllerType === controllerType) return;
    publishedControllerType = controllerType;
    // Newer apps expose a dedicated method; older apps just default to Pro.
    if (window.NSBridge && typeof NSBridge.onTouchControllerType === 'function') {
        try { NSBridge.onTouchControllerType(controllerType); } catch (_) {}
    }
}
function publishExtraButtons() {
    if (publishedExtraButtons === state.extraButtons) return;
    publishedExtraButtons = state.extraButtons;
    if (window.NSBridge && typeof NSBridge.onTouchExtraButtons === 'function') {
        try { NSBridge.onTouchExtraButtons(state.extraButtons); } catch (_) {}
    }
}
function publishTouchState() {
    const sendButtons = normalizeSystemShortcuts(state.buttons);
    if (window.NSBridge && typeof NSBridge.onTouchState === 'function') {
        // Keep the historical 6-arg call: the Android JS bridge resolves
        // methods by argument count, so adding a 7th argument breaks touch
        // input on APKs that predate controller-type support.
        publishControllerType();
        publishExtraButtons();
        NSBridge.onTouchState(sendButtons, state.hat, state.lx, state.ly, state.rx, state.ry);
        return true;
    }
    if (window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.nsBridge) {
        window.webkit.messageHandlers.nsBridge.postMessage({type:'touchState', buttons:sendButtons, hat:state.hat, lx:state.lx, ly:state.ly, rx:state.rx, ry:state.ry, controllerType});
        return true;
    }
    return false;
}
function sendPacket() {
    if (!ws || ws.readyState !== WebSocket.OPEN) return;
    if (publishTouchState()) return;
    const buffer = new ArrayBuffer(PACKET_SIZE), view = new DataView(buffer);
    view.setUint32(0, PROTO_MAGIC, true); view.setUint8(4, PROTO_VERSION); view.setUint8(5, FLAG_SINGLE_PAD);
    view.setUint16(6, 0, true); view.setUint32(8, seqCounter++, true); view.setBigUint64(12, BigInt(Date.now()*1000), true);
    let off = 20;
    const sendButtons = normalizeSystemShortcuts(state.buttons);
    view.setUint16(off, sendButtons, true); view.setUint8(off+2, state.hat);
    view.setUint8(off+3, state.lx); view.setUint8(off+4, state.ly); view.setUint8(off+5, state.rx); view.setUint8(off+6, state.ry); view.setUint8(off+7, PAD_PRESENT);
    for (let k = 8; k < EXT_REPORT_SIZE; k++) view.setUint8(off + k, 0);
    if (motionSamples.length === 3) {
        for (let s=0; s<3; s++) for (let v=0; v<6; v++) view.setInt16(off + 8 + s*12 + v*2, motionSamples[s][v], true);
        view.setUint8(off + 44, 1);
        let motionFlags = view.getUint8(off + 46) | EXT_STATUS_MOTION_FRESH_VALID;
        if (motionRevision !== sentMotionRevision) motionFlags |= EXT_STATUS_MOTION_FRESH;
        view.setUint8(off + 46, motionFlags);
        sentMotionRevision = motionRevision;
    }
    if (touchBatteryPercent !== null) {
        view.setUint8(off + 45, touchBatteryPercent);
        let batteryFlags = view.getUint8(off + 46) | EXT_STATUS_BATTERY_VALID;
        if (touchBatteryCharging) batteryFlags |= EXT_STATUS_BATTERY_CHARGING;
        view.setUint8(off + 46, batteryFlags);
    }
    view.setUint8(off + 7, PAD_PRESENT | state.extraButtons);
    view.setUint8(off + 47, controllerType);
    for(let p=1; p<4; p++) {
        off = 20 + (p*EXT_REPORT_SIZE); view.setUint16(off, 0, true); view.setUint8(off+2, 8);
        view.setUint8(off+3, 128); view.setUint8(off+4, 128); view.setUint8(off+5, 128); view.setUint8(off+6, 128); view.setUint8(off+7, 0);
        for(let k=8; k<EXT_REPORT_SIZE; k++) view.setUint8(off+k, 0);
    }
    ws.send(buffer);
    if (Date.now() - lastNamesSentMs > 2000) sendTouchName();
}
document.getElementById('btnConnect').onclick = async () => {
    if (isConnected) {
        if (ws) ws.close();
        resetTouchConnectionUi('Disconnected');
        return;
    }
    await enableMotion();
    if (document.documentElement.requestFullscreen) { document.documentElement.requestFullscreen().catch(()=>{}); }
    const wsUrl = makeWsUrl();
    ws = new WebSocket(wsUrl, "nspc-protocol"); ws.binaryType = "arraybuffer";
    ws.onmessage = handleTouchWsBinaryMessage;
    ws.onopen = () => {
        isConnected = true; serverFull = false; serverSlot = 255; lastNamesSentMs = 0; const btn = document.getElementById('btnConnect');
        btn.innerText = "Connected"; btn.classList.add('connected');
        connectTimeout = setTimeout(() => {
            if (window._connectionFailed) { window._connectionFailed = false; return; }
            btn.style.display = 'none'; document.getElementById('statusDot').style.display = 'block';
        }, 3000);
        publishTouchState();
        loopId = setInterval(sendPacket, 16);
    };
    ws.onerror = () => { resetTouchConnectionUi('Connection failed'); alert("Connection failed"); };
    ws.onclose = () => { resetTouchConnectionUi(serverFull ? 'Server full' : 'Disconnected'); };
};
document.getElementById('statusDot').onclick = () => { if(ws) ws.close(); };
