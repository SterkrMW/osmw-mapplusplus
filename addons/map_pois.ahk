#Requires AutoHotkey v2.0

; Points of interest on the minimap — NPCs, shops, portals, quest spots, notes.
;
; Authoring works the way the original NPC generator did: stand where the thing
; is, press a key, and the live memory position is captured. That position is
; exact, so no click-to-place inverse math (and no fighting the overlay's
; drag-to-move) is involved. The same data can be exported in the server repo's
; NPC entry format, which is what that generator was written for — Export writes
; every POI on the map instead of one position at a time.
;
; Storage: maps\pois.ini, one section per map id, alongside calibration.ini.
;   [MAP302]
;   1=1234|5678|Grocery|shop

global _Pois_KINDS := ["npc", "shop", "portal", "quest", "note"]
; Marker colour per kind, and the label colour that goes with it.
global _Pois_COLORS := Map(
    "npc",    "FFD700",
    "shop",   "00CED1",
    "portal", "9370DB",
    "quest",  "32CD32",
    "note",   "FFFFFF"
)
; "autohide" (default), "always" or "never" — see MARKER_LABEL_MODES.
global _Pois_LabelMode := "autohide"
global _Pois_LayerVisible := true
global _Pois_DefaultKind := "npc"
; Slots used by the last draw, so a hover change can toggle just those labels.
global _Pois_UsedCount := 0

; mapId → array of {x, y, label, kind}; dropped whenever the file is written.
global _Pois_Cache := Map()
; Control pool on the overlay Gui, same approach as party_markers.
global _Pois_Pool := []
global _Pois_GuiHwnd := 0
global _Pois_ManageGui := 0

RegisterAddon(Map(
    "name",             "MapPois",
    "settingsLabel",    "Map POIs",
    "OnInit",           _Pois_OnInit,
    "OnSettings",       _Pois_OnSettings,
    "OnTrayMenu",       _Pois_OnTrayMenu,
    "OnOverlayShow",    _Pois_OnOverlayShow,
    "OnOverlayHover",   _Pois_OnOverlayHover,
    "OnOverlayHide",    _Pois_HideAll,
    "OnOverlayRebuild", _Pois_Reset,
    "OnMapChange",      _Pois_OnMapChange
))

RegisterHotkeyAction(Map(
    "id", "poiAddHere",
    "label", "Add POI at my position",
    "category", "Map POIs",
    "default", "^!p",
    "addon", "MapPois",
    "handler", (*) => _Pois_AddHere()
))
RegisterHotkeyAction(Map(
    "id", "poiToggleLayer",
    "label", "Show/hide POI layer",
    "category", "Map POIs",
    "default", "^!o",
    "addon", "MapPois",
    "handler", (*) => _Pois_ToggleLayer()
))
RegisterHotkeyAction(Map(
    "id", "poiGenerateNpc",
    "label", "Generate NPC entry at my position",
    "category", "Map POIs",
    "default", "^!n",
    "addon", "MapPois",
    "handler", (*) => GenerateNpcEntry()
))

_Pois_OnInit() {
    _Pois_LoadConfig()
}

_Pois_OnTrayMenu(trayMenu) {
    poiMenu := Menu()
    poiMenu.Add("Add POI Here`t" GetHotkeyDisplay("poiAddHere"), (*) => _Pois_AddHere())
    poiMenu.Add("Show/Hide Layer`t" GetHotkeyDisplay("poiToggleLayer"), (*) => _Pois_ToggleLayer())
    poiMenu.Add("Manage POIs…", (*) => _Pois_ShowManageWindow())
    poiMenu.Add()
    poiMenu.Add("Export This Map's POIs", (*) => _Pois_ExportCurrentMap())
    poiMenu.Add("Generate NPC Entry`t" GetHotkeyDisplay("poiGenerateNpc"), (*) => GenerateNpcEntry())
    trayMenu.Add("Map POIs", poiMenu)
}

