'use strict';

let profiles = [];
let classes = [];
let clients = [];
let selectedProfile = -1;
let dirty = false;
let captureTarget = null;
let captureNotice = '';
let lastBound = '';

const $ = id => document.getElementById(id);

function send(type, payload = {}) {
    if (window.chrome?.webview) {
        window.chrome.webview.postMessage(JSON.stringify({ type, ...payload }));
    }
}

function cloneProfiles(source) {
    return source.map(profile => ({
        name: profile.name,
        classId: Number(profile.classId),
        bindings: (profile.bindings || []).map(binding => ({
            chord: binding.chord || '',
            actor: binding.actor || 'player',
            skill: Number(binding.skill)
        }))
    }));
}

function markDirty(next = true) {
    dirty = next;
    $('saveProfiles').disabled = !dirty;
    $('saveState').textContent = dirty ? 'Unsaved changes' : 'No unsaved changes';
    $('saveState').classList.toggle('dirty', dirty);
    send('dirty-state', { dirty });
}

function classById(id) {
    return classes.find(item => item.id === Number(id));
}

function liveCharacter(name) {
    const key = name.trim().toLocaleLowerCase();
    return clients.find(client => client.name.trim().toLocaleLowerCase() === key) || null;
}

function avatarPath(classId) {
    return `../../avatars/c${classId}.png`;
}

function profileStatus(profile) {
    const live = liveCharacter(profile.name);
    if (!live) return { label: 'Offline', tone: '' };
    if (live.active) return { label: live.inBattle ? 'Active · In battle' : 'Active', tone: 'active' };
    return { label: live.inBattle ? 'Online · In battle' : 'Online', tone: 'online' };
}

function renderAll() {
    renderClassOptions();
    renderCreateClients();
    renderProfiles();
    renderEditor();
}

function renderClassOptions() {
    const create = $('characterClass');
    const profile = $('profileClass');
    const options = classes.map(item => `<option value="${item.id}">${escapeHtml(item.name)}</option>`).join('');
    create.innerHTML = options;
    profile.innerHTML = options;
}

function renderCreateClients() {
    const select = $('runningCharacter');
    const current = select.value;
    const unique = new Map();
    for (const client of clients) {
        if (client.name && client.classId >= 0) unique.set(client.name.toLocaleLowerCase(), client);
    }
    select.innerHTML = '<option value="">Enter manually</option>' + [...unique.values()]
        .sort((a, b) => a.name.localeCompare(b.name))
        .map(client => `<option value="${escapeAttr(client.name)}">${escapeHtml(client.name)} — ${escapeHtml(classById(client.classId)?.name || 'Unknown class')}</option>`)
        .join('');
    if ([...select.options].some(option => option.value === current)) select.value = current;
}

function renderProfiles() {
    const list = $('profileList');
    list.innerHTML = '';
    profiles.forEach((profile, index) => {
        const status = profileStatus(profile);
        const button = document.createElement('button');
        button.type = 'button';
        button.className = 'profile-item';
        button.setAttribute('aria-current', index === selectedProfile ? 'true' : 'false');
        button.innerHTML = `
            <img src="${avatarPath(profile.classId)}" alt="">
            <span class="profile-item-copy">
                <span class="profile-item-name" title="${escapeAttr(profile.name)}">${escapeHtml(profile.name)}</span>
                <span class="profile-item-meta">${escapeHtml(classById(profile.classId)?.name || 'Unknown class')} · ${status.label}</span>
            </span>
            <span class="count-chip" title="${profile.bindings.length} bindings">${profile.bindings.length}</span>`;
        button.addEventListener('click', () => {
            if (captureTarget) send('cancel-capture');
            selectedProfile = index;
            captureTarget = null;
            captureNotice = '';
            renderProfiles();
            renderEditor();
        });
        list.appendChild(button);
    });
    $('profileEmpty').hidden = profiles.length > 0;
    list.hidden = profiles.length === 0;
}

