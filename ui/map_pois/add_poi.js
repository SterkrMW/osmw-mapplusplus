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
        const coordVal = document.getElementById('coordValue');
        if (coordVal && msg.zoneName && msg.coordText) {
            coordVal.textContent = `${msg.zoneName} at ${msg.coordText}`;
        }
        const sel = document.getElementById('kindSelect');
        if (sel && Array.isArray(msg.kinds) && msg.kinds.length > 0) {
            sel.innerHTML = '';
            msg.kinds.forEach(k => {
                const opt = document.createElement('option');
                opt.value = k;
                opt.textContent = k.charAt(0).toUpperCase() + k.slice(1);
                if (k === msg.defaultKind) opt.selected = true;
                sel.appendChild(opt);
            });
            if (msg.defaultKind) sel.value = msg.defaultKind;
        }
        setTimeout(() => {
            const input = document.getElementById('labelInput');
            if (input) input.focus();
        }, 50);
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
document.getElementById('labelInput').addEventListener('input', (e) => {
    e.target.classList.remove('input-invalid');
});

function submitAdd() {
    const btn = document.getElementById('btnAdd');
    if (btn.disabled) return; // already submitted — AHK is closing this window

    const input = document.getElementById('labelInput');
    const label = input.value.trim();
    const kind = document.getElementById('kindSelect').value;
    if (!label) {
        input.classList.remove('input-invalid');
        void input.offsetWidth; // restart the animation if it's already showing
        input.classList.add('input-invalid');
        input.focus();
        return;
    }
    btn.disabled = true;
    sendToAhk('accept', { label, kind });
}

document.getElementById('btnTitleClose').addEventListener('click', () => sendToAhk('cancel'));
document.getElementById('btnCancel').addEventListener('click', () => sendToAhk('cancel'));

window.addEventListener('DOMContentLoaded', () => {
    sendToAhk('init-request');
});
