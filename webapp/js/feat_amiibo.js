// feat_amiibo.js — Phase 4 (client): Amiibo over the WebSocket (Switch 2).
//
// Mirrors the desktop ns-client flow: the console asks for a scan
// (AmiiboRequestPacket, sequence-guarded), the user loads a 540/572-byte
// NTAG215 .bin, we upload it as an AmiiboDataPacket; writebacks from the
// console are offered as a .bin download. Shown only when the assignment is
// a native-NFC S2 type (PRO_S2 / JOYCON_R_S2 / PAIR_S2 — main_window.cpp rule).
'use strict';

(function () {
    const C = NSCore.C, caps = NSCore.caps, el = NSCore.el;
    let page = 'index';
    let scanRequested = [false, false, false, false];
    let lastSeq = [null, null, null, null];
    let card = null, statusEl = null, scanBtn = null, settingsScanBtn = null;
    let fileInput = null, mobileBtn = null;

    function activeSubpad() {
        for (let s = 0; s < 4; s++) if (scanRequested[s]) return s;
        return 0;
    }

    // ── Upload (.bin -> AmiiboDataPacket over WS) ────────────────────────────
    function pickFile() {
        if (!NSCore.state.connected || !NSCore.s2NfcAssigned() || !scanRequested.some(Boolean)) return;
        if (fileInput) fileInput.click();
    }
    function sendAmiibo(bytes, subpad) {
        const buf = new ArrayBuffer(C.AMIIBO_DATA_SIZE);
        const v = new DataView(buf);
        v.setUint32(0, C.AMIIBO_DATA_MAGIC, true);
        v.setUint8(4, subpad & 3);
        v.setUint16(5, bytes.length, true); // packed: data_len at offset 5
        new Uint8Array(buf, C.AMIIBO_DATA_HEADER, bytes.length).set(bytes);
        if (!NSCore.wsSend(buf)) { alert('Not connected.'); return; }
        setStatus('Amiibo sent to the console.');
    }
    function onFilePicked(e) {
        const f = e.target.files[0];
        e.target.value = '';
        if (!f) return;
        const reader = new FileReader();
        reader.onload = () => {
            const bytes = new Uint8Array(reader.result);
            if (bytes.length !== C.AMIIBO_RAW_DUMP_SIZE && bytes.length !== C.AMIIBO_EXTENDED_DUMP_SIZE) {
                alert('Unsupported Amiibo dump: expected ' + C.AMIIBO_RAW_DUMP_SIZE
                    + ' or ' + C.AMIIBO_EXTENDED_DUMP_SIZE + ' bytes, got ' + bytes.length + '.');
                return;
            }
            sendAmiibo(bytes, activeSubpad());
        };
        reader.readAsArrayBuffer(f);
    }

    // ── Writeback (console -> client, save updated dump) ────────────────────
    function onWriteback(view) {
        const len = view.getUint16(5, true);
        if (len !== C.AMIIBO_RAW_DUMP_SIZE && len !== C.AMIIBO_EXTENDED_DUMP_SIZE) return;
        if (view.byteLength < C.AMIIBO_DATA_HEADER + len) return;
        const data = new Uint8Array(view.buffer, C.AMIIBO_DATA_HEADER, len);
        const blob = new Blob([data], { type: 'application/octet-stream' });
        const a = document.createElement('a');
        a.href = URL.createObjectURL(blob);
        a.download = 'amiibo-writeback-' + new Date().toISOString().replace(/[:.]/g, '-') + '.bin';
        a.click();
        setTimeout(() => URL.revokeObjectURL(a.href), 1000);
        setStatus('Console updated the Amiibo — writeback saved as a download.');
    }

    // ── UI ───────────────────────────────────────────────────────────────────
    function setStatus(text) { if (statusEl) statusEl.textContent = text; }
    function ensureFileInput() {
        if (fileInput) return;
        fileInput = el('input', { type: 'file', accept: '.bin,application/octet-stream', style: 'display:none' });
        fileInput.onchange = onFilePicked;
        document.body.appendChild(fileInput);
    }
    function ensureIndexCard() {
        if (card) return;
        const shell = document.querySelector('.shell');
        if (!shell) return;
        statusEl = el('div', { class: 'status', text: 'Waiting for the console to request an Amiibo scan.' });
        scanBtn = el('button', { text: 'Scan Amiibo (.bin)…', onclick: pickFile });
        card = el('section', { class: 'card' }, [
            el('div', { class: 'card-title', text: 'Amiibo (Switch 2)' }),
            el('div', { class: 'btn-group' }, [scanBtn]),
            statusEl
        ]);
        card.style.display = 'none';
        shell.appendChild(card);
    }
    function ensureMobileBtn() {
        if (mobileBtn) return;
        const surface = document.getElementById('gamepad') || document.body;
        mobileBtn = el('button', {
            class: 'ns-gear', text: '◉', title: 'Scan Amiibo', onclick: pickFile,
            style: 'top:60px; display:none;'
        });
        surface.appendChild(mobileBtn);
    }
    function refreshUi() {
        const eligible = NSCore.s2NfcAssigned();
        const waiting = scanRequested.some(Boolean);
        if (page === 'index') {
            ensureIndexCard();
            if (card) card.style.display = eligible ? '' : 'none';
            if (eligible)
                setStatus(waiting
                    ? 'Console is waiting for an Amiibo — load a .bin dump.'
                    : 'Waiting for the console to request an Amiibo scan.');
            if (scanBtn) {
                scanBtn.classList.remove('btn-primary');
                scanBtn.disabled = !(NSCore.state.connected && waiting);
            }
            if (settingsScanBtn) {
                settingsScanBtn.classList.remove('ns-btn-accent');
                settingsScanBtn.disabled = !(NSCore.state.connected && waiting);
            }
        } else if (!caps.isNative) {
            ensureMobileBtn();
            if (mobileBtn) mobileBtn.style.display = (eligible && waiting) ? 'flex' : 'none';
        }
    }

    NSCore.registerFeature({
        id: 'amiibo',
        mountUI(p) {
            page = p;
            ensureFileInput();
            NSCore.onStateChanged(refreshUi);
            refreshUi();
        },
        onDisconnect() {
            scanRequested = [false, false, false, false];
            lastSeq = [null, null, null, null];
            refreshUi();
        },
        onWsMessage(magic, view) {
            if (magic === C.AMIIBO_REQUEST_MAGIC && view.byteLength === C.AMIIBO_REQUEST_SIZE) {
                const r = NSCore.parseAmiiboRequest(view);
                if (r.subpad < 4) {
                    // Monotonic sequence: a delayed "start scan" must not override
                    // a newer "stop scan" (same rule as the UDP client).
                    const prev = lastSeq[r.subpad];
                    const diff = prev === null ? 1 : (r.sequence - prev) & 0xFFFF;
                    const newer = diff !== 0 && diff < 0x8000;
                    if (newer) {
                        lastSeq[r.subpad] = r.sequence;
                        scanRequested[r.subpad] = r.requested;
                        refreshUi();
                    }
                }
                return true;
            }
            if (magic === C.AMIIBO_DATA_MAGIC && view.byteLength >= C.AMIIBO_DATA_HEADER) {
                onWriteback(view);
                return true;
            }
            return false;
        },
        settingsUI(ui, p) {
            if (!NSCore.s2NfcAssigned()) return;
            const sec = ui.section('Amiibo (Switch 2)');
            settingsScanBtn = sec.button('Load Amiibo dump (.bin)…', pickFile, {
                disabled: !(NSCore.state.connected && scanRequested.some(Boolean))
            });
            sec.note('540-byte raw NTAG215 dumps and 572-byte extended dumps (with signature) are supported.');
        }
    });
})();