_Pois_OnSettings(ctx) {
    global _Pois_LabelMode, _Pois_DefaultKind, _Pois_KINDS
    global MARKER_LABEL_MODES, MARKER_LABEL_MODE_LABELS

    g := ctx.gui
    g.Add("Text", "xs y+16 w430",
        "Marks NPCs, shops, portals and notes on the minimap. Stand where the`n"
        . "thing is and press the Add POI hotkey — the position is read from the`n"
        . "game, so it lands exactly where you stood.")

    g.Add("Text", "xs y+14 w130", "Show labels:")
    modeDdl := g.Add("DropDownList", "x+10 yp-3 w250", MARKER_LABEL_MODE_LABELS)
    modeDdl.Value := MarkerLabelModeIndex(_Pois_LabelMode)

    g.Add("Text", "xs y+16 w130", "Default type:")
    kindDdl := g.Add("DropDownList", "x+10 yp-3 w160", _Pois_KINDS)
    kindDdl.Value := _Pois_KindIndex(_Pois_DefaultKind)

    g.Add("Text", "xs y+18 w430 c666666",
        "Stored in maps\pois.ini. Tray → Map POIs → Manage POIs… edits the`n"
        . "current map; Export writes them in the server repo's NPC entry format.")

    ctx.saveHandlers.Push(() => _Pois_ApplySettings(MARKER_LABEL_MODES[modeDdl.Value],
        _Pois_KINDS[kindDdl.Value]))
}

_Pois_ApplySettings(labelMode, defaultKind) {
    global _Pois_LabelMode, _Pois_DefaultKind
    _Pois_LabelMode := labelMode
    _Pois_DefaultKind := defaultKind
    _Pois_SaveConfig()
    _Pois_Redraw()
}

_Pois_LoadConfig() {
    global _Pois_LabelMode, _Pois_DefaultKind, CONFIG_INI
    mode := Trim(IniRead(CONFIG_INI, "MapPois", "LabelMode", ""))
    if (mode != "") {
        _Pois_LabelMode := NormalizeMarkerLabelMode(mode)
    } else {
        ; Migrate the older on/off setting: off stays off, on becomes autohide.
        _Pois_LabelMode := (Trim(IniRead(CONFIG_INI, "MapPois", "ShowLabels", "1")) = "0")
            ? "never" : "autohide"
    }
    kind := Trim(IniRead(CONFIG_INI, "MapPois", "DefaultKind", "npc"))
    if _Pois_IsKind(kind) {
        _Pois_DefaultKind := kind
    }
}

_Pois_SaveConfig() {
    global _Pois_LabelMode, _Pois_DefaultKind, CONFIG_INI
    IniWrite(_Pois_LabelMode, CONFIG_INI, "MapPois", "LabelMode")
    IniWrite(_Pois_DefaultKind, CONFIG_INI, "MapPois", "DefaultKind")
}

; Labels are positioned on every draw, so a hover change is a plain toggle.
_Pois_OnOverlayHover(isOver) {
    global _Pois_Pool, _Pois_UsedCount, _Pois_LabelMode, _Pois_LayerVisible
    SetMarkerLabelsVisible(_Pois_Pool, _Pois_UsedCount,
        _Pois_LayerVisible && ShouldShowMarkerLabels(_Pois_LabelMode))
}

_Pois_IsKind(kind) {
    global _Pois_KINDS
    for k in _Pois_KINDS {
        if (k = kind)
            return true
    }
    return false
}

_Pois_KindIndex(kind) {
    global _Pois_KINDS
    for i, k in _Pois_KINDS {
        if (k = kind)
            return i
    }
    return 1
}

_Pois_ColorFor(kind) {
    global _Pois_COLORS
    return _Pois_COLORS.Has(kind) ? _Pois_COLORS[kind] : "FFFFFF"
}

; ── Store ────────────────────────────────────────────────────────

_Pois_Path() {
    global MAP_DIR
    return MAP_DIR "\pois.ini"
}

