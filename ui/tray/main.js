/* Custom Web Tray Menu Frontend Logic */

'use strict';

let menuData = [];
let activeItem = null;
let typeahead = '';
let typeaheadTimer = 0;

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
    activeItem = null;

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

    const container = document.getElementById('menuContainer');
    container.classList.remove('opening');
    void container.offsetWidth;
    container.classList.add('opening');
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
        el.className = `menu-item${item.isDefault ? ' default' : ''}${item.isExit ? ' exit' : ''}${item.isSelected ? ' selected' : ''}`;
        el.setAttribute('role', item.state ? 'menuitemcheckbox' : 'menuitem');
        if (item.state) el.setAttribute('aria-checked', item.state === 'On' ? 'true' : 'false');
        if (item.isSelected) el.setAttribute('aria-current', 'true');
        el.tabIndex = -1;
        el.dataset.label = String(item.label || '').toLocaleLowerCase();

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

        const right = document.createElement('div');
        right.className = 'item-right';

        if (item.state) {
            const stateEl = document.createElement('span');
            stateEl.className = `item-state ${item.state === 'On' ? 'is-on' : 'is-off'}`;
            stateEl.textContent = item.state;
            right.appendChild(stateEl);
        }

        if (item.isSelected) {
            const check = document.createElement('span');
            check.className = 'item-check';
            check.textContent = '✓';
            check.setAttribute('aria-hidden', 'true');
            right.appendChild(check);
        }

        if (item.shortcut) {
            const sc = document.createElement('span');
            sc.className = 'item-shortcut';
            sc.textContent = item.shortcut;
            right.appendChild(sc);
        } else if (hasChildren) {
            const arrow = document.createElement('span');
            arrow.className = 'item-arrow material-symbols-outlined';
            arrow.textContent = 'arrow_drop_down';
            right.appendChild(arrow);
        }

        if (right.childNodes.length) el.appendChild(right);

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
                const opening = !subContainer.classList.contains('open');
                if (opening) {
                    // Accordion behavior applies only to peers at this depth.
                    // Closing every expanded item in the document also closes
                    // an ancestor when a nested submenu is opened.
                    [...parentEl.children].filter(child =>
                        child.classList.contains('menu-item')
                        && child.classList.contains('has-submenu')
                        && child.classList.contains('expanded')
                    ).forEach(other => {
                        if (other === el) return;
                        other.classList.remove('expanded');
                        other.setAttribute('aria-expanded', 'false');
                        other.nextElementSibling?.classList.remove('open');
                    });
                }
                el.classList.toggle('expanded', opening);
                subContainer.classList.toggle('open', opening);
                el.setAttribute('aria-expanded', String(opening));
            };
            el.addEventListener('click', toggleSubmenu);
            el.addEventListener('keydown', (e) => {
                if (e.key === 'Enter' || e.key === ' ') {
                    e.preventDefault();
                    toggleSubmenu(e);
                }
            });
        } else if (item.id) {
            const executeItem = (e) => {
                e.stopPropagation();
                sendToAhk('execute-item', { id: item.id });
            };
            el.addEventListener('click', executeItem);
            el.addEventListener('keydown', (e) => {
                if (e.key === 'Enter' || e.key === ' ') {
                    e.preventDefault();
                    executeItem(e);
                }
            });
        }

        el.addEventListener('focus', () => setActiveItem(el, false));
        el.addEventListener('pointermove', () => setActiveItem(el, false));
    }
}

function visibleItems() {
    return [...document.querySelectorAll('.menu-item')]
        .filter(item => item.offsetParent !== null);
}

function setActiveItem(item, focus = true) {
    if (!item) return;
    if (activeItem && activeItem !== item) {
        activeItem.classList.remove('active');
        activeItem.tabIndex = -1;
    }
    activeItem = item;
    item.classList.add('active');
    if (focus) {
        item.tabIndex = 0;
        item.focus({ preventScroll: true });
        item.scrollIntoView({ block: 'nearest' });
    }
}

function moveActive(delta) {
    const items = visibleItems();
    if (!items.length) return;
    const current = items.indexOf(activeItem);
    const next = current < 0
        ? (delta > 0 ? 0 : items.length - 1)
        : (current + delta + items.length) % items.length;
    setActiveItem(items[next]);
}

function openActiveSubmenu() {
    if (!activeItem?.classList.contains('has-submenu')) return false;
    if (!activeItem.classList.contains('expanded')) activeItem.click();
    const firstChild = activeItem.nextElementSibling?.querySelector('.menu-item');
    if (firstChild) setActiveItem(firstChild);
    return true;
}

function closeActiveSubmenu() {
    if (activeItem?.classList.contains('has-submenu') && activeItem.classList.contains('expanded')) {
        activeItem.click();
        return true;
    }
    const submenu = activeItem?.closest('.submenu-list.open');
    const parent = submenu?.previousElementSibling;
    if (!submenu || !parent) return false;
    submenu.classList.remove('open');
    parent.classList.remove('expanded');
    parent.setAttribute('aria-expanded', 'false');
    setActiveItem(parent);
    return true;
}

