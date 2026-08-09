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

/** UI-only state for clear save feedback and per-tab scroll restoration. */
let dirty = false;
let saving = false;
let currentTab = '';
const tabScroll = {};

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
    'update-check-result': onUpdateCheckResult,
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
    populateAppearance();
    populateHotkeys();
    populateAddonTabs();
    populateAddons();
    buildTabBar();
    activateTab(state.tabNames[0] || 'Launcher');
    setDirty(false);
}

// ── Tab switching ──────────────────────────────────────────────

const TAB_ICONS = {
    'Launcher': 'rocket_launch',
    'Minimap': 'map',
    'Appearance': 'settings',
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
    bar.setAttribute('role', 'tablist');
    bar.setAttribute('aria-orientation', 'vertical');
    bar.innerHTML = '';
    for (const name of state.tabNames) {
        const btn = document.createElement('button');
        btn.className = 'tab-btn';
        btn.type = 'button';
        btn.setAttribute('role', 'tab');
        btn.dataset.tab = name;
        btn.title = name;

        const iconName = TAB_ICONS[name] || 'tune';
        const icon = document.createElement('span');
        icon.className = 'material-symbols-outlined tab-icon';
        icon.textContent = iconName;

        const label = document.createElement('span');
        label.className = 'tab-label';
        label.textContent = name;

        btn.appendChild(icon);
        btn.appendChild(label);
        btn.addEventListener('click', () => activateTab(name));
        btn.addEventListener('keydown', event => {
            if (event.key !== 'ArrowDown' && event.key !== 'ArrowUp') return;
            event.preventDefault();
            const tabs = [...bar.querySelectorAll('.tab-btn')];
            const step = event.key === 'ArrowDown' ? 1 : -1;
            const next = tabs[(tabs.indexOf(btn) + step + tabs.length) % tabs.length];
            next.focus();
            activateTab(next.dataset.tab);
        });
        bar.appendChild(btn);
    }
}

// ── Appearance ────────────────────────────────────────────────

function populateAppearance() {
    const scheme = window.AccentTheme
        ? window.AccentTheme.normalize(state.appearance && state.appearance.accentScheme)
        : 'amber';
    const option = document.querySelector(`input[name="accentScheme"][value="${scheme}"]`);
    if (option) option.checked = true;
    if (window.AccentTheme) window.AccentTheme.preview(scheme);
}

document.querySelectorAll('input[name="accentScheme"]').forEach(option => {
    option.addEventListener('change', () => {
        if (option.checked && window.AccentTheme) window.AccentTheme.preview(option.value);
    });
});

function activateTab(name) {
    // Leaving the Hotkeys tab mid-capture would otherwise leave AHK's
    // InputHook listening in the background with no visible "capturing"
    // button — the next keystroke typed anywhere (e.g. into another
    // tab's text field) would silently get bound to the hidden action.
    if (name !== 'Hotkeys' && capturingActionId) {
        cancelActiveCapture();
    }

    const content = document.querySelector('.content-area');
    if (currentTab) tabScroll[currentTab] = content.scrollTop;
    currentTab = name;

    // Buttons
    document.querySelectorAll('.tab-btn').forEach(b => {
        const active = b.dataset.tab === name;
        b.classList.toggle('active', active);
        b.setAttribute('aria-selected', active ? 'true' : 'false');
        b.tabIndex = active ? 0 : -1;
    });
    // Panels
    document.querySelectorAll('.panel').forEach(p => {
        p.classList.toggle('active', p.dataset.tab === name);
    });
    requestAnimationFrame(() => { content.scrollTop = tabScroll[name] || 0; });
}

// ── Launcher ───────────────────────────────────────────────────

