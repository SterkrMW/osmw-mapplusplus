'use strict';

/* Character Vendor pricing grid.
 *
 * AHK owns the store: every edit is sent over and echoed back, so the grid
 * never holds a price the addon does not also have. Nothing here writes game
 * memory — Apply asks AHK, which guards, writes and verifies. */

const $ = (id) => document.getElementById(id);

function sendToAhk(type, payload = {}) {
    if (window.chrome && window.chrome.webview) {
        window.chrome.webview.postMessage(JSON.stringify({ type, ...payload }));
    }
}

const state = {
    slotCount: 24,
    cols: 6,
    maxPrice: 99999999,
    idsKnown: false,
    iconBase: 0,
    canWrite: false,
    dirty: false,
    slots: [],          // {i, itemId, price, live, band, empty, suspect}
    presets: [],
    selectedPreset: 0,
    pendingDiff: null
};

const sel = new Set();
let anchor = 0;
let built = false;

/* ── Message pump ────────────────────────────────────────────── */

function onAhkMessage(event) {
    let msg = event.data;
    if (typeof msg === 'string') {
        try { msg = JSON.parse(msg); } catch (e) { return; }
    }
    if (!msg || typeof msg !== 'object') return;

    switch (msg.type) {
        case 'shop-state':
            adoptShopState(msg);
            break;
        case 'grid-state':
            adoptGridState(msg);
            break;
        case 'slot-updated':
            adoptSlotUpdate(msg);
            break;
        case 'presets-state':
            state.presets = msg.presets || [];
            renderPresets();
            break;
        case 'preset-diff':
            showDiff(msg);
            break;
        case 'save-result':
            setStatus(msg.message || '', msg.ok ? 'success' : 'error');
            break;
        case 'guard-state':
            state.canWrite = !!msg.canWrite;
            renderGuard(msg.reason || '');
            updateApplyEnabled();
            break;
        case 'toast':
            setStatus(msg.text || '', msg.level === 'error' ? 'error'
                : (msg.level === 'warn' ? 'warn' : ''));
            break;
    }
}

if (window.chrome && window.chrome.webview) {
    window.chrome.webview.addEventListener('message', onAhkMessage);
}

/* ── State adoption ──────────────────────────────────────────── */

function adoptShopState(msg) {
    state.slotCount = msg.slotCount || 24;
    state.cols = msg.cols || 6;
    state.maxPrice = msg.maxPrice || 99999999;
    state.idsKnown = !!msg.idsKnown;
    state.iconBase = msg.iconBase || 0;

    const t = msg.target || {};
    state.canWrite = !!t.canWrite;
    $('titleText').textContent = t.charName
        ? `Character Vendor — ${t.charName}` : 'Character Vendor';
    renderGuard(t.reason || '');
    renderClients(msg.clients || [], t.pid || 0);
    document.documentElement.style.setProperty('--cols', state.cols);
    buildGrid();
    updateApplyEnabled();
}

function adoptGridState(msg) {
    state.slots = msg.slots || [];
    state.dirty = !!msg.dirty;
    // Inventory can change while the panel is open. Never leave a now-empty
    // slot selected after a fresh snapshot arrives.
    for (const i of [...sel]) {
        if (!slotHasItemFileId(i)) sel.delete(i);
    }
    buildGrid();
    renderGrid();
    renderCounts(msg.listed || 0, msg.liveListed);
    updateApplyEnabled();
}

/* A single slot, so typing in one field never triggers a whole-grid re-render
 * that would steal focus mid-edit. */
function adoptSlotUpdate(msg) {
    const s = state.slots.find((x) => x.i === msg.slot);
    if (s) {
        s.price = msg.price;
        s.band = msg.band;
        // The user has authored over whatever was read, so an out-of-range
        // read no longer applies — and must stop blocking Apply.
        s.suspect = false;
    }
    state.dirty = !!msg.dirty;
    renderSlot(msg.slot, document.activeElement);
    renderCounts(msg.listed || 0);
    updateApplyEnabled();
}

