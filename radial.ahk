#Requires AutoHotkey v2.0

; ── Radial ring engine ───────────────────────────────────────────
; A ring of buttons centred on the cursor. The window is a plain square;
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
;
; ── Using it ─────────────────────────────────────────────────────
; An addon registers a ring in its OnInit and drives it by name:
;
;   RadialRegister(Map(
;       "name",     "clients",                  ; unique key
;       "page",     "ui/radial/index.html",
;       "title",    "Maps++ Radial — Clients",  ; window title; unique per ring
;       "size",     420,                        ; optional, defaults to RADIAL_SIZE
;       "onBeforeOpen", fn,                     ; optional; refresh your data here
;       "getItems", fn,                         ; fn() → Array of item Maps
;       "getHub",   fn,                         ; optional fn() → hub Map
;       "onSelect", fn,                         ; fn(item, index), AFTER the ring closes
;       "onHub",    fn,                         ; optional fn(), AFTER the ring closes
;       "restoreFocus", true))                  ; optional, default true
;
; Item descriptor (one per spoke):
;   Map("key","…",             ; opaque id, echoed back through the hit map
;       "kind","icon",         ; "icon" | "avatar"
;       "label","View mode",
;       "icon","visibility",   ; Material Symbols glyph name   (kind = "icon")
;       "image","../../avatars/c3.png",  ; portrait url        (kind = "avatar")
;       "initials","Ka",       ; shown when the image fails    (kind = "avatar")
;       "state","",            ; "" | "on" | "off" — badge; "" = no badge
;       "active",false,        ; standing accent glow
;       "tooltip","…")
;
; Hub descriptor:
;   Map("title","3", "sub","clients",
;       "hoverIcon","format_list_bulleted", "hoverSub","List view",
;       "enabled",true)
;
; Only one ring is ever on screen: the mouse grab below is process-global, so
; two visible rings would fight over it. Opening one closes any other.

global RADIAL_SIZE := 420
global RADIAL_HOVER_INTERVAL := 40
; Colour-key backdrop. Near-black so any fringe on an anti-aliased edge reads
; as part of the drop shadow rather than as a halo.
global RADIAL_KEY_COLOR := "010203"
global RADIAL_KEY_COLOR_REF := 0x030201   ; same colour as a COLORREF (0x00BBGGRR)
global gRadialKeyBrush := 0               ; created once, kept for the process

global gRadialRings := Map()              ; name → ring state
global gRadialHosts := Map()              ; host Static hwnd → ring name
global gRadialActive := ""                ; ring on screen right now ("" = none)
global gRadialPending := ""               ; ring waiting for its page to draw
global gRadialPrevActive := 0             ; window focused when the ring opened

; ── Registration ─────────────────────────────────────────────────

RadialRegister(spec) {
    global gRadialRings
    if !spec.Has("name") || spec["name"] = "" {
        TrayTip("RadialRegister() missing name — ignored.", "Radial", "Iconx")
        return ""
    }
    name := spec["name"]
    gRadialRings[name] := {
        name: name,
        spec: spec,
        gui: 0,
        hostHwnd: 0,
        hit: 0,             ; page's self-measured layout, in its own CSS pixels
        items: [],          ; what the page was last told to draw
        payload: "",        ; last rendered JSON; unchanged refreshes are no-ops
        hoverIndex: 0,      ; 0 none, -1 hub, else 1-based spoke
        pos: 0              ; where the next reveal puts the window
    }
    return name
}

_Radial_Ring(name) {
    global gRadialRings
    return gRadialRings.Has(name) ? gRadialRings[name] : 0
}

; ── Window ───────────────────────────────────────────────────────

; Paints a ring's WebView2 host Static in the key colour. Guarded on the hosts
; we own so every other Gui in the script keeps its normal colours.
OnMessage(0x0138, _Radial_OnCtlColorStatic)   ; WM_CTLCOLORSTATIC

_Radial_OnCtlColorStatic(wParam, lParam, msg, hwnd) {
    global gRadialHosts, gRadialKeyBrush, RADIAL_KEY_COLOR_REF
    if (!gRadialKeyBrush || !gRadialHosts.Has(lParam)) {
        return
    }
    DllCall("SetBkColor", "Ptr", wParam, "UInt", RADIAL_KEY_COLOR_REF)
    return gRadialKeyBrush
}

