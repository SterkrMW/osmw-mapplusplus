#Requires AutoHotkey v2.0

; ── GUI gallery (debug / promotional) ────────────────────────────
; Opens every panel this build has, all at once, and lays them out in one
; evenly spaced grid so a single screenshot can show the whole app. Tray →
; Debug → GUI Gallery; running it again closes everything it opened.
;
; Dev-only by construction: it is listed in no variants\*.txt manifest, so
; build.ps1 never ships it, while GenerateAddonIncludes() picks it up out of
; addons\ on any source run.
;
; ── What it deliberately leaves out ──────────────────────────────
; The two overlays — the minimap and the party-health strip — are not in the
; grid. Both derive their position from the game client's rect on their own
; timer, and the minimap closes itself when focus leaves the game, so a window
; moved into a cell here would snap back or vanish within 200ms. They are
; already where a promotional shot wants them: over the client.
;
; ── Reaching panels it cannot name ───────────────────────────────
; Every addon-owned panel is opened through a *name string*, never a direct
; call. This file loads alongside whatever happens to be in addons\, and a call
; to a function that is not there is a load-time error, not a runtime one — so
; a build without ShopPrices would refuse to start rather than skip a cell. The
; same goes for reading each panel's Gui object out of its own global.
;
; ── The three parked surfaces ────────────────────────────────────
; The tray menu, the radial rings and the dialog have no non-blocking "just
; show it" entry point, by design: they exist to be dismissed by the next click
; or to block their caller until it is answered. The gallery renders them
; itself through their own ensure/post functions and puts them back at -30000
; afterwards rather than closing them, which is what dismissing them does
; anyway. A ring shown this way is inert — gRadialActive stays empty, so
; nothing grabs the mouse and two rings on screen cannot fight over it.

global GALLERY_GAP       := 24    ; px between windows, and the margin around the grid
global GALLERY_OPEN_MS   := 120   ; pause after each open, so pages start loading in turn
global GALLERY_SETTLE_MS := 1200  ; budget for the last pages to paint before measuring
global GALLERY_DIALOG_MS := 900   ; how long the sample dialog gets to measure itself
global GALLERY_PARK_X    := -30000

global _Gallery_Active := false
global _Gallery_Placed := []      ; one entry per shown surface, for teardown

RegisterAddon(Map(
    "name",       "DebugGallery",
    "OnTrayMenu", _Gallery_OnTrayMenu
))

RegisterHotkeyAction(Map(
    "id", "debugGuiGallery",
    "label", "Toggle the GUI gallery (debug)",
    "category", "Debug",
    ; Unbound by default: a promotional tool does not deserve a chord out of the
    ; box, and the tray entry is the discoverable way in.
    "default", "",
    "addon", "DebugGallery",
    "handler", _Gallery_Toggle
))

_Gallery_OnTrayMenu(trayGroups) {
    global _Gallery_Active
    if !trayGroups.Has("debug")
        return
    label := _Gallery_Active ? "Close GUI Gallery" : "GUI Gallery…"
    trayGroups["debug"].Add(label "`t" GetHotkeyDisplay("debugGuiGallery"),
        (*) => _Gallery_Toggle())
}

_Gallery_Toggle(*) {
    global _Gallery_Active
    if _Gallery_Active {
        _Gallery_CloseAll()
        return
    }
    ; Opening walks a dozen windows and sleeps between them. Doing that inside
    ; the tray-menu callback would hold the menu open on top of the result.
    SetTimer(_Gallery_Open, -150)
}

; ── Opening ──────────────────────────────────────────────────────

_Gallery_Open() {
    global _Gallery_Active, _Gallery_Placed, GALLERY_OPEN_MS, GALLERY_SETTLE_MS
    if _Gallery_Active
        return

    ; An open ring holds the process-global mouse grab and dismisses itself on
    ; the first activation change, which every panel below would cause.
    if RadialIsOpen()
        RadialClose()

    placed := []
    skipped := []
    for spec in _Gallery_Surfaces() {
        reason := _Gallery_Unavailable(spec)
        if (reason != "") {
            skipped.Push(spec["label"] " — " reason)
            continue
        }
        hwnd := 0
        try {
            hwnd := _Gallery_OpenSurface(spec)
        } catch as err {
            skipped.Push(spec["label"] " — " err.Message)
            LogWarn("Gallery", "Opening " spec["label"] " failed: " err.Message)
            continue
        }
        if !hwnd {
            ; A panel that refuses to open says why itself, in its own TrayTip:
            ; no client pinned, no map entered, no layouts saved yet.
            skipped.Push(spec["label"] " — declined to open (see its own notice)")
            continue
        }
        placed.Push({
            label:   spec["label"],
            hwnd:    hwnd,
            park:    spec.Has("park") && spec["park"],
            closeFn: spec.Has("closeFn") ? spec["closeFn"] : ""
        })
        Sleep(GALLERY_OPEN_MS)
    }

    if !placed.Length {
        LogWarn("Gallery", "Nothing could be opened — skipped: " _Gallery_Join(skipped))
        TrayTip("GUI gallery: nothing could be opened. See the log.", "Maps++", "Icon!")
        return
    }

    _Gallery_Placed := placed
    _Gallery_Active := true
    ; The pages opened last are still painting, and an AutoSize window has not
    ; settled on its final size until it has drawn once.
    Sleep(GALLERY_SETTLE_MS)
    _Gallery_Arrange(placed)

    shown := []
    for item in placed
        shown.Push(item.label)
    LogInfo("Gallery", "Shown (" placed.Length "): " _Gallery_Join(shown)
        . (skipped.Length ? "  |  Skipped (" skipped.Length "): " _Gallery_Join(skipped) : ""))
    TrayTip(placed.Length " panel(s) shown"
        . (skipped.Length ? ", " skipped.Length " skipped — see the log" : "")
        . ".`nRun it again to put them away.", "GUI Gallery", "Iconi")
}

