/* Chat mod settings.
 *
 * Host contract, matching the other Maps++ pages:
 *   host -> page   { type: 'chat-settings', settings: { ...ini keys... } }
 *   page -> host   { type: 'chat-settings-save', settings: { ... } }
 *                  { type: 'close' }
 *
 * The keys are exactly the [Chat] names in config.ini, so the host can write
 * them straight through without a translation table. The DLL picks changes up
 * from its own settings poll, which is why saving needs no restart and no
 * message to the game.
 */

'use strict';

function sendToAhk(type, payload = {}) {
    if (window.chrome && window.chrome.webview) {
        window.chrome.webview.postMessage(JSON.stringify({ type, ...payload }));
    }
}

/* Every preset is either at or below the face's measured natural height, or
   pinned to the client's 8px cell. All fixed-pitch: a proportional face can
   overflow on wide capitals even below its natural height. Measured with
   patches/chatmod/tools/fontpresets.py — keep the two in step. */
/* The client imposes two ceilings, both found by testing against the real
   renderer: text must fit an 8px character cell or lines run off the right
   edge, and the font must be no taller than MAX_HEIGHT or the bottom of every
   line is cut. Underscores go first, before descenders do. */
const MAX_HEIGHT = 18;

const PRESETS = {
    stock: { FontFace: '',              FontSize: 0,  FontWidth: -1,
             hint: "The client's own Courier New at 16px." },
    crisp: { FontFace: 'Consolas',      FontSize: 16, FontWidth: -1,
             hint: 'Same size as default, cleaner glyphs.' },
    large: { FontFace: 'Consolas',      FontSize: 18, FontWidth: -1,
             hint: 'The largest the client draws in full.' },
    wide:  { FontFace: 'Cascadia Mono', FontSize: 18, FontWidth: -1,
             hint: 'Same size, a wider-looking face. Needs Cascadia Mono.' }
};

const CHANNELS = ['local', 'party', 'pm', 'guild', 'market', 'world',
                  'system', 'info', 'fd', 'gm'];

const $ = (id) => document.getElementById(id);

function presetMatching(face, size, width) {
    return Object.keys(PRESETS).find((name) => {
        const p = PRESETS[name];
        return p.FontFace === face && p.FontSize === size && p.FontWidth === width;
    });
}

function applyPresetToFields(name) {
    const p = PRESETS[name];
    if (!p) return;
    $('fontFace').value = p.FontFace;
    $('fontSize').value = String(p.FontSize);
    $('fontWidth').value = String(p.FontWidth);
}

function refreshFontUi() {
    const name = $('preset').value;
    const custom = name === 'custom';
    $('customFont').hidden = !custom;
    $('presetHint').textContent = custom
        ? 'Set the font yourself. Read the note below first.'
        : (PRESETS[name] ? PRESETS[name].hint : '');
    if (custom) validateCustomFont();
}

/* Warn rather than block: the size limit depends on the face, and someone who
   knows their font may well be right. */
function validateCustomFont() {
    const size = parseInt($('fontSize').value, 10) || 0;
    const width = parseInt($('fontWidth').value, 10);
    const warn = $('customWarning');

    if (size > MAX_HEIGHT) {
        warn.hidden = false;
        warn.className = 'note note-warn';
        warn.textContent = 'Above ' + MAX_HEIGHT + 'px the client cuts the '
            + 'bottom of every line. Underscores disappear first, then the '
            + 'tails of y, g and q. No setting avoids this.';
    } else if (size > MAX_HEIGHT && width !== 8) {
        warn.hidden = false;
        warn.className = 'note note-warn';
        warn.textContent = 'At this size text will probably run off the right '
            + 'edge. Set the character width to 8 to narrow the glyphs instead.';
    } else {
        warn.hidden = true;
    }
}

function validateTabs() {
    const spec = $('tabs').value.trim();
    const error = $('tabsError');
    if (!spec) { error.hidden = true; return true; }

    const bad = [];
    spec.split('|').forEach((tab) => {
        const colon = tab.indexOf(':');
        if (colon < 1) { bad.push(`"${tab.trim()}" has no name:channels`); return; }
        tab.slice(colon + 1).split(',').forEach((item) => {
            const cleaned = item.trim().replace(/^-/, '');
            if (cleaned && cleaned !== '*' && !CHANNELS.includes(cleaned.toLowerCase())) {
                bad.push(`unknown channel "${cleaned}"`);
            }
        });
    });

    error.hidden = bad.length === 0;
    error.textContent = bad.join('; ');
    return bad.length === 0;
}

/* The DLL feeds this string to the CRT's sprintf with a fixed argument list:
   an int, then two strings. Warn rather than block, as with the font size —
   but the warning has to be read, because the wrong specifier order here is
   passed straight through to a printf inside the game process. */