function renderEditor() {
    const exists = selectedProfile >= 0 && selectedProfile < profiles.length;
    $('welcomeState').hidden = exists;
    $('editorContent').hidden = !exists;
    if (!exists) return;

    const profile = profiles[selectedProfile];
    const classInfo = classById(profile.classId);
    $('profileName').textContent = profile.name;
    $('profileAvatar').src = avatarPath(profile.classId);
    $('profileAvatar').alt = `${classInfo?.name || 'Character'} avatar`;
    $('profileClass').value = String(profile.classId);
    renderLiveStatus();

    renderBindings(profile, classInfo);
    renderSkillMatrix(classInfo);
}

function renderLiveStatus() {
    if (selectedProfile < 0 || selectedProfile >= profiles.length) return;
    const profile = profiles[selectedProfile];
    const live = liveCharacter(profile.name);
    const status = profileStatus(profile);
    $('profilePresence').textContent = status.label;
    $('profilePresence').className = `presence ${status.tone}`;
    const mismatch = live && live.classId >= 0 && live.classId !== profile.classId;
    $('classWarning').hidden = !mismatch;
    if (mismatch) {
        $('classWarning').textContent = `${profile.name} is currently detected as ${classById(live.classId)?.name || 'another class'}. Update this profile before using its hotkeys.`;
    }
}

function renderBindings(profile, classInfo) {
    const list = $('bindingList');
    list.innerHTML = '';
    $('bindingCount').textContent = `${profile.bindings.length} ${profile.bindings.length === 1 ? 'binding' : 'bindings'}`;
    $('bindingEmpty').hidden = profile.bindings.length > 0;
    $('captureBanner').hidden = !captureTarget;
    $('bindingNotice').hidden = !captureNotice;
    $('bindingNotice').textContent = captureNotice;

    profile.bindings.forEach((binding, bindingIndex) => {
        const row = document.createElement('div');
        const isCapturing = captureTarget?.profileIndex === selectedProfile + 1 && captureTarget?.bindingIndex === bindingIndex + 1;
        const bindingKey = `${selectedProfile}:${bindingIndex}`;
        row.className = `binding-row${isCapturing ? ' capturing' : ''}${binding.chord ? '' : ' invalid'}${lastBound === bindingKey ? ' just-bound' : ''}`;

        const main = document.createElement('div');
        main.className = 'binding-main';
        const hotkey = document.createElement('button');
        hotkey.type = 'button';
        hotkey.className = `hotkey-button${binding.chord ? '' : ' unset'}`;
        hotkey.textContent = isCapturing ? 'Press shortcut…' : (binding.chord ? displayChord(binding.chord) : 'Set hotkey…');
        hotkey.setAttribute('aria-label', `Set hotkey for ${skillName(classInfo, binding.skill)}`);
        hotkey.addEventListener('click', () => startCapture(bindingIndex));

        const skill = document.createElement('select');
        skill.className = 'binding-skill';
        skill.setAttribute('aria-label', 'Skill');
        skill.innerHTML = classInfo.skills.map(item => `<option value="${item.enum}">${escapeHtml(item.name)}</option>`).join('');
        skill.value = String(binding.skill);
        skill.addEventListener('change', () => {
            binding.skill = Number(skill.value);
            markDirty();
            renderProfiles();
        });
        main.append(hotkey, skill);

        const actions = document.createElement('div');
        actions.className = 'binding-actions';
        const actor = document.createElement('span');
        actor.className = 'actor-label';
        actor.textContent = 'Player';
        const remove = document.createElement('button');
        remove.type = 'button';
        remove.className = 'quiet-button delete-binding';
        remove.textContent = '×';
        remove.setAttribute('aria-label', `Delete ${skillName(classInfo, binding.skill)} binding`);
        remove.addEventListener('click', () => {
            if (isCapturing) send('cancel-capture');
            profile.bindings.splice(bindingIndex, 1);
            captureTarget = null;
            markDirty();
            renderProfiles();
            renderEditor();
        });
        actions.append(actor, remove);
        row.append(main, actions);
        list.appendChild(row);
    });
}

