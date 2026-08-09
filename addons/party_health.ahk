#Requires AutoHotkey v2.0

; A combat HUD: a strip along the bottom of the active client showing every
; fighting character's health and mana, one cell per client.
;
; The client roster shows the same numbers, but it is a window you open — and
; the radial ring is built to close the moment you click it. Neither is a thing
; you can watch while fighting, which is the only time this data matters.
;
; It reads no memory of its own. Everything comes from the core's shared client
; poll through OnSnapshot, which already joins the per-character block to the
; live battle array (see ReadCharacterVitals).
;
; ── Two properties it must not lose ──────────────────────────────
;
; **Click-through.** The HUD sits over the game, so WS_EX_TRANSPARENT is what
; keeps it from swallowing clicks meant for the client underneath. A combat
; overlay that eats a click during a fight is worse than no overlay.
;
; **No focus stealing.** WS_EX_NOACTIVATE, and shown with SW_SHOWNOACTIVATE
; rather than Gui.Show(): appearing mid-fight must never pull focus off the
; game.
;
; ── Placement is derived, never stored ───────────────────────────
;
; The strip is centred along the bottom of the ACTIVE client's rect and is
; never wider than that client, so it belongs to the window being played and
; follows across a multi-box layout. There is deliberately no saved position
; and no drag: a stored coordinate would be wrong the moment the user alt-tabbed
; to a differently-placed window.
;
; Native controls rather than a WebView2 panel, deliberately: this window is
; visible for the whole session, and a Chromium subprocess parked on top of the
; game for a handful of progress bars is the wrong trade. It is also why it
; works identically in both interface modes with no second frontend.

; Layout, in pixels.
;
; Cells sit side by side; one cell is one client. Height is the constraint that
; shapes the rest: the strip has to clear the game's own HP/MP/SP gauges and
; still sit inside the client. So each value sits on its own line with a hairline
; bar under it, rather than a tall bar with the figures alongside — the numbers
; are what you read, and the bar only has to answer "how full" at a glance.
global _PartyHealth_PAD := 4
global _PartyHealth_GAP := 8
; Cells share the client's width and stop growing at MAX, so two clients on a
; wide window get a sane strip rather than two enormous cells. MIN is roughly
; the width of the widest figures a cell shows.
global _PartyHealth_CELL_W_MAX := 210
global _PartyHealth_CELL_W_MIN := 110
global _PartyHealth_NAME_H := 12
global _PartyHealth_VAL_H := 11
global _PartyHealth_BAR_H := 3
global _PartyHealth_PET_BAR_H := 2
global _PartyHealth_ROW_STEP := 14      ; value line + its bar
global _PartyHealth_PET_STEP := 13
; A party holds five characters (BATTLE_PARTY_SLOTS counts their pets too), so
; no legitimate group is larger. Also what keeps the strip from being squeezed
; into unreadability by a stray extra client.
global _PartyHealth_MAX_CELLS := 5

; ── Colours ──────────────────────────────────────────────────────
;
; These ARE the shared design tokens from ui\common\style.css, converted to hex
; once because native Gui controls cannot read CSS custom properties — that
; conversion is the only reason they are repeated here rather than referenced.
; The token each came from is named beside it.
;
; Do not invent a colour in this file. Take the matching token and convert it;
; a HUD drifting away from the panels is how a shared palette stops being one.
global _PartyHealth_COL_BG := "13171E"          ; --surface
global _PartyHealth_COL_TROUGH := "2A313C"      ; --border
global _PartyHealth_COL_TEXT := "EBEFF5"        ; --text
global _PartyHealth_COL_TEXT_MUTED := "9BA5B7"  ; --text-muted
global _PartyHealth_COL_TEXT_DIM := "676F7D"    ; --text-dim
global _PartyHealth_COL_MP := "53A3F2"          ; --info
global _PartyHealth_COL_PET := "C084FC"         ; --poi-portal
; The HP bar is graded across these three rather than switching at a threshold,
; so a bar that is merely dropping looks different from one that is nearly out.
global _PartyHealth_COL_HP_FULL := "30C765"     ; --success
global _PartyHealth_COL_HP_WARN := "F2A618"     ; --warning
global _PartyHealth_COL_HP_LOW := "F54748"      ; --danger
; Gradient steps. The colour is recomputed only when the bar crosses one, since
; Opt() forces a repaint and doing that every poll makes the bar flicker.
global _PartyHealth_HP_STEPS := 24

