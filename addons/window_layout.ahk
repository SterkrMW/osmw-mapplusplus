#Requires AutoHotkey v2.0

global _WindowLayout_DefaultLayout := "Grid2x2"
global _WindowLayout_MainCharacter  := ""
global _WindowLayout_TargetMonitor  := 0  ; 0 = primary

RegisterAddon(Map(
    "name",              "WindowLayout",
    "settingsLabel",     "Window Layout",
    "OnTrayMenu",        _WindowLayout_OnTrayMenu,
    "OnSettingsWeb",     _WindowLayout_OnSettingsWeb,
    "OnSettingsWebSave", _WindowLayout_OnSettingsWebSave,
    "OnInit",            _WindowLayout_OnInit
))

RegisterHotkeyAction(Map(
    "id", "windowLayoutPrimary",
    "label", "Apply default layout (primary monitor)",
    "category", "Window Layout",
    "default", "^+l",
    "addon", "WindowLayout",
    "handler", (*) => _WindowLayout_ApplyDefaultLayout(MonitorGetPrimary()),
    "hotIfWinActive", true
))
RegisterHotkeyAction(Map(
    "id", "windowLayoutSecondary",
    "label", "Apply default layout (secondary monitor)",
    "category", "Window Layout",
    "default", "^+k",
    "addon", "WindowLayout",
    "handler", (*) => _WindowLayout_ApplyDefaultLayout(GetSecondaryMonitorIndex()),
    "hotIfWinActive", true
))

_WindowLayout_OnInit() {
    global _WindowLayout_MainCharacter
    _WindowLayout_LoadConfig()
    if (_WindowLayout_MainCharacter = "")
        _WindowLayout_PromptMainCharacter()
}

_WindowLayout_OnTrayMenu(trayMenu) {
    ; Action items only — configuration (default layout, main character, target
    ; display) lives in the Settings window's Window Layout tab.
    layoutMenu := Menu()
    layoutMenu.Add("Apply (Primary)`t" GetHotkeyDisplay("windowLayoutPrimary"), (*) => _WindowLayout_ApplyDefaultLayout(MonitorGetPrimary()))
    layoutMenu.Add("Apply (Secondary)`t" GetHotkeyDisplay("windowLayoutSecondary"), (*) => _WindowLayout_ApplyDefaultLayout(GetSecondaryMonitorIndex()))

    applyMenu := Menu()
    for name in ["Reset", "Single", "Grid2x2", "Grid3x2", "CenterFocus", "DiceLeft", "DiceRight"]
        applyMenu.Add(name, _WindowLayout_ApplyPreset.Bind(name))
    layoutMenu.Add("Apply Preset", applyMenu)

    trayMenu.Add("Window Layout", layoutMenu)
}

_WindowLayout_OnSettingsWeb() {
    global _WindowLayout_DefaultLayout, _WindowLayout_MainCharacter, _WindowLayout_TargetMonitor
    presets := ["Reset", "Single", "Grid2x2", "Grid3x2", "CenterFocus", "DiceLeft", "DiceRight"]

    names := _WindowLayout_GetCharacterNames()
    if (_WindowLayout_MainCharacter != "" && !_WindowLayout_ArrayHas(names, _WindowLayout_MainCharacter))
        names.InsertAt(1, _WindowLayout_MainCharacter)

    monChoices := ["Primary (auto)"]
    Loop MonitorGetCount()
        monChoices.Push(_WindowLayout_DisplayLabel(A_Index))

    layoutIdx := _WindowLayout_IndexOf(presets, _WindowLayout_DefaultLayout, 3) - 1
    monIdx := (_WindowLayout_TargetMonitor >= 1 && _WindowLayout_TargetMonitor <= MonitorGetCount())
        ? _WindowLayout_TargetMonitor : 0

    return [
        Map("type", "dropdown", "id", "layoutIdx", "label", "Default layout:",
            "options", presets, "value", layoutIdx),
        Map("type", "combo", "id", "mainChar", "label", "Main character:",
            "options", names, "value", _WindowLayout_MainCharacter),
        Map("type", "dropdown", "id", "targetMonitorIdx", "label", "Target display:",
            "options", monChoices, "value", monIdx)
    ]
}

