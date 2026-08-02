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
; Whether the layer draws at all. Persisted, so a layer turned off stays off
; across a reload rather than quietly coming back.
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
    "OnSettingsWeb",     _Pois_OnSettingsWeb,
    "OnSettingsWebSave", _Pois_OnSettingsWebSave,
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
    trayMenu.Add("Map POIs", poiMenu)
}

_Pois_OnSettingsWeb() {
    global _Pois_LabelMode, _Pois_DefaultKind, _Pois_KINDS, _Pois_LayerVisible
    global MARKER_LABEL_MODES, MARKER_LABEL_MODE_LABELS

    return [
        Map("type", "info", "text",
            "Marks NPCs, shops, portals and notes on the minimap. Stand where the`n"
            . "thing is and press the Add POI hotkey — the position is read from the`n"
            . "game, so it lands exactly where you stood."),
        Map("type", "checkbox", "id", "layerVisible", "label", "Show the POI layer",
            "value", _Pois_LayerVisible ? true : false),
        Map("type", "dropdown", "id", "labelModeIdx", "label", "Show labels:",
            "options", MARKER_LABEL_MODE_LABELS,
            "value", MarkerLabelModeIndex(_Pois_LabelMode) - 1),
        Map("type", "dropdown", "id", "kindIdx", "label", "Default type:",
            "options", _Pois_KINDS,
            "value", _Pois_KindIndex(_Pois_DefaultKind) - 1),
        Map("type", "info", "text",
            "Stored in maps\pois.ini. Tray → Map POIs → Manage POIs… edits the`n"
            . "current map; Export writes them in the server repo's NPC entry format.")
    ]
}

_Pois_OnSettingsWebSave(values) {
    global MARKER_LABEL_MODES, _Pois_KINDS
    layerVis := values.Has("layerVisible") && values["layerVisible"] ? true : false
    modeIdx := values.Has("labelModeIdx") ? Integer(values["labelModeIdx"]) + 1 : 1
    if (modeIdx < 1 || modeIdx > MARKER_LABEL_MODES.Length)
        modeIdx := 1
    kindIdx := values.Has("kindIdx") ? Integer(values["kindIdx"]) + 1 : 1
    if (kindIdx < 1 || kindIdx > _Pois_KINDS.Length)
        kindIdx := 1
    _Pois_ApplySettings(MARKER_LABEL_MODES[modeIdx], _Pois_KINDS[kindIdx], layerVis)
}

_Pois_ApplySettings(labelMode, defaultKind, layerVisible) {
    global _Pois_LabelMode, _Pois_DefaultKind
    _Pois_LabelMode := labelMode
    _Pois_DefaultKind := defaultKind
    _Pois_SetLayerVisible(layerVisible)   ; saves the config and redraws
}

_Pois_LoadConfig() {
    global _Pois_LabelMode, _Pois_DefaultKind, _Pois_LayerVisible, CONFIG_INI
    _Pois_LayerVisible := (Trim(IniRead(CONFIG_INI, "MapPois", "LayerVisible", "1")) != "0")
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
    global _Pois_LabelMode, _Pois_DefaultKind, _Pois_LayerVisible, CONFIG_INI
    IniWrite(_Pois_LabelMode, CONFIG_INI, "MapPois", "LabelMode")
    IniWrite(_Pois_DefaultKind, CONFIG_INI, "MapPois", "DefaultKind")
    IniWrite(_Pois_LayerVisible ? "1" : "0", CONFIG_INI, "MapPois", "LayerVisible")
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

    ; Read the zone name once — the dialog blocks, and by the time it closes
    ; the player may have been moved by a portal.
    zone := ZoneDisplayName(mapId)

    entry := _Pois_PromptForEntry(zone, rawPos.x, rawPos.y)
    if !IsObject(entry) {
        return
    }
    ; Stored raw; only the display is converted to in-game coordinates.
    _Pois_Add(mapId, { x: rawPos.x, y: rawPos.y, label: entry.label, kind: entry.kind })
    _Pois_DefaultKind := entry.kind
    _Pois_SaveConfig()
    _Pois_Redraw()
    TrayTip("Map POIs", "Added '" entry.label "' (" entry.kind ")`n"
        . zone " at " GameCoordText(rawPos.x, rawPos.y), "Iconi")
}

