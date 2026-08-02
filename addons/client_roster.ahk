#Requires AutoHotkey v2.0

; A compact always-on-top list of every running client — character, zone and
; status — so you can see which alt is stuck at a login prompt or sitting in a
; battle without alt-tabbing through all of them. Double-click a row to focus
; that client. All data comes from the core's shared client poll (OnSnapshot);
; this addon reads no memory itself.

global _ClientRoster_Gui := 0
global _ClientRoster_List := 0
global _ClientRoster_Visible := false
; Off by default: the roster is a thing you ask for, not something that should
; appear over the game every time a second client starts or Maps++ reloads.
; (Config key renamed from the old "AutoShow" — that defaulted to on and was
; written into everyone's config, so the old value is deliberately ignored.)
global _ClientRoster_AutoShow := false
; Set when the user closes the window by hand, so auto-show doesn't drag it
; back on the next poll. Cleared once every client has exited.
global _ClientRoster_SuppressAuto := false
; Set when the window was opened by hand rather than by auto-show. A window the
; user asked for stays open with an empty list when no clients are running; an
; auto-shown one closes itself.
global _ClientRoster_ManualOpen := false
global _ClientRoster_X := ""             ; last position ("" = let Windows place it)
global _ClientRoster_Y := ""
; hwnd per displayed row, so a double-click can focus the right client.
global _ClientRoster_RowHwnds := []

RegisterAddon(Map(
    "name",          "ClientRoster",
    "settingsLabel", "Client Roster",
    "OnInit",        _ClientRoster_OnInit,
    "OnSettingsWeb",     _ClientRoster_OnSettingsWeb,
    "OnSettingsWebSave", _ClientRoster_OnSettingsWebSave,
    "OnTrayMenu",    _ClientRoster_OnTrayMenu,
    "OnSnapshot",    _ClientRoster_OnSnapshot
))

RegisterHotkeyAction(Map(
    "id", "clientRosterToggle",
    "label", "Show/hide client roster",
    "category", "Client Roster",
    "default", "^!m",
    "addon", "ClientRoster",
    "handler", (*) => _ClientRoster_Toggle()
))

_ClientRoster_OnInit() {
    _ClientRoster_LoadConfig()
    OnExit(_ClientRoster_OnExit)
}

_ClientRoster_OnExit(*) {
    _ClientRoster_SavePosition()
}

_ClientRoster_OnTrayMenu(trayMenu) {
    trayMenu.Add("Client Roster`t" GetHotkeyDisplay("clientRosterToggle"), (*) => _ClientRoster_Toggle())
}

_ClientRoster_OnSettingsWeb() {
    global _ClientRoster_AutoShow
    return [
        Map("type", "info", "text",
            "Lists every running client with its character, zone and status.`n"
            . "Double-click a row to focus that client.`n"
            . "Open it with " GetHotkeyDisplay("clientRosterToggle") " or from the tray menu."),
        Map("type", "checkbox", "id", "autoShow", "label",
            "Also open it automatically when 2 or more clients are running",
            "value", _ClientRoster_AutoShow ? true : false)
    ]
}

_ClientRoster_OnSettingsWebSave(values) {
    _ClientRoster_ApplySettings(values.Has("autoShow") && values["autoShow"] ? true : false)
}

_ClientRoster_ApplySettings(autoShow) {
    global _ClientRoster_AutoShow
    _ClientRoster_AutoShow := autoShow
    _ClientRoster_SaveConfig()
}

_ClientRoster_LoadConfig() {
    global _ClientRoster_AutoShow, _ClientRoster_X, _ClientRoster_Y, CONFIG_INI
    _ClientRoster_AutoShow := (Trim(IniRead(CONFIG_INI, "ClientRoster", "AutoShowWithClients", "0")) = "1")
    x := Trim(IniRead(CONFIG_INI, "ClientRoster", "X", ""))
    y := Trim(IniRead(CONFIG_INI, "ClientRoster", "Y", ""))
    if (IsInteger(x) && IsInteger(y)) {
        _ClientRoster_X := Integer(x)
        _ClientRoster_Y := Integer(y)
    }
}

_ClientRoster_SaveConfig() {
    global _ClientRoster_AutoShow, CONFIG_INI
    IniWrite(_ClientRoster_AutoShow ? "1" : "0", CONFIG_INI, "ClientRoster", "AutoShowWithClients")
}

_ClientRoster_SavePosition() {
    global _ClientRoster_Gui, _ClientRoster_X, _ClientRoster_Y, CONFIG_INI
    if !IsObject(_ClientRoster_Gui) {
        return
    }
    try {
        WinGetPos(&x, &y, , , "ahk_id " _ClientRoster_Gui.Hwnd)
    } catch {
        return
    }
    _ClientRoster_X := x
    _ClientRoster_Y := y
    IniWrite(x, CONFIG_INI, "ClientRoster", "X")
    IniWrite(y, CONFIG_INI, "ClientRoster", "Y")
}

; ── Window ───────────────────────────────────────────────────────

_ClientRoster_Toggle() {
    global _ClientRoster_Visible
    if _ClientRoster_Visible {
        _ClientRoster_UserHide()
        return
    }
    _ClientRoster_Show(true)
    ; Poll now rather than showing a stale (or empty) list for up to a second.
    UpdateClientSnapshots()
    _ClientRoster_OnSnapshot(GetClientSnapshots())
}

; Closed by the user — stays closed until they ask for it again (or every
; client exits), instead of being re-opened by the next auto-show check.
_ClientRoster_UserHide() {
    global _ClientRoster_SuppressAuto
    _ClientRoster_SuppressAuto := true
    _ClientRoster_Hide()
}

_ClientRoster_Show(manual := false) {
    global _ClientRoster_Gui, _ClientRoster_Visible, _ClientRoster_X, _ClientRoster_Y
    global _ClientRoster_SuppressAuto, _ClientRoster_ManualOpen
    _ClientRoster_SuppressAuto := false
    _ClientRoster_ManualOpen := manual
    _ClientRoster_EnsureGui()
    opts := "NoActivate AutoSize"
    if (_ClientRoster_X != "" && _ClientRoster_Y != "") {
        opts := "x" _ClientRoster_X " y" _ClientRoster_Y " NoActivate AutoSize"
    }
    _ClientRoster_Gui.Show(opts)
    _ClientRoster_Visible := true
}

_ClientRoster_Hide() {
    global _ClientRoster_Gui, _ClientRoster_Visible, _ClientRoster_ManualOpen
    _ClientRoster_ManualOpen := false
    if !IsObject(_ClientRoster_Gui) {
        return
    }
    _ClientRoster_SavePosition()
    try _ClientRoster_Gui.Hide()
    _ClientRoster_Visible := false
}

_ClientRoster_EnsureGui() {
    global _ClientRoster_Gui, _ClientRoster_List

    if IsObject(_ClientRoster_Gui) && _ClientRoster_Gui.Hwnd {
        return
    }
    ; +ToolWindow keeps it off the taskbar; WS_EX_NOACTIVATE (E0x08000000) so
    ; showing or clicking it never pulls focus away from the game.
    g := Gui("+AlwaysOnTop +ToolWindow -MaximizeBox +E0x08000000", "Maps++ Clients")
    g.MarginX := 6
    g.MarginY := 6
    g.SetFont("s9")
    lv := g.Add("ListView", "w330 r6 -Multi NoSortHdr Grid", ["Character", "Zone", "Status"])
    lv.OnEvent("DoubleClick", _ClientRoster_OnRowActivate)
    g.OnEvent("Close", (*) => _ClientRoster_UserHide())
    g.OnEvent("Escape", (*) => _ClientRoster_UserHide())
    _ClientRoster_Gui := g
    _ClientRoster_List := lv
}

_ClientRoster_OnRowActivate(lv, row) {
    global _ClientRoster_RowHwnds
    if (row < 1 || row > _ClientRoster_RowHwnds.Length) {
        return
    }
    hwnd := _ClientRoster_RowHwnds[row]
    if WinExist("ahk_id " hwnd) {
        WinActivate("ahk_id " hwnd)
    }
}

; ── Rows ─────────────────────────────────────────────────────────

_ClientRoster_OnSnapshot(snapshots) {
    global _ClientRoster_Visible, _ClientRoster_AutoShow, _ClientRoster_List, _ClientRoster_RowHwnds
    global _ClientRoster_SuppressAuto, _ClientRoster_ManualOpen

    if (snapshots.Length = 0) {
        ; Every client is gone — the next session starts from a clean slate.
        _ClientRoster_SuppressAuto := false
        ; A window the user opened stays put (with an empty list); one that
        ; opened itself closes itself.
        if (_ClientRoster_Visible && !_ClientRoster_ManualOpen) {
            _ClientRoster_Hide()
            return
        }
    }
    ; Single-boxers never see this window.
    if (_ClientRoster_AutoShow && !_ClientRoster_Visible && !_ClientRoster_SuppressAuto
        && snapshots.Length >= 2) {
        _ClientRoster_Show()
    }
    if (!_ClientRoster_Visible || !IsObject(_ClientRoster_List)) {
        return
    }

    rows := []
    _ClientRoster_List.Opt("-Redraw")
    _ClientRoster_List.Delete()
    for snap in snapshots {
        rows.Push(snap.hwnd)
        _ClientRoster_List.Add(,
            (snap.charName != "" ? snap.charName : "PID " snap.pid),
            _ClientRoster_ZoneText(snap),
            _ClientRoster_StatusText(snap))
    }
    _ClientRoster_RowHwnds := rows
    loop 3 {
        _ClientRoster_List.ModifyCol(A_Index, "AutoHdr")
    }
    _ClientRoster_List.Opt("+Redraw")
}

_ClientRoster_ZoneText(snap) {
    if (snap.mapName != "") {
        return snap.mapName
    }
    return (snap.mapId != "") ? snap.mapId : "—"
}

_ClientRoster_StatusText(snap) {
    global GAME_STATE_READY, GAME_STATE_WORLD
    if (snap.gameState < 0) {
        return "No read"
    }
    if snap.inBattle {
        return "In battle"
    }
    if (snap.gameState < GAME_STATE_READY) {
        return "Not ready"      ; still at the intro / login prompts
    }
    if (snap.gameState != GAME_STATE_WORLD) {
        return "Loading"
    }
    return "Playing"
}
