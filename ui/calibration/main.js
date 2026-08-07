/* Map Calibration panel — frontend logic.

   AHK owns everything: this page never computes a calibration, it renders the
   state AHK pushes (about four times a second) and sends back the two actions
   the user can take. Capturing points is a hotkey, not a button here — see the
   note in index.html. */

'use strict';

function sendToAhk(type, payload = {}) {
    if (window.chrome && window.chrome.webview) {
        window.chrome.webview.postMessage(JSON.stringify({ type, ...payload }));
    }
}

function onAhkMessage(event) {
    let msg = event.data;
    if (typeof msg === 'string') {
        try { msg = JSON.parse(msg); } catch (e) { return; }
    }
    if (!msg || typeof msg !== 'object') return;
    if (msg.type === 'calib-state') renderState(msg);
    else if (msg.type === 'toast') showToast(msg.level, msg.text);
}

if (window.chrome && window.chrome.webview) {
    window.chrome.webview.addEventListener('message', onAhkMessage);
}

window.addEventListener('DOMContentLoaded', () => {
    sendToAhk('init-request');
    document.getElementById('btnTitleClose').addEventListener('click', () => sendToAhk('close'));
    document.getElementById('btnApply').addEventListener('click', () => sendToAhk('apply'));
    document.getElementById('btnReset').addEventListener('click', () => sendToAhk('reset'));
    document.getElementById('btnMapsFolder').addEventListener('click',
        () => sendToAhk('open-maps-folder'));
});

function setText(id, text) {
    const el = document.getElementById(id);
    if (el) el.textContent = text;
}

/* The one thing standing in the way, if anything is. Ordered by how early it
   stops you: no map at all, then no image, then no minimap to point at. */
function prerequisite(s) {
    if (!s.mapName) {
        return 'Stand in a zone with the minimap open. Maps++ cannot tell which map to calibrate yet.';
    }
    if (!s.hasImage) {
        return `No image for ${s.mapName} in your maps folder, so there is nothing to calibrate against yet.`;
    }
    if (!s.overlayOpen) {
        return 'Open the minimap (Tab) — capturing a point reads where your mouse is over it.';
    }
    return '';
}

function renderState(s) {
    setText('mapName', s.mapName || '');

    const blocker = prerequisite(s);
    const prereq = document.getElementById('prereq');
    prereq.hidden = !blocker;
    prereq.textContent = blocker;

    setText('point1', s.p1 || 'not captured');
    setText('point2', s.p2 || 'not captured');
    document.getElementById('point1Row').classList.toggle('is-set', !!s.p1);
    document.getElementById('point2Row').classList.toggle('is-set', !!s.p2);

    setText('rawVal', s.rawText || '—');
    setText('gameVal', s.gameText || '—');
    setText('calVal', s.calText || 'none yet');

    const apply = document.getElementById('btnApply');
    apply.disabled = !s.canApply || !!blocker;
    const status = document.getElementById('applyStatus');
    status.textContent = s.canApply
        ? 'Both points captured — ready to apply.'
        : (s.blocker || '');
    status.classList.toggle('is-ready', !!s.canApply && !blocker);
}

let toastTimer = null;
function showToast(level, text) {
    const el = document.getElementById('toast');
    if (!el) return;
    el.textContent = text;
    el.className = 'toast is-' + (level || 'info');
    el.hidden = false;
    if (toastTimer) clearTimeout(toastTimer);
    // Long enough to read a sentence, since the apply result is a sentence.
    toastTimer = setTimeout(() => { el.hidden = true; }, 9000);
}
