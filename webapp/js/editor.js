const nativeMobileHost = !!(window.NSBridge && typeof NSBridge.onTouchState === 'function');
const isMobileClient = /Mobi|Android|iPhone|iPad/i.test(navigator.userAgent) || ('ontouchstart' in window && navigator.maxTouchPoints > 0);
let controllerType = 3;
if (nativeMobileHost || isMobileClient) {
    controllerType = parseInt(localStorage.getItem('nswc_controller_type') || '3');
    if (![1,2,3].includes(controllerType)) controllerType = 3;
} else {
    document.querySelector('label[for="controllerType"]')?.remove();
    document.getElementById('controllerType')?.remove();
}
const layouts = {
    1: nsLoadControllerLayout(1),
    2: nsLoadControllerLayout(2),
    3: nsLoadControllerLayout(3)
};
let defLayout = NSControllerLayouts[controllerType];
let layout = layouts[controllerType];
const joyconLeftOnly = new Set(['btn-zl','btn-l','btn-minus','btn-capture','lstick','btn-ls','dpad','btn-sl','btn-sr']);
const joyconRightOnly = new Set(['btn-zr','btn-r','btn-plus','btn-home','rstick','btn-rs','abxy','btn-sl','btn-sr']);
function allowedForController(id) {
    if (controllerType === 3) return id !== 'btn-sl' && id !== 'btn-sr';
    return controllerType === 1 ? joyconLeftOnly.has(id) : joyconRightOnly.has(id);
}
function applyLayout() {
    const gamepad = document.getElementById('gamepad');
    nsApplyControllerSkin(gamepad, controllerType);
    nsApplyControllerFaceButtons(gamepad, controllerType);
    for(let id of NSControllerControlIds) {
        let el = document.getElementById(id);
        if(!el) continue;
        let conf = layout[id] || defLayout[id] || NSControllerLayouts[3][id];
        if(conf.hide || !allowedForController(id)) { el.style.display = 'none'; }
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
if (nativeMobileHost) document.getElementById('controllerType').value = String(controllerType);
function changeControllerType() {
    if (!nativeMobileHost && !isMobileClient) return;
    layouts[controllerType] = layout;
    controllerType = parseInt(document.getElementById('controllerType').value);
    defLayout = NSControllerLayouts[controllerType];
    layout = layouts[controllerType];
    applyLayout(); populateAdd(); checkOverlaps();
}
function populateAdd() {
    let sel = document.getElementById('addSel'); sel.innerHTML = '<option value="">+ Add Button</option>';
    for(let id of NSControllerControlIds) {
        let conf = layout[id] || defLayout[id] || NSControllerLayouts[3][id];
        if(conf.hide && allowedForController(id)) { let opt = document.createElement('option'); opt.value = id; opt.innerText = id; sel.appendChild(opt); }
    }
}
populateAdd();
function addBtn() {
    let val = document.getElementById('addSel').value;
    if(!val) return;
    if(!layout[val]) layout[val] = {...(defLayout[val] || NSControllerLayouts[3][val])};
    layout[val].hide = false; layout[val].l = 45; layout[val].t = 15;
    applyLayout(); populateAdd(); checkOverlaps();
}
function checkOverlaps() {
    let els = Array.from(document.querySelectorAll('.edit-item')).filter(e => e.style.display !== 'none');
    els.forEach(e => e.classList.remove('overlap'));
    let hasOverlap = false;
    for(let i=0; i<els.length; i++) {
        for(let j=i+1; j<els.length; j++) {
            let b1 = els[i].getBoundingClientRect(), b2 = els[j].getBoundingClientRect();
            let s1x = b1.width * 0.1, s1y = b1.height * 0.1;
            let s2x = b2.width * 0.1, s2y = b2.height * 0.1;
            if (!(b1.right - s1x < b2.left + s2x || b1.left + s1x > b2.right - s2x || b1.bottom - s1y < b2.top + s2y || b1.top + s1y > b2.bottom - s2y)) {
                els[i].classList.add('overlap'); els[j].classList.add('overlap'); hasOverlap = true;
            }
        }
    }
    return hasOverlap;
}
function toggleMenu() {
    let eb = document.getElementById('editor-bar'); let tg = document.getElementById('menu-toggle');
    if(eb.style.display === 'none') { eb.style.display = 'flex'; tg.style.display = 'none'; }
    else { eb.style.display = 'none'; tg.style.display = 'flex'; }
}
let eb = document.getElementById('editor-bar');
let isDraggingMenu = false, menuDx, menuDy;
eb.addEventListener('touchstart', e => {
    if (e.target.closest('button, select, input')) return;
    isDraggingMenu = true; let rect = eb.getBoundingClientRect();
    menuDx = e.touches[0].clientX - (rect.left + rect.width/2);
    menuDy = e.touches[0].clientY - (rect.top + rect.height/2);
    e.stopPropagation();
}, {passive:false});
window.addEventListener('touchmove', e => {
    if(isDraggingMenu) {
        e.preventDefault();
        eb.style.left = (e.touches[0].clientX - menuDx) + 'px';
        eb.style.top = (e.touches[0].clientY - menuDy) + 'px';
    }
}, {passive:false});
window.addEventListener('touchend', e => { isDraggingMenu = false; });
let activeEl = null, mode = 'none', startX, startY, startL, startT, initialDist, startW, startH, deleteMode = false;
function toggleDel() {
    deleteMode = !deleteMode; const btn = document.getElementById('toggleDel');
    if(deleteMode) { btn.classList.add('active'); btn.innerText = "🗑️ Delete Mode: ON"; }
    else { btn.classList.remove('active'); btn.innerText = "🗑️ Delete Mode: OFF"; }
}
function getDist(t1, t2) { return Math.hypot(t1.clientX - t2.clientX, t1.clientY - t2.clientY); }
document.getElementById('gamepad').addEventListener('touchstart', e => {
    if(e.target.closest('#editor-bar') || e.target.closest('#menu-toggle')) return;
    let el = e.target.closest('.edit-item'); if(!el) return;
    e.preventDefault();
    if (deleteMode) {
        if(!layout[el.id]) layout[el.id] = {...(defLayout[el.id] || NSControllerLayouts[3][el.id])};
        layout[el.id].hide = true; applyLayout(); populateAdd(); checkOverlaps(); return;
    }
    activeEl = el; if(!layout[el.id]) layout[el.id] = {...defLayout[el.id]};
    if(e.touches.length === 1) {
        mode = 'drag'; startX = e.touches[0].clientX; startY = e.touches[0].clientY;
        startL = layout[el.id].l; startT = layout[el.id].t;
    } else if(e.touches.length === 2) {
        mode = 'pinch'; initialDist = getDist(e.touches[0], e.touches[1]);
        startW = layout[el.id].w; startH = layout[el.id].h;
    }
}, {passive:false});
document.getElementById('gamepad').addEventListener('touchmove', e => {
    if(!activeEl || deleteMode) return; e.preventDefault();
    let pw = window.innerWidth, ph = window.innerHeight, conf = layout[activeEl.id];
    if(mode === 'drag' && e.touches.length === 1) {
        let dx = e.touches[0].clientX - startX, dy = e.touches[0].clientY - startY;
        conf.l = startL + (dx / pw * 100); conf.t = startT + (dy / ph * 100);
    } else if(mode === 'pinch' && e.touches.length >= 2) {
        let scale = getDist(e.touches[0], e.touches[1]) / initialDist;
        conf.w = Math.max(2, startW * scale); if(startH) conf.h = Math.max(2, startH * scale);
    }
    applyLayout(); checkOverlaps();
}, {passive:false});
document.getElementById('gamepad').addEventListener('touchend', e => {
    if(e.touches.length === 0) { activeEl = null; mode = 'none'; }
    else if(e.touches.length === 1 && activeEl) {
        mode = 'drag'; startX = e.touches[0].clientX; startY = e.touches[0].clientY;
        startL = layout[activeEl.id].l; startT = layout[activeEl.id].t;
    }
});
setTimeout(checkOverlaps, 500);
function saveLayout() {
    if(checkOverlaps()) { alert('Fix overlapping buttons (red) before saving!'); return; }
    layouts[controllerType] = layout;
    for (const type of [1, 2, 3]) {
        localStorage.setItem(nsControllerLayoutStorageKey(type), JSON.stringify(layouts[type]));
    }
    // Keep the legacy key as the Pro layout for older app/web bundles.
    localStorage.setItem('nswc_layout', JSON.stringify(layouts[3]));
    if (nativeMobileHost) localStorage.setItem('nswc_controller_type', String(controllerType));
    window.location.href = 'mobile.html';
}
function resetLayout() {
    if(confirm('Are you sure you want to reset all buttons to their default positions and sizes?')) {
        layout = nsCloneControllerLayout(defLayout);
        layouts[controllerType] = layout;
        applyLayout();
        populateAdd();
        checkOverlaps();
    }
}
