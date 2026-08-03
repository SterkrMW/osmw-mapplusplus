/* ═══════════════════════════════════════════════════════════════
   osMW Maps++ — Settings  ·  Frontend logic
   ═══════════════════════════════════════════════════════════════
   Communication with AHK via window.chrome.webview.postMessage()
   and window.chrome.webview 'message' events.
   ═══════════════════════════════════════════════════════════════ */

'use strict';

// ── State ──────────────────────────────────────────────────────

/** Full settings state received from AHK on init. */
let state = null;

/** Pending hotkey chord edits: { actionId: chord } */
const pendingHotkeys = {};

/** Which hotkey action is currently being captured (null if none). */
let capturingActionId = null;

/** Addon save payloads collected from dynamic addon tabs. */
const addonSettingsValues = {};

// ── AHK ↔ JS Messaging ────────────────────────────────────────

/** Send a JSON message to AHK. */
function sendToAhk(type, payload = {}) {
    if (window.chrome && window.chrome.webview) {
        window.chrome.webview.postMessage(JSON.stringify({ type, ...payload }));
    }
}

/** Listen for messages from AHK. */
function onAhkMessage(event) {
    let msg = event.data;
    if (typeof msg === 'string') {
        try {
            msg = JSON.parse(msg);
        } catch (e) {
            console.error('Failed to parse message from AHK:', e);
            return;
        }
    }
    if (!msg || typeof msg !== 'object') return;
    const handler = messageHandlers[msg.type];
    if (handler) handler(msg);
}

const messageHandlers = {
    'settings-state':      handleSettingsState,
    'browse-result':       handleBrowseResult,
    'hotkey-captured':     handleHotkeyCaptured,
    'hotkey-capture-cancelled': handleHotkeyCaptureCancelled,
    'save-result':         handleSaveResult,
};

if (window.chrome && window.chrome.webview) {
    window.chrome.webview.addEventListener('message', onAhkMessage);
}

// Request initial state from AHK once the page loads.
window.addEventListener('DOMContentLoaded', () => {
    sendToAhk('init-request');
});

// ── State handler ──────────────────────────────────────────────

function handleSettingsState(msg) {
    state = msg;
    populateLauncher();
    populateMinimap();
    populateHotkeys();
    populateAddonTabs();
    populateAddons();
    buildTabBar();
    activateTab(state.tabNames[0] || 'Launcher');
}

// ── Tab switching ──────────────────────────────────────────────

const TAB_ICONS = {
    'Launcher': 'rocket_launch',
    'Minimap': 'map',
    'Hotkeys': 'keyboard',
    'Client Roster': 'groups',
    'Discord': 'forum',
    'Map POIs': 'place',
    'Party Markers': 'shield',
    'Window Layout': 'grid_view',
    'Addons': 'extension',
};

function buildTabBar() {
    const bar = document.getElementById('tab-bar');
    bar.innerHTML = '';
    for (const name of state.tabNames) {
        const btn = document.createElement('button');
        btn.className = 'tab-btn';
        btn.dataset.tab = name;

        const iconName = TAB_ICONS[name] || 'tune';
        const icon = document.createElement('span');
        icon.className = 'material-symbols-outlined tab-icon';
        icon.textContent = iconName;

        const label = document.createElement('span');
        label.textContent = name;

        btn.appendChild(icon);
        btn.appendChild(label);
        btn.addEventListener('click', () => activateTab(name));
        bar.appendChild(btn);
    }
}

function activateTab(name) {
    // Buttons
    document.querySelectorAll('.tab-btn').forEach(b => {
        b.classList.toggle('active', b.dataset.tab === name);
    });
    // Panels
    document.querySelectorAll('.panel').forEach(p => {
        p.classList.toggle('active', p.dataset.tab === name);
    });
}

// ── Launcher ───────────────────────────────────────────────────

function populateLauncher() {
    document.getElementById('gamePath').value = state.launcher.gamePath || '';
    document.getElementById('gameArgs').value = state.launcher.gameArgs || '';
    document.getElementById('autoStart').checked = !!state.launcher.autoStart;
    document.getElementById('launchOnStartup').checked = !!state.launcher.launchOnStartup;
    document.getElementById('multiClientCount').value = state.launcher.multiClientCount || 5;
    document.getElementById('multiClientDelay').value = state.launcher.multiClientDelay || 0;

    // Monitor dropdowns
    populateSelect('primaryMonitor', state.launcher.monitorChoices, state.launcher.primaryMonitorChoice);
    populateSelect('secondaryMonitor', state.launcher.monitorChoices, state.launcher.secondaryMonitorChoice);

    // Browse button
    document.getElementById('btnBrowse').addEventListener('click', () => {
        sendToAhk('browse-game-path');
    });
}

