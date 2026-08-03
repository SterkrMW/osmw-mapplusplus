#Requires AutoHotkey v2.0

; Two views of every running client, both fed by the core's shared client poll
; (OnSnapshot) — this addon reads no memory itself.
;
;   Radial (default) — a ring of class avatars around the cursor. Click one to
;   focus that client. Fastest way to jump between alts mid-session.
;   List — a compact always-on-top table of character, zone and status, so you
;   can see which alt is stuck at a login prompt or sitting in a battle.
;   Double-click a row to focus that client.

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
; Which view the hotkey and the main tray entry open: "radial" or "list".
global _ClientRoster_DefaultView := "radial"

; The ring itself lives in the core engine (radial.ahk); this addon only
; supplies its items and says what a click means.
global CLIENT_RADIAL_RING := "clients"

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
    global CLIENT_RADIAL_RING
    _ClientRoster_LoadConfig()
    OnExit(_ClientRoster_OnExit)
    RadialRegister(Map(
        "name",     CLIENT_RADIAL_RING,
        "page",     "ui/radial/index.html",
        "title",    "Maps++ Radial — Clients",
        ; Open on the clients running now, not on the last poll's.
        "onBeforeOpen", (*) => UpdateClientSnapshots(),
        "getItems", _ClientRadial_Items,
        "getHub",   _ClientRadial_Hub,
        "onSelect", _ClientRadial_OnSelect,
        "onHub",    _ClientRadial_OnHub
    ))
    ; Build the radial window in the background once startup has settled, so
    ; the first press of the hotkey is as quick as every one after it.
    SetTimer(_ClientRadial_Prewarm, -5000)
}

_ClientRoster_OnExit(*) {
    _ClientRoster_SavePosition()
}

_ClientRoster_OnTrayMenu(trayMenu) {
    trayMenu.Add("Client Roster`t" GetHotkeyDisplay("clientRosterToggle"), (*) => _ClientRoster_Toggle())
    ; The list stays one click away whichever view the hotkey is set to.
    trayMenu.Add("Client Roster (list)", (*) => _ClientRoster_ToggleList())
}

_ClientRoster_OnSettingsWeb() {
    global _ClientRoster_AutoShow, _ClientRoster_DefaultView
    return [
        Map("type", "info", "text",
            "Shows every running client so you can jump straight to an alt.`n"
            . "The radial menu opens a ring of class avatars around the cursor — click one to focus it.`n"
            . "The list shows character, zone and status; double-click a row to focus that client.`n"
            . "Open it with " GetHotkeyDisplay("clientRosterToggle") " or from the tray menu."),
        Map("type", "checkbox", "id", "listView", "label",
            "Open the list view instead of the radial menu",
            "value", (_ClientRoster_DefaultView = "list") ? true : false),
        Map("type", "checkbox", "id", "autoShow", "label",
            "Also open the list automatically when 2 or more clients are running",
            "value", _ClientRoster_AutoShow ? true : false)
    ]
}

_ClientRoster_OnSettingsWebSave(values) {
    _ClientRoster_ApplySettings(
        values.Has("autoShow") && values["autoShow"] ? true : false,
        values.Has("listView") && values["listView"] ? "list" : "radial")
}

_ClientRoster_ApplySettings(autoShow, defaultView := "radial") {
    global _ClientRoster_AutoShow, _ClientRoster_DefaultView
    _ClientRoster_AutoShow := autoShow
    _ClientRoster_DefaultView := (defaultView = "list") ? "list" : "radial"
    _ClientRoster_SaveConfig()
}

_ClientRoster_LoadConfig() {
    global _ClientRoster_AutoShow, _ClientRoster_X, _ClientRoster_Y, CONFIG_INI
    global _ClientRoster_DefaultView
    _ClientRoster_AutoShow := (Trim(IniRead(CONFIG_INI, "ClientRoster", "AutoShowWithClients", "0")) = "1")
    _ClientRoster_DefaultView := (Trim(IniRead(CONFIG_INI, "ClientRoster", "DefaultView", "radial")) = "list")
        ? "list" : "radial"
    x := Trim(IniRead(CONFIG_INI, "ClientRoster", "X", ""))
    y := Trim(IniRead(CONFIG_INI, "ClientRoster", "Y", ""))
    if (IsInteger(x) && IsInteger(y)) {
        _ClientRoster_X := Integer(x)
        _ClientRoster_Y := Integer(y)
    }
}

