/* Setup guide.

   Host contract (setup.ahk):
     host -> page  { type: 'setup-state', firstRun, game, minimap, launch, features, finish }
                   { type: 'game-result', ok, path, status, tone, modsInstalled, accountCount }
                   { type: 'capture-result', chord, display } / { type: 'capture-cancelled' }
     page -> host  init-request · refresh · browse-game-path · use-game-path {path}
                   start-capture · cancel-capture · edit-accounts
                   finish {choices} · skip · close

   The game path is saved by the host as soon as it is picked (the accounts
   editor and the client-mods check read it); everything else is collected
   here and written once, by Setup_Apply, when Finish is pressed. */

'use strict';

const $ = (id) => document.getElementById(id);

function sendToAhk(type, payload = {}) {
    if (window.chrome && window.chrome.webview) {
        window.chrome.webview.postMessage(JSON.stringify({ type, ...payload }));
    }
}

const STEPS = ['welcome', 'game', 'minimap', 'launching', 'features', 'finish'];
const STEP_LABELS = {
    welcome: 'Welcome', game: 'Game', minimap: 'Minimap',
    launching: 'Launching', features: 'Features', finish: 'Finish'
};
const FEATURE_GROUPS = [['map', 'Map'], ['multibox', 'Multi-boxing'], ['tools', 'Tools']];

let state = null;
let choices = null;
let current = 'welcome';
let reached = 0;          // furthest step index visited, for clickable pills
let capturing = false;

// ── Messages in ────────────────────────────────────────────────

function onAhkMessage(event) {
    let msg = event.data;
    if (typeof msg === 'string') {
        try { msg = JSON.parse(msg); } catch (e) { return; }
    }
    if (!msg || typeof msg !== 'object') return;
    switch (msg.type) {
        case 'setup-state':
            if (!state) {
                state = msg;
                choices = initialChoices(msg);
                renderAll();
                go('welcome');
            } else {
                // A refresh (window focus, back from the accounts editor):
                // take the host's facts, keep the user's choices.
                state.game = msg.game;
                state.launch.accountCount = msg.launch.accountCount;
                state.launch.modsInstalled = msg.launch.modsInstalled;
                renderGame();
                renderLaunching();
            }
            break;
        case 'game-result':
            state.game.path = msg.path;
            state.game.status = msg.status;
            state.game.tone = msg.tone;
            state.launch.modsInstalled = msg.modsInstalled;
            state.launch.accountCount = msg.accountCount;
            renderGame();
            renderLaunching();
            renderFinish();
            break;
        case 'capture-result':
            capturing = false;
            choices.openChord = msg.chord || '';
            choices.openChordDisplay = msg.display || '';
            choices.openChordSet = true;
            renderOpenKey();
            break;
        case 'capture-cancelled':
            capturing = false;
            renderOpenKey();
            break;
    }
}

if (window.chrome && window.chrome.webview) {
    window.chrome.webview.addEventListener('message', onAhkMessage);
}

// ── Choices ────────────────────────────────────────────────────

function initialChoices(s) {
    const features = {};
    s.features.forEach(f => { features[f.name] = !!f.enabled; });
    return {
        // A first run asks; a re-run starts from how Maps++ is set up now.
        style: s.firstRun ? null : s.launch.style,
        minimapSource: s.minimap.source || 'game',
        openChord: s.minimap.openChord || '',
        openChordDisplay: s.minimap.openChordDisplay || '',
        openChordSet: false,
        primaryCount: s.launch.primaryCount || 1,
        primaryMonitor: s.launch.primaryMonitor || 0,
        primaryLayout: s.launch.primaryLayout || '',
        secondEnabled: !!s.launch.secondEnabled,
        secondCount: s.launch.secondCount || 1,
        secondMonitor: s.launch.secondMonitor || 0,
        autoLogin: !!s.launch.autoLogin,
        features,
        interfaceMode: s.finish.interfaceMode || 'webview',
        accent: s.finish.accent || 'amber',
        autoStart: !!s.finish.autoStart,
        launchNow: false
    };
}