function renderCounts(listed, liveListed) {
    const dirtyCount = state.slots.filter((slot) => slot.price !== slot.live).length;
    const draft = $('draftCount');
    draft.hidden = dirtyCount === 0;
    draft.textContent = `${dirtyCount} change${dirtyCount === 1 ? '' : 's'}`;
    $('listedCount').textContent =
        `${listed} of ${state.slotCount} listed` +
        (liveListed !== undefined && liveListed !== listed ? ` (${liveListed} now)` : '');
}

/* ── Formatting ──────────────────────────────────────────────── */

function groupDigits(digits) {
    return digits.replace(/\B(?=(\d{3})+(?!\d))/g, ',');
}

function formatPrice(n) {
    if (!n) return '';
    return groupDigits(String(n));
}

function formatShort(n) {
    if (!n) return '';
    if (n >= 1000000) return (n / 1000000).toFixed(1) + 'm';
    if (n >= 1000) return Math.round(n / 1000) + 'k';
    return String(n);
}

/* Mirrors _ShopPrices_MagnitudeBand for instant feedback while typing; the
 * authoritative band arrives with slot-updated. */
function bandFor(n) {
    if (!n || n <= 0) return 0;
    if (n > state.maxPrice) return 6;
    if (n < 1000) return 1;
    if (n < 100000) return 2;
    if (n < 1000000) return 3;
    if (n < 10000000) return 4;
    return 5;
}

/* Item icons ship as <n>.<n+1>.png, so the filename carries the id twice and
 * the base decides which half is meant. Both readings resolve to a real file
 * for nearly every id, so this cannot be probed — it is a setting. */
function iconSrc(id) {
    const n = id - state.iconBase;
    if (n < 0) return null;
    return `../items/${String(n).padStart(3, '0')}.${n + 1}.png`;
}

function slotState(i) {
    return state.slots.find((slot) => slot.i === i) || null;
}

// A price is only meaningful when the inventory record has a confirmed file
// ID. Match AHK's authoritative empty-slot sentinel check here. File ID 0 is
// legitimate when IconBase is 0; the live empty record uses 0xFFFF.
function slotHasItemFileId(i) {
    const slot = slotState(i);
    if (!state.idsKnown || !slot) return false;
    const id = Number(slot.itemId);
    return Number.isInteger(id) && id !== -1 && id !== 0xFFFF;
}

/* ── Grid ────────────────────────────────────────────────────── */

function buildGrid() {
    if (built) return;
    const grid = $('slotGrid');
    grid.innerHTML = '';
    for (let i = 1; i <= state.slotCount; i++) {
        const cell = document.createElement('div');
        cell.className = 'slot band-0';
        cell.dataset.slot = String(i);
        cell.setAttribute('role', 'gridcell');
        cell.setAttribute('aria-selected', 'false');
        cell.tabIndex = -1;

        const top = document.createElement('div');
        top.className = 'slot-top';
        const idx = document.createElement('span');
        idx.className = 'slot-index';
        idx.textContent = String(i);
        const img = document.createElement('img');
        img.className = 'slot-icon';
        img.alt = '';
        img.loading = 'lazy';
        img.hidden = true;
        img.addEventListener('error', () => {
            // A missing file becomes a placeholder, never a broken-image glyph.
            img.hidden = true;
            const ph = img.parentElement.querySelector('.slot-ph');
            if (ph) ph.hidden = false;
        });
        const ph = document.createElement('span');
        ph.className = 'slot-ph';
        top.append(idx, img, ph);

        const input = document.createElement('input');
        input.type = 'text';
        input.className = 'input input-sm price-input';
        input.inputMode = 'numeric';
        input.autocomplete = 'off';
        input.setAttribute('aria-label', `Slot ${i} price`);
        input.dataset.slot = String(i);

        const hint = document.createElement('span');
        hint.className = 'slot-hint';
        const short = document.createElement('span');
        short.className = 'slot-short';
        const was = document.createElement('span');
        was.className = 'slot-was';
        hint.append(short, was);

        cell.append(top, input, hint);
        grid.appendChild(cell);
    }
    built = true;
}

/* Grid pushes now arrive unprompted once a second (prices changed in the game's
 * own shop window), so a full render must never disturb a field mid-edit. */
function renderGrid() {
    const focused = document.activeElement;
    for (let i = 1; i <= state.slotCount; i++) renderSlot(i, focused);
    renderSelection();
}

