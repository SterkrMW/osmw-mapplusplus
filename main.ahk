#Requires AutoHotkey v2.0
;@Ahk2Exe-SetName Maps++
;@Ahk2Exe-SetDescription Maps++
;@Ahk2Exe-SetProductName Maps++
;@Ahk2Exe-SetCompanyName SterkrMW
#SingleInstance Force
#Warn

Persistent
SetWorkingDir(A_ScriptDir)
SetTitleMatchMode(2)

#Include variables.ahk
#Include functions.ahk
#Include settings.ahk
#Include hotkeys.ahk
#Include *i _addons.ahk

InitAppNotificationRegistration()

; ── Launcher startup ─────────────────────────────────────────────

LoadLauncherConfig()
LoadMinimapConfig()
LoadNpcNextId()
if !A_IsCompiled
    GenerateAddonIncludes()
LoadAddonEnabledStates()
; Addons load their own config here rather than at include time: gInterfaceMode
; is set by LoadLauncherConfig above, and the enabled/disabled state is known, so
; a disabled addon's OnInit is correctly skipped.
FireAddonHook("OnInit")
RegisterCoreHotkeyActions()
LoadHotkeyOverrides()
ApplyAllHotkeys()
SetTimer(UpdateMapState, 200)
SetTimer(CloseOverlayIfFocusLeftGame, 200)

; Auto-launch a game instance on startup / re-launch.
if (gLaunchOnStartup) {
    LaunchGameInstance("primary")
}

; ── Startup checks ───────────────────────────────────────────────

if !DirExist(MAP_DIR) {
    TrayTip("Map folder missing:`n" MAP_DIR, "Maps++", "Iconi")
}
if !FileExist(MARKER_PNG) {
    TrayTip("marker.png missing next to script — position marker disabled.`n" MARKER_PNG, "Maps++", "Iconi")
}

; ── Tray menu ────────────────────────────────────────────────────

RebuildTrayMenu()
if IsWebViewInterface() {
    OnMessage(0x0404, _OnTrayNotify)
    OnMessage(0x0006, _OnTrayWmActivate)
    ; Enhanced mode intentionally pays the memory cost up front so the first
    ; right-click is as quick as every one after it.
    SetTimer(_PrewarmWebTrayMenu, -6000)
}

_OnTrayNotify(wParam, lParam, msg, hwnd) {
    ; 0x0205 = WM_RBUTTONUP (right-click on system tray icon)
    if (lParam = 0x0205) {
        ShowWebTrayMenu()
        return 0 ; Suppress native Win32 context menu
    }
}


_OnTrayWmActivate(wParam, lParam, msg, hwnd) {
    global gWebTrayGui, gWebTrayShown
    ; wParam = 0 (WA_INACTIVE). The window outlives any single open now, so this
    ; must only fire for a menu that is actually on screen.
    if (wParam = 0 && gWebTrayShown && IsObject(gWebTrayGui) && gWebTrayGui.Hwnd
        && hwnd = gWebTrayGui.Hwnd) {
        SetTimer(_CloseWebTrayMenu, -10)
    }
}

global gWebTrayGui := 0, gWebTrayCallbacks := Map()
; Same approach as the radial menu: standing up a WebView2 and loading the page
; costs hundreds of milliseconds, so the window is built once and parked
; off-screen between uses, and it is only moved into view after the page says
; it has drawn the items. Recreating it per right-click is what made the menu
; feel slow and flash an unstyled page first.
global gWebTrayItems := []
global gWebTrayPos := 0
global gWebTrayShown := false     ; on screen right now
global gWebTrayPending := false   ; an open is waiting for the page
global TRAY_MENU_W := 320, TRAY_MENU_H := 580