// Global key handlers
window.addEventListener('keydown', (e) => {
    if (e.key === 'Escape') {
        sendToAhk('dismiss');
    } else if (e.key === 'ArrowDown') {
        e.preventDefault();
        moveActive(1);
    } else if (e.key === 'ArrowUp') {
        e.preventDefault();
        moveActive(-1);
    } else if (e.key === 'Home' || e.key === 'End') {
        e.preventDefault();
        const items = visibleItems();
        setActiveItem(e.key === 'Home' ? items[0] : items[items.length - 1]);
    } else if (e.key === 'ArrowRight') {
        e.preventDefault();
        openActiveSubmenu();
    } else if (e.key === 'ArrowLeft') {
        if (closeActiveSubmenu()) e.preventDefault();
    } else if (e.key.length === 1 && !e.ctrlKey && !e.altKey && !e.metaKey) {
        clearTimeout(typeaheadTimer);
        typeahead += e.key.toLocaleLowerCase();
        const items = visibleItems();
        const match = items.find(item => item.dataset.label.startsWith(typeahead))
            || items.find(item => item.dataset.label.startsWith(e.key.toLocaleLowerCase()));
        if (match) setActiveItem(match);
        typeaheadTimer = setTimeout(() => { typeahead = ''; }, 650);
    }
});

window.addEventListener('DOMContentLoaded', () => {
    sendToAhk('init-request');
});

// Local visual-regression fixture. WebView2 never enters this branch.
const previewMode = new URLSearchParams(location.search).get('preview');
if (previewMode !== null && !window.chrome?.webview) {
    document.documentElement.classList.add('preview-mode');
    // Mirrors RebuildTrayMenu's full-build tree. Keep the two in step.
    onAhkMessage({ data: { type: 'tray-menu-state', items: [
        { id: 'launch-primary', label: 'Launch Primary', icon: 'rocket_launch', shortcut: 'Ctrl+Alt+L', isDefault: true },
        { id: 'launch-secondary', label: 'Launch Alts on the second screen', icon: 'rocket_launch', shortcut: 'Ctrl+Alt+K' },
        { label: 'Launch other', icon: 'format_list_bulleted', children: [
            { id: 'profile-trade', label: 'Trading pair — 2 clients' },
            { isDivider: true },
            { id: 'group-a', label: 'Group: Team A — 5 accounts' }
        ] },
        { isDivider: true },
        { label: 'Minimap', icon: 'map', children: [
            { id: 'open-map', label: 'Open Maps++ minimap' },
            { id: 'calibrate', label: 'Calibrate this map…' },
            { isDivider: true },
            { id: 'pois', label: 'Show POIs', state: 'On' },
            { id: 'add-poi', label: 'Add POI here' },
            { id: 'manage-pois', label: 'Manage POIs…' },
            { isDivider: true },
            { id: 'markers', label: 'Party markers', state: 'Off' }
        ] },
        { label: 'Clients', icon: 'group', children: [
            { id: 'roster', label: 'Client roster' },
            { id: 'health', label: 'Party health', shortcut: 'Ctrl+Alt+H' },
            { isDivider: true },
            { id: 'arrange', label: 'Arrange windows' },
            { id: 'undo-arrange', label: 'Undo arrange' },
            { id: 'layouts', label: 'Window layouts…' },
            { isDivider: true },
            { id: 'chat-others', label: 'Toggle chat on other clients', shortcut: 'Ctrl+Shift+C' },
            { id: 'chat-this', label: 'Toggle chat on this client', shortcut: 'Ctrl+C' },
            { id: 'chat-size', label: 'Toggle chat size', shortcut: 'Ctrl+Alt+C' },
            { isDivider: true },
            { id: 'accounts', label: 'Accounts…' }
        ] },
        { label: 'Actions', icon: 'tune', children: [
            { id: 'ring', label: 'Action ring', shortcut: 'Ctrl+Alt+A' },
            { isDivider: true },
            { id: 'ready', label: 'Send Enter until ready', shortcut: 'Ctrl+Alt+E' },
            { id: 'battle', label: 'Battle action', shortcut: 'Alt+Shift+Q' },
            { id: 'inventory', label: 'Open inventory', shortcut: 'Alt+E' },
            { id: 'view', label: 'Toggle view mode' },
            { isDivider: true },
            { id: 'vendor', label: 'Character vendor…' },
            { id: 'skills', label: 'Skill hotkeys…' }
        ] },
        { isDivider: true },
        { id: 'settings', label: 'Settings…', icon: 'settings', shortcut: 'Ctrl+Alt+,' },
        { label: 'Help', icon: 'help', children: [
            { id: 'setup', label: 'Setup guide…' },
            { isDivider: true },
            { id: 'diag', label: 'Copy diagnostics' },
            { id: 'logs', label: 'Open log folder' },
            { id: 'mods', label: 'Client mods status…' },
            { id: 'mod-log', label: 'Client mod logging', state: 'Off' },
            { isDivider: true },
            { id: 'about', label: 'About Maps++…' }
        ] },
        { isDivider: true },
        { id: 'exit', label: 'Exit', icon: 'power_settings_new', shortcut: 'Ctrl+Alt+Q', isExit: true }
    ] } });
    if (previewMode === 'expanded' || previewMode === 'nested') {
        document.querySelector('.menu-item.has-submenu')?.click();
    }
    if (previewMode === 'nested') {
        document.querySelector('.submenu-list.open > .menu-item.has-submenu')?.click();
    }
}