/* Swap only the band class — replacing className wholesale would drop the
 * dirty/empty/selected markers on every keystroke. */
function setBand(cell, band) {
    for (let b = 0; b <= 6; b++) cell.classList.remove('band-' + b);
    cell.classList.add('band-' + band);
}

function renderSlot(i, focused) {
    const cell = $('slotGrid').querySelector(`.slot[data-slot="${i}"]`);
    if (!cell) return;
    const s = state.slots.find((x) => x.i === i)
        || { i, itemId: 0, price: 0, live: 0, band: 0, empty: true, suspect: false };

    const input = cell.querySelector('.price-input');
    const typing = input === focused;
    // Never overwrite or restyle the field being typed into — livePreview owns
    // its value, colour and hint until the edit is committed.
    if (!typing) {
        input.value = formatPrice(s.price);
        input.classList.toggle('input-invalid', !!s.suspect);
        setBand(cell, s.band || 0);
    }

    const hasItem = slotHasItemFileId(i);
    const empty = state.idsKnown && s.empty;
    input.disabled = !hasItem;
    input.title = hasItem ? '' : (state.idsKnown
        ? 'Empty inventory slot — only slots containing an item can be priced.'
        : 'Item file IDs are unavailable — verify the slot mapping first.');
    cell.classList.toggle('slot-empty', empty);
    cell.classList.toggle('slot-dirty', s.price !== s.live);
    cell.classList.toggle('slot-selected', sel.has(i));

    const img = cell.querySelector('.slot-icon');
    const ph = cell.querySelector('.slot-ph');
    const src = hasItem ? iconSrc(s.itemId) : null;
    if (src) {
        if (img.getAttribute('src') !== src) {
            img.hidden = false;
            ph.hidden = true;
            img.src = src;
            img.title = `Item ${s.itemId}`;
        }
    } else {
        img.hidden = true;
        img.removeAttribute('src');
        ph.hidden = false;
        ph.textContent = state.idsKnown ? '' : '?';
    }

    cell.querySelector('.slot-short').textContent = formatShort(s.price);
    const was = cell.querySelector('.slot-was');
    was.textContent = (s.price !== s.live && s.live > 0) ? formatPrice(s.live) : '';
}

function renderSelection() {
    const grid = $('slotGrid');
    for (const i of [...sel]) {
        if (!slotHasItemFileId(i)) sel.delete(i);
    }
    for (let i = 1; i <= state.slotCount; i++) {
        const cell = grid.querySelector(`.slot[data-slot="${i}"]`);
        if (!cell) continue;
        const on = sel.has(i);
        cell.classList.toggle('slot-selected', on);
        cell.setAttribute('aria-selected', on ? 'true' : 'false');
    }
    const n = sel.size;
    $('bulkCount').textContent = n
        ? `${n} slot${n === 1 ? '' : 's'} selected` : 'Nothing selected';
    $('btnSetSelected').disabled = n === 0;
    $('btnClearSelected').disabled = n === 0;
}

/* ── Selection ───────────────────────────────────────────────── */

function selectOnly(i) {
    sel.clear();
    if (slotHasItemFileId(i)) {
        sel.add(i);
        anchor = i;
    }
    renderSelection();
}

function toggle(i) {
    if (!slotHasItemFileId(i)) return;
    if (sel.has(i)) sel.delete(i); else sel.add(i);
    anchor = i;
    renderSelection();
}

function selectRange(from, to, additive) {
    if (!additive) sel.clear();
    const lo = Math.min(from, to);
    const hi = Math.max(from, to);
    for (let i = lo; i <= hi; i++) {
        if (slotHasItemFileId(i)) sel.add(i);
    }
    renderSelection();
}

function selectAll() {
    sel.clear();
    for (let i = 1; i <= state.slotCount; i++) {
        if (slotHasItemFileId(i)) sel.add(i);
    }
    renderSelection();
}

function selectNone() {
    sel.clear();
    renderSelection();
}

/* Click, ctrl-click, shift-click and a drag marquee, all off one pointer
 * gesture: a press that never moves is a click. */
