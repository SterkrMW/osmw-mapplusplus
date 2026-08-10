#Requires AutoHotkey v2.0

; ── Central hotkey registry ──────────────────────────────────────
; Actions register via RegisterHotkeyAction(); overrides live in config.ini
; [Hotkeys]. ApplyAllHotkeys() binds at runtime with Hotkey().

global gHotkeyActions := Map()       ; id → action spec
global gAppliedHotkeys := []         ; { chord, hotIf } for teardown on re-apply
; Must match the static hotkeys in main.ahk's "Fixed hotkeys (not rebindable)"
; block exactly — this list has no compile-time link to those bindings, so a
; new fixed hotkey added there needs a matching entry added here.
global gHotkeyReserved := ["^!r", "^!q", "^!d", "^!s", "^!v", "^!1", "^!2", "^!3", "^!4"]

RegisterHotkeyAction(spec) {
    global gHotkeyActions
    if !spec.Has("id") || spec["id"] = "" {
        TrayTip("RegisterHotkeyAction() missing id — ignored.", "Hotkeys", "Iconx")
        return
    }
    id := spec["id"]
    if !spec.Has("default")
        spec["default"] := ""
    spec["chord"] := spec["default"]
    gHotkeyActions[id] := spec
}

IsHotkeyActionEnabled(action) {
    global gDisabledAddons
    if action.Has("addon") {
        addonName := action["addon"]
        if gDisabledAddons.Has(addonName) && gDisabledAddons[addonName]
            return false
    }
    return true
}

GetHotkeyChord(id) {
    global gHotkeyActions
    if !gHotkeyActions.Has(id)
        return ""
    return gHotkeyActions[id]["chord"]
}

GetHotkeyDisplay(id) {
    return FormatHotkeyDisplay(GetHotkeyChord(id))
}

FormatHotkeyDisplay(chord) {
    if (chord = "")
        return ""
    modNames := Map("^", "Ctrl", "!", "Alt", "+", "Shift", "#", "Win")
    mods := ""
    i := 1
    while (i <= StrLen(chord)) {
        ch := SubStr(chord, i, 1)
        if modNames.Has(ch) {
            mods .= modNames[ch] "+"
            i++
        } else
            break
    }
    key := SubStr(chord, i)
    if (key = "")
        return RTrim(mods, "+")
    return mods key
}

FormatHotkeySettingsDisplay(chord) {
    display := FormatHotkeyDisplay(chord)
    return (display = "") ? "Not bound" : display
}

NormalizeHotkeyChord(chord) {
    chord := Trim(chord)
    if (chord = "")
        return ""
    return chord
}

ToHotkeyApiName(chord) {
    return NormalizeHotkeyChord(chord)
}

; The key a chord ends on, with its modifiers stripped — "!+e" → "e". Returns
; "" for an unbound or modifier-only chord.
;
; Exists for handlers that want to act on the *release* of the key that fired
; them (see addons\inventory_toggle.ahk): the chord is rebindable, so they
; cannot hardcode the key name and must ask the registry what it currently is.
HotkeyTriggerKey(chord) {
    chord := NormalizeHotkeyChord(chord)
    modChars := "^!+#<>"
    i := 1
    while (i <= StrLen(chord)) {
        if !InStr(modChars, SubStr(chord, i, 1))
            break
        i++
    }
    return SubStr(chord, i)
}

IsHotkeyChordValid(chord) {
    chord := NormalizeHotkeyChord(chord)
    ; An empty chord is a deliberate, valid "unbound" setting. Binding code
    ; skips it, while persistence must preserve it instead of restoring the
    ; action's default on the next launch.
    if (chord = "")
        return true
    key := HotkeyTriggerKey(chord)
    if (key = "")
        return false
    if RegExMatch(key, "i)^(Control|Alt|Shift|LShift|RShift|LControl|RControl|LAlt|RAlt|LWin|RWin)$")
        return false
    return true
}

