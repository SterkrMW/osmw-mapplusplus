/* Login roster editor.
 *
 * Host contract, matching the other Maps++ pages:
 *   host -> page   { type: 'login-roster', rows: [{id,account,character,group,enabled,hasPassword}] }
 *   page -> host   { type: 'login-roster-save',
 *                    rows: [{id,account,character,group,enabled,password?,passwordFrom?}] }
 *                  { type: 'login-roster-request' }
 *                  { type: 'close' }
 *
 * Password is write-only: the host must never send it. The page only includes
 * `password` on a row when the user typed a new one.
 *
 * `passwordFrom` is how Duplicate works without one. The page cannot read a
 * stored password back, so a duplicated row names the row to copy the blob from
 * and the host does the copying — which is the whole point, since five
 * characters on one account are five rows and retyping the password five times
 * is five chances to typo something nobody can ever read back to check.
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

        const tdGroup = document.createElement('td');
        tdGroup.className = 'cell-truncate';
        tdGroup.textContent = row.group || '';
        tdGroup.title = row.group || '';

        const tdOn = document.createElement('td');
        tdOn.textContent = row.enabled ? 'yes' : 'no';

        tr.appendChild(tdAcc);
        tr.appendChild(tdChar);
        tr.appendChild(tdGroup);
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

function renderGroupOptions() {
    const list = $('groupNames');
    const seen = [];
    rows.forEach((row) => {
        const name = (row.group || '').trim();
        if (name && !seen.includes(name))
            seen.push(name);
    });
    list.innerHTML = '';
    seen.forEach((name) => {
        const opt = document.createElement('option');
        opt.value = name;
        list.appendChild(opt);
    });
}

function selectRow(i) {
    selected = i;
    const row = rows[i];
    if (!row) {
        $('account').value = '';
        $('password').value = '';
        $('character').value = '';
        $('group').value = '';
        $('enabled').checked = true;
        $('passwordHint').textContent = 'Write-only. Blank keeps the stored password.';
        renderList();
        renderGroupOptions();
        return;
    }
    $('account').value = row.account || '';
    $('password').value = '';
    $('character').value = row.character || '';
    $('group').value = row.group || '';
    $('enabled').checked = !!row.enabled;
    $('passwordHint').textContent = (row.hasPassword || row.passwordFrom)
        ? 'Stored. Leave blank to keep it, or type a new password.'
        : 'No password stored yet — type one before save.';
    renderList();
    renderGroupOptions();
}

function readFields() {
    return {
        account: $('account').value.trim(),
        password: $('password').value,
        character: $('character').value.trim(),
        group: $('group').value.trim(),
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
    row.group = f.group;
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
        group: f.group,
        enabled: f.enabled,
        hasPassword: true,
        password: f.password
    });
    $('password').value = '';
    selectRow(rows.length - 1);
    $('status').textContent = 'Row added. Save to write the file.';
}

/* The usual reason for a second row is the same account's next character, so
 * the slot advances by one. Anything that is not a #N slot is copied verbatim —
 * a character ID or a name has no "next". */
function nextCharacterSlot(character) {
    const m = /^#(\d+)$/.exec((character || '').trim());
    if (!m)
        return character || '#1';
    const n = parseInt(m[1], 10) + 1;
    return n <= 5 ? '#' + n : character;
}

function duplicateRow() {
    if (selected < 0 || selected >= rows.length) {
        $('status').textContent = 'Select a row to duplicate first.';
        return;
    }
    if (rows.length >= 16) {
        $('status').textContent = 'At most 16 rows can be stored.';
        return;
    }
    const src = rows[selected];
    rows.splice(selected + 1, 0, {
        id: newId(),
        account: src.account,
        character: nextCharacterSlot(src.character),
        group: src.group || '',
        enabled: src.enabled,
        hasPassword: true,
        /* The page never sees the stored password, so it names the row to copy
         * the blob from and lets the host do it. A row still unsaved is handled
         * too — the host follows passwordFrom back to whoever typed one. */
        password: src.password || undefined,
        passwordFrom: src.password ? undefined : (src.passwordFrom || src.id)
    });
    selectRow(selected + 1);
    $('status').textContent = 'Duplicated — set the character, then Apply and Save.';
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
        group: row.group || '',
        enabled: row.enabled !== false && row.enabled !== 0 && row.enabled !== '0',
        hasPassword: !!row.hasPassword,
        password: undefined,
        passwordFrom: undefined
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
            group: row.group || '',
            enabled: !!row.enabled
        };
        if (row.password)
            out.password = row.password;
        else if (row.passwordFrom)
            out.passwordFrom = row.passwordFrom;
        return out;
    });
}

// Launches act on the saved roster, not this draft: AHK resolves the row by id
// from the file, and refuses with a "save first" note if it is not there yet.
function launch(mode) {
    if (selected < 0 || selected >= rows.length) {
        $('status').textContent = 'Select a row to launch.';
        return;
    }
    sendToAhk('launch', { mode, id: rows[selected].id, which: $('launchWhich').value });
    $('status').textContent = 'Launching…';
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
        if (msg.ok) {
            /* The host has written the blobs, so a duplicate's borrowed
             * password is now its own — clearing both keeps a later save from
             * re-borrowing from a row that may since have been removed. */
            rows.forEach((row) => {
                row.password = undefined;
                row.passwordFrom = undefined;
            });
        }
    }
}

if (window.chrome && window.chrome.webview) {
    window.chrome.webview.addEventListener('message', onAhkMessage);
}

$('btnAdd').addEventListener('click', addRow);
$('btnApply').addEventListener('click', applySelected);
$('btnDuplicate').addEventListener('click', duplicateRow);
$('btnRemove').addEventListener('click', removeRow);
$('btnUp').addEventListener('click', () => moveRow(-1));
$('btnDown').addEventListener('click', () => moveRow(1));
$('btnSave').addEventListener('click', save);
$('btnLaunchOnly').addEventListener('click', () => launch('only'));
$('btnLaunchFrom').addEventListener('click', () => launch('start'));
$('btnClose').addEventListener('click', () => sendToAhk('close'));
$('btnTitleClose').addEventListener('click', () => sendToAhk('close'));

populate([]);
sendToAhk('login-roster-request');