(function wireSelection() {
    const wrap = $('gridWrap');
    const box = $('marquee');
    let drag = null;

    wrap.addEventListener('pointerdown', (e) => {
        if (e.button !== 0) return;
        if (e.target.classList.contains('price-input')) return;  // let the field focus
        const wrapRect = wrap.getBoundingClientRect();
        drag = {
            id: e.pointerId,
            cx: e.clientX,
            cy: e.clientY,
            x0: e.clientX - wrapRect.left + wrap.scrollLeft,
            y0: e.clientY - wrapRect.top + wrap.scrollTop,
            additive: e.ctrlKey || e.metaKey,
            shift: e.shiftKey,
            cell: e.target.closest('.slot'),
            base: new Set(sel),
            moved: false
        };
        wrap.setPointerCapture(e.pointerId);
    });

    wrap.addEventListener('pointermove', (e) => {
        if (!drag) return;
        if (!drag.moved) {
            if (Math.abs(e.clientX - drag.cx) < 4 && Math.abs(e.clientY - drag.cy) < 4) return;
            drag.moved = true;
            box.hidden = false;
        }
        const wrapRect = wrap.getBoundingClientRect();
        const x1 = e.clientX - wrapRect.left + wrap.scrollLeft;
        const y1 = e.clientY - wrapRect.top + wrap.scrollTop;
        box.style.left = Math.min(drag.x0, x1) + 'px';
        box.style.top = Math.min(drag.y0, y1) + 'px';
        box.style.width = Math.abs(x1 - drag.x0) + 'px';
        box.style.height = Math.abs(y1 - drag.y0) + 'px';

        // Hit-test in viewport coords so no scroll maths is needed.
        const lo = { x: Math.min(drag.cx, e.clientX), y: Math.min(drag.cy, e.clientY) };
        const hi = { x: Math.max(drag.cx, e.clientX), y: Math.max(drag.cy, e.clientY) };
        sel.clear();
        if (drag.additive) drag.base.forEach((i) => sel.add(i));
        $('slotGrid').querySelectorAll('.slot').forEach((cell) => {
            const r = cell.getBoundingClientRect();
            if (r.right >= lo.x && r.left <= hi.x && r.bottom >= lo.y && r.top <= hi.y) {
                const i = Number(cell.dataset.slot);
                if (slotHasItemFileId(i)) sel.add(i);
            }
        });
        renderSelection();
    });

    function endDrag(e) {
        if (!drag) return;
        const d = drag;
        drag = null;
        box.hidden = true;
        try { wrap.releasePointerCapture(d.id); } catch (err) { /* already gone */ }

        if (d.moved) return;              // marquee already committed the set
        if (!d.cell) { selectNone(); return; }
        const i = Number(d.cell.dataset.slot);
        if (d.shift && anchor) selectRange(anchor, i, d.additive);
        else if (d.additive) toggle(i);
        else selectOnly(i);
    }

    wrap.addEventListener('pointerup', endDrag);
    wrap.addEventListener('pointercancel', endDrag);
})();

/* ── Price fields ────────────────────────────────────────────── */

/* Reformats to grouped digits and restores the caret by digit index, so the
 * user always sees 1,000,000 as they type — which is the entire point. */
function reformatCaret(el) {
    const before = el.value;
    const caret = el.selectionStart === null ? before.length : el.selectionStart;
    const digitsLeft = (before.slice(0, caret).match(/\d/g) || []).length;

    let digits = before.replace(/\D/g, '').replace(/^0+(?=\d)/, '');
    if (digits.length > 9) digits = digits.slice(0, 9);
    const formatted = digits ? groupDigits(digits) : '';
    el.value = formatted;

    let seen = 0;
    let pos = digitsLeft === 0 ? 0 : formatted.length;
    if (digitsLeft > 0) {
        for (let i = 0; i < formatted.length; i++) {
            if (formatted[i] >= '0' && formatted[i] <= '9') seen++;
            if (seen >= digitsLeft) { pos = i + 1; break; }
        }
    }
    try { el.setSelectionRange(pos, pos); } catch (e) { /* not focused */ }
    return digits ? parseInt(digits, 10) : 0;
}