; Builds a ring's window once and keeps it for the rest of the session, parked
; off-screen between uses. Standing up a WebView2 and loading the page is by
; far the most expensive part of showing a ring — everything else (a poll, a
; re-render, a WinMove) is single-digit milliseconds — so doing it per open is
; what made the hotkey feel sluggish.
;
; It stays *shown* off-screen rather than hidden: WebView2 suspends rendering
; while its host window is hidden, and WinSetTransColor does not stick on a
; window that has never been shown.
_Radial_EnsureGui(ring) {
    global gRadialKeyBrush, gRadialHosts
    global RADIAL_SIZE, RADIAL_KEY_COLOR, RADIAL_KEY_COLOR_REF

    if IsObject(ring.gui) && ring.gui.Hwnd {
        return
    }

    if !gRadialKeyBrush {
        gRadialKeyBrush := DllCall("CreateSolidBrush", "UInt", RADIAL_KEY_COLOR_REF, "Ptr")
    }

    spec := ring.spec
    size := spec.Has("size") ? spec["size"] : RADIAL_SIZE
    title := spec.Has("title") ? spec["title"] : ("Maps++ Radial — " ring.name)
    page := spec.Has("page") ? spec["page"] : "ui/radial/index.html"

    dllDir := (A_PtrSize = 8) ? "64bit" : "32bit"
    dllPath := A_ScriptDir "\Lib\" dllDir "\WebView2Loader.dll"
    wvSettings := { DllPath: dllPath, DefaultWidth: size, DefaultHeight: size }

    g := WebViewGui("-Caption -Border +AlwaysOnTop +ToolWindow -MaximizeBox", title, , wvSettings)
    ; Alpha 0 = transparent. WebViewToo's own DefaultBackgroundColor wrapper
    ; can't express this — its ConvertColor() hardcodes alpha 0xFF — so this
    ; goes straight to the controller property.
    try g.Control.wvc.DefaultBackgroundColor := 0
    ; Step 2 of the three above: hand the host Static to the WM_CTLCOLORSTATIC
    ; handler. BackColor is set as well so any sliver of the Gui itself that
    ; ever shows is the key colour too.
    g.BackColor := RADIAL_KEY_COLOR
    try {
        ring.hostHwnd := g.Control.Hwnd
        gRadialHosts[ring.hostHwnd] := ring.name
    }
    ; No rounded corners or border tint — on an otherwise invisible window they
    ; draw a faint rectangle over the game.
    try DllCall("Dwmapi.dll\DwmSetWindowAttribute", "Ptr", g.Hwnd,
        "UInt", 33, "Ptr*", 1, "UInt", 4)             ; CORNER_PREFERENCE = DONOTROUND
    try DllCall("Dwmapi.dll\DwmSetWindowAttribute", "Ptr", g.Hwnd,
        "UInt", 34, "Ptr*", 0xFFFFFFFE, "UInt", 4)    ; BORDER_COLOR = NONE

    g.OnEvent("Close", (*) => RadialClose())
    g.WebMessageReceived(_Radial_MakeMessageHandler(ring.name))
    g.DOMContentLoaded(_Radial_MakeLoadHandler(ring.name))
    g.Navigate(UiPageUrl(page))

    ring.gui := g

    g.Show("x-30000 y-30000 w" size " h" size " NoActivate")
    try WinSetTransColor(RADIAL_KEY_COLOR, "ahk_id " g.Hwnd)
    ; Repaint children so the host Static picks up the key brush.
    DllCall("RedrawWindow", "Ptr", g.Hwnd, "Ptr", 0, "Ptr", 0, "UInt", 0x0085)
}

; Each ring has its own page, and they all post into the same script — so the
; handlers are closures that remember which ring replied.
_Radial_MakeMessageHandler(name) {
    return (wv, args) => _Radial_OnWebMessage(name, wv, args)
}

; The page is ready but may still be settling; give it a beat before the first
; push, exactly as the roster's own load handler did.
_Radial_MakeLoadHandler(name) {
    return (*) => SetTimer(_Radial_PushItems.Bind(_Radial_Ring(name), true), -50)
}

; Warms a ring up in the background so even the first press is quick. The
; caller is responsible for its own "is my addon enabled" check.
RadialPrewarm(name) {
    if IsNativeInterface() {
        return
    }
    ring := _Radial_Ring(name)
    if !IsObject(ring) {
        return
    }
    try _Radial_EnsureGui(ring)
}

