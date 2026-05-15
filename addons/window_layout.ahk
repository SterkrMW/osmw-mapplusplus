#Requires AutoHotkey v2.0

global _WindowLayout_DefaultLayout := "SideBySide"
global _WindowLayout_MainCharacter  := ""
global _WindowLayout_DefaultMenu    := 0

RegisterAddon(Map(
    "name",       "WindowLayout",
    "OnTrayMenu", _WindowLayout_OnTrayMenu,
    "OnInit",     _WindowLayout_OnInit
))

^+l:: _WindowLayout_ApplyDefaultLayout()

_WindowLayout_OnInit() {
    global _WindowLayout_MainCharacter
    _WindowLayout_LoadConfig()
    if (_WindowLayout_MainCharacter = "")
        _WindowLayout_PromptMainCharacter()
}

_WindowLayout_OnTrayMenu(trayMenu) {
    global _WindowLayout_DefaultLayout, _WindowLayout_DefaultMenu

    layoutMenu := Menu()
    layoutMenu.Add("Apply`tCtrl+Shift+L", (*) => _WindowLayout_ApplyDefaultLayout())

    applyMenu := Menu()
    for name in ["Reset", "Single", "Grid2x2", "CenterFocus"]
        applyMenu.Add(name, _WindowLayout_ApplyPreset.Bind(name))
    layoutMenu.Add("Apply Preset", applyMenu)

    layoutMenu.Add("Set Main Character...", (*) => _WindowLayout_PromptMainCharacter())

    defaultMenu := Menu()
    for name in ["Reset", "Single", "Grid2x2", "CenterFocus"]
        defaultMenu.Add(name, _WindowLayout_SetDefaultLayout.Bind(name))
    try defaultMenu.Check(_WindowLayout_DefaultLayout)
    _WindowLayout_DefaultMenu := defaultMenu
    layoutMenu.Add("Set Default", defaultMenu)

    trayMenu.Add("Window Layout", layoutMenu)
}

_WindowLayout_LoadConfig() {
    global _WindowLayout_DefaultLayout, _WindowLayout_MainCharacter, CONFIG_INI
    _WindowLayout_DefaultLayout := Trim(IniRead(CONFIG_INI, "WindowLayout", "DefaultLayout", "Grid2x2"))
    _WindowLayout_MainCharacter := Trim(IniRead(CONFIG_INI, "WindowLayout", "MainCharacter", ""))
}

_WindowLayout_SaveConfig() {
    global _WindowLayout_DefaultLayout, _WindowLayout_MainCharacter, CONFIG_INI
    IniWrite(_WindowLayout_DefaultLayout, CONFIG_INI, "WindowLayout", "DefaultLayout")
    IniWrite(_WindowLayout_MainCharacter, CONFIG_INI, "WindowLayout", "MainCharacter")
}

_WindowLayout_PromptMainCharacter() {
    global _WindowLayout_MainCharacter

    names := _WindowLayout_GetCharacterNames()
    if names.Length = 0 {
        MsgBox("No game windows found. Open the game first, then try again.", "Window Layout — Main Character", "Icon!")
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
    for hwnd in _WindowLayout_GetTopLevelGameWindows() {
        title := WinGetTitle("ahk_id " hwnd)
        if RegExMatch(title, "Server:\s+(.+?)\s+ID\b", &m) {
            name := m[1]
            if !seen.Has(name) {
                seen[name] := true
                names.Push(name)
            }
        }
    }
    return names
}

_WindowLayout_ApplyDefaultLayout() {
    global _WindowLayout_DefaultLayout
    _WindowLayout_ApplyPreset(_WindowLayout_DefaultLayout)
}

_WindowLayout_ApplyPreset(layoutName, *) {
    static validLayouts := ["Reset", "Single", "Grid2x2", "CenterFocus"]
    found := false
    for n in validLayouts
        if n = layoutName
            found := true
    if !found
        return
    windows := _WindowLayout_GetTopLevelGameWindows()
    if (windows.Length = 0)
        return
    ordered := _WindowLayout_OrderWindows(windows)

    if layoutName = "Reset" {
        for hwnd in ordered
            WinMove(0, 0, , , "ahk_id " hwnd)
        _WindowLayout_ActivateMainCharacter(ordered)
        return
    }

    ; Use the first window's size to compute positions — never resize.
    WinGetPos(, , &winW, &winH, "ahk_id " ordered[1])
    slots := _WindowLayout_ComputeSlots(layoutName, winW, winH)

    Loop ordered.Length {
        hwnd := ordered[A_Index]
        slot := slots[Mod(A_Index - 1, slots.Length) + 1]
        WinMove(slot.x, slot.y, , , "ahk_id " hwnd)
    }
    _WindowLayout_ActivateMainCharacter(ordered)
}

_WindowLayout_ComputeSlots(layoutName, winW, winH) {
    MonitorGetWorkArea(, &wl, &wt, &wr, &wb)
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
    if layoutName = "CenterFocus"
        return [
            {x: ox + (sw - winW) // 2, y: oy + (sh - winH) // 2},  ; center — main char
            {x: ox,                    y: oy},                        ; top-left
            {x: ox + sw - winW,        y: oy},                        ; top-right
            {x: ox,                    y: oy + sh - winH},            ; bottom-left
            {x: ox + sw - winW,        y: oy + sh - winH}             ; bottom-right
        ]
    return []
}

_WindowLayout_GetTopLevelGameWindows() {
    global GAME_WIN_FILTER
    result := []
    for hwnd in WinGetList(GAME_WIN_FILTER) {
        ; Skip child windows and owned popups (chat panel, sub-dialogs, etc.)
        if DllCall("GetParent", "Ptr", hwnd, "Ptr") != 0
            continue
        if DllCall("GetWindow", "Ptr", hwnd, "UInt", 4, "Ptr") != 0  ; GW_OWNER = 4
            continue
        result.Push(hwnd)
    }
    return result
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

_WindowLayout_SetDefaultLayout(name, *) {
    global _WindowLayout_DefaultLayout, _WindowLayout_DefaultMenu
    prev := _WindowLayout_DefaultLayout
    _WindowLayout_DefaultLayout := name
    _WindowLayout_SaveConfig()
    if IsObject(_WindowLayout_DefaultMenu) {
        try _WindowLayout_DefaultMenu.Uncheck(prev)
        try _WindowLayout_DefaultMenu.Check(name)
    }
}