function livePreview(el, cell) {
    const raw = el.value;
    if (/[a-z.]/i.test(raw)) return;          // shorthand — expanded on blur
    const n = reformatCaret(el);
    const over = n > state.maxPrice;
    el.classList.toggle('input-invalid', over);
    el.title = over ? `Maximum is ${groupDigits(String(state.maxPrice))}` : '';
    if (cell) {
        setBand(cell, bandFor(n));
        // Keep the short form in step too, so both readings of the number stay
        // live while typing rather than one lagging on the committed value.
        const short = cell.querySelector('.slot-short');
        if (short) short.textContent = formatShort(n);
    }
    updateApplyEnabled();
}

$('slotGrid').addEventListener('input', (e) => {
    if (!e.target.classList.contains('price-input')) return;
    livePreview(e.target, e.target.closest('.slot'));
});

$('slotGrid').addEventListener('focusin', (e) => {
    if (e.target.classList.contains('price-input')) e.target.select();
});

$('slotGrid').addEventListener('change', (e) => {
    if (!e.target.classList.contains('price-input')) return;
    commitSlot(e.target);
});

$('slotGrid').addEventListener('keydown', (e) => {
    if (!e.target.classList.contains('price-input')) return;
    if (e.key === 'Enter') {
        e.preventDefault();
        commitSlot(e.target);
        const next = Number(e.target.dataset.slot) + 1;
        const nextEl = $('slotGrid').querySelector(`.price-input[data-slot="${next}"]`);
        if (nextEl) nextEl.focus();
    } else if (e.key === 'Escape') {
        const i = Number(e.target.dataset.slot);
        const s = state.slots.find((x) => x.i === i);
        e.target.value = formatPrice(s ? s.price : 0);
        e.target.classList.remove('input-invalid');
        e.target.blur();
    }
});

function commitSlot(el) {
    const i = Number(el.dataset.slot);
    if (!slotHasItemFileId(i)) {
        renderSlot(i, null);
        setStatus('Only inventory slots containing an item can be priced.', 'warn');
        return;
    }
    sendToAhk('set-price', { slot: i, text: el.value });
}

$('bulkPrice').addEventListener('input', (e) => {
    if (/[a-z.]/i.test(e.target.value)) return;
    const n = reformatCaret(e.target);
    e.target.classList.toggle('input-invalid', n > state.maxPrice);
});

$('bulkPrice').addEventListener('keydown', (e) => {
    if (e.key === 'Enter') { e.preventDefault(); applyBulk(); }
});

function applyBulk() {
    if (!sel.size) return;
    sendToAhk('bulk-set', { slots: [...sel], text: $('bulkPrice').value });
}

document.querySelectorAll('.price-chip').forEach((button) => {
    button.addEventListener('click', () => {
        $('bulkPrice').value = formatPrice(Number(button.dataset.price));
        $('bulkPrice').classList.remove('input-invalid');
        $('bulkPrice').focus();
        $('bulkPrice').select();
    });
});

/* ── Header, guard, status ───────────────────────────────────── */

function renderClients(clients, activePid) {
    const s = $('clientSelect');
    s.innerHTML = '';
    clients.forEach((c) => {
        const o = document.createElement('option');
        o.value = String(c.pid);
        o.textContent = c.charName + (c.isActive ? '  (active)' : '');
        if (c.pid === activePid) o.selected = true;
        s.appendChild(o);
    });
    s.disabled = clients.length < 2;
}

function renderGuard(reason) {
    const b = $('guardBadge');
    if (state.canWrite) {
        b.className = 'badge badge-success';
        b.textContent = 'Ready';
        b.title = '';
    } else {
        b.className = 'badge badge-warning';
        b.textContent = 'Blocked';
        b.title = reason || '';
    }
    if (reason) setStatus(reason, 'warn');
}

function setStatus(text, cls = '') {
    const el = $('status');
    el.textContent = text;
    el.className = 'status' + (cls ? ' ' + cls : '');
}

function anyInvalid() {
    return !!$('slotGrid').querySelector('.price-input.input-invalid');
}

function updateApplyEnabled() {
    $('btnApply').disabled = !state.dirty || !state.canWrite || anyInvalid();
    $('btnRevert').disabled = !state.dirty;
}

/* ── Presets ─────────────────────────────────────────────────── */