GetHotkeyConflictAction(chord, exceptId := "") {
    global gHotkeyActions, gHotkeyReserved, gSettingsHotkeyRows
    chord := NormalizeHotkeyChord(chord)
    if (chord = "")
        return ""
    for reserved in gHotkeyReserved {
        if (StrLower(chord) = StrLower(reserved))
            return "(reserved — cannot rebind Reload, Exit, or debug/calibration keys)"
    }
    if IsObject(gSettingsHotkeyRows) {
        for row in gSettingsHotkeyRows {
            if row.Has("id") && row["id"] = exceptId
                continue
            pending := row.Has("pending") ? row["pending"] : row["action"]["chord"]
            if (StrLower(pending) = StrLower(chord))
                return row["action"].Has("label") ? row["action"]["label"] : row["id"]
        }
        ; Data-driven providers may contribute hidden runtime actions which do
        ; not have a Settings row. They still reserve their chord, otherwise a
        ; general hotkey edit could silently replace a Better Hotkeys handler.
        for id, action in gHotkeyActions {
            if !(action.Has("hidden") && action["hidden"])
                continue
            if !IsHotkeyActionEnabled(action)
                continue
            if (StrLower(action["chord"]) = StrLower(chord))
                return action.Has("label") ? action["label"] : id
        }
        return ""
    }
    for id, action in gHotkeyActions {
        if (id = exceptId)
            continue
        if !IsHotkeyActionEnabled(action)
            continue
        if (StrLower(action["chord"]) = StrLower(chord))
            return action.Has("label") ? action["label"] : id
    }
    return ""
}

LoadHotkeyOverrides() {
    global gHotkeyActions, CONFIG_INI
    if !FileExist(CONFIG_INI)
        return
    for id, action in gHotkeyActions {
        if action.Has("persist") && !action["persist"]
            continue
        raw := Trim(IniRead(CONFIG_INI, "Hotkeys", id, "__MISSING__"))
        if (raw = "__MISSING__")
            continue
        if IsHotkeyChordValid(raw)
            action["chord"] := raw
        else
            action["chord"] := action["default"]
    }
}

SaveHotkeyOverrides() {
    global gHotkeyActions, CONFIG_INI
    for id, action in gHotkeyActions {
        if action.Has("persist") && !action["persist"]
            continue
        IniWrite(action["chord"], CONFIG_INI, "Hotkeys", id)
    }
}

; Resolves an action's HotIf context into a single {mode, hotIfFn} object.
; mode is "winActive" (bound to GAME_WIN_FILTER), "callback" (a HotIf()
; predicate), or "global" (unconditional). Reused at both bind and teardown
; time (_ApplyHotkeyContext) so the two can't drift out of sync.
_ResolveHotkeyContext(action) {
    if action.Has("hotIfWinActive") && action["hotIfWinActive"]
        return { mode: "winActive", hotIfFn: 0 }
    if action.Has("hotIfFn") && action["hotIfFn"] is Func
        return { mode: "callback", hotIfFn: action["hotIfFn"] }
    return { mode: "global", hotIfFn: 0 }
}

_ApplyHotkeyContext(context) {
    if !IsObject(context)
        return
    if (context.mode = "winActive")
        HotIfWinActive(GAME_WIN_FILTER)
    else if (context.mode = "callback") && context.hotIfFn is Func
        HotIf(context.hotIfFn)
    else
        HotIf
}

_ApplyHotkeyOff(entry) {
    if !IsObject(entry)
        return
    _ApplyHotkeyContext(entry)
    try Hotkey(entry.chord, "Off")
    catch {
        ; Already off or never registered.
    }
    HotIf
}

_HotkeyWrapHandler(handler) {
    return (*) => handler.Call()
}

