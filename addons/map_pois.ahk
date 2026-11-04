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
; Ceiling on how many POIs are drawn at once. The pool is two controls per POI
; on the always-on-top overlay Gui and only ever grew, so a densely annotated
; map put hundreds of statics on it. Well above any hand-authored map; the
; overflow is reported once rather than silently dropped.
global POI_MAX_DRAWN := 120
global _Pois_OverflowNotified := ""
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
    "settingsGroup", "map",
    "settingsIcon",  "place",
    "description",   "Mark NPCs, shops and portals on the Maps++ minimap, and share them.",
    "settingsLabel",    "Map POIs",
    "OnInit",           _Pois_OnInit,
    "OnSettingsWeb",     _Pois_OnSettingsWeb,
    "OnSettingsWebSave", _Pois_OnSettingsWebSave,
    "OnSettingsAction",  _Pois_OnSettingsAction,
    "OnTrayMenu",       _Pois_OnTrayMenu,
    "OnOverlayShow",    _Pois_OnOverlayShow,
    "OnOverlayHover",   _Pois_OnOverlayHover,
    "OnOverlayHide",    _Pois_HideAll,
    "OnOverlayRebuild", _Pois_Reset,
    "OnMapChange",      _Pois_OnMapChange
))

RegisterHotkeyAction(Map(
    "id", "poiAddHere",
    "label", "Add POI here",
    "category", "Minimap",
    "default", "",
    "addon", "MapPois",
    "handler", (*) => _Pois_AddHere()
))
RegisterHotkeyAction(Map(
    "id", "poiToggleLayer",
    "label", "Show/hide POIs",
    "category", "Minimap",
    "default", "",
    "addon", "MapPois",
    "handler", (*) => _Pois_ToggleLayer()
))
RegisterHotkeyAction(Map(
    "id", "poiGenerateNpc",
    "label", "Write an NPC entry for here (server format)",
    "category", "Developer",
    "default", "",
    "addon", "MapPois",
    "handler", (*) => GenerateNpcEntry()
))

_Pois_OnInit() {
    _Pois_LoadConfig()
}

_Pois_OnTrayMenu(trayGroups) {
    global _Pois_LayerVisible
    minimapGroup := trayGroups["minimap"]
    minimapGroup.Add("Show POIs", (*) => _Pois_ToggleLayer(),
        Map("section", "pois", "order", 10, "hotkey", "poiToggleLayer",
            "toggle", _Pois_LayerVisible ? true : false))
    minimapGroup.Add("Add POI here", (*) => _Pois_AddHere(),
        Map("section", "pois", "order", 20, "hotkey", "poiAddHere"))
    minimapGroup.Add("Manage POIs…", (*) => _Pois_ShowManageWindow(),
        Map("section", "pois", "order", 30))
    ; Sharing with other players and with the game's own minimap works on every
    ; map at once, so it is on Settings → Map POIs rather than here. This one
    ; writes the server repo's NPC format: a contributor's tool.
    trayGroups["developer"].Add("Export this map's POIs (server format)",
        (*) => _Pois_ExportCurrentMap(), Map("section", "tools"))
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
            "value", _Pois_LayerVisible ? true : false,
            "default", (DefaultRead("MapPois", "LayerVisible", "1") != "0")),
        ; LabelMode is not in defaults.ini — it doubles as a migration sentinel
        ; (see the design doc), so its default is this addon's own compiled
        ; initial value ("autohide"), not a DefaultRead lookup.
        Map("type", "dropdown", "id", "labelModeIdx", "label", "Show labels:",
            "options", MARKER_LABEL_MODE_LABELS,
            "value", MarkerLabelModeIndex(_Pois_LabelMode) - 1,
            "default", MarkerLabelModeIndex("autohide") - 1),
        Map("type", "dropdown", "id", "kindIdx", "label", "Default type:",
            "options", _Pois_KINDS,
            "value", _Pois_KindIndex(_Pois_DefaultKind) - 1,
            "default", _Pois_KindIndex(DefaultRead("MapPois", "DefaultKind", "npc")) - 1),
        Map("type", "info", "text",
            "Stored in maps\pois.ini. Tray → Minimap → Manage POIs… edits the`n"
            . "current map's POIs."),
        ; Sharing works on every map at once, so it lives here rather than in
        ; Manage POIs, which only opens on the map the player is standing on.
        ; These act immediately; they do not wait for Save.
        Map("type", "info", "text",
            "Sharing — these act right away, on every map's POIs:"),
        Map("type", "button", "id", "exportPack", "label", "Export POI pack…",
            "hint", "Save your POIs to a file another player can import."),
        Map("type", "button", "id", "importPack", "label", "Import POI pack…",
            "hint", "Add POIs from a pack file. Existing POIs are kept."),
        Map("type", "button", "id", "exportGameMarkers", "label", "Export to game markers…",
            "hint", "Show your POIs on the game's own minimap (needs the client mods)."),
        Map("type", "button", "id", "removeGameMarkers", "label", "Remove exported game markers…",
            "hint", "Take back only the markers Maps++ wrote.")
    ]
}