_EnsureWebTrayGui() {
    global gWebTrayGui, TRAY_MENU_W, TRAY_MENU_H

    if IsObject(gWebTrayGui) && gWebTrayGui.Hwnd {
        return
    }

    dllDir := (A_PtrSize = 8) ? "64bit" : "32bit"
    dllPath := A_ScriptDir "\Lib\" dllDir "\WebView2Loader.dll"
    wvSettings := { DllPath: dllPath, DefaultWidth: TRAY_MENU_W, DefaultHeight: TRAY_MENU_H }

    g := WebViewGui("-Caption +AlwaysOnTop +ToolWindow -MaximizeBox", "Maps++ Menu", , wvSettings)
    gWebTrayGui := g

    g.OnEvent("Close", (*) => _CloseWebTrayMenu())
    g.WebMessageReceived(_OnWebTrayMessage)
    g.DOMContentLoaded((*) => SetTimer(_PushTrayMenuState, -50))
    g.Navigate("ui/tray/index.html")

    g.Show("x-30000 y-30000 w" TRAY_MENU_W " h" TRAY_MENU_H " NoActivate")
}

; Warms the menu up shortly after launch so the first right-click is quick.
_PrewarmWebTrayMenu() {
    try _EnsureWebTrayGui()
}

ShowWebTrayMenu() {
    global gWebTrayGui, gWebTrayCallbacks, gWebTrayItems, gWebTrayPos, gWebTrayPending
    global TRAY_MENU_W, TRAY_MENU_H

    RebuildTrayMenu()
    gWebTrayCallbacks := Map()
    gWebTrayItems := _ConvertHmenuToWebItems(A_TrayMenu.Handle)

    _EnsureWebTrayGui()
    if !IsObject(gWebTrayGui) {
        return
    }

    mW := TRAY_MENU_W, mH := TRAY_MENU_H

    CoordMode("Mouse", "Screen")
    MouseGetPos(&mX, &mY)
    ; Clamp against the display the tray icon was clicked on, not the primary.
    MonitorGetWorkArea(GetMonitorIndexAtPoint(mX, mY), &sLeft, &sTop, &sRight, &sBottom)

    wX := mX - mW + 20
    wY := mY - mH - 10
    if (wX + mW > sRight - 10)
        wX := sRight - mW - 10
    if (wX < sLeft + 10)
        wX := sLeft + 10
    if (wY < sTop + 10)
        wY := mY + 10
    if (wY + mH > sBottom - 10)
        wY := sBottom - mH - 10

    gWebTrayPos := { x: wX, y: wY, w: mW, h: mH }
    gWebTrayPending := true

    ; Re-render with the menu as it stands now; the page answers with "rendered"
    ; and that is what brings the window into view.
    _PushTrayMenuState()
    ; Backstop for a page that fails to load and never reports.
    SetTimer(_RevealWebTrayMenu, -600)
}

_RevealWebTrayMenu() {
    global gWebTrayGui, gWebTrayPos, gWebTrayShown, gWebTrayPending
    if !gWebTrayPending || !IsObject(gWebTrayGui) || !IsObject(gWebTrayPos) {
        return
    }
    gWebTrayPending := false
    gWebTrayShown := true
    SetTimer(_RevealWebTrayMenu, 0)

    p := gWebTrayPos
    hwnd := gWebTrayGui.Hwnd
    WinMove(p.x, p.y, p.w, p.h, "ahk_id " hwnd)
    ; Parked NoActivate; activate now, since losing activation dismisses it.
    try WinActivate("ahk_id " hwnd)
}

_PushTrayMenuState() {
    global gWebTrayGui, gWebTrayItems
    if !IsObject(gWebTrayGui)
        return
    itemsJson := _TrayItemsToJson(gWebTrayItems)
    try gWebTrayGui.PostWebMessageAsJson('{"type":"tray-menu-state","items":' itemsJson '}')
}

_OnWebTrayMessage(wv, args) {
    global gWebTrayGui, gWebTrayCallbacks, gWebTrayItems
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
            if IsObject(gWebTrayGui) {
                RebuildTrayMenu()
                gWebTrayItems := _ConvertHmenuToWebItems(A_TrayMenu.Handle)
                _PushTrayMenuState()
            }
        case "rendered":
            ; The page has drawn the items, so there is something worth showing.
            _RevealWebTrayMenu()
        case "execute-item":
            if msg.Has("id") && gWebTrayCallbacks.Has(msg["id"]) {
                cb := gWebTrayCallbacks[msg["id"]]
                _CloseWebTrayMenu()
                try cb()
            }
        case "dismiss":
            _CloseWebTrayMenu()
    }
}