_Gallery_OpenSurface(spec) {
    ; A parked surface renders itself and knows its own window.
    if spec.Has("openFn")
        return spec["openFn"].Call()

    fn := _Gallery_Lookup(spec["fn"])
    if !fn
        return 0
    if spec.Has("args")
        fn.Call(spec["args"]*)
    else
        fn.Call()
    return _Gallery_HwndFrom(spec["guiVars"])
}

_Gallery_Unavailable(spec) {
    if (spec.Has("webOnly") && spec["webOnly"] && !IsWebViewInterface())
        return "enhanced interface only"
    if (spec.Has("addon") && spec["addon"] != "" && !IsAddonEnabled(spec["addon"]))
        return "addon off or not in this build"
    if (spec.Has("fn") && !_Gallery_Lookup(spec["fn"]))
        return "not in this build"
    return ""
}

; Every surface the gallery knows how to put on screen. `fn`/`guiVars` name a
; panel's own entry point and the global it stores its Gui in; `openFn` is for
; the three that render themselves. A panel that refuses to open (no client
; pinned, no map entered) reports that itself with a TrayTip and simply leaves
; no window behind, which the caller records as a skip.
_Gallery_Surfaces() {
    return [
        Map("key", "tray", "label", "Tray menu",
            "webOnly", true, "openFn", _Gallery_OpenTrayMenu, "park", true),
        Map("key", "settings", "label", "Settings",
            "fn", "ShowSettingsWindow", "guiVars", ["gSettingsGui"]),
        Map("key", "calibration", "label", "Map calibration",
            "fn", "ShowCalibrationPanel", "guiVars", ["gCalibGui"]),
        Map("key", "roster", "label", "Client roster", "addon", "ClientRoster",
            "fn", "_ClientRoster_Show", "args", [true],
            "guiVars", ["_ClientRoster_Gui"], "closeFn", "_ClientRoster_Hide"),
        Map("key", "radialClients", "label", "Radial — clients", "addon", "ClientRoster",
            "webOnly", true, "openFn", _Gallery_OpenClientsRing, "park", true),
        Map("key", "radialActions", "label", "Radial — quick actions", "addon", "QuickActions",
            "webOnly", true, "openFn", _Gallery_OpenActionsRing, "park", true),
        Map("key", "pois", "label", "Map POIs", "addon", "MapPois",
            "fn", "_Pois_ShowManageWindow", "guiVars", ["_Pois_ManageGui"]),
        Map("key", "betterHotkeys", "label", "Better Hotkeys", "addon", "BetterHotkeys",
            "fn", "_BH_OpenEditor", "guiVars", ["_BH_WebGui", "_BH_NativeGui"]),
        Map("key", "windowLayout", "label", "Window layouts", "addon", "WindowLayout",
            "fn", "_WindowLayout_ShowManager",
            "guiVars", ["_WindowLayout_WebGui", "_WindowLayout_MgrGui"]),
        Map("key", "shopPrices", "label", "Character vendor", "addon", "ShopPrices",
            "fn", "_ShopPrices_Open",
            "guiVars", ["_ShopPrices_WebGui", "_ShopPrices_NativeGui"]),
        ; Last: it is the one surface that reuses a window another dialog may
        ; want, so it is also the first thing put away.
        Map("key", "dialog", "label", "Dialog", "webOnly", true,
            "openFn", _Gallery_OpenSampleDialog, "closeFn", _Gallery_CloseSampleDialog)
    ]
}

; ── The parked surfaces ──────────────────────────────────────────

