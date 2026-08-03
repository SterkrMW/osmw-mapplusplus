/* Radial ring frontend logic
 *
 * One page, driven by whatever items AHK sends: a spoke is either an `avatar`
 * (a class portrait, for the client switcher) or an `icon` (a Material Symbols
 * glyph, for the quick-actions ring). Everything else — geometry, the bloom,
 * the hit map — is shared, which is the point of having one page.
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
   rings so two items don't end up absurdly far apart, and opens up for big
   ones so the faces don't collide. At 420px square with a 72px face, 150 is
   still comfortably inside the window (150 + 36 = 186 < 210). */
const RADIUS_SMALL = 118;   // <= 4 items
const RADIUS_LARGE = 132;   // 5-8
const RADIUS_XL = 150;      // 9+

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

    if (msg.type === 'radial-items') {
        renderRing(msg.items || [], msg.hub || null);
    } else if (msg.type === 'hover') {
        applyHover(msg.index || 0);
    }
}

if (window.chrome && window.chrome.webview) {
    window.chrome.webview.addEventListener('message', onAhkMessage);
}

/* An avatar spoke: the portrait if we have one, initials if the class could
   not be read or the image is missing. Initials arrive precomputed from AHK —
   the page does no name parsing of its own. */
function buildAvatarFace(item) {
    const frame = document.createElement('span');
    frame.className = 'face-frame avatar-frame';

    const fallback = () => {
        frame.innerHTML = '';
        // Initials need something behind them; a portrait does not.
        frame.classList.add('is-initials');
        const initials = document.createElement('span');
        initials.className = 'avatar-initials';
        initials.textContent = item.initials || '?';
        frame.appendChild(initials);
    };

    if (!item.image) {
        fallback();
        return frame;
    }

    const img = document.createElement('img');
    img.alt = '';
    img.addEventListener('error', fallback);
    img.src = item.image;
    frame.appendChild(img);
    return frame;
}

/* An icon spoke, optionally carrying a small on/off badge for actions whose
   current state the app already knows. */
function buildIconFace(item) {
    const frame = document.createElement('span');
    frame.className = 'face-frame icon-frame';

    const glyph = document.createElement('span');
    glyph.className = 'material-symbols-outlined spoke-icon';
    glyph.textContent = item.icon || 'pin_drop';
    frame.appendChild(glyph);

    if (item.state === 'on' || item.state === 'off') {
        const badge = document.createElement('span');
        badge.className = 'state-badge is-' + item.state;
        frame.appendChild(badge);
    }
    return frame;
}

function buildFace(item) {
    return item.kind === 'avatar' ? buildAvatarFace(item) : buildIconFace(item);
}

function applyHover(index) {
    hoverIndex = index;
    document.querySelectorAll('.spoke-btn').forEach((btn, i) => {
        btn.classList.toggle('is-hover', i + 1 === index);
    });
    document.getElementById('hub').classList.toggle('is-hover', index === -1);
}

function renderHub(hub) {
    const el = document.getElementById('hub');
    const enabled = !hub || hub.enabled !== false;

    document.getElementById('hubTitle').textContent = hub ? (hub.title || '') : '';
    document.getElementById('hubSub').textContent = hub ? (hub.sub || '') : '';
    document.getElementById('hubHoverIcon').textContent = hub ? (hub.hoverIcon || '') : '';
    document.getElementById('hubHoverSub').textContent = hub ? (hub.hoverSub || '') : '';

    el.hidden = !hub;
    el.classList.toggle('is-static', !enabled);
    // Long words ("No clients") need to drop a size to fit the 92px disc.
    el.classList.toggle('is-wordy', !!(hub && hub.title && hub.title.length > 3));
    return enabled && !!hub;
}

function renderRing(items, hub) {
    const ring = document.getElementById('ring');

    ring.innerHTML = '';
    hoverIndex = 0;

    const hubHittable = renderHub(hub);

    const radius = items.length <= 4 ? RADIUS_SMALL
        : items.length <= 8 ? RADIUS_LARGE
        : RADIUS_XL;

    items.forEach((item, i) => {
        // First item at 12 o'clock, then clockwise.
        const angle = (-90 + (i * 360) / items.length) * (Math.PI / 180);

        const spoke = document.createElement('div');
        spoke.className = 'spoke';
        spoke.style.setProperty('--x', `${Math.cos(angle) * radius}px`);
        spoke.style.setProperty('--y', `${Math.sin(angle) * radius}px`);

        const btn = document.createElement('button');
        btn.className = 'spoke-btn' + (item.active ? ' active' : '');
        btn.style.setProperty('--i', String(i));
        btn.title = item.tooltip || item.label || '';

        const name = document.createElement('span');
        name.className = 'spoke-name';
        name.textContent = item.label || '';

        btn.appendChild(buildFace(item));
        btn.appendChild(name);

        spoke.appendChild(btn);
        ring.appendChild(spoke);
    });

    postHitMap(items, hubHittable);
}

/* Everything here is deliberately measured off layout, never off
   getBoundingClientRect of an animating element: the spokes bloom in with a
   scale animation, so the button's rect is wrong until it settles, and AHK
   needs the hit map immediately. The `.spoke` wrapper carries only the static
   positioning transform, and offsetWidth ignores transforms entirely. */
function postHitMap(items, hubHittable) {
    const hub = document.getElementById('hub');
    const spokes = [];

    document.querySelectorAll('.spoke').forEach((spoke, i) => {
        if (!items[i]) return;
        const rect = spoke.getBoundingClientRect();
        const frame = spoke.querySelector('.face-frame');
        const d = frame.offsetWidth || 72;
        spokes.push({
            key: items[i].key,
            cx: rect.left + rect.width / 2,   // column is centre-aligned
            cy: rect.top + d / 2,             // the face is the first row
            r: d / 2
        });
    });

    sendToAhk('hit-map', {
        vw: window.innerWidth,
        vh: window.innerHeight,
        // 0 disables hub hit-testing entirely; the hub is exactly centred.
        hubR: hubHittable ? (hub.offsetWidth || 92) / 2 : 0,
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
