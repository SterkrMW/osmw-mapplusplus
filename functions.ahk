#Requires AutoHotkey v2.0

InitAppNotificationRegistration() {
    try {
        A_AppUserModelID := "SterkrMW.MapsPlusPlus"
        regKey := "HKCU\Software\Classes\AppUserModelId\SterkrMW.MapsPlusPlus"
        RegWrite("Maps++", "REG_SZ", regKey, "DisplayName")
        if A_IsCompiled {
            RegWrite(A_ScriptFullPath, "REG_SZ", regKey, "IconUri")
        }
    }
}

; ── Process & memory ──────────────────────────────────────────────

GetGameProcessId() {
    global gTrackedGameHwnd
    ; Prefer the HWND WinActive already returned — never use "A", which can
    ; vanish between the check and WinGetPID during Alt-Tab / focus changes.
    activeHwnd := WinActive(GAME_WIN_FILTER)
    if activeHwnd {
        try return WinGetPID("ahk_id " activeHwnd)
    }
    if (gTrackedGameHwnd && WinExist("ahk_id " gTrackedGameHwnd)) {
        ; Validate the tracked HWND still belongs to PROCESS_EXE.
        try {
            trackedExe := WinGetProcessName("ahk_id " gTrackedGameHwnd)
            if (StrLower(trackedExe) = StrLower(PROCESS_EXE)) {
                return WinGetPID("ahk_id " gTrackedGameHwnd)
            }
        }
        gTrackedGameHwnd := 0
    }
    return ProcessExist(PROCESS_EXE)
}

; Returns a cached {handle, modBase} for the game process. Reopens only when PID changes or handle is stale.
GetCachedProcessHandleAndBase() {
    global gCachedPID, gCachedProcessHandle, gCachedModuleBase
    pid := GetGameProcessId()
    if !pid {
        ReleaseCachedProcessHandle()
        return { ok: false }
    }
    if (pid = gCachedPID && gCachedProcessHandle && gCachedModuleBase) {
        return { ok: true, handle: gCachedProcessHandle, modBase: gCachedModuleBase, pid: pid }
    }
    ReleaseCachedProcessHandle()
    processHandle := DllCall(
        "OpenProcess",
        "UInt", 0x0010 | 0x0400,
        "Int", 0,
        "UInt", pid,
        "Ptr"
    )
    if !processHandle {
        return { ok: false }
    }
    moduleBase := GetModuleBaseAddress(processHandle, PROCESS_EXE)
    if !moduleBase {
        DllCall("CloseHandle", "Ptr", processHandle)
        return { ok: false }
    }
    gCachedPID := pid
    gCachedProcessHandle := processHandle
    gCachedModuleBase := moduleBase
    ; Resolve build-specific RVAs once per attach (cheap on cache hit, scans
    ; the .text section on cache miss). Falls back to hardcoded constants
    ; if no signature has been captured yet.
    EnsureResolvedOffsetsForBuild(processHandle, moduleBase)
    return { ok: true, handle: processHandle, modBase: moduleBase, pid: pid }
}

ReleaseCachedProcessHandle() {
    global gCachedPID, gCachedProcessHandle, gCachedModuleBase
    global gResolvedOffsets, gResolvedBuildStamp
    if gCachedProcessHandle {
        DllCall("CloseHandle", "Ptr", gCachedProcessHandle)
    }
    gCachedPID := 0
    gCachedProcessHandle := 0
    gCachedModuleBase := 0
    ; Force re-resolution on next attach (build may have changed).
    gResolvedOffsets := Map()
    gResolvedBuildStamp := 0
}

GetModuleBaseAddress(hProcess, moduleName) {
    static LIST_MODULES_ALL := 0x03

    ; Psapi: works when Toolhelp MODULEENTRY32 layout mismatches (e.g. 64-bit AHK vs 32-bit main.exe).
    bufSize := 1024 * A_PtrSize
    buf := Buffer(bufSize, 0)
    cbNeeded := 0
    ok := DllCall("psapi\EnumProcessModulesEx", "Ptr", hProcess, "Ptr", buf.Ptr, "UInt", bufSize, "UInt*", &cbNeeded, "UInt", LIST_MODULES_ALL, "Int")
    if !ok {
        return 0
    }
    if (cbNeeded > bufSize) {
        bufSize := cbNeeded
        buf := Buffer(bufSize, 0)
        if !DllCall("psapi\EnumProcessModulesEx", "Ptr", hProcess, "Ptr", buf.Ptr, "UInt", bufSize, "UInt*", &cbNeeded, "UInt", LIST_MODULES_ALL, "Int") {
            return 0
        }
    }

    isWow64 := 0
    if (A_Is64bitOS) {
        DllCall("kernel32\IsWow64Process", "Ptr", hProcess, "Int*", &isWow64)
    }
    ; 64-bit script enumerating 32-bit process: module table is DWORD-sized bases.
    stride := (A_PtrSize == 8 && isWow64) ? 4 : A_PtrSize
    nModules := cbNeeded // stride
    if (nModules < 1) {
        return 0
    }

    moduleNameLower := StrLower(moduleName)
    nameBuf := Buffer(520, 0)
    loop nModules {
        modBase := NumGet(buf, (A_Index - 1) * stride, stride == 4 ? "UInt" : "Ptr")
        if !modBase {
            continue
        }
        nameLen := DllCall("psapi\GetModuleBaseNameW", "Ptr", hProcess, "Ptr", modBase, "Ptr", nameBuf.Ptr, "UInt", 260, "UInt")
        if !nameLen {
            continue
        }
        name := StrGet(nameBuf.Ptr, 260, "UTF-16")
        name := Trim(name, "`0")
        if (StrLower(name) = moduleNameLower) {
            return modBase
        }
    }
    return 0
}

; Returns true only when game state is 10 and the player is not in battle.
IsMinimapAllowed() {
    cached := GetCachedProcessHandleAndBase()
    if !cached.ok {
        return false
    }
    valBuf := Buffer(4, 0)
    ok := DllCall(
        "ReadProcessMemory",
        "Ptr", cached.handle,
        "Ptr", cached.modBase + GetResolvedOffset("GAME_STATE_OFFSET"),
        "Ptr", valBuf.Ptr,
        "UPtr", 4,
        "UPtr*", 0,
        "Int"
    )
    if !ok || NumGet(valBuf, 0, "Int") != 10 {
        return false
    }
    ok := DllCall(
        "ReadProcessMemory",
        "Ptr", cached.handle,
        "Ptr", cached.modBase + GetResolvedOffset("BATTLE_STATE_OFFSET"),
        "Ptr", valBuf.Ptr,
        "UPtr", 4,
        "UPtr*", 0,
        "Int"
    )
    if !ok {
        return false
    }
    return NumGet(valBuf, 0, "Int") = 0
}

; Reads the map filename from memory (e.g. "MAP007.map") and returns the
; base name with .map swapped for .jpg (e.g. "MAP007.jpg") for minimap lookup.
ReadCurrentMapName() {
    global gLastReadStatus

    cached := GetCachedProcessHandleAndBase()
    if !cached.ok {
        gLastReadStatus := "process_not_found"
        return ""
    }

    targetAddr := cached.modBase + GetResolvedOffset("MAP_FILE_OFFSET")
    rawBytes := Buffer(MAP_FILE_LEN, 0)
    bytesRead := 0
    ok := DllCall(
        "ReadProcessMemory",
        "Ptr", cached.handle,
        "Ptr", targetAddr,
        "Ptr", rawBytes.Ptr,
        "UPtr", MAP_FILE_LEN,
        "UPtr*", &bytesRead,
        "Int"
    )

    if (!ok || bytesRead < 1) {
        gLastReadStatus := "read_failed"
        ReleaseCachedProcessHandle()
        return ""
    }

    mapFile := StrGet(rawBytes, MAP_FILE_LEN, "CP0")
    mapFile := Trim(mapFile, " `t`r`n`0")
    if (mapFile = "") {
        gLastReadStatus := "read_empty_string"
        return ""
    }
    ; Swap .map extension for .jpg — the minimap images mirror the game's map filenames.
    mapFile := RegExReplace(mapFile, "i)\.map$", ".jpg")
    gLastReadStatus := "ok"
    return mapFile
}

ReadRawPlayerPosition() {
    cached := GetCachedProcessHandleAndBase()
    if !cached.ok {
        return { ok: false }
    }

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
        return { ok: false }
    }
    return { ok: true, x: NumGet(posBuf, 0, "Int"), y: NumGet(posBuf, 4, "Int") }
}

; Friendly zone name for the tracked client (e.g. "Stillreach"), or "" when it
; can't be read. Data is keyed by the map file id (MAP302) because that's what
; the game and the server repo use, but this is what a player calls the place,
; so it's what UI should show.
ReadCurrentZoneName() {
    global MAP_NAME_LEN
    cached := GetCachedProcessHandleAndBase()
    if !cached.ok {
        return ""
    }
    buf := Buffer(MAP_NAME_LEN, 0)
    ok := DllCall(
        "ReadProcessMemory",
        "Ptr", cached.handle,
        "Ptr", cached.modBase + GetResolvedOffset("MAP_NAME_OFFSET"),
        "Ptr", buf.Ptr,
        "UPtr", MAP_NAME_LEN,
        "UPtr*", 0,
        "Int"
    )
    if !ok {
        return ""
    }
    name := Trim(StrGet(buf, MAP_NAME_LEN, "CP0"), " `t`r`n`0")
    ; Filename spill — the zone name field isn't populated right now.
    if RegExMatch(name, "i)^MAP\d+\.map$") {
        return ""
    }
    return name
}

; Zone name when it's readable, else the map id, so UI always has something.
ZoneDisplayName(mapId) {
    zone := ReadCurrentZoneName()
    return (zone != "") ? zone : mapId
}

; ── Coordinates ──────────────────────────────────────────────────
; The game displays coordinates as raw memory X/16 and Y/8 — those are the
; numbers a player reads off their screen and quotes to other players. Stored
; POI data and the server repo's NPC entries stay in raw memory units, so this
; conversion is display-only.

; Floor(), not AHK's // — that truncates toward zero, so a position just outside
; the walkable area (raw below -offset) would round the wrong way and read one
; unit high instead of one low.
GameCoordX(rawX) {
    global GAME_COORD_DIV_X, GAME_COORD_OFFSET_X
    return Floor((rawX + GAME_COORD_OFFSET_X) / GAME_COORD_DIV_X)
}

GameCoordY(rawY) {
    global GAME_COORD_DIV_Y, GAME_COORD_OFFSET_Y
    return Floor((rawY + GAME_COORD_OFFSET_Y) / GAME_COORD_DIV_Y)
}

; Displayed coordinate back to a raw position — the inverse of the pair above.
; Returns the low end of the raw range that displays as this coordinate, since
; the forward direction is lossy (16 raw units share one displayed X).
RawCoordFromGameX(gameX) {
    global GAME_COORD_DIV_X, GAME_COORD_OFFSET_X
    return gameX * GAME_COORD_DIV_X - GAME_COORD_OFFSET_X
}

RawCoordFromGameY(gameY) {
    global GAME_COORD_DIV_Y, GAME_COORD_OFFSET_Y
    return gameY * GAME_COORD_DIV_Y - GAME_COORD_OFFSET_Y
}

GameCoordText(rawX, rawY) {
    return GameCoordX(rawX) ", " GameCoordY(rawY)
}

; ── Map resolution ───────────────────────────────────────────────

ResolveMapPath(mapName) {
    if (mapName = "") {
        return ""
    }
    fullPath := MAP_DIR "\" mapName
    if FileExist(fullPath) {
        return fullPath
    }
    return ""
}

; ── Calibration ──────────────────────────────────────────────────

CombinedCalibrationPath() {
    global MAP_DIR
    return MAP_DIR "\calibration.ini"
}

SaveExplicitCalibrationToIni(mapName, multX, addX, multY, addY, sourceW := 0, sourceH := 0) {
    global MAP_DIR
    path := CombinedCalibrationPath()
    if !DirExist(MAP_DIR) {
        DirCreate(MAP_DIR)
    }
    IniWrite("explicit", path, mapName, "mode")
    IniWrite(Format("{:.10f}", multX), path, mapName, "multX")
    IniWrite(Format("{:.10f}", addX), path, mapName, "addX")
    IniWrite(Format("{:.10f}", multY), path, mapName, "multY")
    IniWrite(Format("{:.10f}", addY), path, mapName, "addY")
    if (sourceW > 0) {
        IniWrite(String(sourceW), path, mapName, "sourceW")
    }
    if (sourceH > 0) {
        IniWrite(String(sourceH), path, mapName, "sourceH")
    }
}

LoadCalibrationFromIni(mapName) {
    combinedPath := CombinedCalibrationPath()
    if !FileExist(combinedPath) {
        return ""
    }
    cal := ReadCalibrationIniSection(combinedPath, mapName)
    if (Type(cal) = "Map") {
        return cal
    }
    return ""
}

ReadCalibrationIniSection(path, section) {
    mode := IniRead(path, section, "mode", "__MISSING__")
    multXKey := IniRead(path, section, "multX", "__NO__")
    if (mode = "__MISSING__" && multXKey = "__NO__") {
        return ""
    }
    if (mode = "__MISSING__") {
        mode := "explicit"
    }
    if (mode = "bounds") {
        rawMinX := Float(IniRead(path, section, "rawMinX", "0"))
        rawMaxX := Float(IniRead(path, section, "rawMaxX", "0"))
        rawMinY := Float(IniRead(path, section, "rawMinY", "0"))
        rawMaxY := Float(IniRead(path, section, "rawMaxY", "0"))
        invertY := Integer(IniRead(path, section, "invertY", "0"))
        boundsCal := CalibrationFromBounds(rawMinX, rawMaxX, rawMinY, rawMaxY, invertY)
        if (Type(boundsCal) = "Map") {
            return boundsCal
        }
        return ""
    }
    m := Map(
        "multX", Float(IniRead(path, section, "multX", "0")),
        "addX", Float(IniRead(path, section, "addX", "0")),
        "multY", Float(IniRead(path, section, "multY", "0")),
        "addY", Float(IniRead(path, section, "addY", "0"))
    )
    sw := Trim(IniRead(path, section, "sourceW", ""))
    sh := Trim(IniRead(path, section, "sourceH", ""))
    if (sw != "") {
        m["sourceW"] := Integer(sw)
    }
    if (sh != "") {
        m["sourceH"] := Integer(sh)
    }
    return m
}

CalibrationFromBounds(rawMinX, rawMaxX, rawMinY, rawMaxY, invertY) {
    global OVERLAY_W, OVERLAY_H
    spanX := rawMaxX - rawMinX
    spanY := rawMaxY - rawMinY
    if (spanX = 0 || spanY = 0) {
        return ""
    }
    multX := OVERLAY_W / spanX
    addX := -rawMinX * multX
    if invertY {
        multY := -OVERLAY_H / spanY
        addY := rawMaxY * OVERLAY_H / spanY
    } else {
        multY := OVERLAY_H / spanY
        addY := -rawMinY * multY
    }
    return Map("multX", multX, "addX", addX, "multY", multY, "addY", addY)
}

GetCalibrationUncached(mapName) {
    cal := GetDefaultCalibration()
    if (mapName = "") {
        return cal
    }
    fileCal := LoadCalibrationFromIni(mapName)
    if (Type(fileCal) = "Map") {
        MergeSourceDimensionsFromImage(mapName, fileCal)
        ApplyCalibrationLayer(cal, fileCal)
        return cal
    }
    layer := Map()
    MergeSourceDimensionsFromImage(mapName, layer)
    ApplyCalibrationLayer(cal, layer)
    return cal
}

MergeSourceDimensionsFromImage(mapName, userCal) {
    global MAP_DIR
    if userCal.Has("sourceW") && userCal.Has("sourceH") {
        return
    }
    path := MAP_DIR "\" mapName
    dims := GetImageDimensionsFromFile(path)
    if (dims.w > 0 && dims.h > 0) {
        if !userCal.Has("sourceW") {
            userCal["sourceW"] := dims.w
        }
        if !userCal.Has("sourceH") {
            userCal["sourceH"] := dims.h
        }
    }
}

ApplyCalibrationLayer(cal, userCal) {
    global SOURCE_MAP_W, SOURCE_MAP_H, OVERLAY_W, OVERLAY_H
    sourceW := SOURCE_MAP_W
    sourceH := SOURCE_MAP_H
    if userCal.Has("sourceW") {
        sourceW := userCal["sourceW"]
    }
    if userCal.Has("sourceH") {
        sourceH := userCal["sourceH"]
    }
    cal.multX := OVERLAY_W / (sourceW * 16.0)
    cal.multY := OVERLAY_H / (sourceH * 8.0)
    if userCal.Has("multX") {
        cal.multX := userCal["multX"]
    }
    if userCal.Has("addX") {
        cal.addX := userCal["addX"]
    }
    if userCal.Has("multY") {
        cal.multY := userCal["multY"]
    }
    if userCal.Has("addY") {
        cal.addY := userCal["addY"]
    }
}

GetCalibration(mapName) {
    global gCalibrationCache
    if (mapName = "") {
        return GetDefaultCalibration()
    }
    if gCalibrationCache.Has(mapName) {
        return gCalibrationCache[mapName]
    }
    cal := GetCalibrationUncached(mapName)
    gCalibrationCache[mapName] := cal
    return cal
}

GetDefaultCalibration() {
    return {
        multX: OVERLAY_W / (SOURCE_MAP_W * 16.0),
        addX: 0.0,
        multY: OVERLAY_H / (SOURCE_MAP_H * 8.0),
        addY: 0.0
    }
}

WorldToOverlayPixels(rawX, rawY, mapName) {
    cal := GetCalibration(mapName)
    px := Floor((rawX * cal.multX) + cal.addX)
    py := Floor((rawY * cal.multY) + cal.addY)
    return { x: px, y: py }
}

