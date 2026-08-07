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

/* A bar plus the numbers. Colour is a second signal only — the figures are
   always there, so a low-health row is not identified by hue alone.
   Health has no numbers at all until a client has been in a fight, and that
   reads as a dash rather than an empty bar, which would look like zero HP. */
function vitalCell(client, kind) {
    const td = document.createElement('td');
    td.className = 'vitals';
    const cur = kind === 'hp' ? client.hp : client.mp;
    const max = kind === 'hp' ? client.maxHp : client.maxMp;
    if (!client.hasVitals || !max || max <= 0) {
        td.textContent = '—';
        td.classList.add('vitals-none');
        return td;
    }
    const pct = Math.max(0, Math.min(100, Math.round((cur / max) * 100)));

    const bar = document.createElement('div');
    bar.className = 'vital-bar';
    const fill = document.createElement('span');
    fill.className = `vital-fill vital-${kind}` + (kind === 'hp' && pct <= 25 ? ' is-low' : '');
    fill.style.width = pct + '%';
    bar.appendChild(fill);

    const label = document.createElement('div');
    label.className = 'vital-text';
    label.textContent = `${cur.toLocaleString()} / ${max.toLocaleString()}`;

    td.appendChild(bar);
    td.appendChild(label);
    td.title = `${cur.toLocaleString()} / ${max.toLocaleString()} (${pct}%)`;
    return td;
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
        tr.tabIndex = 0;

        // Character column
        const charName = client.charName || `PID ${client.pid}`;
        const tdChar = document.createElement('td');
        tdChar.className = 'char-name cell-truncate';
        tdChar.textContent = charName;
        tdChar.title = charName;

        // Zone column
        const zoneText = client.zoneText || '—';
        const tdZone = document.createElement('td');
        tdZone.className = 'cell-truncate';
        tdZone.textContent = zoneText;
        tdZone.title = zoneText;

        // Status column
        const tdStatus = document.createElement('td');
        const badge = document.createElement('span');
        badge.className = `badge ${getStatusBadgeClass(client.statusText)}`;
        badge.textContent = client.statusText;
        tdStatus.appendChild(badge);

        tr.appendChild(tdChar);
        tr.appendChild(tdZone);
        tr.appendChild(tdStatus);
        tr.appendChild(vitalCell(client, 'hp'));
        tr.appendChild(vitalCell(client, 'mp'));

        const activate = () => sendToAhk('activate-client', { hwnd: client.hwnd });
        tr.addEventListener('dblclick', activate);
        tr.addEventListener('keydown', (e) => {
            if (e.key === 'Enter' || e.key === ' ') {
                e.preventDefault();
                activate();
            }
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
