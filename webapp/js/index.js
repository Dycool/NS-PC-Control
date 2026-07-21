// FIX #20: 'use strict' enables stricter JS parsing and prevents accidental
// global variable creation from typos, catching many issues at runtime.
// A full IIFE/module wrapper is not applied here because index.js relies on
// window.onload and deferred DOM queries that span the module boundary.
'use strict';
// FIX #9: Removed unused SECRET constant.
// WebSocket traffic is trusted at the network level only (no HMAC); exposing
// a shared-secret constant in JS source served over HTTP is misleading.
// FIX #11 (security model): This webapp sends input over an unauthenticated
// WebSocket. Ensure the server port is not reachable from untrusted networks.
// Protocol constants live in ns_core.js (single source of truth shared with
// mobile.js and the feat_* modules); keep the historical local names.
const NC = NSCore.C;
const PROTO_MAGIC = NC.PROTO_MAGIC;
const CLIENT_ASSIGNMENT_MAGIC = NC.CLIENT_ASSIGNMENT_MAGIC;
const CLIENT_ASSIGNMENT_SIZE = NC.CLIENT_ASSIGNMENT_SIZE;
const CLIENT_ASSIGNMENT_FLAG_ACCEPTED = NC.CLIENT_ASSIGNMENT_FLAG_ACCEPTED;
const CLIENT_ASSIGNMENT_FLAG_SERVER_FULL = NC.CLIENT_ASSIGNMENT_FLAG_SERVER_FULL;
const CLIENT_ASSIGNMENT_FLAG_ASSIGNMENT_VALID = NC.CLIENT_ASSIGNMENT_FLAG_ASSIGNMENT_VALID;
const CLIENT_ASSIGNMENT_FLAG_PROFILE_UNSUPPORTED = NC.CLIENT_ASSIGNMENT_FLAG_PROFILE_UNSUPPORTED;
const CLIENT_NAMES_MAGIC = NC.CLIENT_NAMES_MAGIC;
const ROSTER_MAGIC = NC.ROSTER_MAGIC;
const SERVER_INFO_VERSION = NC.SERVER_INFO_VERSION;
const ROSTER_NAME_CAP = NC.ROSTER_NAME_CAP;
const ROSTER_ENTRY_SIZE = NC.ROSTER_ENTRY_SIZE;
const ROSTER_SIZE = NC.ROSTER_SIZE;
const CLIENT_NAMES_SIZE = NC.CLIENT_NAMES_SIZE;
// Version 6 (WEB_PROTO_VERSION_3): same 212-byte layout, but declares that the
// motion area carries 3 samples per pad (used when browser gyro is enabled).
const PROTO_VERSION = NC.PROTO_VERSION_3;
const PAD_PRESENT = NC.PAD_PRESENT;
const EXT_REPORT_SIZE = NC.EXT_REPORT_SIZE;
const PACKET_SIZE = NC.PACKET_SIZE;
const BTN_Y = NC.BTN_Y, BTN_B = NC.BTN_B, BTN_A = NC.BTN_A, BTN_X = NC.BTN_X;
const BTN_L = NC.BTN_L, BTN_R = NC.BTN_R, BTN_ZL = NC.BTN_ZL, BTN_ZR = NC.BTN_ZR;
const BTN_MINUS = NC.BTN_MINUS, BTN_PLUS = NC.BTN_PLUS, BTN_LSTICK = NC.BTN_LSTICK, BTN_RSTICK = NC.BTN_RSTICK;
const BTN_HOME = NC.BTN_HOME, BTN_CAPTURE = NC.BTN_CAPTURE;
const HAT_N = NC.HAT_N, HAT_NE = NC.HAT_NE, HAT_E = NC.HAT_E, HAT_SE = NC.HAT_SE,
      HAT_S = NC.HAT_S, HAT_SW = NC.HAT_SW, HAT_W = NC.HAT_W, HAT_NW = NC.HAT_NW, HAT_NEUTRAL = NC.HAT_NEUTRAL;
let ws = null;
let isConnected = false;
let loopId = null;
let seqCounter = 0;
let lastNamesSent = '';
let lastNamesSentMs = 0;
let lastActivePads = [];
let serverAssignment = { accepted:false, serverFull:false, serverSlot:255, masks:[0,0,0,0], primary:[255,255,255,255], activeClients:0, maxClients:4, freeSlots:0 };
function resetServerAssignment() { serverAssignment = { accepted:false, serverFull:false, serverSlot:255, masks:[0,0,0,0], primary:[255,255,255,255], activeClients:0, maxClients:4, freeSlots:0 }; }
let roster = { valid:false, ports:[{present:0,gyro:0,name:''},{present:0,gyro:0,name:''},{present:0,gyro:0,name:''},{present:0,gyro:0,name:''}] };
function resetRoster() { roster = { valid:false, ports:[{present:0,gyro:0,name:''},{present:0,gyro:0,name:''},{present:0,gyro:0,name:''},{present:0,gyro:0,name:''}] }; }
function parseRosterPacket(view) {
    const ports = [];
    for (let i = 0; i < 4; i++) {
        const off = 8 + i * ROSTER_ENTRY_SIZE;
        const present = view.getUint8(off);
        const gyro = view.getUint8(off + 1);
        let name = '';
        for (let k = 0; k < ROSTER_NAME_CAP; k++) {
            const ch = view.getUint8(off + 2 + k);
            if (ch === 0) break;
            name += String.fromCharCode(ch);
        }
        ports.push({ present, gyro, name });
    }
    roster = { valid:true, ports };
}
function handleAssignmentPacket(view) {
    NSCore.parseAssignment(view); // keeps NSCore.state (S2 gating, settings UI) in sync
    const flags = view.getUint8(5);
    const subpad = view.getUint8(7);
    serverAssignment.serverFull = !!(flags & CLIENT_ASSIGNMENT_FLAG_SERVER_FULL);
    if (flags & CLIENT_ASSIGNMENT_FLAG_ACCEPTED) {
        serverAssignment.accepted = true;
        serverAssignment.serverSlot = view.getUint8(6);
    }
    serverAssignment.activeClients = view.getUint8(12);
    serverAssignment.maxClients = view.getUint8(13);
    serverAssignment.freeSlots = view.getUint8(14);
    if (subpad < 4) {
        serverAssignment.masks[subpad] = view.getUint8(8);
        serverAssignment.primary[subpad] = view.getUint8(9);
    }
    if (flags & CLIENT_ASSIGNMENT_FLAG_PROFILE_UNSUPPORTED) {
        resetMainConnectionUi('Switch 2 mode does not support Joy-Con L + R');
        try { if (ws) ws.close(); } catch (_) {}
        alert('Switch 2 mode supports one controller only; Joy-Con L + R is not supported.');
    } else if (serverAssignment.serverFull) {
        resetMainConnectionUi('Server full');
        try { if (ws) ws.close(); } catch (_) {}
        alert('Server full: all virtual controller slots are in use.');
    }
}
function handleWsBinaryMessage(ev) {
    if (!(ev.data instanceof ArrayBuffer)) return;
    const view = new DataView(ev.data);
    if (view.byteLength < 4) return;
    const magic = view.getUint32(0, true);
    if (magic === CLIENT_ASSIGNMENT_MAGIC && view.byteLength === CLIENT_ASSIGNMENT_SIZE) handleAssignmentPacket(view);
    else if (magic === ROSTER_MAGIC && view.byteLength === ROSTER_SIZE) { parseRosterPacket(view); NSCore.parseRoster(view); }
    else NSCore.dispatch.wsMessage(magic, view); // rumble/status/amiibo/audio features
}