_ClientRoster_SaveConfig() {
    global _ClientRoster_AutoShow, _ClientRoster_DefaultView, CONFIG_INI
    IniWrite(_ClientRoster_AutoShow ? "1" : "0", CONFIG_INI, "ClientRoster", "AutoShowWithClients")
    IniWrite(_ClientRoster_DefaultView, CONFIG_INI, "ClientRoster", "DefaultView")
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

; The hotkey and the main tray entry open whichever view is configured.
_ClientRoster_Toggle() {
    global _ClientRoster_DefaultView
    if IsNativeInterface() || (_ClientRoster_DefaultView = "list") {
        _ClientRoster_ToggleList()
        return
    }
    _ClientRadial_Toggle()
}

_ClientRoster_ToggleList() {
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

    if IsNativeInterface() {
        g := Gui("+AlwaysOnTop +ToolWindow -MaximizeBox +E0x08000000", "Maps++ Clients")
        g.MarginX := 6
        g.MarginY := 6
        g.SetFont("s9", "Segoe UI")
        lv := g.Add("ListView", "w330 r6 -Multi NoSortHdr Grid", ["Character", "Zone", "Status"])
        lv.OnEvent("DoubleClick", _ClientRoster_OnNativeRowActivate)
        g.OnEvent("Close", (*) => _ClientRoster_UserHide())
        g.OnEvent("Escape", (*) => _ClientRoster_UserHide())
        _ClientRoster_Gui := g
        _ClientRoster_List := lv
        return
    }

    dllDir := (A_PtrSize = 8) ? "64bit" : "32bit"
    dllPath := A_ScriptDir "\Lib\" dllDir "\WebView2Loader.dll"
    wvSettings := { DllPath: dllPath, DefaultWidth: 380, DefaultHeight: 260 }

    g := WebViewGui("-Caption +AlwaysOnTop +ToolWindow -MaximizeBox +E0x08000000", "Maps++ Clients",, wvSettings)
    g.OnEvent("Close", (*) => _ClientRoster_UserHide())
    g.WebMessageReceived(_ClientRoster_OnWebMessage)
    g.DOMContentLoaded((*) => SetTimer(_ClientRoster_PushSnapshot, -50))
    g.Navigate("ui/client_roster/index.html")

    _ClientRoster_Gui := g
}

_ClientRoster_OnNativeRowActivate(lv, row) {
    global _ClientRoster_RowHwnds
    if (row < 1 || row > _ClientRoster_RowHwnds.Length)
        return
    hwnd := _ClientRoster_RowHwnds[row]
    if WinExist("ahk_id " hwnd)
        WinActivate("ahk_id " hwnd)
}

_ClientRoster_PushSnapshot() {
    _ClientRoster_OnSnapshot(GetClientSnapshots())
}

_ClientRoster_OnWebMessage(wv, args) {
    global _ClientRoster_Gui
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
            _ClientRoster_PushSnapshot()
        case "activate-client":
            if msg.Has("hwnd") && WinExist("ahk_id " msg["hwnd"]) {
                WinActivate("ahk_id " msg["hwnd"])
            }
        case "close":
            _ClientRoster_UserHide()
    }
}

; ── Radial ───────────────────────────────────────────────────────
; A ring of class avatars centred on the cursor: click one to focus that
; client. The ring itself — the transparent window, the hit-testing, the
; park/reveal — is the shared engine in radial.ahk; everything below is just
; this addon's half of it.

_ClientRadial_Toggle() {
    global CLIENT_RADIAL_RING
    RadialToggle(CLIENT_RADIAL_RING)
}

; Warms the ring up shortly after launch so even the first press is quick.
_ClientRadial_Prewarm() {
    global gDisabledAddons, CLIENT_RADIAL_RING
    if (gDisabledAddons.Has("ClientRoster") && gDisabledAddons["ClientRoster"]) {
        return
    }
    RadialPrewarm(CLIENT_RADIAL_RING)
}

