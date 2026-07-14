(function () {
    const PRO = 3, JOYCON_L = 1, JOYCON_R = 2;

    // Last shipped Joy-Con defaults. If a saved layout still matches these
    // exactly, silently migrate it to the new shared-template defaults below.
    const legacyDefaults = {
        [JOYCON_L]: {
            'btn-zl': {l:31, t:2, w:38, h:8}, 'btn-l': {l:34, t:11, w:32, h:7},
            'btn-sl': {l:64, t:33, w:6, h:16}, 'btn-sr': {l:64, t:53, w:6, h:16},
            'btn-minus': {l:57, t:18, w:6}, 'btn-capture': {l:59, t:75, w:6},
            'lstick': {l:38, t:22, w:19}, 'btn-ls': {l:34, t:46, w:6},
            'dpad': {l:40, t:56, w:19}
        },
        [JOYCON_R]: {
            'btn-zr': {l:31, t:2, w:38, h:8}, 'btn-r': {l:34, t:11, w:32, h:7},
            'btn-sl': {l:30, t:33, w:6, h:16}, 'btn-sr': {l:30, t:53, w:6, h:16},
            'btn-plus': {l:37, t:18, w:6}, 'btn-home': {l:37, t:75, w:6},
            'abxy': {l:44, t:22, w:19},
            'rstick': {l:39, t:56, w:19}, 'btn-rs': {l:58, t:70, w:6}
        }
    };

    const layouts = {
        [PRO]: {
            'btn-zl': {l:4, t:4, w:14, h:8}, 'btn-l': {l:4, t:14, w:14, h:8},
            'btn-zr': {l:82, t:4, w:14, h:8}, 'btn-r': {l:82, t:14, w:14, h:8},
            'btn-sl': {l:38, t:29, w:9, h:7}, 'btn-sr': {l:53, t:29, w:9, h:7},
            'btn-minus': {l:38, t:5, w:6}, 'btn-plus': {l:56, t:5, w:6},
            'btn-capture': {l:42, t:18, w:5}, 'btn-home': {l:53, t:18, w:5},
            'lstick': {l:6, t:35, w:16}, 'btn-ls': {l:2, t:65, w:5},
            'dpad': {l:22, t:60, w:16},
            'abxy': {l:78, t:35, w:16},
            'rstick': {l:62, t:60, w:16}, 'btn-rs': {l:85, t:80, w:5}
        },
        // Shared Joy-Con layout template: L and R look almost the same.
        [JOYCON_L]: {
            'btn-zl': {l: 2.56, t: 49.63, w: 10, h: 22}, 'btn-l': {l: 14.34, t: 54.16, w: 8, h: 15},
            'btn-sl': {l: 34, t: 9, w: 14, h: 8}, 'btn-sr': {l: 53, t: 9, w: 14, h: 8},
            'btn-minus': {l: 18.35, t: 26.33, w: 8}, 'btn-capture': {l: 82.88, t: 26.29, w: 8},
            'lstick': {l: 28.44, t: 35.80, w: 24}, 'btn-ls': {l: 83.26, t: 73.78, w: 8},
            'dpad': {l: 58.28, t: 32.98, w: 24}
        },
        [JOYCON_R]: {
            'btn-zr': {l: 2.56, t: 49.63, w: 10, h: 22}, 'btn-r': {l: 14.34, t: 54.16, w: 8, h: 15},
            'btn-sl': {l: 34, t: 9, w: 14, h: 8}, 'btn-sr': {l: 53, t: 9, w: 14, h: 8},
            'btn-plus': {l: 8.35, t: 26.33, w: 8}, 'btn-home': {l: 82.88, t: 26.29, w: 8},
            'rstick': {l: 58.28, t: 32.98, w: 24}, 'btn-rs': {l: 83.26, t: 73.78, w: 8},
            'abxy': {l: 28.44, t: 35.80, w: 24}
        }
    };

    const faceButtonSlots = [
        {selector: '#abxy .up', standardLabel: 'X', standardBit: 8, joyconRLabel: 'A', joyconRBit: 4},
        {selector: '#abxy .left', standardLabel: 'Y', standardBit: 1, joyconRLabel: 'X', joyconRBit: 8},
        {selector: '#abxy .right', standardLabel: 'A', standardBit: 4, joyconRLabel: 'B', joyconRBit: 2},
        {selector: '#abxy .down', standardLabel: 'B', standardBit: 2, joyconRLabel: 'Y', joyconRBit: 1}
    ];

    // Joy-Con R uses the rotated on-screen order requested by the user:
    // A on top, B on the right, Y on the bottom and X on the left.
    // The emitted bits follow the same rotated mapping:
    // physical X->A, physical A->B, physical B->Y, physical Y->X.
    function applyFaceButtonMapping(root, type) {
        if (!root) return;
        const joyconR = type === JOYCON_R;
        for (const slot of faceButtonSlots) {
            const button = root.querySelector(slot.selector);
            if (!button) continue;
            const label = button.querySelector('.label');
            if (label) label.textContent = joyconR ? slot.joyconRLabel : slot.standardLabel;
            if (button.hasAttribute('data-btn')) {
                button.dataset.btn = String(joyconR ? slot.joyconRBit : slot.standardBit);
            }
        }
    }

    const allIds = Object.keys(layouts[PRO]);
    const clone = value => JSON.parse(JSON.stringify(value));
    const storageKey = type => `nswc_layout_${type}`;

    function isPlainObject(value) {
        return !!value && typeof value === 'object' && !Array.isArray(value);
    }

    function sameValue(a, b) {
        if (a === b) return true;
        if (typeof a !== typeof b) return false;
        if (Array.isArray(a) || Array.isArray(b)) return false;
        if (!isPlainObject(a) || !isPlainObject(b)) return false;
        const aKeys = Object.keys(a).sort();
        const bKeys = Object.keys(b).sort();
        if (aKeys.length !== bKeys.length) return false;
        for (let i = 0; i < aKeys.length; i++) if (aKeys[i] !== bKeys[i]) return false;
        for (const key of aKeys) {
            const av = a[key], bv = b[key];
            if (isPlainObject(av) || isPlainObject(bv)) {
                if (!sameValue(av, bv)) return false;
            } else if (av !== bv) {
                return false;
            }
        }
        return true;
    }

    function maybeMigrate(type, parsed) {
        if (type === JOYCON_L || type === JOYCON_R) {
            const legacy = legacyDefaults[type];
            if (legacy && sameValue(parsed, legacy)) return clone(layouts[type]);
        }
        return parsed;
    }

    function load(type) {
        let raw = localStorage.getItem(storageKey(type));
        if (!raw && type === PRO) raw = localStorage.getItem('nswc_layout');
        if (raw) {
            try {
                const parsed = JSON.parse(raw);
                if (parsed && typeof parsed === 'object') return maybeMigrate(type, parsed);
            } catch (_) {}
        }
        return clone(layouts[type] || layouts[PRO]);
    }

    function applySkin(element, type) {
        if (!element) return;
        element.classList.remove('controller-pro', 'controller-joycon-l', 'controller-joycon-r');
        element.classList.add(type === JOYCON_L ? 'controller-joycon-l'
            : type === JOYCON_R ? 'controller-joycon-r' : 'controller-pro');
    }

    window.NSControllerLayouts = layouts;
    window.NSControllerControlIds = allIds;
    window.nsCloneControllerLayout = clone;
    window.nsControllerLayoutStorageKey = storageKey;
    window.nsLoadControllerLayout = load;
    window.nsApplyControllerSkin = applySkin;
    window.nsApplyControllerFaceButtons = applyFaceButtonMapping;
})();
