#Requires AutoHotkey v2.0
#SingleInstance Force
#Warn

Persistent
SetWorkingDir(A_ScriptDir)
SetTitleMatchMode(2)

#Include variables.ahk
#Include functions.ahk
#Include *i _addons.ahk

; ── Launcher startup ─────────────────────────────────────────────

LoadLauncherConfig()
LoadNpcNextId()
if !A_IsCompiled
    GenerateAddonIncludes()
LoadAddonEnabledStates()

; Auto-launch a game instance on startup / re-launch.
if (gLaunchOnStartup) {
    LaunchGameInstance("primary")
}

; ── Startup checks ───────────────────────────────────────────────

if !DirExist(MAP_DIR) {
    TrayTip("AHK Minimap", "Map folder missing:`n" MAP_DIR, "Iconi")
}
if !FileExist(MARKER_PNG) {
    TrayTip("AHK Minimap", "marker.png missing next to script — position marker disabled.`n" MARKER_PNG, "Iconi")
}

; ── Tray menu ────────────────────────────────────────────────────

trayMenu := A_TrayMenu
trayMenu.Delete()
trayMenu.Add("Launch Game`tCtrl+Alt+L", (*) => LaunchGameInstance("primary"))
trayMenu.Add("Launch Game (Secondary)`tCtrl+Alt+K", (*) => LaunchGameInstance("secondary"))
trayMenu.Add("Set Game Path...", (*) => PromptForGamePath())
trayMenu.Add()
FireAddonHook("OnTrayMenu", trayMenu)
trayMenu.Add()
if gAddonHooks.Length > 0 {
    addonsMenu := Menu()
    for _idx, _am in gAddonHooks {
        _n := _am.Has("name") ? _am["name"] : "<unnamed>"
        addonsMenu.Add(_n, _ToggleAddon.Bind(_n, addonsMenu))
        if !gDisabledAddons.Has(_n) || !gDisabledAddons[_n]
            addonsMenu.Check(_n)
    }
    trayMenu.Add("Addons", addonsMenu)
}
trayMenu.Add("Reload`tCtrl+Alt+R", (*) => Reload())
debugMenu := Menu()
debugMenu.Add("Debug State`tCtrl+Alt+D", (*) => ShowDebugState())
debugMenu.Add("Calibrate Signatures`tCtrl+Alt+S", (*) => CalibrateSignaturesNow())
debugMenu.Add("Verify Signatures`tCtrl+Alt+V", (*) => VerifyResolution())
trayMenu.Add("Debug", debugMenu)
trayMenu.Add()
trayMenu.Add("Exit`tCtrl+Alt+Q", (*) => ExitApp())
trayMenu.Default := "Launch Game`tCtrl+Alt+L"
A_IconTip := "osMW Maps++"

; ── Timers ───────────────────────────────────────────────────────

SetTimer(UpdateMapState, 250)
; Marker timer starts disabled — enabled only while the overlay is visible.
SetTimer(UpdateMarkerPosition, 0)
SetTimer(CloseOverlayIfFocusLeftGame, 100)
OnExit((*) => ReleaseCachedProcessHandle())
FireAddonHook("OnInit")

; ── Hotkeys ──────────────────────────────────────────────────────

#HotIf gOverlayVisible && IsGameOrOverlayActive()
$Tab:: CloseOverlay()
$RButton:: CloseOverlay()
#HotIf

#HotIf WinActive(GAME_WIN_FILTER) && gCanOverride && !gOverlayVisible && IsMinimapAllowed()
$Tab:: HandleTab()
#HotIf

^!r:: Reload()
^!q:: ExitApp()
#HotIf IsGameOrOverlayActive()
^!l:: LaunchGameInstance("primary")
^!k:: LaunchGameInstance("secondary")
^!d:: ShowDebugState()
^!s:: CalibrateSignaturesNow()
^!v:: VerifyResolution()
; ^!n:: GenerateNpcEntry()
^!1:: CaptureCalibrationPoint(1)
^!2:: CaptureCalibrationPoint(2)
^!3:: ApplyCalibrationFromPoints()
^!4:: ExportCurrentCalibrationToFile()
#HotIf

