#Requires AutoHotkey v2.0

global _WindowLayout_DefaultLayout := "Grid2x2"
global _WindowLayout_MainCharacter  := ""
global _WindowLayout_TargetMonitor  := 0  ; 0 = primary

RegisterAddon(Map(
    "name",          "WindowLayout",
    "settingsLabel", "Window Layout",
    "OnTrayMenu",    _WindowLayout_OnTrayMenu,
    "OnSettings",    _WindowLayout_OnSettings,
    "OnInit",        _WindowLayout_OnInit
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

; Contributes the Window Layout tab to the Settings window. ctx = { gui, tab,
; saveHandlers }; the core has already selected this addon's tab and added a
; Section anchor, so controls position with xs/ys.
_WindowLayout_OnSettings(ctx) {
    global _WindowLayout_DefaultLayout, _WindowLayout_MainCharacter, _WindowLayout_TargetMonitor

    g := ctx.gui
    presets := ["Reset", "Single", "Grid2x2", "Grid3x2", "CenterFocus", "DiceLeft", "DiceRight"]

    g.Add("Text", "xs y+16 w130", "Default layout:")
    layoutDdl := g.Add("DropDownList", "x+10 yp-3 w200", presets)
    layoutDdl.Value := _WindowLayout_IndexOf(presets, _WindowLayout_DefaultLayout, 3)  ; default Grid2x2

    ; Main character — seed the combo with any currently-detected names plus the
    ; saved value, but allow free text so it can be set with no game windows open.
    names := _WindowLayout_GetCharacterNames()
    if (_WindowLayout_MainCharacter != "" && !_WindowLayout_ArrayHas(names, _WindowLayout_MainCharacter))
        names.InsertAt(1, _WindowLayout_MainCharacter)
    g.Add("Text", "xs y+14 w130", "Main character:")
    charCombo := g.Add("ComboBox", "x+10 yp-3 w200", names)
    charCombo.Text := _WindowLayout_MainCharacter

    g.Add("Text", "xs y+14 w130", "Target display:")
    monChoices := ["Primary (auto)"]
    Loop MonitorGetCount()
        monChoices.Push(_WindowLayout_DisplayLabel(A_Index))
    displayDdl := g.Add("DropDownList", "x+10 yp-3 w220", monChoices)
    displayDdl.Value := (_WindowLayout_TargetMonitor >= 1 && _WindowLayout_TargetMonitor <= MonitorGetCount())
        ? _WindowLayout_TargetMonitor + 1 : 1

    ctx.saveHandlers.Push(() => _WindowLayout_ApplySettings(
        presets[layoutDdl.Value], Trim(charCombo.Text),
        (displayDdl.Value <= 1) ? 0 : displayDdl.Value - 1))
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

    chosen := ""
    dlg := Gui("+AlwaysOnTop -MinimizeBox", "Window Layout — Main Character")
    dlg.Add("Text", "w220", "Select the main character:")
    ddl := dlg.Add("DropDownList", "w220", names)
    for i, n in names
        if n = _WindowLayout_MainCharacter {
            ddl.Value := i
            break
        }
    dlg.Add("Button", "Default w80 xm y+10", "OK").OnEvent("Click", (*) => (chosen := ddl.Text, dlg.Destroy()))
    dlg.Add("Button", "w80 x+8", "Cancel").OnEvent("Click", (*) => dlg.Destroy())
    dlg.OnEvent("Close", (*) => dlg.Destroy())
    dlg.Show("AutoSize")
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
        title := WinGetTitle("ahk_id " hwnd)
        if RegExMatch(title, "Behemoth:\s+(.+?)\s+ID\b", &m) {
            name := m[1]
            if !seen.Has(name) {
                seen[name] := true
                names.Push(name)
            }
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
