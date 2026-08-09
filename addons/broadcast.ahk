#Requires AutoHotkey v2.0

; SUSPENDED — deliberately absent from every variants\*.txt, so no build ships
; it. The addon loads and registers, but it needs more work before it is safe to
; hand to users and sits below other work in priority. Kept rather than deleted
; so that work resumes where it stopped; add the filename back to the manifests
; to ship it.

; Sends one keystroke to every running client at once — the thing multi-boxers
; reach for constantly and that this app had no answer for.
;
; It is the same shape as SendEnterUntilReady (functions.ahk): walk the top-level
; game windows and ControlSend to each, without activating anything. Nothing new
; is invented here; what this addon adds is which key, to whom, and the guards.
;
; ── The dangerous version of this feature ────────────────────────
;
; A broadcast that fires on a bare key would send whatever you type in chat to
; every client. So triggers are always modified chords (Alt+1, not 1), and the
; hotkeys only exist while a game window is focused.
;
; A broadcast that reaches a client sitting at the login or character-select
; screen types into a password field. So every target's game state is read at
; the moment of sending — not from the shared poll, which can be a second stale
; — and any client that is not in the world is skipped and reported.
;
; ── Why the hotkeys are transient ────────────────────────────────
;
; One action per configured key, rebuilt through OnBeforeApplyHotkeys, exactly
; as better_hotkeys does for its skill chords. They are hidden and persist:false
; so they never appear in the general Settings hotkey list and never get written
; into config.ini [Hotkeys] — this addon's own config is the source of truth.

global _Broadcast_ACTION_ID_PREFIX := "broadcast."

; Trigger = this modifier plus the key. Alt is the default because it is the
; least likely to collide with anything the game itself uses.
global _Broadcast_Modifier := "!"
global _BROADCAST_MODIFIERS := ["!", "^!", "+!", "^+"]
global _BROADCAST_MODIFIER_LABELS := ["Alt + key", "Ctrl + Alt + key", "Shift + Alt + key", "Ctrl + Shift + key"]

; The keys to broadcast, as typed by the user.
global _Broadcast_Keys := []
global _BROADCAST_KEYS_DEFAULT := "1,2,3,4,5"
global _BROADCAST_MAX_KEYS := 24

; "all" = every client including the focused one; "others" = everyone else.
global _Broadcast_Target := "all"
global _BROADCAST_TARGETS := ["all", "others"]
global _BROADCAST_TARGET_LABELS := ["All clients, including this one", "All clients except the focused one"]

; Character names that never receive a broadcast — a bank alt, a client parked
; somewhere deliberately. Matched case-insensitively against the window title.
global _Broadcast_Excluded := []

RegisterAddon(Map(
    "name",                 "Broadcast",
    "settingsLabel",        "Broadcast",
    "OnInit",               _Broadcast_OnInit,
    "OnTrayMenu",           _Broadcast_OnTrayMenu,
    "OnSettingsWeb",        _Broadcast_OnSettingsWeb,
    "OnSettingsWebSave",    _Broadcast_OnSettingsWebSave,
    "OnBeforeApplyHotkeys", _Broadcast_OnBeforeApplyHotkeys
))

; The one fixed action, so the target can be flipped mid-fight from the Quick
; Actions ring instead of by opening Settings.
RegisterHotkeyAction(Map(
    "id", "broadcastToggleTarget",
    "label", "Broadcast: all or all but focused",
    "category", "Broadcast",
    "default", "",
    "addon", "Broadcast",
    "handler", (*) => _Broadcast_ToggleTarget()
))

_Broadcast_OnInit() {
    _Broadcast_LoadConfig()
}

; ── Config ───────────────────────────────────────────────────────

_Broadcast_LoadConfig() {
    global CONFIG_INI, _Broadcast_Modifier, _Broadcast_Keys, _Broadcast_Target
    global _Broadcast_Excluded, _BROADCAST_KEYS_DEFAULT

    ; Not named `mod`: Mod() is a built-in, and shadowing it is a
    ; load-time name conflict that Ahk2Exe rejects outright.
    modChord := Trim(ConfigRead("Broadcast", "Modifier", "!"))
    _Broadcast_Modifier := _Broadcast_IsModifier(modChord) ? modChord : "!"

    _Broadcast_Keys := _Broadcast_ParseKeys(
        ConfigRead("Broadcast", "Keys", _BROADCAST_KEYS_DEFAULT))

    target := StrLower(Trim(ConfigRead("Broadcast", "Target", "all")))
    _Broadcast_Target := (target = "others") ? "others" : "all"

    _Broadcast_Excluded := _Broadcast_ParseNames(
        ConfigRead("Broadcast", "Exclude", ""))
}