// Picking a style sets the multi-boxing features to match; anything else in
// the list keeps whatever the user (or the current setup) chose.
function setStyle(style) {
    if (choices.style === style) return;
    choices.style = style;
    state.features.forEach(f => {
        if (f.multibox) choices.features[f.name] = style === 'multi';
    });
    if (style === 'solo') choices.primaryCount = 1;
    else if (choices.primaryCount < 2) choices.primaryCount = 2;
    renderWelcome();
    renderLaunching();
    renderFeatures();
    renderSteps();
    updateFooter();
}

function visibleSteps() {
    return STEPS.filter(s => s !== 'launching' || choices.style === 'multi');
}

// ── Navigation ─────────────────────────────────────────────────

function go(step) {
    if (capturing) { sendToAhk('cancel-capture'); capturing = false; renderOpenKey(); }
    current = step;
    const idx = visibleSteps().indexOf(step);
    reached = Math.max(reached, idx);
    document.querySelectorAll('.step').forEach(el =>
        el.classList.toggle('active', el.dataset.step === step));
    $('body').scrollTop = 0;
    if (step === 'launching') sendToAhk('refresh');
    renderSteps();
    updateFooter();
}

function renderSteps() {
    const nav = $('steps');
    nav.innerHTML = '';
    const steps = visibleSteps();
    const at = steps.indexOf(current);
    steps.forEach((step, i) => {
        const pill = document.createElement('button');
        pill.type = 'button';
        pill.className = 'step-pill' + (i === at ? ' current' : '') + (i < at || (i <= reached && i !== at) ? ' done' : '');
        pill.disabled = !(i <= reached && i !== at) || (i > 0 && !choices.style);
        pill.setAttribute('aria-current', i === at ? 'step' : 'false');
        const num = document.createElement('span');
        num.className = 'step-num';
        num.textContent = i < at ? '✓' : String(i + 1);
        const label = document.createElement('span');
        label.textContent = STEP_LABELS[step];
        pill.append(num, label);
        pill.addEventListener('click', () => { if (!pill.disabled) go(step); });
        nav.appendChild(pill);
    });
}

function updateFooter() {
    const steps = visibleSteps();
    const at = steps.indexOf(current);
    $('btnBack').hidden = at <= 0;
    const last = at === steps.length - 1;
    $('btnNext').textContent = last ? 'Finish' : 'Next';
    $('btnNext').disabled = current === 'welcome' && !choices.style;
    $('btnSkip').textContent = state && state.firstRun ? 'Skip setup' : 'Cancel';
    $('footerStatus').textContent = current === 'welcome' && !choices.style
        ? 'Choose one to continue.' : '';
}

$('btnNext').addEventListener('click', () => {
    const steps = visibleSteps();
    const at = steps.indexOf(current);
    if (at === steps.length - 1) return finish();
    go(steps[at + 1]);
});
$('btnBack').addEventListener('click', () => {
    const steps = visibleSteps();
    const at = steps.indexOf(current);
    if (at > 0) go(steps[at - 1]);
});
$('btnSkip').addEventListener('click', () => sendToAhk('skip'));
$('btnTitleClose').addEventListener('click', () => sendToAhk('close'));

function finish() {
    if (capturing) sendToAhk('cancel-capture');
    document.body.classList.add('applying');
    $('footerStatus').textContent = 'Applying…';
    sendToAhk('finish', { choices });
}

// ── Rendering ──────────────────────────────────────────────────

function renderAll() {
    renderWelcome();
    renderGame();
    renderMinimap();
    renderLaunching();
    renderFeatures();
    renderFinish();
}

function renderWelcome() {
    document.querySelectorAll('.choice-card').forEach(card => {
        card.setAttribute('aria-checked', card.dataset.style === choices.style ? 'true' : 'false');
    });
}
document.querySelectorAll('.choice-card').forEach(card =>
    card.addEventListener('click', () => setStyle(card.dataset.style)));

