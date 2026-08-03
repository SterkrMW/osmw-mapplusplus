/* Map POIs Manager Frontend Logic */

'use strict';

let allPois = [];
let selectedIndex = -1;
let currentMapName = '';
let availableKinds = [];

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

    if (msg.type === 'pois-state') {
        currentMapName = msg.zoneName || 'Map';
        allPois = msg.pois || [];
        availableKinds = msg.kinds || ['npc', 'shop', 'portal', 'note'];
        document.getElementById('winTitle').textContent = `Map POIs — ${currentMapName}`;
        renderPois();
        populateKindSelect();
    } else if (msg.type === 'prompt-add-poi') {
        openAddModal(msg.zoneName, msg.gameX, msg.gameY, msg.defaultKind);
    }
}

if (window.chrome && window.chrome.webview) {
    window.chrome.webview.addEventListener('message', onAhkMessage);
}

function renderPois() {
    const tbody = document.getElementById('poiBody');
    const empty = document.getElementById('emptyState');
    const search = document.getElementById('searchInput').value.toLowerCase().trim();
    tbody.innerHTML = '';
    selectedIndex = -1;

    const filtered = allPois.filter(p => {
        if (!search) return true;
        return p.label.toLowerCase().includes(search) || p.kind.toLowerCase().includes(search);
    });

    if (filtered.length === 0) {
        empty.style.display = 'block';
        return;
    }
    empty.style.display = 'none';

    filtered.forEach((poi, index) => {
        const tr = document.createElement('tr');
        tr.dataset.realIndex = poi.index; // 1-based index from AHK

        const tdLabel = document.createElement('td');
        tdLabel.textContent = poi.label;

        const tdType = document.createElement('td');
        const badge = document.createElement('span');
        badge.className = `badge badge-${poi.kind.toLowerCase()}`;
        badge.textContent = poi.kind;
        tdType.appendChild(badge);

        const tdX = document.createElement('td');
        tdX.textContent = poi.gameX;

        const tdY = document.createElement('td');
        tdY.textContent = poi.gameY;

        tr.appendChild(tdLabel);
        tr.appendChild(tdType);
        tr.appendChild(tdX);
        tr.appendChild(tdY);

        tr.addEventListener('click', () => {
            document.querySelectorAll('#poiBody tr').forEach(r => r.classList.remove('selected'));
            tr.classList.add('selected');
            selectedIndex = poi.index;
        });

        tbody.appendChild(tr);
    });
}

function populateKindSelect() {
    const sel = document.getElementById('modalKindSelect');
    sel.innerHTML = '';
    availableKinds.forEach(k => {
        const opt = document.createElement('option');
        opt.value = k;
        opt.textContent = k;
        sel.appendChild(opt);
    });
}

// Search
document.getElementById('searchInput').addEventListener('input', renderPois);

// Delete
document.getElementById('btnDeleteSelected').addEventListener('click', () => {
    if (selectedIndex < 1) return;
    sendToAhk('delete-poi', { index: selectedIndex });
});

// Export
document.getElementById('btnExport').addEventListener('click', () => {
    sendToAhk('export-map');
});

// Add POI Modal
document.getElementById('btnAddPoiHeader').addEventListener('click', () => {
    sendToAhk('request-add-poi');
});

function openAddModal(zoneName, x, y, defaultKind) {
    document.getElementById('modalCoordText').textContent = `${zoneName} at ${x}, ${y}`;
    document.getElementById('modalLabelInput').value = '';
    if (defaultKind) document.getElementById('modalKindSelect').value = defaultKind;
    document.getElementById('addPoiModal').style.display = 'flex';
    document.addEventListener('keydown', onModalKeydown);
    document.addEventListener('focus', trapModalFocus, true);
    setTimeout(() => document.getElementById('modalLabelInput').focus(), 50);
}

document.getElementById('btnModalClose').addEventListener('click', closeAddModal);
document.getElementById('btnModalCancel').addEventListener('click', closeAddModal);

function closeAddModal() {
    document.getElementById('addPoiModal').style.display = 'none';
    document.removeEventListener('keydown', onModalKeydown);
    document.removeEventListener('focus', trapModalFocus, true);
}

function onModalKeydown(e) {
    if (e.key === 'Escape') closeAddModal();
}

// Tab never leaves the modal while it's open — the modal is a sibling of the
// table, not a native <dialog>, so nothing else stops focus escaping into it.
function trapModalFocus(e) {
    const modal = document.getElementById('addPoiModal');
    if (!modal.contains(e.target)) {
        document.getElementById('modalLabelInput').focus();
    }
}

document.getElementById('btnModalAdd').addEventListener('click', () => {
    const label = document.getElementById('modalLabelInput').value.trim();
    const kind = document.getElementById('modalKindSelect').value;
    if (!label) return;
    sendToAhk('submit-add-poi', { label, kind });
    closeAddModal();
});

// Title & Close
document.getElementById('btnTitleClose').addEventListener('click', () => sendToAhk('close'));
document.getElementById('btnClose').addEventListener('click', () => sendToAhk('close'));

window.addEventListener('DOMContentLoaded', () => {
    sendToAhk('init-request');
});
