(function () {
    const PRO = 3, JOYCON_L = 1, JOYCON_R = 2;

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
        // Upright Joy-Con (L): stick above the directional buttons, system
        // controls on the inner edge, and SL/SR following the right rail.
        [JOYCON_L]: {
            'btn-zl': {l:33, t:2, w:34, h:7}, 'btn-l': {l:35, t:10, w:30, h:7},
            'btn-sl': {l:64, t:34, w:5, h:16}, 'btn-sr': {l:64, t:54, w:5, h:16},
            'btn-minus': {l:57, t:20, w:5}, 'btn-capture': {l:59, t:82, w:5},
            'lstick': {l:40, t:21, w:15}, 'btn-ls': {l:35, t:43, w:5},
            'dpad': {l:42, t:57, w:16}
        },
        // Joy-Con (R) mirrors the body/rail and follows the real controller's
        // face-buttons-above-stick arrangement.
        [JOYCON_R]: {
            'btn-zr': {l:33, t:2, w:34, h:7}, 'btn-r': {l:35, t:10, w:30, h:7},
            'btn-sl': {l:31, t:34, w:5, h:16}, 'btn-sr': {l:31, t:54, w:5, h:16},
            'btn-plus': {l:37, t:20, w:4}, 'btn-home': {l:38, t:82, w:5},
            'abxy': {l:42, t:20, w:16},
            'rstick': {l:45, t:57, w:15}, 'btn-rs': {l:61, t:70, w:5}
        }
    };

    const allIds = Object.keys(layouts[PRO]);
    const clone = value => JSON.parse(JSON.stringify(value));
    const storageKey = type => `nswc_layout_${type}`;

    function load(type) {
        let raw = localStorage.getItem(storageKey(type));
        // Preserve the pre-profile custom layout as the user's Pro layout.
        if (!raw && type === PRO) raw = localStorage.getItem('nswc_layout');
        if (raw) {
            try {
                const parsed = JSON.parse(raw);
                if (parsed && typeof parsed === 'object') return parsed;
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