; Label + type dialog. Returns {label, kind}, or 0 when cancelled.
; `zone` is the player-facing place name; x/y are raw and shown as in-game.
_Pois_PromptForEntry(zone, x, y) {
    global _Pois_KINDS, _Pois_DefaultKind

    result := 0
    dllDir := (A_PtrSize = 8) ? "64bit" : "32bit"
    dllPath := A_ScriptDir "\Lib\" dllDir "\WebView2Loader.dll"
    wvSettings := { DllPath: dllPath, DefaultWidth: 360, DefaultHeight: 330 }

    dlg := WebViewGui("-Caption +AlwaysOnTop -Resize", "Add POI",, wvSettings)

    kindsJson := "["
    for i, k in _Pois_KINDS {
        if (i > 1) kindsJson .= ","
        kindsJson .= _JSON_Str(k)
    }
    kindsJson .= "]"

    coordStr := GameCoordText(x, y)

    dlg.WebMessageReceived(OnWebMsg)
    dlg.DOMContentLoaded((*) => _SafePostMsg(dlg, '{"type":"init-add-poi"'
        . ',"zoneName":' _JSON_Str(zone)
        . ',"coordText":' _JSON_Str(coordStr)
        . ',"kinds":' kindsJson
        . ',"defaultKind":' _JSON_Str(_Pois_DefaultKind) '}'))

    dlg.Navigate("ui/map_pois/add_poi.html")
    dlg.Show("w360 h330")

    OnWebMsg(wv, args) {
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
                _SafePostMsg(dlg, '{"type":"init-add-poi"'
                    . ',"zoneName":' _JSON_Str(zone)
                    . ',"coordText":' _JSON_Str(coordStr)
                    . ',"kinds":' kindsJson
                    . ',"defaultKind":' _JSON_Str(_Pois_DefaultKind) '}')
            case "accept":
                if msg.Has("label") && msg.Has("kind") {
                    result := { label: msg["label"], kind: msg["kind"] }
                }
                dlg.Destroy()
            case "cancel":
                dlg.Destroy()
        }
    }

    WinWaitClose("ahk_id " dlg.Hwnd)
    return result
}

; ── Manage window ────────────────────────────────────────────────

_Pois_ShowManageWindow() {
    global _Pois_ManageGui, _Pois_KINDS, _Pois_DefaultKind

    mapId := _Pois_CurrentMapId()
    if (mapId = "") {
        TrayTip("Map POIs", "Enter a map first — POIs are stored per map.", "Iconx")
        return
    }
    if IsObject(_Pois_ManageGui) {
        try _Pois_ManageGui.Destroy()
        _Pois_ManageGui := 0
    }

    dllDir := (A_PtrSize = 8) ? "64bit" : "32bit"
    dllPath := A_ScriptDir "\Lib\" dllDir "\WebView2Loader.dll"
    wvSettings := { DllPath: dllPath, DefaultWidth: 500, DefaultHeight: 460 }

    g := WebViewGui("-Caption +AlwaysOnTop -Resize", "Map POIs — " ZoneDisplayName(mapId),, wvSettings)
    _Pois_ManageGui := g

    g.OnEvent("Close", (*) => (_Pois_ManageGui := 0))
    g.WebMessageReceived(_Pois_OnManageWebMsg)
    g.DOMContentLoaded((*) => SetTimer(_Pois_SendManageState, -50))
    g.Navigate("ui/map_pois/index.html")

    g.Show("w500 h460")
}

_Pois_SendManageState() {
    global _Pois_ManageGui, _Pois_KINDS, _Pois_DefaultKind
    if !IsObject(_Pois_ManageGui)
        return

    mapId := _Pois_CurrentMapId()
    zoneName := ZoneDisplayName(mapId)
    pois := _Pois_Load(mapId)

    poisJson := "["
    first := true
    for i, poi in pois {
        if !first
            poisJson .= ","
        first := false
        gx := (IsObject(poi) && poi.HasOwnProp("x") && IsNumber(poi.x)) ? GameCoordX(poi.x) : 0
        gy := (IsObject(poi) && poi.HasOwnProp("y") && IsNumber(poi.y)) ? GameCoordY(poi.y) : 0
        labelStr := (IsObject(poi) && poi.HasOwnProp("label")) ? poi.label : ""
        kindStr := (IsObject(poi) && poi.HasOwnProp("kind")) ? poi.kind : "npc"
        poisJson .= '{"index":' i
            . ',"label":' _JSON_Str(labelStr)
            . ',"kind":' _JSON_Str(kindStr)
            . ',"gameX":' gx
            . ',"gameY":' gy '}'
    }
    poisJson .= "]"

    kindsJson := "["
    for i, k in _Pois_KINDS {
        if (i > 1)
            kindsJson .= ","
        kindsJson .= _JSON_Str(k)
    }
    kindsJson .= "]"

    _SafePostMsg(_Pois_ManageGui, '{"type":"pois-state"'
        . ',"zoneName":' _JSON_Str(zoneName)
        . ',"pois":' poisJson
        . ',"kinds":' kindsJson '}')
}