function populateSelect(id, options, selectedIndex) {
    const sel = document.getElementById(id);
    sel.innerHTML = '';
    (options || []).forEach((label, i) => {
        const opt = document.createElement('option');
        opt.value = i;
        opt.textContent = label;
        if (i === selectedIndex) opt.selected = true;
        sel.appendChild(opt);
    });
}

function handleBrowseResult(msg) {
    if (msg.path) {
        document.getElementById('gamePath').value = msg.path;
    }
}

// ── Minimap ────────────────────────────────────────────────────

function populateMinimap() {
    const scale = document.getElementById('minimapScale');
    const opacity = document.getElementById('minimapOpacity');

    scale.value = state.minimap.scale;
    opacity.value = state.minimap.opacity;

    updateScaleLabel(state.minimap.scale);
    updateOpacityLabel(state.minimap.opacity);

    scale.addEventListener('input', () => updateScaleLabel(scale.value));
    opacity.addEventListener('input', () => updateOpacityLabel(opacity.value));

    // Anchor
    const anchor = document.getElementById('minimapAnchor');
    anchor.value = state.minimap.anchor;

    // Offsets
    document.getElementById('offsetX').value = state.minimap.offsetX;
    document.getElementById('offsetY').value = state.minimap.offsetY;
    document.getElementById('keepOpen').checked = !!state.minimap.keepOpen;

    // Reset position button
    document.getElementById('btnResetPos').addEventListener('click', () => {
        document.getElementById('offsetX').value = 0;
        document.getElementById('offsetY').value = 0;
        document.getElementById('minimapAnchor').value = 'Center';
    });
}

function updateScaleLabel(val) {
    const baseW = state.minimap.baseW || 400;
    const baseH = state.minimap.baseH || 300;
    const w = Math.round(baseW * val / 100);
    const h = Math.round(baseH * val / 100);
    document.getElementById('scaleValue').textContent = `${val}%  (${w}×${h})`;
}

function updateOpacityLabel(val) {
    document.getElementById('opacityValue').textContent = `${val}%`;
}

// ── Hotkeys ────────────────────────────────────────────────────

function populateHotkeys() {
    const container = document.getElementById('hotkeysContainer');
    container.innerHTML = '';
    let prevCat = '';

    for (const action of state.hotkeys.actions) {
        const cat = action.category || 'Other';
        if (cat !== prevCat) {
            const header = document.createElement('div');
            header.className = 'hotkey-category';
            header.textContent = cat;
            container.appendChild(header);
            prevCat = cat;
        }

        const row = document.createElement('div');
        row.className = 'hotkey-row';

        const label = document.createElement('span');
        label.className = 'hotkey-label';
        label.textContent = action.label;

        const btn = document.createElement('button');
        btn.className = 'hotkey-btn';
        btn.id = `hk-btn-${action.id}`;
        btn.textContent = action.display;
        btn.addEventListener('click', () => startCapture(action.id));

        const reset = document.createElement('button');
        reset.className = 'hotkey-reset';
        reset.textContent = 'Reset';
        reset.addEventListener('click', () => resetHotkey(action.id, action.defaultDisplay, action.defaultChord));

        row.appendChild(label);
        row.appendChild(btn);
        row.appendChild(reset);
        container.appendChild(row);

        // Initialize pending state
        pendingHotkeys[action.id] = action.chord;
    }

    // Reset all button
    document.getElementById('btnResetAllHotkeys').addEventListener('click', () => {
        for (const action of state.hotkeys.actions) {
            resetHotkey(action.id, action.defaultDisplay, action.defaultChord);
        }
    });
}

function startCapture(actionId) {
    // Cancel any current capture
    if (capturingActionId) {
        const prevBtn = document.getElementById(`hk-btn-${capturingActionId}`);
        if (prevBtn) prevBtn.classList.remove('capturing');
    }

    capturingActionId = actionId;
    const btn = document.getElementById(`hk-btn-${actionId}`);
    if (btn) {
        btn.textContent = 'Press new shortcut…';
        btn.classList.add('capturing');
    }
    sendToAhk('start-hotkey-capture', { actionId });
}

function handleHotkeyCaptured(msg) {
    const btn = document.getElementById(`hk-btn-${msg.actionId}`);
    if (btn) {
        btn.classList.remove('capturing');
        if (msg.ok) {
            btn.textContent = msg.display;
            pendingHotkeys[msg.actionId] = msg.chord;
        } else {
            // Conflict — show brief error, revert text
            btn.textContent = msg.display || pendingHotkeys[msg.actionId] || '';
            showToast(msg.conflict || 'Conflict', 'error');
        }
    }
    if (capturingActionId === msg.actionId) {
        capturingActionId = null;
    }
}

