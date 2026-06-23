const defLayout = {
    'btn-zl': {l:4, t:4, w:14, h:8}, 'btn-l': {l:4, t:14, w:14, h:8},
    'btn-zr': {l:82, t:4, w:14, h:8}, 'btn-r': {l:82, t:14, w:14, h:8},
    'btn-minus': {l:38, t:5, w:6}, 'btn-plus': {l:56, t:5, w:6},
    'btn-capture': {l:42, t:18, w:5}, 'btn-home': {l:53, t:18, w:5},
    'lstick': {l:6, t:35, w:16}, 'btn-ls': {l:2, t:65, w:5},
    'dpad': {l:22, t:60, w:16},
    'abxy': {l:78, t:35, w:16},
    'rstick': {l:62, t:60, w:16}, 'btn-rs': {l:85, t:80, w:5}
};
let layout = JSON.parse(localStorage.getItem('nswc_layout')) || defLayout;
function applyLayout() {
    for(let id in defLayout) {
        let el = document.getElementById(id);
        if(!el) continue;
        let conf = layout[id] || defLayout[id];
        if(conf.hide) { el.style.display = 'none'; }
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
const PROTO_MAGIC = 0x4E535743, PROTO_VERSION = 5;
const PAD_PRESENT = 1;
const FLAG_SINGLE_PAD = 0x04;
const EXT_REPORT_SIZE = 48, PACKET_SIZE = 212;
const BTN_MINUS = 1<<8, BTN_PLUS = 1<<9, BTN_LSTICK = 1<<10, BTN_RSTICK = 1<<11;
const BTN_HOME = 1<<12, BTN_CAPTURE = 1<<13;
let ws = null, loopId = null, seqCounter = 0, isConnected = false, connectTimeout = null;
let state = { buttons: 0, hat: 8, lx: 128, ly: 128, rx: 128, ry: 128 };
const buttonControls = Array.from(document.querySelectorAll('.btn-map,.btn-dpad'));
const activeTouchControls = new Map();
const allTouchButtonBits = buttonControls
    .filter(el => el.classList.contains('btn-map'))
    .reduce((mask, el) => mask | parseInt(el.dataset.btn), 0);
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
        const el = node.closest && node.closest('.btn-map,.btn-dpad');
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
    dpad.u = dpad.d = dpad.l = dpad.r = false;
    buttonControls.forEach(el => el.classList.remove('active'));
    for (const info of activeTouchControls.values()) {
        if (!info) continue;
        info.el.classList.add('active');
        if (info.kind === 'button') state.buttons |= info.bit;
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
        state[axisX] = Math.round(((dx / maxDist) + 1) * 127.5);
        state[axisY] = Math.round(((dy / maxDist) + 1) * 127.5);
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
function publishTouchState() {
    const sendButtons = normalizeSystemShortcuts(state.buttons);
    if (window.NSBridge && typeof NSBridge.onTouchState === 'function') {
        NSBridge.onTouchState(sendButtons, state.hat, state.lx, state.ly, state.rx, state.ry);
        return true;
    }
    if (window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.nsBridge) {
        window.webkit.messageHandlers.nsBridge.postMessage({type:'touchState', buttons:sendButtons, hat:state.hat, lx:state.lx, ly:state.ly, rx:state.rx, ry:state.ry});
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
    for(let p=1; p<4; p++) {
        off = 20 + (p*EXT_REPORT_SIZE); view.setUint16(off, 0, true); view.setUint8(off+2, 8);
        view.setUint8(off+3, 128); view.setUint8(off+4, 128); view.setUint8(off+5, 128); view.setUint8(off+6, 128); view.setUint8(off+7, 0);
        for(let k=8; k<EXT_REPORT_SIZE; k++) view.setUint8(off+k, 0);
    }
    ws.send(buffer);
}
document.getElementById('btnConnect').onclick = async () => {
    if (isConnected) { ws.close(); return; }
    if (document.documentElement.requestFullscreen) { document.documentElement.requestFullscreen().catch(()=>{}); }
    const wsUrl = makeWsUrl();
    ws = new WebSocket(wsUrl); ws.binaryType = "arraybuffer";
    ws.onopen = () => {
        isConnected = true; const btn = document.getElementById('btnConnect');
        btn.innerText = "Connected"; btn.classList.add('connected');
        connectTimeout = setTimeout(() => {
            if (window._connectionFailed) { window._connectionFailed = false; return; }
            btn.style.display = 'none'; document.getElementById('statusDot').style.display = 'block';
        }, 2000);
        publishTouchState();
        loopId = setInterval(sendPacket, 16);
    };
    ws.onerror = () => alert("Connection failed!");
    ws.onclose = () => {
        isConnected = false; clearInterval(loopId); clearTimeout(connectTimeout);
        const btn = document.getElementById('btnConnect');
        btn.style.display = 'block'; btn.innerText = "Connect"; btn.classList.remove('connected');
        document.getElementById('statusDot').style.display = 'none';
    };
};
document.getElementById('statusDot').onclick = () => { if(ws) ws.close(); };