global _PartyHealth_Gui := 0
; Cells currently built: array of control bundles, one per client.
global _PartyHealth_Cells := []
; What the current layout was built for — the clients, whether a pet row is
; needed, and the cell width. Values change every poll; the layout only changes
; when this string does.
global _PartyHealth_Signature := ""
global _PartyHealth_Shown := false
; Size of the built strip, and where it was last placed, so following the
; active window can skip the move when nothing has changed.
global _PartyHealth_Size := { w: 0, h: 0 }
global _PartyHealth_LastPos := ""
; The 1 Hz snapshot poll is far too slow to track an alt-tab, so placement runs
; on its own short timer. It only measures one window and moves another — no
; memory is read, which is why this does not violate the one-shared-poll rule.
global _PartyHealth_FOLLOW_MS := 200

; How the strip behaves — see _PartyHealth_MODES.
;
; "peek" is the default because that is how the thing is actually used: press
; the key, take in four numbers, get back to the fight. Left on screen it is
; another gauge competing with the game's own, and most of the time it is
; telling you what you already know.
;
; The master on/off is the addon system's own (Settings > Addons); this used to
; carry a second Enabled flag of its own, which was one switch too many.
global _PartyHealth_Mode := "peek"
global _PartyHealth_MODES := ["peek", "combat", "always"]
global _PartyHealth_MODE_LABELS := [
    "Only when I press the key, for a few seconds",
    "Automatically while a client is in battle",
    "Always, whenever a client is running"
]
; How long a peek lasts. Long enough to read four numbers without being long
; enough to become scenery.
global _PartyHealth_PeekSeconds := 5
global _PartyHealth_Peeking := false
; The client a peek was opened on. A peek belongs to that window: it answers a
; question about that client's fight, so switching away ends it rather than
; dragging the strip along and regrouping around wherever the user landed.
global _PartyHealth_PeekPid := 0
global _PartyHealth_ShowPets := true

RegisterAddon(Map(
    "name",              "PartyHealth",
    "settingsLabel",     "Party Health HUD",
    "OnInit",            _PartyHealth_OnInit,
    "OnSettingsWeb",     _PartyHealth_OnSettingsWeb,
    "OnSettingsWebSave", _PartyHealth_OnSettingsWebSave,
    "OnTrayMenu",        _PartyHealth_OnTrayMenu,
    "OnSnapshot",        _PartyHealth_OnSnapshot
))

RegisterHotkeyAction(Map(
    "id", "partyHealthToggle",
    "label", "Peek at party health",
    "category", "Party Health HUD",
    ; Bound by default, unlike most actions: in the default mode this key is the
    ; only way the HUD ever appears, and an unbound one would make the whole
    ; addon look broken.
    "default", "^!h",
    "addon", "PartyHealth",
    "handler", (*) => _PartyHealth_Peek()
))

_PartyHealth_OnInit() {
    global _PartyHealth_FOLLOW_MS
    _PartyHealth_LoadConfig()
    SetTimer(_PartyHealth_Follow, _PartyHealth_FOLLOW_MS)
}

_PartyHealth_LoadConfig() {
    global _PartyHealth_Mode, _PartyHealth_ShowPets, _PartyHealth_PeekSeconds, CONFIG_INI
    _PartyHealth_ShowPets := (Trim(ConfigRead("PartyHealth", "ShowPets", "1")) != "0")
    _PartyHealth_Mode := _PartyHealth_NormalizeMode(Trim(ConfigRead("PartyHealth", "Mode", "peek")))
    secs := Trim(ConfigRead("PartyHealth", "PeekSeconds", "5"))
    if (IsInteger(secs) && Integer(secs) >= 1 && Integer(secs) <= 60) {
        _PartyHealth_PeekSeconds := Integer(secs)
    }
}

