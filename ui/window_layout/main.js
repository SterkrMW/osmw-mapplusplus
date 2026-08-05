/* Window Layouts editor frontend.

   AHK owns the store: every mutation is a message, and the page never assumes a
   change stuck until a fresh `layout-detail` comes back. Slot positions travel
   as fractions of the target monitor's work area; the stage renders them at
   whatever scale fits, so nothing here deals in screen pixels except the two
   number inputs, which convert on the way in and out. */

'use strict';

const GUIDE_SNAP = 6;      // stage px — how close an edge has to be to snap
const NUDGE = 1;           // real px per arrow key
const NUDGE_FAST = 10;     // ...with Shift

let state = { layouts: [], characters: [], monitors: [], presets: [], defaultLayoutName: '', clientCount: 0 };
let detail = null;         // the layout being edited, or null
let slotEls = [];          // parallel to detail.slots
let selectedSlot = -1;
let scale = 1;             // stage px per real px
let dirty = false;
let armedDiscard = false;  // a discard is armed, waiting for a confirming click
let modalMode = 'new';

const $ = (id) => document.getElementById(id);

function sendToAhk(type, payload = {}) {
    if (window.chrome && window.chrome.webview) {
        window.chrome.webview.postMessage(JSON.stringify({ type, ...payload }));
    }
}

/* ── Messages in ─────────────────────────────────────────────── */

function onAhkMessage(event) {
    let msg = event.data;
    if (typeof msg === 'string') {
        try { msg = JSON.parse(msg); } catch (e) { return; }
    }
    if (!msg || typeof msg !== 'object') return;

    switch (msg.type) {
        case 'layouts-state':
            state = Object.assign(state, msg);
            renderLayoutList();
            renderMonitorSelect();
            renderCharSelect();
            updateControls();
            break;
        case 'layout-detail':
            adoptDetail(msg);
            break;
        case 'capture-result':
            setStatus(msg.message || '', msg.ok ? 'success' : 'error');
            break;
        case 'apply-result':
            setStatus(msg.message || '', msg.ok ? 'success' : 'warn');
            break;
        case 'toast':
            setStatus(msg.text || '', msg.level === 'error' ? 'error' : 'info');
            break;
    }
}

if (window.chrome && window.chrome.webview) {
    window.chrome.webview.addEventListener('message', onAhkMessage);
}

function adoptDetail(msg) {
    if (!msg.id) {
        detail = null;
        selectedSlot = -1;
        dirty = false;
        armedDiscard = false;
        layoutStage();
        renderSlotForm();
        renderLayoutList();
        updateControls();
        return;
    }
    detail = {
        id: msg.id,
        name: msg.name || '',
        monitorIndex: msg.monitorIndex || 0,
        fingerprintOk: msg.fingerprintOk !== false,
        workW: msg.workW || 1920,
        workH: msg.workH || 1080,
        winW: msg.winW || 1024,
        winH: msg.winH || 768,
        slots: (msg.slots || []).map(s => ({
            fx: Number(s.fx) || 0,
            fy: Number(s.fy) || 0,
            charIndex: Number(s.charIndex) || 0,
            focus: !!s.focus
        }))
    };
    selectedSlot = detail.slots.length ? 0 : -1;
    dirty = false;
    armedDiscard = null;
    renderMonitorSelect();
    layoutStage();
    renderSlotForm();
    renderLayoutList();
    updateControls();
    if (!detail.fingerprintOk) {
        setStatus('Authored on a different display setup — positions are rescaled.', 'warn');
    }
}

/* ── Left rail ───────────────────────────────────────────────── */