_Pois_OnManageWebMsg(wv, args) {
    global _Pois_ManageGui, _Pois_KINDS, _Pois_DefaultKind
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

    mapId := _Pois_CurrentMapId()

    switch msg["type"] {
        case "init-request":
            _Pois_SendManageState()
        case "delete-poi":
            if msg.Has("index") && msg["index"] >= 1 {
                _Pois_DeleteAt(mapId, msg["index"])
                _Pois_SendManageState()
                _Pois_Redraw()
            }
        case "export-map":
            _Pois_ExportCurrentMap()
        case "request-add-poi":
            _Pois_HandleManageAddPoi(mapId)
        case "submit-add-poi":
            if msg.Has("label") && msg.Has("kind") {
                _Pois_HandleManageSubmitPoi(mapId, msg["label"], msg["kind"])
            }
        case "close":
            if IsObject(_Pois_ManageGui) {
                try _Pois_ManageGui.Destroy()
            }
            _Pois_ManageGui := 0
    }
}

_Pois_HandleManageAddPoi(mapId) {
    global _Pois_ManageGui, _Pois_DefaultKind
    if !IsObject(_Pois_ManageGui)
        return

    rawPos := ReadRawPlayerPosition()
    gx := (IsObject(rawPos) && rawPos.HasOwnProp("ok") && rawPos.ok && IsNumber(rawPos.x)) ? GameCoordX(rawPos.x) : 0
    gy := (IsObject(rawPos) && rawPos.HasOwnProp("ok") && rawPos.ok && IsNumber(rawPos.y)) ? GameCoordY(rawPos.y) : 0
    zoneName := ZoneDisplayName(mapId)

    _SafePostMsg(_Pois_ManageGui, '{"type":"prompt-add-poi"'
        . ',"zoneName":' _JSON_Str(zoneName)
        . ',"gameX":' gx
        . ',"gameY":' gy
        . ',"defaultKind":' _JSON_Str(_Pois_DefaultKind) '}')
}

_SafePostMsg(gui, json) {
    if !IsObject(gui)
        return
    try {
        gui.PostWebMessageAsJson(json)
    } catch {
        try gui.PostWebMessageAsString(json)
    }
}

_Pois_HandleManageSubmitPoi(mapId, label, kind) {
    global _Pois_DefaultKind
    rawPos := ReadRawPlayerPosition()
    if !rawPos.ok {
        TrayTip("Map POIs", "Could not read position — is the game client running?", "Iconx")
        return
    }
    _Pois_Add(mapId, { x: rawPos.x, y: rawPos.y, label: label, kind: kind })
    _Pois_DefaultKind := kind
    _Pois_SaveConfig()
    _Pois_SendManageState()
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
        TrayTip("Map POIs", "No POIs saved for " ZoneDisplayName(mapId) " yet.", "Iconi")
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
    ; The map id is named here on purpose — it's what the exported entries use.
    TrayTip("Map POIs", "Exported " list.Length " POI(s) for " ZoneDisplayName(mapId) " (" mapId ")`n"
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
    _Pois_SetLayerVisible(!_Pois_LayerVisible)
    ; The overlay may well be closed when this is pressed, so say what happened.
    TrayTip("Map POIs", _Pois_LayerVisible ? "Layer on" : "Layer off", "Iconi")
}

_Pois_SetLayerVisible(visible) {
    global _Pois_LayerVisible
    _Pois_LayerVisible := visible
    _Pois_SaveConfig()
    if visible {
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