; Deliberately not ShowWebTrayMenu(): that reveals at the cursor and starts the
; focus watchdog, which would dismiss the menu as soon as the gallery opened the
; next panel. gWebTrayShown stays false, so nothing dismisses this one.
_Gallery_OpenTrayMenu() {
    global gWebTrayGui, gWebTrayItems, gWebTrayCallbacks
    RebuildTrayMenu()
    gWebTrayCallbacks := Map()
    gWebTrayItems := _ConvertHmenuToWebItems(A_TrayMenu.Handle)
    _EnsureWebTrayGui()
    if !IsObject(gWebTrayGui) || !gWebTrayGui.Hwnd
        return 0
    _PushTrayMenuState()
    return gWebTrayGui.Hwnd
}

_Gallery_OpenClientsRing() {
    return _Gallery_ShowRingInert(_Gallery_Const("CLIENT_RADIAL_RING", "clients"))
}

_Gallery_OpenActionsRing() {
    return _Gallery_ShowRingInert(_Gallery_Const("QUICK_ACTIONS_RING", "actions"))
}

; Builds the ring if needed and re-renders it where it is parked. The caller
; moves it into its cell; RadialClose() is never involved, so the ring takes no
; input and nothing dismisses it.
_Gallery_ShowRingInert(name) {
    RadialPrewarm(name)
    ring := _Radial_Ring(name)
    if !IsObject(ring) || !IsObject(ring.gui) || !ring.gui.Hwnd
        return 0
    ; A ring prewarmed at startup is still showing whatever was true then.
    ; Pushing to a parked ring is safe: each ring owns its own hit map, and the
    ; open above guaranteed none is on screen.
    if ring.spec.Has("onBeforeOpen")
        ring.spec["onBeforeOpen"].Call()
    _Radial_PushItems(ring, true)
    return ring.gui.Hwnd
}

; A stand-in dialog, posted into the shared dialog window without going through
; _Dlg_Run — which would block the gallery until someone answered it.
;
; gDlgBusy stays false, so nothing is waiting on an answer and a click on the
; buttons is ignored (_Dlg_Answer no-ops). gDlgSpec is set because _Dlg_Reveal
; needs it to size the window to the height the page measures for itself; that
; measurement is the whole reason this posts a dialog instead of picking a
; height and moving an empty window.
_Gallery_OpenSampleDialog() {
    global gDlgGui, gDlgBusy, gDlgSpec, gDlgShown, gDlgOwner, gDlgToken
    global GALLERY_DIALOG_MS

    ; A real dialog is up: it owns the one window and is blocking its caller.
    if gDlgBusy
        return 0
    if !_Dlg_CanUseWebView() || !_Dlg_EnsureGui()
        return 0

    spec := _Dlg_Spec("ask",
        "Delete the layout “Dual box — 1440p”?`n`nThe slot positions it stores cannot be recovered.",
        "Delete layout",
        Map("severity", "danger", "danger", true, "okLabel", "Delete", "cancelLabel", "Keep"))

    gDlgToken += 1
    gDlgSpec := spec
    gDlgShown := false
    gDlgOwner := 0
    _Dlg_Post(spec)

    deadline := A_TickCount + GALLERY_DIALOG_MS
    while (!gDlgShown && A_TickCount < deadline)
        Sleep(15)
    if !gDlgShown
        _Dlg_Reveal(0)   ; page never reported — show it at the default height
    return (IsObject(gDlgGui) && gDlgGui.Hwnd) ? gDlgGui.Hwnd : 0
}

_Gallery_CloseSampleDialog() {
    global gDlgBusy, gDlgSpec
    ; A real dialog took the window over while the gallery was up. It is
    ; someone's blocking call now, so leave it exactly where it is.
    if gDlgBusy
        return
    _Dlg_Park()
    gDlgSpec := 0
}

; ── Layout ───────────────────────────────────────────────────────