function renderLayoutList() {
    const list = $('layoutList');
    const empty = $('layoutEmpty');
    list.innerHTML = '';
    empty.hidden = state.layouts.length > 0;

    state.layouts.forEach(l => {
        const li = document.createElement('li');
        li.className = 'layout-item' + (detail && detail.id === l.id ? ' selected' : '')
            + (l.connected ? '' : ' offline');
        li.tabIndex = 0;
        li.setAttribute('role', 'option');
        li.setAttribute('aria-selected', detail && detail.id === l.id ? 'true' : 'false');

        const name = document.createElement('span');
        name.className = 'layout-name';
        name.textContent = l.name;
        name.title = l.name;
        li.appendChild(name);

        if (l.isDefault) {
            const dot = document.createElement('span');
            dot.className = 'default-dot';
            dot.textContent = '●';
            dot.title = 'Default layout';
            li.appendChild(dot);
        }

        const meta = document.createElement('span');
        meta.className = 'layout-meta';
        meta.textContent = `${l.slotCount} slot${l.slotCount === 1 ? '' : 's'} · ${l.monitorLabel}`;
        li.appendChild(meta);

        const pick = () => selectLayout(l.id);
        li.addEventListener('click', pick);
        li.addEventListener('dblclick', () => sendToAhk('apply-layout', { id: l.id, monitorIndex: 0 }));
        li.addEventListener('keydown', (e) => {
            if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); pick(); }
        });
        list.appendChild(li);
    });
}

function selectLayout(id) {
    if (detail && detail.id === id) return;
    if (!confirmDiscard()) return;
    sendToAhk('select-layout', { id });
}

/* ── Toolbar selects ─────────────────────────────────────────── */

function renderMonitorSelect() {
    const sel = $('monitorSelect');
    const want = detail ? detail.monitorIndex : 0;
    sel.innerHTML = '';
    state.monitors.forEach(m => {
        const opt = document.createElement('option');
        opt.value = String(m.index);
        opt.textContent = m.label;
        sel.appendChild(opt);
    });
    sel.value = String(want);
    sel.disabled = !detail;
}

function renderCharSelect() {
    const sel = $('slotChar');
    const want = sel.value;
    sel.innerHTML = '';
    const any = document.createElement('option');
    any.value = '0';
    any.textContent = 'Any client';
    sel.appendChild(any);
    state.characters.forEach((c, i) => {
        const opt = document.createElement('option');
        opt.value = String(i + 1);
        opt.textContent = c.running ? c.name : `${c.name} (offline)`;
        sel.appendChild(opt);
    });
    if (want) sel.value = want;
}

/* ── Stage ───────────────────────────────────────────────────── */

// Fits the work area's aspect ratio into whatever the frame gives us, then
// re-renders so every box lands at the new scale.
function layoutStage() {
    const frame = $('stageFrame');
    const stage = $('stage');
    if (!detail) {
        $('stageContext').hidden = true;
        stage.style.width = '0px';
        stage.style.height = '0px';
        slotEls = [];
        stage.innerHTML = '';
        $('stageHint').textContent = state.layouts.length
            ? 'Select a layout on the left, or capture your current window positions.'
            : 'Capture your current window positions, or create a layout from a preset.';
        return;
    }

    $('stageContext').hidden = false;
    $('activeLayoutName').textContent = detail.name;
    $('activeLayoutName').title = detail.name;
    $('stageMetrics').textContent = `${detail.workW} × ${detail.workH} display · ${detail.winW} × ${detail.winH} clients`;

    const availW = Math.max(80, frame.clientWidth);
    const availH = Math.max(60, frame.clientHeight);
    const ar = detail.workW / detail.workH;
    let w = availW;
    let h = w / ar;
    if (h > availH) { h = availH; w = h * ar; }

    stage.style.width = Math.round(w) + 'px';
    stage.style.height = Math.round(h) + 'px';
    scale = w / detail.workW;

    const chars = detail.slots.filter(s => s.charIndex > 0).length;
    $('stageHint').textContent =
        `${detail.slots.length} slot${detail.slots.length === 1 ? '' : 's'}, `
        + `${chars} bound to a character. Drag to move · arrows nudge 1 px · Shift nudges 10 px.`;

    renderSlots();
}

function stageSize() {
    const stage = $('stage');
    return { w: stage.clientWidth, h: stage.clientHeight };
}

function boxSize() {
    return {
        w: Math.max(14, Math.round(detail.winW * scale)),
        h: Math.max(12, Math.round(detail.winH * scale))
    };
}