function handleHotkeyCaptureCancelled(msg) {
    const btn = document.getElementById(`hk-btn-${msg.actionId}`);
    if (btn) {
        btn.classList.remove('capturing');
        // Restore previous display
        const action = state.hotkeys.actions.find(a => a.id === msg.actionId);
        const chord = pendingHotkeys[msg.actionId] || (action && action.chord) || '';
        // Find display for the current pending chord
        btn.textContent = action ? formatChordForDisplay(chord, action) : chord;
    }
    if (capturingActionId === msg.actionId) {
        capturingActionId = null;
    }
}

function formatChordForDisplay(chord, action) {
    // If the chord matches original, use original display
    if (chord === action.chord) return action.display;
    if (chord === action.defaultChord) return action.defaultDisplay;
    // Best-effort: the AHK side sends display strings, so just return chord
    return chord;
}

function resetHotkey(actionId, defaultDisplay, defaultChord) {
    pendingHotkeys[actionId] = defaultChord;
    const btn = document.getElementById(`hk-btn-${actionId}`);
    if (btn) {
        btn.textContent = defaultDisplay;
        btn.classList.remove('capturing');
    }
}

// ── Addon settings tabs ────────────────────────────────────────

function populateAddonTabs() {
    const panels = document.getElementById('panels');

    // State may be refreshed while this page is open. Replace previously
    // generated add-on panels instead of appending another copy of each one.
    panels.querySelectorAll('.addon-panel').forEach(panel => panel.remove());

    if (!state.addonTabs) return;

    for (const addonTab of state.addonTabs) {
        const section = document.createElement('section');
        section.className = 'panel addon-panel';
        section.dataset.tab = addonTab.label;

        const header = document.createElement('div');
        header.className = 'panel-header';
        const h2 = document.createElement('h2');
        h2.textContent = addonTab.label;
        header.appendChild(h2);
        section.appendChild(header);

        const card = document.createElement('div');
        card.className = 'setting-card';

        for (const field of addonTab.fields) {
            const el = renderAddonField(addonTab.addonName, field);
            if (el) card.appendChild(el);
        }

        section.appendChild(card);

        // Insert before the Addons panel
        const addonsPanel = panels.querySelector('[data-tab="Addons"]');
        panels.insertBefore(section, addonsPanel);
    }
}

function renderAddonField(addonName, field) {
    switch (field.type) {
        case 'info': {
            const div = document.createElement('div');
            div.className = 'addon-field-info';
            div.textContent = field.text;
            return div;
        }
        case 'checkbox': {
            const label = document.createElement('label');
            label.className = 'field field-check';
            const cb = document.createElement('input');
            cb.type = 'checkbox';
            cb.className = 'toggle';
            cb.checked = !!field.value;
            cb.id = `addon-${addonName}-${field.id}`;
            cb.addEventListener('change', () => {
                ensureAddonValues(addonName);
                addonSettingsValues[addonName][field.id] = cb.checked;
            });
            const span = document.createElement('span');
            span.textContent = field.label;
            label.appendChild(cb);
            label.appendChild(span);
            // Init value
            ensureAddonValues(addonName);
            addonSettingsValues[addonName][field.id] = cb.checked;
            return label;
        }
        case 'dropdown': {
            const wrapper = document.createElement('label');
            wrapper.className = 'field';
            const lbl = document.createElement('span');
            lbl.className = 'field-label';
            lbl.textContent = field.label;
            const sel = document.createElement('select');
            sel.className = 'select';
            sel.id = `addon-${addonName}-${field.id}`;
            (field.options || []).forEach((opt, i) => {
                const o = document.createElement('option');
                o.value = i;
                o.textContent = opt;
                if (i === field.value) o.selected = true;
                sel.appendChild(o);
            });
            sel.addEventListener('change', () => {
                ensureAddonValues(addonName);
                addonSettingsValues[addonName][field.id] = parseInt(sel.value, 10);
            });
            wrapper.appendChild(lbl);
            wrapper.appendChild(sel);
            // Init value
            ensureAddonValues(addonName);
            addonSettingsValues[addonName][field.id] = parseInt(sel.value, 10);
            return wrapper;
        }
        case 'combo': {
            const wrapper = document.createElement('label');
            wrapper.className = 'field';
            const lbl = document.createElement('span');
            lbl.className = 'field-label';
            lbl.textContent = field.label;
            const inp = document.createElement('input');
            inp.type = 'text';
            inp.className = 'input';
            inp.id = `addon-${addonName}-${field.id}`;
            inp.value = field.value || '';
            inp.setAttribute('list', `addon-${addonName}-${field.id}-list`);
            const dl = document.createElement('datalist');
            dl.id = `addon-${addonName}-${field.id}-list`;
            (field.options || []).forEach(opt => {
                const o = document.createElement('option');
                o.value = opt;
                dl.appendChild(o);
            });
            inp.addEventListener('input', () => {
                ensureAddonValues(addonName);
                addonSettingsValues[addonName][field.id] = inp.value;
            });
            wrapper.appendChild(lbl);
            wrapper.appendChild(inp);
            wrapper.appendChild(dl);
            // Init value
            ensureAddonValues(addonName);
            addonSettingsValues[addonName][field.id] = inp.value;
            return wrapper;
        }
        case 'number': {
            const wrapper = document.createElement('label');
            wrapper.className = 'field';
            const lbl = document.createElement('span');
            lbl.className = 'field-label';
            lbl.textContent = field.label;
            const inp = document.createElement('input');
            inp.type = 'number';
            inp.className = 'input input-narrow';
            inp.id = `addon-${addonName}-${field.id}`;
            inp.value = field.value || 0;
            if (field.min !== undefined) inp.min = field.min;
            if (field.max !== undefined) inp.max = field.max;
            inp.addEventListener('input', () => {
                ensureAddonValues(addonName);
                addonSettingsValues[addonName][field.id] = parseInt(inp.value, 10);
            });
            wrapper.appendChild(lbl);
            wrapper.appendChild(inp);
            ensureAddonValues(addonName);
            addonSettingsValues[addonName][field.id] = parseInt(inp.value, 10);
            return wrapper;
        }
        default:
            return null;
    }
}

