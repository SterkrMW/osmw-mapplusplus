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

; The radial ring. Destroyed and rebuilt on every open rather than hidden —
; it is rebuilt from a fresh poll each time and has no position to remember.
global _ClientRadial_Gui := 0
global _ClientRadial_Size := 420         ; square; the ring is centred inside it
; Colour-key backdrop. Near-black so any fringe on an anti-aliased edge reads
; as part of the drop shadow rather than as a halo.
global _ClientRadial_KeyColor := "010203"
global _ClientRadial_KeyColorRef := 0x030201   ; same colour as a COLORREF (0x00BBGGRR)
global _ClientRadial_KeyBrush := 0             ; created once, kept for the process
global _ClientRadial_HostHwnd := 0             ; the Static the WebView2 is parented to
; Hit map posted by the page: viewport size plus each spoke's circle, all in
; CSS pixels. Mouse input never reaches the WebView (see the notes below), so
; AHK tests the cursor against this itself.
global _ClientRadial_Hit := 0
global _ClientRadial_HoverIndex := 0           ; 0 none, -1 hub, else 1-based spoke
global _ClientRadial_HoverInterval := 40
; The window is built once and parked off-screen between uses; an open moves it
; under the cursor only once the page has re-rendered, so it never flashes an
; unstyled or stale ring over the game.
global _ClientRadial_Pos := 0
global _ClientRadial_Shown := false     ; on screen right now
global _ClientRadial_Pending := false   ; an open is waiting for the page

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

; The core never fires the OnInit hook, so config has to be loaded at include
; time (variables.ahk, and therefore CONFIG_INI, is already loaded by now).
; OnInit stays registered for whenever the core starts firing it — the loads
; are idempotent.
_ClientRoster_OnInit()