function renderSlots() {
    const stage = $('stage');
    stage.innerHTML = '';
    slotEls = [];
    if (!detail) return;

    const { w: sw, h: sh } = stageSize();
    const box = boxSize();

    detail.slots.forEach((slot, i) => {
        const el = document.createElement('div');
        el.className = 'slot';
        el.tabIndex = 0;
        el.style.width = box.w + 'px';
        el.style.height = box.h + 'px';
        el.style.left = Math.round(slot.fx * sw) + 'px';
        el.style.top = Math.round(slot.fy * sh) + 'px';

        const idx = document.createElement('span');
        idx.className = 'slot-index';
        idx.textContent = String(i + 1);
        el.appendChild(idx);

        const character = state.characters[slot.charIndex - 1];
        if (character) {
            const nm = document.createElement('span');
            nm.className = 'slot-name';
            nm.textContent = character.name;
            el.appendChild(nm);
            if (!character.running) el.classList.add('offline');
        } else {
            el.classList.add('unbound');
            const nm = document.createElement('span');
            nm.className = 'slot-name';
            nm.textContent = 'Any client';
            el.appendChild(nm);
        }
        if (slot.focus) {
            const flag = document.createElement('span');
            flag.className = 'slot-flag';
            flag.textContent = '★ focus';
            el.appendChild(flag);
        }

        el.setAttribute('aria-label',
            `Slot ${i + 1}, ${character ? character.name : 'any client'}`);
        if (i === selectedSlot) el.classList.add('selected');

        el.addEventListener('pointerdown', (e) => onSlotPointerDown(e, i));
        el.addEventListener('keydown', (e) => onSlotKeyDown(e, i));
        el.addEventListener('focus', () => selectSlot(i));

        stage.appendChild(el);
        slotEls.push(el);
    });
}

function selectSlot(i) {
    if (selectedSlot === i) return;
    selectedSlot = i;
    slotEls.forEach((el, n) => el.classList.toggle('selected', n === i));
    renderSlotForm();
}

/* ── Dragging ────────────────────────────────────────────────── */

let drag = null;

function onSlotPointerDown(e, i) {
    if (!detail || e.button !== 0) return;
    const el = slotEls[i];
    selectSlot(i);
    el.focus();
    el.setPointerCapture(e.pointerId);
    el.classList.add('dragging');
    drag = {
        i,
        pointerId: e.pointerId,
        startX: e.clientX,
        startY: e.clientY,
        originLeft: parseFloat(el.style.left) || 0,
        originTop: parseFloat(el.style.top) || 0
    };
    e.preventDefault();

    el.addEventListener('pointermove', onSlotPointerMove);
    el.addEventListener('pointerup', onSlotPointerUp);
    el.addEventListener('pointercancel', onSlotPointerUp);
}

function onSlotPointerMove(e) {
    if (!drag || e.pointerId !== drag.pointerId) return;
    const el = slotEls[drag.i];
    const { w: sw, h: sh } = stageSize();
    const box = boxSize();

    let left = drag.originLeft + (e.clientX - drag.startX);
    let top = drag.originTop + (e.clientY - drag.startY);

    const snapPx = Number($('snapSelect').value) || 0;
    if (snapPx > 0) {
        // Quantise in real pixels, so "16 px" means 16 game pixels regardless
        // of how big the stage happens to be drawn.
        left = Math.round(left / scale / snapPx) * snapPx * scale;
        top = Math.round(top / scale / snapPx) * snapPx * scale;
        clearGuides();
    } else {
        const snapped = applyGuides(drag.i, left, top, box, sw, sh);
        left = snapped.left;
        top = snapped.top;
    }

    left = clamp(left, 0, Math.max(0, sw - box.w));
    top = clamp(top, 0, Math.max(0, sh - box.h));
    el.style.left = Math.round(left) + 'px';
    el.style.top = Math.round(top) + 'px';
}

function onSlotPointerUp(e) {
    if (!drag) return;
    const el = slotEls[drag.i];
    el.classList.remove('dragging');
    try { el.releasePointerCapture(drag.pointerId); } catch (err) { /* already gone */ }
    el.removeEventListener('pointermove', onSlotPointerMove);
    el.removeEventListener('pointerup', onSlotPointerUp);
    el.removeEventListener('pointercancel', onSlotPointerUp);
    clearGuides();
    commitFromElement(drag.i);
    drag = null;
}