function renderSkillMatrix(classInfo) {
    const matrix = $('skillMatrix');
    matrix.innerHTML = '';
    if (!classInfo) return;
    for (let i = 0; i < classInfo.skills.length; i += 4) {
        const group = classInfo.skills.slice(i, i + 4);
        const family = document.createElement('div');
        family.className = 'skill-family';
        family.dataset.search = group.map(skill => skill.name).join(' ').toLocaleLowerCase();
        const label = document.createElement('span');
        label.className = 'family-name';
        label.textContent = familyName(group[0].name);
        family.appendChild(label);
        for (const skill of group) {
            const button = document.createElement('button');
            button.type = 'button';
            button.className = 'tier-button';
            button.textContent = roman(skill.tier);
            button.title = `Bind ${skill.name}`;
            button.setAttribute('aria-label', `Bind ${skill.name}`);
            button.addEventListener('click', () => addBinding(skill.enum));
            family.appendChild(button);
        }
        matrix.appendChild(family);
    }
    filterSkillMatrix();
}

function filterSkillMatrix() {
    const query = $('skillSearch').value.trim().toLocaleLowerCase();
    let shown = 0;
    $('skillMatrix').querySelectorAll('.skill-family').forEach(family => {
        const match = !query || family.dataset.search.includes(query);
        family.hidden = !match;
        if (match) shown++;
    });
    $('skillEmpty').hidden = shown > 0;
}

function addBinding(skill) {
    if (selectedProfile < 0) return;
    const profile = profiles[selectedProfile];
    if (profile.bindings.length >= 64) {
        showToast('A character profile can contain up to 64 bindings.', 'error');
        return;
    }
    profile.bindings.push({ chord: '', actor: 'player', skill });
    captureNotice = '';
    markDirty();
    renderProfiles();
    renderEditor();
    startCapture(profile.bindings.length - 1);
}

function startCapture(bindingIndex) {
    captureNotice = '';
    captureTarget = { profileIndex: selectedProfile + 1, bindingIndex: bindingIndex + 1 };
    renderEditor();
    send('start-capture', captureTarget);
}

function handleCaptured(message) {
    const profileIndex = Number(message.profileIndex) - 1;
    const bindingIndex = Number(message.bindingIndex) - 1;
    const profile = profiles[profileIndex];
    const binding = profile?.bindings[bindingIndex];
    if (!binding) return;
    const duplicate = profile.bindings.some((item, index) => index !== bindingIndex && item.chord.toLocaleLowerCase() === message.chord.toLocaleLowerCase());
    captureTarget = null;
    if (duplicate) {
        captureNotice = `${message.display} is already assigned on ${profile.name}. Choose another shortcut or remove the existing binding.`;
        showToast(`${message.display} is already assigned.`, 'error');
        renderEditor();
        return;
    }
    binding.chord = message.chord;
    captureNotice = '';
    lastBound = `${profileIndex}:${bindingIndex}`;
    markDirty();
    renderEditor();
    showToast(`${message.display} now casts ${skillName(classById(profile.classId), binding.skill)}.`, 'success');
    setTimeout(() => {
        lastBound = '';
        document.querySelector('.binding-row.just-bound')?.classList.remove('just-bound');
    }, 650);
}

function showCreate() {
    $('createProfile').hidden = false;
    $('showCreate').disabled = true;
    $('createError').textContent = '';
    renderCreateClients();
    $('characterName').focus();
}

function hideCreate() {
    $('createProfile').hidden = true;
    $('showCreate').disabled = false;
    $('createError').textContent = '';
    $('runningCharacter').value = '';
    $('characterName').value = '';
}

function createProfile(event) {
    event.preventDefault();
    const name = $('characterName').value.trim().replace(/\s+/g, ' ').slice(0, 48);
    const classId = Number($('characterClass').value);
    if (!name) {
        $('createError').textContent = 'Enter a character name before creating the profile.';
        $('characterName').focus();
        return;
    }
    if (profiles.some(profile => profile.name.toLocaleLowerCase() === name.toLocaleLowerCase())) {
        $('createError').textContent = `A profile for “${name}” already exists.`;
        return;
    }
    profiles.push({ name, classId, bindings: [] });
    selectedProfile = profiles.length - 1;
    markDirty();
    hideCreate();
    renderProfiles();
    renderEditor();
}