; Dismisses the menu by parking it off-screen again. The window and its loaded
; page survive, which is what makes the next right-click instant.
_CloseWebTrayMenu() {
    global gWebTrayGui, gWebTrayShown, gWebTrayPending, gWebTrayPos
    SetTimer(_RevealWebTrayMenu, 0)
    if IsObject(gWebTrayGui) && gWebTrayGui.Hwnd {
        try WinMove(-30000, -30000, , , "ahk_id " gWebTrayGui.Hwnd)
    }
    gWebTrayShown := false
    gWebTrayPending := false
    gWebTrayPos := 0
}

_TrayItemsToJson(items) {
    json := "["
    first := true
    for item in items {
        if !first
            json .= ","
        first := false
        if item.Has("isDivider") && item["isDivider"] {
            json .= '{"isDivider":true}'
            continue
        }
        json .= '{'
            . '"id":' _JSON_Str(item.Has("id") ? item["id"] : "")
            . ',"label":' _JSON_Str(item.Has("label") ? item["label"] : "")
            . ',"icon":' _JSON_Str(item.Has("icon") ? item["icon"] : "")
            . ',"shortcut":' _JSON_Str(item.Has("shortcut") ? item["shortcut"] : "")
            . ',"isDefault":' (item.Has("isDefault") && item["isDefault"] ? "true" : "false")
        if item.Has("children") {
            json .= ',"children":' _TrayItemsToJson(item["children"])
        }
        json .= '}'
    }
    json .= "]"
    return json
}

_ConvertHmenuToWebItems(hMenu, idPrefix := "menu_") {
    global gWebTrayCallbacks
    items := []
    count := DllCall("GetMenuItemCount", "Ptr", hMenu, "Int")
    if (count <= 0)
        return items

    loop count {
        idx := A_Index - 1
        meta := _GetHmenuItemMeta(hMenu, idx)
        if meta.isSeparator {
            items.Push(Map("isDivider", true))
            continue
        }

        bufLen := DllCall("GetMenuString", "Ptr", hMenu, "UInt", idx, "Ptr", 0, "Int", 0, "UInt", 0x400)
        if (bufLen <= 0) {
            items.Push(Map("isDivider", true))
            continue
        }
        buf := Buffer((bufLen + 1) * 2, 0)
        DllCall("GetMenuString", "Ptr", hMenu, "UInt", idx, "Ptr", buf, "Int", bufLen + 1, "UInt", 0x400)
        str := StrGet(buf)

        parts := StrSplit(str, "`t")
        label := parts[1]
        shortcut := (parts.Length >= 2) ? parts[2] : ""
        isDefault := meta.isDefault

        hSub := DllCall("GetSubMenu", "Ptr", hMenu, "Int", idx, "Ptr")
        if (hSub) {
            children := _ConvertHmenuToWebItems(hSub, idPrefix idx "_")
            items.Push(Map("label", label, "icon", _IconForLabel(label), "children", children))
        } else {
            itemId := idPrefix idx
            cmdId := DllCall("GetMenuItemID", "Ptr", hMenu, "Int", idx, "UInt")
            gWebTrayCallbacks[itemId] := _MakeHmenuCallback(hMenu, cmdId)
            items.Push(Map("id", itemId, "label", label, "icon", _IconForLabel(label), "shortcut", shortcut,
            "isDefault", isDefault))
        }
    }
    return items
}

