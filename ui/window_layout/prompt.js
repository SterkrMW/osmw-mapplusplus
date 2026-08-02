/* Main Character Prompt JS */

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

    if (msg.type === 'init-prompt') {
        const sel = document.getElementById('charSelect');
        sel.innerHTML = '';
        (msg.names || []).forEach(name => {
            const opt = document.createElement('option');
            opt.value = name;
            opt.textContent = name;
            if (name === msg.selectedName) opt.selected = true;
            sel.appendChild(opt);
        });
        setTimeout(() => sel.focus(), 50);
    }
}

if (window.chrome && window.chrome.webview) {
    window.chrome.webview.addEventListener('message', onAhkMessage);
}

document.getElementById('btnOk').addEventListener('click', submitSelection);
document.getElementById('charSelect').addEventListener('keydown', (e) => {
    if (e.key === 'Enter') submitSelection();
    if (e.key === 'Escape') sendToAhk('cancel');
});

function submitSelection() {
    const sel = document.getElementById('charSelect').value;
    sendToAhk('accept', { chosen: sel });
}

document.getElementById('btnTitleClose').addEventListener('click', () => sendToAhk('cancel'));
document.getElementById('btnCancel').addEventListener('click', () => sendToAhk('cancel'));

window.addEventListener('DOMContentLoaded', () => {
    sendToAhk('init-request');
});