_Broadcast_SaveConfig() {
    global CONFIG_INI, _Broadcast_Modifier, _Broadcast_Keys, _Broadcast_Target
    global _Broadcast_Excluded
    EnsureIniUtf16(CONFIG_INI)
    IniWrite(_Broadcast_Modifier, CONFIG_INI, "Broadcast", "Modifier")
    IniWrite(_Broadcast_Join(_Broadcast_Keys), CONFIG_INI, "Broadcast", "Keys")
    IniWrite(_Broadcast_Target, CONFIG_INI, "Broadcast", "Target")
    IniWrite(_Broadcast_Join(_Broadcast_Excluded), CONFIG_INI, "Broadcast", "Exclude")
}

_Broadcast_IsModifier(m) {
    global _BROADCAST_MODIFIERS
    for v in _BROADCAST_MODIFIERS
        if (v = m)
            return true
    return false
}

_Broadcast_Join(list) {
    out := ""
    for v in list
        out .= (out = "" ? "" : ",") v
    return out
}

; Keys the user typed, keeping only ones that can actually be sent, and dropping
; duplicates so one key never registers two competing hotkeys.
_Broadcast_ParseKeys(csv) {
    global _BROADCAST_MAX_KEYS
    keys := [], seen := Map()
    for part in StrSplit(csv, ",") {
        k := Trim(part)
        if (k = "" || _Broadcast_SendSpec(k) = "")
            continue
        low := StrLower(k)
        if seen.Has(low)
            continue
        seen[low] := true
        keys.Push(k)
        if (keys.Length >= _BROADCAST_MAX_KEYS)
            break
    }
    return keys
}

_Broadcast_ParseNames(csv) {
    names := [], seen := Map()
    for part in StrSplit(csv, ",") {
        n := Trim(part)
        if (n = "")
            continue
        low := StrLower(n)
        if seen.Has(low)
            continue
        seen[low] := true
        names.Push(n)
    }
    return names
}

; ── Keys ─────────────────────────────────────────────────────────

; The ControlSend form of a key, or "" when it is not one we will send.
;
; A whitelist rather than escaping: ControlSend treats ^!+#{} as syntax, so
; passing user text through unchecked turns a typo into an arbitrary keystroke
; sequence delivered to every client at once.
_Broadcast_SendSpec(key) {
    static NAMED := "|Enter|Escape|Space|Tab|Backspace|Delete|Insert|Home|End|"
                  . "PgUp|PgDn|Up|Down|Left|Right|"
                  . "Numpad0|Numpad1|Numpad2|Numpad3|Numpad4|Numpad5|Numpad6|"
                  . "Numpad7|Numpad8|Numpad9|NumpadEnter|"
                  . "F1|F2|F3|F4|F5|F6|F7|F8|F9|F10|F11|F12|"
                  . "F13|F14|F15|F16|F17|F18|F19|F20|F21|F22|F23|F24|"
    k := Trim(key)
    if (k = "")
        return ""
    ; A single letter or digit goes as itself.
    if (StrLen(k) = 1 && RegExMatch(k, "^[0-9A-Za-z]$"))
        return k
    ; Anything else has to be a name we recognise, and is sent braced.
    if InStr(NAMED, "|" k "|")
        return "{" k "}"
    return ""
}

; ── Hotkey registry integration ──────────────────────────────────

_Broadcast_IsOwnActionId(id) {
    global _Broadcast_ACTION_ID_PREFIX
    return SubStr(String(id), 1, StrLen(_Broadcast_ACTION_ID_PREFIX)) = _Broadcast_ACTION_ID_PREFIX
}

; "" when the chord is free, otherwise what already owns it. Own actions are
; skipped because they are torn down and rebuilt on the same pass.
_Broadcast_Conflict(chord) {
    global gHotkeyActions, gHotkeyReserved
    key := StrLower(NormalizeHotkeyChord(chord))
    if (key = "")
        return "an empty chord"
    for reserved in gHotkeyReserved {
        if (StrLower(reserved) = key)
            return "a reserved Maps++ shortcut"
    }
    for id, action in gHotkeyActions {
        if _Broadcast_IsOwnActionId(id) || !IsHotkeyActionEnabled(action)
            continue
        if (StrLower(action["chord"]) = key)
            return action.Has("label") ? action["label"] : id
    }
    return ""
}