_PartyHealth_SaveConfig() {
    global _PartyHealth_Mode, _PartyHealth_ShowPets, _PartyHealth_PeekSeconds, CONFIG_INI
    try {
        EnsureIniUtf16(CONFIG_INI)
        IniWrite(_PartyHealth_ShowPets ? "1" : "0", CONFIG_INI, "PartyHealth", "ShowPets")
        IniWrite(_PartyHealth_Mode, CONFIG_INI, "PartyHealth", "Mode")
        IniWrite(_PartyHealth_PeekSeconds, CONFIG_INI, "PartyHealth", "PeekSeconds")
    } catch as err {
        LogWarn("PartyHealth", "Could not save settings: " err.Message)
    }
}

_PartyHealth_NormalizeMode(chosen) {
    global _PartyHealth_MODES
    for m in _PartyHealth_MODES {
        if (StrLower(chosen) = m) {
            return m
        }
    }
    return "peek"
}

_PartyHealth_ModeIndex(chosen) {
    global _PartyHealth_MODES
    for i, m in _PartyHealth_MODES {
        if (m = chosen) {
            return i
        }
    }
    return 1
}

; ── Settings ─────────────────────────────────────────────────────

_PartyHealth_OnSettingsWeb() {
    global _PartyHealth_ShowPets, _PartyHealth_Mode, _PartyHealth_MODE_LABELS, _PartyHealth_PeekSeconds
    return [
        Map("type", "info", "text",
            "A strip along the bottom of the client you are playing, showing every`n"
            . "character's health and mana side by side.`n`n"
            . "By default it only appears while you hold it up with "
            . GetHotkeyDisplay("partyHealthToggle") . " —`n"
            . "long enough to read, then gone. It follows the active window and is`n"
            . "click-through, so it never intercepts a click meant for the game."),
        Map("type", "dropdown", "id", "modeIdx", "label", "Show it:",
            "options", _PartyHealth_MODE_LABELS,
            "value", _PartyHealth_ModeIndex(_PartyHealth_Mode) - 1,
            "default", _PartyHealth_ModeIndex(_PartyHealth_NormalizeMode(DefaultRead("PartyHealth", "Mode", "peek"))) - 1),
        Map("type", "number", "id", "peekSeconds", "label", "Seconds a peek lasts:",
            "min", 1, "max", 60, "value", _PartyHealth_PeekSeconds,
            "default", Integer(DefaultRead("PartyHealth", "PeekSeconds", 5))),
        Map("type", "checkbox", "id", "showPets", "label", "Include pet health",
            "value", _PartyHealth_ShowPets ? true : false,
            "default", (DefaultRead("PartyHealth", "ShowPets", "1") != "0"))
    ]
}

_PartyHealth_OnSettingsWebSave(values) {
    global _PartyHealth_MODES, _PartyHealth_ShowPets, _PartyHealth_Mode, _PartyHealth_PeekSeconds
    _PartyHealth_ShowPets := values.Has("showPets") && values["showPets"] ? true : false
    idx := values.Has("modeIdx") ? Integer(values["modeIdx"]) + 1 : 1
    if (idx < 1 || idx > _PartyHealth_MODES.Length) {
        idx := 1
    }
    _PartyHealth_Mode := _PartyHealth_MODES[idx]
    if values.Has("peekSeconds") {
        secs := Integer(values["peekSeconds"])
        if (secs >= 1 && secs <= 60) {
            _PartyHealth_PeekSeconds := secs
        }
    }
    _PartyHealth_SaveConfig()
    ; The pet toggle changes cell height, so force a rebuild rather than
    ; leaving the old layout with a stale bar in it.
    _PartyHealth_Invalidate()
    _PartyHealth_Refresh()
}