function populateLauncher() {
    document.getElementById('gamePath').value = state.launcher.gamePath || '';
    document.getElementById('gameArgs').value = state.launcher.gameArgs || '';
    document.getElementById('autoStart').checked = !!state.launcher.autoStart;
    document.getElementById('launchOnStartup').checked = !!state.launcher.launchOnStartup;

    // Version, so a screenshot of this window identifies the build.
    const versionEl = document.getElementById('appVersion');
    if (versionEl) versionEl.textContent = state.appVersion || '';

    // The update controls only exist when this build has somewhere to check.
    // "Check now" is deliberately independent of the startup-check toggle:
    // turning off automatic checks should not take away the manual one.
    const available = !!state.launcher.versionCheckAvailable;
    const vcField = document.getElementById('versionCheckField');
    const vcInput = document.getElementById('versionCheck');
    if (vcField && vcInput) {
        vcField.hidden = !available;
        vcInput.checked = !!state.launcher.versionCheck;
    }
    const vcRow = document.getElementById('updateCheckRow');
    if (vcRow) vcRow.hidden = !available;

    // Carry over an update this session already found, so reopening Settings
    // does not look like it forgot.
    if (state.launcher.updateVersion) {
        setUpdateStatus('update', `${state.launcher.updateVersion} is available`
            + (state.launcher.updateNotes ? ' — ' + state.launcher.updateNotes : ''));
        const get = document.getElementById('btnGetUpdate');
        if (get) get.hidden = false;
    }
    document.getElementById('multiClientCount').value = state.launcher.multiClientCount || 5;
    document.getElementById('multiClientDelay').value = state.launcher.multiClientDelay || 0;

    // Monitor dropdowns
    populateSelect('primaryMonitor', state.launcher.monitorChoices, state.launcher.primaryMonitorChoice);
    populateSelect('secondaryMonitor', state.launcher.monitorChoices, state.launcher.secondaryMonitorChoice);
    document.querySelectorAll('.launch-layout-field').forEach(el => {
        el.hidden = !state.launcher.layoutAvailable;
    });
    if (state.launcher.layoutAvailable) {
        populateValueSelect('primaryLaunchLayout', state.launcher.launchLayoutOptions,
            state.launcher.primaryLaunchLayout);
        populateValueSelect('secondaryLaunchLayout', state.launcher.launchLayoutOptions,
            state.launcher.secondaryLaunchLayout);
    }

    // Browse button
    document.getElementById('btnBrowse').addEventListener('click', () => {
        sendToAhk('browse-game-path');
    });

    // Manual update check. The button is disabled while the request is in
    // flight — it is bounded by a timeout AHK-side, but a button that can be
    // mashed into three concurrent checks is still wrong.
    document.getElementById('btnCheckUpdate').addEventListener('click', () => {
        const btn = document.getElementById('btnCheckUpdate');
        btn.disabled = true;
        setUpdateStatus('', 'Checking…');
        document.getElementById('btnGetUpdate').hidden = true;
        sendToAhk('check-update');
    });

    document.getElementById('btnGetUpdate').addEventListener('click', () => {
        sendToAhk('open-update-page');
    });
}

function setUpdateStatus(kind, text) {
    const el = document.getElementById('updateStatus');
    if (!el) return;
    el.className = 'update-status' + (kind ? ' is-' + kind : '');
    el.textContent = text;
}

function onUpdateCheckResult(msg) {
    const btn = document.getElementById('btnCheckUpdate');
    if (btn) btn.disabled = false;
    const get = document.getElementById('btnGetUpdate');

    if (msg.status === 'update') {
        setUpdateStatus('update',
            `${msg.version} is available${msg.notes ? ' — ' + msg.notes : ''}`);
        if (get) get.hidden = false;
        return;
    }
    if (get) get.hidden = true;
    if (msg.status === 'current') {
        setUpdateStatus('current', `You are up to date (${msg.current}).`);
    } else {
        // "failed" and "disabled" both land here; reason explains which.
        setUpdateStatus('error', msg.reason || 'The check could not be completed.');
    }
}