; One spoke per running client. The hwnd is the item key, which is what comes
; back on a click.
_ClientRadial_Items() {
    items := []
    for snap in GetClientSnapshots() {
        cName := (snap.charName != "" ? snap.charName : "PID " snap.pid)
        items.Push(Map(
            "key",      String(snap.hwnd),
            "kind",     "avatar",
            "label",    cName,
            ; classId is -1 when the class could not be read or the client is
            ; still sitting at the login screen — the page falls back to
            ; initials when there is no portrait to show.
            "image",    (snap.classId >= 0) ? ("../../avatars/c" snap.classId ".png") : "",
            "initials", _ClientRadial_Initials(cName),
            "active",   snap.isActive ? true : false,
            "tooltip",  cName
        ))
    }
    return items
}

_ClientRadial_Initials(name) {
    parts := []
    for part in StrSplit(Trim(name), " ", " ") {
        if (part != "")
            parts.Push(part)
    }
    if (parts.Length = 0) {
        return "?"
    }
    if (parts.Length = 1) {
        return SubStr(parts[1], 1, 2)
    }
    return SubStr(parts[1], 1, 1) SubStr(parts[2], 1, 1)
}

_ClientRadial_Hub() {
    n := GetClientSnapshots().Length
    return Map(
        ; No clients running — the hub says so instead of showing a bare "0".
        "title",     (n = 0) ? "No clients" : String(n),
        "sub",       (n = 0) ? "running" : (n = 1 ? "client" : "clients"),
        "hoverIcon", "format_list_bulleted",
        "hoverSub",  "List view"
    )
}

_ClientRadial_OnSelect(item, index) {
    if WinExist("ahk_id " item["key"]) {
        WinActivate("ahk_id " item["key"])
    }
}

_ClientRadial_OnHub() {
    ; Show rather than toggle — the hub is a "take me to the list" button, so
    ; it must not close a list that is already up.
    _ClientRoster_Show(true)
    _ClientRoster_OnSnapshot(GetClientSnapshots())
}

; ── Rows ─────────────────────────────────────────────────────────

_ClientRoster_OnSnapshot(snapshots) {
    global _ClientRoster_Visible, _ClientRoster_AutoShow, _ClientRoster_Gui
    global _ClientRoster_List, _ClientRoster_RowHwnds
    global _ClientRoster_SuppressAuto, _ClientRoster_ManualOpen, CLIENT_RADIAL_RING

    ; Keep an open ring in step with the poll — a client that quits should not
    ; leave a spoke pointing at a dead window. No-ops while the ring is parked.
    RadialRefresh(CLIENT_RADIAL_RING)

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
    ; Single-boxers never see this window, and it never elbows in over a ring
    ; the user is already looking at.
    if (_ClientRoster_AutoShow && !_ClientRoster_Visible && !_ClientRoster_SuppressAuto
        && !RadialIsOpen() && snapshots.Length >= 2) {
        _ClientRoster_Show()
    }
    if (!_ClientRoster_Visible || !IsObject(_ClientRoster_Gui)) {
        return
    }

    if IsNativeInterface() {
        if !IsObject(_ClientRoster_List)
            return
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
        loop 3
            _ClientRoster_List.ModifyCol(A_Index, "AutoHdr")
        _ClientRoster_List.Opt("+Redraw")
        return
    }

    clientsJson := "["
    first := true
    for snap in snapshots {
        if !first
            clientsJson .= ","
        first := false
        cName := (snap.charName != "" ? snap.charName : "PID " snap.pid)
        clientsJson .= '{"hwnd":' snap.hwnd
            . ',"pid":' snap.pid
            . ',"charName":' _JSON_Str(cName)
            . ',"zoneText":' _JSON_Str(_ClientRoster_ZoneText(snap))
            . ',"statusText":' _JSON_Str(_ClientRoster_StatusText(snap))
        . '}'
    }
    clientsJson .= "]"

    try _ClientRoster_Gui.PostWebMessageAsJson('{"type":"snapshot-update","clients":' clientsJson '}')
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