; ── Open / close ─────────────────────────────────────────────────

RadialIsOpen(name := "") {
    global gRadialActive
    if (name = "") {
        return gRadialActive != ""
    }
    return gRadialActive = name
}

RadialToggle(name) {
    if RadialIsOpen(name) {
        RadialClose()
        return
    }
    RadialOpen(name)
}

RadialOpen(name) {
    global gRadialActive, gRadialPending, gRadialPrevActive, RADIAL_SIZE

    ; Two visible rings would fight over the mouse grab below.
    if (gRadialActive != "" && gRadialActive != name) {
        RadialClose()
    }

    ring := _Radial_Ring(name)
    if !IsObject(ring) {
        return
    }
    spec := ring.spec
    if spec.Has("onBeforeOpen") {
        spec["onBeforeOpen"].Call()
    }

    _Radial_EnsureGui(ring)
    if !IsObject(ring.gui) {
        return
    }

    ; Remembered before the ring takes activation, so closing can hand focus
    ; back — several actions a ring can fire only work against a focused game.
    gRadialPrevActive := WinActive("A")

    size := spec.Has("size") ? spec["size"] : RADIAL_SIZE
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

    ring.pos := { x: wX, y: wY, w: size, h: size }
    gRadialPending := name

    ; Re-render with the items that apply now. The page answers with a fresh
    ; hit map, and that is what slides the ring in — so it never appears
    ; showing the previous open's spokes.
    _Radial_PushItems(ring, true)
    ; Backstop for a page that fails to load and never reports.
    SetTimer(_Radial_RevealPending, -600)
}

; Slides the pending ring in from off-screen and starts taking input. Safe to
; call more than once — whichever of the hit map or the backstop timer gets
; here first wins.
_Radial_RevealPending() {
    global gRadialPending, gRadialActive, RADIAL_HOVER_INTERVAL
    if (gRadialPending = "") {
        return
    }
    ring := _Radial_Ring(gRadialPending)
    if !IsObject(ring) || !IsObject(ring.gui) || !IsObject(ring.pos) {
        return
    }
    gRadialActive := ring.name
    gRadialPending := ""
    SetTimer(_Radial_RevealPending, 0)

    p := ring.pos
    hwnd := ring.gui.Hwnd
    WinMove(p.x, p.y, p.w, p.h, "ahk_id " hwnd)
    ; It is kept NoActivate while parked; activate now, since losing activation
    ; is what dismisses the ring.
    try WinActivate("ahk_id " hwnd)

    SetTimer(_Radial_HoverTick, RADIAL_HOVER_INTERVAL)
    _Radial_BindMouse(true)
}

; Dismisses the open ring by parking it off-screen again. The window and its
; loaded page survive, which is what makes the next open instant.
RadialClose() {
    global gRadialActive, gRadialPending, gRadialPrevActive
    SetTimer(_Radial_HoverTick, 0)
    SetTimer(_Radial_RevealPending, 0)
    _Radial_BindMouse(false)

    name := gRadialActive
    gRadialActive := ""
    gRadialPending := ""

    ring := _Radial_Ring(name)
    if !IsObject(ring) {
        gRadialPrevActive := 0
        return
    }

    hwnd := IsObject(ring.gui) ? ring.gui.Hwnd : 0
    ; Only worth handing focus back if the ring still has it. When the ring was
    ; dismissed *by* losing activation — a click on the game, an alt-tab — the
    ; new window is already where the user wants to be.
    wasActive := (hwnd && WinActive("A") = hwnd)
    if hwnd {
        try WinMove(-30000, -30000, , , "ahk_id " hwnd)
    }
    ; Clearing the hit map also disarms the click handler.
    ring.hit := 0
    ring.hoverIndex := 0
    ring.pos := 0

    restore := !ring.spec.Has("restoreFocus") || ring.spec["restoreFocus"]
    if (restore && wasActive && gRadialPrevActive && gRadialPrevActive != hwnd
        && WinExist("ahk_id " gRadialPrevActive)) {
        try WinActivate("ahk_id " gRadialPrevActive)
    }
    gRadialPrevActive := 0
}

; Dismiss when the ring loses focus — clicking the game, alt-tabbing, or
; anything else that takes activation away. Mirrors the web tray menu.
OnMessage(0x0006, _Radial_OnWmActivate)