; The inverse of WorldToOverlayPixels: base-space pixel back to a raw game
; position. Takes BASE pixels (the unscaled 400x300 space calibrations are
; stored in), so callers working from screen pixels must divide by
; MinimapScaleFactor() first — the same asymmetry the drawing code already has.
;
; Returns {ok, x, y}, matching ReadRawPlayerPosition's shape. A zero multiplier
; would be a divide by zero: ApplyCalibrationFromPoints refuses to write one, but
; calibration.ini is a text file a user can hand-edit, so it is checked rather
; than assumed.
OverlayPixelsToWorld(px, py, mapName) {
    cal := GetCalibration(mapName)
    if (cal.multX = 0 || cal.multY = 0) {
        return { ok: false, x: 0, y: 0 }
    }
    return {
        ok: true,
        x: Round((px - cal.addX) / cal.multX),
        y: Round((py - cal.addY) / cal.multY)
    }
}

; ── Image parsing ────────────────────────────────────────────────

GetImageDimensionsFromFile(path) {
    global gImageDimsCache
    if gImageDimsCache.Has(path) {
        return gImageDimsCache[path]
    }
    result := _ParseImageDimensions(path)
    if (result.w > 0 && result.h > 0) {
        gImageDimsCache[path] := result
    }
    return result
}

_ParseImageDimensions(path) {
    if !FileExist(path) {
        return { w: 0, h: 0 }
    }
    buf := FileRead(path, "RAW")
    size := buf.Size
    if (size < 24) {
        return { w: 0, h: 0 }
    }
    ; PNG: IHDR width/height at bytes 16-23 (big-endian)
    if (NumGet(buf, 0, "UChar") = 0x89 && NumGet(buf, 1, "UChar") = 0x50) {
        w := ReadUInt32BE(buf, 16)
        h := ReadUInt32BE(buf, 20)
        if (w > 0 && h > 0) {
            return { w: w, h: h }
        }
    }
    ; JPEG: scan segment markers, skip by segment length instead of byte-by-byte.
    if (NumGet(buf, 0, "UChar") = 0xFF && NumGet(buf, 1, "UChar") = 0xD8) {
        i := 2
        while (i < size - 8) {
            if (NumGet(buf, i, "UChar") != 0xFF) {
                i += 1
                continue
            }
            b1 := NumGet(buf, i + 1, "UChar")
            ; SOF markers: C0-CF except C4 (DHT), C8 (JPG), CC (DAC)
            if (b1 >= 0xC0 && b1 <= 0xCF && b1 != 0xC4 && b1 != 0xC8 && b1 != 0xCC) {
                h := (NumGet(buf, i + 5, "UChar") << 8) | NumGet(buf, i + 6, "UChar")
                w := (NumGet(buf, i + 7, "UChar") << 8) | NumGet(buf, i + 8, "UChar")
                if (w > 0 && h > 0) {
                    return { w: w, h: h }
                }
            }
            ; Standalone markers (RST, SOI, EOI, TEM) have no length field.
            if (b1 = 0x00 || b1 = 0x01 || (b1 >= 0xD0 && b1 <= 0xD9)) {
                i += 2
                continue
            }
            ; Skip segment using its 2-byte length field.
            if (i + 4 > size) {
                break
            }
            segLen := (NumGet(buf, i + 2, "UChar") << 8) | NumGet(buf, i + 3, "UChar")
            if (segLen < 2) {
                break
            }
            i += 2 + segLen
        }
    }
    return { w: 0, h: 0 }
}

ReadUInt32BE(buf, offset) {
    return (NumGet(buf, offset, "UChar") << 24)
        | (NumGet(buf, offset + 1, "UChar") << 16)
        | (NumGet(buf, offset + 2, "UChar") << 8)
        | NumGet(buf, offset + 3, "UChar")
}

; ── Minimap appearance ───────────────────────────────────────────

; Displayed size of the map image. Calibration always works in the 400×300 base
; space (OVERLAY_W/H); only drawing is scaled, so changing the scale can never
; invalidate maps\calibration.ini.
MinimapScaleFactor() {
    global gMinimapScale
    return gMinimapScale / 100.0
}

MinimapDisplayW() {
    global OVERLAY_W
    return Round(OVERLAY_W * MinimapScaleFactor())
}

MinimapDisplayH() {
    global OVERLAY_H
    return Round(OVERLAY_H * MinimapScaleFactor())
}

MinimapMarkerSize() {
    global MARKER_SIZE
    return Max(3, Round(MARKER_SIZE * MinimapScaleFactor()))
}

LoadMinimapConfig() {
    global gMinimapScale, gMinimapOpacity, gMinimapAnchor, gMinimapOffsetX, gMinimapOffsetY
    global gMinimapKeepOpen, MINIMAP_ANCHORS, CONFIG_INI, gShowHoverCoords
    if !FileExist(CONFIG_INI) {
        return
    }
    scale := Trim(IniRead(CONFIG_INI, "Minimap", "Scale", "100"))
    if IsInteger(scale) {
        gMinimapScale := Clamp(Integer(scale), 50, 200)
    }
    opacity := Trim(IniRead(CONFIG_INI, "Minimap", "Opacity", "100"))
    if IsInteger(opacity) {
        gMinimapOpacity := Clamp(Integer(opacity), 30, 100)
    }
    anchor := Trim(IniRead(CONFIG_INI, "Minimap", "Anchor", "Center"))
    for _, name in MINIMAP_ANCHORS {
        if (name = anchor) {
            gMinimapAnchor := name
            break
        }
    }
    ; Offsets are unbounded on purpose — a multi-monitor user can legitimately
    ; push the overlay outside the client rect.
    offX := Trim(IniRead(CONFIG_INI, "Minimap", "OffsetX", "0"))
    if IsInteger(offX) {
        gMinimapOffsetX := Integer(offX)
    }
    offY := Trim(IniRead(CONFIG_INI, "Minimap", "OffsetY", "0"))
    if IsInteger(offY) {
        gMinimapOffsetY := Integer(offY)
    }
    gMinimapKeepOpen := (Trim(IniRead(CONFIG_INI, "Minimap", "KeepOpenOnFocusLoss", "0")) = "1")
    gShowHoverCoords := (Trim(IniRead(CONFIG_INI, "Minimap", "ShowHoverCoords", "1")) != "0")
}

SaveMinimapConfig() {
    global gMinimapScale, gMinimapOpacity, gMinimapAnchor, gMinimapOffsetX, gMinimapOffsetY
    global gMinimapKeepOpen, CONFIG_INI, gShowHoverCoords
    IniWrite(gShowHoverCoords ? "1" : "0", CONFIG_INI, "Minimap", "ShowHoverCoords")
    IniWrite(gMinimapScale, CONFIG_INI, "Minimap", "Scale")
    IniWrite(gMinimapOpacity, CONFIG_INI, "Minimap", "Opacity")
    IniWrite(gMinimapAnchor, CONFIG_INI, "Minimap", "Anchor")
    IniWrite(gMinimapOffsetX, CONFIG_INI, "Minimap", "OffsetX")
    IniWrite(gMinimapOffsetY, CONFIG_INI, "Minimap", "OffsetY")
    IniWrite(gMinimapKeepOpen ? "1" : "0", CONFIG_INI, "Minimap", "KeepOpenOnFocusLoss")
}

; Applies the configured opacity. Must run after every Show() — AHK recreates
; the layered-window state when a Gui is hidden and shown again.
ApplyOverlayOpacity() {
    global gGui, gMinimapOpacity
    if !IsObject(gGui) || !gGui.Hwnd {
        return
    }
    if (gMinimapOpacity >= 100) {
        try WinSetTransparent("Off", "ahk_id " gGui.Hwnd)
        return
    }
    try WinSetTransparent(Round(255 * gMinimapOpacity / 100), "ahk_id " gGui.Hwnd)
}

; Drops the overlay Gui so the next ShowOrToggleOverlay() rebuilds it at the
; current size. Used when the scale changes — Gui controls can't be resized
; reliably in place once a picture is loaded into them.
RebuildOverlayGui() {
    global gGui, gPic, gMarkerDot, gOverlayVisible, gCurrentMapName, gCurrentMapPath
    wasVisible := gOverlayVisible
    mapName := gCurrentMapName
    mapPath := gCurrentMapPath
    SetTimer(UpdateMarkerPosition, 0)
    if IsObject(gGui) {
        try gGui.Destroy()
    }
    gGui := 0
    gPic := 0
    gMarkerDot := 0
    gOverlayVisible := false
    gCurrentMapName := ""
    gCurrentMapPath := ""
    ; Core controls on the old Gui are dangling now too.
    ResetCoordReadout()
    ResetWaypointControls()
    ; Addons that added controls to the old Gui must drop their references.
    FireAddonHook("OnOverlayRebuild")
    if (wasVisible && mapPath != "" && FileExist(mapPath)) {
        ShowOrToggleOverlay(mapName, mapPath)
    }
}

; ── Overlay positioning ──────────────────────────────────────────

; Screen position of the overlay's top-left corner, honouring anchor + offsets.
GetOverlayPositionForGameWindow() {
    global gMinimapOffsetX, gMinimapOffsetY
    base := GetOverlayAnchorPosition()
    return { x: base.x + gMinimapOffsetX, y: base.y + gMinimapOffsetY }
}

; Screen rect of the active game client's drawing area, for anything that has
; to sit on top of the game.
;
; The CLIENT rect, not the window rect, so placement ignores invisible DWM
; borders, the title bar and DPI-scaled chrome — those differ per Windows
; version and would otherwise shift every overlay by a few pixels.
;
; Falls back to the first game window found, then to the primary screen, so a
; caller always gets usable numbers; `ok` says whether a game window was behind
; them.
GameClientRect() {
    global GAME_WIN_FILTER
    hwnd := WinActive(GAME_WIN_FILTER)
    if !hwnd {
        hwnd := WinExist(GAME_WIN_FILTER)
    }
    if !hwnd {
        return { ok: false, hwnd: 0, x: 0, y: 0, w: A_ScreenWidth, h: A_ScreenHeight }
    }
    rc := Buffer(16, 0)
    DllCall("user32\GetClientRect", "Ptr", hwnd, "Ptr", rc)
    pt := Buffer(8, 0)
    DllCall("user32\ClientToScreen", "Ptr", hwnd, "Ptr", pt)
    return {
        ok: true, hwnd: hwnd,
        x: NumGet(pt, 0, "Int"), y: NumGet(pt, 4, "Int"),
        w: NumGet(rc, 8, "Int"), h: NumGet(rc, 12, "Int")
    }
}

; Same, with the user's offsets excluded — the zero point a drag is measured
; against, so a dragged overlay stays put when the game window moves.
GetOverlayAnchorPosition() {
    global MINIMAP_MAP_INSET, gMinimapAnchor
    totalW := MinimapDisplayW() + 2 * MINIMAP_MAP_INSET
    totalH := MinimapDisplayH() + 2 * MINIMAP_MAP_INSET

    area := GameClientRect()
    areaX := area.x, areaY := area.y, areaW := area.w, areaH := area.h

    switch gMinimapAnchor {
        case "TopLeft":
            return { x: areaX, y: areaY }
        case "TopRight":
            return { x: areaX + areaW - totalW, y: areaY }
        case "BottomLeft":
            return { x: areaX, y: areaY + areaH - totalH }
        case "BottomRight":
            return { x: areaX + areaW - totalW, y: areaY + areaH - totalH }
    }
    return {
        x: Floor(areaX + ((areaW - totalW) / 2)),
        y: Floor(areaY + ((areaH - totalH) / 2))
    }
}

; ── Marker labels ────────────────────────────────────────────────
; Shared by every layer that labels a dot on the minimap (party markers, POIs)
; so they stay visually identical. Labels sit in an opaque box sized to the
; text and centred over the dot — transparent text was unreadable against map
; art, and a fixed-width box would have been a black bar across the map.

; Rendered size of `text` in the control's own font.
MeasureControlText(ctrl, text) {
    if (text = "") {
        return { w: 0, h: 0 }
    }
    hdc := DllCall("GetDC", "Ptr", ctrl.Hwnd, "Ptr")
    if !hdc {
        return { w: 0, h: 0 }
    }
    previousFont := 0
    if (hFont := SendMessage(0x0031, 0, 0, ctrl)) {   ; WM_GETFONT
        previousFont := DllCall("SelectObject", "Ptr", hdc, "Ptr", hFont, "Ptr")
    }
    size := Buffer(8, 0)
    DllCall("GetTextExtentPoint32W", "Ptr", hdc, "WStr", text, "Int", StrLen(text), "Ptr", size)
    w := NumGet(size, 0, "Int")
    h := NumGet(size, 4, "Int")
    if previousFont {
        DllCall("SelectObject", "Ptr", hdc, "Ptr", previousFont, "Ptr")
    }
    DllCall("ReleaseDC", "Ptr", ctrl.Hwnd, "Ptr", hdc)
    return { w: w, h: h }
}

; Adds a marker label control to the overlay Gui: opaque background, bold
; high-contrast text, unthemed (themed statics ignore background colours).
AddMarkerLabelControl(gui) {
    global MARKER_LABEL_TEXT_COLOR
    gui.SetFont("s8 Bold")
    ctrl := gui.AddText("x0 y0 w10 h10 Hidden Center Background000000 c" MARKER_LABEL_TEXT_COLOR, "")
    gui.SetFont()
    DllCall("uxtheme\SetWindowTheme", "Ptr", ctrl.Hwnd, "WStr", "", "WStr", "")
    return ctrl
}

; Should labels be drawn right now, for a layer configured with `mode`?
ShouldShowMarkerLabels(mode) {
    global gOverlayHover
    if (mode = "always") {
        return true
    }
    if (mode = "never") {
        return false
    }
    ; "autohide": visible while playing, out of the way when the mouse is on
    ; the minimap — which is when you're inspecting the map itself.
    return !gOverlayHover
}

IsMarkerLabelMode(mode) {
    global MARKER_LABEL_MODES
    for m in MARKER_LABEL_MODES {
        if (m = mode) {
            return true
        }
    }
    return false
}

; Config value → a mode this build understands. "hover" was the same setting
; with the opposite sense (show only while hovering); it becomes "autohide".
NormalizeMarkerLabelMode(mode, fallback := "autohide") {
    mode := Trim(mode)
    if (mode = "hover") {
        return "autohide"
    }
    return IsMarkerLabelMode(mode) ? mode : fallback
}

MarkerLabelModeIndex(mode) {
    global MARKER_LABEL_MODES
    for i, m in MARKER_LABEL_MODES {
        if (m = mode) {
            return i
        }
    }
    return 1
}

; Flips label visibility for the first `usedCount` slots of a marker pool
; (array of {dot, label}) without repositioning anything — the positions are
; kept current by each layer's draw pass, so a hover change is just a toggle.
SetMarkerLabelsVisible(pool, usedCount, visible) {
    loop Min(usedCount, pool.Length) {
        entry := pool[A_Index]
        try entry.label.Visible := visible && (entry.label.Text != "")
    }
}

