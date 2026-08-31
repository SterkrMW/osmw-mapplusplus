/* Chat tab editor.
 *
 * Host contract:
 *   host -> page   { type: 'init-tabs', rows, channels, layouts,
 *                    maxTabs, nameMax, notice }
 *                  { type: 'set-rows',  rows }
 *                  { type: 'tabs-preview', spec, length, max, error }
 *   page -> host   { type: 'init-request' }
 *                  { type: 'preview', rows }
 *                  { type: 'layout',  index }   1-based
 *                  { type: 'accept',  rows }
 *                  { type: 'cancel' }
 *
 * A row is { name, channels: [bool x channels.length] }.
 *
 * The page deliberately does NOT know the tab-spec language. It never builds
 * "All:*|Chat:*,-system,-info" and never measures it: it sends rows and is
 * told what they came to. There is one formatter and one validator, they are
 * in addons\chat_mod.ahk, and they are the same ones the OK button runs — so
 * the preview cannot promise something the save would not produce.
 */

'use strict';

function sendToAhk(type, payload = {}) {
    if (window.chrome && window.chrome.webview) {
        window.chrome.webview.postMessage(JSON.stringify({ type, ...payload }));
    }
}

const $ = (id) => document.getElementById(id);

let channels = [];
let maxTabs = 6;
let nameMax = 15;
let ready = false;

/* Ask the host what the current rows come to. Debounced because it fires on
   every keystroke and each one is a cross-process message. */
let previewTimer = 0;
function requestPreview() {
    if (!ready) return;
    clearTimeout(previewTimer);
    previewTimer = setTimeout(() => {
        sendToAhk('preview', { rows: readRows() });
        renderStrip();
    }, 90);
}

function readRows() {
    const rows = [];
    for (let r = 0; r < maxTabs; r++) {
        const name = $(`name-${r}`).value.trim();
        const ticks = channels.map((_, c) => $(`ch-${r}-${c}`).checked);
        if (!name && !ticks.some(Boolean)) continue;
        rows.push({ name, channels: ticks });
    }
    return rows;
}

function writeRows(rows) {
    for (let r = 0; r < maxTabs; r++) {
        const row = rows[r];
        $(`name-${r}`).value = row ? row.name : '';
        channels.forEach((_, c) => {
            $(`ch-${r}-${c}`).checked = !!(row && row.channels[c]);
        });
    }
    requestPreview();
}

/* The strip as the chat window draws it, so a choice can be judged by looking
   rather than by reading a string. */
function renderStrip() {
    const strip = $('strip');
    strip.innerHTML = '';
    readRows().forEach((row, i) => {
        const tab = document.createElement('span');
        tab.className = i === 0 ? 'strip-tab strip-tab-active' : 'strip-tab';
        tab.textContent = row.name || '(unnamed)';
        strip.appendChild(tab);
    });
}

function buildGrid() {
    /* Rebuilt, not appended to: init-tabs arrives twice on a normal open (the
       page asks on load, and the host also pushes on DOMContentLoaded), and
       appending gave two sets of channel headers. */
    const head = $('gridHead');
    head.innerHTML = '';
    const nameHead = document.createElement('th');
    nameHead.className = 'col-name';
    nameHead.textContent = 'Tab name';
    head.appendChild(nameHead);
    channels.forEach((ch) => {
        const th = document.createElement('th');
        th.textContent = ch;
        head.appendChild(th);
    });

    const body = $('gridBody');
    body.innerHTML = '';
    for (let r = 0; r < maxTabs; r++) {
        const tr = document.createElement('tr');

        const nameCell = document.createElement('td');
        nameCell.className = 'col-name';
        const input = document.createElement('input');
        input.className = 'input';
        input.id = `name-${r}`;
        input.maxLength = nameMax;
        input.spellcheck = false;
        input.placeholder = r === 0 ? 'All' : '';
        input.addEventListener('input', requestPreview);
        nameCell.appendChild(input);
        tr.appendChild(nameCell);

        channels.forEach((ch, c) => {
            const td = document.createElement('td');
            const cb = document.createElement('input');
            cb.type = 'checkbox';
            cb.className = 'toggle-box';
            cb.id = `ch-${r}-${c}`;
            cb.title = `${ch} on tab ${r + 1}`;
            cb.addEventListener('change', requestPreview);
            td.appendChild(cb);
            tr.appendChild(td);
        });

        body.appendChild(tr);
    }
}

function init(msg) {
    channels = msg.channels || [];
    maxTabs = msg.maxTabs || 6;
    nameMax = msg.nameMax || 15;

    buildGrid();

    const sel = $('layout');
    sel.innerHTML = '';
    (msg.layouts || []).forEach((layout, i) => {
        sel.add(new Option(layout.label, String(i + 1)));
    });

    if (msg.notice) {
        $('notice').textContent = msg.notice;
        $('notice').hidden = false;
    }

    ready = true;
    writeRows(msg.rows || []);
}

function onAhkMessage(event) {
    let msg = event.data;
    if (typeof msg === 'string') {
        try { msg = JSON.parse(msg); } catch (e) { return; }
    }
    if (!msg || typeof msg !== 'object') return;

    if (msg.type === 'init-tabs') {
        init(msg);
    } else if (msg.type === 'set-rows') {
        writeRows(msg.rows || []);
    } else if (msg.type === 'tabs-preview') {
        const error = $('error');
        error.hidden = !msg.error;
        error.textContent = msg.error || '';
        $('btnOk').disabled = !!msg.error;
        $('budget').textContent = `${msg.length} of ${msg.max} characters`;
    }
}

if (window.chrome && window.chrome.webview) {
    window.chrome.webview.addEventListener('message', onAhkMessage);
}

$('btnLayout').addEventListener('click', () => {
    sendToAhk('layout', { index: parseInt($('layout').value, 10) });
});
$('btnOk').addEventListener('click', () => {
    sendToAhk('accept', { rows: readRows() });
});
$('btnCancel').addEventListener('click', () => sendToAhk('cancel'));
$('btnTitleClose').addEventListener('click', () => sendToAhk('cancel'));

sendToAhk('init-request');