_PartyHealth_OnTrayMenu(trayGroups) {
    ; Not named `menu`: Menu() is a built-in and shadowing one is a load-time
    ; conflict Ahk2Exe refuses to compile.
    clientsMenu := trayGroups["clients"]
    clientsMenu.Add("Peek at party health`t" GetHotkeyDisplay("partyHealthToggle"),
        (*) => _PartyHealth_Peek())
}

; Show the strip for a few seconds, or take it away again if it is already up.
;
; Pressing the key twice must dismiss it rather than extend it: the second
; press means "I have read it", and re-arming the timer would leave the HUD on
; screen for longer the more impatiently it was dismissed.
_PartyHealth_Peek() {
    global _PartyHealth_Peeking, _PartyHealth_PeekSeconds, _PartyHealth_PeekPid
    if _PartyHealth_Peeking {
        _PartyHealth_EndPeek()
        return
    }
    _PartyHealth_Peeking := true
    _PartyHealth_PeekPid := _PartyHealth_ActivePid()
    SetTimer(_PartyHealth_EndPeek, -(_PartyHealth_PeekSeconds * 1000))
    ; Poll before drawing rather than using the last tick's data, which can be
    ; a second old. A peek follows a keypress that usually follows a window
    ; switch, so the stalest possible moment is exactly when it is asked — and
    ; a stale snapshot shows the new client the fight the OLD one was in.
    UpdateClientSnapshots()
    _PartyHealth_Refresh()
}

_PartyHealth_EndPeek() {
    global _PartyHealth_Peeking, _PartyHealth_PeekPid
    SetTimer(_PartyHealth_EndPeek, 0)
    _PartyHealth_Peeking := false
    _PartyHealth_PeekPid := 0
    _PartyHealth_Refresh()
}

; ── What it lists ────────────────────────────────────────────────

; The pid of the game window focused right now.
;
; Not snap.isActive, which is up to a second old: a peek is triggered by a
; keypress that usually lands immediately after alt-tabbing to the very client
; being asked about, so a stale answer is wrong exactly when it is used. Asking
; Windows costs nothing and cannot lag.
_PartyHealth_ActivePid() {
    hwnd := WinActive(GAME_WIN_FILTER)
    if !hwnd {
        return 0
    }
    pid := 0
    try pid := WinGetPID("ahk_id " hwnd)
    return pid
}

; The clients in ONE fight — the one the focused window is in.
;
; With six clients boxed across two battles, "everyone fighting" is two
; parties' worth of bars in one strip, with the ones you can act on mixed into
; ones you cannot. Clients agree on `battleKey` only within a single battle
; (see BattleFingerprint), so the focused window picks the fight.
;
; activePid is passed in rather than looked up here so the grouping stays a
; pure function of its inputs.
;
; Returns empty when nobody is fighting — that is the signal the caller uses to
; decide between hiding and showing everything. If a fight is on but no key
; could be read, every fighting client is returned rather than none: a strip
; listing too much beats a strip that vanishes mid-battle.
_PartyHealth_FightGroup(snapshots, activePid) {
    anchor := ""
    for snap in snapshots {
        if (snap.pid = activePid && snap.inBattle && snap.battleKey != "") {
            anchor := snap.battleKey
            break
        }
    }
    if (anchor = "") {
        for snap in snapshots {
            if (snap.inBattle && snap.battleKey != "") {
                anchor := snap.battleKey
                break
            }
        }
    }
    out := []
    for snap in snapshots {
        if !snap.inBattle {
            continue
        }
        if (anchor = "" || snap.battleKey = anchor) {
            out.Push(snap)
        }
    }
    return out
}