; Centres `text` over the dot at (dotX, dotY, dotSize) — coordinates relative
; to the Gui, i.e. already including MINIMAP_MAP_INSET — and keeps the box
; inside the map image, flipping below the dot when there's no room above.
; Positioning happens even when `visible` is false so that turning labels back
; on (mouse-over) is an instant toggle rather than a redraw.
PositionMarkerLabel(ctrl, text, dotX, dotY, dotSize, visible := true) {
    global MINIMAP_MAP_INSET, MARKER_LABEL_PAD_X
    dims := MeasureControlText(ctrl, text)
    w := dims.w + 2 * MARKER_LABEL_PAD_X
    h := dims.h + 2
    x := dotX + (dotSize // 2) - (w // 2)
    y := dotY - h - 1
    if (y < MINIMAP_MAP_INSET) {
        y := dotY + dotSize + 1          ; no room above — sit under the dot
    }
    x := Clamp(x, MINIMAP_MAP_INSET, MINIMAP_MAP_INSET + MinimapDisplayW() - w)
    y := Clamp(y, MINIMAP_MAP_INSET, MINIMAP_MAP_INSET + MinimapDisplayH() - h)
    ctrl.Text := text
    ctrl.Move(x, y, w, h)
    ctrl.Visible := visible
    if visible {
        ctrl.Redraw()
    }
}

; Tracks whether the cursor is over the overlay and publishes changes through
; the OnOverlayHover hook. Called from the marker timer, which only runs while
; the overlay is visible.
; ── Hover coordinate readout ─────────────────────────────────────
;
; The in-game coordinates under the cursor, shown in the corner of the minimap
; while the mouse is over it. Hovering already means "let me look at the map"
; (it is what hides the marker labels), so this is the moment the numbers are
; wanted — for calling a location out to someone, or for checking that a map's
; calibration is actually right.

; Created once per overlay Gui, alongside the marker pool controls.
EnsureCoordReadout() {
    global gGui, gCoordReadout, MARKER_LABEL_TEXT_COLOR
    if (IsObject(gCoordReadout) || !IsObject(gGui)) {
        return
    }
    gGui.SetFont("s8 Bold")
    gCoordReadout := gGui.AddText(
        "x0 y0 w10 h10 Hidden Center Background000000 c" MARKER_LABEL_TEXT_COLOR, "")
    gGui.SetFont()
    DllCall("uxtheme\SetWindowTheme", "Ptr", gCoordReadout.Hwnd, "WStr", "", "WStr", "")
}

; Dropped when the overlay Gui is rebuilt, same as the addon marker pools.
ResetCoordReadout() {
    global gCoordReadout
    gCoordReadout := 0
}

; Called from the marker timer. Reads the cursor, converts to raw game
; coordinates and parks the text in the bottom-left of the map.
UpdateCoordReadout() {
    global gGui, gCoordReadout, gOverlayHover, gShowHoverCoords, gCurrentMapName
    global MINIMAP_MAP_INSET, MARKER_LABEL_PAD_X

    if (!gShowHoverCoords || !gOverlayHover || !IsObject(gGui) || !gGui.Hwnd) {
        if IsObject(gCoordReadout)
            gCoordReadout.Visible := false
        return
    }
    EnsureCoordReadout()
    if !IsObject(gCoordReadout) {
        return
    }

    CoordMode("Mouse", "Screen")
    MouseGetPos(&mx, &my)
    pt := Buffer(8, 0)
    DllCall("user32\ClientToScreen", "Ptr", gGui.Hwnd, "Ptr", pt)
    relX := mx - NumGet(pt, 0, "Int") - MINIMAP_MAP_INSET
    relY := my - NumGet(pt, 4, "Int") - MINIMAP_MAP_INSET
    if (relX < 0 || relY < 0 || relX >= MinimapDisplayW() || relY >= MinimapDisplayH()) {
        gCoordReadout.Visible := false      ; over the border, not the map
        return
    }

    ; Calibration lives in base map space, so undo the user's display scale
    ; before inverting — the same asymmetry the drawing code has.
    scale := MinimapScaleFactor()
    world := OverlayPixelsToWorld(Round(relX / scale), Round(relY / scale), gCurrentMapName)
    if !world.ok {
        gCoordReadout.Visible := false      ; unusable calibration
        return
    }

    text := GameCoordText(world.x, world.y)
    dims := MeasureControlText(gCoordReadout, text)
    w := dims.w + 2 * MARKER_LABEL_PAD_X
    h := dims.h + 2
    gCoordReadout.Text := text
    gCoordReadout.Move(MINIMAP_MAP_INSET + 2,
        MINIMAP_MAP_INSET + MinimapDisplayH() - h - 2, w, h)
    gCoordReadout.Visible := true
    gCoordReadout.Redraw()
}

; ── Waypoint ─────────────────────────────────────────────────────
;
; Shift+click the minimap to mark a spot; the mark shows how far away it is and
; which way to walk. Deliberately not a POI: POIs are permanent, per-map and
; saved to disk, whereas this is "meet me here" — one at a time, never written
; anywhere, and gone when you leave the zone.
;
; Shift because the other gestures on the overlay are taken: Ctrl+drag moves the
; window, double-click re-centres it, right-click closes it, and a plain hover
; is what reveals marker labels.

EnsureWaypointControls() {
    global gGui, gWaypointDot, gWaypointLabel, WAYPOINT_COLOR
    if (!IsObject(gGui))
        return
    if !IsObject(gWaypointDot) {
        gWaypointDot := gGui.AddText("x0 y0 w1 h1 Hidden Background" WAYPOINT_COLOR, "")
        DllCall("uxtheme\SetWindowTheme", "Ptr", gWaypointDot.Hwnd, "WStr", "", "WStr", "")
    }
    if !IsObject(gWaypointLabel) {
        gWaypointLabel := AddMarkerLabelControl(gGui)
    }
}

ResetWaypointControls() {
    global gWaypointDot, gWaypointLabel
    gWaypointDot := 0
    gWaypointLabel := 0
}

ClearWaypoint() {
    global gWaypoint, gWaypointDot, gWaypointLabel
    gWaypoint := 0
    if IsObject(gWaypointDot)
        try gWaypointDot.Visible := false
    if IsObject(gWaypointLabel)
        try gWaypointLabel.Visible := false
}

; Places (or clears) the waypoint from a click in overlay client coordinates.
SetWaypointFromClientPoint(clientX, clientY) {
    global gWaypoint, gCurrentMapName, MINIMAP_MAP_INSET

    relX := clientX - MINIMAP_MAP_INSET
    relY := clientY - MINIMAP_MAP_INSET
    if (relX < 0 || relY < 0 || relX >= MinimapDisplayW() || relY >= MinimapDisplayH())
        return false                       ; the border, not the map

    scale := MinimapScaleFactor()
    world := OverlayPixelsToWorld(Round(relX / scale), Round(relY / scale), gCurrentMapName)
    if !world.ok {
        TrayTip("This map has no usable calibration, so a waypoint cannot be placed.",
            "Maps++", "Icon!")
        return true                        ; handled: don't fall through to drag
    }

    ; Clicking the existing waypoint again takes it away, so setting and
    ; clearing are the same gesture and nothing new has to be learned.
    if IsObject(gWaypoint) {
        prev := WorldToOverlayPixels(gWaypoint.rawX, gWaypoint.rawY, gCurrentMapName)
        size := Max(6, MinimapMarkerSize())
        if (Abs(Round(prev.x * scale) - relX) <= size
            && Abs(Round(prev.y * scale) - relY) <= size) {
            ClearWaypoint()
            return true
        }
    }

    gWaypoint := { rawX: world.x, rawY: world.y, mapName: gCurrentMapName }
    return true
}

; Draws the waypoint and its distance/bearing. Called from the marker timer, so
; the readout tracks the player rather than freezing at the moment of the click.
UpdateWaypoint() {
    global gWaypoint, gWaypointDot, gWaypointLabel, gGui, gOverlayVisible
    global gCurrentMapName, gLastRawX, gLastRawY, MINIMAP_MAP_INSET

    if (!gOverlayVisible || !IsObject(gGui) || !gGui.Hwnd || !IsObject(gWaypoint)) {
        if IsObject(gWaypointDot)
            try gWaypointDot.Visible := false
        if IsObject(gWaypointLabel)
            try gWaypointLabel.Visible := false
        return
    }
    ; A waypoint belongs to the zone it was dropped in.
    if (gWaypoint.mapName != gCurrentMapName) {
        ClearWaypoint()
        return
    }

    EnsureWaypointControls()
    if (!IsObject(gWaypointDot) || !IsObject(gWaypointLabel))
        return

    scale := MinimapScaleFactor()
    size := Max(5, MinimapMarkerSize() - 2)
    pos := WorldToOverlayPixels(gWaypoint.rawX, gWaypoint.rawY, gCurrentMapName)
    px := Clamp(Round(pos.x * scale), 0, MinimapDisplayW() - size)
    py := Clamp(Round(pos.y * scale), 0, MinimapDisplayH() - size)

    gWaypointDot.Move(px + MINIMAP_MAP_INSET, py + MINIMAP_MAP_INSET, size, size)
    gWaypointDot.Visible := true
    gWaypointDot.Redraw()

    PositionMarkerLabel(gWaypointLabel, WaypointBearingText(),
        px + MINIMAP_MAP_INSET, py + MINIMAP_MAP_INSET, size, true)
}

; "42 SW" — how far the waypoint is and which way it lies.
;
; Measured in the coordinates the game shows the player, not raw memory units:
; raw X and Y are scaled differently (÷16 and ÷8), so a distance computed on raw
; values would be wrong by a factor of two along one axis.
WaypointBearingText() {
    global gWaypoint, gLastRawX, gLastRawY
    dx := GameCoordX(gWaypoint.rawX) - GameCoordX(gLastRawX)
    dy := GameCoordY(gWaypoint.rawY) - GameCoordY(gLastRawY)
    dist := Round(Sqrt(dx * dx + dy * dy))
    if (dist = 0)
        return "here"
    return dist " " _CompassPoint(dx, dy)
}

; Eight-point compass for a delta in map space. The minimap is drawn with y
; increasing downward, so up on the image is north — this describes the picture
; the player is looking at, which is the only frame that helps them walk.
_CompassPoint(dx, dy) {
    static POINTS := ["E", "SE", "S", "SW", "W", "NW", "N", "NE"]
    if (dx = 0 && dy = 0)
        return ""
    ; ATan2 via ATan, avoiding a divide by zero on a purely vertical delta.
    angle := (dx = 0) ? (dy > 0 ? 90 : -90) : ATan(dy / dx) * 180 / 3.14159265358979
    if (dx < 0)
        angle += 180
    ; Bucket into 45-degree sectors starting at due east.
    idx := Mod(Round(angle / 45) + 8, 8) + 1
    return POINTS[idx]
}

UpdateOverlayHoverState() {
    global gGui, gOverlayHover
    hovering := false
    if (IsObject(gGui) && gGui.Hwnd) {
        ; The window under the cursor, so another window covering the minimap
        ; correctly counts as "not hovering".
        MouseGetPos(, , &winUnderCursor)
        hovering := (winUnderCursor = gGui.Hwnd)
    }
    if (hovering = gOverlayHover) {
        return
    }
    gOverlayHover := hovering
    FireAddonHook("OnOverlayHover", hovering)
}

; ── Overlay dragging ─────────────────────────────────────────────
; The overlay is -Caption +E0x08000000 (WS_EX_NOACTIVATE), so Windows won't
; move it for us. Reporting HTCAPTION for its client area makes DefWindowProc
; run its own move loop — the Picture/Text children return HTTRANSPARENT, so
; the hit test reaches the Gui itself. WM_EXITSIZEMOVE then converts wherever
; the user dropped it back into OffsetX/OffsetY and persists that.
;
; Only while Ctrl is held: mousing over the minimap is how marker labels are
; revealed, so an unmodified click there must not start dragging the window.

RegisterOverlayMouseHandlers() {
    OnMessage(0x0084, _Overlay_OnNcHitTest)      ; WM_NCHITTEST
    OnMessage(0x0231, _Overlay_OnEnterSizeMove)  ; WM_ENTERSIZEMOVE
    OnMessage(0x0232, _Overlay_OnExitSizeMove)   ; WM_EXITSIZEMOVE
    OnMessage(0x0201, _Overlay_OnLButtonDown)    ; WM_LBUTTONDOWN
    OnMessage(0x0203, _Overlay_OnLButtonDblClk)  ; WM_LBUTTONDBLCLK
}

; Puts the overlay back where it starts out: centred on the game's client area.
; Bound to a double-click on the overlay itself, so an overlay dragged somewhere
; awkward (or off a display that's since been unplugged) is one gesture away
; from being findable again.
ResetOverlayPosition() {
    global gMinimapAnchor, gMinimapOffsetX, gMinimapOffsetY, gGui
    if (gMinimapAnchor = "Center" && gMinimapOffsetX = 0 && gMinimapOffsetY = 0) {
        return  ; already centred — don't rewrite the config for nothing
    }
    gMinimapAnchor := "Center"
    gMinimapOffsetX := 0
    gMinimapOffsetY := 0
    SaveMinimapConfig()
    if (IsObject(gGui) && gGui.Hwnd) {
        pos := GetOverlayPositionForGameWindow()
        gGui.Move(pos.x, pos.y)
    }
}

_Overlay_IsOverlayHwnd(hwnd) {
    global gGui
    return IsObject(gGui) && gGui.Hwnd && (hwnd = gGui.Hwnd)
}

_Overlay_OnNcHitTest(wParam, lParam, msg, hwnd) {
    static HTCAPTION := 2
    if !_Overlay_IsOverlayHwnd(hwnd) {
        return  ; not ours — let default processing run
    }
    if !GetKeyState("Ctrl") {
        return  ; plain hover/click — no drag
    }
    return HTCAPTION
}

_Overlay_OnEnterSizeMove(wParam, lParam, msg, hwnd) {
    global gOverlayDragging
    if !_Overlay_IsOverlayHwnd(hwnd) {
        return
    }
    gOverlayDragging := true
}

; Double-click to re-centre. Whether Windows delivers WM_LBUTTONDBLCLK depends
; on the window class having CS_DBLCLKS, so the pair of clicks is also timed
; here — with CS_DBLCLKS the second click arrives as DBLCLK rather than DOWN,
; so exactly one of the two paths fires.
_Overlay_OnLButtonDown(wParam, lParam, msg, hwnd) {
    global gOverlayLastClickTick
    if !_Overlay_IsOverlayHwnd(hwnd) {
        return
    }
    ; Shift+click drops or lifts the waypoint. Handled before the double-click
    ; timing below so two quick waypoint clicks cannot also re-centre the
    ; overlay, and the tick is left alone so it does not poison the next
    ; genuine double-click.
    if GetKeyState("Shift") {
        ; lParam packs the click point as two signed 16-bit client coords.
        cx := lParam & 0xFFFF
        cy := (lParam >> 16) & 0xFFFF
        if (cx > 0x7FFF)
            cx -= 0x10000
        if (cy > 0x7FFF)
            cy -= 0x10000
        if SetWaypointFromClientPoint(cx, cy) {
            UpdateWaypoint()
            return 0
        }
        return
    }
    now := A_TickCount
    if (gOverlayLastClickTick && (now - gOverlayLastClickTick) <= DllCall("GetDoubleClickTime", "UInt")) {
        gOverlayLastClickTick := 0
        ResetOverlayPosition()
        return 0
    }
    gOverlayLastClickTick := now
}

_Overlay_OnLButtonDblClk(wParam, lParam, msg, hwnd) {
    global gOverlayLastClickTick
    if !_Overlay_IsOverlayHwnd(hwnd) {
        return
    }
    gOverlayLastClickTick := 0
    ResetOverlayPosition()
    return 0
}

_Overlay_OnExitSizeMove(wParam, lParam, msg, hwnd) {
    global gOverlayDragging, gGui, gMinimapOffsetX, gMinimapOffsetY
    if !_Overlay_IsOverlayHwnd(hwnd) {
        return
    }
    gOverlayDragging := false
    try {
        WinGetPos(&winX, &winY, , , "ahk_id " gGui.Hwnd)
    } catch {
        return
    }
    base := GetOverlayAnchorPosition()
    gMinimapOffsetX := winX - base.x
    gMinimapOffsetY := winY - base.y
    SaveMinimapConfig()
}

; ── Utilities ────────────────────────────────────────────────────

Clamp(value, minValue, maxValue) {
    if (value < minValue) {
        return minValue
    }
    if (value > maxValue) {
        return maxValue
    }
    return value
}

IsGameOrOverlayActive() {
    global gGui
    if WinActive(GAME_WIN_FILTER) {
        return true
    }
    if IsObject(gGui) && gGui.Hwnd {
        if WinActive("ahk_id " gGui.Hwnd) {
            return true
        }
    }
    return false
}

EnsureMarkerControl() {
    global gGui, gMarkerDot, MARKER_PNG
    if IsObject(gMarkerDot) {
        return
    }
    if !IsObject(gGui) {
        return
    }
    if !FileExist(MARKER_PNG) {
        return
    }
    ; Added after gPic so the marker draws above the map image.
    size := MinimapMarkerSize()
    gMarkerDot := gGui.AddPicture("x0 y0 w" size " h" size " Hidden", MARKER_PNG)
}

; ── Launcher ─────────────────────────────────────────────────────

; Resolves the game executable path.
; Priority: A_ScriptDir\main.exe → config.ini GamePath → file picker prompt.
LoadLauncherConfig() {
    global gGamePath, gGameArgs, gLaunchOnStartup, gMultiClientCount, gMultiClientDelay, CONFIG_INI, PROCESS_EXE
    global gPrimaryMonitorOverride, gSecondaryMonitorOverride
    global gPrimaryLaunchLayout, gSecondaryLaunchLayout, LAUNCH_LAYOUT_DEFAULT
    global gInterfaceMode, gAccentScheme, gVersionCheckEnabled

    ; 1. Check for main.exe next to the script (same directory install).
    localExe := A_ScriptDir "\" PROCESS_EXE
    if FileExist(localExe) {
        gGamePath := localExe
    }

    ; 2. Read config.ini overrides (GamePath only used if local exe wasn't found).
    if FileExist(CONFIG_INI) {
        if (gGamePath = "") {
            cfgPath := Trim(IniRead(CONFIG_INI, "Launcher", "GamePath", ""))
            if (cfgPath != "" && FileExist(cfgPath)) {
                gGamePath := cfgPath
            }
        }
        gGameArgs := Trim(IniRead(CONFIG_INI, "Launcher", "GameArgs", ""))
        startupVal := Trim(IniRead(CONFIG_INI, "Launcher", "LaunchOnStartup", "__MISSING__"))
        if (startupVal != "__MISSING__") {
            gLaunchOnStartup := (startupVal = "1")
        }
        cnt := Trim(IniRead(CONFIG_INI, "Launcher", "MultiClientCount", "5"))
        if (IsInteger(cnt) && Integer(cnt) >= 1)
            gMultiClientCount := Integer(cnt)
        dly := Trim(IniRead(CONFIG_INI, "Launcher", "MultiClientDelay", "0"))
        if (IsInteger(dly) && Integer(dly) >= 0)
            gMultiClientDelay := Integer(dly)
        ; Monitor overrides (0 = auto). Validation against the live monitor count
        ; happens at resolve time so a display that is temporarily disconnected
        ; doesn't permanently discard the user's choice.
        primMon := Trim(IniRead(CONFIG_INI, "Launcher", "PrimaryMonitor", "0"))
        if (IsInteger(primMon) && Integer(primMon) >= 0)
            gPrimaryMonitorOverride := Integer(primMon)
        secMon := Trim(IniRead(CONFIG_INI, "Launcher", "SecondaryMonitor", "0"))
        if (IsInteger(secMon) && Integer(secMon) >= 0)
            gSecondaryMonitorOverride := Integer(secMon)
        gPrimaryLaunchLayout := Trim(IniRead(CONFIG_INI, "Launcher", "PrimaryLaunchLayout", LAUNCH_LAYOUT_DEFAULT))
        gSecondaryLaunchLayout := Trim(IniRead(CONFIG_INI, "Launcher", "SecondaryLaunchLayout", LAUNCH_LAYOUT_DEFAULT))

        gVersionCheckEnabled :=
            (Trim(IniRead(CONFIG_INI, "Launcher", "VersionCheck", "1")) != "0")

        uiMode := StrLower(Trim(IniRead(CONFIG_INI, "UI", "Mode", "webview")))
        gInterfaceMode := (uiMode = "native") ? "native" : "webview"
        SetAccentScheme(IniRead(CONFIG_INI, "UI", "AccentScheme", "amber"), false)
    }

    ; 3. Still no path — ask the user to locate it.
    if (gGamePath = "") {
        PromptForGamePath()
    }
}

NormalizeAccentScheme(value) {
    scheme := StrLower(Trim(String(value)))
    return (scheme = "blue" || scheme = "green") ? scheme : "amber"
}

AccentSchemeMinimapColor(scheme) {
    switch NormalizeAccentScheme(scheme) {
        case "blue":
            return "2d6fa6"
        case "green":
            return "27794e"
        default:
            return "9c7c10"
    }
}

SetAccentScheme(scheme, persist := true) {
    global gAccentScheme, MINIMAP_COLOR_GOLD, CONFIG_INI
    normalized := NormalizeAccentScheme(scheme)
    changed := normalized != gAccentScheme
    gAccentScheme := normalized
    MINIMAP_COLOR_GOLD := AccentSchemeMinimapColor(normalized)
    if persist
        IniWrite(normalized, CONFIG_INI, "UI", "AccentScheme")
    return changed
}

; Appends the saved scheme before navigation so every WebView paints with the
; correct tokens on its first frame. theme.js also mirrors it to localStorage,
; allowing already-open panels to update when Settings saves a new choice.
UiPageUrl(page) {
    global gAccentScheme
    separator := InStr(page, "?") ? "&" : "?"
    return page separator "accent=" NormalizeAccentScheme(gAccentScheme)
}

IsWebViewInterface() {
    global gInterfaceMode
    return gInterfaceMode = "webview"
}

IsNativeInterface() {
    return !IsWebViewInterface()
}

; Changing frontend affects startup message handlers and prewarmed windows, so
; apply it with a reload rather than trying to replace live controllers.
SetInterfaceMode(mode, *) {
    global gInterfaceMode, CONFIG_INI
    mode := (StrLower(mode) = "native") ? "native" : "webview"
    if (mode = gInterfaceMode)
        return
    IniWrite(mode, CONFIG_INI, "UI", "Mode")
    gInterfaceMode := mode
    Reload()
}

; Opens a file-picker dialog for the user to locate the game executable.
; Saves the selected path to config.ini for future runs.
PromptForGamePath() {
    global gGamePath, PROCESS_EXE

    selected := FileSelect(
        1,
        A_ScriptDir,
        "Locate " PROCESS_EXE " (game executable)",
        "Executables (*.exe)"
    )
    if (selected = "") {
        TrayTip("No game path selected — launcher disabled.", "Maps++", "Iconx")
        return
    }
    if !FileExist(selected) {
        TrayTip("Selected file does not exist.", "Maps++", "Iconx")
        return
    }
    gGamePath := selected
    SaveGamePathToConfig(selected)
    TrayTip("Game path set:`n" selected, "Maps++", "Iconi")
}

; The command Windows runs at login to start Maps++. For a compiled build that's
; just the exe; for a loose script it's the AutoHotkey interpreter plus the script.
GetStartupCommand() {
    if A_IsCompiled
        return '"' A_ScriptFullPath '"'
    return '"' A_AhkPath '" "' A_ScriptFullPath '"'
}

; True when the per-user "run on login" registry value exists.
IsRunOnStartupEnabled() {
    global STARTUP_RUN_KEY, STARTUP_RUN_NAME
    try {
        RegRead(STARTUP_RUN_KEY, STARTUP_RUN_NAME)
        return true
    }
    return false
}

; Enables or disables launching Maps++ when Windows starts. Enabling (re)writes
; the value to the current executable path, so it survives the app being moved.
SetRunOnStartup(enabled) {
    global STARTUP_RUN_KEY, STARTUP_RUN_NAME
    try {
        if (enabled)
            RegWrite(GetStartupCommand(), "REG_SZ", STARTUP_RUN_KEY, STARTUP_RUN_NAME)
        else if IsRunOnStartupEnabled()
            RegDelete(STARTUP_RUN_KEY, STARTUP_RUN_NAME)
    } catch as err {
        TrayTip("Could not update Windows startup setting:`n" err.Message, "Maps++", "Iconx")
    }
}

; Persists the game executable path into config.ini.
SaveGamePathToConfig(path) {
    global CONFIG_INI
    IniWrite(path, CONFIG_INI, "Launcher", "GamePath")
}

GetSecondaryMonitorIndex() {
    primary := MonitorGetPrimary()
    Loop MonitorGetCount() {
        if A_Index != primary
            return A_Index
    }
    return primary
}

ResolveMonitorForHotkey(which) {
    global gPrimaryMonitorOverride, gSecondaryMonitorOverride
    count := MonitorGetCount()
    if (which = "secondary") {
        ; Honor a pinned secondary display when it is currently connected;
        ; otherwise fall back to the first non-primary monitor.
        if (gSecondaryMonitorOverride >= 1 && gSecondaryMonitorOverride <= count)
            return gSecondaryMonitorOverride
        return GetSecondaryMonitorIndex()
    }
    if (gPrimaryMonitorOverride >= 1 && gPrimaryMonitorOverride <= count)
        return gPrimaryMonitorOverride
    return MonitorGetPrimary()
}

; Returns the monitor index whose bounds contain a screen point. Cursor-anchored
; popups clamp against this rather than the primary display, or they get yanked
; onto the wrong monitor.
GetMonitorIndexAtPoint(x, y) {
    Loop MonitorGetCount() {
        MonitorGet(A_Index, &ml, &mt, &mr, &mb)
        if (x >= ml && x < mr && y >= mt && y < mb)
            return A_Index
    }
    return MonitorGetPrimary()
}

; Returns the monitor index whose bounds contain the window center.
GetWindowMonitorIndex(hwnd) {
    WinGetPos(&x, &y, &w, &h, "ahk_id " hwnd)
    return GetMonitorIndexAtPoint(x + w // 2, y + h // 2)
}

FilterWindowsOnMonitor(windows, monIdx) {
    result := []
    for hwnd in windows {
        if GetWindowMonitorIndex(hwnd) = monIdx
            result.Push(hwnd)
    }
    return result
}

GetTopLevelGameWindows() {
    global GAME_WIN_FILTER
    result := []
    for hwnd in WinGetList(GAME_WIN_FILTER) {
        if DllCall("GetParent", "Ptr", hwnd, "Ptr") != 0
            continue
        if DllCall("GetWindow", "Ptr", hwnd, "UInt", 4, "Ptr") != 0  ; GW_OWNER = 4
            continue
        result.Push(hwnd)
    }
    return result
}

WaitForNewGameWindow(beforeHwnds, timeoutMs := 15000) {
    beforeSet := Map()
    for hwnd in beforeHwnds
        beforeSet[hwnd] := true
    deadline := A_TickCount + timeoutMs
    while (A_TickCount < deadline) {
        for hwnd in GetTopLevelGameWindows() {
            if !beforeSet.Has(hwnd)
                return hwnd
        }
        Sleep 200
    }
    return 0
}

CenterWindowOnMonitor(hwnd, monIdx) {
    MonitorGetWorkArea(monIdx, &wl, &wt, &wr, &wb)
    WinGetPos(, , &winW, &winH, "ahk_id " hwnd)
    sw := wr - wl
    sh := wb - wt
    x := wl + (sw - winW) // 2
    y := wt + (sh - winH) // 2
    WinMove(x, y, , , "ahk_id " hwnd)
}

; Launches a new game client instance.
; Uses the game executable's parent directory as the working directory.
; monitorWhich: "primary" (default) or "secondary" — centers the new window on that display.
LaunchGameInstance(monitorWhich := "primary") {
    global gGamePath, gGameArgs

    if (gGamePath = "" || !FileExist(gGamePath)) {
        settingsShortcut := GetHotkeyDisplay("openSettings")
        settingsHint := (settingsShortcut != "") ? "Settings (" settingsShortcut ")" : "Settings from the tray menu"
        TrayTip("Game path not configured or file missing.`nOpen " settingsHint " → Launcher → Browse…", "Maps++", "Iconx")
        return
    }

    before := GetTopLevelGameWindows()
    workDir := ""
    SplitPath(gGamePath, , &workDir)
    try {
        if (gGameArgs != "") {
            Run('"' gGamePath '" ' gGameArgs, workDir)
        } else {
            Run('"' gGamePath '"', workDir)
        }
    } catch as err {
        TrayTip("Failed to launch game:`n" err.Message, "Maps++", "Iconx")
        return
    }

    monIdx := ResolveMonitorForHotkey(monitorWhich)
    if (monitorWhich = "secondary" && monIdx = MonitorGetPrimary() && MonitorGetCount() = 1) {
        TrayTip("Only one display — centering on primary.", "Maps++", "Iconi")
    }

    hwnd := WaitForNewGameWindow(before)
    if !hwnd {
        TrayTip("Game launched but window not detected.", "Maps++", "Iconx")
        return
    }

    CenterWindowOnMonitor(hwnd, monIdx)
    TrayTip("Game instance launched.", "Maps++", "Iconi")
}

; Returns the Func object for a globally-defined function name, or "" when no
; such function exists in this build. Used so core code can call into an addon
; (e.g. WindowLayout) without a static reference that would break variants that
; don't bundle that addon. The dynamic deref avoids load-time #Warn noise.
GetFuncByName(name) {
    try
        return %name%
    catch
        return ""
}

; True only when an addon is both bundled in this build and currently enabled.
; Core launch actions use this before dynamically calling WindowLayout, so the
; Battle variant and users who disable the addon both keep a clean fallback.
IsAddonEnabled(addonName) {
    global gAddonHooks, gDisabledAddons
    installed := false
    for _, am in gAddonHooks {
        if (am.Has("name") && am["name"] = addonName) {
            installed := true
            break
        }
    }
    return installed && !(gDisabledAddons.Has(addonName) && gDisabledAddons[addonName])
}

; Resolves a launch target's layout preference to a concrete preset/custom name.
; A missing pinned layout falls back to the current default instead of silently
; skipping organisation after the user has asked for it.
ResolveLaunchLayoutName(monitorWhich) {
    global gPrimaryLaunchLayout, gSecondaryLaunchLayout, LAUNCH_LAYOUT_DEFAULT
    if !IsAddonEnabled("WindowLayout")
        return ""

    preference := (monitorWhich = "secondary") ? gSecondaryLaunchLayout : gPrimaryLaunchLayout
    if (preference = "")
        return ""

    defaultFn := GetFuncByName("_WindowLayout_GetDefaultLayoutName")
    defaultName := defaultFn ? defaultFn() : "Grid2x2"
    if (preference = LAUNCH_LAYOUT_DEFAULT)
        return defaultName

    namesFn := GetFuncByName("_WindowLayout_AllLayoutNames")
    if namesFn {
        for name in namesFn() {
            if (name = preference)
                return preference
        }
    }
    return defaultName
}

; Launches the configured client count onto the requested display, then applies
; that display's optional layout preference. Every new window is centered on the
; target as it appears, ensuring the layout pass sees the complete launch batch.
; count defaults to gMultiClientCount and is exposed for internal one-off callers.
LaunchConfiguredClients(monitorWhich := "primary", count := 0) {
    global gGamePath, gGameArgs, gMultiClientCount, gMultiClientDelay

    if (count < 1)
        count := gMultiClientCount

    if (gGamePath = "" || !FileExist(gGamePath)) {
        settingsShortcut := GetHotkeyDisplay("openSettings")
        settingsHint := (settingsShortcut != "") ? "Settings (" settingsShortcut ")" : "Settings from the tray menu"
        TrayTip("Game path not configured or file missing.`nOpen " settingsHint " → Launcher → Browse…", "Maps++", "Iconx")
        return
    }

    workDir := ""
    SplitPath(gGamePath, , &workDir)
    monIdx := ResolveMonitorForHotkey(monitorWhich)
    layoutName := ResolveLaunchLayoutName(monitorWhich)

    ; Snapshot existing windows, then fire the launches so the clients' startup
    ; time overlaps instead of being serialized one-wait-per-client. An optional
    ; per-launch delay paces the burst for games that dislike simultaneous starts.
    before := Map()
    for hwnd in GetTopLevelGameWindows()
        before[hwnd] := true

    launchCmd := (gGameArgs != "") ? '"' gGamePath '" ' gGameArgs : '"' gGamePath '"'
    started := 0
    Loop count {
        try {
            Run(launchCmd, workDir)
            started++
        } catch as err {
            TrayTip("Failed to launch game:`n" err.Message, "Maps++", "Iconx")
            break
        }
        if (A_Index < count && gMultiClientDelay > 0)
            Sleep gMultiClientDelay
    }
    if (started = 0)
        return

    ; Collect new windows as they appear, centering each onto the target monitor
    ; once so the layout pass picks them all up.
    seen := Map()
    launched := []
    deadline := A_TickCount + 20000
    while (launched.Length < started && A_TickCount < deadline) {
        for hwnd in GetTopLevelGameWindows() {
            if before.Has(hwnd) || seen.Has(hwnd)
                continue
            seen[hwnd] := true
            CenterWindowOnMonitor(hwnd, monIdx)
            launched.Push(hwnd)
        }
        if (launched.Length < started)
            Sleep 100
    }

    if (launched.Length = 0) {
        TrayTip("No new game windows detected.", "Maps++", "Iconx")
        return
    }

    applyFn := (layoutName != "") ? GetFuncByName("_WindowLayout_ApplyNamed") : ""
    if applyFn {
        applyFn(layoutName, monIdx)
        TrayTip(launched.Length " client(s) launched and arranged.", "Maps++", "Iconi")
    } else {
        TrayTip(launched.Length " client(s) launched and centered.", "Maps++", "Iconi")
    }
}

; Persists all [Launcher] settings into config.ini from the current globals.
SaveLauncherConfig() {
    global gGamePath, gGameArgs, gLaunchOnStartup, gMultiClientCount, gMultiClientDelay
    global gPrimaryMonitorOverride, gSecondaryMonitorOverride, CONFIG_INI
    global gPrimaryLaunchLayout, gSecondaryLaunchLayout, gVersionCheckEnabled
    IniWrite(gVersionCheckEnabled ? "1" : "0", CONFIG_INI, "Launcher", "VersionCheck")
    IniWrite(gGamePath, CONFIG_INI, "Launcher", "GamePath")
    IniWrite(gGameArgs, CONFIG_INI, "Launcher", "GameArgs")
    IniWrite(gLaunchOnStartup ? "1" : "0", CONFIG_INI, "Launcher", "LaunchOnStartup")
    IniWrite(gMultiClientCount, CONFIG_INI, "Launcher", "MultiClientCount")
    IniWrite(gMultiClientDelay, CONFIG_INI, "Launcher", "MultiClientDelay")
    IniWrite(gPrimaryMonitorOverride, CONFIG_INI, "Launcher", "PrimaryMonitor")
    IniWrite(gSecondaryMonitorOverride, CONFIG_INI, "Launcher", "SecondaryMonitor")
    IniWrite(gPrimaryLaunchLayout, CONFIG_INI, "Launcher", "PrimaryLaunchLayout")
    IniWrite(gSecondaryLaunchLayout, CONFIG_INI, "Launcher", "SecondaryLaunchLayout")
}

; Reads the game-state integer from a specific window's process.
; stateRva: the resolved RVA of GAME_STATE_OFFSET (same across instances of one build).
; Returns the state value, or -1 if the process or memory could not be read.
ReadGameStateForWindow(hwnd, stateRva) {
    pid := 0
    try pid := WinGetPID("ahk_id " hwnd)
    if !pid
        return -1
    handle := DllCall("OpenProcess", "UInt", 0x0010 | 0x0400, "Int", 0, "UInt", pid, "Ptr")
    if !handle
        return -1
    result := -1
    modBase := GetModuleBaseAddress(handle, PROCESS_EXE)
    if modBase {
        buf := Buffer(4, 0)
        ok := DllCall("ReadProcessMemory",
            "Ptr", handle,
            "Ptr", modBase + stateRva,
            "Ptr", buf.Ptr,
            "UPtr", 4, "UPtr*", 0, "Int")
        if ok
            result := NumGet(buf, 0, "Int")
    }
    DllCall("CloseHandle", "Ptr", handle)
    return result
}

; Sends Enter to each game client whose game state is below GAME_STATE_READY,
; repeating until every client reaches that state (or a timeout fires). Used to
; click through intro/login prompts on freshly launched clients.
SendEnterUntilReady() {
    SetKeyDelay 0, 100
    static GAME_STATE_READY := 5
    rva := GetResolvedOffset("GAME_STATE_OFFSET")
    deadline := A_TickCount + 30000

    Loop {
        windows := GetTopLevelGameWindows()
        if (windows.Length = 0) {
            TrayTip("No game windows found.", "Maps++", "Iconx")
            return
        }
        pending := 0
        for hwnd in windows {
            state := ReadGameStateForWindow(hwnd, rva)
            if (state < 0 || state >= GAME_STATE_READY)  ; read failed or already ready
                continue
            pending++
            try ControlSend("{Enter}", , "ahk_id " hwnd)
        }
        if (pending = 0)
            break
        if (A_TickCount >= deadline) {
            TrayTip("Timed out — " pending " client(s) still not ready.", "Maps++", "Iconx")
            return
        }
        Sleep 100
    }
    TrayTip("All clients ready (game state " GAME_STATE_READY "+).", "Maps++", "Iconi")
}

; Returns the number of windows matching the game process name.
CountGameInstances() {
    global GAME_WIN_FILTER
    try {
        ids := WinGetList(GAME_WIN_FILTER)
        return ids.Length
    } catch {
        return 0
    }
}

; ── Client snapshots ─────────────────────────────────────────────
; A single poll of every running client, shared by every addon that needs
; per-client state. Before this each addon opened the game process itself on
; its own tick (Discord RPC did it twice every 1.5 s, re-reading the PE .text
; section each time), so handle churn multiplied with each new feature.

; Cached {handle, modBase} per PID. Deliberately separate from the single-handle
; cache used by the 60 ms marker path (gCachedProcessHandle) — that hot path is
; left untouched.
GetClientProcess(pid) {
    global gClientHandles, PROCESS_EXE
    if gClientHandles.Has(pid) {
        return gClientHandles[pid]
    }
    handle := DllCall("OpenProcess", "UInt", 0x0010 | 0x0400, "Int", 0, "UInt", pid, "Ptr")
    if !handle {
        return { ok: false }
    }
    modBase := GetModuleBaseAddress(handle, PROCESS_EXE)
    if !modBase {
        DllCall("CloseHandle", "Ptr", handle)
        return { ok: false }
    }
    entry := { ok: true, handle: handle, modBase: modBase }
    gClientHandles[pid] := entry
    return entry
}

ReleaseClientProcess(pid) {
    global gClientHandles
    if !gClientHandles.Has(pid) {
        return
    }
    try DllCall("CloseHandle", "Ptr", gClientHandles[pid].handle)
    gClientHandles.Delete(pid)
}

ReleaseAllClientProcesses() {
    global gClientHandles
    for pid, _ in gClientHandles.Clone() {
        ReleaseClientProcess(pid)
    }
}

; Reads `size` bytes at modBase+rva from a client, or 0 when the read fails.
ReadClientBuffer(entry, rva, size) {
    buf := Buffer(size, 0)
    ok := DllCall("ReadProcessMemory",
        "Ptr", entry.handle,
        "Ptr", entry.modBase + rva,
        "Ptr", buf.Ptr,
        "UPtr", size,
        "UPtr*", 0,
        "Int")
    return ok ? buf : 0
}

; "MAP301.map" / "MAP301.jpg" / "MAP301" → "MAP301". Snapshots carry the map id
; without an extension; the overlay tracks the image filename.
MapIdFromName(name) {
    return RegExReplace(Trim(name), "i)\.(map|jpg|jpeg|png)$", "")
}

; Bounded stand-in for WinGetTitle/GetWindowText: that call sends a plain,
; timeout-less WM_GETTEXT to the target window, which never returns if the
; target's thread stops pumping messages (a hitch, a stall, a crash that
; leaves the window alive) — freezing this whole single-threaded app. This
; caps the wait instead, exactly like the HTTP timeouts and WinWaitActive
; deadline elsewhere in the app. SMTO_ABORTIFHUNG returns immediately once
; Windows has already flagged the window as not-responding; the explicit
; timeoutMs covers the window before that flag is set.
SafeWindowTitle(hwnd, timeoutMs := 200) {
    static WM_GETTEXT := 0x000D
    static SMTO_ABORTIFHUNG := 0x0002
    if (!hwnd || !DllCall("IsWindow", "Ptr", hwnd)) {
        return ""
    }
    buf := Buffer(512, 0)   ; 256 WCHARs
    result := 0
    ok := DllCall("SendMessageTimeoutW",
        "Ptr", hwnd, "UInt", WM_GETTEXT, "Ptr", 256, "Ptr", buf,
        "UInt", SMTO_ABORTIFHUNG, "UInt", timeoutMs, "Ptr*", &result, "Ptr")
    if (!ok) {
        return ""   ; timed out, hung, or failed — same fallback as the old catch
    }
    return StrGet(buf, "UTF-16")
}

; The character name is whatever sits between the last ": " and " ID" in the
; window title. The prefix varies with the server/patch — "Behemoth: Name ID 5"
; and "MythWar … [ Local Server: Name ID 16 ]" are both seen — so match on the
; ": … ID" shape rather than on any particular prefix.
CharacterNameFromWindow(hwnd) {
    title := SafeWindowTitle(hwnd)
    if RegExMatch(title, ".*:\s+(.+?)\s+ID\b", &m) {
        return Trim(m[1])
    }
    return ""
}

; ── Battle actors ────────────────────────────────────────────────
;
; The combatant array described in variables.ahk. One ReadProcessMemory covers
; every location — the records are strided, so reading them individually would
; be 20 calls for 9 KB that arrives in one.

; Every location up to `slotCount`, as raw records carrying BOTH field blocks.
; Choosing between the blocks is deliberately left to the caller so the
; diagnostic can show them side by side.
ReadBattleActors(proc, slotCount) {
    global BATTLE_ACTOR_STRIDE, BATTLE_ACTOR_RECORD_BYTES, BATTLE_PARTY_SLOTS
    global BATTLE_ACTOR_BLOCK_A, BATTLE_ACTOR_BLOCK_B
    global BATTLE_F_MAXHP, BATTLE_F_HP, BATTLE_F_MAXMP, BATTLE_F_MP
    global BATTLE_F_ACTOR_ID, BATTLE_F_ACTIVE

    if (slotCount < 1)
        return []
    span := (slotCount - 1) * BATTLE_ACTOR_STRIDE + BATTLE_ACTOR_RECORD_BYTES
    buf := ReadClientBuffer(proc, GetResolvedOffset("BATTLE_ACTOR_BASE"), span)
    if !buf
        return []

    out := []
    Loop slotCount {
        loc := A_Index - 1
        o := loc * BATTLE_ACTOR_STRIDE
        out.Push({
            loc:     loc,
            isPet:   Mod(loc, 2) = 1,
            isEnemy: loc >= BATTLE_PARTY_SLOTS,
            owner:   (Mod(loc, 2) = 1) ? loc - 1 : loc,
            id:      NumGet(buf, o + BATTLE_F_ACTOR_ID, "Int"),
            active:  NumGet(buf, o + BATTLE_F_ACTIVE, "Int"),
            a: {
                maxHp: NumGet(buf, o + BATTLE_ACTOR_BLOCK_A + BATTLE_F_MAXHP, "Int"),
                hp:    NumGet(buf, o + BATTLE_ACTOR_BLOCK_A + BATTLE_F_HP,    "Int"),
                maxMp: NumGet(buf, o + BATTLE_ACTOR_BLOCK_A + BATTLE_F_MAXMP, "Int"),
                mp:    NumGet(buf, o + BATTLE_ACTOR_BLOCK_A + BATTLE_F_MP,    "Int")
            },
            b: {
                maxHp: NumGet(buf, o + BATTLE_ACTOR_BLOCK_B + BATTLE_F_MAXHP, "Int"),
                hp:    NumGet(buf, o + BATTLE_ACTOR_BLOCK_B + BATTLE_F_HP,    "Int"),
                maxMp: NumGet(buf, o + BATTLE_ACTOR_BLOCK_B + BATTLE_F_MAXMP, "Int"),
                mp:    NumGet(buf, o + BATTLE_ACTOR_BLOCK_B + BATTLE_F_MP,    "Int")
            }
        })
    }
    return out
}

; The block everything outside the diagnostic reads.
BattleActorStats(rec) {
    global BATTLE_STATS_BLOCK
    return (BATTLE_STATS_BLOCK = 1) ? rec.b : rec.a
}

; Whether a location's numbers are usable at all — a sanity check on the pools,
; not a statement about occupancy.
;
; This is what stops a base that has moved after a game patch from being drawn
; as a confident-looking health bar. It cannot tell a live combatant from the
; values one left behind: a leftover measured at 29997/30971 passes every test
; here. Occupancy is BattleActorOccupied's job.
BattleActorStatsSane(stats) {
    global BATTLE_HP_SANE_MAX
    return stats.maxHp > 0 && stats.maxHp <= BATTLE_HP_SANE_MAX
        && stats.hp >= 0 && stats.hp <= stats.maxHp
}

; Whether anyone is actually standing in this location.
;
; The actor id is -1 for an empty location and for one still holding a departed
; combatant's values, which is the only field found that separates the two.
BattleActorOccupied(rec) {
    global BATTLE_ACTOR_ID_NONE
    return rec.id != BATTLE_ACTOR_ID_NONE && rec.id >= 0
}

; Occupied, and reading numbers worth showing. This is the test to count
; combatants with.
BattleActorIsPresent(rec) {
    return BattleActorOccupied(rec) && BattleActorStatsSane(BattleActorStats(rec))
}

; A fingerprint of the fight itself, identical in every client taking part.
;
; This is the same property that made the battle array so misleading for
; health: it describes ONE fight from the party leader's seat, so every client
; in that fight holds byte-for-byte the same array. What is useless for saying
; "whose health is this" is exactly right for saying "are you two in the same
; battle" — two clients agree here only if they are in the same fight.
;
; Built from location and the MAXIMA — deliberately NOT the actor id, and never
; current HP.
;
; Current values move constantly, so two clients polled a moment apart would
; disagree for no good reason. The actor id looked like the ideal ingredient and
; is the opposite: keying on it gave every client a fingerprint no other client
; shared, so a five-strong party grouped as five parties of one. Whatever that
; id counts, it is not the same number in two clients looking at one fight.
;
; The maxima are known to agree across clients, and from the worst possible
; source: the bug where the roster showed one character's health on every row
; was two clients reading identical maxima out of this array. What made that
; wrong for identity makes it exactly right for grouping.
;
; Occupied party locations only, in location order, so a departed combatant's
; leftovers cannot change the answer.
BattleFingerprint(actors) {
    key := ""
    for rec in actors {
        if (rec.isEnemy || !BattleActorIsPresent(rec)) {
            continue
        }
        st := BattleActorStats(rec)
        key .= rec.loc ":" st.maxHp ":" st.maxMp "|"
    }
    return key
}

; Which battle location is THIS client's character.
;
; Neither source is sufficient alone, and they fail in opposite directions:
;
;   the battle array   is live during a fight, but leader-relative — location 0
;                      is the party leader, not whoever is looking
;   the character block is reliably per-character, but its CURRENT hp/mp stop
;                      updating the moment a fight starts (measured: a HUD
;                      showing 2121162 while the block still read 984027)
;
; The join is the MAXIMA. Those stay correct in the character block during
; battle — verified against the HUD mid-fight — and the battle record carries
; them too, so the location whose maximum HP and MP both equal this character's
; is this character. That turns the live array into a per-client reading.
;
; Matching on the pair rather than HP alone is what keeps two similarly-built
; party members apart, and an ambiguous match returns nothing rather than
; guessing: picking arbitrarily between two members is precisely the bug this
; whole exercise started with.
FindOwnBattleLocation(actors, maxHp, maxMp) {
    if (maxHp <= 0) {
        return 0
    }
    found := 0
    for rec in actors {
        if (rec.isEnemy || rec.isPet || !BattleActorIsPresent(rec)) {
            continue
        }
        st := BattleActorStats(rec)
        if (st.maxHp = maxHp && st.maxMp = maxMp) {
            if (found) {
                return 0
            }
            found := rec
        }
    }
    return found
}

; This character's own HP/MP, from the per-character block.
;
; Unlike the battle actor array this is genuinely per client, so it is what the
; roster reads. One 16-byte read on the poll's existing handle.
;
; Returns ok:false at a login screen, where the block is all zeros — a client
; with nothing loaded has no vitals, which is different from having zero health.
ReadCharacterVitals(proc, inBattle := false) {
    global CHAR_VITALS_BYTES, CHAR_V_MAXHP, CHAR_V_HP, CHAR_V_MAXMP, CHAR_V_MP, BATTLE_HP_SANE_MAX
    global BATTLE_PARTY_SLOTS
    blank := { ok: false, live: false, hp: 0, maxHp: 0, mp: 0, maxMp: 0,
        hasPet: false, petHp: 0, petMaxHp: 0, petMp: 0, petMaxMp: 0, battleKey: "" }
    buf := ReadClientBuffer(proc, GetResolvedOffset("CHAR_VITALS_OFFSET"), CHAR_VITALS_BYTES)
    if !buf {
        return blank
    }
    maxHp := NumGet(buf, CHAR_V_MAXHP, "Int")
    hp    := NumGet(buf, CHAR_V_HP, "Int")
    maxMp := NumGet(buf, CHAR_V_MAXMP, "Int")
    mp    := NumGet(buf, CHAR_V_MP, "Int")
    ; Same plausibility rule the offset validator uses, so a patch that moves
    ; this shows as "no data" rather than as a wrong bar.
    good := (maxHp > 0 && maxHp <= BATTLE_HP_SANE_MAX && hp >= 0 && hp <= maxHp
        && maxMp >= 0 && maxMp <= BATTLE_HP_SANE_MAX && mp >= 0 && mp <= maxMp)
    res := { ok: good, live: true, hp: hp, maxHp: maxHp, mp: mp, maxMp: maxMp,
        hasPet: false, petHp: 0, petMaxHp: 0, petMp: 0, petMaxMp: 0, battleKey: "" }
    if (!good || !inBattle) {
        return res
    }

    ; Out of a fight the block above is the live one. In a fight its current
    ; values freeze, so they come from the battle array instead — via the
    ; maxima, which are what say which location is ours.
    actors := ReadBattleActors(proc, BATTLE_PARTY_SLOTS)
    ; Costs nothing extra — the array is already read and in hand.
    res.battleKey := BattleFingerprint(actors)
    rec := FindOwnBattleLocation(actors, maxHp, maxMp)
    if !rec {
        res.live := false
        return res
    }
    st := BattleActorStats(rec)
    res.hp := st.hp
    res.mp := st.mp
    res.maxHp := st.maxHp
    res.maxMp := st.maxMp

    ; A pet sits in the location after its owner, so identifying the owner
    ; identifies the pet — which reading location 1 blindly never could.
    for other in actors {
        if (other.loc = rec.loc + 1 && BattleActorIsPresent(other)) {
            pst := BattleActorStats(other)
            res.hasPet := true
            res.petHp := pst.hp
            res.petMaxHp := pst.maxHp
            res.petMp := pst.mp
            res.petMaxMp := pst.maxMp
        }
    }
    return res
}

GetClientSnapshots() {
    global gClientSnapshots
    return gClientSnapshots
}

; Polls every client and fires OnSnapshot. Skipped entirely while no enabled
; addon consumes it, so single-feature installs pay nothing.
UpdateClientSnapshots() {
    global gClientSnapshots, gClientHandles, gResolvedBuildStamp
    global MAP_FILE_LEN, MAP_NAME_LEN

    if !HasEnabledAddonHook("OnSnapshot") {
        if (gClientSnapshots.Length > 0) {
            gClientSnapshots := []
            ReleaseAllClientProcesses()
        }
        return
    }

    windows := GetTopLevelGameWindows()
    if (windows.Length = 0) {
        ReleaseAllClientProcesses()
        if (gClientSnapshots.Length > 0) {
            gClientSnapshots := []
            FireAddonHook("OnSnapshot", gClientSnapshots)
        }
        return
    }

    ; RVAs are per build, not per process — resolve once and reuse for every
    ; client. EnsureResolvedOffsetsForBuild re-reads the whole .text section,
    ; so it must not run on this tick once something is already resolved.
    if (gResolvedBuildStamp = 0) {
        GetCachedProcessHandleAndBase()
    }
    mapFileRva := GetResolvedOffset("MAP_FILE_OFFSET")
    mapNameRva := GetResolvedOffset("MAP_NAME_OFFSET")
    posRva := GetResolvedOffset("POS_X_OFFSET")
    stateRva := GetResolvedOffset("GAME_STATE_OFFSET")
    battleRva := GetResolvedOffset("BATTLE_STATE_OFFSET")
    classRva := GetResolvedOffset("CHAR_CLASS_OFFSET")

    activePid := 0
    if (activeHwnd := WinActive(GAME_WIN_FILTER)) {
        try activePid := WinGetPID("ahk_id " activeHwnd)
    }

    snapshots := []
    livePids := Map()
    for hwnd in windows {
        pid := 0
        try pid := WinGetPID("ahk_id " hwnd)
        ; WinGetList can return several hwnds for one client — first one wins.
        if (!pid || livePids.Has(pid)) {
            continue
        }
        proc := GetClientProcess(pid)
        if !proc.ok {
            continue
        }

        fileBuf := ReadClientBuffer(proc, mapFileRva, MAP_FILE_LEN)
        if !fileBuf {
            ; Client is going away (or protected) — drop the handle and retry
            ; on the next tick rather than caching a dead one.
            ReleaseClientProcess(pid)
            continue
        }
        livePids[pid] := true

        mapId := MapIdFromName(Trim(StrGet(fileBuf, MAP_FILE_LEN, "CP0"), " `t`r`n`0"))

        mapName := ""
        if (nameBuf := ReadClientBuffer(proc, mapNameRva, MAP_NAME_LEN)) {
            candidate := Trim(StrGet(nameBuf, MAP_NAME_LEN, "CP0"), " `t`r`n`0")
            ; Reject filename spill so callers never show "MAP301.map" as a zone.
            if !RegExMatch(candidate, "i)^MAP\d+\.map$") {
                mapName := candidate
            }
        }

        x := 0
        y := 0
        if (posBuf := ReadClientBuffer(proc, posRva, 8)) {  ; X and Y are contiguous
            x := NumGet(posBuf, 0, "Int")
            y := NumGet(posBuf, 4, "Int")
        }

        gameState := -1
        if (stateBuf := ReadClientBuffer(proc, stateRva, 4)) {
            gameState := NumGet(stateBuf, 0, "Int")
        }

        inBattle := false
        if (battleBuf := ReadClientBuffer(proc, battleRva, 4)) {
            inBattle := NumGet(battleBuf, 0, "Int") != 0
        }

        ; -1 = unknown. Before login there is no character loaded, so whatever
        ; sits at the class address is stale; anything outside 0-7 has no avatar.
        classId := -1
        if (gameState >= GAME_STATE_READY && (classBuf := ReadClientBuffer(proc, classRva, 4))) {
            candidateClass := NumGet(classBuf, 0, "Int")
            if (candidateClass >= 0 && candidateClass <= 7) {
                classId := candidateClass
            }
        }

        ; Health rides the same poll rather than adding a loop of its own: one
        ; 16-byte read per client, from the process handle already open.
        vit := ReadCharacterVitals(proc, inBattle)
        snapshots.Push({
            hwnd: hwnd,
            pid: pid,
            charName: CharacterNameFromWindow(hwnd),
            mapId: mapId,
            mapName: mapName,
            x: x,
            y: y,
            gameState: gameState,
            inBattle: inBattle,
            classId: classId,
            isActive: (pid = activePid),
            ; live is false only when a fight is on and no location matched,
            ; where the block's numbers are frozen — "—" beats a stale bar.
            hasVitals: vit.ok && vit.live,
            ; Empty outside a fight. Clients sharing a non-empty key are in the
            ; same battle — see BattleFingerprint.
            battleKey: vit.battleKey,
            hp:    vit.ok ? vit.hp    : 0,
            maxHp: vit.ok ? vit.maxHp : 0,
            mp:    vit.ok ? vit.mp    : 0,
            maxMp: vit.ok ? vit.maxMp : 0,
            ; Only during a fight, and only once the owner's location is known.
            hasPet: vit.ok && vit.live && vit.hasPet,
            petHp: vit.petHp,
            petMaxHp: vit.petMaxHp,
            petMp: vit.petMp,
            petMaxMp: vit.petMaxMp
        })
    }

    ; Release handles for clients that have closed.
    for pid, _ in gClientHandles.Clone() {
        if !livePids.Has(pid) {
            ReleaseClientProcess(pid)
        }
    }

    gClientSnapshots := snapshots
    FireAddonHook("OnSnapshot", snapshots)
}

; ── NPC Generator ────────────────────────────────────────────────

; Loads the next NPC ID counter from config.ini, falling back to NPC_ID_START.
LoadNpcNextId() {
    global gNpcNextId, NPC_ID_START, CONFIG_INI
    if FileExist(CONFIG_INI) {
        saved := Trim(IniRead(CONFIG_INI, "NpcGenerator", "NextId", ""))
        if (saved != "") {
            gNpcNextId := Integer(saved)
            return
        }
    }
    gNpcNextId := NPC_ID_START
}

; Persists the current NPC ID counter to config.ini.
SaveNpcNextId() {
    global gNpcNextId, CONFIG_INI
    IniWrite(Format("0x{:08X}", gNpcNextId), CONFIG_INI, "NpcGenerator", "NextId")
}

; Reads the raw map filename from game memory (e.g. "MAP007.map") and returns
; the base name without extension (e.g. "MAP007").
ReadCurrentMapBaseName() {
    global gLastReadStatus

    cached := GetCachedProcessHandleAndBase()
    if !cached.ok {
        return ""
    }

    targetAddr := cached.modBase + GetResolvedOffset("MAP_FILE_OFFSET")
    rawBytes := Buffer(MAP_FILE_LEN, 0)
    bytesRead := 0
    ok := DllCall(
        "ReadProcessMemory",
        "Ptr", cached.handle,
        "Ptr", targetAddr,
        "Ptr", rawBytes.Ptr,
        "UPtr", MAP_FILE_LEN,
        "UPtr*", &bytesRead,
        "Int"
    )

    if (!ok || bytesRead < 1) {
        return ""
    }

    mapFile := StrGet(rawBytes, MAP_FILE_LEN, "CP0")
    mapFile := Trim(mapFile, " `t`r`n`0")
    if (mapFile = "") {
        return ""
    }
    ; Strip extension to get base name (e.g. "MAP007").
    return RegExReplace(mapFile, "\.[^.]+$", "")
}

; One entry in the server repo's TypeScript object-literal shape. Shared by the
; single-position generator below and the map-POI bulk export, so both produce
; text that can be pasted into the repo unchanged.
BuildNpcEntryText(id, name, mapBase, x, y) {
    return "`t{`n"
        . "`t`tid: " Format("0x{:08X}", id) ",`n"
        . "`t`tname: '" StrReplace(name, "'", "\'") "',`n"
        . "`t`tfile: 135,`n"
        . "`t`tmap: MapID." mapBase ",`n"
        . "`t`tpoint: { x: " x ", y: " y " },`n"
        . "`t`tdirection: Direction.South,`n"
        . "`t},`n"
}

; Captures the current player position and map, then appends a new NPC entry
; to the output file in TypeScript-compatible object literal format.
GenerateNpcEntry() {
    global gNpcNextId, NPC_OUTPUT_FILE

    ; Read raw player position from game memory.
    rawPos := ReadRawPlayerPosition()
    if !rawPos.ok {
        TrayTip("Failed to read player position from memory.", "NPC Generator", "Iconx")
        return
    }

    ; Get map identifier (e.g. "MAP007").
    mapBase := ReadCurrentMapBaseName()
    if (mapBase = "") {
        TrayTip("Failed to read map name from memory.", "NPC Generator", "Iconx")
        return
    }

    idHex := Format("0x{:08X}", gNpcNextId)
    entry := BuildNpcEntryText(gNpcNextId, "Placeholder", mapBase, rawPos.x, rawPos.y)

    ; Append to output file.
    FileAppend(entry, NPC_OUTPUT_FILE, "UTF-8")

    ; Increment and persist the counter.
    gNpcNextId += 1
    SaveNpcNextId()

    ; Raw position is what the entry carries; the in-game numbers are there so
    ; it can be matched against what's on screen.
    TrayTip("NPC " idHex " added`n"
        . "Pos: " rawPos.x ", " rawPos.y " (in-game " GameCoordText(rawPos.x, rawPos.y) ")`n"
        . "Map: " ZoneDisplayName(mapBase) " (" mapBase ")", "NPC Generator", "Iconi")
}

; ── Signature-based RVA discovery ────────────────────────────────
;
; Why this exists:
;   Game patches reshuffle .data, so hardcoded RVAs in variables.ahk go stale.
;   Instead of guessing addresses, we keep a byte-pattern fingerprint of the
;   instruction that references each global. After a patch, we re-scan the
;   .text section for that fingerprint and read the new RVA out of the operand.
;
; Workflow:
;   1. User confirms the hardcoded constants are correct (positions/states read
;      sane values in-game) and presses Ctrl+Alt+S → CalibrateSignaturesNow().
;      For each name in SIGNATURE_NAMES, we find every code reference to
;      modBase+RVA and save the smallest unique surrounding-byte signature to
;      signatures.ini (operand bytes wildcarded).
;   2. Future runs: on attach we read the PE TimeDateStamp; if offsets_cache.ini
;      already has resolved RVAs for this build, use them. Otherwise, scan with
;      the saved signatures, persist the result, and use it. Falls back to the
;      hardcoded constants if no signature is available yet.
;
; Assumes 32-bit target (main_client.exe). 32-bit instructions embed absolute
; addresses directly; x64 RIP-relative encoding is not handled here.

; Reads PE info from the live module: timestamp (build identifier) and the
; .text section bytes (so we can scan for code references).
ReadPEInfo(handle, modBase) {
    dosBuf := Buffer(64, 0)
    if !DllCall("ReadProcessMemory", "Ptr", handle, "Ptr", modBase, "Ptr", dosBuf.Ptr, "UPtr", 64, "UPtr*", 0, "Int") {
        return { ok: false, reason: "dos_read_failed" }
    }
    if (NumGet(dosBuf, 0, "UShort") != 0x5A4D) {
        return { ok: false, reason: "no_mz" }
    }
    elfanew := NumGet(dosBuf, 60, "UInt")

    ; NT signature (4) + IMAGE_FILE_HEADER (20) + max optional header (240).
    ntBuf := Buffer(264, 0)
    if !DllCall("ReadProcessMemory", "Ptr", handle, "Ptr", modBase + elfanew, "Ptr", ntBuf.Ptr, "UPtr", 264, "UPtr*", 0, "Int") {
        return { ok: false, reason: "nt_read_failed" }
    }
    if (NumGet(ntBuf, 0, "UInt") != 0x00004550) { ; "PE\0\0"
        return { ok: false, reason: "no_pe" }
    }
    machine := NumGet(ntBuf, 4, "UShort")
    numSections := NumGet(ntBuf, 6, "UShort")
    timeDateStamp := NumGet(ntBuf, 8, "UInt")
    sizeOfOpt := NumGet(ntBuf, 4 + 16, "UShort")

    sectionsAddr := modBase + elfanew + 4 + 20 + sizeOfOpt
    sectionsSize := numSections * 40
    if (sectionsSize <= 0 || sectionsSize > 0x10000) {
        return { ok: false, reason: "bad_section_count" }
    }
    secBuf := Buffer(sectionsSize, 0)
    if !DllCall("ReadProcessMemory", "Ptr", handle, "Ptr", sectionsAddr, "Ptr", secBuf.Ptr, "UPtr", sectionsSize, "UPtr*", 0, "Int") {
        return { ok: false, reason: "sections_read_failed" }
    }

    ; Prefer ".text"; otherwise first section with executable characteristics.
    codeRva := 0
    codeSize := 0
    fallbackRva := 0
    fallbackSize := 0
    Loop numSections {
        off := (A_Index - 1) * 40
        name := StrGet(secBuf.Ptr + off, 8, "CP0")
        name := Trim(name, " `t`r`n`0")
        characteristics := NumGet(secBuf, off + 36, "UInt")
        virtSize := NumGet(secBuf, off + 8, "UInt")
        virtAddr := NumGet(secBuf, off + 12, "UInt")
        if (name = ".text") {
            codeRva := virtAddr
            codeSize := virtSize
            break
        }
        if (!fallbackRva && (characteristics & 0x20000000)) {
            fallbackRva := virtAddr
            fallbackSize := virtSize
        }
    }
    if (!codeRva) {
        codeRva := fallbackRva
        codeSize := fallbackSize
    }
    if (!codeRva || !codeSize) {
        return { ok: false, reason: "no_code_section" }
    }

    codeBuf := Buffer(codeSize, 0)
    bytesRead := 0
    if !DllCall("ReadProcessMemory", "Ptr", handle, "Ptr", modBase + codeRva, "Ptr", codeBuf.Ptr, "UPtr", codeSize, "UPtr*", &bytesRead, "Int") {
        return { ok: false, reason: "code_read_failed" }
    }
    return {
        ok: true,
        machine: machine,
        timeDateStamp: timeDateStamp,
        codeRva: codeRva,
        codeSize: bytesRead,
        codeBuf: codeBuf
    }
}

; Returns array of byte offsets (within codeBuf) where the 4-byte little-endian
; encoding of `targetAbs` appears. Used during bootstrap: every match is a
; candidate code reference to the global at modBase + RVA.
FindAbs32References(codeBuf, codeLen, targetAbs) {
    results := []
    if (codeLen < 4) {
        return results
    }
    target := targetAbs & 0xFFFFFFFF
    last := codeLen - 4
    Loop last + 1 {
        i := A_Index - 1
        if (NumGet(codeBuf, i, "UInt") = target) {
            results.Push(i)
        }
    }
    return results
}

; Parses an IDA-style hex pattern string into a fixed-byte buffer + same-length
; mask buffer (1 = compare, 0 = wildcard). Accepts "??" or "?" for wildcards.
;   "8B 0D ?? ?? ?? ?? 89 45 FC"  →  bytes + mask, len = 9
ParseHexPattern(patternStr) {
    tokens := []
    Loop Parse, patternStr, " `t" {
        if (A_LoopField != "") {
            tokens.Push(A_LoopField)
        }
    }
    n := tokens.Length
    if (n = 0) {
        return { ok: false, reason: "empty" }
    }
    bytes := Buffer(n, 0)
    mask := Buffer(n, 0)
    Loop n {
        i := A_Index - 1
        tok := tokens[A_Index]
        if (tok = "??" || tok = "?") {
            NumPut("UChar", 0, mask, i)
            NumPut("UChar", 0, bytes, i)
            continue
        }
        if !RegExMatch(tok, "^[0-9A-Fa-f]{2}$") {
            return { ok: false, reason: "bad_token: " tok }
        }
        NumPut("UChar", Integer("0x" tok), bytes, i)
        NumPut("UChar", 1, mask, i)
    }
    return { ok: true, bytes: bytes, mask: mask, len: n }
}

; Hex-encodes `len` bytes from `buf` starting at `off`, with the four bytes at
; `[wildOff, wildOff+4)` rendered as "??". Used when capturing a signature so
; the operand position survives a recompile that reshuffles addresses.
HexEncodeWithWildcardOperand(buf, off, len, wildOff) {
    out := ""
    Loop len {
        i := A_Index - 1
        if (out != "") {
            out .= " "
        }
        if (i >= wildOff && i < wildOff + 4) {
            out .= "??"
        } else {
            out .= Format("{:02X}", NumGet(buf, off + i, "UChar"))
        }
    }
    return out
}

; Counts matches of (bytes, mask) in codeBuf, stopping early once `cap` is
; reached (we only need to know "1 vs >1" during signature capture).
CountMaskedMatches(codeBuf, codeLen, bytes, mask, patLen, cap := 2) {
    if (patLen <= 0 || patLen > codeLen) {
        return 0
    }
    firstFixed := -1
    Loop patLen {
        if (NumGet(mask, A_Index - 1, "UChar")) {
            firstFixed := A_Index - 1
            break
        }
    }
    if (firstFixed < 0) {
        return 0
    }
    firstByte := NumGet(bytes, firstFixed, "UChar")
    last := codeLen - patLen
    count := 0
    Loop last + 1 {
        i := A_Index - 1
        if (NumGet(codeBuf, i + firstFixed, "UChar") != firstByte) {
            continue
        }
        match := true
        Loop patLen {
            j := A_Index - 1
            if (!NumGet(mask, j, "UChar")) {
                continue
            }
            if (NumGet(codeBuf, i + j, "UChar") != NumGet(bytes, j, "UChar")) {
                match := false
                break
            }
        }
        if (match) {
            count += 1
            if (count >= cap) {
                return count
            }
        }
    }
    return count
}

; First match offset of (bytes, mask) in codeBuf, or -1.
FindMaskedMatch(codeBuf, codeLen, bytes, mask, patLen) {
    if (patLen <= 0 || patLen > codeLen) {
        return -1
    }
    firstFixed := -1
    Loop patLen {
        if (NumGet(mask, A_Index - 1, "UChar")) {
            firstFixed := A_Index - 1
            break
        }
    }
    if (firstFixed < 0) {
        return -1
    }
    firstByte := NumGet(bytes, firstFixed, "UChar")
    last := codeLen - patLen
    Loop last + 1 {
        i := A_Index - 1
        if (NumGet(codeBuf, i + firstFixed, "UChar") != firstByte) {
            continue
        }
        match := true
        Loop patLen {
            j := A_Index - 1
            if (!NumGet(mask, j, "UChar")) {
                continue
            }
            if (NumGet(codeBuf, i + j, "UChar") != NumGet(bytes, j, "UChar")) {
                match := false
                break
            }
        }
        if (match) {
            return i
        }
    }
    return -1
}

; Builds a unique signature for `rva`. Strategy: find every code site that
; contains modBase+RVA as a 4-byte absolute operand. For each candidate, grow
; symmetric byte context around the operand until the masked pattern matches
; exactly once. The first candidate that becomes unique wins.
;
; Returns { ok, sig, opOffset, sigLen } on success, or { ok:false, reason }.
CaptureSignatureForRva(modBase, codeBuf, codeLen, codeRva, rva) {
    targetAbs := (modBase + rva) & 0xFFFFFFFF
    refs := FindAbs32References(codeBuf, codeLen, targetAbs)
    if (refs.Length = 0) {
        return { ok: false, reason: "no_refs" }
    }

    ; Try increasing context until the masked pattern is unique.
    static contextSteps := [4, 6, 8, 12, 16, 24]

    for refIndex, opPos in refs {
        for stepIdx, ctx in contextSteps {
            back := ctx
            fwd := ctx
            startOff := opPos - back
            sigLen := back + 4 + fwd
            if (startOff < 0 || startOff + sigLen > codeLen) {
                continue
            }
            ; Build pattern + mask with operand bytes wildcarded.
            bytes := Buffer(sigLen, 0)
            mask := Buffer(sigLen, 0)
            Loop sigLen {
                i := A_Index - 1
                NumPut("UChar", NumGet(codeBuf, startOff + i, "UChar"), bytes, i)
                isOperand := (i >= back && i < back + 4)
                NumPut("UChar", isOperand ? 0 : 1, mask, i)
            }
            count := CountMaskedMatches(codeBuf, codeLen, bytes, mask, sigLen, 2)
            if (count = 1) {
                sigHex := HexEncodeWithWildcardOperand(codeBuf, startOff, sigLen, back)
                return {
                    ok: true,
                    sig: sigHex,
                    opOffset: back,
                    sigLen: sigLen,
                    refCount: refs.Length
                }
            }
        }
    }
    return { ok: false, reason: "non_unique", refCount: refs.Length }
}

; Resolves an RVA from a saved signature against the live process.
;   modBase   - current load address of main_client.exe
;   codeBuf   - bytes of the .text section
;   sigHex    - "8B 0D ?? ?? ?? ?? ..."
;   opOffset  - byte index of the wildcarded 4-byte operand within the signature
ResolveRvaFromSignature(modBase, codeBuf, codeLen, sigHex, opOffset) {
    parsed := ParseHexPattern(sigHex)
    if !parsed.ok {
        return { ok: false, reason: "parse: " parsed.reason }
    }
    if (opOffset < 0 || opOffset + 4 > parsed.len) {
        return { ok: false, reason: "bad_op_offset" }
    }
    pos := FindMaskedMatch(codeBuf, codeLen, parsed.bytes, parsed.mask, parsed.len)
    if (pos < 0) {
        return { ok: false, reason: "not_found" }
    }
    ; Make sure it's unique — non-unique resolves are unsafe.
    count := CountMaskedMatches(codeBuf, codeLen, parsed.bytes, parsed.mask, parsed.len, 2)
    if (count != 1) {
        return { ok: false, reason: "not_unique" }
    }
    operandAbs := NumGet(codeBuf, pos + opOffset, "UInt")
    rva := (operandAbs - modBase) & 0xFFFFFFFF
    return { ok: true, rva: rva }
}

; ── Persistence ──────────────────────────────────────────────────

LoadSignaturesIni() {
    sigs := Map()
    if !FileExist(SIGNATURES_INI) {
        return sigs
    }
    for _, name in SIGNATURE_NAMES {
        sig := Trim(IniRead(SIGNATURES_INI, name, "sig", ""))
        if (sig = "") {
            continue
        }
        opStr := Trim(IniRead(SIGNATURES_INI, name, "opOffset", ""))
        if (opStr = "") {
            continue
        }
        sigs[name] := { sig: sig, opOffset: Integer(opStr) }
    }
    return sigs
}

SaveSignatureIni(name, sig, opOffset) {
    IniWrite(sig, SIGNATURES_INI, name, "sig")
    IniWrite(opOffset, SIGNATURES_INI, name, "opOffset")
}

BuildKeyForStamp(stamp) {
    return "Build_" Format("{:08X}", stamp)
}

; ── Config with shipped defaults ─────────────────────────────────
;
; Precedence: config.ini (the user's own edits) > defaults.ini (what a
; maintainer tunes) > the literal already at the call site (last-resort
; safety net if defaults.ini is missing or a hand-edited mistake).

DefaultRead(section, key, fallback) {
    global DEFAULTS_INI
    return IniRead(DEFAULTS_INI, section, key, fallback)
}

ConfigRead(section, key, fallback) {
    global CONFIG_INI
    return IniRead(CONFIG_INI, section, key, DefaultRead(section, key, fallback))
}

ResolveWritableIniPath(filename) {
    scriptPath := A_ScriptDir "\" filename
    if IsPathWritable(scriptPath) {
        return scriptPath
    }
    fallbackDir := A_AppData "\osMW Maps++"
    if !DirExist(fallbackDir) {
        try DirCreate(fallbackDir)
    }
    fallbackPath := fallbackDir "\" filename
    ; Carry over any existing file so users don't lose state when their folder
    ; becomes unwritable (e.g. moved into Program Files post-install).
    if (FileExist(scriptPath) && !FileExist(fallbackPath)) {
        try FileCopy(scriptPath, fallbackPath)
    }
    return fallbackPath
}

; ── Logging ──────────────────────────────────────────────────────
;
; Errors used to go to a TrayTip and nowhere else, so anything that happened
; while the user was in a fight — or that happened once at startup — was gone by
; the time they came to report it. One line per entry, plain text, next to the
; exe (or in %AppData%, by exactly the same rule as the inis).

LogPath() {
    global gLogPath
    if (gLogPath = "")
        gLogPath := ResolveWritableIniPath("mapsplusplus.log")
    return gLogPath
}

; level is "INFO" | "WARN" | "ERROR". source names the subsystem or addon.
Log(level, source, message) {
    global gLogFailed, LOG_MAX_BYTES
    if gLogFailed
        return
    try {
        path := LogPath()
        ; Rotate before writing so the cap is a cap, not a target.
        if (FileExist(path) && FileGetSize(path) > LOG_MAX_BYTES) {
            try FileDelete(path ".1")
            try FileMove(path, path ".1")
        }
        line := FormatTime(A_Now, "yyyy-MM-dd HH:mm:ss")
            . "  " Format("{:-5}", level)
            . "  [" source "]  " _LogOneLine(message) "`n"
        FileAppend(line, path, "UTF-8")
    } catch {
        ; A log that cannot be written must never become the thing that breaks
        ; the app. Give up quietly for the rest of the session.
        gLogFailed := true
    }
}

LogInfo(source, message) => Log("INFO", source, message)
LogWarn(source, message) => Log("WARN", source, message)
LogError(source, message) => Log("ERROR", source, message)

; Keeps one entry on one line so the file stays greppable.
_LogOneLine(s) {
    s := StrReplace(StrReplace(String(s), "`r", " "), "`n", " | ")
    return Trim(s)
}

; The last `count` lines, for the diagnostics report.
LogTail(count := 40) {
    try {
        path := LogPath()
        if !FileExist(path)
            return "(log is empty)"
        lines := StrSplit(Trim(FileRead(path, "UTF-8"), "`r`n"), "`n", "`r")
        out := ""
        start := Max(1, lines.Length - count + 1)
        Loop lines.Length - start + 1
            out .= lines[start + A_Index - 1] "`n"
        return RTrim(out, "`n")
    } catch as err {
        return "(could not read the log: " err.Message ")"
    }
}

; ── Diagnostics report ───────────────────────────────────────────
;
; Everything a bug report needs, in one paste. The point is that "it doesn't
; work" from a beta tester becomes something that can be acted on without a
; round trip: which build, which offsets resolved and how, which addons are on,
; and what the log has been saying.

BuildDiagnosticsReport() {
    global APP_VERSION, gInterfaceMode, gResolvedBuildStamp, SIGNATURE_NAMES
    global gAddonHooks, gDisabledAddons, gQuarantinedAddons

    out := "osMW Maps++ diagnostics`n"
        . "========================`n"
        . "Version   : " APP_VERSION (A_IsCompiled ? " (compiled)" : " (source)") "`n"
        . "Generated : " FormatTime(A_Now, "yyyy-MM-dd HH:mm:ss") "`n"
        . "AutoHotkey: " A_AhkVersion "  " (A_PtrSize = 8 ? "64-bit" : "32-bit") "`n"
        . "OS        : " A_OSVersion "`n"
        . "Interface : " gInterfaceMode "`n"
        . "Script dir: " A_ScriptDir "`n"
        . "Log       : " LogPath() "`n"

    ; The addon set is what actually distinguishes one variant from another, and
    ; it says more than a variant name would — it also shows what is switched off.
    out .= "`nAddons`n------`n"
    if (gAddonHooks.Length = 0) {
        out .= "  (none loaded)`n"
    }
    for _, am in gAddonHooks {
        name := am.Has("name") ? am["name"] : "<unnamed>"
        state := "on"
        if (gDisabledAddons.Has(name) && gDisabledAddons[name]) {
            state := gQuarantinedAddons.Has(name)
                ? "QUARANTINED (failed in " gQuarantinedAddons[name] ")"
                : "off"
        }
        out .= "  " Format("{:-16}", name) " " state "`n"
    }

    out .= "`nMemory offsets`n--------------`n"
        . "Build stamp: " (gResolvedBuildStamp ? Format("0x{:08X}", gResolvedBuildStamp) : "<unresolved>") "`n"
    for _, name in SIGNATURE_NAMES {
        rva := GetResolvedOffset(name)
        out .= "  " Format("{:-28}", name) " " Format("0x{:08X}", rva)
            . "  (" OffsetSourceLabel(name) ")`n"
    }

    out .= "`nDisplays`n--------`n"
    Loop MonitorGetCount() {
        MonitorGet(A_Index, &l, &t, &r, &b)
        MonitorGetWorkArea(A_Index, &wl, &wt, &wr, &wb)
        out .= "  " A_Index ": " (r - l) "x" (b - t) " at " l "," t
            . "  work " (wr - wl) "x" (wb - wt)
            . (A_Index = MonitorGetPrimary() ? "  (primary)" : "") "`n"
    }

    out .= "`nClients`n-------`n"
    UpdateClientSnapshots()
    snapshots := GetClientSnapshots()
    if (snapshots.Length = 0) {
        out .= "  (no game clients running)`n"
    }
    for _, snap in snapshots {
        out .= "  " Format("{:-16}", (snap.charName = "" ? "PID " snap.pid : snap.charName))
            . " map " Format("{:-8}", (snap.mapId = "" ? "?" : snap.mapId))
            . " class " Format("{:2}", snap.classId)
            . " state " Format("{:3}", snap.gameState)
            . (snap.inBattle ? "  in battle" : "")
            . (snap.isActive ? "  (active)" : "") "`n"
    }

    out .= "`nRecent log`n----------`n" LogTail(40) "`n"
    return out
}

; Copies the report and writes it beside the log, so the user has it either way
; — a tester who cannot paste a clipboard can attach a file instead.
CopyDiagnosticsReport() {
    report := BuildDiagnosticsReport()
    A_Clipboard := report
    savedTo := ""
    try {
        savedTo := LogPath() ".diagnostics.txt"
        try FileDelete(savedTo)
        FileAppend(report, savedTo, "UTF-8")
    } catch {
        savedTo := ""
    }
    LogInfo("Diagnostics", "Report generated.")
    TrayTip("Diagnostics copied to the clipboard."
        . (savedTo != "" ? "`nAlso saved to " savedTo : ""),
        "Maps++", "Iconi")
}

; ── Passive version check ────────────────────────────────────────
;
; One request, on a delay, off the startup path. It reports that a newer build
; exists and nothing else: no download, no install, no second request. A game
; companion that reads process memory has to be able to say exactly what it
; talks to and why, so this stays boring on purpose — and silent on failure,
; because a version check is never worth interrupting someone over.

CheckForUpdateAsync() {
    global VERSION_CHECK_URL, gVersionCheckEnabled
    if (!gVersionCheckEnabled || VERSION_CHECK_URL = "")
        return
    SetTimer(_DoVersionCheck, -8000)
}

; The startup check: silent unless there is genuinely something to say.
_DoVersionCheck() {
    res := FetchUpdateStatus()
    if (res.status != "update")
        return
    ; The tray gains a "Get the update" entry. Note this only lasts for the
    ; session — every restart clears it until the next check completes, which
    ; is why Settings has a "Check now" button that works at any time.
    SetTimer(RebuildTrayMenu, -1)
    TrayTip("Maps++ " res.version " is available — you have " APP_VERSION "."
        . (res.notes != "" ? "`n" res.notes : "")
        . "`nSettings → Launcher → Check now, or the tray menu",
        "Update available", "Iconi")
}

; The user-initiated check, from Settings. Unlike the startup one this always
; produces an answer — "couldn't reach the server" is information when someone
; pressed a button, and silence would just look broken.
;
; Returns the same {status, version, notes, reason} as FetchUpdateStatus.
CheckForUpdatesNow() {
    res := FetchUpdateStatus(true)
    if (res.status = "update")
        SetTimer(RebuildTrayMenu, -1)
    return res
}

; Does the request, parse and comparison, and records the result in the update
; globals. Returns {status, version, notes, reason} where status is:
;   "update"   a newer version exists
;   "current"  up to date
;   "failed"   could not fetch or could not understand the answer
;   "disabled" no update location is configured in this build
;
; `force` ignores the user's startup-check preference — it is only ever true for
; an explicit button press, which is consent by definition.
FetchUpdateStatus(force := false) {
    global VERSION_CHECK_URL, VERSION_CHECK_TIMEOUT_MS, APP_VERSION
    global gUpdateVersion, gUpdateNotes, gVersionCheckEnabled

    if (VERSION_CHECK_URL = "")
        return { status: "disabled", version: "", notes: "", reason: "No update location is configured in this build." }
    if (!force && !gVersionCheckEnabled)
        return { status: "disabled", version: "", notes: "", reason: "The update check is switched off." }

    body := ""
    try {
        req := ComObject("WinHttp.WinHttpRequest.5.1")
        ; resolve / connect / send / receive, all bounded.
        req.SetTimeouts(VERSION_CHECK_TIMEOUT_MS, VERSION_CHECK_TIMEOUT_MS,
            VERSION_CHECK_TIMEOUT_MS, VERSION_CHECK_TIMEOUT_MS)
        req.Open("GET", VERSION_CHECK_URL, true)
        ; A cached answer would mean an upload nobody sees. No query string, so
        ; the request carries nothing identifying the user.
        req.SetRequestHeader("Cache-Control", "no-cache")
        req.Send()
        req.WaitForResponse(VERSION_CHECK_TIMEOUT_MS // 1000)
        if (req.Status != 200) {
            LogInfo("VersionCheck", "HTTP " req.Status "; ignored.")
            return { status: "failed", version: "", notes: "",
                     reason: "The server answered with HTTP " req.Status "." }
        }
        body := Trim(req.ResponseText, " `t`r`n")
    } catch as err {
        ; Offline, DNS down, TLS failure, no route — all the same to us.
        LogInfo("VersionCheck", "Skipped: " err.Message)
        return { status: "failed", version: "", notes: "",
                 reason: "Could not reach the update server." }
    }

    parsed := _ParseVersionManifest(body)
    if !parsed.ok {
        LogWarn("VersionCheck", "Unusable response; ignored. (" parsed.reason ")")
        return { status: "failed", version: "", notes: "",
                 reason: "The server's answer could not be understood." }
    }
    if (CompareVersions(parsed.version, APP_VERSION) <= 0) {
        LogInfo("VersionCheck", "Up to date (latest " parsed.version ").")
        return { status: "current", version: parsed.version, notes: parsed.notes, reason: "" }
    }

    gUpdateVersion := parsed.version
    gUpdateNotes := parsed.notes
    LogInfo("VersionCheck", "Newer version available: " parsed.version
        . (parsed.notes != "" ? " — " parsed.notes : ""))
    return { status: "update", version: parsed.version, notes: parsed.notes, reason: "" }
}

; {ok, version, notes, reason}. Accepts the JSON manifest documented in
; web\README.md, or a bare version string so a mis-uploaded plain-text file
; still works instead of failing silently.
;
; Everything is validated before it is believed: this is the one input the app
; takes from the network, and a misconfigured or hijacked endpoint must not be
; able to put arbitrary text in front of the user.
_ParseVersionManifest(body) {
    global VERSION_NOTES_MAX
    fail(reason) => { ok: false, version: "", notes: "", reason: reason }

    if (body = "")
        return fail("empty body")
    if (StrLen(body) > 4096)
        return fail("body too large")

    version := "", notes := ""
    if (SubStr(body, 1, 1) = "{") {
        msg := ""
        try msg := _JSON_Parse(body)
        catch as err
            return fail("bad JSON: " err.Message)
        if (!IsObject(msg) || !msg.Has("version"))
            return fail("no version field")
        version := Trim(String(msg["version"]))
        if msg.Has("notes")
            notes := Trim(String(msg["notes"]))
    } else {
        version := body
    }

    if (version = "" || StrLen(version) > 32
        || !RegExMatch(version, "^[0-9][0-9A-Za-z.\-+]*$"))
        return fail("version '" SubStr(version, 1, 32) "' is not a version string")

    ; Notes are shown in a notification, so they get one line and a hard cap.
    notes := Trim(StrReplace(StrReplace(StrReplace(notes, "`r", " "), "`n", " "), "`t", " "))
    while InStr(notes, "  ")
        notes := StrReplace(notes, "  ", " ")
    if (StrLen(notes) > VERSION_NOTES_MAX)
        notes := Trim(SubStr(notes, 1, VERSION_NOTES_MAX - 1)) "…"

    return { ok: true, version: version, notes: notes, reason: "" }
}

; Opens the download page. The URL is the compiled-in constant, never anything
; the manifest supplied — see the note on VERSION_DOWNLOAD_URL.
OpenUpdatePage() {
    global VERSION_DOWNLOAD_URL
    try {
        Run(VERSION_DOWNLOAD_URL)
    } catch as err {
        LogWarn("VersionCheck", "Could not open " VERSION_DOWNLOAD_URL ": " err.Message)
        TrayTip("Could not open " VERSION_DOWNLOAD_URL, "Maps++", "Iconx")
    }
}

; -1 / 0 / 1 for a < b, a = b, a > b, following semver precedence: dotted
; numeric core first, then the pre-release suffix.
;
; The pre-release comparison is not optional detail — during a beta it is the
; ONLY thing that differs between releases. Comparing just the core would make
; 0.9.0-beta.2 look identical to 0.9.0-beta.1, so no beta user would ever be
; told about a new beta.
CompareVersions(a, b) {
    aCore := _VersionCore(a), bCore := _VersionCore(b)
    ; StrSplit("") returns an EMPTY array in v2, not [""], so indexing [1] here
    ; would throw on an empty version. The loop below reads past either array's
    ; end as 0, which is also what makes "1.0" and "1.0.0" compare equal.
    aParts := StrSplit(aCore, "."), bParts := StrSplit(bCore, ".")
    Loop Max(aParts.Length, bParts.Length) {
        av := (A_Index <= aParts.Length && IsInteger(aParts[A_Index])) ? Integer(aParts[A_Index]) : 0
        bv := (A_Index <= bParts.Length && IsInteger(bParts[A_Index])) ? Integer(bParts[A_Index]) : 0
        if (av != bv)
            return (av > bv) ? 1 : -1
    }
    return _ComparePreRelease(_VersionPre(a), _VersionPre(b))
}

; Semver pre-release precedence, for two versions whose cores already match.
; "" (no suffix) outranks any pre-release: 1.0.0 is newer than 1.0.0-beta.1.
_ComparePreRelease(a, b) {
    if (a = b)
        return 0
    if (a = "")
        return 1            ; a is the real release
    if (b = "")
        return -1
    aIds := StrSplit(a, "."), bIds := StrSplit(b, ".")
    Loop Max(aIds.Length, bIds.Length) {
        ; Ran out of identifiers first = lower precedence (beta < beta.1).
        if (A_Index > aIds.Length)
            return -1
        if (A_Index > bIds.Length)
            return 1
        ai := aIds[A_Index], bi := bIds[A_Index]
        aNum := IsInteger(ai), bNum := IsInteger(bi)
        if (aNum && bNum) {
            ; Numeric, so beta.10 beats beta.9 rather than losing to it.
            av := Integer(ai), bv := Integer(bi)
            if (av != bv)
                return (av > bv) ? 1 : -1
        } else if (aNum != bNum) {
            return aNum ? -1 : 1        ; numeric identifiers rank below text
        } else {
            c := StrCompare(ai, bi, true)
            if (c != 0)
                return (c > 0) ? 1 : -1
        }
    }
    return 0
}

; The numeric part of a version: everything before a pre-release suffix, with
; any build metadata ("+abc") dropped — semver ignores it for precedence.
_VersionCore(v) {
    v := _VersionNoBuild(v)
    p := InStr(v, "-")
    return p ? SubStr(v, 1, p - 1) : v
}

; The pre-release suffix without its leading "-", or "" when there is none.
_VersionPre(v) {
    v := _VersionNoBuild(v)
    p := InStr(v, "-")
    return p ? SubStr(v, p + 1) : ""
}

_VersionNoBuild(v) {
    v := Trim(String(v))
    p := InStr(v, "+")
    return p ? SubStr(v, 1, p - 1) : v
}

; Makes sure an ini exists as UTF-16LE before anything writes to it.
;
; Windows' profile API decides a file's encoding once, from what is already on
; disk, and AHK only gets that right when IniWrite is what creates the file. A
; store whose first call is IniDelete — deleting a section it is about to
; rewrite — instead gets an empty ANSI file, and from then on every IniWrite
; through it drops anything outside the system codepage: a character name or POI
; label in Chinese became "??" permanently, on a Western-locale machine, at the
; moment the file was created.
;
; Call this before the first mutation in any save path. It is a no-op once the
; file exists, so it costs one FileExist on the paths that were already fine.
EnsureIniUtf16(path) {
    if FileExist(path)
        return
    try {
        f := FileOpen(path, "w", "UTF-16")   ; writes the BOM
        f.Close()
    }
}

IsPathWritable(path) {
    try {
        f := FileOpen(path, "a")
        if !IsObject(f) {
            return false
        }
        f.Close()
        return true
    } catch {
        return false
    }
}

LoadOffsetsCacheForBuild(stamp) {
    out := Map()
    if !FileExist(OFFSETS_CACHE_INI) {
        return out
    }
    section := BuildKeyForStamp(stamp)
    for _, name in SIGNATURE_NAMES {
        v := Trim(IniRead(OFFSETS_CACHE_INI, section, name, ""))
        if (v = "") {
            continue
        }
        ; Stored as "0xNNNNNNNN" hex.
        out[name] := Integer(v)
    }
    return out
}

SaveOffsetForBuild(stamp, name, rva) {
    IniWrite(Format("0x{:08X}", rva), OFFSETS_CACHE_INI, BuildKeyForStamp(stamp), name)
}

; ── Resolution flow ──────────────────────────────────────────────

; Populates gResolvedOffsets for the live build. Called after a successful
; process attach. Order: per-build cache → signature scan → fallback to
; hardcoded constants. Side-effects: writes newly resolved RVAs to the cache
; so the scan only happens once per build.
EnsureResolvedOffsetsForBuild(handle, modBase) {
    global gResolvedOffsets, gResolvedBuildStamp

    pe := ReadPEInfo(handle, modBase)
    if !pe.ok {
        ; Can't read PE — keep whatever's already resolved (or empty so we
        ; fall back to constants). Don't blow away cache.
        return
    }

    if (gResolvedBuildStamp = pe.timeDateStamp && gResolvedOffsets.Count > 0) {
        return
    }

    resolved := LoadOffsetsCacheForBuild(pe.timeDateStamp)
    needScan := false
    for _, name in SIGNATURE_NAMES {
        if !resolved.Has(name) {
            needScan := true
            break
        }
    }

    if (needScan) {
        sigs := LoadSignaturesIni()
        for _, name in SIGNATURE_NAMES {
            if resolved.Has(name) {
                continue
            }
            if !sigs.Has(name) {
                continue
            }
            entry := sigs[name]
            res := ResolveRvaFromSignature(modBase, pe.codeBuf, pe.codeSize, entry.sig, entry.opOffset)
            if (res.ok) {
                resolved[name] := res.rva
                SaveOffsetForBuild(pe.timeDateStamp, name, res.rva)
            }
        }
    }

    ; Derived offsets (see DERIVED_OFFSETS) come last — they need their source
    ; resolved first. Only a "bad" verdict rejects the candidate; "unknown" (an
    ; empty read, typical on a loading screen) still uses it for this session
    ; but isn't cached, so the check runs again on the next attach.
    for name, spec in DERIVED_OFFSETS {
        if (resolved.Has(name) || !resolved.Has(spec.from)) {
            continue
        }
        candidate := resolved[spec.from] + spec.delta
        ; .Call() — spec.validate(...) would invoke it as a method and pass
        ; spec itself as an extra leading argument.
        verdict := spec.validate.Call(handle, modBase + candidate)
        if (verdict = "bad") {
            continue
        }
        resolved[name] := candidate
        if (verdict = "ok") {
            SaveOffsetForBuild(pe.timeDateStamp, name, candidate)
        }
    }

    gResolvedOffsets := resolved
    gResolvedBuildStamp := pe.timeDateStamp
}

; Sanity check for a derived MAP_NAME_OFFSET: reads the candidate string and
; reports "ok" (looks like a zone name), "unknown" (empty or unreadable — the
; usual answer during a loading screen), or "bad" (it's the map *filename*,
; so the delta doesn't hold for this build).
; Plausible only if the four vitals agree with each other: positive maxima and
; current values that fit inside them. A moved offset lands on unrelated data,
; which fails one of these almost immediately.
ValidateCharVitalsRva(handle, addr) {
    global CHAR_VITALS_BYTES, CHAR_V_MAXHP, CHAR_V_HP, CHAR_V_MAXMP, CHAR_V_MP, BATTLE_HP_SANE_MAX
    buf := Buffer(CHAR_VITALS_BYTES, 0)
    ok := DllCall("ReadProcessMemory",
        "Ptr", handle,
        "Ptr", addr,
        "Ptr", buf.Ptr,
        "UPtr", CHAR_VITALS_BYTES,
        "UPtr*", 0,
        "Int")
    if !ok {
        return "unknown"
    }
    maxHp := NumGet(buf, CHAR_V_MAXHP, "Int")
    hp    := NumGet(buf, CHAR_V_HP, "Int")
    maxMp := NumGet(buf, CHAR_V_MAXMP, "Int")
    mp    := NumGet(buf, CHAR_V_MP, "Int")
    ; All zero at a login screen — right address, nothing loaded into it yet.
    if (maxHp = 0 && hp = 0 && maxMp = 0 && mp = 0) {
        return "unknown"
    }
    if (maxHp <= 0 || maxHp > BATTLE_HP_SANE_MAX) {
        return "bad"
    }
    if (hp < 0 || hp > maxHp) {
        return "bad"
    }
    if (maxMp < 0 || maxMp > BATTLE_HP_SANE_MAX || mp < 0 || mp > maxMp) {
        return "bad"
    }
    return "ok"
}

ValidateMapNameRva(handle, addr) {
    buf := Buffer(MAP_NAME_LEN, 0)
    ok := DllCall("ReadProcessMemory",
        "Ptr", handle,
        "Ptr", addr,
        "Ptr", buf.Ptr,
        "UPtr", MAP_NAME_LEN,
        "UPtr*", 0,
        "Int")
    if !ok {
        return "unknown"
    }
    name := Trim(StrGet(buf, MAP_NAME_LEN, "CP0"), " `t`r`n`0")
    if (name = "") {
        return "unknown"
    }
    if RegExMatch(name, "i)^MAP\d+\.map$") {
        return "bad"
    }
    return "ok"
}

; How GetResolvedOffset(name) arrived at its value — for the debug/verify UI.
OffsetSourceLabel(name) {
    global gResolvedOffsets, DERIVED_OFFSETS
    if !gResolvedOffsets.Has(name) {
        return "fallback"
    }
    return DERIVED_OFFSETS.Has(name) ? "derived" : "sig"
}

; Resolved RVA for `name`, or the hardcoded fallback if unresolved.
GetResolvedOffset(name) {
    global gResolvedOffsets, gFallbackOffsets
    if gResolvedOffsets.Has(name) {
        return gResolvedOffsets[name]
    }
    return gFallbackOffsets[name]
}

; Diagnostic: re-runs signature resolution against the live process (ignoring
; the gResolvedOffsets cache) and compares each result against the hardcoded
; constant + the persisted offsets_cache.ini entry. Tells the user whether
; the saved signatures are actually finding the same RVA the script is
; currently using.
VerifyResolution() {
    cached := GetCachedProcessHandleAndBase()
    if !cached.ok {
        ShowMessage("Game process not found.", "Verify Signatures", Map("severity", "warn"))
        return
    }
    pe := ReadPEInfo(cached.handle, cached.modBase)
    if !pe.ok {
        ShowMessage("PE read failed: " pe.reason, "Verify Signatures", Map("severity", "danger"))
        return
    }

    sigs := LoadSignaturesIni()
    cacheMap := LoadOffsetsCacheForBuild(pe.timeDateStamp)

    out := "(constant | cached | live scan)`n`n"

    okCount := 0
    failCount := 0
    for _, name in SIGNATURE_NAMES {
        constRva := gFallbackOffsets[name]
        cacheStr := cacheMap.Has(name) ? Format("0x{:08X}", cacheMap[name]) : "<none>"

        if (!sigs.Has(name) && DERIVED_OFFSETS.Has(name)) {
            spec := DERIVED_OFFSETS[name]
            srcStr := cacheMap.Has(spec.from) ? Format("0x{:08X}", cacheMap[spec.from]) : "<unresolved>"
            live := GetResolvedOffset(name)
            derivedOk := gResolvedOffsets.Has(name)
            if (derivedOk) {
                okCount += 1
            } else {
                failCount += 1
            }
            out .= name " — " (derivedOk ? "DERIVED" : "NOT DERIVED (source unresolved)") "`n"
                . "  from  : " spec.from " " srcStr " " (spec.delta < 0 ? "-" : "+") " 0x" Format("{:X}", Abs(spec.delta)) "`n"
                . "  const : " Format("0x{:08X}", constRva) "`n"
                . "  in use: " Format("0x{:08X}", live) "`n`n"
            continue
        }
        if !sigs.Has(name) {
            failCount += 1
            out .= name " — NO SIGNATURE`n"
                . "  const : " Format("0x{:08X}", constRva) "`n"
                . "  cached: " cacheStr "`n`n"
            continue
        }
        entry := sigs[name]
        res := ResolveRvaFromSignature(cached.modBase, pe.codeBuf, pe.codeSize, entry.sig, entry.opOffset)
        if !res.ok {
            failCount += 1
            out .= name " — SCAN FAILED (" res.reason ")`n"
                . "  const : " Format("0x{:08X}", constRva) "`n"
                . "  cached: " cacheStr "`n`n"
            continue
        }
        scanStr := Format("0x{:08X}", res.rva)
        cacheMatches := cacheMap.Has(name) ? (res.rva = cacheMap[name]) : false
        constMatches := (res.rva = constRva)

        verdict := ""
        if (constMatches && cacheMatches) {
            verdict := "OK (all match)"
            okCount += 1
        } else if (cacheMatches) {
            verdict := "OK (scan = cache; constant is stale, expected after a patch)"
            okCount += 1
        } else if (constMatches) {
            verdict := "OK (scan = constant; cache empty)"
            okCount += 1
        } else {
            verdict := "MISMATCH"
            failCount += 1
        }

        out .= name " — " verdict "`n"
            . "  const : " Format("0x{:08X}", constRva) "`n"
            . "  cached: " cacheStr "`n"
            . "  scan  : " scanStr "`n`n"
    }
    ; The per-signature breakdown is column-aligned hex, so it goes in the
    ; dialog's monospace detail block and the verdict stands on its own above it.
    ShowMessage("Build " Format("0x{:08X}", pe.timeDateStamp) " — "
        okCount " ok, " failCount " problem" (failCount = 1 ? "" : "s") ".",
        "Verify Signatures",
        Map("severity", failCount ? "warn" : "success", "detail", out))
}

; User-triggered: capture signatures for every name in SIGNATURE_NAMES from the
; current process using the hardcoded constants as known-good RVAs. Run this
; after manually verifying that map name / position / state read correctly in
; variables.ahk. Persists signatures and the resolved offsets for this build.
CalibrateSignaturesNow() {
    cached := GetCachedProcessHandleAndBase()
    if !cached.ok {
        ShowMessage("Game process not found. Launch the game first.",
            "Calibrate Signatures", Map("severity", "warn"))
        return
    }
    pe := ReadPEInfo(cached.handle, cached.modBase)
    if !pe.ok {
        ShowMessage("Failed to read PE info: " pe.reason, "Calibrate Signatures",
            Map("severity", "danger"))
        return
    }
    if (pe.machine != 0x14C) {
        ShowMessage("Target is not 32-bit (machine=0x" Format("{:04X}", pe.machine) ")."
            . "`nOnly 32-bit absolute operand scans are implemented.",
            "Calibrate Signatures", Map("severity", "danger"))
        return
    }

    summary := ""
    failures := 0
    for _, name in SIGNATURE_NAMES {
        ; Derived offsets have no signature by design — EnsureResolvedOffsetsForBuild
        ; computes them from their source below.
        if DERIVED_OFFSETS.Has(name) {
            summary .= name ": derived from " DERIVED_OFFSETS[name].from " (no signature needed)`n"
            continue
        }
        rva := gFallbackOffsets[name]
        cap := CaptureSignatureForRva(cached.modBase, pe.codeBuf, pe.codeSize, pe.codeRva, rva)
        if !cap.ok {
            failures += 1
            summary .= name ": FAILED (" cap.reason
            if cap.HasOwnProp("refCount") {
                summary .= ", refs=" cap.refCount
            }
            summary .= ")`n"
            continue
        }
        SaveSignatureIni(name, cap.sig, cap.opOffset)
        SaveOffsetForBuild(pe.timeDateStamp, name, rva)
        summary .= name ": ok (refs=" cap.refCount ", sigLen=" cap.sigLen ")`n"
    }

    ; Refresh in-memory resolved map so the new offsets are used immediately.
    global gResolvedOffsets, gResolvedBuildStamp
    gResolvedBuildStamp := 0
    EnsureResolvedOffsetsForBuild(cached.handle, cached.modBase)

    summary .= "`nWrote " SIGNATURES_INI
    summary .= "`nWrote " OFFSETS_CACHE_INI

    headline := "Build " Format("0x{:08X}", pe.timeDateStamp) " — "
        . (failures
            ? failures " signature(s) could not be uniquely captured, so those"
              . " RVAs fall back to the hardcoded constants."
            : "every signature captured.")
    ShowMessage(headline, "Calibrate Signatures",
        Map("severity", failures ? "warn" : "success", "detail", summary))
}

; ── Addon system ─────────────────────────────────────────────────

; Registers an addon-owned RVA into the shared signature/rescan system.
; Call at global scope (outside any function) so it runs at include time.
; After this, CalibrateSignaturesNow() and EnsureResolvedOffsetsForBuild()
; will scan/cache the offset alongside core app offsets, and
; GetResolvedOffset(name) returns the live-resolved value or the fallback.
; Registering the same name twice is expected: two addons can legitimately share
; an address (see BattleAction, used by both battle_send and better_hotkeys), and
; each must register it so every variant that ships either one resolves it. The
; first registration wins — a duplicate would otherwise be scanned twice and
; listed twice in the Verify Signatures report.
RegisterAddonOffset(name, fallbackRva) {
    global SIGNATURE_NAMES, gFallbackOffsets
    if gFallbackOffsets.Has(name)
        return
    SIGNATURE_NAMES.Push(name)
    gFallbackOffsets[name] := fallbackRva
}

RegisterAddon(addonMap) {
    global gAddonHooks
    if !addonMap.Has("name") || addonMap["name"] = "" {
        TrayTip("RegisterAddon() called without a 'name' key — ignored.", "Addon System", "Iconx")
        return
    }
    gAddonHooks.Push(addonMap)
}

FireAddonHook(hookName, params*) {
    global gAddonHooks, gDisabledAddons
    for _, addonMap in gAddonHooks {
        if !addonMap.Has(hookName)
            continue
        addonName := addonMap.Has("name") ? addonMap["name"] : "<unnamed>"
        if gDisabledAddons.Has(addonName) && gDisabledAddons[addonName]
            continue
        fn := addonMap[hookName]
        if !(fn is Func)
            continue
        try {
            fn(params*)
            _AddonHookSucceeded(addonName, hookName)
        } catch as err {
            _AddonHookFailed(addonName, hookName, err)
        }
    }
}

; A hook that works again clears its streak, so an addon that fails
; intermittently is never quarantined for failures spread over a session.
_AddonHookSucceeded(addonName, hookName) {
    global gAddonFailCounts
    key := addonName "|" hookName
    if gAddonFailCounts.Has(key)
        gAddonFailCounts.Delete(key)
}

; Every failure is logged; the user hears about it at most once per
; ADDON_FAIL_TIP_MS, and an addon that keeps throwing on the same hook is put
; out of the dispatch loop for the rest of the session.
;
; The case this exists for: an addon throwing from OnSnapshot used to raise a
; TrayTip once a second, forever, with no record of what actually went wrong and
; no way for the user to stop it.
_AddonHookFailed(addonName, hookName, err) {
    global gAddonFailCounts, gAddonFailLastTip, gDisabledAddons, gQuarantinedAddons
    global ADDON_FAIL_TIP_MS, ADDON_FAIL_QUARANTINE

    key := addonName "|" hookName
    count := (gAddonFailCounts.Has(key) ? gAddonFailCounts[key] : 0) + 1
    gAddonFailCounts[key] := count

    LogError(addonName, hookName " (failure " count "): " err.Message
        . (err.HasProp("Line") ? "  line " err.Line : ""))

    if (count >= ADDON_FAIL_QUARANTINE) {
        ; Session-only: gDisabledAddons is what FireAddonHook already consults,
        ; and deliberately no IniWrite — a restart gives the addon another go.
        gDisabledAddons[addonName] := true
        gQuarantinedAddons[addonName] := hookName
        gAddonFailCounts.Delete(key)
        LogError(addonName, "Quarantined for this session after " count
            . " consecutive failures in " hookName ".")
        TrayTip(addonName " kept failing in " hookName ", so it has been turned"
            . " off for this session.`nDetails: " LogPath(),
            "Addon Quarantined", "Iconx")
        ; The tray reflects enabled state, so let it catch up.
        SetTimer(RebuildTrayMenu, -1)
        return
    }

    now := A_TickCount
    if (gAddonFailLastTip.Has(key) && now - gAddonFailLastTip[key] < ADDON_FAIL_TIP_MS)
        return
    gAddonFailLastTip[key] := now
    TrayTip(hookName ": " err.Message, "Addon Error [" addonName "]", "Iconx")
}

; True when at least one *enabled* addon registers this hook. Lets producers
; (e.g. the client snapshot poll) skip work nobody is listening for.
HasEnabledAddonHook(hookName) {
    global gAddonHooks, gDisabledAddons
    for _, addonMap in gAddonHooks {
        if !addonMap.Has(hookName) {
            continue
        }
        name := addonMap.Has("name") ? addonMap["name"] : ""
        if (name != "" && gDisabledAddons.Has(name) && gDisabledAddons[name]) {
            continue
        }
        return true
    }
    return false
}

LoadAddonEnabledStates() {
    global gAddonHooks, gDisabledAddons, CONFIG_INI
    if !FileExist(CONFIG_INI)
        return
    for _, addonMap in gAddonHooks {
        name := addonMap.Has("name") ? addonMap["name"] : ""
        if (name = "")
            continue
        val := Trim(IniRead(CONFIG_INI, "Addons", name, "1"))
        if (val = "0")
            gDisabledAddons[name] := true
    }
}

; Sets an addon's enabled state and persists it to config.ini.
; Enable/disable takes effect live for hook dispatch (FireAddonHook reads
; gDisabledAddons each fire). Hotkeys for disabled addons are skipped on the
; next ApplyAllHotkeys() (e.g. when Settings is saved).
SetAddonEnabled(addonName, enabled) {
    global gDisabledAddons, CONFIG_INI
    if (enabled) {
        if gDisabledAddons.Has(addonName)
            gDisabledAddons.Delete(addonName)
        IniWrite("1", CONFIG_INI, "Addons", addonName)
    } else {
        gDisabledAddons[addonName] := true
        IniWrite("0", CONFIG_INI, "Addons", addonName)
    }
}

GenerateAddonIncludes() {
    global ADDONS_DIR, ADDONS_INCLUDE_FILE
    addonFiles := []
    if DirExist(ADDONS_DIR) {
        loop files, ADDONS_DIR "\*.ahk" {
            addonFiles.Push(A_LoopFileName)
        }
        addonFiles := _SortStrArray(addonFiles)
    }

    newContent := ""
    for _, fname in addonFiles {
        newContent .= "#Include addons\" fname "`n"
    }

    existingContent := FileExist(ADDONS_INCLUDE_FILE) ? FileRead(ADDONS_INCLUDE_FILE) : ""
    if (newContent = existingContent)
        return

    if FileExist(ADDONS_INCLUDE_FILE)
        FileDelete(ADDONS_INCLUDE_FILE)
    if (newContent != "")
        FileAppend(newContent, ADDONS_INCLUDE_FILE, "UTF-8")

    Reload()
}

_SortStrArray(arr) {
    joined := ""
    for _, v in arr
        joined .= v "`n"
    sorted := Sort(Trim(joined, "`n"), "D`n")
    out := []
    loop parse, sorted, "`n" {
        if (A_LoopField != "")
            out.Push(A_LoopField)
    }
    return out
}