function deleteSelectedProfile() {
    if (selectedProfile < 0) return;
    if (captureTarget) send('cancel-capture');
    const removed = profiles[selectedProfile].name;
    profiles.splice(selectedProfile, 1);
    selectedProfile = Math.min(selectedProfile, profiles.length - 1);
    captureTarget = null;
    markDirty();
    renderProfiles();
    renderEditor();
    showToast(`${removed} will be deleted when you save.`, 'info');
}

function changeSelectedClass() {
    if (selectedProfile < 0) return;
    const profile = profiles[selectedProfile];
    const nextClass = Number($('profileClass').value);
    if (nextClass === profile.classId) return;
    profile.classId = nextClass;
    if (profile.bindings.length) {
        profile.bindings = [];
        showToast('Existing bindings were cleared because the available skills changed.', 'info');
    }
    markDirty();
    renderAll();
}

function saveProfiles() {
    const unfinished = profiles.find(profile => profile.bindings.some(binding => !binding.chord));
    if (unfinished) {
        showToast(`Set every hotkey on ${unfinished.name}, or delete the unfinished row.`, 'error');
        return;
    }
    send('save', { profiles });
    $('saveProfiles').disabled = true;
    $('saveState').textContent = 'Saving hotkeys…';
}

function handleMessage(event) {
    const message = typeof event.data === 'string' ? JSON.parse(event.data) : event.data;
    switch (message.type) {
        case 'editor-state':
            profiles = cloneProfiles(message.profiles || []);
            classes = message.classes || [];
            if (selectedProfile >= profiles.length) selectedProfile = profiles.length - 1;
            if (selectedProfile < 0 && profiles.length) selectedProfile = 0;
            markDirty(false);
            renderAll();
            break;
        case 'live-state':
            clients = message.clients || [];
            renderCreateClients();
            renderProfiles();
            renderLiveStatus();
            break;
        case 'capture-started':
            captureNotice = '';
            captureTarget = { profileIndex: Number(message.profileIndex), bindingIndex: Number(message.bindingIndex) };
            renderEditor();
            break;
        case 'capture-cancelled':
            captureTarget = null;
            renderEditor();
            break;
        case 'hotkey-captured':
            handleCaptured(message);
            break;
        case 'save-result':
            if (message.ok) {
                markDirty(false);
                showToast(message.message, 'success');
            } else {
                $('saveProfiles').disabled = false;
                $('saveState').textContent = 'Changes need attention';
                $('saveState').classList.add('dirty');
                showToast(message.message, 'error');
            }
            break;
    }
}

function showToast(text, tone = 'info') {
    const toast = document.createElement('div');
    toast.className = `toast ${tone}`;
    toast.textContent = text;
    $('toastStack').appendChild(toast);
    setTimeout(() => toast.remove(), 4200);
}

function displayChord(chord) {
    const names = { '^': 'Ctrl+', '!': 'Alt+', '+': 'Shift+', '#': 'Win+' };
    let output = '';
    let index = 0;
    while (names[chord[index]]) output += names[chord[index++]];
    return output + chord.slice(index);
}

function familyName(skillNameText) {
    return skillNameText.replace(/\s+(I|II|III|IV)$/, '');
}

function skillName(classInfo, enumValue) {
    return classInfo?.skills.find(skill => skill.enum === Number(enumValue))?.name || 'Skill';
}

function roman(value) {
    return ['', 'I', 'II', 'III', 'IV'][Number(value)] || String(value);
}