_Broadcast_OnBeforeApplyHotkeys() {
    global gHotkeyActions, _Broadcast_Keys, _Broadcast_Modifier
    global _Broadcast_ACTION_ID_PREFIX

    for id, _ in gHotkeyActions.Clone() {
        if _Broadcast_IsOwnActionId(id)
            gHotkeyActions.Delete(id)
    }

    for key in _Broadcast_Keys {
        chord := _Broadcast_Modifier key
        if !IsHotkeyChordValid(chord)
            continue
        ; A chord already spoken for is skipped rather than stolen; the Settings
        ; tab reports the clash so it is not silent.
        if (_Broadcast_Conflict(chord) != "")
            continue
        RegisterHotkeyAction(Map(
            "id", _Broadcast_ACTION_ID_PREFIX key,
            "label", "Broadcast " key,
            "category", "Broadcast",
            "default", chord,
            "addon", "Broadcast",
            "handler", _Broadcast_Send.Bind(key),
            ; Only while a client is focused: a broadcast that fires from a
            ; browser or from Discord is never what anyone meant.
            "hotIfWinActive", true,
            "hidden", true,
            "persist", false
        ))
    }
}

; ── Sending ──────────────────────────────────────────────────────

_Broadcast_Send(key) {
    global _Broadcast_Target, _Broadcast_Excluded, GAME_STATE_READY

    spec := _Broadcast_SendSpec(key)
    if (spec = "")
        return

    activeHwnd := WinActive(GAME_WIN_FILTER)
    if !activeHwnd
        return
    activePid := 0
    try activePid := WinGetPID("ahk_id " activeHwnd)

    stateRva := GetResolvedOffset("GAME_STATE_OFFSET")
    sent := 0, skippedNotReady := 0, skippedExcluded := 0
    seen := Map()

    for hwnd in GetTopLevelGameWindows() {
        pid := 0
        try pid := WinGetPID("ahk_id " hwnd)
        ; One send per process — a client can contribute more than one hwnd, and
        ; a doubled keystroke is a real action taken twice in game.
        if (!pid || seen.Has(pid))
            continue
        seen[pid] := true

        if (_Broadcast_Target = "others" && pid = activePid)
            continue

        if _Broadcast_IsExcluded(hwnd) {
            skippedExcluded++
            continue
        }

        ; Read the state now rather than trusting the shared poll: this is the
        ; guard that keeps a keystroke out of a login screen's password field,
        ; and a second-old answer is not good enough for that.
        state := ReadGameStateForWindow(hwnd, stateRva)
        if (state >= 0 && state < GAME_STATE_READY) {
            skippedNotReady++
            continue
        }

        try {
            ControlSend(spec, , "ahk_id " hwnd)
            sent++
        }
    }

    if (skippedNotReady > 0) {
        TrayTip(skippedNotReady " client(s) are still loading or at a login"
            . " screen and were skipped.", "Broadcast", "Icon!")
    }
    if (sent = 0 && skippedNotReady = 0 && skippedExcluded = 0)
        TrayTip("No clients to broadcast to.", "Broadcast", "Iconi")
}

_Broadcast_IsExcluded(hwnd) {
    global _Broadcast_Excluded
    if (_Broadcast_Excluded.Length = 0)
        return false
    name := CharacterNameFromWindow(hwnd)
    if (name = "")
        return false
    for n in _Broadcast_Excluded {
        if (StrLower(n) = StrLower(name))
            return true
    }
    return false
}

_Broadcast_ToggleTarget() {
    global _Broadcast_Target
    _Broadcast_Target := (_Broadcast_Target = "all") ? "others" : "all"
    _Broadcast_SaveConfig()
    TrayTip(_Broadcast_Target = "all"
        ? "Broadcasting to all clients."
        : "Broadcasting to all clients except the focused one.",
        "Broadcast", "Iconi")
    SetTimer(RebuildTrayMenu, -1)
}

; ── Tray ─────────────────────────────────────────────────────────

_Broadcast_OnTrayMenu(trayGroups) {
    global _Broadcast_Keys, _Broadcast_Target, _Broadcast_Modifier
    n := _Broadcast_Keys.Length
    summary := (n = 0)
        ? "Broadcast — no keys set"
        : ("Broadcast — " n " key" (n = 1 ? "" : "s") " - "
            . (_Broadcast_Target = "all" ? "all" : "all but focused"))
    bcMenu := Menu()
    bcMenu.Add(summary, (*) => ShowSettingsWindow())
    if n {
        bcMenu.Add("Triggers: " FormatHotkeyDisplay(_Broadcast_Modifier "…")
            . " + " _Broadcast_Join(_Broadcast_Keys), (*) => ShowSettingsWindow())
    }
    bcMenu.Add()
    bcMenu.Add("Switch target`t" GetHotkeyDisplay("broadcastToggleTarget"),
        (*) => _Broadcast_ToggleTarget())
    trayGroups["clients"].Add("Broadcast", bcMenu)
}

; ── Settings ─────────────────────────────────────────────────────