ApplyAllHotkeys() {
    global gAppliedHotkeys, gHotkeyActions

    for entry in gAppliedHotkeys
        _ApplyHotkeyOff(entry)
    gAppliedHotkeys := []

    ; Addons with data-driven bindings (Better Hotkeys, for example) rebuild
    ; their transient action specs immediately before the registry is applied.
    ; Keeping them in this same cycle gives them the same teardown, addon
    ; enable/disable, and conflict behaviour as every static action.
    FireAddonHook("OnBeforeApplyHotkeys")

    for id, action in gHotkeyActions {
        if !IsHotkeyActionEnabled(action)
            continue
        chord := action["chord"]
        if (chord = "") || !IsHotkeyChordValid(chord)
            continue

        context := _ResolveHotkeyContext(action)
        _ApplyHotkeyContext(context)

        usePrefix := action.Has("passThrough") && action["passThrough"]
        bindChord := (usePrefix ? "$" : "") ToHotkeyApiName(chord)
        handler := action["handler"]
        try {
            Hotkey(bindChord, _HotkeyWrapHandler(handler))
            ; Updating a variant's action does NOT clear the Off state applied
            ; by the teardown above, so every binding has to be switched back on
            ; explicitly. Without this, the first Settings save disables every
            ; hotkey — including Tab — until the next Reload.
            Hotkey(bindChord, "On")
            context.chord := bindChord
            gAppliedHotkeys.Push(context)
        } catch as err {
            TrayTip("Failed to bind " id ": " err.Message, "Hotkeys", "Iconx")
        }
    }
    HotIf
}

; HotIf callback criteria for dynamic Hotkey() — mirrors the old static #HotIf blocks.
HotIfToggleMinimap(*) {
    global gOverlayVisible, gCanOverride
    if gOverlayVisible && IsGameOrOverlayActive()
        return true
    if WinActive(GAME_WIN_FILTER) && gCanOverride && !gOverlayVisible && IsMinimapAllowed()
        return true
    return false
}

HotIfCloseOverlay(*) {
    global gOverlayVisible
    return gOverlayVisible && IsGameOrOverlayActive()
}

HotIfGameOrOverlay(*) {
    return IsGameOrOverlayActive()
}

RegisterCoreHotkeyActions() {
    ; Keep the out-of-box set intentionally small. Addon actions are available
    ; through Quick Actions and the tray, and users can opt into direct chords.
    RegisterHotkeyAction(Map(
        "id", "toggleMinimap",
        "label", "Toggle minimap",
        "category", "Core",
        "default", "Tab",
        "handler", HandleTab,
        "hotIfFn", HotIfToggleMinimap,
        "passThrough", true
    ))
    RegisterHotkeyAction(Map(
        "id", "closeOverlay",
        "label", "Close overlay",
        "category", "Core",
        "default", "RButton",
        "handler", CloseOverlay,
        "hotIfFn", HotIfCloseOverlay,
        "passThrough", true,
        "allowMouse", true
    ))
    RegisterHotkeyAction(Map(
        "id", "launchPrimary",
        "label", "Launch clients (primary monitor)",
        "category", "Core",
        "default", "^!l",
        "handler", (*) => LaunchConfiguredClients("primary")
    ))
    RegisterHotkeyAction(Map(
        "id", "launchSecondary",
        "label", "Launch clients (secondary monitor)",
        "category", "Core",
        "default", "^!k",
        "handler", (*) => LaunchConfiguredClients("secondary")
    ))
    RegisterHotkeyAction(Map(
        "id", "sendEnterUntilReady",
        "label", "Send Enter until ready",
        "category", "Core",
        "default", "^!e",
        "handler", SendEnterUntilReady,
        "hotIfFn", HotIfGameOrOverlay
    ))
    RegisterHotkeyAction(Map(
        "id", "openSettings",
        "label", "Open Settings",
        "category", "Core",
        "default", "^!,",
        "handler", ShowSettingsWindow
    ))
}