; GetMenuState() packs a popup submenu's item count into its high byte, so a
; submenu with eight children also has bit 0x800 set and looks like
; MF_SEPARATOR. GetMenuItemInfoW keeps type and state in distinct fields.
_GetHmenuItemMeta(hMenu, idx) {
    static MIIM_STATE := 0x1, MIIM_FTYPE := 0x100
    static MFT_SEPARATOR := 0x800, MFS_DEFAULT := 0x1000
    infoSize := (A_PtrSize = 8) ? 80 : 48
    info := Buffer(infoSize, 0)
    NumPut("UInt", infoSize, info, 0)
    NumPut("UInt", MIIM_STATE | MIIM_FTYPE, info, 4)
    if !DllCall("GetMenuItemInfoW", "Ptr", hMenu, "UInt", idx, "Int", true,
        "Ptr", info.Ptr, "Int")
        return {isSeparator: false, isDefault: false}
    itemType := NumGet(info, 8, "UInt")
    itemState := NumGet(info, 12, "UInt")
    return {
        isSeparator: (itemType & MFT_SEPARATOR) ? true : false,
        isDefault: (itemState & MFS_DEFAULT) ? true : false
    }
}

_MakeHmenuCallback(hMenu, cmdId) {
    return (*) => DllCall("PostMessage", "Ptr", A_ScriptHwnd, "UInt", 0x0111, "Ptr", cmdId, "Ptr", hMenu)
}

_IconForLabel(lbl) {
    switch lbl {
        case "Launch Game", "Launch Game (Secondary)": return "rocket_launch"
        case "Launch Clients + Apply Layout": return "devices"
        case "Send Enter Until Ready": return "keyboard"
        case "Settings…": return "settings"
        case "Reload": return "refresh"
        case "Debug", "Debug State": return "bug_report"
        case "Calibrate Signatures": return "track_changes"
        case "Verify Signatures": return "search"
        case "Exit": return "power_settings_new"
        case "Chat": return "chat"
        case "Client Roster": return "group"
        case "Client Roster (list)": return "format_list_bulleted"
        case "Inventory": return "inventory_2"
        case "Map POIs": return "location_on"
        case "Party Markers": return "shield"
        case "View Mode": return "visibility"
        case "Window Layout", "Apply Preset", "Apply Custom": return "grid_view"
        case "Capture Current As…": return "devices"
        case "Layouts…": return "format_list_bulleted"
        case "Undo Last Apply": return "refresh"
        case "Interface": return "tune"
        case "Native (low memory)", "● Native (low memory)": return "memory"
        case "WebView2 (enhanced)", "● WebView2 (enhanced)": return "web_asset"
        default: return "pin_drop"
    }
}

RebuildTrayMenu() {
    global gInterfaceMode
    trayMenu := A_TrayMenu
    trayMenu.Delete()
    trayMenu.Add("Launch Game`t" GetHotkeyDisplay("launchPrimary"), (*) => LaunchGameInstance("primary"))
    trayMenu.Add("Launch Game (Secondary)`t" GetHotkeyDisplay("launchSecondary"), (*) => LaunchGameInstance("secondary"
    ))
    trayMenu.Add("Launch Clients + Apply Layout`t" GetHotkeyDisplay("launchClientsLayout"), (*) =>
        LaunchClientsAndApplyLayout())
    trayMenu.Add("Send Enter Until Ready`t" GetHotkeyDisplay("sendEnterUntilReady"), (*) => SendEnterUntilReady())
    trayMenu.Add()
    FireAddonHook("OnTrayMenu", trayMenu)
    trayMenu.Add()
    interfaceMenu := Menu()
    nativeLabel := (gInterfaceMode = "native" ? "● " : "") "Native (low memory)"
    webLabel := (gInterfaceMode = "webview" ? "● " : "") "WebView2 (enhanced)"
    interfaceMenu.Add(nativeLabel, (*) => SetInterfaceMode("native"))
    interfaceMenu.Add(webLabel, (*) => SetInterfaceMode("webview"))
    trayMenu.Add("Interface", interfaceMenu)
    trayMenu.Add("Settings…`t" GetHotkeyDisplay("openSettings"), (*) => ShowSettingsWindow())
    trayMenu.Add("Reload`tCtrl+Alt+R", (*) => Reload())
    debugMenu := Menu()
    debugMenu.Add("Debug State`tCtrl+Alt+D", (*) => ShowDebugState())
    debugMenu.Add("Calibrate Signatures`tCtrl+Alt+S", (*) => CalibrateSignaturesNow())
    debugMenu.Add("Verify Signatures`tCtrl+Alt+V", (*) => VerifyResolution())
    trayMenu.Add("Debug", debugMenu)
    trayMenu.Add()
    trayMenu.Add("Exit`tCtrl+Alt+Q", (*) => ExitApp())
    trayMenu.Default := "Launch Game`t" GetHotkeyDisplay("launchPrimary")
    A_IconTip := "osMW Maps++"
}