_Radial_OnWmActivate(wParam, lParam, msg, hwnd) {
    global gRadialActive
    ; wParam = 0 (WA_INACTIVE). Ring windows outlive any single open, so this
    ; must only fire for the ring that is actually on screen.
    if (wParam != 0 || gRadialActive = "") {
        return
    }
    ring := _Radial_Ring(gRadialActive)
    if (IsObject(ring) && IsObject(ring.gui) && ring.gui.Hwnd && hwnd = ring.gui.Hwnd) {
        SetTimer(RadialClose, -10)
    }
}

; ── Messages ─────────────────────────────────────────────────────

_Radial_OnWebMessage(name, wv, args) {
    ring := _Radial_Ring(name)
    if !IsObject(ring) {
        return
    }
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
            _Radial_PushItems(ring, true)
        case "hit-map":
            _Radial_SetHitMap(ring, msg)
        case "dismiss":
            ; Only the ring on screen may dismiss; a parked page must not close
            ; the one the user is looking at.
            if (RadialIsOpen(ring.name))
                RadialClose()
    }
}

; Stores the page's self-measured layout, in the page's own CSS pixels.
;
; This lives on the ring rather than in a global on purpose: every ring's page
; posts into the same handler, so a background push to a parked ring would
; otherwise clobber the live ring's hit map and land clicks on the wrong spoke.
_Radial_SetHitMap(ring, msg) {
    global gRadialPending
    if !msg.Has("vw") || !msg.Has("spokes") {
        return
    }
    spokes := []
    for _, s in msg["spokes"] {
        spokes.Push({
            key: s.Has("key") ? s["key"] : "",
            cx: s["cx"],
            cy: s["cy"],
            r: s["r"]
        })
    }
    ring.hit := {
        vw: msg["vw"],
        vh: msg["vh"],
        hubR: msg.Has("hubR") ? msg["hubR"] : 0,
        spokes: spokes
    }
    ; The ring just re-rendered, so whatever was highlighted no longer is.
    ring.hoverIndex := 0
    ; The page has laid out, so there is something worth showing now.
    if (gRadialPending = ring.name) {
        _Radial_RevealPending()
    }
}

; Re-pushes a ring's items. No-ops unless that ring is the one on screen —
; a parked ring is re-rendered by RadialOpen anyway, and pushing to it here
; would replace the visible ring's hit map with the parked one's.
RadialRefresh(name) {
    if !RadialIsOpen(name) {
        return
    }
    _Radial_PushItems(_Radial_Ring(name))
}

_Radial_PushItems(ring, force := false) {
    if !IsObject(ring) || !IsObject(ring.gui) {
        return
    }
    spec := ring.spec
    items := spec.Has("getItems") ? spec["getItems"].Call() : []
    if !IsObject(items) {
        items := []
    }
    ring.items := items

    itemsJson := "["
    for i, item in items {
        if (i > 1)
            itemsJson .= ","
        itemsJson .= _Radial_ItemJson(item)
    }
    itemsJson .= "]"

    hubJson := "null"
    if spec.Has("getHub") {
        hub := spec["getHub"].Call()
        if IsObject(hub)
            hubJson := _Radial_HubJson(hub)
    }

    payload := '{"type":"radial-items","ring":' _JSON_Str(ring.name)
        . ',"items":' itemsJson ',"hub":' hubJson '}'
    ; Snapshot consumers may ask for a refresh on every poll. Rebuilding an
    ; unchanged page resets hover and replays the spoke bloom, which looks like
    ; the whole menu is periodically flashing. Always push on page load/open
    ; (a fresh hit map is required), but make routine refreshes data-driven.
    if (!force && payload = ring.payload) {
        return
    }
    try {
        ring.gui.PostWebMessageAsJson(payload)
        ring.payload := payload
    }
}

_Radial_Field(m, key, default := "") {
    return (IsObject(m) && m.Has(key)) ? m[key] : default
}

_Radial_ItemJson(item) {
    return '{"key":' _JSON_Str(_Radial_Field(item, "key"))
        . ',"kind":' _JSON_Str(_Radial_Field(item, "kind", "icon"))
        . ',"label":' _JSON_Str(_Radial_Field(item, "label"))
        . ',"icon":' _JSON_Str(_Radial_Field(item, "icon"))
        . ',"image":' _JSON_Str(_Radial_Field(item, "image"))
        . ',"initials":' _JSON_Str(_Radial_Field(item, "initials"))
        . ',"state":' _JSON_Str(_Radial_Field(item, "state"))
        . ',"tooltip":' _JSON_Str(_Radial_Field(item, "tooltip"))
        . ',"active":' (_Radial_Field(item, "active", false) ? "true" : "false")
        . '}'
}

