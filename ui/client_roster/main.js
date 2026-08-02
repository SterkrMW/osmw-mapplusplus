/* Client Roster Frontend Logic */

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

    if (msg.type === 'snapshot-update') {
        renderRoster(msg.clients || []);
    }
}

if (window.chrome && window.chrome.webview) {
    window.chrome.webview.addEventListener('message', onAhkMessage);
}

function renderRoster(clients) {
    const tbody = document.getElementById('rosterBody');
    const empty = document.getElementById('emptyState');
    tbody.innerHTML = '';

    if (clients.length === 0) {
        empty.style.display = 'block';
        return;
    }
    empty.style.display = 'none';

    for (const client of clients) {
        const tr = document.createElement('tr');
        tr.dataset.hwnd = client.hwnd;

        // Character column
        const tdChar = document.createElement('td');
        tdChar.className = 'char-name';
        tdChar.textContent = client.charName || `PID ${client.pid}`;

        // Zone column
        const tdZone = document.createElement('td');
        tdZone.textContent = client.zoneText || '—';

        // Status column
        const tdStatus = document.createElement('td');
        const badge = document.createElement('span');
        badge.className = `badge ${getStatusBadgeClass(client.statusText)}`;
        badge.textContent = client.statusText;
        tdStatus.appendChild(badge);

        tr.appendChild(tdChar);
        tr.appendChild(tdZone);
        tr.appendChild(tdStatus);

        tr.addEventListener('dblclick', () => {
            sendToAhk('activate-client', { hwnd: client.hwnd });
        });

        tbody.appendChild(tr);
    }
}

function getStatusBadgeClass(status) {
    switch (status) {
        case 'Playing':   return 'badge-success';
        case 'In battle': return 'badge-warning';
        case 'Loading':   return 'badge-info';
        case 'Not ready': return 'badge-muted';
        default:          return 'badge-danger';
    }
}

document.getElementById('btnTitleClose').addEventListener('click', () => sendToAhk('close'));
document.getElementById('btnClose').addEventListener('click', () => sendToAhk('close'));

window.addEventListener('DOMContentLoaded', () => {
    sendToAhk('init-request');
});