_WindowLayout_OnSettingsWebSave(values) {
    presets := ["Reset", "Single", "Grid2x2", "Grid3x2", "CenterFocus", "DiceLeft", "DiceRight"]
    layoutIdx := values.Has("layoutIdx") ? Integer(values["layoutIdx"]) + 1 : 3
    if (layoutIdx < 1 || layoutIdx > presets.Length)
        layoutIdx := 3

    mainChar := values.Has("mainChar") ? Trim(values["mainChar"]) : ""
    targetMonIdx := values.Has("targetMonitorIdx") ? Integer(values["targetMonitorIdx"]) : 0

    _WindowLayout_ApplySettings(presets[layoutIdx], mainChar, targetMonIdx)
}

; Persists the values chosen in the Settings window's Window Layout tab.
_WindowLayout_ApplySettings(layout, mainChar, targetMonitor) {
    global _WindowLayout_DefaultLayout, _WindowLayout_MainCharacter, _WindowLayout_TargetMonitor
    _WindowLayout_DefaultLayout := layout
    _WindowLayout_MainCharacter := mainChar
    _WindowLayout_TargetMonitor := targetMonitor
    _WindowLayout_SaveConfig()
}

; 1-based position of needle in arr, or fallbackIdx when not present.
_WindowLayout_IndexOf(arr, needle, fallbackIdx) {
    for i, v in arr
        if (v = needle)
            return i
    return fallbackIdx
}

_WindowLayout_ArrayHas(arr, needle) {
    for v in arr
        if (v = needle)
            return true
    return false
}

_WindowLayout_LoadConfig() {
    global _WindowLayout_DefaultLayout, _WindowLayout_MainCharacter, _WindowLayout_TargetMonitor, CONFIG_INI
    _WindowLayout_DefaultLayout := Trim(IniRead(CONFIG_INI, "WindowLayout", "DefaultLayout", "Grid2x2"))
    _WindowLayout_MainCharacter := Trim(IniRead(CONFIG_INI, "WindowLayout", "MainCharacter", ""))
    _WindowLayout_TargetMonitor := Integer(IniRead(CONFIG_INI, "WindowLayout", "TargetMonitor", 0))
}

_WindowLayout_SaveConfig() {
    global _WindowLayout_DefaultLayout, _WindowLayout_MainCharacter, _WindowLayout_TargetMonitor, CONFIG_INI
    IniWrite(_WindowLayout_DefaultLayout, CONFIG_INI, "WindowLayout", "DefaultLayout")
    IniWrite(_WindowLayout_MainCharacter, CONFIG_INI, "WindowLayout", "MainCharacter")
    IniWrite(_WindowLayout_TargetMonitor, CONFIG_INI, "WindowLayout", "TargetMonitor")
}

