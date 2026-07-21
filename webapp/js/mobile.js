const CONTROLLER_JOYCON_L = 1, CONTROLLER_JOYCON_R = 2, CONTROLLER_PRO = 3;
// Joy-Con selection is a native-app feature. A normal browser is a generic
// Pro source and lets the server choose S1 Pro, S2 Pro, or HORI.
const nativeMobileHost = !!(window.NSBridge && typeof NSBridge.onTouchState === 'function');
let controllerType = parseInt(localStorage.getItem('nswc_controller_type') || String(CONTROLLER_PRO));
const isMobileClient = /Mobi|Android|iPhone|iPad/i.test(navigator.userAgent) || ('ontouchstart' in window && navigator.maxTouchPoints > 0);
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
    const gamepad = document.getElementById('gamepad');
    nsApplyControllerSkin(gamepad, controllerType);
    nsApplyControllerFaceButtons(gamepad, controllerType);
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
// Protocol constants live in ns_core.js; keep the historical local names.
const NC = NSCore.C;
const PROTO_MAGIC = NC.PROTO_MAGIC, PROTO_VERSION = NC.PROTO_VERSION_3;
const CLIENT_ASSIGNMENT_MAGIC = NC.CLIENT_ASSIGNMENT_MAGIC, CLIENT_ASSIGNMENT_SIZE = NC.CLIENT_ASSIGNMENT_SIZE;
const CLIENT_ASSIGNMENT_FLAG_ACCEPTED = NC.CLIENT_ASSIGNMENT_FLAG_ACCEPTED, CLIENT_ASSIGNMENT_FLAG_SERVER_FULL = NC.CLIENT_ASSIGNMENT_FLAG_SERVER_FULL, CLIENT_ASSIGNMENT_FLAG_PROFILE_UNSUPPORTED = NC.CLIENT_ASSIGNMENT_FLAG_PROFILE_UNSUPPORTED;
const CLIENT_NAMES_MAGIC = NC.CLIENT_NAMES_MAGIC, SERVER_INFO_VERSION = NC.SERVER_INFO_VERSION;
const ROSTER_NAME_CAP = NC.ROSTER_NAME_CAP, ROSTER_ENTRY_SIZE = NC.ROSTER_ENTRY_SIZE, CLIENT_NAMES_SIZE = NC.CLIENT_NAMES_SIZE;
const PAD_PRESENT = NC.PAD_PRESENT;
const FLAG_SINGLE_PAD = NC.FLAG_SINGLE_PAD;
const EXT_BUTTON_SL = NC.EXT_BUTTON_SL, EXT_BUTTON_SR = NC.EXT_BUTTON_SR;
const EXT_REPORT_SIZE = NC.EXT_REPORT_SIZE, PACKET_SIZE = NC.PACKET_SIZE;
const BTN_MINUS = NC.BTN_MINUS, BTN_PLUS = NC.BTN_PLUS, BTN_LSTICK = NC.BTN_LSTICK, BTN_RSTICK = NC.BTN_RSTICK;
const BTN_HOME = NC.BTN_HOME, BTN_CAPTURE = NC.BTN_CAPTURE;
let ws = null, loopId = null, seqCounter = 0, isConnected = false, connectTimeout = null;
let serverSlot = 255, serverFull = false, lastNamesSentMs = 0;
function sendTouchName() {
    if (!ws || ws.readyState !== WebSocket.OPEN) return;
    lastNamesSentMs = Date.now();
    const buf = new ArrayBuffer(CLIENT_NAMES_SIZE), v = new DataView(buf);
    v.setUint32(0, CLIENT_NAMES_MAGIC, true);
    v.setUint8(4, SERVER_INFO_VERSION);
    v.setUint8(8, 1); // pad 0 present
    v.setUint8(9, NSCore.motion.enabled ? 1 : 0); // gyro flag for the roster
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
    if (view.byteLength < 4) return;
    const magic = view.getUint32(0, true);
    if (magic === NC.ROSTER_MAGIC && view.byteLength === NC.ROSTER_SIZE) { NSCore.parseRoster(view); return; }
    if (view.byteLength !== CLIENT_ASSIGNMENT_SIZE || magic !== CLIENT_ASSIGNMENT_MAGIC) {
        NSCore.dispatch.wsMessage(magic, view); // rumble/status/amiibo/audio features
        return;
    }
    NSCore.parseAssignment(view);
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

// Motion (DeviceMotion) and battery are provided by ns_core.js now; the math
// and behavior are unchanged (ported verbatim from this file).
async function enableMotion() {
    if (!NSCore.settings.get('gyro')) return;
    await NSCore.motion.enable();
}
function resetTouchConnectionUi(text) {
    isConnected = false;
    NSCore.dispatch.disconnect();
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
        if (controllerType === CONTROLLER_JOYCON_L || controllerType === CONTROLLER_JOYCON_R) {
            // Landscape Joy-Con axes: left -> up, down -> left,
            // right -> down, and up -> right.
            physX = -dy;
            physY = dx;
        }
        
        state[axisX] = Math.round(((physX / maxDist) + 1) * 127.5);
        state[axisY] = Math.round(((physY / maxDist) + 1) * 127.5);
        publishTouchState();
    }
}
setupJoystick('lstick', 'lknob', 'lx', 'ly');
setupJoystick('rstick', 'rknob', 'rx', 'ry');
// Shared implementation honors the Home/Capture shortcut toggles in Settings.
function normalizeSystemShortcuts(buttons) { return NSCore.normalizeSystemShortcuts(buttons); }
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
    const m = NSCore.motion.consume();
    const frame = {
        state: {
            buttons: normalizeSystemShortcuts(state.buttons), hat: state.hat,
            lx: state.lx, ly: state.ly, rx: state.rx, ry: state.ry
        },
        ext: {
            present: true,
            extraBits: state.extraButtons,
            controllerType,
            motionSamples: m ? m.samples : null,
            motionFresh: m ? m.fresh : false,
            batteryPercent: NSCore.state.battery.percent,
            batteryCharging: NSCore.state.battery.charging
        }
    };
    NSCore.dispatch.buildFrame(frame); // features (e.g. audio caps refresh) may hook here
    const buffer = new ArrayBuffer(PACKET_SIZE), view = new DataView(buffer);
    view.setUint32(0, PROTO_MAGIC, true); view.setUint8(4, PROTO_VERSION); view.setUint8(5, FLAG_SINGLE_PAD);
    view.setUint16(6, 0, true); view.setUint32(8, seqCounter++, true); view.setBigUint64(12, BigInt(Date.now()*1000), true);
    NSCore.buildExtPad(view, 20, frame.state, frame.ext);
    for (let p = 1; p < 4; p++)
        NSCore.buildExtPad(view, 20 + p * EXT_REPORT_SIZE, {}, { present: false });
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
        isConnected = true; serverFull = false; serverSlot = 255; lastNamesSentMs = 0;
        NSCore.dispatch.connect(ws);
        const btn = document.getElementById('btnConnect');
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
NSCore.dispatch.mountUI('mobile');