; Grouping applies to peek as well as to the automatic battle mode — the two
; differ in WHEN the strip appears, not in what it lists. Gating the grouping
; on the battle mode alone left the default mode showing whichever five clients
; the poll happened to list first, which looked right only when the fight you
; were in happened to sort first.
;
; A peek outside a fight falls back to every client, since "check on my alts"
; is the other reason to press the key and returning nothing would read as
; broken.
_PartyHealth_VisibleClients(snapshots) {
    global _PartyHealth_Mode
    if (_PartyHealth_Mode = "always") {
        return _PartyHealth_Capped(snapshots)
    }
    fight := _PartyHealth_FightGroup(snapshots, _PartyHealth_ActivePid())
    if (fight.Length > 0) {
        return _PartyHealth_Capped(fight)
    }
    if (_PartyHealth_Mode = "combat") {
        return []
    }
    return _PartyHealth_Capped(snapshots)
}

_PartyHealth_Capped(clients) {
    global _PartyHealth_MAX_CELLS
    if (clients.Length <= _PartyHealth_MAX_CELLS) {
        return clients
    }
    out := []
    for snap in clients {
        out.Push(snap)
        if (out.Length >= _PartyHealth_MAX_CELLS) {
            break
        }
    }
    return out
}

; HP bar colour for a fill fraction: green at full, amber through the middle,
; red as it runs out.
;
; Graded rather than switched at a threshold — a single cutover tells you only
; "past it or not", while a ramp shows a bar sliding towards trouble while
; there is still time to act on it. Full green is held down to 90% so a
; character at a scratch below full does not already look hurt.
_PartyHealth_HpColor(frac) {
    global _PartyHealth_COL_HP_FULL, _PartyHealth_COL_HP_WARN, _PartyHealth_COL_HP_LOW
    if (frac >= 0.9) {
        return _PartyHealth_COL_HP_FULL
    }
    if (frac >= 0.45) {
        ; 0.45 -> amber, 0.90 -> green
        return _PartyHealth_MixColor(_PartyHealth_COL_HP_WARN, _PartyHealth_COL_HP_FULL,
            (frac - 0.45) / 0.45)
    }
    ; 0 -> red, 0.45 -> amber
    return _PartyHealth_MixColor(_PartyHealth_COL_HP_LOW, _PartyHealth_COL_HP_WARN, frac / 0.45)
}

; Linear blend of two RRGGBB strings; t=0 gives the first, t=1 the second.
_PartyHealth_MixColor(fromHex, toHex, t) {
    if (t < 0) {
        t := 0
    }
    if (t > 1) {
        t := 1
    }
    out := ""
    Loop 3 {
        i := (A_Index - 1) * 2 + 1
        c1 := Integer("0x" SubStr(fromHex, i, 2))
        c2 := Integer("0x" SubStr(toHex, i, 2))
        out .= Format("{:02X}", Round(c1 + (c2 - c1) * t))
    }
    return out
}

; ── Placement ────────────────────────────────────────────────────

; Cell width for `count` clients inside `availW`, plus the resulting strip
; width. Below CELL_W_MIN the strip is allowed to be wider than the window
; rather than shrinking into unreadability — a cell narrower than its own text
; helps nobody, and the caller keeps the result on screen.
_PartyHealth_CellMetrics(count, availW) {
    global _PartyHealth_PAD, _PartyHealth_GAP, _PartyHealth_CELL_W_MAX, _PartyHealth_CELL_W_MIN
    if (count < 1) {
        return { cellW: 0, totalW: 0 }
    }
    chrome := _PartyHealth_PAD * 2 + _PartyHealth_GAP * (count - 1)
    cellW := Floor((availW - chrome) / count)
    if (cellW > _PartyHealth_CELL_W_MAX) {
        cellW := _PartyHealth_CELL_W_MAX
    }
    if (cellW < _PartyHealth_CELL_W_MIN) {
        cellW := _PartyHealth_CELL_W_MIN
    }
    return { cellW: cellW, totalW: cellW * count + chrome }
}

; Centred along the bottom edge of the active client's rect.
_PartyHealth_StripPosition(w, h) {
    area := GameClientRect()
    x := area.x + Floor((area.w - w) / 2)
    y := area.y + area.h - h
    ; A window wider than the strip is the normal case; a narrower one must not
    ; push the strip off the left edge of the client it belongs to.
    if (x < area.x) {
        x := area.x
    }
    return { x: x, y: y }
}

