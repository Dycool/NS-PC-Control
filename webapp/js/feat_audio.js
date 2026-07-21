// feat_audio.js — Phase 6 (client): Switch 2 console audio + microphone over WS.
//
// Playback: WS S2AudioPcmPacket frames (S16LE stereo 48 kHz, 5 ms batches)
// feed an AudioWorklet ring buffer; on plain HTTP (no AudioWorklet) each batch
// is scheduled as a chained AudioBufferSource — native-thread rendering, so
// playback quality on HTTP stays close to the worklet path.
// Microphone: getUserMedia (secure origins only — a hard browser rule)
// -> AudioWorklet capture -> 48 kHz S16LE 5 ms chunks -> WS. A capabilities
// frame is sent on toggle and refreshed every 2 s (server timeout is 5 s).
// Everything sits behind settings flags and S2 Pro eligibility.
'use strict';

(function () {
    const C = NSCore.C, S = NSCore.settings, caps = NSCore.caps, el = NSCore.el;
    const FRAMES_PER_PACKET = 240; // 5 ms @ 48 kHz (960 bytes stereo S16LE)
    let page = 'index';
    let ctx = null;                 // AudioContext (48 kHz)
    let playerNode = null;          // AudioWorkletNode (secure contexts)
    let workletReady = false;
    let usingFallback = false;      // plain HTTP: scheduled AudioBufferSources
    let playbackOn = false;
    let fbSchedTime = 0;            // next scheduled buffer start (ctx.currentTime base)
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
        if (playbackOn) return;
        playbackOn = true;
        if (workletReady) {
            playerNode = new AudioWorkletNode(ctx, 'ns-pcm-player', {
                outputChannelCount: [2]
            });
            playerNode.connect(ctx.destination);
            usingFallback = false;
        } else {
            // Plain-HTTP fallback: schedule each 5 ms PCM batch as an
            // AudioBufferSource chained on the context clock. Unlike the
            // deprecated ScriptProcessor, rendering happens on the native
            // audio thread, so main-thread jank cannot glitch the stream.
            usingFallback = true;
            fbSchedTime = 0;
        }
        if (ctx.state === 'suspended') ctx.resume().catch(() => {});
    }
    function stopPlayback() {
        playbackOn = false;
        if (playerNode) { try { playerNode.disconnect(); } catch (_) {} playerNode = null; }
        fbSchedTime = 0;
    }
    function feedPlayback(int16) {
        if (!playbackOn || !ctx) return;
        if (!usingFallback) {
            if (playerNode) playerNode.port.postMessage(int16);
            return;
        }
        const frames = int16.length >> 1;
        const now = ctx.currentTime;
        // Backlog beyond ~200 ms means the tab fell behind: drop this batch
        // (latency wins over completeness; the stream re-primes below).
        if (fbSchedTime > now + 0.2) return;
        // (Re)prime with a 50 ms jitter buffer after start or an underrun.
        if (fbSchedTime < now + 0.02) fbSchedTime = now + 0.05;
        const buf = ctx.createBuffer(2, frames, C.S2_AUDIO_SAMPLE_RATE);
        const L = buf.getChannelData(0), R = buf.getChannelData(1);
        for (let i = 0; i < frames; i++) {
            L[i] = int16[i * 2] / 32768;
            R[i] = int16[i * 2 + 1] / 32768;
        }
        const src = ctx.createBufferSource();
        src.buffer = buf;
        src.connect(ctx.destination);
        src.start(fbSchedTime);
        fbSchedTime += frames / C.S2_AUDIO_SAMPLE_RATE;
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
                        && wantPlayback() && playbackOn) {
                    // PCM starts at byte 20 (even offset, so Int16Array is valid).
                    feedPlayback(new Int16Array(view.buffer, 20, FRAMES_PER_PACKET * 2));
                }
                return true;
            }
            return false;
        },
        // Mirrors the desktop SettingsDialog's "Switch 2 headset" group: two
        // device combos with "Disabled" / "System default" entries; the mic is
        // only selectable while headphones are attached. Hidden unless the
        // session is eligible, like the Qt dialog.
        settingsUI(ui) {
            if (caps.isNative || !eligible()) return;
            const sec = ui.section('Switch 2 headset');
            const DISABLED = '@disabled';
            const micAllowed = caps.isSecureContext && caps.hasGetUserMedia;

            const outSel = sec.select('Audio output', null,
                [[DISABLED, 'Disabled'], ['', 'System default']], {
                    numeric: false,
                    value: S.get('audioPlayback') ? (S.get('audioOutputDevice') || '') : DISABLED,
                    onChange: () => {} // replaced below (needs both selects)
                }).select;
            const inSel = sec.select('Microphone', null,
                [[DISABLED, 'Disabled'], ['', 'System default']], {
                    numeric: false,
                    value: (micAllowed && S.get('audioMicrophone') && S.get('audioPlayback'))
                        ? (S.get('audioInputDevice') || '') : DISABLED,
                    onChange: () => {}
                }).select;

            sec.note('Disabled reports that no headphones (or no microphone) are attached to the emulated Pro Controller 2.');
            if (!caps.isSecureContext)
                sec.note('Playback works on plain HTTP. The microphone needs a secure origin: in Chrome add this address under chrome://flags/#unsafely-treat-insecure-origin-as-secure, or use HTTPS (e.g. Tailscale/Caddy).', true);
            sec.note('Audio is S16LE stereo 48 kHz with a ~50 ms jitter buffer. Expect more latency than the desktop client.');

            // Device labels appear once a permission grants access to them.
            if (navigator.mediaDevices && navigator.mediaDevices.enumerateDevices) {
                navigator.mediaDevices.enumerateDevices().then(devs => {
                    const canSetSink = typeof AudioContext !== 'undefined'
                        && typeof AudioContext.prototype.setSinkId === 'function';
                    for (const d of devs) {
                        const label = d.label || (d.kind + ' ' + d.deviceId.slice(0, 6));
                        if (d.kind === 'audiooutput' && canSetSink)
                            outSel.appendChild(el('option', { value: d.deviceId, text: label }));
                        else if (d.kind === 'audioinput' && micAllowed)
                            inSel.appendChild(el('option', { value: d.deviceId, text: label }));
                    }
                    outSel.value = S.get('audioPlayback') ? (S.get('audioOutputDevice') || '') : DISABLED;
                    inSel.value = (micAllowed && S.get('audioMicrophone') && S.get('audioPlayback'))
                        ? (S.get('audioInputDevice') || '') : DISABLED;
                    updateMicAvailability();
                }).catch(() => {});
            }

            const updateMicAvailability = () => {
                const headphones = outSel.value !== DISABLED;
                inSel.disabled = !headphones || !micAllowed;
                if (!headphones) inSel.value = DISABLED;
            };
            outSel.onchange = async () => {
                if (outSel.value === DISABLED) {
                    S.set('audioPlayback', false);
                    S.set('audioMicrophone', false);
                } else {
                    S.set('audioOutputDevice', outSel.value);
                    S.set('audioPlayback', true);
                    if (ctx && typeof ctx.setSinkId === 'function') { try { await ctx.setSinkId(outSel.value || ''); } catch (_) {} }
                }
                updateMicAvailability();
            };
            inSel.onchange = () => {
                if (inSel.value === DISABLED) {
                    S.set('audioMicrophone', false);
                } else {
                    S.set('audioInputDevice', inSel.value);
                    S.set('audioMicrophone', true);
                    if (micNode) { stopMic(); startMic(); }
                }
            };
            updateMicAvailability();
        }
    });
})();