function resetMainConnectionUi(text) {
    isConnected = false;
    NSCore.dispatch.disconnect();
    resetServerAssignment();
    resetRoster();
    lastNamesSent = '';
    lastNamesSentMs = 0;
    if (loopId) { clearInterval(loopId); loopId = null; }
    const btn = document.getElementById('btnConnect');
    if (btn) btn.innerText = 'Connect';
    const kb = document.getElementById('kbMode');
    if (kb) kb.disabled = false;
    const status = document.getElementById('statusText');
    if (status && text) status.innerText = text;
}
window.__nsMainDisconnected = resetMainConnectionUi;
const keysDown = new Set();
const defaultBindings = {
    'BTN_Y': 'KeyZ', 'BTN_B': 'KeyX', 'BTN_A': 'KeyV', 'BTN_X': 'KeyC',
    'BTN_L': 'KeyQ', 'BTN_R': 'KeyE', 'BTN_ZL': 'Digit1', 'BTN_ZR': 'Digit2',
    'BTN_MINUS': 'Digit3', 'BTN_PLUS': 'Digit4',
    'BTN_LSTICK': 'ShiftLeft', 'BTN_RSTICK': 'ShiftRight',
    'BTN_HOME': 'Home', 'BTN_CAPTURE': 'PrintScreen',
    'DPAD_UP': 'ArrowUp', 'DPAD_DOWN': 'ArrowDown', 'DPAD_LEFT': 'ArrowLeft', 'DPAD_RIGHT': 'ArrowRight',
    'LSTICK_UP': 'KeyW', 'LSTICK_DOWN': 'KeyS', 'LSTICK_LEFT': 'KeyA', 'LSTICK_RIGHT': 'KeyD',
    'RSTICK_UP': 'KeyI', 'RSTICK_DOWN': 'KeyK', 'RSTICK_LEFT': 'KeyJ', 'RSTICK_RIGHT': 'KeyL'
};
let currentBindings = { ...defaultBindings };
let preEditBindings = {};
window.onload = () => {
    const isMobile = /Android|webOS|iPhone|iPad|iPod|BlackBerry|IEMobile|Opera Mini/i.test(navigator.userAgent);
    if (isMobile) {
        document.getElementById('kbModeContainer').style.display = 'none';
        document.getElementById('btnBindings').style.display = 'none';
        document.getElementById('btnMacros').style.display = 'none';
        document.getElementById('btnTouchControls').style.display = 'inline-block';
        document.getElementById('btnEditor').style.display = 'inline-block';
    }
    const savedMode = localStorage.getItem('nswc_mode');
    if (savedMode) document.getElementById('kbMode').value = savedMode;
    const savedBindings = localStorage.getItem('nswc_bindings');
    if (savedBindings) currentBindings = JSON.parse(savedBindings);
    wireMacroMenu();
    NSCore.dispatch.mountUI('index');
};
document.getElementById('kbMode').onchange = (e) => localStorage.setItem('nswc_mode', e.target.value);
window.addEventListener('keydown', (e) => {
    if (activeBindKey) { e.preventDefault(); remapKey(e.code); return; }
    const m = savedMacros.find(x => normalizeMacroKey(x.hotkey) && normalizeMacroKey(x.hotkey) === normalizeMacroKey(e.code));
    if (m && !macroHotkeyConflicts(e.code, savedMacros.indexOf(m))) { e.preventDefault(); runMacroServerSide(m); return; }
    if (!['INPUT','TEXTAREA','SELECT','BUTTON'].includes((e.target && e.target.tagName) || '')) e.preventDefault();
    keysDown.add(e.code);
});
window.addEventListener('keyup', (e) => { e.preventDefault(); keysDown.delete(e.code); });
function getNeutralState() { return { buttons: 0, hat: HAT_NEUTRAL, lx: 128, ly: 128, rx: 128, ry: 128 }; }
function getKeyboardState() {
    let buttons = 0, hat = HAT_NEUTRAL, lx = 128, ly = 128, rx = 128, ry = 128;
    if (keysDown.has(currentBindings['BTN_Y'])) buttons |= BTN_Y;
    if (keysDown.has(currentBindings['BTN_B'])) buttons |= BTN_B;
    if (keysDown.has(currentBindings['BTN_A'])) buttons |= BTN_A;
    if (keysDown.has(currentBindings['BTN_X'])) buttons |= BTN_X;
    if (keysDown.has(currentBindings['BTN_L'])) buttons |= BTN_L;
    if (keysDown.has(currentBindings['BTN_R'])) buttons |= BTN_R;
    if (keysDown.has(currentBindings['BTN_ZL'])) buttons |= BTN_ZL;
    if (keysDown.has(currentBindings['BTN_ZR'])) buttons |= BTN_ZR;
    if (keysDown.has(currentBindings['BTN_MINUS'])) buttons |= BTN_MINUS;
    if (keysDown.has(currentBindings['BTN_PLUS'])) buttons |= BTN_PLUS;
    if (keysDown.has(currentBindings['BTN_LSTICK'])) buttons |= BTN_LSTICK;
    if (keysDown.has(currentBindings['BTN_RSTICK'])) buttons |= BTN_RSTICK;
    if (keysDown.has(currentBindings['BTN_HOME'])) buttons |= BTN_HOME;
    if (keysDown.has(currentBindings['BTN_CAPTURE'])) buttons |= BTN_CAPTURE;
    const up = keysDown.has(currentBindings['DPAD_UP']), down = keysDown.has(currentBindings['DPAD_DOWN']);
    const left = keysDown.has(currentBindings['DPAD_LEFT']), right = keysDown.has(currentBindings['DPAD_RIGHT']);
    if (up && right) hat = HAT_NE; else if (up && left) hat = HAT_NW;
    else if (down && right) hat = HAT_SE; else if (down && left) hat = HAT_SW;
    else if (up) hat = HAT_N; else if (down) hat = HAT_S;
    else if (left) hat = HAT_W; else if (right) hat = HAT_E;
    if (keysDown.has(currentBindings['LSTICK_LEFT']) && !keysDown.has(currentBindings['LSTICK_RIGHT'])) lx = 0;
    else if (keysDown.has(currentBindings['LSTICK_RIGHT']) && !keysDown.has(currentBindings['LSTICK_LEFT'])) lx = 255;
    if (keysDown.has(currentBindings['LSTICK_UP']) && !keysDown.has(currentBindings['LSTICK_DOWN'])) ly = 0;
    else if (keysDown.has(currentBindings['LSTICK_DOWN']) && !keysDown.has(currentBindings['LSTICK_UP'])) ly = 255;
    if (keysDown.has(currentBindings['RSTICK_LEFT']) && !keysDown.has(currentBindings['RSTICK_RIGHT'])) rx = 0;
    else if (keysDown.has(currentBindings['RSTICK_RIGHT']) && !keysDown.has(currentBindings['RSTICK_LEFT'])) rx = 255;
    if (keysDown.has(currentBindings['RSTICK_UP']) && !keysDown.has(currentBindings['RSTICK_DOWN'])) ry = 0;
    else if (keysDown.has(currentBindings['RSTICK_DOWN']) && !keysDown.has(currentBindings['RSTICK_UP'])) ry = 255;
    return { buttons, hat, lx, ly, rx, ry };
}
function getGamepadState(pad) {
    if (!pad) return null;
    let buttons = 0, hat = HAT_NEUTRAL, lx = 128, ly = 128, rx = 128, ry = 128;
    if (pad.buttons[0]?.pressed) buttons |= BTN_B;
    if (pad.buttons[1]?.pressed) buttons |= BTN_A;
    if (pad.buttons[2]?.pressed) buttons |= BTN_Y;
    if (pad.buttons[3]?.pressed) buttons |= BTN_X;
    if (pad.buttons[4]?.pressed) buttons |= BTN_L;
    if (pad.buttons[5]?.pressed) buttons |= BTN_R;
    if (pad.buttons[6]?.pressed) buttons |= BTN_ZL;
    if (pad.buttons[7]?.pressed) buttons |= BTN_ZR;
    if (pad.buttons[8]?.pressed) buttons |= BTN_MINUS;
    if (pad.buttons[9]?.pressed) buttons |= BTN_PLUS;
    if (pad.buttons[10]?.pressed) buttons |= BTN_LSTICK;
    if (pad.buttons[11]?.pressed) buttons |= BTN_RSTICK;
    if (pad.buttons[16]?.pressed) buttons |= BTN_HOME;
    if (pad.buttons[17]?.pressed) buttons |= BTN_CAPTURE;
    if ((buttons & BTN_LSTICK) && (buttons & BTN_RSTICK)) buttons |= BTN_HOME;
    if ((buttons & BTN_MINUS) && (buttons & BTN_PLUS)) buttons |= BTN_CAPTURE;
    const pup = pad.buttons[12]?.pressed, pdown = pad.buttons[13]?.pressed;
    const pleft = pad.buttons[14]?.pressed, pright = pad.buttons[15]?.pressed;
    if (pup && pright) hat = HAT_NE; else if (pup && pleft) hat = HAT_NW;
    else if (pdown && pright) hat = HAT_SE; else if (pdown && pleft) hat = HAT_SW;
    else if (pup) hat = HAT_N; else if (pdown) hat = HAT_S;
    else if (pleft) hat = HAT_W; else if (pright) hat = HAT_E;
    const applyDeadzone = (val) => { val = Number(val || 0); if (Math.abs(val) < 0.15) return 128; return Math.max(0, Math.min(255, Math.round(((val + 1) / 2) * 255))); };
    if (pad.axes.length >= 2) {
        lx = applyDeadzone(pad.axes[0]); ly = applyDeadzone(pad.axes[1]);
    }
    if (pad.axes.length >= 4) {
        rx = applyDeadzone(pad.axes[2]); ry = applyDeadzone(pad.axes[3]);
    }
    return { buttons, hat, lx, ly, rx, ry };
}
function mergeStates(s1, s2) {
    if (!s1) return s2; if (!s2) return s1;
    return { buttons: s1.buttons | s2.buttons, hat: s1.hat !== HAT_NEUTRAL ? s1.hat : s2.hat,
        lx: s1.lx !== 128 ? s1.lx : s2.lx, ly: s1.ly !== 128 ? s1.ly : s2.ly,
        rx: s1.rx !== 128 ? s1.rx : s2.rx, ry: s1.ry !== 128 ? s1.ry : s2.ry };
}
// Shared implementation honors the Home/Capture shortcut toggles in Settings.
function normalizeSystemShortcuts(buttons) { return NSCore.normalizeSystemShortcuts(buttons); }
function clamp16(v) { return NSCore.clampI16(v); }
function makeWsUrl() { return NSCore.makeWsUrl(); }
let savedMacros = JSON.parse(localStorage.getItem('nswc_macros') || '[]');
if (savedMacros && Array.isArray(savedMacros.macros)) savedMacros = savedMacros.macros; if (!Array.isArray(savedMacros)) savedMacros = [];
let macroRunning = false, macroSteps = [], macroStepIndex = 0, macroStepUntil = 0, macroState = null;
let macroRecording = false, macroRecordLast = null, macroRecordSince = 0, macroRecorded = [];
function macroBtnBit(name) {
    name = String(name || '').trim().toUpperCase();
    const map = {Y:BTN_Y,B:BTN_B,A:BTN_A,X:BTN_X,L:BTN_L,R:BTN_R,ZL:BTN_ZL,ZR:BTN_ZR,MINUS:BTN_MINUS,'-':BTN_MINUS,PLUS:BTN_PLUS,'+':BTN_PLUS,LSTICK:BTN_LSTICK,LS:BTN_LSTICK,RSTICK:BTN_RSTICK,RS:BTN_RSTICK,HOME:BTN_HOME,CAPTURE:BTN_CAPTURE};
    return map[name] || 0;
}
function macroButtonsFromText(txt) { return String(txt || '').split(/[+|, ]+/).reduce((b, x) => b | macroBtnBit(x), 0); }
function macroButtonsToText(buttons) {
    const names = [['Y',BTN_Y],['B',BTN_B],['A',BTN_A],['X',BTN_X],['L',BTN_L],['R',BTN_R],['ZL',BTN_ZL],['ZR',BTN_ZR],['MINUS',BTN_MINUS],['PLUS',BTN_PLUS],['LSTICK',BTN_LSTICK],['RSTICK',BTN_RSTICK],['HOME',BTN_HOME],['CAPTURE',BTN_CAPTURE]];
    return names.filter(x => buttons & x[1]).map(x => x[0]).join('+') || 'WAIT';
}
function macroFrameFromState(s) { const axis=v=>v<80?-1:(v>176?1:0); s=s||getNeutralState(); return {buttons:s.buttons||0, hat:s.hat, lx:axis(s.lx), ly:axis(s.ly), rx:axis(s.rx), ry:axis(s.ry)}; }
function macroFrameEqual(a,b) { return !!a && !!b && a.buttons===b.buttons && a.hat===b.hat && a.lx===b.lx && a.ly===b.ly && a.rx===b.rx && a.ry===b.ry; }
function macroFrameToText(f) { const parts=[]; const btn=macroButtonsToText(f.buttons); if(btn!=='WAIT') parts.push(btn); if(f.hat===HAT_N) parts.push('DPAD_UP'); else if(f.hat===HAT_NE) parts.push('DPAD_UP','DPAD_RIGHT'); else if(f.hat===HAT_E) parts.push('DPAD_RIGHT'); else if(f.hat===HAT_SE) parts.push('DPAD_DOWN','DPAD_RIGHT'); else if(f.hat===HAT_S) parts.push('DPAD_DOWN'); else if(f.hat===HAT_SW) parts.push('DPAD_DOWN','DPAD_LEFT'); else if(f.hat===HAT_W) parts.push('DPAD_LEFT'); else if(f.hat===HAT_NW) parts.push('DPAD_UP','DPAD_LEFT'); if(f.lx<0) parts.push('LSTICK_LEFT'); else if(f.lx>0) parts.push('LSTICK_RIGHT'); if(f.ly<0) parts.push('LSTICK_UP'); else if(f.ly>0) parts.push('LSTICK_DOWN'); if(f.rx<0) parts.push('RSTICK_LEFT'); else if(f.rx>0) parts.push('RSTICK_RIGHT'); if(f.ry<0) parts.push('RSTICK_UP'); else if(f.ry>0) parts.push('RSTICK_DOWN'); return parts.join('+') || 'WAIT'; }
function macroCommandString(objOrText) {
    if (!objOrText) return '';
    if (typeof objOrText === 'string') { try { return macroCommandString(JSON.parse(objOrText)); } catch(e) { return objOrText; } }
    if (Array.isArray(objOrText)) return objOrText.map(s => `${s.buttons || s.button || s.cmd || 'WAIT'} ${s.ms || s.duration || 100}`).join('; ');
    return objOrText.commands || objOrText.macro || '';
}
const MACRO_JSON_MAX_BYTES = 50 * 1024 * 1024;
const MACRO_VALID_INPUTS = new Set(['A','B','X','Y','L','R','ZL','ZR','MINUS','-','PLUS','+','LSTICK','LS','RSTICK','RS','HOME','CAPTURE','DPAD_UP','DPAD_DOWN','DPAD_LEFT','DPAD_RIGHT','UP','DOWN','LEFT','RIGHT','LSTICK_UP','LSTICK_DOWN','LSTICK_LEFT','LSTICK_RIGHT','LS_UP','LS_DOWN','LS_LEFT','LS_RIGHT','RSTICK_UP','RSTICK_DOWN','RSTICK_LEFT','RSTICK_RIGHT','RS_UP','RS_DOWN','RS_LEFT','RS_RIGHT']);
function macroCommandsArray(objOrText) { const obj = (typeof objOrText === 'string') ? (()=>{ try { return JSON.parse(objOrText); } catch(_) { return {commands:objOrText}; } })() : objOrText; const c = obj && obj.commands !== undefined ? obj.commands : objOrText; if (Array.isArray(c)) return c.map(String); if (typeof c === 'string') return c.split(/[;\n\r]+/).map(x=>x.trim()).filter(Boolean); throw new Error('commands must be a string or array of strings'); }
const defLayout = {}; // Placeholder if referenced globally. In index it's not but we handle variables.
function validateMacro(objOrText) { const lines = macroCommandsArray(objOrText); if (!lines.length) throw new Error('no macro commands found'); for (const line of lines) { const m = line.trim().match(/^(.*?)\s+(\d+)$/); if (!m) throw new Error('missing duration: '+line); const cmd=m[1].trim(), ms=Number(m[2]); if (!Number.isSafeInteger(ms) || ms <= 0 || ms > 4294967295) throw new Error('invalid duration: '+line); if (cmd.toUpperCase()==='WAIT' || cmd.toUpperCase()==='LOOP') continue; const toks=cmd.split(/[+,|\s]+/).filter(Boolean); if (!toks.length) throw new Error('missing input: '+line); const seen=new Set(); for (const t0 of toks) { const t=t0.toUpperCase(); if (!MACRO_VALID_INPUTS.has(t)) throw new Error('unknown input '+t0+' in '+line); seen.add(t); } const has=(...xs)=>xs.some(x=>seen.has(x)); if (has('DPAD_UP','UP')&&has('DPAD_DOWN','DOWN')) throw new Error('DPAD up/down conflict: '+line); if (has('DPAD_LEFT','LEFT')&&has('DPAD_RIGHT','RIGHT')) throw new Error('DPAD left/right conflict: '+line); if (has('LSTICK_UP','LS_UP')&&has('LSTICK_DOWN','LS_DOWN')) throw new Error('left stick up/down conflict: '+line); if (has('LSTICK_LEFT','LS_LEFT')&&has('LSTICK_RIGHT','LS_RIGHT')) throw new Error('left stick left/right conflict: '+line); if (has('RSTICK_UP','RS_UP')&&has('RSTICK_DOWN','RS_DOWN')) throw new Error('right stick up/down conflict: '+line); if (has('RSTICK_LEFT','RS_LEFT')&&has('RSTICK_RIGHT','RS_RIGHT')) throw new Error('right stick left/right conflict: '+line); } return lines; }
function macroPrettyObject(objOrText) { const lines=validateMacro(objOrText); const obj=(typeof objOrText==='string')?(()=>{try{return JSON.parse(objOrText);}catch(_){return {commands:objOrText};}})():objOrText; return {name:(obj&&obj.name)||'Macro', commands:lines}; }
function parseMacro(objOrText) {
    const text = macroCommandString(objOrText).replace(/[\r\n]+/g, ';');
    const steps = [];
    let segmentStart = 0;
    // FIX #21: Track whether any LOOP was truncated so we can warn the user.
    let loopTruncated = false;
    const addStep = p => {
        const m = p.match(/^(.*?)\s+(\d+)$/);
        if (!m) return;
        // FIX #7: Allow duration 0 (no longer clamping to Math.max(1,...)).
        // The C++ server now accepts 0-duration steps; both parsers are consistent.
        const cmd = m[1].trim().toUpperCase(), ms = parseInt(m[2], 10);
        if (cmd === 'LOOP') {
            const block = steps.slice(segmentStart);
            let added = 0;
            for (let i = 1; i < ms && steps.length < 1000000; ++i) {
                steps.push(...block.map(x => ({...x})));
                added++;
            }
            // FIX #21: Warn if LOOP was cut short.
            // The server rejects macros that exceed MAX_EXPANDED_STEPS; a silent
            // truncation here produces a macro that the server will reject.
            if (added < ms - 1) loopTruncated = true;
            segmentStart = steps.length;
            return;
        }
        const st = {...getNeutralState(), ms};
        if (cmd !== 'WAIT') for (const t0 of cmd.split(/[+,|\s]+/).filter(Boolean)) { const t=t0.toUpperCase(); st.buttons |= macroBtnBit(t); if (t==='DPAD_UP'||t==='UP') st.hat=HAT_N; else if (t==='DPAD_DOWN'||t==='DOWN') st.hat=HAT_S; else if (t==='DPAD_LEFT'||t==='LEFT') st.hat=HAT_W; else if (t==='DPAD_RIGHT'||t==='RIGHT') st.hat=HAT_E; else if (t==='LSTICK_UP'||t==='LS_UP') st.ly=0; else if (t==='LSTICK_DOWN'||t==='LS_DOWN') st.ly=255; else if (t==='LSTICK_LEFT'||t==='LS_LEFT') st.lx=0; else if (t==='LSTICK_RIGHT'||t==='LS_RIGHT') st.lx=255; else if (t==='RSTICK_UP'||t==='RS_UP') st.ry=0; else if (t==='RSTICK_DOWN'||t==='RS_DOWN') st.ry=255; else if (t==='RSTICK_LEFT'||t==='RS_LEFT') st.rx=0; else if (t==='RSTICK_RIGHT'||t==='RS_RIGHT') st.rx=255; }
        steps.push(st);
    };
    for (const raw of text.split(';')) { const p = raw.trim(); if (p) addStep(p); }
    if (loopTruncated) {
        console.warn('[ns-macro] LOOP was truncated at 1,000,000 expanded steps. ' +
            'The server will reject macros that exceed its step limit. ' +
            'Reduce the LOOP count or split the macro.');
    }
    return steps;
}
function normalizeMacroKey(k) { return String(k || '').trim().toUpperCase(); }
function isValidKeyCode(code) {
    if (!code) return true;
    const c = normalizeMacroKey(code);
    if (c === 'ESCAPE' || c === 'SPACE' || c === 'ENTER' || c === 'TAB' || c === 'BACKSPACE' || c === 'DELETE' || c === 'INSERT' || c === 'HOME' || c === 'END' || c === 'PAGEUP' || c === 'PAGEDOWN' || c === 'CAPSLOCK' || c === 'NUMLOCK' || c === 'SCROLLLOCK' || c === 'PAUSE' || c === 'PRINTSCREEN' || c === 'CONTEXTMENU') return true;
    if (/^KEY[A-Z]$/.test(c)) return true;
    if (/^DIGIT[0-9]$/.test(c)) return true;
    if (/^F(?:[1-9]|1[0-9]|2[0-4])$/.test(c)) return true;
    if (/^ARROW(?:UP|DOWN|LEFT|RIGHT)$/.test(c)) return true;
    if (/^(?:SHIFT|CONTROL|ALT|META)(?:LEFT|RIGHT)$/.test(c)) return true;
    if (/^NUMPAD(?:[0-9]|ADD|SUBTRACT|MULTIPLY|DIVIDE|DECIMAL|ENTER)$/.test(c)) return true;
    return false;
}
function macroEntryName(m,i) { return (m && m.name) || `Macro ${i+1}`; }
function uniqueMacroName(base) { base=String(base||'Recorded Macro').trim()||'Recorded Macro'; let n=base, s=2; while(savedMacros.some(m=>String(m.name||'').toUpperCase()===n.toUpperCase())) n=`${base} ${s++}`; return n; }
function macroPrettyEntry(obj) { const m=macroPrettyObject(obj); if (!m.name) m.name='Macro'; const hk = normalizeMacroKey(obj && obj.hotkey); m.hotkey = isValidKeyCode(hk) ? hk : ''; return m; }
function persistMacros() { savedMacros = savedMacros.map(macroPrettyEntry); localStorage.setItem('nswc_macros', JSON.stringify(savedMacros)); }
function macroEntriesExport(entries=savedMacros) { return JSON.stringify({macros:entries.map(macroPrettyEntry)}, null, 2); }
function macroHotkeyConflicts(code, skip=-1) { code=normalizeMacroKey(code); if (!code) return false; if (Object.values(currentBindings).some(v=>normalizeMacroKey(v)===code)) return 'keyboard binding'; const dup=savedMacros.find((m,i)=>i!==skip && normalizeMacroKey(m.hotkey)===code); return dup ? (dup.name || 'another macro') : false; }
function refreshMacroList() { const rows=document.getElementById('macroRows'); if(!rows) return; rows.innerHTML=''; if(!savedMacros.length){ const e=document.createElement('div'); e.textContent='No macros'; e.style.cssText='text-align:center; width:100%; padding:4px;'; rows.appendChild(e); } savedMacros.forEach((m,i)=>{ const r=document.createElement('div'); r.style.cssText='display:grid; grid-template-columns:minmax(0,250px) 110px 68px 64px 64px; gap:4px; margin-bottom:4px;'; const add=(txt,fn)=>{const b=document.createElement('button'); b.textContent=txt; b.onclick=fn; b.style.cssText='width:100%;box-sizing:border-box;display:flex;justify-content:center;align-items:center;'; r.appendChild(b);}; add(macroEntryName(m,i),()=>runMacroServerSide(m)); add(formatKeyName(m.hotkey),()=>beginMacroHotkeyListen(i)); add('Rename',()=>renameMacro(i)); add('Export',()=>exportOneMacro(i)); add('Delete',()=>{savedMacros.splice(i,1); persistMacros(); refreshMacroList();}); rows.appendChild(r); }); }
function startMacro(objOrText) { macroSteps = parseMacro(objOrText); if (!macroSteps.length) { alert('No usable macro commands. Example: WAIT 200; A 100; B 100'); return; } macroRunning = true; macroStepIndex = 0; macroState = getNeutralState(); macroStepUntil = performance.now(); }
function updateMacroState() {
    if (!macroRunning) return null; const now = performance.now();
    while (macroRunning && now >= macroStepUntil) { const st = macroSteps[macroStepIndex++]; if (!st) { macroRunning = false; macroState = getNeutralState(); return null; } macroState = {...getNeutralState(), ...st}; macroStepUntil = now + st.ms; }
    return macroState;
}
function sampleMacroRecording(p1) {
    if (!macroRecording) return; const now = performance.now(); const frame = macroFrameFromState(p1);
    if (macroRecordLast === null) { macroRecordLast = frame; macroRecordSince = now; return; }
    if (!macroFrameEqual(frame, macroRecordLast)) { const dur = Math.max(1, Math.round(now - macroRecordSince)); macroRecorded.push(`${macroFrameToText(macroRecordLast)} ${dur}`); macroRecordLast = frame; macroRecordSince = now; }
}
function sendServerMacroChunks(payload, subpad=0) {
    const enc = new TextEncoder(); const bytes = enc.encode(payload);
    if (bytes.length > MACRO_JSON_MAX_BYTES) throw new Error('macro JSON exceeds 50MB limit');
    // Must not exceed the server's UDP_CHUNK_MAX (1200); larger chunks are silently dropped.
    const chunkSize = 1200, count = Math.ceil(bytes.length / chunkSize), uploadId = (Date.now() ^ Math.floor(Math.random()*0xFFFFFFFF)) >>> 0;
    for (let i=0; i<count; i++) {
        const start=i*chunkSize, chunk=bytes.slice(start, Math.min(bytes.length, start+chunkSize));
        const buf = new ArrayBuffer(30 + chunk.length), v = new DataView(buf);
        v.setUint32(0, 0x4E534D4B, true); v.setUint8(4, PROTO_VERSION); v.setUint8(5, subpad & 3); v.setUint8(6, i+1===count ? 1 : 0); v.setUint8(7, 0);
        v.setUint32(8, uploadId, true); v.setUint32(12, i, true); v.setUint32(16, count, true); v.setUint32(20, bytes.length, true); v.setUint16(24, chunk.length, true); v.setUint32(26, seqCounter++, true);
        new Uint8Array(buf, 30).set(chunk); ws.send(buf);
    }
}
function runMacroServerSide(objOrText) {
    const obj = (typeof objOrText === 'string') ? {name:'Macro', commands:objOrText} : objOrText;
    try { validateMacro(obj); } catch(err) { alert('Invalid macro: '+err.message); return; }
    if (ws && ws.readyState === WebSocket.OPEN) { try { sendServerMacroChunks(JSON.stringify(macroPrettyObject(obj))); return; } catch(err) { alert('Macro upload failed: '+err.message); return; } }
    startMacro(obj); // fallback for old/offline servers
}
function exportMacrosJson() {
    const blob = new Blob([macroEntriesExport()], {type:'application/json'});
    const a = document.createElement('a'); a.href = URL.createObjectURL(blob); a.download = 'ns-macros.json'; a.click(); setTimeout(()=>URL.revokeObjectURL(a.href), 1000);
}
function exportOneMacro(i) { const blob=new Blob([macroEntriesExport([savedMacros[i]])],{type:'application/json'}); const a=document.createElement('a'); a.href=URL.createObjectURL(blob); a.download=`${macroEntryName(savedMacros[i],i).replace(/[\\/:*?"<>|]/g,'_')}.json`; a.click(); setTimeout(()=>URL.revokeObjectURL(a.href),1000); }
function importMacrosJsonText(txt) {
    let data = JSON.parse(txt); let replace=false; if (data && Array.isArray(data.macros)) { data=data.macros; replace=true; } else if (!Array.isArray(data)) data=[data];
    const imported=[]; for (const m of data) if (m && (m.commands || typeof m === 'string')) { const obj=macroPrettyEntry(typeof m === 'string' ? {name:`Macro ${savedMacros.length+1}`, commands:m} : m); validateMacro(obj); if (obj.hotkey && (!isValidKeyCode(obj.hotkey) || Object.values(currentBindings).some(v=>v.toUpperCase()===obj.hotkey.toUpperCase()))) obj.hotkey=''; imported.push(obj); }
    const nameInUse = (name, arr) => arr.some(x => String(x.name||'').toUpperCase() === String(name||'').toUpperCase());
    const uniqueName = (base, arr) => { if (!nameInUse(base, arr)) return base; let n=1; while(nameInUse(`${base} (${n})`, arr)) n++; return `${base} (${n})`; };
    if (replace || imported.length > 1) { for (const m of imported) m.name = uniqueName(m.name, imported); savedMacros=imported; }
    else if (imported.length) { const m=imported[0]; m.name = uniqueName(m.name, savedMacros); savedMacros.push(m); }
    persistMacros(); refreshMacroList();
}
function beginMacroHotkeyListen(i) { const once=(ev)=>{ ev.preventDefault(); let code=ev.code==='Escape'?'':ev.code; const conflict=macroHotkeyConflicts(code,i); if(conflict){ alert('Macro keybind is already used by: '+conflict); } else { savedMacros[i].hotkey=code; persistMacros(); refreshMacroList(); } window.removeEventListener('keydown', once, true); }; window.addEventListener('keydown', once, true); }
function renameMacro(i) { const old=macroEntryName(savedMacros[i],i); const name=prompt('Macro name:', old); if(name===null) return; const n=name.trim(); if(!n){ alert('Macro name cannot be empty.'); return; } const dup=savedMacros.findIndex((m,j)=>j!==i && String(m.name||'').toUpperCase()===n.toUpperCase()); if(dup>=0){ alert('Another macro already uses that name.'); return; } savedMacros[i].name=n; savedMacros[i]=macroPrettyEntry(savedMacros[i]); persistMacros(); refreshMacroList(); }
function wireMacroMenu() {
    const isMobile = /Android|webOS|iPhone|iPad|iPod|BlackBerry|IEMobile|Opera Mini/i.test(navigator.userAgent);
    const btn = document.getElementById('btnMacros'); if (!btn || isMobile) return;
    btn.onclick = () => { if (!isConnected) { alert('Not connected to server.'); return; } refreshMacroList(); document.getElementById('macroOverlay').style.display='flex'; };
    document.getElementById('btnMacroClose').onclick = () => document.getElementById('macroOverlay').style.display='none';
    document.getElementById('btnMacroExport').onclick = exportMacrosJson;
    document.getElementById('btnMacroImport').onclick = () => document.getElementById('macroImportFile').click();
    document.getElementById('macroImportFile').onchange = e => { const f=e.target.files[0]; if(!f) return; if (f.size > MACRO_JSON_MAX_BYTES) { alert('Macro JSON exceeds the 50MB limit.'); e.target.value=''; return; } const r=new FileReader(); r.onload=()=>{ try{ importMacrosJsonText(String(r.result)); } catch(err){ alert('Invalid macro JSON: '+err.message); } }; r.readAsText(f); e.target.value=''; };
    document.getElementById('btnMacroRecord').onclick = () => {
        if (!macroRecording) { macroRecording=true; macroRecordLast=null; macroRecorded=[]; document.getElementById('btnMacroRecord').innerText='Stop'; }
        else { macroRecording=false; if (macroRecordLast !== null) macroRecorded.push(`${macroFrameToText(macroRecordLast)} ${Math.max(1, Math.round(performance.now()-macroRecordSince))}`); if(macroRecorded.some(x=>!x.startsWith('WAIT '))) savedMacros.push(macroPrettyEntry({name:uniqueMacroName('Recorded Macro'), commands:macroRecorded})); persistMacros(); refreshMacroList(); document.getElementById('btnMacroRecord').innerText='Record P1'; }
    };
}
function sendNamesIfChanged(slotPresent, slotName) {
    if (!ws || ws.readyState !== WebSocket.OPEN) return;
    const key = slotName.map((n, i) => slotPresent[i] ? n : '').join('');
    const now = Date.now();
    if (key === lastNamesSent && (now - lastNamesSentMs) < 2000) return;
    lastNamesSent = key;
    lastNamesSentMs = now;
    const buf = new ArrayBuffer(CLIENT_NAMES_SIZE), v = new DataView(buf);
    v.setUint32(0, CLIENT_NAMES_MAGIC, true);
    v.setUint8(4, SERVER_INFO_VERSION);
    for (let p = 0; p < 4; p++) {
        const off = 8 + p * ROSTER_ENTRY_SIZE;
        v.setUint8(off, slotPresent[p] ? 1 : 0);
        v.setUint8(off + 1, 0);
        const name = (slotPresent[p] ? (slotName[p] || 'Controller') : '').slice(0, ROSTER_NAME_CAP - 1);
        for (let k = 0; k < name.length; k++) v.setUint8(off + 2 + k, name.charCodeAt(k) & 0xff);
    }
    ws.send(buf);
}
function updateRosterUi() {
    let playerNum = 1;
    for (let p = 0; p < 4; p++) {
        const el = document.getElementById(`p${p+1}Text`);
        if (!el) continue;
        if (roster.valid && roster.ports[p] && roster.ports[p].present === 2) {
            el.style.display = 'none';
            continue;
        }
        el.style.display = 'block';
        let label = 'Not connected';
        if (roster.valid && roster.ports[p] && roster.ports[p].present === 1) {
            label = roster.ports[p].name || 'Controller';
            if (roster.ports[p].gyro) label += ' + gyro';
            el.innerText = `P${playerNum}: ${label}`;
            playerNum++;
        } else {
            el.innerText = `P${playerNum}: ${label}`;
            playerNum++;
        }
    }
}
function buildAndSendPacket() {
    if (!ws || ws.readyState !== WebSocket.OPEN) return;
    if (window.NSBridge || (window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.nsBridge)) return;
    const rawGamepads = navigator.getGamepads ? navigator.getGamepads() : [];
    const activePads = [];
    for (let i = 0; i < rawGamepads.length; i++) if (rawGamepads[i]) activePads.push(rawGamepads[i]);
    lastActivePads = activePads;
    const mode = parseInt(document.getElementById('kbMode').value);
    const kbState = getKeyboardState();
    let slotStates = [null, null, null, null];
    let slotName = ["", "", "", ""];
    let slotPresent = [false, false, false, false];
    const padName = (pad) => (pad && pad.id) ? pad.id : "Controller";
    if (mode === 0) {
        for (let i = 0; i < 4; i++) {
            let pad = activePads[i];
            let gp = getGamepadState(pad);
            slotStates[i] = gp || getNeutralState();
            slotPresent[i] = !!gp;
            slotName[i] = gp ? padName(pad) : "";
        }
    } else if (mode === 1) {
        slotStates[0] = kbState; slotPresent[0] = true; slotName[0] = "Keyboard";
        for (let i = 1; i < 4; i++) {
            let pad = activePads[i - 1];
            let gp = getGamepadState(pad);
            slotStates[i] = gp || getNeutralState();
            slotPresent[i] = !!gp;
            slotName[i] = gp ? padName(pad) : "";
        }
    } else if (mode === 2) {
        let pad0 = activePads[0];
        let gp0 = getGamepadState(pad0);
        slotStates[0] = mergeStates(kbState, gp0 || getNeutralState());
        slotPresent[0] = true;
        slotName[0] = gp0 ? (padName(pad0) + " + Keyboard") : "Keyboard";
        for (let i = 1; i < 4; i++) {
            let pad = activePads[i];
            let gp = getGamepadState(pad);
            slotStates[i] = gp || getNeutralState();
            slotPresent[i] = !!gp;
            slotName[i] = gp ? padName(pad) : "";
        }
    }
    sampleMacroRecording(slotStates[0]);
    const macroOverride = updateMacroState();
    if (macroOverride) slotStates[0] = macroOverride;
    updateRosterUi();
    // Give features (gyro, mouse mode, ...) a chance to mutate pad 0 and its
    // extended fields before the packet is serialized.
    const controllerType = NSCore.settings.get('controllerType');
    const frame = {
        state: slotStates[0],
        ext: {
            present: slotPresent[0],
            controllerType,
            motionSamples: null,
            motionFresh: false,
            batteryPercent: null,
            batteryCharging: false
        }
    };
    NSCore.dispatch.buildFrame(frame);
    slotStates[0] = frame.state;
    slotPresent[0] = frame.ext.present;
    const buffer = new ArrayBuffer(PACKET_SIZE), view = new DataView(buffer);
    view.setUint32(0, PROTO_MAGIC, true); view.setUint8(4, PROTO_VERSION); view.setUint8(5, 0);
    view.setUint16(6, 0, true); view.setUint32(8, seqCounter++, true); view.setBigUint64(12, BigInt(Date.now() * 1000), true);
    for (let p = 0; p < 4; p++) {
        const s = { ...slotStates[p], buttons: normalizeSystemShortcuts(slotStates[p].buttons) };
        // The desktop ns-client stamps every live pad with the selected profile;
        // mirror that so multi-gamepad setups enumerate consistently.
        const ex = p === 0 ? frame.ext
            : { present: slotPresent[p], controllerType: slotPresent[p] ? controllerType : 0 };
        NSCore.buildExtPad(view, 20 + p * EXT_REPORT_SIZE, s, ex);
    }
    ws.send(buffer);
    sendNamesIfChanged(slotPresent, slotName);
}
document.getElementById('btnConnect').onclick = async () => {
    if (isConnected) {
        if (ws) ws.close();
        resetMainConnectionUi('Disconnected');
        return;
    }
    const wsUrl = makeWsUrl();
    ws = new WebSocket(wsUrl, "nspc-protocol"); ws.binaryType = "arraybuffer";
    ws.onmessage = handleWsBinaryMessage;
    ws.onopen = () => {
        isConnected = true;
        NSCore.dispatch.connect(ws);
        document.getElementById('btnConnect').innerText = "Disconnect";
        document.getElementById('kbMode').disabled = true;
        document.getElementById('statusText').innerText = `Connected.`;
        try { window.focus(); document.body.focus(); } catch(e) {}
        loopId = setInterval(buildAndSendPacket, 4);
    };
    ws.onerror = () => {
        document.getElementById('statusText').innerText = "Connection failed";
    };
    ws.onclose = () => {
        const current = document.getElementById('statusText').innerText;
        resetMainConnectionUi((current === 'Connection failed' || current === 'Server full') ? current : 'Disconnected');
    }
};
function formatKeyName(code) {
    if (code === 'Unbound') return '';
    const c = code.toLowerCase();
    if (c.startsWith('key')) return code.slice(3);
    if (c.startsWith('digit')) return code.slice(5);
    if (c.startsWith('arrow')) return code.slice(5);
    if (c === 'shiftleft') return 'LShift'; if (c === 'shiftright') return 'RShift';
    if (c === 'controlleft') return 'LCtrl'; if (c === 'controlright') return 'RCtrl';
    if (c === 'altleft') return 'LAlt'; if (c === 'altright') return 'RAlt';
    return code;
}
let activeBindKey = null; let isSetupMode = false; let setupQueue = [];
function renderBindings() {
    const list = document.getElementById('bindingsList'); list.innerHTML = '';
    for (const [btn, code] of Object.entries(currentBindings)) {
        const row = document.createElement('div'); row.className = 'bind-row';
        const label = document.createElement('span'); label.innerText = btn.replace('BTN_', '');
        const btnChange = document.createElement('button'); btnChange.className = 'bind-btn';
        btnChange.innerText = formatKeyName(code); btnChange.id = `btn-${btn}`;
        btnChange.onclick = () => {
            if (isSetupMode) return;
            if (activeBindKey) document.getElementById(`btn-${activeBindKey}`).classList.remove('listening');
            activeBindKey = btn; btnChange.innerText = "---"; btnChange.classList.add('listening');
        };
        row.appendChild(label); row.appendChild(btnChange); list.appendChild(row);
    }
}
function startNextSetupBind() {
    if (setupQueue.length === 0) {
        isSetupMode = false; activeBindKey = null; renderBindings(); return;
    }
    activeBindKey = setupQueue.shift(); renderBindings();
    const targetBtn = document.getElementById(`btn-${activeBindKey}`);
    if (targetBtn) { targetBtn.innerText = "---"; targetBtn.classList.add('listening'); targetBtn.scrollIntoView({ behavior: 'smooth', block: 'center' }); }
}
function remapKey(code) {
    if (!activeBindKey) return;
    if (code === 'Escape') { currentBindings[activeBindKey] = 'Unbound'; if (isSetupMode) { startNextSetupBind(); } else { activeBindKey = null; renderBindings(); } return; }
    if (isSetupMode) {
        for (const existingCode of Object.values(currentBindings)) if (existingCode === code) return;
        currentBindings[activeBindKey] = code; startNextSetupBind();
    } else {
        for (const [existingBtn, existingCode] of Object.entries(currentBindings)) {
            if (existingCode === code && existingBtn !== activeBindKey) currentBindings[existingBtn] = 'Unbound';
        }
        currentBindings[activeBindKey] = code; activeBindKey = null; renderBindings();
    }
}
document.getElementById('btnBindings').onclick = () => {
    preEditBindings = { ...currentBindings }; isSetupMode = false; activeBindKey = null;
    renderBindings(); document.getElementById('modalOverlay').style.display = 'flex';
};
document.getElementById('btnSaveBindings').onclick = () => {
    localStorage.setItem('nswc_bindings', JSON.stringify(currentBindings));
    isSetupMode = false; activeBindKey = null; document.getElementById('modalOverlay').style.display = 'none';
};
document.getElementById('btnCancelBindings').onclick = () => {
    currentBindings = { ...preEditBindings };
    isSetupMode = false; activeBindKey = null; document.getElementById('modalOverlay').style.display = 'none';
};
document.getElementById('btnClearBindings').onclick = () => {
    if (isSetupMode) isSetupMode = false;
    for (let k in currentBindings) currentBindings[k] = 'Unbound';
    activeBindKey = null; renderBindings();
};
document.getElementById('btnResetBindings').onclick = () => {
    if (isSetupMode) isSetupMode = false;
    currentBindings = { ...defaultBindings }; activeBindKey = null; renderBindings();
};
document.getElementById('btnSetupBindings').onclick = () => {
    for (let k in currentBindings) currentBindings[k] = 'Unbound';
    setupQueue = Object.keys(currentBindings); isSetupMode = true; startNextSetupBind();
};
