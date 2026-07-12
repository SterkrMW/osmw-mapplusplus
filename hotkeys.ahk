#Requires AutoHotkey v2.0

; ── Central hotkey registry ──────────────────────────────────────
; Actions register via RegisterHotkeyAction(); overrides live in config.ini
; [Hotkeys]. ApplyAllHotkeys() binds at runtime with Hotkey().

global gHotkeyActions := Map()       ; id → action spec
global gAppliedHotkeys := []         ; { chord, hotIf } for teardown on re-apply
global gHotkeyReserved := ["^!r", "^!q", "^!d", "^!s", "^!v", "^!1", "^!2", "^!3", "^!4"]

RegisterHotkeyAction(spec) {
    global gHotkeyActions
    if !spec.Has("id") || spec["id"] = "" {
        TrayTip("Hotkeys", "RegisterHotkeyAction() missing id — ignored.", "Iconx")
        return
    }
    id := spec["id"]
    if !spec.Has("default") || spec["default"] = ""
        spec["default"] := spec.Has("chord") ? spec["chord"] : ""
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

NormalizeHotkeyChord(chord) {
    chord := Trim(chord)
    if (chord = "")
        return ""
    return chord
}

; Escape trailing comma for Hotkey() API (^!, → ^!`,).
ToHotkeyApiName(chord) {
    chord := NormalizeHotkeyChord(chord)
    if RegExMatch(chord, ",$")
        return SubStr(chord, 1, -1) "``,"
    return chord
}

IsHotkeyChordValid(chord) {
    chord := NormalizeHotkeyChord(chord)
    if (chord = "")
        return false
    modChars := "^!+#"
    i := 1
    while (i <= StrLen(chord)) {
        if !InStr(modChars, SubStr(chord, i, 1))
            break
        i++
    }
    key := SubStr(chord, i)
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
        IniWrite(action["chord"], CONFIG_INI, "Hotkeys", id)
    }
}

_ApplyActionHotIf(action) {
    if action.Has("hotIfWinActive") && action["hotIfWinActive"]
        HotIfWinActive(GAME_WIN_FILTER)
    else if action.Has("hotIfFn") && action["hotIfFn"] is Func
        HotIf(action["hotIfFn"])
    else
        HotIf
}

_HotkeyContextMode(action) {
    if action.Has("hotIfWinActive") && action["hotIfWinActive"]
        return "winActive"
    if action.Has("hotIfFn") && action["hotIfFn"] is Func
        return "callback"
    return "global"
}

_ApplyHotkeyContext(entry) {
    if !(entry is Map)
        return
    mode := entry.Has("mode") ? entry["mode"] : "global"
    if (mode = "winActive")
        HotIfWinActive(GAME_WIN_FILTER)
    else if (mode = "callback") && entry.Has("hotIfFn") && entry["hotIfFn"] is Func
        HotIf(entry["hotIfFn"])
    else
        HotIf
}

_ApplyHotkeyOff(entry) {
    if !(entry is Map)
        return
    _ApplyHotkeyContext(entry)
    try Hotkey(entry["chord"], "Off")
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

    for id, action in gHotkeyActions {
        if !IsHotkeyActionEnabled(action)
            continue
        chord := action["chord"]
        if (chord = "") || !IsHotkeyChordValid(chord)
            continue

        _ApplyActionHotIf(action)

        usePrefix := action.Has("passThrough") && action["passThrough"]
        bindChord := (usePrefix ? "$" : "") ToHotkeyApiName(chord)
        handler := action["handler"]
        mode := _HotkeyContextMode(action)
        hotIfFn := action.Has("hotIfFn") ? action["hotIfFn"] : 0
        try {
            Hotkey(bindChord, _HotkeyWrapHandler(handler))
            gAppliedHotkeys.Push(Map(
                "chord", bindChord,
                "mode", mode,
                "hotIfFn", hotIfFn
            ))
        } catch as err {
            TrayTip("Hotkeys", "Failed to bind " id ": " err.Message, "Iconx")
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
        "label", "Launch game (primary monitor)",
        "category", "Core",
        "default", "^!l",
        "handler", (*) => LaunchGameInstance("primary")
    ))
    RegisterHotkeyAction(Map(
        "id", "launchSecondary",
        "label", "Launch game (secondary monitor)",
        "category", "Core",
        "default", "^!k",
        "handler", (*) => LaunchGameInstance("secondary")
    ))
    RegisterHotkeyAction(Map(
        "id", "launchClientsLayout",
        "label", "Launch clients + apply layout",
        "category", "Core",
        "default", "^!5",
        "handler", LaunchClientsAndApplyLayout
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
    categoryOrder := ["Core", "Window Layout", "Inventory", "Battle", "Chat"]
    byCategory := Map()
    for id, action in gHotkeyActions {
        if !IsHotkeyActionEnabled(action)
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
        TrayTip("Hotkeys", "Already used by " conflict ".", "Iconx")
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