// Compares the dragged box's edges and centre against every other box and the
// stage itself, drawing a line and pulling the box into line when close.
function applyGuides(i, left, top, box, sw, sh) {
    clearGuides();
    const vTargets = [0, sw / 2, sw];
    const hTargets = [0, sh / 2, sh];
    slotEls.forEach((other, n) => {
        if (n === i) return;
        const ol = parseFloat(other.style.left) || 0;
        const ot = parseFloat(other.style.top) || 0;
        vTargets.push(ol, ol + box.w / 2, ol + box.w);
        hTargets.push(ot, ot + box.h / 2, ot + box.h);
    });

    const v = nearestEdge(left, box.w, vTargets);
    const h = nearestEdge(top, box.h, hTargets);
    if (v) { left = v.value; drawGuide('v', v.at); }
    if (h) { top = h.value; drawGuide('h', h.at); }
    return { left, top };
}

// Best snap for one axis: tries the box's near edge, centre and far edge.
function nearestEdge(pos, size, targets) {
    let best = null;
    const edges = [
        { offset: 0, at: pos },
        { offset: size / 2, at: pos + size / 2 },
        { offset: size, at: pos + size }
    ];
    edges.forEach(edge => {
        targets.forEach(t => {
            const d = Math.abs(edge.at - t);
            if (d <= GUIDE_SNAP && (!best || d < best.dist)) {
                best = { dist: d, value: t - edge.offset, at: t };
            }
        });
    });
    return best;
}

function drawGuide(axis, at) {
    const g = document.createElement('div');
    g.className = `guide guide-${axis}`;
    if (axis === 'v') g.style.left = Math.round(at) + 'px';
    else g.style.top = Math.round(at) + 'px';
    $('stage').appendChild(g);
}

function clearGuides() {
    $('stage').querySelectorAll('.guide').forEach(g => g.remove());
}

function commitFromElement(i) {
    const el = slotEls[i];
    const { w: sw, h: sh } = stageSize();
    if (!sw || !sh) return;
    detail.slots[i].fx = (parseFloat(el.style.left) || 0) / sw;
    detail.slots[i].fy = (parseFloat(el.style.top) || 0) / sh;
    markDirty();
    renderSlotForm();
}

/* ── Keyboard ────────────────────────────────────────────────── */

function onSlotKeyDown(e, i) {
    if (!detail) return;
    const step = (e.shiftKey ? NUDGE_FAST : NUDGE);
    let dx = 0, dy = 0;
    switch (e.key) {
        case 'ArrowLeft':  dx = -step; break;
        case 'ArrowRight': dx = step; break;
        case 'ArrowUp':    dy = -step; break;
        case 'ArrowDown':  dy = step; break;
        case 'Delete':
        case 'Backspace':
            e.preventDefault();
            removeSlot(i);
            return;
        default:
            return;
    }
    e.preventDefault();
    nudgeSlot(i, dx, dy);
}

function nudgeSlot(i, dxReal, dyReal) {
    const slot = detail.slots[i];
    const x = clamp(Math.round(slot.fx * detail.workW) + dxReal, 0, Math.max(0, detail.workW - detail.winW));
    const y = clamp(Math.round(slot.fy * detail.workH) + dyReal, 0, Math.max(0, detail.workH - detail.winH));
    slot.fx = x / detail.workW;
    slot.fy = y / detail.workH;
    positionSlotEl(i);
    markDirty();
    renderSlotForm();
}

function positionSlotEl(i) {
    const el = slotEls[i];
    if (!el) return;
    const { w: sw, h: sh } = stageSize();
    el.style.left = Math.round(detail.slots[i].fx * sw) + 'px';
    el.style.top = Math.round(detail.slots[i].fy * sh) + 'px';
}

/* ── Right rail ──────────────────────────────────────────────── */