; The sharing buttons above. settings.ahk runs this outside the WebView message
; callback, so each may raise its own dialogs and file pickers.
_Pois_OnSettingsAction(actionId) {
    switch actionId {
        case "exportPack":        _Pois_ExportPack()
        case "importPack":        _Pois_ImportPack()
        case "exportGameMarkers": _Pois_ExportGameMarkers()
        case "removeGameMarkers": _Pois_RemoveGameMarkers()
    }
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
    _Pois_LayerVisible := (Trim(ConfigRead("MapPois", "LayerVisible", "1")) != "0")
    mode := Trim(IniRead(CONFIG_INI, "MapPois", "LabelMode", ""))
    if (mode != "") {
        _Pois_LabelMode := NormalizeMarkerLabelMode(mode)
    } else {
        ; Migrate the older on/off setting: off stays off, on becomes autohide.
        _Pois_LabelMode := (Trim(IniRead(CONFIG_INI, "MapPois", "ShowLabels", "1")) = "0")
            ? "never" : "autohide"
    }
    kind := Trim(ConfigRead("MapPois", "DefaultKind", "npc"))
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

; One record — "x|y|label|kind" — as a POI object, or 0 when it is not usable.
; Shared by the live store and the pack reader so the two cannot disagree about
; what a record means.
;
; Records we write always have exactly four fields, because labels are stripped
; of "|" on the way in. A hand-edited or third-party file may not: the rule for
; extra fields is that a trailing recognised kind wins, and everything between
; the coordinates and it belongs to the label. Without that, "Grocer | Baker"
; silently loses half its name and takes "Baker" as its type.
_Pois_ParseRecord(value) {
    parts := StrSplit(value, "|")
    if (parts.Length < 3)
        return 0
    x := Trim(parts[1]), y := Trim(parts[2])
    if (!IsInteger(x) || !IsInteger(y))
        return 0

    kind := "npc"
    last := parts.Length
    if (parts.Length >= 4 && _Pois_IsKind(Trim(parts[parts.Length]))) {
        kind := Trim(parts[parts.Length])
        last := parts.Length - 1
    }
    label := ""
    loop last - 2
        label .= (label = "" ? "" : " ") parts[A_Index + 2]

    return { x: Integer(x), y: Integer(y), label: Trim(label), kind: kind }
}

; "|" is the field delimiter in a POI record, so a label containing one would
; push `kind` out of parts[4]; a newline would split the ini line and the whole
; POI would be dropped on the next load. Stripped on the way in rather than
; escaped on every write, the same trade the layout and preset stores make.
_Pois_SanitizeLabel(label) {
    s := String(label)
    for ch in ["|", "`r", "`n", "`t"]
        s := StrReplace(s, ch, " ")
    while InStr(s, "  ")
        s := StrReplace(s, "  ", " ")
    s := Trim(s)
    return (StrLen(s) > 48) ? Trim(SubStr(s, 1, 48)) : s
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
            poi := _Pois_ParseRecord(value)
            if IsObject(poi)
                list.Push(poi)
        }
    }
    _Pois_Cache[mapId] := list
    return list
}

; Rewrites one map's section from `list`, then drops the cache entry.
_Pois_Save(mapId, list) {
    global _Pois_Cache
    path := _Pois_Path()
    ; Before the IniDelete: on a fresh install that delete is what creates the
    ; file, and it creates an ANSI one that silently mangles non-ASCII labels.
    EnsureIniUtf16(path)
    try IniDelete(path, mapId)
    for i, poi in list {
        IniWrite(poi.x "|" poi.y "|" poi.label "|" poi.kind, path, mapId, String(i))
    }
    _Pois_Cache.Delete(mapId)
}

