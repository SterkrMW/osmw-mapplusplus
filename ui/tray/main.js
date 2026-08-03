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
        // AHK keeps this window parked off-screen and only moves it into view
        // once the items are on the page, so it never flashes an empty menu.
        sendToAhk('rendered');
    }
}

if (window.chrome && window.chrome.webview) {
    window.chrome.webview.addEventListener('message', onAhkMessage);
}

function renderMenu(items) {
    const menuItems = document.getElementById('menuItems');
    const menuFooter = document.getElementById('menuFooter');
    menuItems.innerHTML = '';
    menuFooter.innerHTML = '';

    const mainItems = [...items];
    const exitIndex = mainItems.findIndex((item) => item.isExit);
    if (exitIndex >= 0) {
        const exitItem = mainItems.splice(exitIndex, 1)[0];
        if (exitIndex > 0 && mainItems[exitIndex - 1]?.isDivider) {
            mainItems.splice(exitIndex - 1, 1);
        }
        buildItemsList([exitItem], menuFooter);
        menuFooter.hidden = false;
    } else {
        menuFooter.hidden = true;
    }

    buildItemsList(mainItems, menuItems);
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
        el.className = `menu-item ${item.isDefault ? 'default' : ''} ${item.isExit ? 'exit' : ''}`;
        el.setAttribute('role', 'menuitem');
        el.tabIndex = 0;

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
            el.setAttribute('aria-haspopup', 'menu');
            el.setAttribute('aria-expanded', 'false');
            const subContainer = document.createElement('div');
            subContainer.className = 'submenu-list';
            subContainer.setAttribute('role', 'menu');
            buildItemsList(item.children, subContainer, true);
            parentEl.appendChild(subContainer);

            const toggleSubmenu = (e) => {
                e.stopPropagation();
                el.classList.toggle('expanded');
                subContainer.classList.toggle('open');
                el.setAttribute('aria-expanded', String(subContainer.classList.contains('open')));
            };
            el.addEventListener('click', toggleSubmenu);
            el.addEventListener('keydown', (e) => {
                if (e.key === 'Enter' || e.key === ' ') toggleSubmenu(e);
            });
        } else if (item.id) {
            const executeItem = (e) => {
                e.stopPropagation();
                sendToAhk('execute-item', { id: item.id });
            };
            el.addEventListener('click', executeItem);
            el.addEventListener('keydown', (e) => {
                if (e.key === 'Enter' || e.key === ' ') executeItem(e);
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