function renderSlotForm() {
    const has = !!detail && selectedSlot >= 0 && selectedSlot < detail.slots.length;
    $('slotEmpty').hidden = has;
    $('slotForm').hidden = !has;
    $('slotTitle').textContent = has ? `Slot ${selectedSlot + 1}` : 'Slot';
    document.querySelectorAll('.align-btn').forEach(button => { button.disabled = !has; });
    $('btnRemoveSlot').disabled = !has || detail.slots.length <= 1;
    if (!has) return;

    const slot = detail.slots[selectedSlot];
    $('slotX').value = Math.round(slot.fx * detail.workW);
    $('slotY').value = Math.round(slot.fy * detail.workH);
    $('slotChar').value = String(slot.charIndex || 0);
    $('slotFocus').checked = !!slot.focus;
    $('slotSizeNote').textContent =
        `Boxes are drawn at ${detail.winW}×${detail.winH} — the client size when this layout was captured. `
        + 'Applying a layout never resizes a window.';
}

function alignSelected(mode) {
    if (!detail || selectedSlot < 0) return;
    const slot = detail.slots[selectedSlot];
    const maxX = Math.max(0, detail.workW - detail.winW);
    const maxY = Math.max(0, detail.workH - detail.winH);
    if (mode === 'left') slot.fx = 0;
    if (mode === 'hcenter') slot.fx = (maxX / 2) / detail.workW;
    if (mode === 'right') slot.fx = maxX / detail.workW;
    if (mode === 'top') slot.fy = 0;
    if (mode === 'vcenter') slot.fy = (maxY / 2) / detail.workH;
    if (mode === 'bottom') slot.fy = maxY / detail.workH;
    positionSlotEl(selectedSlot);
    const el = slotEls[selectedSlot];
    if (el) {
        el.classList.remove('settled');
        void el.offsetWidth;
        el.classList.add('settled');
        el.addEventListener('animationend', () => el.classList.remove('settled'), { once: true });
    }
    markDirty();
    renderSlotForm();
}

function readSlotForm() {
    if (!detail || selectedSlot < 0) return;
    const slot = detail.slots[selectedSlot];
    const x = clamp(parseInt($('slotX').value, 10) || 0, 0, Math.max(0, detail.workW - detail.winW));
    const y = clamp(parseInt($('slotY').value, 10) || 0, 0, Math.max(0, detail.workH - detail.winH));
    slot.fx = x / detail.workW;
    slot.fy = y / detail.workH;
    slot.charIndex = Number($('slotChar').value) || 0;
    slot.focus = $('slotFocus').checked;
    // Only one slot can take focus — a second WinActivate would just undo it.
    if (slot.focus) detail.slots.forEach((s, i) => { if (i !== selectedSlot) s.focus = false; });
    markDirty();
    renderSlots();
    renderSlotForm();
}

function addSlot() {
    if (!detail) return;
    const n = detail.slots.length;
    const stepX = detail.winW * 0.25;
    const stepY = detail.winH * 0.25;
    detail.slots.push({
        fx: clamp((n * stepX) / detail.workW, 0, 0.9),
        fy: clamp((n * stepY) / detail.workH, 0, 0.9),
        charIndex: 0,
        focus: false
    });
    selectedSlot = detail.slots.length - 1;
    markDirty();
    layoutStage();
    renderSlotForm();
}

function removeSlot(i) {
    if (!detail || detail.slots.length <= 1) {
        setStatus('A layout needs at least one slot.', 'warn');
        return;
    }
    detail.slots.splice(i, 1);
    selectedSlot = Math.min(i, detail.slots.length - 1);
    markDirty();
    layoutStage();
    renderSlotForm();
}

/* ── Status & dirty state ────────────────────────────────────── */

function setStatus(text, cls = '') {
    const el = $('status');
    el.textContent = text;
    el.className = 'status' + (cls ? ' ' + cls : '') + (dirty ? ' dirty' : '');
}

function markDirty() {
    dirty = true;
    armedDiscard = null;
    setStatus($('status').textContent || 'Editing…');
    updateControls();
}

function updateControls() {
    const hasDetail = !!detail;
    ['btnDuplicate', 'btnRename', 'btnDelete', 'btnAddSlot', 'btnSetDefault']
        .forEach(id => { $(id).disabled = !hasDetail; });
    $('btnSave').disabled = !hasDetail || !dirty;
    // Apply works from the persisted layout. Keeping it unavailable while a
    // draft is open prevents a visually unchanged, stale layout from moving windows.
    $('btnApply').disabled = !hasDetail || dirty;
    if (!hasDetail) $('btnRemoveSlot').disabled = true;
}