function renderPresets() {
    const list = $('presetList');
    list.innerHTML = '';
    $('presetEmpty').hidden = state.presets.length > 0;
    state.presets.forEach((p) => {
        const li = document.createElement('li');
        li.className = 'preset-item' + (p.id === state.selectedPreset ? ' selected' : '');
        li.setAttribute('role', 'option');
        li.setAttribute('aria-selected', p.id === state.selectedPreset ? 'true' : 'false');
        li.dataset.id = String(p.id);
        const name = document.createElement('span');
        name.className = 'preset-name';
        name.textContent = p.name;
        name.title = p.name;
        const meta = document.createElement('span');
        meta.className = 'preset-meta';
        meta.textContent = `${p.slots} priced` + (p.savedBy ? ` · ${p.savedBy}` : '');
        li.append(name, meta);
        li.addEventListener('click', () => {
            state.selectedPreset = p.id;
            renderPresets();
        });
        li.addEventListener('dblclick', () => {
            state.selectedPreset = p.id;
            sendToAhk('preset-preview', { id: p.id });
        });
        list.appendChild(li);
    });
    const has = state.selectedPreset > 0;
    ['btnPresetLoad', 'btnPresetRename', 'btnPresetDelete']
        .forEach((id) => { $(id).disabled = !has; });
}

/* ── Diff modal ──────────────────────────────────────────────── */

const ACTION_BADGE = {
    change: ['badge-info', 'Change'],
    set: ['badge-info', 'Set'],
    clear: ['badge-danger', 'Clear'],
    skip: ['badge-muted', 'Skip']
};

function showDiff(msg) {
    state.pendingDiff = msg.id;
    $('diffTitle').textContent = msg.name || 'Preview';
    const body = $('diffBody');
    body.innerHTML = '';

    (msg.rows || []).forEach((r) => {
        const tr = document.createElement('tr');

        const tdSlot = document.createElement('td');
        tdSlot.textContent = r.slot;

        const tdItem = document.createElement('td');
        if (state.idsKnown && r.itemId) {
            const src = iconSrc(r.itemId);
            if (src) {
                const im = document.createElement('img');
                im.className = 'diff-icon';
                im.src = src;
                im.alt = '';
                im.addEventListener('error', () => im.remove());
                tdItem.appendChild(im);
            }
            tdItem.appendChild(document.createTextNode(String(r.itemId)));
        } else {
            tdItem.textContent = state.idsKnown ? '(empty)' : '?';
        }

        const tdFrom = document.createElement('td');
        tdFrom.textContent = r.from ? groupDigits(String(r.from)) : '—';
        const tdTo = document.createElement('td');
        tdTo.textContent = r.to ? groupDigits(String(r.to)) : '—';

        const tdAct = document.createElement('td');
        const [cls, label] = ACTION_BADGE[r.action] || ['badge-muted', r.action];
        const b = document.createElement('span');
        b.className = 'badge ' + (r.warn === 'different-item' ? 'badge-warning' : cls);
        b.textContent = r.warn === 'different-item' ? 'Different item' : label;
        if (r.warn === 'empty-slot') b.textContent = 'Empty slot';
        tdAct.appendChild(b);

        tr.append(tdSlot, tdItem, tdFrom, tdTo, tdAct);
        body.appendChild(tr);
    });

    const n = (msg.rows || []).length;
    const warns = msg.warnCount || 0;
    $('diffSummary').textContent =
        `${n} change${n === 1 ? '' : 's'}` +
        (warns ? `, ${warns} warning${warns === 1 ? '' : 's'}` : '') +
        '. This only fills in the grid — you still have to press Apply.';

    const ok = $('btnDiffOk');
    ok.className = 'btn ' + (warns ? 'btn-danger' : 'btn-primary');
    $('diffModal').hidden = false;
}

function closeDiff() {
    $('diffModal').hidden = true;
    state.pendingDiff = null;
}

/* ── Wiring ──────────────────────────────────────────────────── */

$('btnTitleClose').addEventListener('click', requestClose);
$('btnRefresh').addEventListener('click', () => sendToAhk('refresh', { force: false }));
$('btnSetSelected').addEventListener('click', applyBulk);
$('btnClearSelected').addEventListener('click',
    () => sel.size && sendToAhk('clear-slots', { slots: [...sel] }));
