/* Radial client switcher frontend logic
 *
 * Mouse input is handled by AHK, not by this page. The window is colour-keyed
 * transparent, which makes it click-through at the OS level — no mouse event
 * ever reaches the WebView, so :hover and click listeners here would never
 * fire. Instead the page measures its own layout and posts a hit map; AHK
 * hit-tests the cursor against it, drives highlighting with `hover` messages
 * and acts on clicks itself. Keyboard still arrives normally.
 */

'use strict';

/* Spokes sit on a ring around the window centre; the radius tightens for small
   rosters so two clients don't end up absurdly far apart. */
const RADIUS_SMALL = 118;   // <= 4 clients
const RADIUS_LARGE = 132;   // 5+

let hoverIndex = 0;         // 0 none, -1 hub, 1-based spoke index

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

    if (msg.type === 'radial-update') {
        renderRadial(msg.clients || []);
    } else if (msg.type === 'hover') {
        applyHover(msg.index || 0);
    }
}

if (window.chrome && window.chrome.webview) {
    window.chrome.webview.addEventListener('message', onAhkMessage);
}

function initialsFor(name) {
    const parts = String(name || '').trim().split(/\s+/).filter(Boolean);
    if (parts.length === 0) return '?';
    if (parts.length === 1) return parts[0].slice(0, 2);
    return parts[0][0] + parts[1][0];
}

function buildAvatar(client) {
    const frame = document.createElement('span');
    frame.className = 'avatar-frame';

    const fallback = () => {
        frame.innerHTML = '';
        // Initials need something behind them; a portrait does not.
        frame.classList.add('is-initials');
        const initials = document.createElement('span');
        initials.className = 'avatar-initials';
        initials.textContent = initialsFor(client.charName);
        frame.appendChild(initials);
    };

    // classId is -1 when the class could not be read or the client is still
    // sitting at the login screen.
    if (typeof client.classId !== 'number' || client.classId < 0) {
        fallback();
        return frame;
    }

    const img = document.createElement('img');
    img.alt = '';
    img.addEventListener('error', fallback);
    img.src = `../../avatars/c${client.classId}.png`;
    frame.appendChild(img);
    return frame;
}

function applyHover(index) {
    hoverIndex = index;
    document.querySelectorAll('.spoke-btn').forEach((btn, i) => {
        btn.classList.toggle('is-hover', i + 1 === index);
    });
    document.getElementById('hub').classList.toggle('is-hover', index === -1);
}

function renderRadial(clients) {
    const radial = document.getElementById('radial');
    const ring = document.getElementById('ring');
    const hubNum = document.getElementById('hubNum');
    const hubSub = document.getElementById('hubSub');

    ring.innerHTML = '';
    hoverIndex = 0;
    radial.classList.toggle('is-empty', clients.length === 0);

    if (clients.length === 0) {
        hubNum.textContent = 'No clients';
        hubSub.textContent = 'running';
    } else {
        hubNum.textContent = String(clients.length);
        hubSub.textContent = clients.length === 1 ? 'client' : 'clients';
    }

    const radius = clients.length <= 4 ? RADIUS_SMALL : RADIUS_LARGE;

    clients.forEach((client, i) => {
        // First client at 12 o'clock, then clockwise.
        const angle = (-90 + (i * 360) / clients.length) * (Math.PI / 180);

        const spoke = document.createElement('div');
        spoke.className = 'spoke';
        spoke.style.setProperty('--x', `${Math.cos(angle) * radius}px`);
        spoke.style.setProperty('--y', `${Math.sin(angle) * radius}px`);

        const btn = document.createElement('button');
        btn.className = 'spoke-btn' + (client.isActive ? ' active' : '');
        btn.style.setProperty('--i', String(i));
        btn.title = client.charName;

        const name = document.createElement('span');
        name.className = 'spoke-name';
        name.textContent = client.charName;

        btn.appendChild(buildAvatar(client));
        btn.appendChild(name);

        spoke.appendChild(btn);
        ring.appendChild(spoke);
    });

    postHitMap(clients);
}

/* Everything here is deliberately measured off layout, never off
   getBoundingClientRect of an animating element: the spokes bloom in with a
   scale animation, so the button's rect is wrong until it settles, and AHK
   needs the hit map immediately. The `.spoke` wrapper carries only the static
   positioning transform, and offsetWidth ignores transforms entirely. */
function postHitMap(clients) {
    const hub = document.getElementById('hub');
    const spokes = [];

    document.querySelectorAll('.spoke').forEach((spoke, i) => {
        if (!clients[i]) return;
        const rect = spoke.getBoundingClientRect();
        const frame = spoke.querySelector('.avatar-frame');
        const d = frame.offsetWidth || 72;
        spokes.push({
            hwnd: clients[i].hwnd,
            cx: rect.left + rect.width / 2,   // column is centre-aligned
            cy: rect.top + d / 2,             // avatar is the first row
            r: d / 2
        });
    });

    sendToAhk('hit-map', {
        vw: window.innerWidth,
        vh: window.innerHeight,
        hubR: (hub.offsetWidth || 92) / 2,    // the hub is exactly centred
        spokes: spokes
    });
}

window.addEventListener('keydown', (e) => {
    if (e.key === 'Escape') {
        sendToAhk('dismiss');
    }
});

window.addEventListener('contextmenu', (e) => e.preventDefault());

window.addEventListener('DOMContentLoaded', () => {
    sendToAhk('init-request');
});
