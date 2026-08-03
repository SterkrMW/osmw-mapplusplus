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
; Whether the layer draws at all. Persisted, so a layer turned off stays off
; across a reload rather than quietly coming back.
global _PartyMarkers_LayerVisible := true
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
    "OnSettingsWeb",     _PartyMarkers_OnSettingsWeb,
    "OnSettingsWebSave", _PartyMarkers_OnSettingsWebSave,
    "OnTrayMenu",       _PartyMarkers_OnTrayMenu,
    "OnSnapshot",       _PartyMarkers_OnSnapshot,
    "OnOverlayHover",   _PartyMarkers_OnOverlayHover,
    "OnOverlayHide",    _PartyMarkers_HideAll,
    "OnOverlayRebuild", _PartyMarkers_Reset
))

RegisterHotkeyAction(Map(
    "id", "partyMarkersToggleLayer",
    "label", "Show/hide party marker layer",
    "category", "Party Markers",
    "default", "^!i",
    "addon", "PartyMarkers",
    "handler", (*) => _PartyMarkers_ToggleLayer()
))

_PartyMarkers_OnInit() {
    _PartyMarkers_LoadConfig()
}

_PartyMarkers_OnTrayMenu(trayMenu) {
    global _PartyMarkers_LayerVisible
    itemLabel := "Party Markers — " (_PartyMarkers_LayerVisible ? "On" : "Off")
        . "`t" GetHotkeyDisplay("partyMarkersToggleLayer")
    trayMenu.Add(itemLabel, (*) => _PartyMarkers_ToggleLayer())
    if _PartyMarkers_LayerVisible {
        ; Native menus get the familiar checkmark as well as the explicit text;
        ; the enhanced tray still receives the On/Off label through its bridge.
        trayMenu.Check(itemLabel)
    }
}

_PartyMarkers_ToggleLayer() {
    global _PartyMarkers_LayerVisible
    _PartyMarkers_SetLayerVisible(!_PartyMarkers_LayerVisible)
    ; The overlay may well be closed when this is pressed, so say what happened.
    TrayTip(_PartyMarkers_LayerVisible ? "Layer on" : "Layer off", "Party Markers", "Iconi")
}

_PartyMarkers_SetLayerVisible(visible) {
    global _PartyMarkers_LayerVisible
    _PartyMarkers_LayerVisible := visible
    _PartyMarkers_SaveConfig()
    if visible {
        ; Redraw straight away from the latest poll instead of waiting a tick.
        _PartyMarkers_OnSnapshot(GetClientSnapshots())
    } else {
        _PartyMarkers_HideAll()
    }
    ; Rebuild after the current callback returns so the native tray's label and
    ; checkmark cannot remain stale. The enhanced tray also rebuilds on open.
    SetTimer(RebuildTrayMenu, -1)
}

_PartyMarkers_OnSettingsWeb() {
    global _PartyMarkers_LabelMode, _PartyMarkers_LayerVisible
    global MARKER_LABEL_MODES, MARKER_LABEL_MODE_LABELS

    return [
        Map("type", "info", "text",
            "Shows your other running clients on the minimap, each in its own colour.`n"
            . "Your own character keeps the standard marker."),
        Map("type", "checkbox", "id", "layerVisible", "label", "Show the party marker layer",
            "value", _PartyMarkers_LayerVisible ? true : false),
        Map("type", "dropdown", "id", "labelModeIdx", "label", "Show names:",
            "options", MARKER_LABEL_MODE_LABELS,
            "value", MarkerLabelModeIndex(_PartyMarkers_LabelMode) - 1)
    ]
}

_PartyMarkers_OnSettingsWebSave(values) {
    global MARKER_LABEL_MODES
    layerVis := values.Has("layerVisible") && values["layerVisible"] ? true : false
    modeIdx := values.Has("labelModeIdx") ? Integer(values["labelModeIdx"]) + 1 : 1
    if (modeIdx < 1 || modeIdx > MARKER_LABEL_MODES.Length)
        modeIdx := 1
    _PartyMarkers_ApplySettings(MARKER_LABEL_MODES[modeIdx], layerVis)
}

_PartyMarkers_ApplySettings(labelMode, layerVisible) {
    global _PartyMarkers_LabelMode, _PartyMarkers_Pool, _PartyMarkers_UsedCount
    _PartyMarkers_LabelMode := labelMode
    _PartyMarkers_SetLayerVisible(layerVisible)   ; saves the config too
    SetMarkerLabelsVisible(_PartyMarkers_Pool, _PartyMarkers_UsedCount,
        layerVisible && ShouldShowMarkerLabels(labelMode))
}

_PartyMarkers_LoadConfig() {
    global _PartyMarkers_LabelMode, _PartyMarkers_LayerVisible, CONFIG_INI
    _PartyMarkers_LayerVisible := (Trim(IniRead(CONFIG_INI, "PartyMarkers", "LayerVisible", "1")) != "0")
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
    global _PartyMarkers_LabelMode, _PartyMarkers_LayerVisible, CONFIG_INI
    IniWrite(_PartyMarkers_LabelMode, CONFIG_INI, "PartyMarkers", "LabelMode")
    IniWrite(_PartyMarkers_LayerVisible ? "1" : "0", CONFIG_INI, "PartyMarkers", "LayerVisible")
}

; Labels are positioned on every draw, so a hover change is a plain toggle.
_PartyMarkers_OnOverlayHover(isOver) {
    global _PartyMarkers_Pool, _PartyMarkers_UsedCount, _PartyMarkers_LabelMode
    global _PartyMarkers_LayerVisible
    SetMarkerLabelsVisible(_PartyMarkers_Pool, _PartyMarkers_UsedCount,
        _PartyMarkers_LayerVisible && ShouldShowMarkerLabels(_PartyMarkers_LabelMode))
}

; ── Drawing ──────────────────────────────────────────────────────

_PartyMarkers_OnSnapshot(snapshots) {
    global gOverlayVisible, gGui, gCurrentMapName, gCachedPID, GAME_STATE_WORLD
    global MINIMAP_MAP_INSET, _PartyMarkers_Pool, _PartyMarkers_LabelMode, _PartyMarkers_UsedCount
    global _PartyMarkers_LayerVisible

    if (!gOverlayVisible || !IsObject(gGui) || !gGui.Hwnd) {
        return
    }
    if !_PartyMarkers_LayerVisible {
        _PartyMarkers_HideAll()
        _PartyMarkers_UsedCount := 0
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
        ; Reapply and redraw the background just as the POI layer does.  These
        ; controls start life hidden; on some redraw paths Windows otherwise
        ; paints the label but leaves the newly-shown static's background
        ; transparent, making the party dot appear to be missing.
        entry.dot.Opt("+Background" _PartyMarkers_ColorFor(used))
        entry.dot.Move(px + MINIMAP_MAP_INSET, py + MINIMAP_MAP_INSET, size, size)
        entry.dot.Visible := true
        entry.dot.Redraw()

        ; Positioned even when hidden, so restoring names after hover is instant.
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