; POIs for a map id ("MAP302"), newest last. Cached until the file is written.
_Pois_Load(mapId) {
    global _Pois_Cache
    if (mapId = "") {
        return []
    }
    if _Pois_Cache.Has(mapId) {
        return _Pois_Cache[mapId]
    }
    list := []
    path := _Pois_Path()
    if FileExist(path) {
        try section := IniRead(path, mapId)
        catch
            section := ""
        loop parse, section, "`n", "`r" {
            line := Trim(A_LoopField)
            if (line = "" || !InStr(line, "=")) {
                continue
            }
            value := SubStr(line, InStr(line, "=") + 1)
            ; x|y|label|kind — "|" so labels may contain commas.
            parts := StrSplit(value, "|")
            if (parts.Length < 3) {
                continue
            }
            x := Trim(parts[1])
            y := Trim(parts[2])
            if (!IsInteger(x) || !IsInteger(y)) {
                continue
            }
            kind := (parts.Length >= 4) ? Trim(parts[4]) : "npc"
            list.Push({
                x: Integer(x),
                y: Integer(y),
                label: Trim(parts[3]),
                kind: _Pois_IsKind(kind) ? kind : "npc"
            })
        }
    }
    _Pois_Cache[mapId] := list
    return list
}

; Rewrites one map's section from `list`, then drops the cache entry.
_Pois_Save(mapId, list) {
    global _Pois_Cache
    path := _Pois_Path()
    try IniDelete(path, mapId)
    for i, poi in list {
        IniWrite(poi.x "|" poi.y "|" poi.label "|" poi.kind, path, mapId, String(i))
    }
    _Pois_Cache.Delete(mapId)
}

_Pois_Add(mapId, poi) {
    list := _Pois_Load(mapId).Clone()
    list.Push(poi)
    _Pois_Save(mapId, list)
}

_Pois_DeleteAt(mapId, index) {
    list := _Pois_Load(mapId).Clone()
    if (index < 1 || index > list.Length) {
        return
    }
    list.RemoveAt(index)
    _Pois_Save(mapId, list)
}

; ── Capture ──────────────────────────────────────────────────────

_Pois_AddHere() {
    global _Pois_DefaultKind

    rawPos := ReadRawPlayerPosition()
    if !rawPos.ok {
        TrayTip("Map POIs", "Could not read your position — is the game running?", "Iconx")
        return
    }
    mapId := ReadCurrentMapBaseName()
    if (mapId = "") {
        TrayTip("Map POIs", "Could not read the current map from memory.", "Iconx")
        return
    }

    entry := _Pois_PromptForEntry(mapId, rawPos.x, rawPos.y)
    if !IsObject(entry) {
        return
    }
    _Pois_Add(mapId, { x: rawPos.x, y: rawPos.y, label: entry.label, kind: entry.kind })
    _Pois_DefaultKind := entry.kind
    _Pois_SaveConfig()
    _Pois_Redraw()
    TrayTip("Map POIs", "Added '" entry.label "' (" entry.kind ")`n" mapId " at " rawPos.x ", " rawPos.y, "Iconi")
}

; Label + type dialog. Returns {label, kind}, or 0 when cancelled.
_Pois_PromptForEntry(mapId, x, y) {
    global _Pois_KINDS, _Pois_DefaultKind

    result := 0
    dlg := Gui("+AlwaysOnTop -MinimizeBox", "Add POI")
    dlg.Add("Text", "w300", mapId " at " x ", " y)
    dlg.Add("Text", "xm y+10 w60", "Label:")
    labelEdit := dlg.Add("Edit", "x+8 yp-3 w232")
    dlg.Add("Text", "xm y+10 w60", "Type:")
    kindDdl := dlg.Add("DropDownList", "x+8 yp-3 w120", _Pois_KINDS)
    kindDdl.Value := _Pois_KindIndex(_Pois_DefaultKind)

    Accept(*) {
        label := Trim(labelEdit.Value)
        if (label = "") {
            TrayTip("Map POIs", "Give the POI a label.", "Iconx")
            return
        }
        result := { label: label, kind: _Pois_KINDS[kindDdl.Value] }
        dlg.Destroy()
    }
    dlg.Add("Button", "xm y+14 w90 Default", "Add").OnEvent("Click", Accept)
    dlg.Add("Button", "x+8 w90", "Cancel").OnEvent("Click", (*) => dlg.Destroy())
    dlg.OnEvent("Close", (*) => dlg.Destroy())
    dlg.OnEvent("Escape", (*) => dlg.Destroy())
    dlg.Show("AutoSize")
    try labelEdit.Focus()
    WinWaitClose("ahk_id " dlg.Hwnd)
    return result
}