function ensureAddonValues(name) {
    if (!addonSettingsValues[name]) addonSettingsValues[name] = {};
}

// ── Addons enable/disable ──────────────────────────────────────

function populateAddons() {
    const container = document.getElementById('addonsContainer');
    container.innerHTML = '';
    if (!state.addons) return;

    for (const addon of state.addons) {
        const row = document.createElement('label');
        row.className = 'addon-row';

        const cb = document.createElement('input');
        cb.type = 'checkbox';
        cb.className = 'toggle';
        cb.checked = addon.enabled;
        cb.dataset.addonName = addon.name;

        const name = document.createElement('span');
        name.className = 'addon-name';
        name.textContent = addon.name;

        row.appendChild(cb);
        row.appendChild(name);
        container.appendChild(row);
    }
}

// ── Save / Cancel / Close ──────────────────────────────────────

const btnTitleClose = document.getElementById('btnTitleClose');
if (btnTitleClose) {
    btnTitleClose.addEventListener('click', () => sendToAhk('cancel'));
}

document.getElementById('btnSave').addEventListener('click', collectAndSave);
document.getElementById('btnCancel').addEventListener('click', () => sendToAhk('cancel'));

function collectAndSave() {
    // Launcher
    const launcher = {
        gamePath:           document.getElementById('gamePath').value,
        gameArgs:           document.getElementById('gameArgs').value,
        autoStart:          document.getElementById('autoStart').checked,
        launchOnStartup:    document.getElementById('launchOnStartup').checked,
        multiClientCount:   parseInt(document.getElementById('multiClientCount').value, 10) || 5,
        multiClientDelay:   parseInt(document.getElementById('multiClientDelay').value, 10) || 0,
        primaryMonitor:     parseInt(document.getElementById('primaryMonitor').value, 10),
        secondaryMonitor:   parseInt(document.getElementById('secondaryMonitor').value, 10),
    };

    // Minimap
    const minimap = {
        scale:    parseInt(document.getElementById('minimapScale').value, 10),
        opacity:  parseInt(document.getElementById('minimapOpacity').value, 10),
        anchor:   document.getElementById('minimapAnchor').value,
        offsetX:  parseInt(document.getElementById('offsetX').value, 10) || 0,
        offsetY:  parseInt(document.getElementById('offsetY').value, 10) || 0,
        keepOpen: document.getElementById('keepOpen').checked,
    };

    // Hotkeys
    const hotkeys = [];
    for (const action of state.hotkeys.actions) {
        hotkeys.push({
            id:    action.id,
            chord: pendingHotkeys[action.id] || action.chord,
        });
    }

    // Addon enable/disable
    const addons = {};
    document.querySelectorAll('#addonsContainer .toggle').forEach(cb => {
        addons[cb.dataset.addonName] = cb.checked;
    });

    sendToAhk('save', { launcher, minimap, hotkeys, addons, addonSettings: addonSettingsValues });
}

function handleSaveResult(msg) {
    if (msg.ok) {
        showToast('Settings saved', 'success');
    } else {
        showToast(msg.error || 'Save failed', 'error');
    }
}

// ── Toast ──────────────────────────────────────────────────────

function showToast(text, type = 'success') {
    const area = document.getElementById('toastArea');
    area.innerHTML = '';
    const toast = document.createElement('span');
    toast.className = `toast toast-${type}`;
    toast.textContent = text;
    area.appendChild(toast);
    setTimeout(() => { if (area.contains(toast)) area.removeChild(toast); }, 3000);
}