; Shelf-packs the windows into rows on the display the cursor is on, tallest
; first, and centres each row. Nothing is resized — most of these panels are
; -Resize, and a promotional shot wants them at the size users see.
;
; When the rows do not fit the height, they overlap by an equal amount each
; rather than spilling off the bottom: an even pitch still reads as a grid,
; whereas half a panel below the taskbar reads as a bug.
_Gallery_Arrange(placed) {
    global GALLERY_GAP

    boxes := []
    for item in placed {
        if !WinExist("ahk_id " item.hwnd)
            continue
        WinGetPos(, , &boxW, &boxH, "ahk_id " item.hwnd)
        boxes.Push({ hwnd: item.hwnd, w: boxW, h: boxH })
    }
    if !boxes.Length
        return

    _Gallery_SortByHeight(boxes)

    CoordMode("Mouse", "Screen")
    MouseGetPos(&pointerX, &pointerY)
    MonitorGetWorkArea(GetMonitorIndexAtPoint(pointerX, pointerY),
        &areaL, &areaT, &areaR, &areaB)
    originX := areaL + GALLERY_GAP
    originY := areaT + GALLERY_GAP
    areaW := (areaR - areaL) - 2 * GALLERY_GAP
    areaH := (areaB - areaT) - 2 * GALLERY_GAP

    rows := _Gallery_Rows(boxes, areaW)
    stacked := 0
    for row in rows
        stacked += row.h
    total := stacked + GALLERY_GAP * (rows.Length - 1)

    if (total <= areaH) {
        rowY := originY + (areaH - total) // 2
        for row in rows {
            _Gallery_PlaceRow(row, originX, areaW, rowY)
            rowY += row.h + GALLERY_GAP
        }
        return
    }

    pitchY := (rows.Length > 1) ? (areaH - rows[rows.Length].h) / (rows.Length - 1) : 0
    for index, row in rows
        _Gallery_PlaceRow(row, originX, areaW, originY + Round(pitchY * (index - 1)))
}

; Greedy shelf packing: a window joins the current row while it still fits the
; width, otherwise it starts the next one. Sorting by height first is what keeps
; a row from being one tall panel and three short ones.
_Gallery_Rows(boxes, areaW) {
    global GALLERY_GAP
    rows := []
    current := []
    used := 0
    for box in boxes {
        needed := (current.Length ? GALLERY_GAP : 0) + box.w
        if (current.Length && used + needed > areaW) {
            rows.Push(_Gallery_Row(current, used))
            current := []
            used := 0
            needed := box.w
        }
        current.Push(box)
        used += needed
    }
    if current.Length
        rows.Push(_Gallery_Row(current, used))
    return rows
}

_Gallery_Row(items, width) {
    tallest := 0
    for box in items
        tallest := Max(tallest, box.h)
    return { items: items, w: width, h: tallest }
}

; Rows are centred and top-aligned. Top-aligned reads as a grid; centring each
; window in its row band would stagger them for no gain.
_Gallery_PlaceRow(row, originX, areaW, rowY) {
    global GALLERY_GAP
    boxX := originX + Max(0, (areaW - row.w) // 2)
    for box in row.items {
        try WinMove(boxX, rowY, , , "ahk_id " box.hwnd)
        boxX += box.w + GALLERY_GAP
    }
}

; Insertion sort, descending by height. A dozen windows never justifies more.
_Gallery_SortByHeight(boxes) {
    loop boxes.Length - 1 {
        i := A_Index + 1
        held := boxes[i]
        j := i - 1
        while (j >= 1 && boxes[j].h < held.h) {
            boxes[j + 1] := boxes[j]
            j -= 1
        }
        boxes[j + 1] := held
    }
}

; ── Closing ──────────────────────────────────────────────────────

_Gallery_CloseAll() {
    global _Gallery_Active, _Gallery_Placed, GALLERY_PARK_X

    ; Reverse order, so the dialog window is released before anything that might
    ; want to raise a real one on its way out.
    index := _Gallery_Placed.Length
    while (index >= 1) {
        item := _Gallery_Placed[index]
        index -= 1
        try {
            if IsObject(item.closeFn) {
                item.closeFn.Call()
            } else if (item.closeFn != "") {
                fn := _Gallery_Lookup(item.closeFn)
                if fn
                    fn.Call()
            } else if item.park {
                WinMove(GALLERY_PARK_X, GALLERY_PARK_X, , , "ahk_id " item.hwnd)
            } else if WinExist("ahk_id " item.hwnd) {
                WinClose("ahk_id " item.hwnd)
            }
        }
    }

    _Gallery_Placed := []
    _Gallery_Active := false
    LogInfo("Gallery", "GUI gallery closed.")
}

; ── Name-based lookups ───────────────────────────────────────────
; See the header: a name that is not in this build throws "Variable not found",
; and that answer is the point — it is what lets one file know about panels the
; build it is running in may not contain.

_Gallery_Lookup(name) {
    try {
        target := %name%
        return (target is Func) ? target : 0
    }
    return 0
}

_Gallery_Const(name, fallback) {
    try {
        value := %name%
        if (value != "")
            return value
    }
    return fallback
}

_Gallery_HwndFrom(names) {
    for name in names {
        panel := 0
        try panel := %name%
        if !IsObject(panel)
            continue
        hwnd := 0
        try hwnd := panel.Hwnd
        ; WinExist and not just a non-zero handle: a panel that was built but is
        ; hidden (the roster between shows) has a window this must not claim.
        if (hwnd && WinExist("ahk_id " hwnd))
            return hwnd
    }
    return 0
}

_Gallery_Join(list) {
    out := ""
    for entry in list
        out .= (out = "" ? "" : "; ") entry
    return out
}