_Radial_HubJson(hub) {
    enabled := !hub.Has("enabled") || hub["enabled"]
    return '{"title":' _JSON_Str(_Radial_Field(hub, "title"))
        . ',"sub":' _JSON_Str(_Radial_Field(hub, "sub"))
        . ',"hoverIcon":' _JSON_Str(_Radial_Field(hub, "hoverIcon"))
        . ',"hoverSub":' _JSON_Str(_Radial_Field(hub, "hoverSub"))
        . ',"enabled":' (enabled ? "true" : "false")
        . '}'
}

; ── Pointing ─────────────────────────────────────────────────────

; Which target is under the cursor: 0 none, -1 the hub, else the 1-based spoke.
; The hit map arrives in CSS pixels, so it is scaled against the client width —
; that keeps this correct at any display scaling without assuming a DPI.
_Radial_HitTest() {
    global gRadialActive
    ring := _Radial_Ring(gRadialActive)
    if !IsObject(ring) || !IsObject(ring.gui) || !IsObject(ring.hit) {
        return 0
    }
    try {
        WinGetClientPos(&cx, &cy, &cw, &ch, "ahk_id " ring.gui.Hwnd)
    } catch {
        return 0
    }
    hit := ring.hit
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
    if (hit.hubR > 0) {
        dx := px - hit.vw / 2        ; the hub is exactly centred
        dy := py - hit.vh / 2
        if (dx * dx + dy * dy <= hit.hubR * hit.hubR) {
            return -1
        }
    }
    return 0
}

; Highlight follows the cursor on a timer, since the page cannot see it.
_Radial_HoverTick() {
    global gRadialActive
    ring := _Radial_Ring(gRadialActive)
    if !IsObject(ring) || !IsObject(ring.gui) {
        return
    }
    idx := _Radial_HitTest()
    if (idx = ring.hoverIndex) {
        return
    }
    ring.hoverIndex := idx
    try ring.gui.PostWebMessageAsJson('{"type":"hover","index":' idx '}')
}

; Clicks are caught by hotkeys registered only while a ring is up, rather than
; by a #HotIf context. #HotIf expressions are skipped when the script is busy,
; and the hover timer above keeps it busy — that dropped clicks at random.
; Registering and unregistering is deterministic.
;
; While a ring is open these swallow the button entirely, so the ring is modal
; in the way a context menu is: a click outside dismisses it and does not also
; reach the game.
;
; The grab is process-global, which is the other reason only one ring may be
; open at a time: a second ring's unbind would disarm the first ring's clicks.
_Radial_BindMouse(on) {
    if (on) {
        Hotkey("LButton", _Radial_OnLeftClick, "On")
        Hotkey("RButton", _Radial_OnRightClick, "On")
        return
    }
    ; Turning off a hotkey that was never created throws — that is fine here,
    ; RadialClose runs on paths where no ring was ever shown.
    try Hotkey("LButton", , "Off")
    try Hotkey("RButton", , "Off")
}

_Radial_OnLeftClick(*) {
    global gRadialActive
    ring := _Radial_Ring(gRadialActive)
    if !IsObject(ring) {
        RadialClose()
        return
    }
    spec := ring.spec
    idx := _Radial_HitTest()

    if (idx = 0) {
        RadialClose()
        return
    }
    if (idx = -1) {
        RadialClose()
        if spec.Has("onHub")
            spec["onHub"].Call()
        return
    }

    ; The item list and the hit map are pushed together, but a refresh landing
    ; between the two would let a click act on something the user never saw —
    ; so only act when the spoke under the cursor still names the item we have.
    item := (idx <= ring.items.Length) ? ring.items[idx] : 0
    if (!IsObject(item) || item["key"] != ring.hit.spokes[idx].key) {
        RadialClose()
        return
    }

    ; Closed first, so the handler runs with focus already handed back and the
    ; mouse grab released.
    RadialClose()
    if spec.Has("onSelect")
        spec["onSelect"].Call(item, idx)
}

_Radial_OnRightClick(*) {
    RadialClose()
}
