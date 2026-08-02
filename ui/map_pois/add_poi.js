/* Add POI Dialog Frontend Logic */

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

    if (msg.type === 'init-add-poi') {
        document.getElementById('coordText').textContent = `${msg.zoneName} at ${msg.coordText}`;
        const sel = document.getElementById('kindSelect');
        sel.innerHTML = '';
        (msg.kinds || []).forEach(k => {
            const opt = document.createElement('option');
            opt.value = k;
            opt.textContent = k;
            if (k === msg.defaultKind) opt.selected = true;
            sel.appendChild(opt);
        });
        setTimeout(() => document.getElementById('labelInput').focus(), 50);
    }
}

if (window.chrome && window.chrome.webview) {
    window.chrome.webview.addEventListener('message', onAhkMessage);
}

document.getElementById('btnAdd').addEventListener('click', submitAdd);
document.getElementById('labelInput').addEventListener('keydown', (e) => {
    if (e.key === 'Enter') submitAdd();
    if (e.key === 'Escape') sendToAhk('cancel');
});

function submitAdd() {
    const label = document.getElementById('labelInput').value.trim();
    const kind = document.getElementById('kindSelect').value;
    if (!label) return;
    sendToAhk('accept', { label, kind });
}

document.getElementById('btnTitleClose').addEventListener('click', () => sendToAhk('cancel'));
document.getElementById('btnCancel').addEventListener('click', () => sendToAhk('cancel'));

window.addEventListener('DOMContentLoaded', () => {
    sendToAhk('init-request');
});
