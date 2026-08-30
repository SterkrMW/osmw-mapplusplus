/* Login roster editor.
 *
 * Host contract, matching the other Maps++ pages:
 *   host -> page   { type: 'login-roster', rows: [{id,account,character,enabled,hasPassword}] }
 *   page -> host   { type: 'login-roster-save', rows: [{id,account,character,enabled,password?}] }
 *                  { type: 'login-roster-request' }
 *                  { type: 'close' }
 *
 * Password is write-only: the host must never send it. The page only includes
 * `password` on a row when the user typed a new one.
 */

'use strict';

function sendToAhk(type, payload = {}) {
    if (window.chrome && window.chrome.webview) {
        window.chrome.webview.postMessage(JSON.stringify({ type, ...payload }));
    }
}

const $ = (id) => document.getElementById(id);

let rows = [];
let selected = -1;

function newId() {
    return 'xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx'.replace(/[xy]/g, (c) => {
        const r = Math.random() * 16 | 0;
        const v = c === 'x' ? r : (r & 0x3 | 0x8);
        return v.toString(16);
    });
}

function renderList() {
    const body = $('rosterBody');
    const empty = $('emptyState');
    body.innerHTML = '';
    empty.style.display = rows.length ? 'none' : 'block';

    rows.forEach((row, i) => {
        const tr = document.createElement('tr');
        tr.tabIndex = 0;
        if (i === selected)
            tr.classList.add('is-selected');

        const tdAcc = document.createElement('td');
        tdAcc.className = 'cell-truncate';
        tdAcc.textContent = row.account || '';
        tdAcc.title = row.account || '';

        const tdChar = document.createElement('td');
        tdChar.className = 'cell-truncate';
        tdChar.textContent = row.character || '#1';

        const tdOn = document.createElement('td');
        tdOn.textContent = row.enabled ? 'yes' : 'no';

        tr.appendChild(tdAcc);
        tr.appendChild(tdChar);
        tr.appendChild(tdOn);
        tr.addEventListener('click', () => selectRow(i));
        tr.addEventListener('keydown', (e) => {
            if (e.key === 'Enter' || e.key === ' ') {
                e.preventDefault();
                selectRow(i);
            }
        });
        body.appendChild(tr);
    });
}

function selectRow(i) {
    selected = i;
    const row = rows[i];
    if (!row) {
        $('account').value = '';
        $('password').value = '';
        $('character').value = '';
        $('enabled').checked = true;
        $('passwordHint').textContent = 'Write-only. Blank keeps the stored password.';
        renderList();
        return;
    }
    $('account').value = row.account || '';
    $('password').value = '';
    $('character').value = row.character || '';
    $('enabled').checked = !!row.enabled;
    $('passwordHint').textContent = row.hasPassword
        ? 'Stored. Leave blank to keep it, or type a new password.'
        : 'No password stored yet — type one before save.';
    renderList();
}

function readFields() {
    return {
        account: $('account').value.trim(),
        password: $('password').value,
        character: $('character').value.trim(),
        enabled: $('enabled').checked
    };
}

function applySelected() {
    if (selected < 0 || selected >= rows.length) {
        $('status').textContent = 'Select a row first, or click Add.';
        return false;
    }
    const f = readFields();
    if (!f.account) {
        $('status').textContent = 'Account cannot be empty.';
        return false;
    }
    const row = rows[selected];
    row.account = f.account;
    row.character = f.character || '#1';
    row.enabled = f.enabled;
    if (f.password) {
        row.password = f.password;
        row.hasPassword = true;
        $('password').value = '';
    }
    renderList();
    $('status').textContent = 'Row updated. Save to write the file.';
    return true;
}

function addRow() {
    const f = readFields();
    if (!f.account || !f.password) {
        $('status').textContent = 'New rows need an account and a password.';
        return;
    }
    if (rows.length >= 16) {
        $('status').textContent = 'At most 16 rows can be stored.';
        return;
    }
    rows.push({
        id: newId(),
        account: f.account,
        character: f.character || '#1',
        enabled: f.enabled,
        hasPassword: true,
        password: f.password
    });
    $('password').value = '';
    selectRow(rows.length - 1);
    $('status').textContent = 'Row added. Save to write the file.';
}

function removeRow() {
    if (selected < 0 || selected >= rows.length)
        return;
    rows.splice(selected, 1);
    selectRow(rows.length ? Math.min(selected, rows.length - 1) : -1);
    $('status').textContent = 'Row removed. Save to write the file.';
}

function moveRow(delta) {
    const j = selected + delta;
    if (selected < 0 || j < 0 || j >= rows.length)
        return;
    const tmp = rows[selected];
    rows[selected] = rows[j];
    rows[j] = tmp;
    selectRow(j);
}

function populate(incoming) {
    rows = (incoming || []).map((row) => ({
        id: row.id || newId(),
        account: row.account || '',
        character: row.character || '#1',
        enabled: row.enabled !== false && row.enabled !== 0 && row.enabled !== '0',
        hasPassword: !!row.hasPassword,
        password: undefined
    }));
    selectRow(rows.length ? 0 : -1);
}

function collect() {
    if (selected >= 0)
        applySelected();
    return rows.map((row) => {
        const out = {
            id: row.id,
            account: row.account,
            character: row.character || '#1',
            enabled: !!row.enabled
        };
        if (row.password)
            out.password = row.password;
        return out;
    });
}

function save() {
    const payload = collect();
    if (payload.some((row) => !row.account)) {
        $('status').textContent = 'Every row needs an account.';
        return;
    }
    sendToAhk('login-roster-save', { rows: payload });
    $('status').textContent = 'Saving…';
}

function onAhkMessage(event) {
    let msg = event.data;
    if (typeof msg === 'string') {
        try { msg = JSON.parse(msg); } catch (e) { return; }
    }
    if (!msg || typeof msg !== 'object')
        return;
    if (msg.type === 'login-roster')
        populate(msg.rows || []);
    else if (msg.type === 'save-result') {
        $('status').textContent = msg.message || (msg.ok ? 'Saved.' : 'Save failed.');
        if (msg.ok)
            rows.forEach((row) => { row.password = undefined; });
    }
}

if (window.chrome && window.chrome.webview) {
    window.chrome.webview.addEventListener('message', onAhkMessage);
}

$('btnAdd').addEventListener('click', addRow);
$('btnApply').addEventListener('click', applySelected);
$('btnRemove').addEventListener('click', removeRow);
$('btnUp').addEventListener('click', () => moveRow(-1));
$('btnDown').addEventListener('click', () => moveRow(1));
$('btnSave').addEventListener('click', save);
$('btnClose').addEventListener('click', () => sendToAhk('close'));
$('btnTitleClose').addEventListener('click', () => sendToAhk('close'));

populate([]);
sendToAhk('login-roster-request');