$('btnSelectAll').addEventListener('click', selectAll);
$('btnSelectNone').addEventListener('click', selectNone);
$('btnRevert').addEventListener('click', () => sendToAhk('revert'));
$('btnApply').addEventListener('click', () => sendToAhk('apply'));

$('clientSelect').addEventListener('change', (e) => {
    sendToAhk('select-client', { pid: Number(e.target.value) });
});

$('btnPresetSave').addEventListener('click', () => sendToAhk('preset-save-request'));
$('btnPresetLoad').addEventListener('click', () => {
    if (state.selectedPreset) sendToAhk('preset-preview', { id: state.selectedPreset });
});
$('btnPresetRename').addEventListener('click', () => {
    if (state.selectedPreset) sendToAhk('preset-rename', { id: state.selectedPreset });
});
$('btnPresetDelete').addEventListener('click', () => {
    if (state.selectedPreset) sendToAhk('preset-delete', { id: state.selectedPreset });
});

$('btnDiffCancel').addEventListener('click', closeDiff);
$('btnDiffCancelX').addEventListener('click', closeDiff);
$('btnDiffOk').addEventListener('click', () => {
    if (state.pendingDiff) sendToAhk('preset-apply', { id: state.pendingDiff });
    closeDiff();
});

function requestClose() {
    // AHK owns the confirm so the native panel behaves the same way.
    sendToAhk('close');
}

document.addEventListener('keydown', (e) => {
    if (!$('diffModal').hidden) {
        if (e.key === 'Escape') closeDiff();
        return;
    }
    const typing = /^(INPUT|SELECT|TEXTAREA)$/.test(document.activeElement?.tagName || '');
    const command = e.ctrlKey || e.metaKey;
    if (command && e.key === 'Enter' && !typing && !$('btnApply').disabled) {
        e.preventDefault();
        sendToAhk('apply');
    } else if (command && e.key.toLowerCase() === 'r' && !typing) {
        e.preventDefault();
        sendToAhk('refresh', { force: false });
    } else if (command && e.key.toLowerCase() === 'a' && !typing) {
        e.preventDefault();
        selectAll();
    } else if (e.key === 'Escape' && !typing) {
        if (sel.size) selectNone(); else requestClose();
    }
});

/* Belt and braces with AHK's DOMContentLoaded push — whichever lands first
 * populates the panel. */
window.addEventListener('DOMContentLoaded', () => {
    sendToAhk('init-request');
});
sendToAhk('init-request');

// Local visual-regression fixture. WebView2 never enters this branch.
const previewMode = new URLSearchParams(location.search).get('preview');
if (previewMode !== null && !window.chrome?.webview) {
    onAhkMessage({ data: {
        type: 'shop-state', slotCount: 24, cols: 6, maxPrice: 99999999,
        idsKnown: true, iconBase: 0,
        target: { pid: 1, charName: 'Ardent', canWrite: true },
        clients: [{ pid: 1, charName: 'Ardent', isActive: true }, { pid: 2, charName: 'Eir' }]
    } });
    const prices = [100000, 500000, 1000000, 2500000, 5000000, 10000000,
        750000, 1200000, 4500000, 9000000, 12000000, 25000000,
        300000, 600000, 1500000, 3500000, 7000000, 15000000,
        0, 0, 800000, 2000000, 0, 0];
    onAhkMessage({ data: {
        type: 'grid-state', dirty: true, listed: 20, liveListed: 18,
        slots: prices.map((price, index) => ({
            i: index + 1,
            itemId: index >= 22 ? 0xFFFF : index + 1,
            price,
            live: index === 2 ? 800000 : (index === 9 ? 8500000 : price),
            band: bandFor(price),
            empty: index >= 22,
            suspect: false
        }))
    } });
    onAhkMessage({ data: { type: 'presets-state', presets: [
        { id: 1, name: 'Fast restock', slots: 18, savedBy: 'Ardent' },
        { id: 2, name: 'Weekend market', slots: 20, savedBy: 'Eir' }
    ] } });
    // Regression fixture: Ctrl+A/Select All must omit slots 23 and 24 because
    // their item file IDs are the live client's 0xFFFF empty-slot sentinel.
    if (previewMode === 'select-all') selectAll();
}
