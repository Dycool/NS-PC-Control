// feat_settings.js — Settings drawer + controller type + player reactivity.
//
// Owns the web-native settings surface (a slide-in drawer, responsive on
// mobile) and the richer P1..P4 status badges (player LEDs, body RGB) fed by
// the Roster/ControllerStatus/Assignment packets the server already sends.
// Other feat_* modules contribute their own rows via the settingsUI hook.
'use strict';

(function () {
    const C = NSCore.C, S = NSCore.settings, caps = NSCore.caps, el = NSCore.el;

    let page = 'index';
    let drawer = null, backdrop = null, drawerBody = null;
    let gateSignature = '';
    // Change Server Type (gadget mode) state — survives drawer rebuilds.
    let serverTypeChoice = C.GADGET_FAMILY_SWITCH1;
    let gadgetStatus = '';
    let gadgetStatusEl = null;
    let gadgetSeq = 1;

    // ── Drawer shell ─────────────────────────────────────────────────────────
    function ensureDrawer() {
        if (drawer) return;
        backdrop = el('div', { class: 'ns-backdrop', onclick: closeDrawer });
        drawerBody = el('div', { class: 'ns-drawer-body' });
        drawer = el('div', { class: 'ns-drawer' }, [
            el('div', { class: 'ns-drawer-header' }, [
                el('h3', { text: 'Settings' }),
                el('button', { class: 'ns-drawer-close', text: '✕', title: 'Close', onclick: closeDrawer })
            ]),
            drawerBody
        ]);
        document.body.appendChild(backdrop);
        document.body.appendChild(drawer);
    }
    function openDrawer() {
        ensureDrawer();
        rebuildBody();
        backdrop.classList.add('open');
        drawer.classList.add('open');
    }
    function closeDrawer() {
        if (!drawer) return;
        backdrop.classList.remove('open');
        drawer.classList.remove('open');
    }

    // ── Section/row builder API (shared with the other features) ────────────
    function makeUi(body) {
        return {
            section(title) {
                const sec = el('div', { class: 'ns-section' },
                    [el('div', { class: 'ns-section-title', text: title })]);
                body.appendChild(sec);
                const api = {
                    el: sec,
                    toggle(label, key, o = {}) {
                        const input = el('input', { type: 'checkbox' });
                        input.checked = !!S.get(key);
                        if (o.disabled) input.disabled = true;
                        input.onchange = async () => {
                            let v = input.checked;
                            if (o.beforeChange) {
                                const ok = await o.beforeChange(v);
                                if (ok === false) { input.checked = !v; return; }
                            }
                            S.set(key, input.checked);
                        };
                        const row = el('div', { class: 'ns-setting-row' }, [
                            el('label', { text: label }),
                            el('label', { class: 'ns-toggle' }, [input, el('span', { class: 'slider' })])
                        ]);
                        sec.appendChild(row);
                        if (o.note) api.note(o.note, o.warnNote);
                        return { row, input };
                    },
                    select(label, key, options, o = {}) {
                        const sel = el('select', { class: 'ns-select' });
                        for (const [value, text] of options) sel.appendChild(el('option', { value, text }));
                        sel.value = String(o.value !== undefined ? o.value : S.get(key));
                        if (o.disabled) sel.disabled = true;
                        sel.onchange = () => {
                            if (o.onChange) o.onChange(sel.value);
                            else S.set(key, o.numeric === false ? sel.value : parseInt(sel.value, 10));
                        };
                        const row = el('div', { class: 'ns-setting-row' },
                            [el('label', { text: label }), sel]);
                        sec.appendChild(row);
                        return { row, select: sel };
                    },
                    range(label, key, min, max, step, o = {}) {
                        const val = el('span', { class: 'ns-range-value', text: Number(S.get(key)).toFixed(2) });
                        const input = el('input', { class: 'ns-range', type: 'range', min, max, step });
                        input.value = S.get(key);
                        if (o.disabled) input.disabled = true;
                        input.oninput = () => {
                            val.textContent = Number(input.value).toFixed(2);
                            S.set(key, parseFloat(input.value));
                        };
                        const row = el('div', { class: 'ns-setting-row' },
                            [el('label', { text: label }), input, val]);
                        sec.appendChild(row);
                        return { row, input };
                    },
                    note(text, warn) {
                        sec.appendChild(el('div', { class: 'ns-note' + (warn ? ' ns-warn' : ''), text }));
                    },
                    button(label, onclick, o = {}) {
                        const b = el('button', {
                            class: 'ns-btn' + (o.accent ? ' ns-btn-accent' : ''), text: label, onclick
                        });
                        if (o.disabled) b.disabled = true;
                        sec.appendChild(el('div', { class: 'ns-setting-row' }, [b]));
                        return b;
                    },
                    custom(node) { sec.appendChild(node); }
                };
                return api;
            }
        };
    }

    function rebuildBody() {
        drawerBody.innerHTML = '';
        const ui = makeUi(drawerBody);

        // Mirror the desktop ns-client SettingsDialog ordering: Gyro / motion,
        // Rumble, Switch 2 headset, shortcuts, mouse, Emulated controller
        // (+ Horizontal mode), then Change Server Type.
        const called = ['settings'];
        const contrib = (id) => {
            called.push(id);
            const f = NSCore.getFeature(id);
            if (f && f.settingsUI) {
                try { f.settingsUI(ui, page); }
                catch (err) { console.error('[settings] ' + id + ' section failed', err); }
            }
        };

        contrib('motion');
        contrib('rumble');
        contrib('audio');

        const sys = ui.section('Shortcuts');
        sys.toggle('Home shortcut (LStick + RStick)', 'homeShortcut');
        sys.toggle('Capture shortcut (Minus + Plus)', 'captureShortcut');

        contrib('mouse');
        buildControllerSection(ui);
        contrib('amiibo');
        buildServerTypeSection(ui);

        // Future features that were not explicitly placed above.
        NSCore.dispatch.settingsUI(ui, page, called);

        buildStatusSection(ui);
    }

    function buildControllerSection(ui) {
        const ctrl = ui.section('Controller');
        if (page === 'mobile') {
            // The touch page keeps its layout-linked controller type; changing it
            // reloads so controller_layouts.js re-applies skin + control set.
            const current = parseInt(localStorage.getItem('nswc_controller_type') || '3', 10);
            ctrl.select('Emulated controller', null, [
                ['3', 'Pro Controller'], ['1', 'Joy-Con (L)'], ['2', 'Joy-Con (R)']
            ], {
                value: current,
                onChange: (v) => {
                    localStorage.setItem('nswc_controller_type', String(parseInt(v, 10)));
                    NSCore.settings.set('controllerType', parseInt(v, 10));
                    location.reload();
                }
            });
            ctrl.note('Changing the type reloads the touch layout.');
        } else if (caps.isNative) {
            // Native app main menu: physical controllers are driven natively, so
            // the type feeds the NSBridge (and the legacy nswc key it reads).
            const current = parseInt(localStorage.getItem('nswc_controller_type') || '3', 10);
            ctrl.select('Emulated controller', null, [
                ['3', 'Pro Controller'], ['1', 'Joy-Con (L)'], ['2', 'Joy-Con (R)']
            ], {
                value: [1, 2, 3].includes(current) ? current : 3,
                onChange: (v) => {
                    const type = parseInt(v, 10);
                    localStorage.setItem('nswc_controller_type', String(type));
                    S.set('controllerType', type);
                    try {
                        if (window.NSBridge && NSBridge.onPhysicalControllerType)
                            NSBridge.onPhysicalControllerType(type);
                    } catch (_) {}
                }
            });
        } else {
            const typeSel = ctrl.select('Emulated controller', 'controllerType', [
                [String(C.TYPE_PRO), 'Pro Controller'],
                [String(C.TYPE_JOYCON_L), 'Joy-Con (L)'],
                [String(C.TYPE_JOYCON_R), 'Joy-Con (R)'],
                [String(C.TYPE_JOYCON_PAIR), 'Joy-Con L + R Pair']
            ]).select;
            const isJoycon = (t) => t === C.TYPE_JOYCON_L || t === C.TYPE_JOYCON_R;
            const horizontal = ctrl.toggle('Horizontal mode', 'joyconHorizontal', {
                disabled: !isJoycon(S.get('controllerType'))
            });
            typeSel.addEventListener('change', () => {
                horizontal.input.disabled = !isJoycon(S.get('controllerType'));
            });
            ctrl.note('Applied to every live pad. In Switch 2 mode the server maps it to the S2 family (Joy-Con Pair is not supported there).');
        }
    }

    // ── Change Server Type (gadget mode) — mirrors the desktop dialog ───────
    function setGadgetStatus(text) {
        gadgetStatus = text;
        if (gadgetStatusEl) gadgetStatusEl.textContent = text;
    }
    function sendGadgetModeRequest() {
        if (!NSCore.state.connected) {
            setGadgetStatus('Connect to the server first.');
            return;
        }
        const buf = new ArrayBuffer(C.GADGET_MODE_REQUEST_SIZE);
        const v = new DataView(buf);
        v.setUint32(0, C.GADGET_MODE_MAGIC, true);
        v.setUint8(4, C.GADGET_MODE_VERSION);
        v.setUint8(5, serverTypeChoice & 0xFF);
        v.setUint32(8, gadgetSeq++ >>> 0, true);
        // bytes 12..27: HMAC field, zeroed — WS is the trusted transport (the
        // UDP equivalent used by the desktop client stays HMAC-verified).
        if (NSCore.wsSend(buf)) setGadgetStatus('Sending request...');
        else setGadgetStatus('Not connected.');
    }
    function buildServerTypeSection(ui) {
        if (caps.isNative) return; // native apps use UDP; no WS to carry this
        const sec = ui.section('Change Server Type');
        sec.note('Choose the USB controller identity the server should emulate. The server only accepts this while no other client is connected and will briefly restart its USB gadget after accepting the change.');
        sec.select('Controller type', null, [
            [String(C.GADGET_FAMILY_HORI), 'HORI'],
            [String(C.GADGET_FAMILY_SWITCH1), 'Switch 1 (Pro Controller)'],
            [String(C.GADGET_FAMILY_SWITCH2), 'Switch 2 (Pro Controller 2)']
        ], {
            value: serverTypeChoice,
            onChange: (v) => { serverTypeChoice = parseInt(v, 10); }
        });
        sec.button('Send', sendGadgetModeRequest, {
            accent: true,
            disabled: !NSCore.state.connected
        });
        gadgetStatusEl = NSCore.el('div', { class: 'ns-note', text: gadgetStatus
            || (NSCore.state.connected ? '' : 'Connect to the server first.') });
        sec.custom(gadgetStatusEl);
    }

    function buildStatusSection(ui) {
        const about = ui.section('Status');
        const a = NSCore.state.assignment;
        about.note(NSCore.state.connected
            ? 'Connected — slot ' + (a.serverSlot === 255 ? '?' : a.serverSlot + 1)
              + ', clients ' + a.activeClients + '/' + a.maxClients
              + (NSCore.s2Active() ? ' — Switch 2 mode' : '')
            : 'Not connected.');
        if (!caps.isSecureContext && !caps.isNative)
            about.note('Served over plain HTTP: gyro and microphone are disabled by the browser (everything else works). See the Motion and Switch 2 headset sections for home-network workarounds.', true);
    }

    // Rebuild the open drawer only when a gating fact changes.
    function currentGateSignature() {
        return [NSCore.state.connected, NSCore.s2Active(), NSCore.s2NfcAssigned(),
                NSCore.s2AudioEligible()].join('|');
    }
    NSCore.onStateChanged(() => {
        const sig = currentGateSignature();
        if (sig !== gateSignature) {
            gateSignature = sig;
            if (drawer && drawer.classList.contains('open')) rebuildBody();
        }
        renderBadges();
    });
    // The controller type gates other rows (Horizontal mode, Joycon Mouse
    // Mode, S2 audio eligibility) — refresh the open drawer when it changes.
    NSCore.settings.onChange('controllerType', () => {
        if (drawer && drawer.classList.contains('open')) rebuildBody();
    });

    // ── Player badges (index page): LEDs, body RGB, hidden ports ────────────
    let badgeEls = null;
    function ensureBadges() {
        if (badgeEls !== null) return badgeEls;
        badgeEls = [];
        for (let p = 0; p < 4; p++) {
            const holder = document.getElementById('p' + (p + 1) + 'Badges');
            if (!holder) { badgeEls = []; return badgeEls; }
            const leds = [];
            const ledBox = el('span', { class: 'ns-leds' });
            for (let i = 0; i < 4; i++) {
                const led = el('span', { class: 'ns-led' });
                leds.push(led);
                ledBox.appendChild(led);
            }
            const rgb = el('span', { class: 'ns-rgb', style: 'display:none' });
            const batt = el('span', { class: 'ns-batt' });
            holder.appendChild(rgb);
            holder.appendChild(ledBox);
            holder.appendChild(batt);
            badgeEls.push({ holder, leds, rgb, batt, row: holder.closest('.player-row') });
        }
        return badgeEls;
    }
    // Console port -> our subpad (via the assignment masks), for LED/RGB data.
    function subpadForPort(port) {
        const subs = NSCore.state.assignment.subpads;
        for (let s = 0; s < 4; s++)
            if (subs[s].valid && (subs[s].mask & (1 << port))) return s;
        return -1;
    }
    function renderBadges() {
        if (page !== 'index') return;
        const els = ensureBadges();
        if (!els.length) return;
        const roster = NSCore.state.roster;
        for (let p = 0; p < 4; p++) {
            const b = els[p];
            const entry = roster.valid ? roster.ports[p] : null;
            if (entry && entry.present === 2) { // hidden port
                if (b.row) b.row.style.display = 'none';
                continue;
            }
            if (b.row) b.row.style.display = '';
            const sp = subpadForPort(p);
            const st = sp >= 0 ? NSCore.state.status[sp] : null;
            for (let i = 0; i < 4; i++)
                b.leds[i].className = 'ns-led' + (st && (st.playerLeds & (1 << i)) ? ' on' : '');
            if (st && st.rgb) {
                b.rgb.style.display = '';
                b.rgb.style.background = 'rgb(' + st.rgb[0] + ',' + st.rgb[1] + ',' + st.rgb[2] + ')';
            } else {
                b.rgb.style.display = 'none';
            }
            // Battery: we only know our own client battery; show it on our pads.
            if (sp >= 0 && NSCore.state.battery.percent !== null) {
                b.batt.textContent = NSCore.state.battery.percent + '%';
                b.batt.className = 'ns-batt' + (NSCore.state.battery.charging ? ' charging' : '');
            } else {
                b.batt.textContent = '';
            }
        }
    }

    // ── Feature registration ─────────────────────────────────────────────────
    NSCore.registerFeature({
        id: 'settings',
        onWsMessage(magic, view) {
            if (magic === C.CONTROLLER_STATUS_MAGIC && view.byteLength === C.CONTROLLER_STATUS_SIZE) {
                NSCore.parseControllerStatus(view);
                return true;
            }
            if (magic === C.GADGET_MODE_MAGIC && view.byteLength === C.GADGET_MODE_REPLY_SIZE) {
                const result = view.getUint8(5);
                const activeClients = view.getUint8(7);
                if (result === C.GADGET_MODE_RESULT_RESTARTING)
                    setGadgetStatus('The server accepted the change and is restarting its USB gadget. Reconnect in a few seconds.');
                else if (result === C.GADGET_MODE_RESULT_UNCHANGED)
                    setGadgetStatus('The server is already running that controller type.');
                else if (result === C.GADGET_MODE_RESULT_SERVER_FULL)
                    setGadgetStatus('Server is full: ' + activeClients + ' client(s) still connected. Disconnect them first.');
                else
                    setGadgetStatus('Unexpected response from server.');
                return true;
            }
            return false;
        },
        mountUI(p) {
            page = p;
            gateSignature = currentGateSignature();
            if (page === 'index') {
                const btn = document.getElementById('btnSettings');
                if (btn) btn.onclick = openDrawer;
                renderBadges();
            } else {
                // Touch page: floating gear (skip inside the native app WebView,
                // where settings live in the native UI).
                if (!caps.isNative) {
                    const gear = el('button', { class: 'ns-gear', text: '⚙︎', title: 'Settings', onclick: openDrawer });
                    const surface = document.getElementById('gamepad') || document.body;
                    surface.appendChild(gear);
                }
            }
        }
    });
})();