_WindowLayout_PromptMainCharacter() {
    global _WindowLayout_MainCharacter

    if GetTopLevelGameWindows().Length = 0
        return

    names := _WindowLayout_GetCharacterNames()
    if names.Length = 0 {
        return
    }

    if IsNativeInterface() {
        chosen := ""
        dlg := Gui("+AlwaysOnTop -MinimizeBox", "Window Layout — Main Character")
        dlg.SetFont("s9", "Segoe UI")
        dlg.Add("Text", "w240", "Select the main character:")
        ddl := dlg.Add("DropDownList", "w240", names)
        for i, name in names {
            if (name = _WindowLayout_MainCharacter) {
                ddl.Value := i
                break
            }
        }
        if (ddl.Value = 0)
            ddl.Value := 1
        dlg.Add("Button", "Default w90 xm y+12", "OK")
            .OnEvent("Click", (*) => (chosen := ddl.Text, dlg.Destroy()))
        dlg.Add("Button", "w90 x+8", "Cancel").OnEvent("Click", (*) => dlg.Destroy())
        dlg.OnEvent("Close", (*) => dlg.Destroy())
        dlg.OnEvent("Escape", (*) => dlg.Destroy())
        dlg.Show("AutoSize")
        WinWaitClose("ahk_id " dlg.Hwnd)
        if (chosen != "") {
            _WindowLayout_MainCharacter := chosen
            _WindowLayout_SaveConfig()
        }
        return
    }

    chosen := ""
    dllDir := (A_PtrSize = 8) ? "64bit" : "32bit"
    dllPath := A_ScriptDir "\Lib\" dllDir "\WebView2Loader.dll"
    wvSettings := { DllPath: dllPath, DefaultWidth: 320, DefaultHeight: 200 }

    dlg := WebViewGui("-Caption +AlwaysOnTop -Resize", "Window Layout — Main Character",, wvSettings)

    namesJson := "["
    for i, n in names {
        if (i > 1) namesJson .= ","
        namesJson .= _JSON_Str(n)
    }
    namesJson .= "]"

    dlg.WebMessageReceived(OnWebMsg)
    dlg.DOMContentLoaded((*) => dlg.PostWebMessageAsJson('{"type":"init-prompt"'
        . ',"names":' namesJson
        . ',"selectedName":' _JSON_Str(_WindowLayout_MainCharacter) '}'))

    dlg.Navigate("ui/window_layout/prompt.html")
    dlg.Show("w320 h200")

    OnWebMsg(wv, args) {
        msgStr := ""
        try msgStr := args.TryGetWebMessageAsString()
        if (msgStr = "") {
            try msgStr := args.WebMessageAsJson
        }
        if (msgStr = "")
            return

        msg := _JSON_Parse(msgStr)
        if !IsObject(msg) || !msg.Has("type")
            return

        switch msg["type"] {
            case "init-request":
                dlg.PostWebMessageAsJson('{"type":"init-prompt"'
                    . ',"names":' namesJson
                    . ',"selectedName":' _JSON_Str(_WindowLayout_MainCharacter) '}')
            case "accept":
                if msg.Has("chosen") && msg["chosen"] != "" {
                    chosen := msg["chosen"]
                }
                dlg.Destroy()
            case "cancel":
                dlg.Destroy()
        }
    }

    WinWaitClose("ahk_id " dlg.Hwnd)

    if chosen != "" {
        _WindowLayout_MainCharacter := chosen
        _WindowLayout_SaveConfig()
    }
}

_WindowLayout_GetCharacterNames() {
    seen  := Map()
    names := []
    for hwnd in GetTopLevelGameWindows() {
        ; Shared with the roster and Discord presence — one title format to fix.
        name := CharacterNameFromWindow(hwnd)
        if (name != "" && !seen.Has(name)) {
            seen[name] := true
            names.Push(name)
        }
    }
    return names
}

_WindowLayout_ApplyDefaultLayout(monitorIdx := unset) {
    global _WindowLayout_DefaultLayout
    _WindowLayout_ApplyPreset(_WindowLayout_DefaultLayout, monitorIdx)
}

_WindowLayout_ApplyPreset(layoutName, monitorIdx := unset, *) {
    static validLayouts := ["Reset", "Single", "Grid2x2", "Grid3x2", "CenterFocus", "DiceLeft", "DiceRight"]
    found := false
    for n in validLayouts
        if n = layoutName
            found := true
    if !found
        return
    monIdx := (IsSet(monitorIdx) && monitorIdx is Integer) ? monitorIdx : _WindowLayout_ResolveMonitor()
    windows := FilterWindowsOnMonitor(GetTopLevelGameWindows(), monIdx)
    if (windows.Length = 0)
        return
    ordered := _WindowLayout_OrderWindows(windows)

    if layoutName = "Reset" {
        MonitorGetWorkArea(monIdx, &wl, &wt)
        for hwnd in ordered
            WinMove(wl, wt, , , "ahk_id " hwnd)
        _WindowLayout_ActivateMainCharacter(ordered)
        return
    }

    ; Use the first window's size to compute positions — never resize.
    WinGetPos(, , &winW, &winH, "ahk_id " ordered[1])
    slots := _WindowLayout_ComputeSlots(layoutName, winW, winH, monIdx)

    Loop ordered.Length {
        hwnd := ordered[A_Index]
        slot := slots[Mod(A_Index - 1, slots.Length) + 1]
        WinMove(slot.x, slot.y, , , "ahk_id " hwnd)
    }
    _WindowLayout_ActivateMainCharacter(ordered)
}