HandleTab() {
    if gOverlayVisible {
        CloseOverlay()
        return
    }

    ; Tab is captured only when a valid custom map exists.
    ShowOrToggleOverlay(gResolvedMapName, gResolvedMapPath)
}

CloseOverlay() {
    global gOverlayVisible, gGui, gOverlayHover
    if gOverlayVisible && IsObject(gGui) {
        gGui.Hide()
        gOverlayVisible := false
        ; The hover timer stops with the overlay, so clear the state rather
        ; than leaving it stuck on for the next time it opens.
        gOverlayHover := false
        SetTimer(UpdateMarkerPosition, 0)
        FireAddonHook("OnOverlayHide")
    }
}

; Hide minimap when focus leaves the game and this overlay (e.g. Alt+Tab to another app). Game or minimap Gui keeps it open.
; Pin mode ("Keep the minimap open when the game loses focus") opts out of this
; check only — the battle / loading / unsupported-map closes in UpdateMapState
; still apply, so a pinned overlay never floats over a game that isn't there.
CloseOverlayIfFocusLeftGame() {
    global gOverlayVisible, gGui, gMinimapKeepOpen, gOverlayDragging
    if !gOverlayVisible || !IsObject(gGui) || !gGui.Hwnd {
        return
    }
    if (gMinimapKeepOpen || gOverlayDragging) {
        return
    }
    if WinActive(GAME_WIN_FILTER) {
        return
    }
    if WinActive("ahk_id " gGui.Hwnd) {
        return
    }
    CloseOverlay()
}

UpdateMapState() {
    global gCanOverride, gResolvedMapName, gResolvedMapPath, gTrackedGameHwnd
    global gOverlayVisible, gCurrentMapPath, gCurrentMapName, gPic
    static sPrevMapName := ""

    activeHwnd := WinActive(GAME_WIN_FILTER)
    if !activeHwnd {
        gCanOverride := false
        gResolvedMapName := ""
        gResolvedMapPath := ""
        return
    }

    ; Auto-close overlay when minimap is no longer allowed (battle, loading screen, etc.)
    if gOverlayVisible && !IsMinimapAllowed() {
        CloseOverlay()
    }

    gTrackedGameHwnd := activeHwnd
    mapName := ReadCurrentMapName()
    if (mapName = "") {
        gCanOverride := false
        gResolvedMapName := ""
        gResolvedMapPath := ""
        if (sPrevMapName != "") {
            prev := sPrevMapName
            sPrevMapName := ""
            FireAddonHook("OnMapChange", "", prev)
        }
        ; Scene changed to one with no readable map name — close overlay.
        if gOverlayVisible {
            CloseOverlay()
        }
        return
    }

    mapPath := ResolveMapPath(mapName)
    if (mapPath = "") {
        gCanOverride := false
        gResolvedMapName := mapName
        gResolvedMapPath := ""
        if (mapName != sPrevMapName) {
            prev := sPrevMapName
            sPrevMapName := mapName
            FireAddonHook("OnMapChange", mapName, prev)
        }
        ; New scene has no custom minimap image — close overlay.
        if gOverlayVisible {
            CloseOverlay()
        }
        return
    }

    gCanOverride := true
    gResolvedMapName := mapName
    gResolvedMapPath := mapPath
    if (mapName != sPrevMapName) {
        prev := sPrevMapName
        sPrevMapName := mapName
        FireAddonHook("OnMapChange", mapName, prev)
    }

    ; Hot-swap the minimap image if the scene changed to a different map.
    if gOverlayVisible && (gCurrentMapPath != mapPath) {
        gPic.Value := mapPath
        gCurrentMapName := mapName
        gCurrentMapPath := mapPath
    }
}