_Broadcast_OnSettingsWeb() {
    global _Broadcast_Keys, _Broadcast_Modifier, _Broadcast_Target, _Broadcast_Excluded
    global _BROADCAST_MODIFIERS, _BROADCAST_MODIFIER_LABELS
    global _BROADCAST_TARGETS, _BROADCAST_TARGET_LABELS, _BROADCAST_MAX_KEYS
    global _BROADCAST_KEYS_DEFAULT

    ; Offer the characters running now, so the usual case is a pick not a typo.
    running := []
    for snap in GetClientSnapshots() {
        if (snap.charName != "")
            running.Push(snap.charName)
    }

    modIdx := 0
    for i, m in _BROADCAST_MODIFIERS {
        if (m = _Broadcast_Modifier) {
            modIdx := i - 1
            break
        }
    }
    targetIdx := (_Broadcast_Target = "others") ? 1 : 0

    defaultModIdx := 0
    defaultModChord := DefaultRead("Broadcast", "Modifier", "!")
    for i, m in _BROADCAST_MODIFIERS {
        if (m = defaultModChord) {
            defaultModIdx := i - 1
            break
        }
    }
    defaultTargetIdx := (DefaultRead("Broadcast", "Target", "all") = "others") ? 1 : 0

    fields := [
        Map("type", "info", "text",
            "Sends one keystroke to every running client at once.`n`n"
            . "Each key below becomes a shortcut: with the Alt trigger, pressing "
            . "Alt+1 sends 1 to your clients. Triggers only work while a game "
            . "window is focused, and always use a modifier so typing in chat "
            . "never broadcasts."),
        Map("type", "dropdown", "id", "modifierIdx", "label", "Trigger:",
            "options", _BROADCAST_MODIFIER_LABELS, "value", modIdx, "default", defaultModIdx),
        Map("type", "combo", "id", "keys",
            "label", "Keys to broadcast (comma separated):",
            "options", ["1,2,3,4,5", "1,2,3,4,5,6,7,8,9,0", "F1,F2,F3,F4"],
            "value", _Broadcast_Join(_Broadcast_Keys),
            "default", DefaultRead("Broadcast", "Keys", _BROADCAST_KEYS_DEFAULT)),
        Map("type", "dropdown", "id", "targetIdx", "label", "Send to:",
            "options", _BROADCAST_TARGET_LABELS, "value", targetIdx, "default", defaultTargetIdx),
        Map("type", "combo", "id", "exclude",
            "label", "Never send to these characters (comma separated):",
            "options", running,
            "value", _Broadcast_Join(_Broadcast_Excluded),
            "default", DefaultRead("Broadcast", "Exclude", "")),
        Map("type", "info", "text",
            "Letters, digits, F1-F24 and named keys (Enter, Space, Tab, Up, "
            . "Numpad1 …) can be sent; up to " _BROADCAST_MAX_KEYS " of them. "
            . "Anything else is ignored.`n`n"
            . "A client sitting at a login or character-select screen is always "
            . "skipped, so a broadcast can never be typed into a password box.")
    ]

    ; Say so when a trigger cannot be bound, rather than leaving the user to
    ; wonder why one key does nothing.
    clashes := ""
    for key in _Broadcast_Keys {
        owner := _Broadcast_Conflict(_Broadcast_Modifier key)
        if (owner != "")
            clashes .= (clashes = "" ? "" : "`n") . key " — already used by " owner
    }
    if (clashes != "") {
        fields.Push(Map("type", "info", "text",
            "These keys cannot be bound with the current trigger:`n" clashes))
    }
    return fields
}

_Broadcast_OnSettingsWebSave(values) {
    global _Broadcast_Modifier, _Broadcast_Keys, _Broadcast_Target, _Broadcast_Excluded
    global _BROADCAST_MODIFIERS, _BROADCAST_TARGETS

    if (values.Has("modifierIdx") && IsNumber(values["modifierIdx"])) {
        idx := Integer(Round(values["modifierIdx"])) + 1
        if (idx >= 1 && idx <= _BROADCAST_MODIFIERS.Length)
            _Broadcast_Modifier := _BROADCAST_MODIFIERS[idx]
    }
    if (values.Has("targetIdx") && IsNumber(values["targetIdx"])) {
        idx := Integer(Round(values["targetIdx"])) + 1
        if (idx >= 1 && idx <= _BROADCAST_TARGETS.Length)
            _Broadcast_Target := _BROADCAST_TARGETS[idx]
    }
    if values.Has("keys")
        _Broadcast_Keys := _Broadcast_ParseKeys(String(values["keys"]))
    if values.Has("exclude")
        _Broadcast_Excluded := _Broadcast_ParseNames(String(values["exclude"]))

    _Broadcast_SaveConfig()
    ; Rebuilds the transient per-key actions through OnBeforeApplyHotkeys.
    ApplyAllHotkeys()
}