function renderGame() {
    const g = state.game;
    const pathEl = $('gamePath');
    pathEl.textContent = g.path || 'Not set';
    pathEl.title = g.path || '';
    pathEl.classList.toggle('is-empty', !g.path);
    const status = $('gameStatus');
    status.textContent = g.status || '';
    status.dataset.tone = g.tone || '';

    const list = $('candidates');
    list.innerHTML = '';
    const others = (g.candidates || []);
    $('candidatesWrap').hidden = others.length === 0;
    others.forEach(c => {
        const li = document.createElement('li');
        const inUse = g.path && c.path.toLowerCase() === g.path.toLowerCase();
        li.className = 'candidate' + (inUse ? ' in-use' : '');
        const src = document.createElement('span');
        src.className = 'source';
        src.textContent = c.source;
        const path = document.createElement('span');
        path.className = 'path';
        path.textContent = c.path;
        path.title = c.path;
        li.append(src, path);
        if (inUse) {
            const tag = document.createElement('span');
            tag.className = 'source';
            tag.textContent = 'In use';
            li.appendChild(tag);
        } else {
            const use = document.createElement('button');
            use.type = 'button';
            use.className = 'btn btn-sm';
            use.textContent = 'Use this';
            use.addEventListener('click', () => sendToAhk('use-game-path', { path: c.path }));
            li.appendChild(use);
        }
        list.appendChild(li);
    });
}
$('btnBrowse').addEventListener('click', () => sendToAhk('browse-game-path'));

function renderMinimap() {
    const input = document.querySelector(`input[name="source"][value="${choices.minimapSource}"]`);
    if (input) input.checked = true;
    // The extra key matters only where Tab goes to the game.
    $('openKeyCard').hidden = choices.minimapSource === 'mapspp';
    renderOpenKey();
}
document.querySelectorAll('input[name="source"]').forEach(input =>
    input.addEventListener('change', () => {
        choices.minimapSource = input.value;
        renderMinimap();
    }));

function renderOpenKey() {
    const chip = $('openKey');
    chip.classList.toggle('capturing', capturing);
    chip.textContent = capturing ? 'Press a key…' : (choices.openChordDisplay || 'None');
    $('btnSetKey').textContent = capturing ? 'Cancel' : 'Set key…';
    $('btnClearKey').hidden = capturing || !choices.openChord;
}
$('btnSetKey').addEventListener('click', () => {
    if (capturing) {
        sendToAhk('cancel-capture');
        capturing = false;
    } else {
        sendToAhk('start-capture');
        capturing = true;
    }
    renderOpenKey();
});
$('btnClearKey').addEventListener('click', () => {
    choices.openChord = '';
    choices.openChordDisplay = '';
    choices.openChordSet = true;
    renderOpenKey();
});

function fillSelect(id, labels, selected) {
    const sel = $(id);
    sel.innerHTML = '';
    labels.forEach((item, i) => {
        const opt = document.createElement('option');
        if (typeof item === 'string') {
            opt.value = String(i);
            opt.textContent = item;
        } else {
            opt.value = item.value;
            opt.textContent = item.label;
        }
        sel.appendChild(opt);
    });
    sel.value = String(selected);
}