; Runs on its own timer so the strip tracks an alt-tab without waiting for the
; 1 Hz snapshot poll. Moves only when the target actually changed — a Move()
; every tick makes the window shimmer over the game.
_PartyHealth_Follow() {
    global _PartyHealth_Gui, _PartyHealth_Shown, _PartyHealth_Size, _PartyHealth_LastPos
    global _PartyHealth_Peeking, _PartyHealth_PeekPid
    ; Clicking away ends a peek. Following instead would slide the strip onto
    ; the new window and regroup around it, so a glance at one team turned into
    ; a half-updated view of another — and the numbers on screen would briefly
    ; belong to neither. Leaving is unambiguous.
    if (_PartyHealth_Peeking && _PartyHealth_ActivePid() != _PartyHealth_PeekPid) {
        _PartyHealth_EndPeek()
        return
    }
    if (!_PartyHealth_Shown || !IsObject(_PartyHealth_Gui) || _PartyHealth_Size.w = 0) {
        return
    }
    pos := _PartyHealth_StripPosition(_PartyHealth_Size.w, _PartyHealth_Size.h)
    key := pos.x "," pos.y
    if (key = _PartyHealth_LastPos) {
        return
    }
    _PartyHealth_LastPos := key
    try _PartyHealth_Gui.Move(pos.x, pos.y)
}

; ── Drawing ──────────────────────────────────────────────────────

_PartyHealth_OnSnapshot(snapshots) {
    _PartyHealth_Refresh()
}

_PartyHealth_Invalidate() {
    global _PartyHealth_Signature
    _PartyHealth_Signature := ""
}

_PartyHealth_Refresh() {
    global _PartyHealth_Gui, _PartyHealth_Shown, _PartyHealth_Mode, _PartyHealth_Peeking
    clients := _PartyHealth_VisibleClients(GetClientSnapshots())
    ; In "combat" mode `clients` is already the fighting ones, so an empty list
    ; is also what makes the strip go away when the fight ends.
    if (clients.Length = 0 || (_PartyHealth_Mode = "peek" && !_PartyHealth_Peeking)) {
        if (_PartyHealth_Shown && IsObject(_PartyHealth_Gui)) {
            try _PartyHealth_Gui.Hide()
            _PartyHealth_Shown := false
        }
        return
    }
    _PartyHealth_Build(clients)
    _PartyHealth_Paint(clients)
    if !_PartyHealth_Shown {
        _PartyHealth_ShowNoActivate()
    }
}

; Shown without activation, so the HUD appearing mid-fight cannot pull focus
; off the game. Gui.Show() would activate it.
_PartyHealth_ShowNoActivate() {
    global _PartyHealth_Gui, _PartyHealth_Shown
    if !IsObject(_PartyHealth_Gui) {
        return
    }
    static SW_SHOWNOACTIVATE := 4
    try DllCall("ShowWindow", "Ptr", _PartyHealth_Gui.Hwnd, "Int", SW_SHOWNOACTIVATE)
    _PartyHealth_Shown := true
}

; What the layout depends on: which clients, whether a pet row is needed, and
; how wide a cell is. Values are not in it — those change every poll and must
; not cause a rebuild.
_PartyHealth_SignatureFor(clients, cellW, anyPet) {
    sig := cellW ":" (anyPet ? "p" : "-") "|"
    for snap in clients {
        sig .= snap.pid ","
    }
    return sig
}

