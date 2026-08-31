/* Blocked-name editor.
 *
 * Host contract:
 *   host -> page   { type: 'init-block', names, nameMax, maxNames, notice }
 *   page -> host   { type: 'init-request' }
 *                  { type: 'accept', names }
 *                  { type: 'cancel' }
 *
 * The page never writes the ini string. It sends names; AHK validates and
 * serialises, the same functions the native editor uses.
 */

'use strict';

function sendToAhk(type, payload = {}) {
    if (window.chrome && window.chrome.webview) {
        window.chrome.webview.postMessage(JSON.stringify({ type, ...payload }));
    }
}

const $ = (id) => document.getElementById(id);

let names = [];
let nameMax = 20;
let maxNames = 32;

function render() {
    const list = $('list');
    list.innerHTML = '';
    names.forEach((name, i) => {
        const li = document.createElement('li');
        li.className = 'name-row';
        const label = document.createElement('span');
        label.textContent = name;
        const btn = document.createElement('button');
        btn.type = 'button';
        btn.className = 'btn btn-sm';
        btn.textContent = 'Remove';
        btn.addEventListener('click', () => {
            names.splice(i, 1);
            render();
        });
        li.appendChild(label);
        li.appendChild(btn);
        list.appendChild(li);
    });
    list.hidden = names.length === 0;
    $('empty').hidden = names.length !== 0;
    $('budget').textContent = `${names.length} of ${maxNames}`;
    $('error').hidden = true;
    $('btnAdd').disabled = names.length >= maxNames;
}

function addName() {
    const input = $('nameInput');
    const n = input.value.trim();
    $('error').hidden = true;
    if (!n) {
        input.focus();
        return;
    }
    if (n.includes(',')) {
        $('error').textContent = 'Names cannot contain commas.';
        $('error').hidden = false;
        return;
    }
    if (n.length > nameMax) {
        $('error').textContent = `Names can be at most ${nameMax} characters.`;
        $('error').hidden = false;
        return;
    }
    if (names.some((x) => x.toLowerCase() === n.toLowerCase())) {
        $('error').textContent = `"${n}" is already on the list.`;
        $('error').hidden = false;
        return;
    }
    if (names.length >= maxNames) {
        $('error').textContent = `At most ${maxNames} names.`;
        $('error').hidden = false;
        return;
    }
    names.push(n);
    input.value = '';
    render();
    input.focus();
}

function init(msg) {
    nameMax = msg.nameMax || 20;
    maxNames = msg.maxNames || 32;
    names = Array.isArray(msg.names) ? msg.names.slice() : [];
    $('nameInput').maxLength = nameMax;
    if (msg.notice) {
        $('notice').textContent = msg.notice;
        $('notice').hidden = false;
    }
    render();
}

function onAhkMessage(event) {
    let msg = event.data;
    if (typeof msg === 'string') {
        try { msg = JSON.parse(msg); } catch (e) { return; }
    }
    if (!msg || typeof msg !== 'object') return;
    if (msg.type === 'init-block')
        init(msg);
}

if (window.chrome && window.chrome.webview) {
    window.chrome.webview.addEventListener('message', onAhkMessage);
}

$('btnAdd').addEventListener('click', addName);
$('nameInput').addEventListener('keydown', (e) => {
    if (e.key === 'Enter') {
        e.preventDefault();
        addName();
    }
});
$('btnOk').addEventListener('click', () => sendToAhk('accept', { names }));
$('btnCancel').addEventListener('click', () => sendToAhk('cancel'));
$('btnTitleClose').addEventListener('click', () => sendToAhk('cancel'));

sendToAhk('init-request');