_WindowLayout_ComputeSlots(layoutName, winW, winH, monIdx) {
    MonitorGetWorkArea(monIdx, &wl, &wt, &wr, &wb)
    sw := wr - wl   ; usable width  (excludes taskbar)
    sh := wb - wt   ; usable height (excludes taskbar)
    ox := wl        ; x origin — non-zero when taskbar is on the left
    oy := wt        ; y origin — non-zero when taskbar is on the top
    if layoutName = "Single"
        return [{x: ox + (sw - winW) // 2, y: oy + (sh - winH) // 2}]
    if layoutName = "Grid2x2"
        return [
            {x: ox,        y: oy},
            {x: ox + winW, y: oy},
            {x: ox,        y: oy + winH},
            {x: ox + winW, y: oy + winH}
        ]
    if layoutName = "Grid3x2" {
        pad  := 8
        gridW := 3 * winW + 2 * pad
        gridH := 2 * winH + pad
        bx   := ox + Max(0, (sw - gridW) // 2)
        by   := oy + Max(0, (sh - gridH) // 2)
        col1 := bx
        col2 := bx + winW + pad
        col3 := bx + 2 * (winW + pad)
        row2 := by + winH + pad
        return [
            {x: col1, y: by},   {x: col2, y: by},   {x: col3, y: by},
            {x: col1, y: row2}, {x: col2, y: row2}, {x: col3, y: row2}
        ]
    }
    if layoutName = "CenterFocus"
        return [
            {x: ox + (sw - winW) // 2, y: oy + (sh - winH) // 2},  ; center — main char
            {x: ox,                    y: oy},                        ; top-left
            {x: ox + sw - winW,        y: oy},                        ; top-right
            {x: ox,                    y: oy + sh - winH},            ; bottom-left
            {x: ox + sw - winW,        y: oy + sh - winH}             ; bottom-right
        ]
    if layoutName = "DiceLeft" || layoutName = "DiceRight" {
        clusterW := 2 * winW
        bx := (layoutName = "DiceLeft") ? ox : ox + sw - clusterW
        topY    := oy
        bottomY := oy + sh - winH
        centerY := oy + (sh - winH) // 2
        return [
            {x: bx + winW // 2, y: centerY},  ; center — main char (drawn on top)
            {x: bx,             y: topY},     ; top-left
            {x: bx + winW,      y: topY},     ; top-right
            {x: bx,             y: bottomY},  ; bottom-left
            {x: bx + winW,      y: bottomY}   ; bottom-right
        ]
    }
    return []
}

_WindowLayout_OrderWindows(windows) {
    global _WindowLayout_MainCharacter
    mainHwnd := 0
    others   := []
    for hwnd in windows {
        title := WinGetTitle("ahk_id " hwnd)
        if (_WindowLayout_MainCharacter != "" && InStr(title, _WindowLayout_MainCharacter, false)) {
            if !mainHwnd
                mainHwnd := hwnd
            else
                others.Push(hwnd)
        } else {
            others.Push(hwnd)
        }
    }
    result := []
    if mainHwnd
        result.Push(mainHwnd)
    for hwnd in others
        result.Push(hwnd)
    return result
}

_WindowLayout_ActivateMainCharacter(ordered) {
    global _WindowLayout_MainCharacter
    if _WindowLayout_MainCharacter = "" || ordered.Length = 0
        return
    title := WinGetTitle("ahk_id " ordered[1])
    if InStr(title, _WindowLayout_MainCharacter, false)
        WinActivate("ahk_id " ordered[1])
}

_WindowLayout_DisplayLabel(idx) {
    if idx = 0
        return "Primary (auto)"
    try {
        MonitorGet(idx, &l, &t, &r, &b)
        label := "Display " idx " — " (r - l) "×" (b - t)
        if idx = MonitorGetPrimary()
            label .= " (Primary)"
        return label
    }
    return "Display " idx
}

_WindowLayout_ResolveMonitor() {
    global _WindowLayout_TargetMonitor
    idx := _WindowLayout_TargetMonitor
    if idx < 1 || idx > MonitorGetCount()
        idx := MonitorGetPrimary()
    return idx
}