ShowDebugState() {
    global gCanOverride, gResolvedMapName, gResolvedMapPath, gLastReadStatus, gLastPosStatus
    global gLastRawX, gLastRawY, gResolvedOffsets, gResolvedBuildStamp, gFallbackOffsets
    msg := "CanOverride: " gCanOverride "`n"
        . "ReadStatus: " gLastReadStatus "`n"
        . "PosStatus: " gLastPosStatus "`n"
        . "RawX: " gLastRawX " RawY: " gLastRawY "`n"
        . "MapName: " (gResolvedMapName = "" ? "<empty>" : gResolvedMapName) "`n"
        . "MapPath: " (gResolvedMapPath = "" ? "<missing>" : gResolvedMapPath) "`n`n"
        . "Build: " (gResolvedBuildStamp ? Format("0x{:08X}", gResolvedBuildStamp) : "<unresolved>") "`n"
    for _, name in SIGNATURE_NAMES {
        rva := GetResolvedOffset(name)
        msg .= "  " name ": " Format("0x{:08X}", rva) " (" OffsetSourceLabel(name) ")`n"
    }
    ; Per-client reads. Mainly here to check the character class against what
    ; the game shows — classId is what picks avatars\c<N>.png in the radial
    ; menu, and -1 means "unreadable or not logged in yet".
    UpdateClientSnapshots()
    snapshots := GetClientSnapshots()
    msg .= "`nClients: " snapshots.Length "`n"
    for _, snap in snapshots {
        msg .= "  " (snap.charName = "" ? "PID " snap.pid : snap.charName)
            . " — class " snap.classId ", state " snap.gameState "`n"
    }
    MsgBox(msg, "AHK Minimap Debug")
}

; ── Calibration handlers ─────────────────────────────────────────

CaptureCalibrationPoint(index) {
    global gOverlayVisible, gGui, gResolvedMapName, MINIMAP_MAP_INSET
    global gCalibrationPoint1, gCalibrationPoint2

    if !gOverlayVisible || !IsObject(gGui) || !gGui.Hwnd {
        MsgBox("Open the minimap first, then hover a landmark and capture again.", "Calibration")
        return
    }
    if (gResolvedMapName = "") {
        MsgBox("No resolved map name available yet.", "Calibration")
        return
    }

    rawPos := ReadRawPlayerPosition()
    if !rawPos.ok {
        MsgBox("Failed to read raw position from memory.", "Calibration")
        return
    }

    CoordMode("Mouse", "Screen")
    MouseGetPos(&mx, &my)
    ; Client (0,0); map image origin is MINIMAP_MAP_INSET from client top-left.
    pt := Buffer(8, 0)
    DllCall("user32\ClientToScreen", "Ptr", gGui.Hwnd, "Ptr", pt)
    clx := NumGet(pt, 0, "Int")
    cly := NumGet(pt, 4, "Int")
    relX := mx - clx - MINIMAP_MAP_INSET
    relY := my - cly - MINIMAP_MAP_INSET
    if (relX < 0 || relY < 0 || relX >= MinimapDisplayW() || relY >= MinimapDisplayH()) {
        MsgBox("Place your mouse over the minimap image before capturing.", "Calibration")
        return
    }
    ; Calibration is stored in base (unscaled) map space, so a user calibrating
    ; at 150 % writes the same numbers as one calibrating at 100 %.
    scale := MinimapScaleFactor()
    relX := Round(relX / scale)
    relY := Round(relY / scale)

    point := {
        mapName: gResolvedMapName,
        rawX: rawPos.x,
        rawY: rawPos.y,
        px: relX,
        py: relY
    }

    if (index = 1) {
        gCalibrationPoint1 := point
    } else {
        gCalibrationPoint2 := point
    }

    MsgBox(
        "Captured point " index "`n"
        . "Map: " point.mapName "`n"
        . "Raw: " point.rawX ", " point.rawY "`n"
        . "Pixel: " point.px ", " point.py,
        "Calibration"
    )
}

