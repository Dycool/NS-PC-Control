(function () {
    const PRO = 3, JOYCON_L = 1, JOYCON_R = 2;

    // These were the original upright/portrait Joy-Con defaults. Keep them only
    // so installations that implicitly saved the old defaults can be migrated
    // to the corrected landscape layout without touching genuinely customised
    // layouts.
    const legacyJoyconLayouts = {
        [JOYCON_L]: {
            'btn-zl': {l:33, t:2, w:34, h:7}, 'btn-l': {l:35, t:10, w:30, h:7},
            'btn-sl': {l:64, t:34, w:5, h:16}, 'btn-sr': {l:64, t:54, w:5, h:16},
            'btn-minus': {l:57, t:20, w:5}, 'btn-capture': {l:59, t:82, w:5},
            'lstick': {l:40, t:21, w:15}, 'btn-ls': {l:35, t:43, w:5},
            'dpad': {l:42, t:57, w:16}
        },
        [JOYCON_R]: {
            'btn-zr': {l:33, t:2, w:34, h:7}, 'btn-r': {l:35, t:10, w:30, h:7},
            'btn-sl': {l:31, t:34, w:5, h:16}, 'btn-sr': {l:31, t:54, w:5, h:16},
            'btn-plus': {l:37, t:20, w:4}, 'btn-home': {l:38, t:82, w:5},
            'abxy': {l:42, t:20, w:16},
            'rstick': {l:45, t:57, w:15}, 'btn-rs': {l:61, t:70, w:5}
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
        // A single Joy-Con is used sideways. The touch surface/editor is always
        // landscape, so spread the controls across the available width instead
        // of drawing an upright controller in the middle of the screen.
        [JOYCON_L]: {
            'btn-zl': {l:1.5, t:25, w:7, h:20}, 'btn-l': {l:1.5, t:55, w:7, h:20},
            'btn-sl': {l:35, t:10, w:12, h:8}, 'btn-sr': {l:53, t:10, w:12, h:8},
            'btn-minus': {l:51, t:29, w:4}, 'btn-capture': {l:51, t:68, w:4},
            'lstick': {l:16, t:34, w:15}, 'btn-ls': {l:34, t:67, w:4},
            'dpad': {l:69, t:34, w:15}
        },
        // Joy-Con (R) is also sideways: stick on the left, face buttons on the
        // right, rail buttons along the top, and R/ZR on the outer edge.
        [JOYCON_R]: {
            'btn-zr': {l:91.5, t:25, w:7, h:20}, 'btn-r': {l:91.5, t:55, w:7, h:20},
            'btn-sl': {l:35, t:10, w:12, h:8}, 'btn-sr': {l:53, t:10, w:12, h:8},
            'btn-plus': {l:45, t:29, w:4}, 'btn-home': {l:45, t:68, w:4},
            'abxy': {l:69, t:34, w:15},
            'rstick': {l:16, t:34, w:15}, 'btn-rs': {l:34, t:67, w:4}
        }
    };

    const allIds = Object.keys(layouts[PRO]);
    const clone = value => JSON.parse(JSON.stringify(value));
    const storageKey = type => `nswc_layout_${type}`;

    function sameDefaultLayout(value, expected) {
        if (!value || typeof value !== 'object') return false;
        const ids = Object.keys(expected);
        if (Object.keys(value).length !== ids.length) return false;
        return ids.every(id => {
            const a = value[id], b = expected[id];
            if (!a || typeof a !== 'object') return false;
            return ['l', 't', 'w', 'h', 'hide'].every(key => {
                const av = a[key], bv = b[key];
                return av === bv || (av == null && bv == null);
            });
        });
    }

    function load(type) {
        let raw = localStorage.getItem(storageKey(type));
        // Preserve the pre-profile custom layout as the user's Pro layout.
        if (!raw && type === PRO) raw = localStorage.getItem('nswc_layout');
        if (raw) {
            try {
                const parsed = JSON.parse(raw);
                if (parsed && typeof parsed === 'object') {
                    // saveLayout() historically wrote all three profiles, so an
                    // untouched Joy-Con profile may still be persisted with the
                    // broken portrait default. Upgrade only exact old defaults;
                    // never overwrite a user-customised layout.
                    if ((type === JOYCON_L || type === JOYCON_R)
                        && sameDefaultLayout(parsed, legacyJoyconLayouts[type])) {
                        const migrated = clone(layouts[type]);
                        localStorage.setItem(storageKey(type), JSON.stringify(migrated));
                        return migrated;
                    }
                    return parsed;
                }
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
})();