; ── Manage window ────────────────────────────────────────────────

_Pois_ShowManageWindow() {
    global _Pois_ManageGui

    mapId := _Pois_CurrentMapId()
    if (mapId = "") {
        TrayTip("Map POIs", "Enter a map first — POIs are stored per map.", "Iconx")
        return
    }
    if IsObject(_Pois_ManageGui) {
        try _Pois_ManageGui.Destroy()
        _Pois_ManageGui := 0
    }

    g := Gui("+AlwaysOnTop -MinimizeBox", "Map POIs — " mapId)
    _Pois_ManageGui := g
    lv := g.Add("ListView", "w420 r10 -Multi Grid", ["Label", "Type", "X", "Y"])
    _Pois_FillManageList(lv, mapId)

    deleteBtn := g.Add("Button", "xm y+10 w110", "Delete selected")
    deleteBtn.OnEvent("Click", (*) => _Pois_DeleteSelected(lv, mapId))
    g.Add("Button", "x+8 w110", "Export map").OnEvent("Click", (*) => _Pois_ExportCurrentMap())
    g.Add("Button", "x+8 w90", "Close").OnEvent("Click", (*) => g.Destroy())
    g.OnEvent("Close", (*) => (_Pois_ManageGui := 0))
    g.OnEvent("Escape", (*) => g.Destroy())
    g.Show("AutoSize")
}

_Pois_FillManageList(lv, mapId) {
    lv.Delete()
    for poi in _Pois_Load(mapId) {
        lv.Add(, poi.label, poi.kind, poi.x, poi.y)
    }
    loop 4 {
        lv.ModifyCol(A_Index, "AutoHdr")
    }
}

_Pois_DeleteSelected(lv, mapId) {
    row := lv.GetNext()
    if (row < 1) {
        TrayTip("Map POIs", "Select a POI to delete.", "Iconx")
        return
    }
    ; Row order matches _Pois_Load order, so the row index is the store index.
    _Pois_DeleteAt(mapId, row)
    _Pois_FillManageList(lv, mapId)
    _Pois_Redraw()
}

; ── Export ───────────────────────────────────────────────────────

; Writes every POI on the current map in the server repo's NPC entry format —
; the bulk version of what GenerateNpcEntry does for a single position.
_Pois_ExportCurrentMap() {
    global gNpcNextId, NPC_OUTPUT_FILE

    mapId := _Pois_CurrentMapId()
    if (mapId = "") {
        TrayTip("Map POIs", "Enter a map first.", "Iconx")
        return
    }
    list := _Pois_Load(mapId)
    if (list.Length = 0) {
        TrayTip("Map POIs", "No POIs saved for " mapId " yet.", "Iconi")
        return
    }

    text := ""
    for poi in list {
        text .= BuildNpcEntryText(gNpcNextId, poi.label, mapId, poi.x, poi.y)
        gNpcNextId += 1
    }
    try {
        FileAppend(text, NPC_OUTPUT_FILE, "UTF-8")
    } catch as err {
        TrayTip("Map POIs", "Export failed: " err.Message, "Iconx")
        return
    }
    SaveNpcNextId()
    A_Clipboard := text
    TrayTip("Map POIs", "Exported " list.Length " POI(s) for " mapId "`n"
        . NPC_OUTPUT_FILE "`n(also copied to the clipboard)", "Iconi")
}

; ── Drawing ──────────────────────────────────────────────────────

_Pois_CurrentMapId() {
    global gCurrentMapName, gResolvedMapName
    if (gCurrentMapName != "") {
        return MapIdFromName(gCurrentMapName)
    }
    if (gResolvedMapName != "") {
        return MapIdFromName(gResolvedMapName)
    }
    return ReadCurrentMapBaseName()
}