_ClientRoster_OnInit() {
    _ClientRoster_LoadConfig()
    OnExit(_ClientRoster_OnExit)
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
; A ring of class avatars centred on the cursor. The window is a plain square;
; everything outside the ring is transparent, which the colour key also makes
; click-through — so a click in the gaps lands on the game and the resulting
; deactivation closes the ring, exactly the behaviour you want from it.
;
; Getting that transparency takes three things together, and it is silently
; opaque if any one is missing:
;   1. The WebView2 controller's DefaultBackgroundColor set to alpha 0, so the
;      page renders with transparency instead of over an opaque white.
;   2. The *host Static control* painted in the key colour. WebViewToo parents
;      the WebView2 to a Gui.Custom "Static", and that control's background —
;      not the Gui's BackColor — is what shows through the transparent page.
;      Only WM_CTLCOLORSTATIC can change it.
;   3. WinSetTransColor on the same key colour. This keys out GDI pixels in the
;      window's redirection surface, which is exactly what (2) produces. It
;      cannot key out pixels the WebView2 itself draws, so colouring the page
;      background instead of the Static does not work.
;
; The catch: a colour-keyed window is click-through *everywhere*. Hit-testing
; reads the same redirection surface, which is entirely key coloured, so the OS
; decides every pixel is transparent and no mouse message ever reaches the
; WebView — :hover and click handlers in the page would simply never fire.
; So the page measures its own layout and posts a hit map, and everything below
; does the pointing: highlight follows a timer, clicks come from a hotkey.

; Paints the WebView2's host Static in the key colour. Guarded on our own
; control so every other Gui in the script keeps its normal colours.
OnMessage(0x0138, _ClientRadial_OnCtlColorStatic)   ; WM_CTLCOLORSTATIC

_ClientRadial_OnCtlColorStatic(wParam, lParam, msg, hwnd) {
    global _ClientRadial_HostHwnd, _ClientRadial_KeyBrush, _ClientRadial_KeyColorRef
    if (!_ClientRadial_HostHwnd || lParam != _ClientRadial_HostHwnd || !_ClientRadial_KeyBrush) {
        return
    }
    DllCall("SetBkColor", "Ptr", wParam, "UInt", _ClientRadial_KeyColorRef)
    return _ClientRadial_KeyBrush
}

_ClientRadial_Toggle() {
    global _ClientRadial_Shown
    if _ClientRadial_Shown {
        _ClientRadial_Close()
        return
    }
    _ClientRadial_Open()
}

; Builds the window once and keeps it for the rest of the session, parked
; off-screen between uses. Standing up a WebView2 and loading the page is by
; far the most expensive part of showing the ring — everything else (a memory
; poll, a re-render, a WinMove) is single-digit milliseconds — so doing it per
; open is what made the hotkey feel sluggish.
;
; It stays *shown* off-screen rather than hidden: WebView2 suspends rendering
; while its host window is hidden, and WinSetTransColor does not stick on a
; window that has never been shown.
_ClientRadial_EnsureGui() {
    global _ClientRadial_Gui, _ClientRadial_Size, _ClientRadial_KeyColor
    global _ClientRadial_KeyColorRef, _ClientRadial_KeyBrush, _ClientRadial_HostHwnd

    if IsObject(_ClientRadial_Gui) && _ClientRadial_Gui.Hwnd {
        return
    }

    if !_ClientRadial_KeyBrush {
        _ClientRadial_KeyBrush := DllCall("CreateSolidBrush", "UInt", _ClientRadial_KeyColorRef, "Ptr")
    }

    size := _ClientRadial_Size
    dllDir := (A_PtrSize = 8) ? "64bit" : "32bit"
    dllPath := A_ScriptDir "\Lib\" dllDir "\WebView2Loader.dll"
    wvSettings := { DllPath: dllPath, DefaultWidth: size, DefaultHeight: size }

    g := WebViewGui("-Caption -Border +AlwaysOnTop +ToolWindow -MaximizeBox", "Maps++ Radial", , wvSettings)
    ; Alpha 0 = transparent. WebViewToo's own DefaultBackgroundColor wrapper
    ; can't express this — its ConvertColor() hardcodes alpha 0xFF — so this
    ; goes straight to the controller property.
    try g.Control.wvc.DefaultBackgroundColor := 0
    ; Step 2 of the three above: hand the host Static to the WM_CTLCOLORSTATIC
    ; handler. BackColor is set as well so any sliver of the Gui itself that
    ; ever shows is the key colour too.
    g.BackColor := _ClientRadial_KeyColor
    try _ClientRadial_HostHwnd := g.Control.Hwnd
    ; No rounded corners or border tint — on an otherwise invisible window they
    ; draw a faint rectangle over the game.
    try DllCall("Dwmapi.dll\DwmSetWindowAttribute", "Ptr", g.Hwnd,
        "UInt", 33, "Ptr*", 1, "UInt", 4)             ; CORNER_PREFERENCE = DONOTROUND
    try DllCall("Dwmapi.dll\DwmSetWindowAttribute", "Ptr", g.Hwnd,
        "UInt", 34, "Ptr*", 0xFFFFFFFE, "UInt", 4)    ; BORDER_COLOR = NONE

    g.OnEvent("Close", (*) => _ClientRadial_Close())
    g.WebMessageReceived(_ClientRadial_OnWebMessage)
    g.DOMContentLoaded((*) => SetTimer(_ClientRadial_PushSnapshot, -50))
    g.Navigate("ui/client_radial/index.html")

    _ClientRadial_Gui := g

    g.Show("x-30000 y-30000 w" size " h" size " NoActivate")
    try WinSetTransColor(_ClientRadial_KeyColor, "ahk_id " g.Hwnd)
    ; Repaint children so the host Static picks up the key brush.
    DllCall("RedrawWindow", "Ptr", g.Hwnd, "Ptr", 0, "Ptr", 0, "UInt", 0x0085)
}

; Warms the window up shortly after launch so even the first press is quick.
_ClientRadial_Prewarm() {
    global gDisabledAddons
    if IsNativeInterface() {
        return
    }
    if (gDisabledAddons.Has("ClientRoster") && gDisabledAddons["ClientRoster"]) {
        return
    }
    try _ClientRadial_EnsureGui()
}

_ClientRadial_Open() {
    global _ClientRadial_Gui, _ClientRadial_Size, _ClientRadial_Pos, _ClientRadial_Pending

    UpdateClientSnapshots()
    _ClientRadial_EnsureGui()
    if !IsObject(_ClientRadial_Gui) {
        return
    }

    size := _ClientRadial_Size
    CoordMode("Mouse", "Screen")
    MouseGetPos(&mX, &mY)
    ; Clamp against the display the cursor is on, not the primary.
    MonitorGetWorkArea(GetMonitorIndexAtPoint(mX, mY), &sLeft, &sTop, &sRight, &sBottom)
    wX := mX - size // 2
    wY := mY - size // 2
    ; Near an edge the ring shifts inward rather than spilling off-screen, so
    ; the cursor ends up off-centre instead of the spokes being unreachable.
    wX := Max(sLeft, Min(wX, sRight - size))
    wY := Max(sTop, Min(wY, sBottom - size))

    _ClientRadial_Pos := { x: wX, y: wY, w: size, h: size }
    _ClientRadial_Pending := true

    ; Re-render with the clients that are running now. The page answers with a
    ; fresh hit map, and that is what slides the ring in — so it never appears
    ; showing the previous session's spokes.
    _ClientRadial_PushSnapshot()
    ; Backstop for a page that fails to load and never reports.
    SetTimer(_ClientRadial_Reveal, -600)
}

; Slides the ring in from off-screen and starts taking input. Safe to call more
; than once — whichever of the hit map or the backstop timer gets here first
; wins.
_ClientRadial_Reveal() {
    global _ClientRadial_Gui, _ClientRadial_Shown, _ClientRadial_Pos
    global _ClientRadial_Pending, _ClientRadial_HoverInterval
    if !_ClientRadial_Pending || !IsObject(_ClientRadial_Gui) || !IsObject(_ClientRadial_Pos) {
        return
    }
    _ClientRadial_Pending := false
    _ClientRadial_Shown := true
    SetTimer(_ClientRadial_Reveal, 0)

    p := _ClientRadial_Pos
    hwnd := _ClientRadial_Gui.Hwnd
    WinMove(p.x, p.y, p.w, p.h, "ahk_id " hwnd)
    ; It is kept NoActivate while parked; activate now, since losing activation
    ; is what dismisses the ring.
    try WinActivate("ahk_id " hwnd)

    SetTimer(_ClientRadial_HoverTick, _ClientRadial_HoverInterval)
    _ClientRadial_BindMouse(true)
}

; Dismisses the ring by parking it off-screen again. The window and its loaded
; page survive, which is what makes the next open instant.
_ClientRadial_Close() {
    global _ClientRadial_Gui, _ClientRadial_Hit, _ClientRadial_HoverIndex
    global _ClientRadial_Shown, _ClientRadial_Pos, _ClientRadial_Pending
    SetTimer(_ClientRadial_HoverTick, 0)
    SetTimer(_ClientRadial_Reveal, 0)
    _ClientRadial_BindMouse(false)
    if IsObject(_ClientRadial_Gui) && _ClientRadial_Gui.Hwnd {
        try WinMove(-30000, -30000, , , "ahk_id " _ClientRadial_Gui.Hwnd)
    }
    ; Clearing the hit map also disarms the LButton hotkey.
    _ClientRadial_Hit := 0
    _ClientRadial_HoverIndex := 0
    _ClientRadial_Shown := false
    _ClientRadial_Pending := false
    _ClientRadial_Pos := 0
}

; Dismiss when the ring loses focus — clicking the game, alt-tabbing, or
; anything else that takes activation away. Mirrors the web tray menu.
OnMessage(0x0006, _ClientRadial_OnWmActivate)

_ClientRadial_OnWmActivate(wParam, lParam, msg, hwnd) {
    global _ClientRadial_Gui, _ClientRadial_Shown
    ; wParam = 0 (WA_INACTIVE). The window outlives any single open now, so this
    ; must only fire for a ring that is actually on screen.
    if (wParam = 0 && _ClientRadial_Shown && IsObject(_ClientRadial_Gui)
        && _ClientRadial_Gui.Hwnd && hwnd = _ClientRadial_Gui.Hwnd) {
        SetTimer(_ClientRadial_Close, -10)
    }
}

_ClientRadial_OnWebMessage(wv, args) {
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
            _ClientRadial_PushSnapshot()
        case "hit-map":
            _ClientRadial_SetHitMap(msg)
        case "dismiss":
            _ClientRadial_Close()
    }
}

