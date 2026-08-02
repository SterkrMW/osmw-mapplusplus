#Requires AutoHotkey v2.0

; Draws every *other* running client on the minimap, so a multi-boxer can see
; where their alts are without alt-tabbing. Positions come from the core's
; shared client poll (OnSnapshot) — this addon reads no memory of its own — and
; use the same per-map calibration as the local player's marker, so no extra
; coordinate math is involved.

; One colour per client, in poll order. marker.png stays the local player's.
global _PartyMarkers_COLORS := ["1E90FF", "32CD32", "FF8C00", "FF69B4", "00CED1", "FFD700", "9370DB", "FF4500"]
; "autohide" (default), "always" or "never" — see MARKER_LABEL_MODES.
global _PartyMarkers_LabelMode := "autohide"
; Slots used by the last draw, so a hover change can toggle just those labels.
global _PartyMarkers_UsedCount := 0
; Control pool: AHK can't destroy individual controls, so they are created once
; per overlay Gui and then shown/hidden/moved.
global _PartyMarkers_Pool := []
; Hwnd of the overlay Gui the pool belongs to. When the overlay is rebuilt (a
; scale change, say) this no longer matches and the pool is recreated — that
; self-heals regardless of who rebuilt the Gui.
global _PartyMarkers_GuiHwnd := 0

RegisterAddon(Map(
    "name",             "PartyMarkers",
    "settingsLabel",    "Party Markers",
    "OnInit",           _PartyMarkers_OnInit,
    "OnSettings",       _PartyMarkers_OnSettings,
    "OnSnapshot",       _PartyMarkers_OnSnapshot,
    "OnOverlayHover",   _PartyMarkers_OnOverlayHover,
    "OnOverlayHide",    _PartyMarkers_HideAll,
    "OnOverlayRebuild", _PartyMarkers_Reset
))

_PartyMarkers_OnInit() {
    _PartyMarkers_LoadConfig()
}

_PartyMarkers_OnSettings(ctx) {
    global _PartyMarkers_LabelMode, MARKER_LABEL_MODES, MARKER_LABEL_MODE_LABELS

    g := ctx.gui
    g.Add("Text", "xs y+16 w430",
        "Shows your other running clients on the minimap, each in its own colour.`n"
        . "Your own character keeps the standard marker.")

    g.Add("Text", "xs y+14 w130", "Show names:")
    modeDdl := g.Add("DropDownList", "x+10 yp-3 w250", MARKER_LABEL_MODE_LABELS)
    modeDdl.Value := MarkerLabelModeIndex(_PartyMarkers_LabelMode)

    ctx.saveHandlers.Push(() => _PartyMarkers_ApplySettings(MARKER_LABEL_MODES[modeDdl.Value]))
}

_PartyMarkers_ApplySettings(labelMode) {
    global _PartyMarkers_LabelMode, _PartyMarkers_Pool, _PartyMarkers_UsedCount
    _PartyMarkers_LabelMode := labelMode
    _PartyMarkers_SaveConfig()
    SetMarkerLabelsVisible(_PartyMarkers_Pool, _PartyMarkers_UsedCount,
        ShouldShowMarkerLabels(labelMode))
}

_PartyMarkers_LoadConfig() {
    global _PartyMarkers_LabelMode, CONFIG_INI
    mode := Trim(IniRead(CONFIG_INI, "PartyMarkers", "LabelMode", ""))
    if (mode != "") {
        _PartyMarkers_LabelMode := NormalizeMarkerLabelMode(mode)
        return
    }
    ; Migrate the older on/off setting: off stays off, on becomes autohide.
    _PartyMarkers_LabelMode := (Trim(IniRead(CONFIG_INI, "PartyMarkers", "ShowNames", "1")) = "0")
        ? "never" : "autohide"
}

_PartyMarkers_SaveConfig() {
    global _PartyMarkers_LabelMode, CONFIG_INI
    IniWrite(_PartyMarkers_LabelMode, CONFIG_INI, "PartyMarkers", "LabelMode")
}

; Labels are positioned on every draw, so a hover change is a plain toggle.
_PartyMarkers_OnOverlayHover(isOver) {
    global _PartyMarkers_Pool, _PartyMarkers_UsedCount, _PartyMarkers_LabelMode
    SetMarkerLabelsVisible(_PartyMarkers_Pool, _PartyMarkers_UsedCount,
        ShouldShowMarkerLabels(_PartyMarkers_LabelMode))
}

; ── Drawing ──────────────────────────────────────────────────────