GetHotkeyActionsForSettings() {
    global gHotkeyActions
    categoryOrder := ["Core", "Quick Actions", "Client Roster", "Window Layout",
        "View Mode", "Battle", "Inventory", "Chat", "Map POIs", "Party Markers",
        "Character Vendor"]
    byCategory := Map()
    for id, action in gHotkeyActions {
        if !IsHotkeyActionEnabled(action)
            continue
        if action.Has("hidden") && action["hidden"]
            continue
        cat := action.Has("category") ? action["category"] : "Other"
        if !byCategory.Has(cat)
            byCategory[cat] := []
        byCategory[cat].Push(action)
    }
    list := []
    added := Map()
    for cat in categoryOrder {
        if !byCategory.Has(cat)
            continue
        added[cat] := true
        for action in byCategory[cat]
            list.Push(action)
    }
    for cat, actions in byCategory {
        if added.Has(cat)
            continue
        for action in actions
            list.Push(action)
    }
    return list
}

; ── Settings capture (InputHook) ─────────────────────────────────

global gHotkeyCaptureHook := 0
global gHotkeyCaptureRow := 0
global gHotkeyCaptureCancel := 0
global gSettingsHotkeyRows := 0

CancelHotkeyCapture() {
    global gHotkeyCaptureHook, gHotkeyCaptureRow, gHotkeyCaptureCancel
    if IsObject(gHotkeyCaptureHook) {
        try gHotkeyCaptureHook.Stop()
        gHotkeyCaptureHook := 0
    }
    if IsObject(gHotkeyCaptureRow) && gHotkeyCaptureRow.Has("button") {
        try gHotkeyCaptureRow["button"].Text := FormatHotkeyDisplay(gHotkeyCaptureRow["pending"])
    }
    gHotkeyCaptureRow := 0
    if gHotkeyCaptureCancel is Func {
        try gHotkeyCaptureCancel()
        gHotkeyCaptureCancel := 0
    }
}

StartHotkeyCapture(row, onDone, onCancel := 0) {
    global gHotkeyCaptureHook, gHotkeyCaptureRow, gHotkeyCaptureCancel, gSettingsGui
    CancelHotkeyCapture()
    gHotkeyCaptureRow := row
    gHotkeyCaptureCancel := onCancel
    row["button"].Text := "Press new shortcut…"

    ih := InputHook("L0")
    ih.KeyOpt("{All}", "N")
    ih.OnKeyDown := (hook, vk, sc) => _HotkeyCaptureKeyDown(hook, vk, onDone)
    ih.Start()
    gHotkeyCaptureHook := ih
}

_HotkeyCaptureKeyDown(hook, vk, onDone) {
    global gHotkeyCaptureRow, gHotkeyCaptureHook, gSettingsGui

    if IsObject(gSettingsGui) && gSettingsGui && !WinActive("ahk_id " gSettingsGui.Hwnd)
        return

    if (vk = 0x1B) { ; Escape
        hook.Stop()
        CancelHotkeyCapture()
        return
    }

    mods := ""
    if GetKeyState("Ctrl")
        mods .= "^"
    if GetKeyState("Alt")
        mods .= "!"
    if GetKeyState("Shift")
        mods .= "+"
    if GetKeyState("LWin") || GetKeyState("RWin")
        mods .= "#"

    keyName := GetKeyName(Format("vk{:02X}", vk))
    if (keyName = "")
        return

    if RegExMatch(keyName, "i)^(Control|Alt|Shift|LShift|RShift|LControl|RControl|LAlt|RAlt|LWin|RWin)$")
        return

    allowMouse := gHotkeyCaptureRow.Has("allowMouse") && gHotkeyCaptureRow["allowMouse"]
    if RegExMatch(keyName, "i)Button$") && !allowMouse
        return

    chord := mods keyName
    if !IsHotkeyChordValid(chord)
        return

    exceptId := gHotkeyCaptureRow.Has("id") ? gHotkeyCaptureRow["id"] : ""
    conflict := GetHotkeyConflictAction(chord, exceptId)
    if (conflict != "") {
        TrayTip("Already used by " conflict ".", "Hotkeys", "Iconx")
        return
    }

    hook.Stop()
    gHotkeyCaptureRow["pending"] := chord
    gHotkeyCaptureRow["button"].Text := FormatHotkeyDisplay(chord)
    gHotkeyCaptureRow := 0
    gHotkeyCaptureHook := 0
    if onDone is Func
        onDone(chord)
}