; Stores the page's self-measured layout, in the page's own CSS pixels.
_ClientRadial_SetHitMap(msg) {
    global _ClientRadial_Hit, _ClientRadial_HoverIndex
    if !msg.Has("vw") || !msg.Has("spokes") {
        return
    }
    spokes := []
    for _, s in msg["spokes"] {
        spokes.Push({
            hwnd: s["hwnd"],
            cx: s["cx"],
            cy: s["cy"],
            r: s["r"]
        })
    }
    _ClientRadial_Hit := {
        vw: msg["vw"],
        vh: msg["vh"],
        hubR: msg["hubR"],
        spokes: spokes
    }
    ; The ring just re-rendered, so whatever was highlighted no longer is.
    _ClientRadial_HoverIndex := 0
    ; The page has laid out, so there is something worth showing now.
    _ClientRadial_Reveal()
}

; ── Pointing ─────────────────────────────────────────────────────

; Which target is under the cursor: 0 none, -1 the hub, else the 1-based spoke.
; The hit map arrives in CSS pixels, so it is scaled against the client width —
; that keeps this correct at any display scaling without assuming a DPI.
_ClientRadial_HitTest() {
    global _ClientRadial_Gui, _ClientRadial_Hit
    if !IsObject(_ClientRadial_Gui) || !IsObject(_ClientRadial_Hit) {
        return 0
    }
    try {
        WinGetClientPos(&cx, &cy, &cw, &ch, "ahk_id " _ClientRadial_Gui.Hwnd)
    } catch {
        return 0
    }
    hit := _ClientRadial_Hit
    if (hit.vw <= 0 || cw <= 0) {
        return 0
    }
    scale := cw / hit.vw

    CoordMode("Mouse", "Screen")
    MouseGetPos(&mx, &my)
    px := (mx - cx) / scale          ; cursor in the page's own coordinates
    py := (my - cy) / scale

    for i, spoke in hit.spokes {
        dx := px - spoke.cx
        dy := py - spoke.cy
        if (dx * dx + dy * dy <= spoke.r * spoke.r) {
            return i
        }
    }
    dx := px - hit.vw / 2            ; the hub is exactly centred
    dy := py - hit.vh / 2
    if (dx * dx + dy * dy <= hit.hubR * hit.hubR) {
        return -1
    }
    return 0
}