function escapeHtml(value) {
    return String(value).replace(/[&<>"']/g, char => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[char]);
}

function escapeAttr(value) {
    return escapeHtml(value).replace(/`/g, '&#96;');
}

$('showCreate').addEventListener('click', showCreate);
$('emptyCreate').addEventListener('click', showCreate);
$('cancelCreate').addEventListener('click', hideCreate);
$('createProfile').addEventListener('submit', createProfile);
$('runningCharacter').addEventListener('change', event => {
    const client = clients.find(item => item.name === event.target.value);
    if (!client) return;
    $('characterName').value = client.name;
    $('characterClass').value = String(client.classId);
});
$('profileClass').addEventListener('change', changeSelectedClass);
$('skillSearch').addEventListener('input', filterSkillMatrix);
$('cancelCapture').addEventListener('click', () => send('cancel-capture'));
$('deleteProfile').addEventListener('click', deleteSelectedProfile);
$('saveProfiles').addEventListener('click', saveProfiles);
$('closeEditor').addEventListener('click', () => send('close'));
$('titleClose').addEventListener('click', () => send('close'));

document.addEventListener('keydown', event => {
    const command = event.ctrlKey || event.metaKey;
    const typing = /^(INPUT|SELECT|TEXTAREA)$/.test(document.activeElement?.tagName || '');
    if (command && event.key.toLowerCase() === 's') {
        event.preventDefault();
        if (!$('saveProfiles').disabled) saveProfiles();
    } else if (event.key === '/' && !typing && selectedProfile >= 0) {
        event.preventDefault();
        $('skillSearch').focus();
    } else if (event.key === 'Escape' && captureTarget) {
        event.preventDefault();
        send('cancel-capture');
    } else if (event.key === 'Escape' && !$('createProfile').hidden) {
        event.preventDefault();
        hideCreate();
    }
});

if (window.chrome?.webview) {
    window.chrome.webview.addEventListener('message', handleMessage);
}

// A local, read-only preview for visual regression work. WebView2 never enters
// this path; opening index.html?preview in a browser supplies realistic state
// without introducing a second production data source.
const previewState = new URLSearchParams(location.search).get('preview');
if (previewState !== null && !window.chrome?.webview) {
    const classNames = ['Male Human', 'Female Human', 'Male Centaur', 'Female Centaur',
        'Male Mage', 'Female Mage', 'Male Borg', 'Female Borg'];
    classes = classNames.map((name, id) => ({ id, name, skills: [] }));
    classes[0].skills = [
        'Frailty I', 'Frailty II', 'Frailty III', 'Frailty IV',
        'Chaos I', 'Chaos II', 'Chaos III', 'Chaos IV',
        'Hypnotize I', 'Hypnotize II', 'Hypnotize III', 'Hypnotize IV',
        'Stun I', 'Stun II', 'Stun III', 'Stun IV'
    ].map((name, enumValue) => ({ enum: enumValue < 4 ? enumValue : enumValue + (enumValue < 16 ? 4 : 0), name, tier: enumValue % 4 + 1 }));
    const centaurEnums = [24, 25, 26, 27, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43];
    classes[3].skills = [
        'Un Stun I', 'Un Stun II', 'Un Stun III', 'Un Stun IV',
        'Blizzard I', 'Blizzard II', 'Blizzard III', 'Blizzard IV',
        'Speed I', 'Speed II', 'Speed III', 'Speed IV',
        'Heal Other I', 'Heal Other II', 'Heal Other III', 'Heal Other IV'
    ].map((name, index) => ({ enum: centaurEnums[index], name, tier: index % 4 + 1 }));
    if (previewState === 'empty') {
        profiles = [];
        clients = [];
        selectedProfile = -1;
    } else {
        profiles = [
            { name: 'Ardent', classId: 0, bindings: [
                { chord: 'F1', actor: 'player', skill: 19 },
                { chord: '^2', actor: 'player', skill: 11 },
                { chord: '!3', actor: 'player', skill: 14 }
            ] },
            { name: 'Eir', classId: 3, bindings: [] }
        ];
        if (previewState === 'unbound') {
            clients = [{ name: 'Eir', classId: 3, inBattle: false, active: false }];
            selectedProfile = 1;
        } else {
            clients = [{ name: 'Ardent', classId: 0, inBattle: true, active: true }];
            selectedProfile = 0;
        }
    }
    renderAll();
} else {
    send('init-request');
}
