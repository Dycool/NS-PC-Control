// feat_amiibo.js — searchable, persistent Amiibo library for Switch 2.
//
// The release workflow embeds a metadata-only catalogue plus synthetic,
// encrypted factory templates. Runtime never downloads a database or asks the
// user for retail keys or tag dumps.
'use strict';

(function () {
    const C = NSCore.C, el = NSCore.el;
    const CATALOGUE_URL = 'data/amiibo_catalog.json';

    let page = 'index';
    let scanRequested = [false, false, false, false];
    let scanDeadline = [0, 0, 0, 0];
    let lastSeq = [null, null, null, null];
    let card = null, statusEl = null, chooseBtn = null, settingsChooseBtn = null;
    let modalBackdrop = null, modal = null, searchInput = null, seriesSelect = null;
    let results = null, modalStatus = null;
    let previewImg = null, previewPlaceholder = null, previewName = null, useBtn = null;
    let selectedAmiiboObj = null;
    let catalogue = [];
    let cataloguePromise = null;
    let selectionPending = false;

    function activeSubpad() {
        for (let s = 0; s < 4; s++) if (scanRequested[s]) return s;
        return 0;
    }

    function normalizeCatalogue(raw) {
        const seen = new Set();
        return (raw && Array.isArray(raw.amiibo) ? raw.amiibo : [])
            .map(a => ({
                head: String(a.head || '').toLowerCase(),
                tail: String(a.tail || '').toLowerCase(),
                name: String(a.name || ''),
                character: String(a.character || ''),
                gameSeries: String(a.gameSeries || ''),
                amiiboSeries: String(a.amiiboSeries || ''),
                type: String(a.type || '')
            }))
            .filter(a => /^[0-9a-f]{8}$/.test(a.head)
                && /^[0-9a-f]{8}$/.test(a.tail) && a.name
                && !seen.has(a.head + a.tail)
                && seen.add(a.head + a.tail))
            .sort((a, b) => a.gameSeries.localeCompare(b.gameSeries)
                || a.name.localeCompare(b.name));
    }

    async function loadCatalogue() {
        if (cataloguePromise) return cataloguePromise;
        cataloguePromise = (async () => {
            try {
                let raw = window.NS_AMIIBO_CATALOG;
                if (!raw) {
                    const response = await fetch(CATALOGUE_URL, {
                        headers: { Accept: 'application/json' }
                    });
                    if (!response.ok) throw new Error('HTTP ' + response.status);
                    raw = await response.json();
                }
                const fresh = normalizeCatalogue(raw);
                if (!fresh.length) throw new Error('catalogue response was empty');
                catalogue = fresh;
                rebuildSeries();
                renderResults();
                setModalStatus(catalogue.length
                    + ' Amiibo available offline. Catalogue metadata: AmiiboAPI.');
            } catch (error) {
                setModalStatus(
                    'Could not load the bundled Amiibo catalogue: '
                    + error.message, true);
            }
            return catalogue;
        })();
        return cataloguePromise;
    }

    function sendLibraryCommand(action, subpad, amiibo) {
        if (!NSCore.state.connected) return false;
        if (window.NSBridge
                && typeof window.NSBridge.onAmiiboLibrary === 'function') {
            const head = amiibo ? amiibo.head : '00000000';
            const tail = amiibo ? amiibo.tail : '00000000';
            window.NSBridge.onAmiiboLibrary(action, subpad & 3, head, tail);
            return true;
        }
        const buffer = new ArrayBuffer(C.AMIIBO_LIBRARY_PACKET_SIZE);
        const view = new DataView(buffer);
        view.setUint32(0, C.AMIIBO_LIBRARY_MAGIC, true);
        view.setUint8(4, C.AMIIBO_LIBRARY_VERSION);
        view.setUint8(5, action);
        view.setUint8(6, subpad & 3);
        if (amiibo) {
            view.setUint32(8, parseInt(amiibo.head, 16) >>> 0, true);
            view.setUint32(12, parseInt(amiibo.tail, 16) >>> 0, true);
        }
        // The trailing bytes are the UDP HMAC. WebSocket is the trusted,
        // session-scoped transport and intentionally leaves them zero.
        return NSCore.wsSend(buffer);
    }

    function selectAmiibo(amiibo) {
        const subpad = activeSubpad();
        if (!scanRequested[subpad] || selectionPending) return;
        if (!sendLibraryCommand(C.AMIIBO_LIBRARY_SELECT, subpad, amiibo)) {
            alert('Could not send the Amiibo selection to the server.');
            return;
        }
        selectionPending = true;
        setStatus('Selecting ' + amiibo.name + ' in the server Amiibo library…');
        setModalStatus('Sending ' + amiibo.name + ' to the server…');
    }

    function onLibraryResult(view) {
        if (view.byteLength !== C.AMIIBO_LIBRARY_RESULT_SIZE
                || view.getUint8(4) !== C.AMIIBO_LIBRARY_VERSION) return;
        const action = view.getUint8(5);
        const result = view.getUint8(6);
        const subpad = view.getUint8(7);
        if (action === C.AMIIBO_LIBRARY_SELECT) selectionPending = false;
        if (result === C.AMIIBO_LIBRARY_OK) {
            if (action === C.AMIIBO_LIBRARY_SELECT) {
                if (subpad < 4) {
                    scanRequested[subpad] = false;
                    scanDeadline[subpad] = 0;
                }
                closePicker();
                setStatus('Amiibo selected and sent to the console.');
            } else if (action === C.AMIIBO_LIBRARY_CLEAR) {
                setStatus('All private Amiibo data was cleared from the server.');
            }
        } else {
            const messages = {
                [C.AMIIBO_LIBRARY_STORAGE_ERROR]:
                    'The server could not update its private Amiibo data folder.',
                [C.AMIIBO_LIBRARY_GENERATION_ERROR]:
                    'This build does not contain a valid template for that Amiibo.',
                [C.AMIIBO_LIBRARY_INVALID_REQUEST]:
                    'The server rejected the Amiibo library request.'
            };
            const message = messages[result] || 'Unknown Amiibo library error.';
            setStatus(message);
            if (action === C.AMIIBO_LIBRARY_SELECT) {
                openPicker();
                setModalStatus(message, true);
            } else {
                alert(message);
            }
        }
        refreshUi();
    }

    function onWriteback(view) {
        const len = view.getUint16(5, true);
        const supported = len === C.AMIIBO_TAGMO_DUMP_SIZE
            || len === C.AMIIBO_RAW_DUMP_SIZE
            || len === C.AMIIBO_EXTENDED_DUMP_SIZE
            || len === C.AMIIBO_V3_DUMP_SIZE;
        if (!supported || view.byteLength < C.AMIIBO_DATA_HEADER + len) return;
        const subpad = view.getUint8(4);
        if (subpad < 4) {
            scanRequested[subpad] = false;
            scanDeadline[subpad] = 0;
        }
        setStatus('Console updated the Amiibo; writeback saved in the server library.');
        refreshUi();
    }

    function handleScanRequest(subpad, requested, sequence) {
        if (subpad >= 4) return;
        const previous = lastSeq[subpad];
        const difference = previous === null
            ? 1 : (sequence - previous) & 0xffff;
        if (sequence !== 0
                && (difference === 0 || difference >= 0x8000)) return;
        if (sequence !== 0) lastSeq[subpad] = sequence;
        scanRequested[subpad] = requested;
        scanDeadline[subpad] = requested ? Date.now() + 10000 : 0;
        if (requested) {
            const deadline = scanDeadline[subpad];
            setTimeout(() => {
                if (scanDeadline[subpad] !== deadline
                        || Date.now() < deadline) return;
                scanRequested[subpad] = false;
                scanDeadline[subpad] = 0;
                setStatus('Waiting for the console to request an Amiibo scan.');
                refreshUi();
            }, 10050);
        }
        setStatus(requested
            ? 'Console is waiting for an Amiibo — choose one from the library.'
            : 'Waiting for the console to request an Amiibo scan.');
        refreshUi();
    }

    // NS Mobile's main menu uses the native authenticated UDP transport rather
    // than a WebSocket. Android/iOS call these hooks with the same packet
    // fields their receive loops parsed; touch controls and the editor never
    // call or expose this UI.
    window.__nsAmiiboNativeAssignment =
        (connected, subpad, mask, virtualType) => {
            if (!connected) {
                NSCore.dispatch.disconnect();
                return;
            }
            const index = Number(subpad) & 3;
            NSCore.state.connected = true;
            NSCore.state.assignment.accepted = true;
            NSCore.state.assignment.subpads[index] = {
                valid: true,
                mask: Number(mask) & 0xff,
                primary: 0,
                requestedType: C.TYPE_DEFAULT,
                virtualType: Number(virtualType) & 0xff
            };
            NSCore.emitStateChanged();
        };
    window.__nsAmiiboNativeRequest = (subpad, requested, sequence) => {
        handleScanRequest(
            Number(subpad) & 3, !!requested, Number(sequence) & 0xffff);
    };
    window.__nsAmiiboNativeResult =
        (action, result, subpad, head, tail, tagSize) => {
            const buffer = new ArrayBuffer(C.AMIIBO_LIBRARY_RESULT_SIZE);
            const view = new DataView(buffer);
            view.setUint32(0, C.AMIIBO_LIBRARY_RESULT_MAGIC, true);
            view.setUint8(4, C.AMIIBO_LIBRARY_VERSION);
            view.setUint8(5, Number(action) & 0xff);
            view.setUint8(6, Number(result) & 0xff);
            view.setUint8(7, Number(subpad) & 3);
            view.setUint32(8, Number(head) >>> 0, true);
            view.setUint32(12, Number(tail) >>> 0, true);
            view.setUint16(16, Number(tagSize) & 0xffff, true);
            onLibraryResult(view);
        };

    function setStatus(text) { if (statusEl) statusEl.textContent = text; }
    function setModalStatus(text, warning) {
        if (!modalStatus) return;
        modalStatus.textContent = text;
        modalStatus.classList.toggle('ns-warn', !!warning);
    }

    function highlightAmiibo(amiibo, targetEl) {
        selectedAmiiboObj = amiibo;
        if (results) {
            results.querySelectorAll('.ns-amiibo-item').forEach(btn => btn.classList.remove('selected'));
        }
        if (targetEl) targetEl.classList.add('selected');
        if (previewImg && previewPlaceholder && previewName && useBtn) {
            const imgUrl = 'https://raw.githubusercontent.com/8bitDream/AmiiboAPI/master/images/icon_'
                + amiibo.head + '-' + amiibo.tail + '.png';
            previewImg.src = imgUrl;
            previewImg.style.display = 'block';
            previewPlaceholder.style.display = 'none';
            previewName.textContent = amiibo.name;
            useBtn.disabled = false;
        }
    }

    function ensurePicker() {
        if (modal) return;
        searchInput = el('input', {
            class: 'ns-amiibo-search', type: 'search',
            placeholder: 'Search name, character, series or type…'
        });
        seriesSelect = el('select', { class: 'ns-select ns-amiibo-series' });
        searchInput.oninput = renderResults;
        seriesSelect.onchange = renderResults;
        results = el('div', { class: 'ns-amiibo-grid' });
        
        previewImg = el('img', {
            class: 'ns-amiibo-preview-img',
            alt: 'Amiibo preview',
            style: 'display:none;'
        });
        previewPlaceholder = el('div', {
            class: 'ns-amiibo-preview-placeholder',
            text: 'Select an Amiibo'
        });
        previewName = el('div', {
            class: 'ns-amiibo-preview-name',
            text: ''
        });
        useBtn = el('button', {
            class: 'ns-amiibo-select-btn',
            text: 'Use Selected Amiibo',
            disabled: true,
            onclick: () => {
                if (selectedAmiiboObj) selectAmiibo(selectedAmiiboObj);
            }
        });
        const previewPanel = el('div', { class: 'ns-amiibo-preview' }, [
            previewImg,
            previewPlaceholder,
            previewName,
            useBtn
        ]);

        const bodyContainer = el('div', { class: 'ns-amiibo-body' }, [
            results,
            previewPanel
        ]);

        modalStatus = el('div', {
            class: 'ns-note',
            text: 'Loading Amiibo catalogue…'
        });
        modal = el('section', {
            class: 'ns-amiibo-modal', role: 'dialog',
            'aria-modal': 'true', 'aria-label': 'Scan Amiibo'
        }, [
            el('div', { class: 'ns-amiibo-header' }, [
                el('div', {}, [
                    el('h2', { text: 'Scan Amiibo' })
                ]),
                el('button', {
                    class: 'ns-drawer-close', text: '×',
                    title: 'Close', onclick: closePicker
                })
            ]),
            el('div', { class: 'ns-amiibo-toolbar' }, [
                searchInput, seriesSelect
            ]),
            bodyContainer,
            modalStatus
        ]);
        modalBackdrop = el('div', {
            class: 'ns-amiibo-backdrop', onclick: event => {
                if (event.target === modalBackdrop) closePicker();
            }
        }, [modal]);
        document.body.appendChild(modalBackdrop);
        document.addEventListener('keydown', event => {
            if (event.key === 'Escape'
                    && modalBackdrop.classList.contains('open')) closePicker();
        });
    }

    function rebuildSeries() {
        if (!seriesSelect) return;
        const selected = seriesSelect.value;
        const series = [...new Set(catalogue.map(a => a.gameSeries).filter(Boolean))]
            .sort((a, b) => a.localeCompare(b));
        seriesSelect.innerHTML = '';
        seriesSelect.appendChild(el('option', { value: '', text: 'All game series' }));
        series.forEach(name =>
            seriesSelect.appendChild(el('option', { value: name, text: name })));
        seriesSelect.value = series.includes(selected) ? selected : '';
    }

    function renderResults() {
        if (!results) return;
        const query = (searchInput.value || '').trim().toLocaleLowerCase();
        const series = seriesSelect.value;
        const matching = catalogue.filter(a => {
            if (series && a.gameSeries !== series) return false;
            if (!query) return true;
            return [a.name, a.character, a.gameSeries, a.amiiboSeries, a.type]
                .join(' ').toLocaleLowerCase().includes(query);
        });
        results.innerHTML = '';
        matching.slice(0, 160).forEach(amiibo => {
            const itemBtn = el('button', {
                class: 'ns-amiibo-item' + (selectedAmiiboObj === amiibo ? ' selected' : ''),
                title: 'Select ' + amiibo.name,
                onclick: (e) => highlightAmiibo(amiibo, e.currentTarget),
                ondblclick: () => selectAmiibo(amiibo)
            }, [
                el('span', { class: 'ns-amiibo-item-name', text: amiibo.name })
            ]);
            results.appendChild(itemBtn);
        });
        if (matching.length > 160) {
            results.appendChild(el('div', {
                class: 'ns-note ns-amiibo-limit',
                text: 'Showing 160 of ' + matching.length
                    + ' results. Refine the search to see the rest.'
            }));
        } else if (!matching.length && catalogue.length) {
            results.appendChild(el('div', {
                class: 'ns-note ns-amiibo-empty',
                text: 'No Amiibo match these filters.'
            }));
        }
    }

    function openPicker() {
        if (!NSCore.state.connected || !NSCore.s2NfcAssigned()
                || !scanRequested.some(Boolean)) return;
        ensurePicker();
        modalBackdrop.classList.add('open');
        document.body.classList.add('ns-modal-open');
        searchInput.focus();
        loadCatalogue();
    }
    function closePicker() {
        if (!modalBackdrop) return;
        modalBackdrop.classList.remove('open');
        document.body.classList.remove('ns-modal-open');
    }

    function ensureIndexCard() {
        if (card) return;
        const shell = document.querySelector('.shell');
        if (!shell) return;
        statusEl = el('div', {
            class: 'status',
            text: 'Waiting for the console to request an Amiibo scan.'
        });
        chooseBtn = el('button', { text: 'Scan Amiibo…', onclick: openPicker });
        card = el('section', { class: 'card' }, [
            el('div', { class: 'card-title', text: 'Amiibo (Switch 2)' }),
            el('div', { class: 'btn-group' }, [chooseBtn]),
            statusEl
        ]);
        card.style.display = 'none';
        shell.appendChild(card);
    }
    function refreshUi() {
        const eligible = NSCore.s2NfcAssigned();
        const waiting = scanRequested.some(Boolean);
        if (page === 'index') {
            ensureIndexCard();
            if (card) card.style.display = eligible ? '' : 'none';
            if (chooseBtn)
                chooseBtn.disabled = !(NSCore.state.connected && waiting);
            if (settingsChooseBtn)
                settingsChooseBtn.disabled = !(NSCore.state.connected && waiting);
        }
    }

    NSCore.registerFeature({
        id: 'amiibo',
        mountUI(p) {
            page = p;
            // The Amiibo library belongs exclusively to the main menu. Do not
            // inject even hidden picker controls into Touch Controls/Editor.
            if (page !== 'index') return;
            ensurePicker();
            NSCore.onStateChanged(refreshUi);
            refreshUi();
        },
        onDisconnect() {
            scanRequested = [false, false, false, false];
            scanDeadline = [0, 0, 0, 0];
            lastSeq = [null, null, null, null];
            selectionPending = false;
            closePicker();
            setStatus('Waiting for the console to request an Amiibo scan.');
            refreshUi();
        },
        onWsMessage(magic, view) {
            if (magic === C.AMIIBO_REQUEST_MAGIC
                    && view.byteLength === C.AMIIBO_REQUEST_SIZE) {
                const request = NSCore.parseAmiiboRequest(view);
                handleScanRequest(
                    request.subpad, request.requested, request.sequence);
                return true;
            }
            if (magic === C.AMIIBO_LIBRARY_RESULT_MAGIC) {
                onLibraryResult(view);
                return true;
            }
            if (magic === C.AMIIBO_DATA_MAGIC
                    && view.byteLength >= C.AMIIBO_DATA_HEADER) {
                onWriteback(view);
                return true;
            }
            return false;
        },
        settingsUI(ui) {
            if (page !== 'index' || !NSCore.state.connected
                    || !NSCore.s2Active()) return;
            const section = ui.section('Amiibo library');
            settingsChooseBtn = section.button('Scan Amiibo…', openPicker, {
                disabled: !(NSCore.state.connected
                    && NSCore.s2NfcAssigned()
                    && scanRequested.some(Boolean))
            });
            section.button('Clear Amiibo Data…', () => {
                if (!NSCore.state.connected) return;
                if (!confirm(
                    'Permanently remove every saved Amiibo and all console '
                    + 'writebacks from the NS-PC-Control server? Bundled '
                    + 'factory templates remain available.')) return;
                if (!sendLibraryCommand(C.AMIIBO_LIBRARY_CLEAR, 0, null))
                    alert('Could not send the clear request to the server.');
            }, { disabled: !NSCore.state.connected });
            section.note(
                'The release contains an offline metadata catalogue and '
                + 'ready-to-use templates. No runtime downloads or key files are required.');
        }
    });
})();