; ── Core handlers ────────────────────────────────────────────────

HandleTab() {
    if gOverlayVisible {
        CloseOverlay()
        return
    }

    ; Tab is captured only when a valid custom map exists.
    ShowOrToggleOverlay(gResolvedMapName, gResolvedMapPath)
}

CloseOverlay() {
    global gOverlayVisible, gGui
    if gOverlayVisible && IsObject(gGui) {
        gGui.Hide()
        gOverlayVisible := false
        SetTimer(UpdateMarkerPosition, 0)
        FireAddonHook("OnOverlayHide")
    }
}

; Hide minimap when focus leaves the game and this overlay (e.g. Alt+Tab to another app). Game or minimap Gui keeps it open.
CloseOverlayIfFocusLeftGame() {
    global gOverlayVisible, gGui
    if !gOverlayVisible || !IsObject(gGui) || !gGui.Hwnd {
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
        src := gResolvedOffsets.Has(name) ? "sig" : "fallback"
        msg .= "  " name ": " Format("0x{:08X}", rva) " (" src ")`n"
    }
    MsgBox(msg, "AHK Minimap Debug")
}

; ── Calibration handlers ─────────────────────────────────────────

CaptureCalibrationPoint(index) {
    global gOverlayVisible, gGui, gResolvedMapName, MINIMAP_MAP_INSET, OVERLAY_W, OVERLAY_H
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
    if (relX < 0 || relY < 0 || relX >= OVERLAY_W || relY >= OVERLAY_H) {
        MsgBox("Place your mouse over the minimap image before capturing.", "Calibration")
        return
    }

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
    global MINIMAP_MAP_INSET, MINIMAP_COLOR_GOLD, OVERLAY_W, OVERLAY_H
    if WinActive(GAME_WIN_FILTER) {
        gTrackedGameHwnd := WinActive(GAME_WIN_FILTER)
    }

    pos := GetOverlayPositionForGameWindow()
    xPos := pos.x
    yPos := pos.y
    ; Explicit client size — otherwise Gui auto-size can omit edges.
    totalW := OVERLAY_W + 2 * MINIMAP_MAP_INSET
    totalH := OVERLAY_H + 2 * MINIMAP_MAP_INSET
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
            "x" MINIMAP_MAP_INSET " y" MINIMAP_MAP_INSET " w" OVERLAY_W " h" OVERLAY_H,
            mapPath
        )
        EnsureMarkerControl()
        gGui.Show(showOpts)
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
        gOverlayVisible := true
        SetTimer(UpdateMarkerPosition, 60)
        UpdateMarkerPosition()
        FireAddonHook("OnOverlayShow", mapName)
    }
}

UpdateMarkerPosition() {
    global gOverlayVisible, gMarkerDot, gLastPosStatus, gLastRawX, gLastRawY
    global MINIMAP_MAP_INSET, OVERLAY_W, OVERLAY_H, MARKER_SIZE

    if !IsObject(gGui) || !gGui.Hwnd {
        return
    }

    ; Keep overlay centered on game window if it moves.
    pos := GetOverlayPositionForGameWindow()
    gGui.Move(pos.x, pos.y)

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
    markerPos := WorldToOverlayPixels(rawX, rawY, gCurrentMapName)
    px := markerPos.x
    py := markerPos.y

    px := Clamp(px, 0, OVERLAY_W - MARKER_SIZE)
    py := Clamp(py, 0, OVERLAY_H - MARKER_SIZE)
    gMarkerDot.Move(px + MINIMAP_MAP_INSET, py + MINIMAP_MAP_INSET, MARKER_SIZE, MARKER_SIZE)
    gMarkerDot.Visible := true
    gLastPosStatus := "ok x=" rawX " y=" rawY
}