; The one place a POI enters the store, so label and kind are made safe here
; rather than at each frontend. Returns false when nothing usable was supplied.
_Pois_Add(mapId, poi) {
    poi.label := _Pois_SanitizeLabel(poi.label)
    if (poi.label = "")
        return false
    if !_Pois_IsKind(poi.kind)
        poi.kind := "npc"
    list := _Pois_Load(mapId).Clone()
    list.Push(poi)
    _Pois_Save(mapId, list)
    return true
}

_Pois_DeleteAt(mapId, index) {
    list := _Pois_Load(mapId).Clone()
    if (index < 1 || index > list.Length) {
        return
    }
    list.RemoveAt(index)
    _Pois_Save(mapId, list)
}

; Every other panel in the app gates its WebView2 frontend on the files it needs
; actually being present (_ShopPrices_CanUseWebView, _BH_CanUseWebView,
; _WindowLayout_CanUseWebView, _Settings_CanUseWebView) and falls back to the
; native one. These two dialogs were the exception; both have a complete native
; path, so there was no reason for them not to use it.
_Pois_CanUseWebView(page) {
    return FileExist(A_ScriptDir "\Lib\WebViewToo.ahk")
        && FileExist(A_ScriptDir "\ui\map_pois\" page)
}

; ── Capture ──────────────────────────────────────────────────────

_Pois_AddHere() {
    global _Pois_DefaultKind

    rawPos := ReadRawPlayerPosition()
    if !rawPos.ok {
        TrayTip("Could not read your position — is the game running?", "Map POIs", "Iconx")
        return
    }
    mapId := ReadCurrentMapBaseName()
    if (mapId = "") {
        TrayTip("Could not read the current map from memory.", "Map POIs", "Iconx")
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
    if !_Pois_Add(mapId, { x: rawPos.x, y: rawPos.y, label: entry.label, kind: entry.kind }) {
        TrayTip("That label came through empty — the POI wasn't saved.", "Map POIs", "Iconx")
        return
    }
    _Pois_DefaultKind := entry.kind
    _Pois_SaveConfig()
    _Pois_Redraw()
    TrayTip("Added '" _Pois_SanitizeLabel(entry.label) "' (" entry.kind ")`n"
        . zone " at " GameCoordText(rawPos.x, rawPos.y), "Map POIs", "Iconi")
}

; Label + type dialog. Returns {label, kind}, or 0 when cancelled.
; `zone` is the player-facing place name; x/y are raw and shown as in-game.
_Pois_PromptForEntry(zone, x, y) {
    global _Pois_KINDS, _Pois_DefaultKind

    if (IsNativeInterface() || !_Pois_CanUseWebView("add_poi.html")) {
        result := 0
        dlg := Gui("+AlwaysOnTop -MinimizeBox", "Add POI")
        dlg.SetFont("s9", "Segoe UI")
        dlg.Add("Text", "w300", zone " at " GameCoordText(x, y))
        dlg.Add("Text", "xm y+12 w60", "Label:")
        labelEdit := dlg.Add("Edit", "x+8 yp-3 w232")
        dlg.Add("Text", "xm y+12 w60", "Type:")
        kindDdl := dlg.Add("DropDownList", "x+8 yp-3 w150", _Pois_KINDS)
        kindDdl.Value := _Pois_KindIndex(_Pois_DefaultKind)

        Accept(*) {
            label := Trim(labelEdit.Value)
            if (label = "") {
                TrayTip("Give the POI a label.", "Map POIs", "Iconx")
                return
            }
            result := { label: label, kind: _Pois_KINDS[kindDdl.Value] }
            dlg.Destroy()
        }

        dlg.Add("Button", "xm y+16 w90 Default", "Add").OnEvent("Click", Accept)
        dlg.Add("Button", "x+8 w90", "Cancel").OnEvent("Click", (*) => dlg.Destroy())
        dlg.OnEvent("Close", (*) => dlg.Destroy())
        dlg.OnEvent("Escape", (*) => dlg.Destroy())
        dlg.Show("AutoSize")
        try labelEdit.Focus()
        WinWaitClose("ahk_id " dlg.Hwnd)
        return result
    }

    result := 0
    dllDir := (A_PtrSize = 8) ? "64bit" : "32bit"
    dllPath := A_ScriptDir "\Lib\" dllDir "\WebView2Loader.dll"
    wvSettings := { DllPath: dllPath, DefaultWidth: 360, DefaultHeight: 330 }

    dlg := WebViewGui("-Caption +AlwaysOnTop -Resize", "Add POI",, wvSettings)
    ; This function blocks in WinWaitClose below, so every way the window can go
    ; away has to actually destroy it — including Alt+F4, which the page's own
    ; cancel button never sees.
    dlg.OnEvent("Close", (*) => dlg.Destroy())

    kindsJson := "["
    for i, k in _Pois_KINDS {
        if (i > 1) kindsJson .= ","
        kindsJson .= _JSON_Str(k)
    }
    kindsJson .= "]"

    coordStr := GameCoordText(x, y)

    dlg.WebMessageReceived(WebMsgHandler(OnWebMsg))
    dlg.DOMContentLoaded((*) => _SafePostMsg(dlg, '{"type":"init-add-poi"'
        . ',"zoneName":' _JSON_Str(zone)
        . ',"coordText":' _JSON_Str(coordStr)
        . ',"kinds":' kindsJson
        . ',"defaultKind":' _JSON_Str(_Pois_DefaultKind) '}'))

    dlg.Navigate(UiPageUrl("ui/map_pois/add_poi.html"))
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
        TrayTip("Enter a map first — POIs are stored per map.", "Map POIs", "Iconx")
        return
    }
    if IsObject(_Pois_ManageGui) {
        try _Pois_ManageGui.Destroy()
        _Pois_ManageGui := 0
    }

    if (IsNativeInterface() || !_Pois_CanUseWebView("index.html")) {
        g := Gui("+AlwaysOnTop -MinimizeBox", "Map POIs — " ZoneDisplayName(mapId))
        g.SetFont("s9", "Segoe UI")
        _Pois_ManageGui := g
        lv := g.Add("ListView", "w440 r10 -Multi Grid", ["Label", "Type", "X", "Y"])
        _Pois_FillNativeManageList(lv, mapId)
        g.Add("Button", "xm y+10 w120", "Delete selected")
            .OnEvent("Click", (*) => _Pois_DeleteNativeSelected(lv, mapId))
        g.Add("Button", "x+8 w105", "Export map")
            .OnEvent("Click", (*) => _Pois_ExportCurrentMap())
        g.Add("Button", "x+8 w90", "Close")
            .OnEvent("Click", (*) => g.Destroy())
        g.OnEvent("Close", (*) => (_Pois_ManageGui := 0))
        g.OnEvent("Escape", (*) => g.Destroy())
        g.Show("AutoSize")
        return
    }

    dllDir := (A_PtrSize = 8) ? "64bit" : "32bit"
    dllPath := A_ScriptDir "\Lib\" dllDir "\WebView2Loader.dll"
    wvSettings := { DllPath: dllPath, DefaultWidth: 500, DefaultHeight: 460 }

    g := WebViewGui("-Caption +AlwaysOnTop -Resize", "Map POIs — " ZoneDisplayName(mapId),, wvSettings)
    _Pois_ManageGui := g

    g.OnEvent("Close", (*) => (_Pois_ManageGui := 0))
    g.WebMessageReceived(WebMsgHandler(_Pois_OnManageWebMsg))
    g.DOMContentLoaded((*) => SetTimer(_Pois_SendManageState, -50))
    g.Navigate(UiPageUrl("ui/map_pois/index.html"))

    g.Show("w500 h460")
}

_Pois_FillNativeManageList(lv, mapId) {
    lv.Delete()
    for poi in _Pois_Load(mapId)
        lv.Add(, poi.label, poi.kind, GameCoordX(poi.x), GameCoordY(poi.y))
    loop 4
        lv.ModifyCol(A_Index, "AutoHdr")
}

_Pois_DeleteNativeSelected(lv, mapId) {
    row := lv.GetNext()
    if (row < 1) {
        TrayTip("Select a POI to delete.", "Map POIs", "Iconx")
        return
    }
    _Pois_DeleteAt(mapId, row)
    _Pois_FillNativeManageList(lv, mapId)
    _Pois_Redraw()
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

_SafePostMsg(targetGui, json) {
    if !IsObject(targetGui)
        return
    try {
        targetGui.PostWebMessageAsJson(json)
    } catch {
        try targetGui.PostWebMessageAsString(json)
    }
}

_Pois_HandleManageSubmitPoi(mapId, label, kind) {
    global _Pois_DefaultKind
    rawPos := ReadRawPlayerPosition()
    if !rawPos.ok {
        TrayTip("Could not read position — is the game client running?", "Map POIs", "Iconx")
        return
    }
    if !_Pois_Add(mapId, { x: rawPos.x, y: rawPos.y, label: label, kind: kind }) {
        TrayTip("Give the POI a label.", "Map POIs", "Iconx")
        return
    }
    _Pois_DefaultKind := _Pois_IsKind(kind) ? kind : "npc"
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
        TrayTip("Enter a map first.", "Map POIs", "Iconx")
        return
    }
    list := _Pois_Load(mapId)
    if (list.Length = 0) {
        TrayTip("No POIs saved for " ZoneDisplayName(mapId) " yet.", "Map POIs", "Iconi")
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
        TrayTip("Export failed: " err.Message, "Map POIs", "Iconx")
        return
    }
    SaveNpcNextId()
    A_Clipboard := text
    ; The map id is named here on purpose — it's what the exported entries use.
    TrayTip("Exported " list.Length " POI(s) for " ZoneDisplayName(mapId) " (" mapId ")`n"
        . NPC_OUTPUT_FILE "`n(also copied to the clipboard)", "Map POIs", "Iconi")
}

; ── POI packs ────────────────────────────────────────────────────
;
; Export writes the server repo's NPC entry format, which is useful to the
; server and useless to another player. A pack is the other half: pois.ini's own
; sections, for every map at once, so a curated set can be handed to someone.
;
; Import MERGES. Replacing would make a shared pack destructive to whatever the
; recipient had already marked, and there is no undo for that.

_Pois_PackPath(forSave) {
    title := forSave ? "Save POI pack" : "Open POI pack"
    if forSave {
        sel := FileSelect("S16", A_ScriptDir "\poi_pack.ini", title, "POI packs (*.ini)")
    } else {
        sel := FileSelect(3, A_ScriptDir, title, "POI packs (*.ini)")
    }
    if (sel = "")
        return ""
    if (forSave && !RegExMatch(sel, "i)\.ini$"))
        sel .= ".ini"
    return sel
}

; Every map's POIs, written in the same shape pois.ini uses so a pack is just a
; pois.ini — readable, diffable, and mergeable by hand if someone wants to.
_Pois_ExportPack() {
    path := _Pois_PackPath(true)
    if (path = "")
        return
    src := _Pois_Path()
    if !FileExist(src) {
        TrayTip("There are no POIs saved yet.", "Map POIs", "Iconi")
        return
    }

    maps := 0, total := 0
    try {
        if FileExist(path)
            FileDelete(path)
        EnsureIniUtf16(path)
        for mapId in _Pois_AllMapIds() {
            list := _Pois_Load(mapId)
            if (list.Length = 0)
                continue
            maps++
            for i, poi in list {
                IniWrite(poi.x "|" poi.y "|" poi.label "|" poi.kind, path, mapId, String(i))
                total++
            }
        }
    } catch as err {
        TrayTip("Could not write the pack: " err.Message, "Map POIs", "Iconx")
        return
    }
    if (total = 0) {
        TrayTip("There are no POIs saved yet.", "Map POIs", "Iconi")
        return
    }
    LogInfo("MapPois", "Exported pack: " total " POIs across " maps " maps -> " path)
    TrayTip("Exported " total " POI(s) across " maps " map(s) to`n" path,
        "Map POIs", "Iconi")
}

; Merges a pack in. A POI already present — same map, same position, same label
; — is skipped rather than duplicated, so importing the same pack twice is a
; no-op instead of doubling everything.
_Pois_ImportPack() {
    path := _Pois_PackPath(false)
    if (path = "")
        return
    if !FileExist(path) {
        TrayTip("That file no longer exists.", "Map POIs", "Iconx")
        return
    }

    added := 0, skipped := 0, maps := 0, rejected := 0
    for mapId in _Pois_PackMapIds(path) {
        incoming := _Pois_ReadSection(path, mapId)
        if (incoming.Length = 0)
            continue
        existing := _Pois_Load(mapId).Clone()
        seen := Map()
        for poi in existing
            seen[_Pois_DedupeKey(poi)] := true

        before := existing.Length
        for poi in incoming {
            ; An imported file is untrusted text: same sanitising as a POI typed
            ; into the app, so a pack cannot smuggle a delimiter into the store.
            poi.label := _Pois_SanitizeLabel(poi.label)
            if (poi.label = "") {
                rejected++
                continue
            }
            if !_Pois_IsKind(poi.kind)
                poi.kind := "npc"
            key := _Pois_DedupeKey(poi)
            if seen.Has(key) {
                skipped++
                continue
            }
            seen[key] := true
            existing.Push(poi)
            added++
        }
        if (existing.Length != before) {
            _Pois_Save(mapId, existing)
            maps++
        }
    }

    if (added = 0 && skipped = 0 && rejected = 0) {
        TrayTip("That file has no POIs in it.", "Map POIs", "Icon!")
        return
    }
    LogInfo("MapPois", "Imported pack " path ": +" added ", " skipped " dupes, "
        . rejected " rejected")
    msg := "Added " added " POI(s) across " maps " map(s)."
    if skipped
        msg .= "`n" skipped " already present, left alone."
    if rejected
        msg .= "`n" rejected " had no usable label and were skipped."
    TrayTip(msg, "Map POIs", "Iconi")
    _Pois_Redraw()
}

; Identity for merge purposes: where it is, what it is called, on which map.
; Position alone would reject two different NPCs standing together.
_Pois_DedupeKey(poi) {
    return poi.x "|" poi.y "|" StrLower(poi.label)
}

; Section names in the live store. IniRead with no section returns them all.
_Pois_AllMapIds() {
    return _Pois_SectionNames(_Pois_Path())
}

_Pois_PackMapIds(path) {
    return _Pois_SectionNames(path)
}

_Pois_SectionNames(path) {
    names := []
    if !FileExist(path)
        return names
    try raw := IniRead(path)
    catch
        return names
    loop parse, raw, "`n", "`r" {
        s := Trim(A_LoopField)
        if (s != "")
            names.Push(s)
    }
    return names
}

; Parses one section of any pois.ini-shaped file. Same record format as
; _Pois_Load, but pointed at an arbitrary path rather than the live store.
_Pois_ReadSection(path, mapId) {
    list := []
    try section := IniRead(path, mapId)
    catch
        return list
    loop parse, section, "`n", "`r" {
        line := Trim(A_LoopField)
        if (line = "" || !InStr(line, "="))
            continue
        poi := _Pois_ParseRecord(SubStr(line, InStr(line, "=") + 1))
        if IsObject(poi)
            list.Push(poi)
    }
    return list
}

; ── Game markers ─────────────────────────────────────────────────
;
; The client mods' minimap has a marker layer of its own, stored in
; map-markers.tsv in the game folder (NativeModsMarkersReplace has the file's
; rules). Exporting puts every POI there, so the game's own minimap shows them
; without the Maps++ overlay open.
;
; Maps++ owns only the markers it exported. Each POI's marker id is derived from
; its map, position and label, and the ids of the last export are kept per game
; folder in map_markers_export.ini. Together those let a re-export update its
; own markers and take out ones for deleted POIs, while a marker the player
; placed in game is never touched. An export is deliberately not kept in sync on
; every POI edit: the file is the mods', and they are written to only when the
; user asks.

; The marker file's kind numbers (map-markers.md section 1). The same five
; kinds as _Pois_KINDS, but this is the file format's enum, not our order.
global _Pois_GAME_MARKER_KIND := Map("npc", 0, "shop", 1, "portal", 2, "quest", 3, "note", 4)

; 64-bit FNV-1a as 16 lowercase hex: the marker file's id shape. AHK integers
; wrap at 64 bits, which is exactly the modulus FNV needs.
_Pois_Fnv64(text) {
    h := -3750763034362895579          ; 0xCBF29CE484222325 as a signed int64
    buf := Buffer(StrPut(text, "UTF-8"))
    loop StrPut(text, buf, "UTF-8") - 1 {
        h := h ^ NumGet(buf, A_Index - 1, "UChar")
        h := h * 1099511628211           ; 0x100000001B3
    }
    return Format("{:016x}", h)
}

; Same identity as a pack import's dedupe (_Pois_DedupeKey), plus the map: a
; moved or renamed POI is a new marker, and the old one goes on the next export.
_Pois_GameMarkerId(mapId, poi) {
    return _Pois_Fnv64("mapspp|" StrUpper(mapId) "|" _Pois_DedupeKey(poi))
}

_Pois_GameMarkerStatePath() {
    return ResolveWritableIniPath("map_markers_export.ini")
}

; One section per game folder, since each folder has its own marker file.
_Pois_GameMarkerSection() {
    return "Folder." _Pois_Fnv64(StrLower(ResolveGameDirectory()))
}

_Pois_LoadExportedIds() {
    ids := []
    raw := ""
    try raw := IniRead(_Pois_GameMarkerStatePath(), _Pois_GameMarkerSection(), "Ids", "")
    for id in StrSplit(raw, ",") {
        id := Trim(id)
        if RegExMatch(id, "^[0-9a-f]{16}$")
            ids.Push(id)
    }
    return ids
}

_Pois_SaveExportedIds(ids) {
    path := _Pois_GameMarkerStatePath()
    section := _Pois_GameMarkerSection()
    joined := ""
    for id in ids
        joined .= (joined = "" ? "" : ",") id
    try {
        EnsureIniUtf16(path)
        IniWrite(ResolveGameDirectory(), path, section, "GameDir")
        IniWrite(joined, path, section, "Ids")
    } catch as err {
        ; The markers are written; only the record of them is lost. A re-export
        ; still finds them by id, so this costs only the removal of markers
        ; for POIs deleted before then.
        LogWarn("MapPois", "Could not record exported marker ids: " err.Message)
    }
}

; Every POI as a marker record: {records, maps, invalid}. Raw positions become
; map units by the same fixed offset as the displayed coordinates, which
; puts a marker exactly where its POI was captured (checked against the game's
; own character position on MAP303).
_Pois_GameMarkerRecords() {
    global GAME_COORD_OFFSET_X, GAME_COORD_OFFSET_Y, _Pois_GAME_MARKER_KIND
    out := { records: [], maps: 0, invalid: 0 }
    for mapId in _Pois_AllMapIds() {
        list := _Pois_Load(mapId)
        if (list.Length = 0)
            continue
        ; The file keys markers on the base map file number (MAPnnn.MAP).
        if !(RegExMatch(mapId, "i)^MAP(\d{1,5})$", &m) && Integer(m[1]) <= 0xFFFF) {
            out.invalid += list.Length
            continue
        }
        mapFile := Integer(m[1])
        placed := false
        for poi in list {
            x := poi.x + GAME_COORD_OFFSET_X
            y := poi.y + GAME_COORD_OFFSET_Y
            if (x < 0 || y < 0) {
                out.invalid++
                continue
            }
            out.records.Push({ id: _Pois_GameMarkerId(mapId, poi), mapFile: mapFile,
                x: x, y: y, label: poi.label,
                kind: _Pois_GAME_MARKER_KIND.Has(poi.kind) ? _Pois_GAME_MARKER_KIND[poi.kind] : 0 })
            placed := true
        }
        if placed
            out.maps++
    }
    return out
}

; Why a write to the marker file did not happen, for the user.
_Pois_GameMarkerFailure(reason) {
    switch reason {
        case "no-game-path":
            return "No game path is set, so Maps++ cannot find the game folder. Set one under "
                . "Settings → Game & startup."
        case "not-installed":
            return "The game's own markers come from the osMW client mods, which are not "
                . "installed in " ResolveGameDirectory() "."
        case "locked":
            return "A game client was saving its markers and did not finish in time. Try again."
        case "lock-unavailable":
            return "Maps++ could not share the marker file's lock with the running game "
                . "clients. Close them and try again."
        case "unknown-format":
            return "map-markers.tsv is in a format this Maps++ does not know — probably written "
                . "by newer client mods — so it was left exactly as it is."
        case "read-failed":
            return "Could not read map-markers.tsv in the game folder."
    }
    return "Could not write map-markers.tsv in the game folder. mapsplusplus.log has the detail."
}

_Pois_ExportGameMarkers() {
    global NATIVE_MODS_MARKERS_PER_MAP, NATIVE_MODS_MARKERS_MAX
    title := "Map POIs — Export to game markers"
    if !NativeModsInstalled() {
        ShowMessage(_Pois_GameMarkerFailure(ResolveGameDirectory() = "" ? "no-game-path"
            : "not-installed") " Nothing was exported.", title, Map("severity", "info"))
        return
    }
    built := _Pois_GameMarkerRecords()
    previous := _Pois_LoadExportedIds()
    if (built.records.Length = 0) {
        ShowMessage("There are no POIs to export."
            . (previous.Length ? " To take back the markers an earlier export added, use "
                . "Remove Exported Game Markers." : ""), title, Map("severity", "info"))
        return
    }
    layerOn := NativeModsMarkerLayerOn()
    ask := "Export " built.records.Length " POI(s) on " built.maps " map(s) as markers on the "
        . "game's own minimap?`n`nThey go into map-markers.tsv in the game folder, which every "
        . "client there shares. Markers you placed in the game are left alone; markers from an "
        . "earlier Maps++ export are replaced by this one."
    if !layerOn
        ask .= "`n`nThe game's marker layer is switched off ([Minimap] MarkerLayer in osmw.ini), "
            . "so the markers will not show in game until it is turned on."
    if !ConfirmAction(ask, title)
        return

    res := NativeModsMarkersReplace(previous, built.records)
    if !res.ok {
        ShowMessage(_Pois_GameMarkerFailure(res.reason) " Nothing was changed.", title,
            Map("severity", "warn"))
        return
    }
    _Pois_SaveExportedIds(res.written)

    writtenSet := Map()
    for id in res.written
        writtenSet[id] := true
    updated := 0, removed := 0
    for id in res.removedIds {
        if writtenSet.Has(id)
            updated++
        else
            removed++
    }
    msg := "Exported " res.written.Length " marker(s): " (res.written.Length - updated) " new, "
        . updated " updated" (removed ? ", and " removed " removed for POIs deleted since the "
        . "last export" : "") "."
    if res.full
        msg .= "`n`n" res.full " did not fit. The game holds at most "
            . NATIVE_MODS_MARKERS_PER_MAP " markers per map, counting the ones you placed "
            . "yourself, and " NATIVE_MODS_MARKERS_MAX " in all."
    if built.invalid
        msg .= "`n`n" built.invalid " POI(s) could not be placed: they belong to a map "
            . "Maps++ cannot number, or sit outside its edge."
    if !layerOn
        msg .= "`n`nThey will appear once the game's marker layer is on ([Minimap] "
            . "MarkerLayer=1 in osmw.ini)."
    ShowMessage(msg, title, Map("severity", (res.full || built.invalid) ? "warn" : "success"))
}

_Pois_RemoveGameMarkers() {
    title := "Map POIs — Remove exported game markers"
    previous := _Pois_LoadExportedIds()
    if (previous.Length = 0) {
        ShowMessage("Maps++ has not exported any markers to this game folder, so there is "
            . "nothing to remove.", title, Map("severity", "info"))
        return
    }
    if !ConfirmAction("Remove the " previous.Length " marker(s) Maps++ exported to the game's "
        . "own minimap?`n`nMarkers you placed in the game are left alone.", title)
        return
    res := NativeModsMarkersReplace(previous, [])
    if !res.ok {
        ShowMessage(_Pois_GameMarkerFailure(res.reason) " Nothing was changed.", title,
            Map("severity", "warn"))
        return
    }
    _Pois_SaveExportedIds([])
    gone := previous.Length - res.removedIds.Length
    ShowMessage("Removed " res.removedIds.Length " marker(s)."
        . (gone > 0 ? " " gone " had already been deleted in the game." : ""), title,
        Map("severity", "success"))
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
    TrayTip(_Pois_LayerVisible ? "Layer on" : "Layer off", "Map POIs", "Iconi")
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
    global gOverlayVisible, gGui, gCurrentMapName, MINIMAP_MAP_INSET, POI_MAX_DRAWN
    global _Pois_LayerVisible, _Pois_LabelMode, _Pois_Pool, _Pois_UsedCount

    if (!gOverlayVisible || !IsObject(gGui) || !gGui.Hwnd) {
        return
    }
    if !_Pois_LayerVisible {
        _Pois_HideAll()
        return
    }
    mapId := MapIdFromName(gCurrentMapName)
    list := _Pois_Load(mapId)
    _Pois_EnsurePool(Min(list.Length, POI_MAX_DRAWN))
    _Pois_WarnOverflowOnce(mapId, list.Length)

    scale := MinimapScaleFactor()
    ; A little smaller than the player marker so it never competes with it.
    size := Max(3, Round(MinimapMarkerSize() * 0.7))
    showLabels := ShouldShowMarkerLabels(_Pois_LabelMode)
    used := 0
    for poi in list {
        ; The cap is stated here, not just implied by the pool size — a pool
        ; grown before the cap existed is never shrunk.
        if (used >= _Pois_Pool.Length || used >= POI_MAX_DRAWN) {
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

; Says so once per map when POIs are being left undrawn, rather than on every
; redraw or not at all. The draw loop stops at the pool size, so the cap needs
; no enforcement of its own here.
_Pois_WarnOverflowOnce(mapId, total) {
    global POI_MAX_DRAWN, _Pois_OverflowNotified
    if (total <= POI_MAX_DRAWN || _Pois_OverflowNotified = mapId)
        return
    _Pois_OverflowNotified := mapId
    TrayTip(ZoneDisplayName(mapId) " has " total " POIs — only the first "
        . POI_MAX_DRAWN " are drawn.", "Map POIs", "Iconi")
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