function populateValueSelect(id, options, selectedValue) {
    const sel = document.getElementById(id);
    sel.innerHTML = '';
    (options || []).forEach(item => {
        const opt = document.createElement('option');
        opt.value = item.value;
        opt.textContent = item.label;
        opt.selected = item.value === selectedValue;
        sel.appendChild(opt);
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
        setDirty(true);
        showToast('Game path selected', 'success');
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

    scale.addEventListener('input', () => {
        updateScaleLabel(scale.value);
        updateMinimapPreview();
    });
    opacity.addEventListener('input', () => {
        updateOpacityLabel(opacity.value);
        updateMinimapPreview();
    });

    // Anchor
    const anchor = document.getElementById('minimapAnchor');
    anchor.value = state.minimap.anchor;

    // Offsets
    document.getElementById('offsetX').value = state.minimap.offsetX;
    document.getElementById('offsetY').value = state.minimap.offsetY;
    document.getElementById('keepOpen').checked = !!state.minimap.keepOpen;
    document.getElementById('showHoverCoords').checked = !!state.minimap.showHoverCoords;
    ['minimapAnchor', 'offsetX', 'offsetY'].forEach(id => {
        document.getElementById(id).addEventListener('input', updateMinimapPreview);
        document.getElementById(id).addEventListener('change', updateMinimapPreview);
    });

    // Reset position button
    document.getElementById('btnResetPos').addEventListener('click', () => {
        document.getElementById('offsetX').value = 0;
        document.getElementById('offsetY').value = 0;
        document.getElementById('minimapAnchor').value = 'Center';
        updateMinimapPreview();
        setDirty(true);
        const preview = document.getElementById('minimapPreview');
        preview.classList.remove('just-reset');
        void preview.offsetWidth;
        preview.classList.add('just-reset');
    });

    updateMinimapPreview();
}

function updateScaleLabel(val) {
    const baseW = state.minimap.baseW || 400;
    const baseH = state.minimap.baseH || 300;
    const w = Math.round(baseW * val / 100);
    const h = Math.round(baseH * val / 100);
    document.getElementById('scaleValue').textContent = `${val}%  (${w}×${h})`;
    updateRangeFill(document.getElementById('minimapScale'));
}

function updateOpacityLabel(val) {
    document.getElementById('opacityValue').textContent = `${val}%`;
    updateRangeFill(document.getElementById('minimapOpacity'));
}

function updateRangeFill(range) {
    const min = Number(range.min) || 0;
    const max = Number(range.max) || 100;
    const pct = ((Number(range.value) - min) / Math.max(1, max - min)) * 100;
    range.style.setProperty('--range-pct', `${pct}%`);
}

function updateMinimapPreview() {
    const map = document.getElementById('previewMap');
    const stage = document.getElementById('previewStage');
    if (!map || !state) return;
    const scale = Number(document.getElementById('minimapScale').value) || 100;
    const opacity = Number(document.getElementById('minimapOpacity').value) || 100;
    const anchor = document.getElementById('minimapAnchor').value || 'Center';
    const offsetX = Number(document.getElementById('offsetX').value) || 0;
    const offsetY = Number(document.getElementById('offsetY').value) || 0;
    const baseW = Number(state.minimap.baseW) || 400;
    const baseH = Number(state.minimap.baseH) || 300;
    const clientW = Number(state.minimap.clientW) || 1024;
    const clientH = Number(state.minimap.clientH) || 768;
    const stageW = stage.clientWidth || 320;
    const stageH = stage.clientHeight || 240;
    const widthPct = (baseW * scale / 100) / clientW * 100;
    map.style.width = `${Math.min(96, widthPct)}%`;
    map.style.aspectRatio = `${baseW} / ${baseH}`;
    map.style.opacity = String(opacity / 100);
    map.style.setProperty('--preview-nudge-x', `${offsetX / clientW * stageW}px`);
    map.style.setProperty('--preview-nudge-y', `${offsetY / clientH * stageH}px`);
    map.dataset.anchor = anchor;
    document.getElementById('previewFrameLabel').textContent =
        `Game window · ${clientW} × ${clientH}`;
    const labels = {
        Center: 'Centred', TopLeft: 'Top-left', TopRight: 'Top-right',
        BottomLeft: 'Bottom-left', BottomRight: 'Bottom-right'
    };
    document.getElementById('previewAnchorLabel').textContent = labels[anchor] || anchor;
}

// ── Hotkeys ────────────────────────────────────────────────────

function populateHotkeys() {
    const container = document.getElementById('hotkeysContainer');
    container.innerHTML = '';
    let prevCat = '';

    for (const action of state.hotkeys.actions) {
        const cat = action.category || 'Other';
        if (cat !== prevCat) {
            const isCore = cat === 'Core';
            const header = document.createElement('div');
            header.className = `hotkey-category ${isCore ? 'category-core' : 'category-addon'}`;
            if (!isCore) {
                const icon = document.createElement('span');
                icon.className = 'material-symbols-outlined category-icon';
                icon.textContent = 'extension';
                icon.title = 'Provided by an addon — disappears if the addon is disabled';
                header.appendChild(icon);
            }
            const label = document.createElement('span');
            label.textContent = cat;
            header.appendChild(label);
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
        btn.textContent = action.display || 'Not bound';
        btn.classList.toggle('is-unbound', !action.chord);
        btn.addEventListener('click', () => startCapture(action.id));

        const actions = document.createElement('span');
        actions.className = 'hotkey-row-actions';

        const unbind = document.createElement('button');
        unbind.className = 'hotkey-action';
        unbind.textContent = 'Unbind';
        unbind.addEventListener('click', () => unbindHotkey(action.id));

        const reset = document.createElement('button');
        reset.className = 'hotkey-action';
        reset.textContent = 'Reset';
        reset.addEventListener('click', () => resetHotkey(action.id, action.defaultDisplay, action.defaultChord));

        actions.appendChild(unbind);
        actions.appendChild(reset);

        row.appendChild(label);
        row.appendChild(btn);
        row.appendChild(actions);
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
    // Cancel any current capture — AHK-side cancellation happens as a
    // side effect of start-hotkey-capture, so no separate message needed.
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
    sendToAhk('start-hotkey-capture', { actionId, hotkeys: collectHotkeys() });
}

// Tell AHK to stop listening for a keypress when the capture is abandoned
// through something other than pressing a key or starting another capture
// (e.g. Reset / Reset all, or switching away from the Hotkeys tab). AHK's
// InputHook otherwise keeps running silently and the next keystroke typed
// anywhere gets bound to the action that's no longer even showing "Press
// new shortcut…".
function cancelActiveCapture() {
    if (!capturingActionId) return;
    const actionId = capturingActionId;
    const btn = document.getElementById(`hk-btn-${actionId}`);
    if (btn) btn.classList.remove('capturing');
    capturingActionId = null;
    sendToAhk('cancel-hotkey-capture', { actionId });
}

function handleHotkeyCaptured(msg) {
    const btn = document.getElementById(`hk-btn-${msg.actionId}`);
    if (btn) {
        btn.classList.remove('capturing');
        if (msg.ok) {
            btn.textContent = msg.display || 'Not bound';
            btn.classList.toggle('is-unbound', !msg.chord);
            pendingHotkeys[msg.actionId] = msg.chord;
            setDirty(true);
            flashHotkeyRow(btn);
            const action = state.hotkeys.actions.find(a => a.id === msg.actionId);
            showToast(`${msg.display || 'Shortcut'} assigned${action ? ` to ${action.label}` : ''}`, 'success');
        } else {
            // Conflict — show brief error, revert text
            const action = state.hotkeys.actions.find(a => a.id === msg.actionId);
            const chord = action ? getPendingHotkey(action) : '';
            btn.textContent = msg.display || formatChordForDisplay(chord, action);
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
        const chord = action ? getPendingHotkey(action) : '';
        // Find display for the current pending chord
        btn.textContent = action ? formatChordForDisplay(chord, action) : chord;
        btn.classList.toggle('is-unbound', !chord);
    }
    if (capturingActionId === msg.actionId) {
        capturingActionId = null;
    }
}

function formatChordForDisplay(chord, action) {
    if (!chord) return 'Not bound';
    // If the chord matches original, use original display
    if (chord === action.chord) return action.display;
    if (chord === action.defaultChord) return action.defaultDisplay;
    // Best-effort: the AHK side sends display strings, so just return chord
    return chord;
}

function getPendingHotkey(action) {
    return Object.prototype.hasOwnProperty.call(pendingHotkeys, action.id)
        ? pendingHotkeys[action.id]
        : action.chord;
}

function collectHotkeys() {
    return state.hotkeys.actions.map(action => ({
        id: action.id,
        chord: getPendingHotkey(action),
    }));
}

function unbindHotkey(actionId) {
    if (capturingActionId === actionId) cancelActiveCapture();
    pendingHotkeys[actionId] = '';
    const btn = document.getElementById(`hk-btn-${actionId}`);
    if (btn) {
        btn.textContent = 'Not bound';
        btn.classList.remove('capturing');
        btn.classList.add('is-unbound');
        flashHotkeyRow(btn);
    }
    setDirty(true);
}

function resetHotkey(actionId, defaultDisplay, defaultChord) {
    if (capturingActionId === actionId) cancelActiveCapture();
    pendingHotkeys[actionId] = defaultChord;
    const btn = document.getElementById(`hk-btn-${actionId}`);
    if (btn) {
        btn.textContent = defaultDisplay || 'Not bound';
        btn.classList.remove('capturing');
        btn.classList.toggle('is-unbound', !defaultChord);
        flashHotkeyRow(btn);
    }
    setDirty(true);
}

function flashHotkeyRow(button) {
    const row = button && button.closest('.hotkey-row');
    if (!row) return;
    row.classList.remove('just-changed');
    void row.offsetWidth;
    row.classList.add('just-changed');
    row.addEventListener('animationend', () => row.classList.remove('just-changed'), { once: true });
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
        case 'orderedlist':
            return renderOrderedList(addonName, field);
        default:
            return null;
    }
}

/* A pick-and-order list: every offerable row, checked ones first in the order
   they were chosen, unchecked ones after. The stored value is the checked ids
   joined with commas — the same shape the native frontend sends, so the addon
   has one save path rather than one per frontend. */
function renderOrderedList(addonName, field) {
    const wrapper = document.createElement('div');
    wrapper.className = 'field';

    const lbl = document.createElement('span');
    lbl.className = 'field-label';
    lbl.textContent = field.label;
    wrapper.appendChild(lbl);

    const list = document.createElement('div');
    list.className = 'ordered-list';
    wrapper.appendChild(list);

    const hint = document.createElement('div');
    hint.className = 'ordered-hint';
    wrapper.appendChild(hint);

    const all = field.items || [];
    const chosen = String(field.value || '')
        .split(',')
        .map(s => s.trim())
        .filter(id => id && all.some(it => it.id === id));

    // Chosen first, in their stored order; everything else keeps catalog order.
    let rows = chosen
        .map(id => ({ id, checked: true }))
        .concat(all.filter(it => !chosen.includes(it.id)).map(it => ({ id: it.id, checked: false })));

    const meta = id => all.find(it => it.id === id) || { id, label: id, icon: '' };

    function commit() {
        ensureAddonValues(addonName);
        addonSettingsValues[addonName][field.id] =
            rows.filter(r => r.checked).map(r => r.id).join(',');
    }

    function move(index, delta) {
        const to = index + delta;
        if (to < 0 || to >= rows.length) return;
        [rows[index], rows[to]] = [rows[to], rows[index]];
        setDirty(true);
        draw();
    }

    function draw() {
        const count = rows.filter(r => r.checked).length;
        const atMax = field.max !== undefined && count >= field.max;

        list.innerHTML = '';
        rows.forEach((row, i) => {
            const info = meta(row.id);
            const el = document.createElement('div');
            el.className = 'ordered-row' + (row.checked ? ' is-on' : '');

            const cb = document.createElement('input');
            cb.type = 'checkbox';
            cb.className = 'toggle';
            cb.checked = row.checked;
            // A full list still lets you uncheck, just not check anything more.
            cb.disabled = atMax && !row.checked;
            cb.addEventListener('change', () => {
                row.checked = cb.checked;
                draw();
            });

            const icon = document.createElement('span');
            icon.className = 'material-symbols-outlined ordered-icon';
            icon.textContent = info.icon || 'pin_drop';

            const name = document.createElement('span');
            name.className = 'ordered-label';
            name.textContent = info.label;

            const up = document.createElement('button');
            up.type = 'button';
            up.className = 'ordered-move';
            up.textContent = '↑';
            up.title = 'Move up';
            up.disabled = i === 0;
            up.addEventListener('click', () => move(i, -1));

            const down = document.createElement('button');
            down.type = 'button';
            down.className = 'ordered-move';
            down.textContent = '↓';
            down.title = 'Move down';
            down.disabled = i === rows.length - 1;
            down.addEventListener('click', () => move(i, 1));

            el.appendChild(cb);
            el.appendChild(icon);
            el.appendChild(name);
            el.appendChild(up);
            el.appendChild(down);
            list.appendChild(el);
        });

        hint.textContent = field.max !== undefined
            ? `${count} of ${field.max} selected${atMax ? ' — uncheck one to add another' : ''}`
            : `${count} selected`;

        commit();
    }

    draw();
    return wrapper;
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
document.getElementById('btnResetDefaults').addEventListener('click', () => sendToAhk('reset-defaults'));

function collectAndSave() {
    if (saving || !dirty) return;
    if (capturingActionId) cancelActiveCapture();
    saving = true;
    document.body.classList.add('is-saving');
    const saveButton = document.getElementById('btnSave');
    saveButton.disabled = true;
    saveButton.textContent = 'Saving…';
    setSaveState('Saving changes…', 'saving');

    // Launcher
    const launcher = {
        gamePath:           document.getElementById('gamePath').value,
        gameArgs:           document.getElementById('gameArgs').value,
        autoStart:          document.getElementById('autoStart').checked,
        launchOnStartup:    document.getElementById('launchOnStartup').checked,
        versionCheck:       document.getElementById('versionCheck').checked,
        multiClientCount:   parseInt(document.getElementById('multiClientCount').value, 10) || 5,
        multiClientDelay:   parseInt(document.getElementById('multiClientDelay').value, 10) || 0,
        primaryMonitor:     parseInt(document.getElementById('primaryMonitor').value, 10),
        secondaryMonitor:   parseInt(document.getElementById('secondaryMonitor').value, 10),
        primaryLaunchLayout: state.launcher.layoutAvailable
            ? document.getElementById('primaryLaunchLayout').value
            : state.launcher.primaryLaunchLayout,
        secondaryLaunchLayout: state.launcher.layoutAvailable
            ? document.getElementById('secondaryLaunchLayout').value
            : state.launcher.secondaryLaunchLayout,
    };

    // Minimap
    const minimap = {
        scale:    parseInt(document.getElementById('minimapScale').value, 10),
        opacity:  parseInt(document.getElementById('minimapOpacity').value, 10),
        anchor:   document.getElementById('minimapAnchor').value,
        offsetX:  parseInt(document.getElementById('offsetX').value, 10) || 0,
        offsetY:  parseInt(document.getElementById('offsetY').value, 10) || 0,
        keepOpen: document.getElementById('keepOpen').checked,
        showHoverCoords: document.getElementById('showHoverCoords').checked,
    };

    const selectedScheme = document.querySelector('input[name="accentScheme"]:checked');
    const appearance = {
        accentScheme: selectedScheme ? selectedScheme.value : 'amber',
    };

    if (window.AccentTheme) window.AccentTheme.apply(appearance.accentScheme);

    // Hotkeys
    const hotkeys = collectHotkeys();

    // Addon enable/disable
    const addons = {};
    document.querySelectorAll('#addonsContainer .toggle').forEach(cb => {
        addons[cb.dataset.addonName] = cb.checked;
    });

    sendToAhk('save', { launcher, minimap, appearance, hotkeys, addons, addonSettings: addonSettingsValues });
}

function handleSaveResult(msg) {
    if (msg.ok) {
        saving = false;
        setDirty(false);
        document.body.classList.remove('is-saving');
        document.body.classList.add('save-complete');
        document.getElementById('btnSave').textContent = 'Saved ✓';
        setSaveState('Settings saved', 'saved');
        showToast('Settings saved', 'success');
    } else {
        saving = false;
        document.body.classList.remove('is-saving');
        document.getElementById('btnSave').textContent = 'Try again';
        document.getElementById('btnSave').disabled = false;
        setSaveState('Changes need attention', 'dirty');
        if (window.AccentTheme && state && state.appearance) {
            window.AccentTheme.apply(state.appearance.accentScheme);
        }
        showToast(msg.error || 'Save failed', 'error');
    }
}

function setDirty(next) {
    dirty = !!next;
    const button = document.getElementById('btnSave');
    if (!saving) {
        button.disabled = !dirty;
        button.textContent = dirty ? 'Save changes' : 'Save changes';
    }
    setSaveState(dirty ? 'Unsaved changes' : 'No unsaved changes', dirty ? 'dirty' : '');
}

function setSaveState(text, tone = '') {
    const el = document.getElementById('saveState');
    el.className = `save-state${tone ? ` ${tone}` : ''}`;
    el.lastChild.textContent = text;
}

function markControlChanged(event) {
    if (!state || saving) return;
    const control = event.target.closest('input, select, textarea');
    if (!control || control.readOnly) return;
    setDirty(true);
    if (event.type === 'change' && control.classList.contains('toggle')) {
        control.classList.remove('just-toggled');
        void control.offsetWidth;
        control.classList.add('just-toggled');
        control.addEventListener('animationend', () => control.classList.remove('just-toggled'), { once: true });
    }
}

document.addEventListener('input', markControlChanged);
document.addEventListener('change', markControlChanged);

document.addEventListener('keydown', event => {
    if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 's') {
        event.preventDefault();
        collectAndSave();
    }
});

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

// Local visual-regression fixture. WebView2 never enters this branch.
const previewTab = new URLSearchParams(location.search).get('preview');
if (previewTab !== null && !window.chrome?.webview) {
    handleSettingsState({
        type: 'settings-state',
        tabNames: ['Launcher', 'Minimap', 'Appearance', 'Hotkeys', 'Map POIs', 'Addons'],
        launcher: {
            gamePath: 'C:\\Games\\MythWar\\mythwar.exe', gameArgs: '-windowed',
            autoStart: true, launchOnStartup: false, multiClientCount: 4, multiClientDelay: 650,
            monitorChoices: ['Display 1 · 1920 × 1080', 'Display 2 · 2560 × 1440'],
            primaryMonitorChoice: 0, secondaryMonitorChoice: 1, layoutAvailable: true,
            launchLayoutOptions: [{ value: '', label: 'Default layout' }, { value: 'Four box', label: 'Four box' }],
            primaryLaunchLayout: 'Four box', secondaryLaunchLayout: ''
        },
        minimap: { scale: 115, opacity: 80, anchor: 'TopRight', offsetX: -12, offsetY: 18, keepOpen: true, baseW: 400, baseH: 300, clientW: 1024, clientH: 768 },
        appearance: { accentScheme: 'amber' },
        hotkeys: { actions: [
            { id: 'toggle-map', label: 'Toggle minimap', category: 'Core', chord: '^m', display: 'Ctrl+M', defaultChord: '^m', defaultDisplay: 'Ctrl+M' },
            { id: 'settings', label: 'Open settings', category: 'Core', chord: '^,', display: 'Ctrl+,', defaultChord: '^,', defaultDisplay: 'Ctrl+,' },
            { id: 'layout', label: 'Apply default layout', category: 'Window Layout', chord: '!l', display: 'Alt+L', defaultChord: '!l', defaultDisplay: 'Alt+L' }
        ] },
        addonTabs: [{ label: 'Map POIs', addonName: 'Map POIs', fields: [
            { type: 'info', text: 'Choose which points of interest appear on the minimap.' },
            { type: 'checkbox', id: 'enabled', label: 'Show map points of interest', value: true },
            { type: 'number', id: 'radius', label: 'Marker radius', value: 8, min: 2, max: 24 }
        ] }],
        addons: [{ name: 'Map POIs', enabled: true }, { name: 'Window Layout', enabled: true }, { name: 'Discord RPC', enabled: false }]
    });
    if (previewTab && state.tabNames.includes(previewTab)) activateTab(previewTab);
}