function renderLaunching() {
    const l = state.launch;
    $('primaryCount').value = choices.primaryCount;
    fillSelect('primaryMonitor', l.monitors, choices.primaryMonitor);
    $('layoutField').hidden = !l.layoutAvailable;
    if (l.layoutAvailable) fillSelect('primaryLayout', l.layouts, choices.primaryLayout);

    $('secondEnabled').checked = choices.secondEnabled;
    $('secondFields').hidden = !choices.secondEnabled;
    $('secondCount').value = choices.secondCount;
    fillSelect('secondMonitor', l.monitors, choices.secondMonitor);

    const shortcuts = (state.finish && state.finish.shortcuts) || [];
    const launchKey = shortcuts.find(s => s.label === 'Launch');
    $('primaryChord').hidden = !launchKey;
    if (launchKey) $('primaryChord').textContent = launchKey.keys;

    // Auto-login needs the client mods' login module; say so rather than
    // offering a switch that would do nothing.
    const canLogin = l.accountsAvailable && l.modsInstalled;
    const autoLogin = $('autoLogin');
    autoLogin.checked = canLogin && choices.autoLogin;
    autoLogin.disabled = !canLogin;
    autoLogin.closest('.check-row').classList.toggle('disabled', !canLogin);
    $('autoLoginNote').textContent = !l.accountsAvailable
        ? 'Not in this build of Maps++.'
        : !l.modsInstalled
            ? (state.game.path ? 'Needs the osMW client mods in the game folder.'
                               : 'Set the game folder first, then this needs the osMW client mods.')
            : 'Each client takes the next saved account.';
    $('accountsRow').hidden = !canLogin || !choices.autoLogin;
    $('accountCount').textContent = l.accountCount > 0
        ? `${l.accountCount} account${l.accountCount === 1 ? '' : 's'} saved.`
        : 'No accounts saved yet.';
}
$('primaryCount').addEventListener('input', e => {
    const n = parseInt(e.target.value, 10);
    if (n >= 1) choices.primaryCount = Math.min(n, 32);
});
$('primaryMonitor').addEventListener('change', e => { choices.primaryMonitor = Number(e.target.value) || 0; });
$('primaryLayout').addEventListener('change', e => { choices.primaryLayout = e.target.value; });
$('secondEnabled').addEventListener('change', e => {
    choices.secondEnabled = e.target.checked;
    $('secondFields').hidden = !choices.secondEnabled;
});
$('secondCount').addEventListener('input', e => {
    const n = parseInt(e.target.value, 10);
    if (n >= 1) choices.secondCount = Math.min(n, 32);
});
$('secondMonitor').addEventListener('change', e => { choices.secondMonitor = Number(e.target.value) || 0; });
$('autoLogin').addEventListener('change', e => {
    choices.autoLogin = e.target.checked;
    $('accountsRow').hidden = !choices.autoLogin;
});
$('btnAccounts').addEventListener('click', () => sendToAhk('edit-accounts'));
// Back from the accounts editor: pick up how many were saved there.
window.addEventListener('focus', () => { if (state && current === 'launching') sendToAhk('refresh'); });

function renderFeatures() {
    const list = $('featureList');
    list.innerHTML = '';
    $('featuresLede').textContent = choices.style === 'solo'
        ? 'The multi-boxing features start off for one character. Turn on anything you want; its menu items and shortcuts come with it.'
        : "Turn off what you won't use. Its menu items and shortcuts go with it.";
    const known = new Set(FEATURE_GROUPS.map(([key]) => key));
    for (const [key, title] of FEATURE_GROUPS) {
        const members = state.features.filter(f => (known.has(f.group) ? f.group : 'tools') === key);
        if (!members.length) continue;
        const h = document.createElement('h2');
        h.textContent = title;
        list.appendChild(h);
        for (const f of members) {
            const row = document.createElement('label');
            row.className = 'feature';
            const cb = document.createElement('input');
            cb.type = 'checkbox';
            cb.className = 'toggle';
            cb.checked = !!choices.features[f.name];
            cb.addEventListener('change', () => { choices.features[f.name] = cb.checked; });
            const copy = document.createElement('span');
            copy.className = 'option-copy';
            const name = document.createElement('span');
            name.className = 'option-name';
            name.textContent = f.label;
            copy.appendChild(name);
            if (f.description) {
                const note = document.createElement('span');
                note.className = 'option-note';
                note.textContent = f.description;
                copy.appendChild(note);
            }
            row.append(cb, copy);
            list.appendChild(row);
        }
    }
}