; Highlight follows the cursor on a timer, since the page cannot see it.
_ClientRadial_HoverTick() {
    global _ClientRadial_Gui, _ClientRadial_HoverIndex
    if !IsObject(_ClientRadial_Gui) {
        return
    }
    idx := _ClientRadial_HitTest()
    if (idx = _ClientRadial_HoverIndex) {
        return
    }
    _ClientRadial_HoverIndex := idx
    try _ClientRadial_Gui.PostWebMessageAsJson('{"type":"hover","index":' idx '}')
}

; Clicks are caught by hotkeys registered only while the ring is up, rather
; than by a #HotIf context. #HotIf expressions are skipped when the script is
; busy, and the hover timer below keeps it busy — that dropped clicks at
; random. Registering and unregistering is deterministic.
;
; While the ring is open these swallow the button entirely, so the ring is
; modal in the way a context menu is: a click outside dismisses it and does
; not also reach the game.
_ClientRadial_BindMouse(on) {
    if (on) {
        Hotkey("LButton", _ClientRadial_OnLeftClick, "On")
        Hotkey("RButton", _ClientRadial_OnRightClick, "On")
        return
    }
    ; Turning off a hotkey that was never created throws — that is fine here,
    ; Close() runs on paths where the ring was never shown.
    try Hotkey("LButton", , "Off")
    try Hotkey("RButton", , "Off")
}

_ClientRadial_OnLeftClick(*) {
    global _ClientRadial_Hit
    idx := _ClientRadial_HitTest()
    if (idx = 0) {
        _ClientRadial_Close()
        return
    }
    if (idx = -1) {
        _ClientRadial_Close()
        ; Show rather than toggle — the hub is a "take me to the list" button,
        ; so it must not close a list that is already up.
        _ClientRoster_Show(true)
        _ClientRoster_OnSnapshot(GetClientSnapshots())
        return
    }
    hwnd := _ClientRadial_Hit.spokes[idx].hwnd
    _ClientRadial_Close()
    if WinExist("ahk_id " hwnd) {
        WinActivate("ahk_id " hwnd)
    }
}

_ClientRadial_OnRightClick(*) {
    _ClientRadial_Close()
}

_ClientRadial_PushSnapshot() {
    global _ClientRadial_Gui
    if !IsObject(_ClientRadial_Gui) {
        return
    }

    clientsJson := "["
    first := true
    for snap in GetClientSnapshots() {
        if !first
            clientsJson .= ","
        first := false
        cName := (snap.charName != "" ? snap.charName : "PID " snap.pid)
        clientsJson .= '{"hwnd":' snap.hwnd
            . ',"charName":' _JSON_Str(cName)
            . ',"classId":' snap.classId
            . ',"isActive":' (snap.isActive ? "true" : "false")
        . '}'
    }
    clientsJson .= "]"

    try _ClientRadial_Gui.PostWebMessageAsJson('{"type":"radial-update","clients":' clientsJson '}')
}

; ── Rows ─────────────────────────────────────────────────────────

_ClientRoster_OnSnapshot(snapshots) {
    global _ClientRoster_Visible, _ClientRoster_AutoShow, _ClientRoster_Gui
    global _ClientRoster_List, _ClientRoster_RowHwnds
    global _ClientRoster_SuppressAuto, _ClientRoster_ManualOpen, _ClientRadial_Shown

    ; Keep an open ring in step with the poll — a client that quits should not
    ; leave a spoke pointing at a dead window.
    _ClientRadial_PushSnapshot()

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
        && !_ClientRadial_Shown && snapshots.Length >= 2) {
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