_PartyHealth_Build(clients) {
    global _PartyHealth_Gui, _PartyHealth_Cells, _PartyHealth_Signature, _PartyHealth_Shown
    global _PartyHealth_PAD, _PartyHealth_GAP, _PartyHealth_BAR_H, _PartyHealth_PET_BAR_H
    global _PartyHealth_COL_BG, _PartyHealth_COL_TROUGH
    global _PartyHealth_COL_MP, _PartyHealth_COL_PET, _PartyHealth_ShowPets
    global _PartyHealth_Size, _PartyHealth_LastPos
    global _PartyHealth_NAME_H, _PartyHealth_VAL_H
    global _PartyHealth_ROW_STEP, _PartyHealth_PET_STEP
    global _PartyHealth_COL_TEXT, _PartyHealth_COL_TEXT_MUTED, _PartyHealth_COL_TEXT_DIM
    global _PartyHealth_COL_HP_FULL

    anyPet := false
    if _PartyHealth_ShowPets {
        for snap in clients {
            if snap.hasPet {
                anyPet := true
            }
        }
    }
    area := GameClientRect()
    metrics := _PartyHealth_CellMetrics(clients.Length, area.w)
    sig := _PartyHealth_SignatureFor(clients, metrics.cellW, anyPet)
    if (sig = _PartyHealth_Signature && IsObject(_PartyHealth_Gui)) {
        return
    }

    ; AHK cannot destroy individual controls, so a layout change rebuilds the
    ; whole window. That is why the signature exists — this must be rare.
    if IsObject(_PartyHealth_Gui) {
        try _PartyHealth_Gui.Destroy()
    }
    _PartyHealth_Cells := []
    _PartyHealth_Shown := false
    _PartyHealth_LastPos := ""

    ; +E0x08000000 WS_EX_NOACTIVATE, +E0x20 WS_EX_TRANSPARENT (click-through).
    g := Gui("+AlwaysOnTop -Caption +ToolWindow +E0x08000000 +E0x20 -DPIScale", "Maps++ Party Health")
    g.BackColor := _PartyHealth_COL_BG
    g.MarginX := 0
    g.MarginY := 0

    cellW := metrics.cellW
    x := _PartyHealth_PAD
    cellBottom := _PartyHealth_PAD
    for snap in clients {
        y := _PartyHealth_PAD
        g.SetFont("s8 bold", "Segoe UI")
        nameCtl := g.Add("Text", Format("x{1} y{2} w{3} h{4} c{5} BackgroundTrans",
            x, y, cellW, _PartyHealth_NAME_H, _PartyHealth_COL_TEXT), "")
        y += _PartyHealth_NAME_H + 2
        g.SetFont("s7 norm", "Segoe UI")

        ; Value on its own line, hairline bar directly beneath it.
        hpTxt := g.Add("Text", Format("x{1} y{2} w{3} h{4} c{5} BackgroundTrans",
            x, y, cellW, _PartyHealth_VAL_H, _PartyHealth_COL_TEXT_MUTED), "")
        hpBar := g.Add("Progress", Format("x{1} y{2} w{3} h{4} Background{5} c{6} Range0-1000",
            x, y + _PartyHealth_VAL_H, cellW, _PartyHealth_BAR_H,
            _PartyHealth_COL_TROUGH, _PartyHealth_COL_HP_FULL), 0)
        y += _PartyHealth_ROW_STEP

        mpTxt := g.Add("Text", Format("x{1} y{2} w{3} h{4} c{5} BackgroundTrans",
            x, y, cellW, _PartyHealth_VAL_H, _PartyHealth_COL_TEXT_MUTED), "")
        mpBar := g.Add("Progress", Format("x{1} y{2} w{3} h{4} Background{5} c{6} Range0-1000",
            x, y + _PartyHealth_VAL_H, cellW, _PartyHealth_BAR_H,
            _PartyHealth_COL_TROUGH, _PartyHealth_COL_MP), 0)
        y += _PartyHealth_ROW_STEP

        petBar := 0, petTxt := 0
        if anyPet {
            ; Built for every cell once any client has a pet, so all cells share
            ; one height; a client without one simply leaves it blank.
            petTxt := g.Add("Text", Format("x{1} y{2} w{3} h{4} c{5} BackgroundTrans",
                x, y, cellW, _PartyHealth_VAL_H, _PartyHealth_COL_TEXT_DIM), "")
            petBar := g.Add("Progress", Format("x{1} y{2} w{3} h{4} Background{5} c{6} Range0-1000",
                x, y + _PartyHealth_VAL_H, cellW, _PartyHealth_PET_BAR_H,
                _PartyHealth_COL_TROUGH, _PartyHealth_COL_PET), 0)
            y += _PartyHealth_PET_STEP
        }

        _PartyHealth_Cells.Push({
            pid: snap.pid, name: nameCtl,
            hpBar: hpBar, hpTxt: hpTxt, mpBar: mpBar, mpTxt: mpTxt,
            petBar: petBar, petTxt: petTxt
        })
        x += cellW + _PartyHealth_GAP
        cellBottom := y
    }

    height := cellBottom + _PartyHealth_PAD
    _PartyHealth_Gui := g
    _PartyHealth_Signature := sig
    _PartyHealth_Size := { w: metrics.totalW, h: height }

    pos := _PartyHealth_StripPosition(metrics.totalW, height)
    _PartyHealth_LastPos := pos.x "," pos.y
    g.Show(Format("x{1} y{2} w{3} h{4} NoActivate Hide", pos.x, pos.y, metrics.totalW, height))
}