// Two-click discard rather than a blocking confirm(): a modal dialog inside the
// WebView would stall the message pump AHK is posting state on. The first click
// arms, the second goes through — and any edit disarms it again.
function confirmDiscard() {
    if (!dirty) return true;
    if (armedDiscard) {
        dirty = false;
        armedDiscard = false;
        return true;
    }
    armedDiscard = true;
    setStatus('Unsaved changes — Save, or click again to discard.', 'warn');
    return false;
}

function clamp(v, lo, hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ── Actions ─────────────────────────────────────────────────── */

function saveLayout() {
    if (!detail) return;
    sendToAhk('save-layout', {
        id: detail.id,
        name: detail.name,
        monitorIndex: Number($('monitorSelect').value) || detail.monitorIndex,
        // toFixed keeps 1e-7 and -0 out of AHK's minimal number parser.
        slots: detail.slots.map(s => ({
            fx: Number(s.fx.toFixed(4)),
            fy: Number(s.fy.toFixed(4)),
            charIndex: s.charIndex | 0,
            focus: !!s.focus
        }))
    });
}

function openNameModal(mode) {
    modalMode = mode;
    $('nameModalTitle').textContent = mode === 'new' ? 'New layout' : 'Rename layout';
    $('btnNameOk').textContent = mode === 'new' ? 'Create' : 'Rename';
    $('seedField').hidden = mode !== 'new';
    $('nameInput').value = mode === 'rename' && detail ? detail.name : '';
    $('nameInput').classList.remove('input-invalid');

    const seed = $('seedSelect');
    seed.innerHTML = '';
    const blank = document.createElement('option');
    blank.value = '';
    blank.textContent = 'Empty (one slot)';
    seed.appendChild(blank);
    state.presets.forEach(p => {
        const opt = document.createElement('option');
        opt.value = p;
        opt.textContent = p;
        seed.appendChild(opt);
    });

    $('nameModal').hidden = false;
    setTimeout(() => $('nameInput').focus(), 30);
}

function closeNameModal() {
    $('nameModal').hidden = true;
}

function submitNameModal() {
    const input = $('nameInput');
    const name = input.value.trim();
    if (!name) {
        input.classList.remove('input-invalid');
        void input.offsetWidth;
        input.classList.add('input-invalid');
        input.focus();
        return;
    }
    if (modalMode === 'new') {
        sendToAhk('new-layout', {
            name,
            monitorIndex: Number($('monitorSelect').value) || 0,
            seedPreset: $('seedSelect').value
        });
    } else if (detail) {
        sendToAhk('rename-layout', { id: detail.id, name });
    }
    closeNameModal();
}

/* ── Wiring ──────────────────────────────────────────────────── */

$('btnNew').addEventListener('click', () => {
    if (!confirmDiscard()) return;
    openNameModal('new');
});
$('btnDuplicate').addEventListener('click', () => {
    if (!detail || !confirmDiscard()) return;
    sendToAhk('duplicate-layout', { id: detail.id });
});
$('btnRename').addEventListener('click', () => {
    if (!detail) return;
    openNameModal('rename');
});
$('btnDelete').addEventListener('click', () => {
    if (!detail) return;
    dirty = false;
    sendToAhk('delete-layout', { id: detail.id });
});

$('btnCapture').addEventListener('click', () => {
    if (!confirmDiscard()) return;
    sendToAhk('capture-current', {
        id: detail ? detail.id : 0,
        monitorIndex: Number($('monitorSelect').value) || 0
    });
});
$('btnAddSlot').addEventListener('click', addSlot);
$('btnRemoveSlot').addEventListener('click', () => {
    if (selectedSlot >= 0) removeSlot(selectedSlot);
});
document.querySelectorAll('.align-btn').forEach(button => {
    button.addEventListener('click', () => alignSelected(button.dataset.align));
});

$('monitorSelect').addEventListener('change', () => {
    if (!detail) return;
    const idx = Number($('monitorSelect').value);
    const mon = state.monitors.find(m => m.index === idx);
    if (!mon) return;
    detail.monitorIndex = idx;
    detail.workW = mon.workW;
    detail.workH = mon.workH;
    markDirty();
    layoutStage();
    renderSlotForm();
});
$('snapSelect').addEventListener('change', () => $('snapSelect').blur());

['slotX', 'slotY'].forEach(id => $(id).addEventListener('change', readSlotForm));
$('slotChar').addEventListener('change', readSlotForm);
$('slotFocus').addEventListener('change', readSlotForm);

$('btnApply').addEventListener('click', () => {
    if (!detail) return;
    sendToAhk('apply-layout', {
        id: detail.id,
        monitorIndex: Number($('monitorSelect').value) || 0
    });
});
$('btnSave').addEventListener('click', saveLayout);
$('btnSetDefault').addEventListener('click', () => {
    if (detail) sendToAhk('set-default', { id: detail.id });
});
$('btnUndo').addEventListener('click', () => sendToAhk('undo-apply'));

const closeEditor = () => { if (confirmDiscard()) sendToAhk('close'); };
$('btnClose').addEventListener('click', closeEditor);
$('btnTitleClose').addEventListener('click', closeEditor);

$('btnNameOk').addEventListener('click', submitNameModal);
$('btnNameCancel').addEventListener('click', closeNameModal);
$('btnNameCancelX').addEventListener('click', closeNameModal);
$('nameInput').addEventListener('input', (e) => e.target.classList.remove('input-invalid'));
$('nameInput').addEventListener('keydown', (e) => {
    if (e.key === 'Enter') { e.preventDefault(); submitNameModal(); }
});

document.addEventListener('keydown', (e) => {
    if (!$('nameModal').hidden) {
        if (e.key === 'Escape') closeNameModal();
        return;
    }
    const command = e.ctrlKey || e.metaKey;
    const typing = /^(INPUT|SELECT|TEXTAREA)$/.test(document.activeElement?.tagName || '');
    if (command && e.key === 'Enter' && !typing && !$('btnApply').disabled) {
        e.preventDefault();
        $('btnApply').click();
    } else if (command && e.key.toLowerCase() === 'n' && !typing) {
        e.preventDefault();
        $('btnNew').click();
    } else if (command && e.key.toLowerCase() === 'd' && !typing && detail) {
        e.preventDefault();
        $('btnDuplicate').click();
    } else if (e.key.toLowerCase() === 's' && command) {
        e.preventDefault();
        saveLayout();
    } else if (e.key === 'Escape') {
        closeEditor();
    }
});

window.addEventListener('resize', () => layoutStage());

window.addEventListener('DOMContentLoaded', () => {
    updateControls();
    sendToAhk('init-request');
});

// Local visual-regression fixture. WebView2 never enters this branch.
if (new URLSearchParams(location.search).has('preview') && !window.chrome?.webview) {
    onAhkMessage({ data: {
        type: 'layouts-state',
        layouts: [
            { id: 1, name: 'Four box — main left', slotCount: 4, monitorLabel: 'Display 1', connected: true, isDefault: true },
            { id: 2, name: 'Trading pair', slotCount: 2, monitorLabel: 'Display 2', connected: false, isDefault: false }
        ],
        characters: [
            { name: 'Ardent', running: true }, { name: 'Eir', running: true },
            { name: 'Sable', running: false }, { name: 'Morrow', running: true }
        ],
        monitors: [{ index: 1, label: 'Display 1 · 1920 × 1080', workW: 1920, workH: 1080 }],
        presets: ['Two columns', 'Four corners'], defaultLayoutName: 'Four box — main left', clientCount: 4
    } });
    onAhkMessage({ data: {
        type: 'layout-detail', id: 1, name: 'Four box — main left', monitorIndex: 1,
        fingerprintOk: true, workW: 1920, workH: 1080, winW: 800, winH: 480,
        slots: [
            { fx: 0, fy: 0, charIndex: 1, focus: true },
            { fx: 1120 / 1920, fy: 0, charIndex: 2, focus: false },
            { fx: 0, fy: 600 / 1080, charIndex: 3, focus: false },
            { fx: 1120 / 1920, fy: 600 / 1080, charIndex: 4, focus: false }
        ]
    } });
}