_PartyMarkers_OnSnapshot(snapshots) {
    global gOverlayVisible, gGui, gCurrentMapName, gCachedPID, GAME_STATE_WORLD
    global MINIMAP_MAP_INSET, _PartyMarkers_Pool, _PartyMarkers_LabelMode, _PartyMarkers_UsedCount

    if (!gOverlayVisible || !IsObject(gGui) || !gGui.Hwnd) {
        return
    }
    currentMapId := MapIdFromName(gCurrentMapName)
    if (currentMapId = "") {
        return
    }
    _PartyMarkers_EnsurePool(snapshots.Length)

    scale := MinimapScaleFactor()
    size := MinimapMarkerSize()
    showLabels := ShouldShowMarkerLabels(_PartyMarkers_LabelMode)
    used := 0
    for snap in snapshots {
        if (used >= _PartyMarkers_Pool.Length) {
            break
        }
        ; The local player already has marker.png.
        if (snap.pid = gCachedPID) {
            continue
        }
        if (MapIdFromName(snap.mapId) != currentMapId) {
            continue
        }
        ; Outside the overworld (loading, battle, login) the coordinates are
        ; stale — better no marker than one frozen in the wrong place.
        if (snap.gameState != GAME_STATE_WORLD) {
            continue
        }

        pos := WorldToOverlayPixels(snap.x, snap.y, gCurrentMapName)
        px := Clamp(Round(pos.x * scale), 0, MinimapDisplayW() - size)
        py := Clamp(Round(pos.y * scale), 0, MinimapDisplayH() - size)

        used++
        entry := _PartyMarkers_Pool[used]
        ; Colour is fixed per pool slot at creation, so slot N is always the
        ; same colour for as long as the overlay lives.
        entry.dot.Move(px + MINIMAP_MAP_INSET, py + MINIMAP_MAP_INSET, size, size)
        entry.dot.Visible := true

        ; Positioned even when hidden, so showing them on hover is instant.
        PositionMarkerLabel(entry.label, snap.charName,
            px + MINIMAP_MAP_INSET, py + MINIMAP_MAP_INSET, size,
            showLabels && snap.charName != "")
    }
    _PartyMarkers_UsedCount := used

    ; Hide whatever this tick didn't use.
    loop _PartyMarkers_Pool.Length {
        if (A_Index <= used) {
            continue
        }
        _PartyMarkers_Pool[A_Index].dot.Visible := false
        _PartyMarkers_Pool[A_Index].label.Visible := false
    }
}

_PartyMarkers_ColorFor(index) {
    global _PartyMarkers_COLORS
    return _PartyMarkers_COLORS[Mod(index - 1, _PartyMarkers_COLORS.Length) + 1]
}

; Creates (or recreates) the control pool on the current overlay Gui.
_PartyMarkers_EnsurePool(needed) {
    global gGui, gMultiClientCount, _PartyMarkers_Pool, _PartyMarkers_GuiHwnd

    if (_PartyMarkers_GuiHwnd = gGui.Hwnd && _PartyMarkers_Pool.Length >= needed) {
        return
    }
    if (_PartyMarkers_GuiHwnd != gGui.Hwnd) {
        _PartyMarkers_Pool := []
        _PartyMarkers_GuiHwnd := gGui.Hwnd
    }
    ; Controls are added after the map picture, so they draw on top of it.
    target := Max(needed, Max(8, gMultiClientCount))
    while (_PartyMarkers_Pool.Length < target) {
        dot := gGui.AddText("x0 y0 w1 h1 Hidden Background" _PartyMarkers_ColorFor(_PartyMarkers_Pool.Length + 1), "")
        ; Keep the theme off these — themed statics ignore the background colour.
        DllCall("uxtheme\SetWindowTheme", "Ptr", dot.Hwnd, "WStr", "", "WStr", "")
        _PartyMarkers_Pool.Push({ dot: dot, label: AddMarkerLabelControl(gGui) })
    }
}

_PartyMarkers_HideAll(*) {
    global _PartyMarkers_Pool
    for entry in _PartyMarkers_Pool {
        try {
            entry.dot.Visible := false
            entry.label.Visible := false
        }
    }
}

; The overlay Gui was destroyed — drop the now-dangling control references.
_PartyMarkers_Reset(*) {
    global _PartyMarkers_Pool, _PartyMarkers_GuiHwnd
    _PartyMarkers_Pool := []
    _PartyMarkers_GuiHwnd := 0
}