_Pois_ToggleLayer() {
    global _Pois_LayerVisible
    _Pois_LayerVisible := !_Pois_LayerVisible
    if _Pois_LayerVisible {
        _Pois_Redraw()
    } else {
        _Pois_HideAll()
    }
}

_Pois_OnOverlayShow(mapName) {
    _Pois_Redraw()
}

_Pois_OnMapChange(mapName, prev) {
    ; The overlay hot-swaps its image on a zone change; the POIs must follow.
    _Pois_Redraw()
}

_Pois_Redraw(*) {
    global gOverlayVisible, gGui, gCurrentMapName, MINIMAP_MAP_INSET
    global _Pois_LayerVisible, _Pois_LabelMode, _Pois_Pool, _Pois_UsedCount

    if (!gOverlayVisible || !IsObject(gGui) || !gGui.Hwnd) {
        return
    }
    if !_Pois_LayerVisible {
        _Pois_HideAll()
        return
    }
    list := _Pois_Load(MapIdFromName(gCurrentMapName))
    _Pois_EnsurePool(list.Length)

    scale := MinimapScaleFactor()
    ; A little smaller than the player marker so it never competes with it.
    size := Max(3, Round(MinimapMarkerSize() * 0.7))
    showLabels := ShouldShowMarkerLabels(_Pois_LabelMode)
    used := 0
    for poi in list {
        if (used >= _Pois_Pool.Length) {
            break
        }
        pos := WorldToOverlayPixels(poi.x, poi.y, gCurrentMapName)
        px := Clamp(Round(pos.x * scale), 0, MinimapDisplayW() - size)
        py := Clamp(Round(pos.y * scale), 0, MinimapDisplayH() - size)

        used++
        entry := _Pois_Pool[used]
        ; A pool slot can be reused for a different kind, so the colour is set
        ; per draw; Redraw() because a colour change alone doesn't repaint.
        entry.dot.Opt("+Background" _Pois_ColorFor(poi.kind))
        entry.dot.Move(px + MINIMAP_MAP_INSET, py + MINIMAP_MAP_INSET, size, size)
        entry.dot.Visible := true
        entry.dot.Redraw()

        ; The dot carries the type colour; the label stays white-on-black so it
        ; reads against any map art. Positioned even when hidden, so showing
        ; them on hover is instant.
        PositionMarkerLabel(entry.label, poi.label,
            px + MINIMAP_MAP_INSET, py + MINIMAP_MAP_INSET, size,
            showLabels && poi.label != "")
    }
    _Pois_UsedCount := used

    loop _Pois_Pool.Length {
        if (A_Index <= used) {
            continue
        }
        _Pois_Pool[A_Index].dot.Visible := false
        _Pois_Pool[A_Index].label.Visible := false
    }
}

; Creates (or recreates) the control pool on the current overlay Gui. Same
; hwnd check as party_markers: if the overlay was rebuilt, so is the pool.
_Pois_EnsurePool(needed) {
    global gGui, _Pois_Pool, _Pois_GuiHwnd

    if (_Pois_GuiHwnd = gGui.Hwnd && _Pois_Pool.Length >= needed) {
        return
    }
    if (_Pois_GuiHwnd != gGui.Hwnd) {
        _Pois_Pool := []
        _Pois_GuiHwnd := gGui.Hwnd
    }
    while (_Pois_Pool.Length < needed) {
        dot := gGui.AddText("x0 y0 w1 h1 Hidden BackgroundFFD700", "")
        ; Themed statics ignore the background colour.
        DllCall("uxtheme\SetWindowTheme", "Ptr", dot.Hwnd, "WStr", "", "WStr", "")
        _Pois_Pool.Push({ dot: dot, label: AddMarkerLabelControl(gGui) })
    }
}

_Pois_HideAll(*) {
    global _Pois_Pool
    for entry in _Pois_Pool {
        try {
            entry.dot.Visible := false
            entry.label.Visible := false
        }
    }
}

; The overlay Gui was destroyed — drop the now-dangling control references.
_Pois_Reset(*) {
    global _Pois_Pool, _Pois_GuiHwnd
    _Pois_Pool := []
    _Pois_GuiHwnd := 0
}