ApplyCalibrationFromPoints() {
    global gCalibrationPoint1, gCalibrationPoint2, MAP_DIR

    if !IsObject(gCalibrationPoint1) || !IsObject(gCalibrationPoint2) {
        MsgBox("Capture two points first (`Ctrl+Alt+1` and `Ctrl+Alt+2`).", "Calibration")
        return
    }
    if (gCalibrationPoint1.mapName != gCalibrationPoint2.mapName) {
        MsgBox("Points are from different maps. Recapture both points on the same map.", "Calibration")
        return
    }

    dxRaw := gCalibrationPoint2.rawX - gCalibrationPoint1.rawX
    dyRaw := gCalibrationPoint2.rawY - gCalibrationPoint1.rawY
    dxPx := gCalibrationPoint2.px - gCalibrationPoint1.px
    dyPx := gCalibrationPoint2.py - gCalibrationPoint1.py
    if (dxRaw = 0 || dyRaw = 0) {
        MsgBox("Captured points are invalid (raw delta is zero). Choose two separated landmarks.", "Calibration")
        return
    }

    multX := dxPx / dxRaw
    multY := dyPx / dyRaw
    addX := gCalibrationPoint1.px - (gCalibrationPoint1.rawX * multX)
    addY := gCalibrationPoint1.py - (gCalibrationPoint1.rawY * multY)

    mapName := gCalibrationPoint1.mapName
    gCalibrationCache.Delete(mapName)
    dims := GetImageDimensionsFromFile(MAP_DIR "\" mapName)
    SaveExplicitCalibrationToIni(mapName, multX, addX, multY, addY, dims.w, dims.h)

    calibText := "[" mapName "]`n"
        . "mode=explicit`n"
        . "multX=" Format("{:.10f}", multX) "`n"
        . "addX=" Format("{:.10f}", addX) "`n"
        . "multY=" Format("{:.10f}", multY) "`n"
        . "addY=" Format("{:.10f}", addY) "`n"
    if (dims.w > 0 && dims.h > 0) {
        calibText .= "sourceW=" dims.w "`nsourceH=" dims.h "`n"
    }

    A_Clipboard := calibText
    MsgBox(
        "Calibration saved for " mapName ".`n`n"
        . CombinedCalibrationPath() "`n`n"
        . "Section copied to clipboard for reference.",
        "Calibration"
    )
}

ExportCurrentCalibrationToFile() {
    global gResolvedMapName
    if (gResolvedMapName = "") {
        MsgBox("Enter a map with a custom minimap first.", "Calibration")
        return
    }
    global MAP_DIR
    cal := GetCalibration(gResolvedMapName)
    dims := GetImageDimensionsFromFile(MAP_DIR "\" gResolvedMapName)
    SaveExplicitCalibrationToIni(gResolvedMapName, cal.multX, cal.addX, cal.multY, cal.addY, dims.w, dims.h)
    gCalibrationCache.Delete(gResolvedMapName)
    MsgBox("Wrote section [" gResolvedMapName "] in " CombinedCalibrationPath(), "Calibration")
}

; ── Overlay display ──────────────────────────────────────────────

ShowOrToggleOverlay(mapName, mapPath) {
    global gOverlayVisible, gCurrentMapName, gCurrentMapPath, gGui, gPic, gTrackedGameHwnd
    global MINIMAP_MAP_INSET, MINIMAP_COLOR_GOLD
    if WinActive(GAME_WIN_FILTER) {
        gTrackedGameHwnd := WinActive(GAME_WIN_FILTER)
    }

    pos := GetOverlayPositionForGameWindow()
    xPos := pos.x
    yPos := pos.y
    ; Displayed size (base map space × user scale).
    mapW := MinimapDisplayW()
    mapH := MinimapDisplayH()
    ; Explicit client size — otherwise Gui auto-size can omit edges.
    totalW := mapW + 2 * MINIMAP_MAP_INSET
    totalH := mapH + 2 * MINIMAP_MAP_INSET
    showOpts := "x" xPos " y" yPos " w" totalW " h" totalH " NoActivate"

    if !IsObject(gGui) {
        ; WS_EX_NOACTIVATE: clicks on the minimap do not steal focus from the game.
        gGui := Gui("+AlwaysOnTop -Caption +ToolWindow +E0x08000000")
        gGui.MarginX := 0
        gGui.MarginY := 0
        ; Outer 1px gold shows as Gui background; black rect at (1,1) leaves that ring; map covers center.
        gGui.BackColor := MINIMAP_COLOR_GOLD
        borderBlack := gGui.AddText(
            "x1 y1 w" (totalW - 2) " h" (totalH - 2) " Background000000",
            ""
        )
        DllCall("uxtheme\SetWindowTheme", "Ptr", borderBlack.Hwnd, "WStr", "", "WStr", "")
        gPic := gGui.AddPicture(
            "x" MINIMAP_MAP_INSET " y" MINIMAP_MAP_INSET " w" mapW " h" mapH,
            mapPath
        )
        EnsureMarkerControl()
        gGui.Show(showOpts)
        ApplyOverlayOpacity()
        gOverlayVisible := true
        gCurrentMapName := mapName
        gCurrentMapPath := mapPath
        SetTimer(UpdateMarkerPosition, 60)
        UpdateMarkerPosition()
        FireAddonHook("OnOverlayShow", mapName)
        return
    }

    if (gCurrentMapPath != mapPath) {
        gPic.Value := mapPath
        gCurrentMapName := mapName
        gCurrentMapPath := mapPath
    }

    if gOverlayVisible {
        gGui.Hide()
        gOverlayVisible := false
        SetTimer(UpdateMarkerPosition, 0)
    } else {
        gGui.Show(showOpts)
        ApplyOverlayOpacity()
        gOverlayVisible := true
        SetTimer(UpdateMarkerPosition, 60)
        UpdateMarkerPosition()
        FireAddonHook("OnOverlayShow", mapName)
    }
}

UpdateMarkerPosition() {
    global gOverlayVisible, gMarkerDot, gLastPosStatus, gLastRawX, gLastRawY
    global MINIMAP_MAP_INSET, gOverlayDragging

    if !IsObject(gGui) || !gGui.Hwnd {
        return
    }

    UpdateOverlayHoverState()

    ; Keep the overlay anchored to the game window as it moves — but never
    ; while the user is dragging it, or the drag fights this timer.
    if !gOverlayDragging {
        pos := GetOverlayPositionForGameWindow()
        gGui.Move(pos.x, pos.y)
    }

    EnsureMarkerControl()
    if !IsObject(gMarkerDot) {
        return
    }

    cached := GetCachedProcessHandleAndBase()
    if !cached.ok {
        gLastPosStatus := "pos_process_not_found"
        return
    }

    ; X and Y are contiguous (4 bytes apart) — read both in one 8-byte call.
    posBuf := Buffer(8, 0)
    ok := DllCall(
        "ReadProcessMemory",
        "Ptr", cached.handle,
        "Ptr", cached.modBase + GetResolvedOffset("POS_X_OFFSET"),
        "Ptr", posBuf.Ptr,
        "UPtr", 8,
        "UPtr*", 0,
        "Int"
    )

    if !ok {
        gLastPosStatus := "pos_read_failed"
        ReleaseCachedProcessHandle()
        return
    }

    rawX := NumGet(posBuf, 0, "Int")
    rawY := NumGet(posBuf, 4, "Int")
    gLastRawX := rawX
    gLastRawY := rawY
    ; Calibration yields base-space pixels; scale them for the displayed size.
    markerPos := WorldToOverlayPixels(rawX, rawY, gCurrentMapName)
    scale := MinimapScaleFactor()
    size := MinimapMarkerSize()
    px := Round(markerPos.x * scale)
    py := Round(markerPos.y * scale)

    px := Clamp(px, 0, MinimapDisplayW() - size)
    py := Clamp(py, 0, MinimapDisplayH() - size)
    gMarkerDot.Move(px + MINIMAP_MAP_INSET, py + MINIMAP_MAP_INSET, size, size)
    gMarkerDot.Visible := true
    gLastPosStatus := "ok x=" rawX " y=" rawY
}
