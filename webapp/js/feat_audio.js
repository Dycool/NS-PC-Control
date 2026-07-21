// feat_audio.js — Phase 6 (client): Switch 2 console audio + microphone over WS.
//
// Playback: WS S2AudioPcmPacket frames (S16LE stereo 48 kHz, 5 ms batches)
// feed an AudioWorklet ring buffer (ScriptProcessor fallback on plain HTTP,
// where AudioWorklet is unavailable). Microphone: getUserMedia (HTTPS only)
// -> AudioWorklet capture -> 48 kHz S16LE 5 ms chunks -> WS. A capabilities
// frame is sent on toggle and refreshed every 2 s (server timeout is 5 s).
// Everything sits behind settings flags and S2 Pro eligibility.
'use strict';

(function () {
    const C = NSCore.C, S = NSCore.settings, caps = NSCore.caps, el = NSCore.el;
    const FRAMES_PER_PACKET = 240; // 5 ms @ 48 kHz (960 bytes stereo S16LE)
    let page = 'index';
    let ctx = null;                 // AudioContext (48 kHz)
    let playerNode = null;          // worklet or ScriptProcessor
    let workletReady = false;
    let usingFallback = false;
    let fallbackRing = null, fbWrite = 0, fbRead = 0, fbAvail = 0, fbStarted = false;
    let micStream = null, micSource = null, micNode = null;
    let micSeq = 0, capsSeq = 0;
    let capsTimer = null;
    let lastSentCaps = null;

    const PLAYER_WORKLET = `
class NsPcmPlayer extends AudioWorkletProcessor {
    constructor() {
        super();
        this.capacity = 48000;              // 1 s of stereo frames
        this.buf = new Float32Array(this.capacity * 2);
        this.read = 0; this.write = 0; this.avail = 0;
        this.started = false;
        this.prebuffer = 2400;              // 50 ms jitter buffer
        this.port.onmessage = (e) => {
            const data = e.data;            // Int16Array, interleaved stereo
            const frames = data.length >> 1;
            for (let i = 0; i < frames; i++) {
                if (this.avail >= this.capacity) { // full: drop oldest frame
                    this.read = (this.read + 1) % this.capacity;
                    this.avail--;
                }
                const w = this.write * 2;
                this.buf[w] = data[i * 2] / 32768;
                this.buf[w + 1] = data[i * 2 + 1] / 32768;
                this.write = (this.write + 1) % this.capacity;
                this.avail++;
            }
        };
    }
    process(inputs, outputs) {
        const out = outputs[0];
        const L = out[0], R = out.length > 1 ? out[1] : out[0];
        const n = L.length;
        if (!this.started && this.avail >= this.prebuffer) this.started = true;
        if (!this.started || this.avail < n) {
            if (this.avail < n) this.started = false; // underrun: rebuffer
            L.fill(0); if (R !== L) R.fill(0);
            return true;
        }
        for (let i = 0; i < n; i++) {
            const r = this.read * 2;
            L[i] = this.buf[r];
            R[i] = this.buf[r + 1];
            this.read = (this.read + 1) % this.capacity;
            this.avail--;
        }
        return true;
    }
}
registerProcessor('ns-pcm-player', NsPcmPlayer);

class NsMicCapture extends AudioWorkletProcessor {
    constructor() {
        super();
        this.chunk = new Int16Array(${FRAMES_PER_PACKET} * 2);
        this.fill = 0;
    }
    process(inputs) {
        const inp = inputs[0];
        if (!inp || !inp[0]) return true;
        const L = inp[0], R = inp.length > 1 && inp[1] ? inp[1] : inp[0];
        for (let i = 0; i < L.length; i++) {
            const l = Math.max(-1, Math.min(1, L[i]));
            const r = Math.max(-1, Math.min(1, R[i]));
            this.chunk[this.fill * 2] = (l * 32767) | 0;
            this.chunk[this.fill * 2 + 1] = (r * 32767) | 0;
            if (++this.fill === ${FRAMES_PER_PACKET}) {
                const out = new Int16Array(this.chunk);
                this.port.postMessage(out, [out.buffer]);
                this.fill = 0;
            }
        }
        return true;
    }
}
registerProcessor('ns-mic-capture', NsMicCapture);
`;

    const eligible = () => NSCore.state.connected && NSCore.s2AudioEligible()
        && S.get('controllerType') === C.TYPE_PRO && !caps.isNative;
    const wantPlayback = () => S.get('audioPlayback');
    const wantMic = () => S.get('audioMicrophone') && wantPlayback()
        && caps.isSecureContext && caps.hasGetUserMedia;

    // ── AudioContext / worklet setup ─────────────────────────────────────────
    async function ensureContext() {
        if (ctx) return ctx;
        ctx = new (window.AudioContext || window.webkitAudioContext)({ sampleRate: C.S2_AUDIO_SAMPLE_RATE });
        if (ctx.audioWorklet && caps.isSecureContext) {
            try {
                const url = URL.createObjectURL(new Blob([PLAYER_WORKLET], { type: 'application/javascript' }));
                await ctx.audioWorklet.addModule(url);
                URL.revokeObjectURL(url);
                workletReady = true;
            } catch (e) {
                console.warn('[ns-audio] AudioWorklet unavailable, using fallback', e);
            }
        }
        const out = S.get('audioOutputDevice');
        if (out && typeof ctx.setSinkId === 'function') { try { await ctx.setSinkId(out); } catch (_) {} }
        // Autoplay policies: resume on the next user gesture if needed.
        const resume = () => { if (ctx && ctx.state === 'suspended') ctx.resume().catch(() => {}); };
        document.addEventListener('click', resume, true);
        document.addEventListener('touchend', resume, true);
        return ctx;
    }
    async function startPlayback() {
        await ensureContext();
        if (playerNode) return;
        if (workletReady) {
            playerNode = new AudioWorkletNode(ctx, 'ns-pcm-player', {
                outputChannelCount: [2]
            });
            playerNode.connect(ctx.destination);
            usingFallback = false;
        } else {
            // Plain-HTTP fallback: main-thread ring + ScriptProcessor.
            fallbackRing = new Float32Array(48000 * 2);
            fbWrite = 0; fbRead = 0; fbAvail = 0; fbStarted = false;
            playerNode = ctx.createScriptProcessor(1024, 0, 2);
            playerNode.onaudioprocess = (e) => {
                const L = e.outputBuffer.getChannelData(0);
                const R = e.outputBuffer.getChannelData(1);
                const n = L.length;
                if (!fbStarted && fbAvail >= 2400) fbStarted = true;
                if (!fbStarted || fbAvail < n) {
                    if (fbAvail < n) fbStarted = false;
                    L.fill(0); R.fill(0);
                    return;
                }
                for (let i = 0; i < n; i++) {
                    const r = fbRead * 2;
                    L[i] = fallbackRing[r]; R[i] = fallbackRing[r + 1];
                    fbRead = (fbRead + 1) % 48000;
                    fbAvail--;
                }
            };
            playerNode.connect(ctx.destination);
            usingFallback = true;
        }
        if (ctx.state === 'suspended') ctx.resume().catch(() => {});
    }
    function stopPlayback() {
        if (playerNode) { try { playerNode.disconnect(); } catch (_) {} playerNode = null; }
        fallbackRing = null;
    }
    function feedPlayback(int16) {
        if (!playerNode) return;
        if (!usingFallback) {
            playerNode.port.postMessage(int16);
            return;
        }
        const frames = int16.length >> 1;
        for (let i = 0; i < frames; i++) {
            if (fbAvail >= 48000) { fbRead = (fbRead + 1) % 48000; fbAvail--; }
            const w = fbWrite * 2;
            fallbackRing[w] = int16[i * 2] / 32768;
            fallbackRing[w + 1] = int16[i * 2 + 1] / 32768;
            fbWrite = (fbWrite + 1) % 48000;
            fbAvail++;
        }
    }

    // ── Microphone ───────────────────────────────────────────────────────────
    async function startMic() {
        if (micNode || !wantMic()) return;
        await ensureContext();
        if (!workletReady) return; // mic implies HTTPS, where the worklet loads
        try {
            const constraints = { audio: { channelCount: 2, sampleRate: C.S2_AUDIO_SAMPLE_RATE } };
            const dev = S.get('audioInputDevice');
            if (dev) constraints.audio.deviceId = { exact: dev };
            micStream = await navigator.mediaDevices.getUserMedia(constraints);
        } catch (e) {
            console.warn('[ns-audio] microphone unavailable', e);
            S.set('audioMicrophone', false);
            return;
        }
        micSource = ctx.createMediaStreamSource(micStream);
        micNode = new AudioWorkletNode(ctx, 'ns-mic-capture');
        micNode.port.onmessage = (e) => sendMicPcm(e.data);
        micSource.connect(micNode);
        // No connection to destination: capture-only graph.
        if (ctx.state === 'suspended') ctx.resume().catch(() => {});
    }
    function stopMic() {
        if (micNode) { try { micSource.disconnect(micNode); micNode.disconnect(); } catch (_) {} micNode = null; micSource = null; }
        if (micStream) { micStream.getTracks().forEach(t => t.stop()); micStream = null; }
    }
    function sendMicPcm(int16) {
        if (!NSCore.state.connected || !wantMic() || !eligible()) return;
        const buf = new ArrayBuffer(C.S2_AUDIO_PCM_SIZE);
        const v = new DataView(buf);
        v.setUint32(0, C.S2_AUDIO_PCM_MAGIC, true);
        v.setUint8(4, C.S2_AUDIO_VERSION);
        v.setUint8(5, C.S2_AUDIO_DIR_CLIENT_TO_CONSOLE);
        v.setUint16(6, C.S2_AUDIO_PCM_BYTES, true);
        v.setUint32(8, micSeq++ >>> 0, true);
        v.setBigUint64(12, BigInt(Date.now()) * 1000n, true);
        new Uint8Array(buf, 20, C.S2_AUDIO_PCM_BYTES)
            .set(new Uint8Array(int16.buffer, 0, C.S2_AUDIO_PCM_BYTES));
        // bytes 980..995: HMAC field, zeroed (trusted WS path)
        NSCore.wsSend(buf);
    }

    // ── Capabilities frames ──────────────────────────────────────────────────
    function currentCaps() {
        if (!eligible()) return 0;
        let f = 0;
        if (wantPlayback()) f |= C.S2_AUDIO_CAP_PLAYBACK;
        if (wantMic()) f |= C.S2_AUDIO_CAP_MICROPHONE;
        return f;
    }
    function sendCaps(force) {
        const f = currentCaps();
        if (!NSCore.state.connected) return;
        if (!force && f === lastSentCaps && f === 0) return;
        const buf = new ArrayBuffer(C.S2_AUDIO_CAPS_SIZE);
        const v = new DataView(buf);
        v.setUint32(0, C.S2_AUDIO_CAPS_MAGIC, true);
        v.setUint8(4, C.S2_AUDIO_VERSION);
        v.setUint8(5, f);
        v.setUint32(8, capsSeq++ >>> 0, true);
        v.setBigUint64(12, BigInt(Date.now()) * 1000n, true);
        if (NSCore.wsSend(buf)) lastSentCaps = f;
    }

    // ── Orchestration ────────────────────────────────────────────────────────
    async function sync() {
        const on = eligible();
        if (on && wantPlayback()) await startPlayback(); else stopPlayback();
        if (on && wantMic()) await startMic(); else stopMic();
        sendCaps(true);
        if (!capsTimer && NSCore.state.connected) {
            capsTimer = setInterval(() => sendCaps(false), 2000);
        }
    }
    function teardown() {
        stopMic();
        stopPlayback();
        if (capsTimer) { clearInterval(capsTimer); capsTimer = null; }
        lastSentCaps = null;
    }

    S.onChange('audioPlayback', () => { sync(); });
    S.onChange('audioMicrophone', () => { sync(); });
    NSCore.onStateChanged(() => {
        // Assignment changes can grant/revoke eligibility mid-session.
        if (NSCore.state.connected && (wantPlayback() || wantMic())) sync();
    });

    NSCore.registerFeature({
        id: 'audio',
        mountUI(p) { page = p; },
        onConnect() { if (wantPlayback() || wantMic()) sync(); },
        onDisconnect() { teardown(); },
        onWsMessage(magic, view) {
            if (magic === C.S2_AUDIO_PCM_MAGIC && view.byteLength === C.S2_AUDIO_PCM_SIZE) {
                if (view.getUint8(5) === C.S2_AUDIO_DIR_CONSOLE_TO_CLIENT
                        && wantPlayback() && playerNode) {
                    // PCM starts at byte 20 (even offset, so Int16Array is valid).
                    feedPlayback(new Int16Array(view.buffer, 20, FRAMES_PER_PACKET * 2));
                }
                return true;
            }
            return false;
        },
        settingsUI(ui) {
            if (caps.isNative) return;
            const sec = ui.section('Switch 2 audio');
            const ok = eligible();
            sec.toggle('Console audio playback', 'audioPlayback', { disabled: !ok });
            sec.toggle('Microphone to console', 'audioMicrophone', {
                disabled: !ok || !caps.isSecureContext || !caps.hasGetUserMedia,
                async beforeChange(enabling) {
                    if (enabling && !S.get('audioPlayback')) S.set('audioPlayback', true);
                    return true;
                }
            });
            if (!ok)
                sec.note('Needs a Switch 2 backend, an active connection and the Pro Controller type.');
            if (!caps.isSecureContext)
                sec.note('Microphone capture requires HTTPS (reverse proxy). Playback works on plain HTTP.', true);
            // Device pickers (populated once permissions expose the labels).
            if (navigator.mediaDevices && navigator.mediaDevices.enumerateDevices) {
                const outSel = sec.select('Output device', 'audioOutputDevice',
                    [['', 'Default']], { numeric: false }).select;
                const inSel = sec.select('Microphone', 'audioInputDevice',
                    [['', 'Default']], { numeric: false }).select;
                navigator.mediaDevices.enumerateDevices().then(devs => {
                    for (const d of devs) {
                        const label = d.label || (d.kind + ' ' + d.deviceId.slice(0, 6));
                        const canSetSink = typeof AudioContext !== 'undefined'
                            && typeof AudioContext.prototype.setSinkId === 'function';
                        if (d.kind === 'audiooutput' && canSetSink)
                            outSel.appendChild(el('option', { value: d.deviceId, text: label }));
                        else if (d.kind === 'audioinput')
                            inSel.appendChild(el('option', { value: d.deviceId, text: label }));
                    }
                    outSel.value = S.get('audioOutputDevice') || '';
                    inSel.value = S.get('audioInputDevice') || '';
                }).catch(() => {});
                outSel.onchange = async () => {
                    S.set('audioOutputDevice', outSel.value);
                    if (ctx && typeof ctx.setSinkId === 'function') { try { await ctx.setSinkId(outSel.value || ''); } catch (_) {} }
                };
                inSel.onchange = () => {
                    S.set('audioInputDevice', inSel.value);
                    if (micNode) { stopMic(); startMic(); }
                };
            }
            sec.note('Audio is S16LE stereo 48 kHz with a ~50 ms jitter buffer. Expect noticeably more latency than the desktop client.');
        }
    });
})();