function validateCooldownFormat() {
    const spec = $('cooldownFormat').value;
    const error = $('cooldownError');
    const found = spec.match(/%[a-zA-Z]/g) || [];
    const problems = [];

    if (found.join(',') !== '%d,%s,%s') {
        problems.push(found.length === 0
            ? 'No placeholders, so the message will never say a number.'
            : 'Placeholders must be %d, then %s, then %s — found '
              + found.join(' ') + '.');
    }

    error.hidden = problems.length === 0;
    error.textContent = problems.join(' ');
    return problems.length === 0;
}

/* Pick an option, keeping a hand-edited ini value that is not in the list
   rather than silently rewriting it to the first entry on the next save. */
function setSelect(id, value) {
    const el = $(id);
    if (![...el.options].some((o) => o.value === value)) {
        el.add(new Option(value + ' (from config.ini)', value));
    }
    el.value = value;
}

function populate(settings) {
    const s = settings || {};
    const face = s.FontFace || '';
    const size = parseInt(s.FontSize, 10) || 0;
    const width = s.FontWidth === undefined ? -1 : parseInt(s.FontWidth, 10);

    $('preset').value = presetMatching(face, size, width) || 'custom';
    $('fontFace').value = face;
    $('fontSize').value = String(size);
    $('fontWidth').value = String(width);

    $('tabs').value = s.Tabs || '';
    $('tabStrip').checked = s.TabStrip !== '0';
    $('tabHotkey').value = s.TabHotkey || '';

    $('timestamps').checked = s.Timestamps === '1';
    setSelect('timestampFormat', s.TimestampFormat || '#c808080[%H:%M]#N');

    $('wheelScroll').checked = s.WheelScroll !== '0';
    $('wheelScrollLines').value = String(parseInt(s.WheelScrollLines, 10) || 0);

    $('cooldownMessage').checked = s.CooldownMessage !== '0';
    $('cooldownFormat').value = s.CooldownFormat
        || '#R#S Wait %d more second%s before posting in %s again!#N';

    $('highlightMentions').checked = s.HighlightMentions !== '0';
    $('myName').value = s.MyName || '';

    $('freePosition').checked = s.FreePosition === '1';
    $('unlockSize').checked = s.UnlockSize === '1';
    $('rememberGeometry').checked = s.RememberGeometry !== '0';
    $('clearHotkey').value = s.ClearHotkey || '';

    refreshFontUi();
    validateCooldownFormat();
}

function collect() {
    const name = $('preset').value;
    if (name !== 'custom') applyPresetToFields(name);

    return {
        FontFace: $('fontFace').value.trim(),
        FontSize: String(parseInt($('fontSize').value, 10) || 0),
        FontWidth: String(parseInt($('fontWidth').value, 10)),
        Tabs: $('tabs').value.trim(),
        TabStrip: $('tabStrip').checked ? '1' : '0',
        TabHotkey: $('tabHotkey').value.trim(),
        Timestamps: $('timestamps').checked ? '1' : '0',
        TimestampFormat: $('timestampFormat').value,
        WheelScroll: $('wheelScroll').checked ? '1' : '0',
        WheelScrollLines: String(parseInt($('wheelScrollLines').value, 10) || 0),
        CooldownMessage: $('cooldownMessage').checked ? '1' : '0',
        CooldownFormat: $('cooldownFormat').value.trim(),
        HighlightMentions: $('highlightMentions').checked ? '1' : '0',
        MyName: $('myName').value.trim(),
        FreePosition: $('freePosition').checked ? '1' : '0',
        UnlockSize: $('unlockSize').checked ? '1' : '0',
        RememberGeometry: $('rememberGeometry').checked ? '1' : '0',
        ClearHotkey: $('clearHotkey').value.trim()
    };
}

function save() {
    if (!validateTabs()) {
        $('status').textContent = 'Fix the tab set before saving.';
        return;
    }
    /* Blocking, unlike the font-size warning: this string is handed to a
       sprintf inside the game process, so a mismatched specifier list is a
       crash rather than something that merely looks wrong. */
    if (!validateCooldownFormat()) {
        $('status').textContent = 'Fix the cooldown wording before saving.';
        return;
    }
    sendToAhk('chat-settings-save', { settings: collect() });
    $('status').textContent = 'Saved. Applies within a couple of seconds.';
}

function onAhkMessage(event) {
    let msg = event.data;
    if (typeof msg === 'string') {
        try { msg = JSON.parse(msg); } catch (e) { return; }
    }
    if (!msg || typeof msg !== 'object') return;
    if (msg.type === 'chat-settings') populate(msg.settings);
}

if (window.chrome && window.chrome.webview) {
    window.chrome.webview.addEventListener('message', onAhkMessage);
}

$('preset').addEventListener('change', refreshFontUi);
['fontSize', 'fontWidth'].forEach((id) =>
    $(id).addEventListener('input', validateCustomFont));
$('tabs').addEventListener('input', validateTabs);
$('cooldownFormat').addEventListener('input', validateCooldownFormat);
$('btnSave').addEventListener('click', save);
$('btnClose').addEventListener('click', () => sendToAhk('close'));
$('btnTitleClose').addEventListener('click', () => sendToAhk('close'));

/* Standalone in a browser, with no host to populate us. */
populate({});
sendToAhk('chat-settings-request');
