/* Shared modal dialog — page side.

   AHK is blocked in a Sleep loop waiting for this page to answer, so the two
   things that must never fail are: report a height (or the window never comes
   into view), and send exactly one result (or the caller waits forever). AHK
   has a deadline behind both, but hitting it means the user sees a
   default-sized window or a stuck-looking panel, so treat them as contracts.

   The token echoed on every message is what lets one long-lived window serve
   every dialog: a reply from a dialog that has already been answered arrives
   with a stale token and AHK drops it. */

'use strict';

const el = {
    glyph: document.getElementById('glyph'),
    title: document.getElementById('title'),
    text: document.getElementById('text'),
    body: document.getElementById('body'),
    field: document.getElementById('field'),
    inputLabel: document.getElementById('inputLabel'),
    input: document.getElementById('input'),
    detailWrap: document.getElementById('detailWrap'),
    detail: document.getElementById('detail'),
    btnOk: document.getElementById('btnOk'),
    btnCancel: document.getElementById('btnCancel'),
    btnCopy: document.getElementById('btnCopy'),
    btnClose: document.getElementById('btnClose'),
};

const GLYPHS = { info: 'i', warn: '!', danger: '!', success: '✓', ask: '?' };

let token = 0;
let kind = 'say';
let answered = false;

function send(type, payload = {}) {
    if (window.chrome && window.chrome.webview) {
        window.chrome.webview.postMessage(JSON.stringify({ type, token, ...payload }));
    }
}

function show(spec) {
    token = spec.token || 0;
    kind = spec.kind || 'say';
    answered = false;

    const severity = GLYPHS[spec.severity] ? spec.severity : 'info';
    el.glyph.className = 'title-bar-icon dlg-glyph is-' + severity;
    el.glyph.textContent = GLYPHS[severity];
    el.title.textContent = spec.title || 'Maps++';
    el.text.textContent = spec.text || '';

    el.field.hidden = kind !== 'prompt';
    el.inputLabel.hidden = !spec.inputLabel;
    el.inputLabel.textContent = spec.inputLabel || '';
    el.input.value = spec.value || '';
    el.input.placeholder = spec.placeholder || '';

    el.detailWrap.hidden = !spec.detail;
    el.detail.textContent = spec.detail || '';
    el.detail.scrollTop = 0;

    el.btnOk.textContent = spec.okLabel || 'OK';
    el.btnOk.className = 'btn ' + (spec.danger ? 'btn-danger' : 'btn-primary');
    el.btnCancel.textContent = spec.cancelLabel || 'Cancel';
    // A message has one way out, so a second button would only be a second
    // name for the same thing.
    el.btnCancel.hidden = kind === 'say';

    el.body.scrollTop = 0;
    focusFirst(spec);
}

/* Reads the height this dialog wants. See body.measuring in style.css for why
   the class is needed: measured without it, the answer is the height of the
   parked window rather than the height of the content. */
function measure() {
    document.body.classList.add('measuring');
    const h = Math.ceil(document.body.scrollHeight);
    document.body.classList.remove('measuring');
    send('dlg-size', { h });
}

/* Inter is a local font but still loads asynchronously, and measuring against
   the fallback metrics leaves the window a few pixels short of its text. Wait
   for the fonts, but never past the point where AHK gives up and shows the
   window at its default height anyway. */
function measureWhenSettled() {
    let done = false;
    const once = () => {
        if (done) return;
        done = true;
        measure();
    };
    if (document.fonts && document.fonts.ready) {
        document.fonts.ready.then(() => requestAnimationFrame(once));
    }
    setTimeout(once, 200);
}

/* The window is activated by AHK only after this page has reported its size,
   so the focus set here happens while the window is still parked and inactive.
   Re-applying it on the window's own focus event is what makes it stick. */
let focusTarget = null;

function focusFirst(spec) {
    // Focus lands on the safe answer for a destructive confirm, the same
    // reason MsgBox got "Default2" at these call sites.
    focusTarget = kind === 'prompt' ? el.input
        : (spec.danger && !el.btnCancel.hidden) ? el.btnCancel
            : el.btnOk;
    applyFocus();
}

function applyFocus() {
    if (!focusTarget) return;
    focusTarget.focus();
    if (focusTarget === el.input) el.input.select();
}

window.addEventListener('focus', () => setTimeout(applyFocus, 0));

function answer(result) {
    if (answered) return;
    answered = true;
    send('dlg-result', { result, value: el.input.value });
}

el.btnOk.addEventListener('click', () => answer('ok'));
el.btnCancel.addEventListener('click', () => answer('cancel'));
// Closing a message is the only thing it can do, so it agrees rather than
// returning a cancel the caller never asked about.
el.btnClose.addEventListener('click', () => answer(kind === 'say' ? 'ok' : 'cancel'));

el.btnCopy.addEventListener('click', () => {
    send('dlg-copy', { text: el.detail.textContent });
    el.btnCopy.textContent = 'Copied';
    setTimeout(() => { el.btnCopy.textContent = 'Copy'; }, 1200);
});

document.addEventListener('keydown', (e) => {
    if (e.key === 'Escape') {
        e.preventDefault();
        answer(kind === 'say' ? 'ok' : 'cancel');
        return;
    }
    if (e.key === 'Enter') {
        // Enter on a focused button is that button's own click; only take over
        // when it would otherwise do nothing.
        if (document.activeElement && document.activeElement.tagName === 'BUTTON') return;
        e.preventDefault();
        answer('ok');
    }
});

function onAhkMessage(event) {
    let msg = event.data;
    if (typeof msg === 'string') {
        try { msg = JSON.parse(msg); } catch (err) { return; }
    }
    if (!msg || typeof msg !== 'object') return;
    if (msg.type === 'dlg-show') {
        show(msg);
        measureWhenSettled();
    }
}

if (window.chrome && window.chrome.webview) {
    window.chrome.webview.addEventListener('message', onAhkMessage);
}
