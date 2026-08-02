/* Custom Web Tray Menu Frontend Logic */

'use strict';

let menuData = [];

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

    if (msg.type === 'tray-menu-state') {
        menuData = msg.items || [];
        renderMenu(menuData);
    }
}

if (window.chrome && window.chrome.webview) {
    window.chrome.webview.addEventListener('message', onAhkMessage);
}

function renderMenu(items) {
    const container = document.getElementById('menuContainer');
    container.innerHTML = '';
    buildItemsList(items, container);
}

function buildItemsList(items, parentEl, isSubmenu = false) {
    for (const item of items) {
        if (item.isDivider) {
            const div = document.createElement('div');
            div.className = 'menu-divider';
            parentEl.appendChild(div);
            continue;
        }

        const el = document.createElement('div');
        el.className = `menu-item ${item.isDefault ? 'default' : ''}`;

        const left = document.createElement('div');
        left.className = 'item-left';

        if (item.icon && !isSubmenu) {
            const icon = document.createElement('span');
            icon.className = 'material-symbols-outlined item-icon';
            icon.textContent = item.icon;
            left.appendChild(icon);
        }

        const label = document.createElement('span');
        label.className = 'item-label';
        label.textContent = item.label;
        left.appendChild(label);
        el.appendChild(left);

        const hasChildren = item.children && item.children.length > 0;

        if (item.shortcut) {
            const sc = document.createElement('span');
            sc.className = 'item-shortcut';
            sc.textContent = item.shortcut;
            el.appendChild(sc);
        } else if (hasChildren) {
            const arrow = document.createElement('span');
            arrow.className = 'item-arrow material-symbols-outlined';
            arrow.textContent = 'arrow_drop_down';
            el.appendChild(arrow);
        }

        parentEl.appendChild(el);

        if (hasChildren) {
            el.classList.add('has-submenu');
            const subContainer = document.createElement('div');
            subContainer.className = 'submenu-list';
            buildItemsList(item.children, subContainer, true);
            parentEl.appendChild(subContainer);

            el.addEventListener('click', (e) => {
                e.stopPropagation();
                el.classList.toggle('expanded');
                subContainer.classList.toggle('open');
            });
        } else if (item.id) {
            el.addEventListener('click', (e) => {
                e.stopPropagation();
                sendToAhk('execute-item', { id: item.id });
            });
        }
    }
}

// Global key handlers
window.addEventListener('keydown', (e) => {
    if (e.key === 'Escape') {
        sendToAhk('dismiss');
    }
});

window.addEventListener('DOMContentLoaded', () => {
    sendToAhk('init-request');
});