function renderFinish() {
    const f = state.finish;
    const table = $('shortcuts');
    table.innerHTML = '';
    (f.shortcuts || []).forEach(s => {
        const tr = document.createElement('tr');
        const what = document.createElement('td');
        what.textContent = s.label;
        const keys = document.createElement('td');
        const kbd = document.createElement('span');
        kbd.className = 'kbd';
        kbd.textContent = s.keys;
        keys.appendChild(kbd);
        tr.append(what, keys);
        table.appendChild(tr);
    });

    // Enhanced is only offered when it can actually run here.
    document.querySelector('.inline-options').hidden = !f.runtimeAvailable;
    const mode = document.querySelector(`input[name="interface"][value="${choices.interfaceMode}"]`);
    if (mode) mode.checked = true;
    const accent = document.querySelector(`input[name="accent"][value="${choices.accent}"]`);
    if (accent) accent.checked = true;

    $('autoStart').checked = choices.autoStart;
    const canLaunch = !!state.game.path;
    $('launchNow').disabled = !canLaunch;
    $('launchNow').checked = canLaunch && choices.launchNow;
    $('launchNowRow').classList.toggle('disabled', !canLaunch);
    $('launchNowNote').textContent = canLaunch ? '' : 'Set the game folder first.';
}
document.querySelectorAll('input[name="interface"]').forEach(input =>
    input.addEventListener('change', () => { choices.interfaceMode = input.value; }));
document.querySelectorAll('input[name="accent"]').forEach(input =>
    input.addEventListener('change', () => {
        choices.accent = input.value;
        if (window.AccentTheme) window.AccentTheme.preview(input.value);
    }));
$('autoStart').addEventListener('change', e => { choices.autoStart = e.target.checked; });
$('launchNow').addEventListener('change', e => { choices.launchNow = e.target.checked; });

window.addEventListener('DOMContentLoaded', () => sendToAhk('init-request'));

// Local visual-regression fixture: open index.html?preview[=step] in a browser.
// WebView2 never enters this branch.
const previewStep = new URLSearchParams(location.search).get('preview');
if (previewStep !== null && !window.chrome?.webview) {
    onAhkMessage({ data: {
        type: 'setup-state', firstRun: true,
        game: {
            path: 'D:\\Games\\osMW\\main.exe',
            status: 'Found. The osMW client mods are installed in this folder.', tone: 'ok',
            candidates: [
                { path: 'D:\\Games\\osMW\\main.exe', source: 'Running now' },
                { path: 'E:\\MythWar\\main.exe', source: 'Beside Maps++' }
            ]
        },
        minimap: { source: 'game', openChord: '', openChordDisplay: '' },
        launch: {
            style: 'multi', primaryCount: 4, primaryMonitor: 0, primaryLayout: '__default__',
            secondEnabled: false, secondCount: 2, secondMonitor: 0,
            monitors: ['Auto', 'Display 1 — 2560×1440 (Primary)', 'Display 2 — 1920×1080'],
            layoutAvailable: true,
            layouts: [{ value: '', label: 'None — center clients only' },
                      { value: '__default__', label: 'Default — Grid 2×2' },
                      { value: 'Grid3x2', label: 'Grid 3×2' }],
            autoLogin: true, accountsAvailable: true, accountCount: 3, modsInstalled: true
        },
        features: [
            { name: 'MapPois', label: 'Map POIs', group: 'map', multibox: false, enabled: true, description: 'Mark NPCs, shops and portals on the Maps++ minimap, and share them.' },
            { name: 'PartyMarkers', label: 'Party markers', group: 'map', multibox: true, enabled: true, description: 'Shows your other clients as dots on the Maps++ minimap.' },
            { name: 'ClientRoster', label: 'Client roster', group: 'multibox', multibox: true, enabled: true, description: 'Your running clients in a ring at the cursor, to switch between them.' },
            { name: 'WindowLayout', label: 'Window layout', group: 'multibox', multibox: true, enabled: true, description: 'Arranges your clients across your displays with one key.' },
            { name: 'QuickActions', label: 'Action ring', group: 'tools', multibox: false, enabled: true, description: 'Your favourite actions in a ring at the cursor.' }
        ],
        finish: {
            interfaceMode: 'webview', runtimeAvailable: true, accent: 'amber', autoStart: false,
            shortcuts: [{ label: 'Minimap', keys: 'Tab' }, { label: 'Launch', keys: 'Ctrl+Alt+L' },
                        { label: 'Action ring', keys: 'Ctrl+Alt+A' }, { label: 'Settings', keys: 'Ctrl+Alt+,' }]
        }
    } });
    if (previewStep && STEPS.includes(previewStep)) {
        setStyle('multi');
        reached = STEPS.length;
        go(previewStep);
    }
}