_PartyHealth_Paint(clients) {
    global _PartyHealth_Cells, _PartyHealth_ShowPets
    global _PartyHealth_HP_STEPS
    if (_PartyHealth_Cells.Length = 0) {
        return
    }
    for i, snap in clients {
        if (i > _PartyHealth_Cells.Length) {
            break
        }
        cell := _PartyHealth_Cells[i]
        cell.name.Text := snap.charName != "" ? snap.charName : "PID " snap.pid

        if !snap.hasVitals {
            cell.hpBar.Value := 0
            cell.mpBar.Value := 0
            cell.hpTxt.Text := "—"
            cell.mpTxt.Text := ""
            if IsObject(cell.petBar) {
                cell.petBar.Value := 0
                cell.petTxt.Text := ""
            }
            continue
        }

        hpFrac := snap.maxHp > 0 ? snap.hp / snap.maxHp : 0
        cell.hpBar.Value := Round(hpFrac * 1000)
        ; Recoloured only when the fill crosses a step, not on every poll: Opt()
        ; forces a repaint and doing that once a second makes the bar flicker.
        step := Round(hpFrac * _PartyHealth_HP_STEPS)
        if (!cell.HasOwnProp("hpStep") || cell.hpStep != step) {
            try cell.hpBar.Opt("c" _PartyHealth_HpColor(hpFrac))
            cell.hpStep := step
        }
        cell.hpTxt.Text := _PartyHealth_Num(snap.hp) " / " _PartyHealth_Num(snap.maxHp)

        cell.mpBar.Value := snap.maxMp > 0 ? Round(snap.mp * 1000 / snap.maxMp) : 0
        cell.mpTxt.Text := _PartyHealth_Num(snap.mp) " / " _PartyHealth_Num(snap.maxMp)

        if IsObject(cell.petBar) {
            if (_PartyHealth_ShowPets && snap.hasPet && snap.petMaxHp > 0) {
                cell.petBar.Value := Round(snap.petHp * 1000 / snap.petMaxHp)
                cell.petTxt.Text := _PartyHealth_Num(snap.petHp) " / " _PartyHealth_Num(snap.petMaxHp)
            } else {
                cell.petBar.Value := 0
                cell.petTxt.Text := ""
            }
        }
    }
}

; 2116113 -> "2,116,113". Six-figure health is unreadable without separators,
; which is the whole reason the HUD shows figures under the bars.
_PartyHealth_Num(n) {
    text := Format("{:d}", Round(n))
    sign := ""
    if (SubStr(text, 1, 1) = "-") {
        sign := "-"
        text := SubStr(text, 2)
    }
    out := ""
    while (StrLen(text) > 3) {
        out := "," SubStr(text, -3) out
        text := SubStr(text, 1, StrLen(text) - 3)
    }
    return sign text out
}
