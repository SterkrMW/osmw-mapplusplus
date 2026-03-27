; ── Process & memory ──────────────────────────────────────────────

GetGameProcessId() {
    global gTrackedGameHwnd
    if WinActive(GAME_WIN_FILTER) {
        return WinGetPID("A")
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
    return { ok: true, handle: processHandle, modBase: moduleBase, pid: pid }
}

ReleaseCachedProcessHandle() {
    global gCachedPID, gCachedProcessHandle, gCachedModuleBase
    if gCachedProcessHandle {
        DllCall("CloseHandle", "Ptr", gCachedProcessHandle)
    }
    gCachedPID := 0
    gCachedProcessHandle := 0
    gCachedModuleBase := 0
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

; Reads the map filename from memory (e.g. "MAP007.map") and returns the
; base name with .map swapped for .jpg (e.g. "MAP007.jpg") for minimap lookup.
ReadCurrentMapName() {
    global gLastReadStatus

    cached := GetCachedProcessHandleAndBase()
    if !cached.ok {
        gLastReadStatus := "process_not_found"
        return ""
    }

    targetAddr := cached.modBase + MAP_FILE_OFFSET
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
        "Ptr", cached.modBase + POS_X_OFFSET,
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

; ── Overlay positioning ──────────────────────────────────────────

GetOverlayPositionForGameWindow() {
    global OVERLAY_W, OVERLAY_H, MINIMAP_MAP_INSET
    totalW := OVERLAY_W + 2 * MINIMAP_MAP_INSET
    totalH := OVERLAY_H + 2 * MINIMAP_MAP_INSET
    ; Center overlay against the active game window when possible.
    hwnd := WinActive(GAME_WIN_FILTER)
    if !hwnd {
        hwnd := WinExist(GAME_WIN_FILTER)
    }

    if hwnd {
        WinGetPos(&winX, &winY, &winW, &winH, "ahk_id " hwnd)
        return {
            x: Floor(winX + ((winW - totalW) / 2)),
            y: Floor(winY + ((winH - totalH) / 2) + 12)
        }
    }

    ; Fallback if game window isn't found.
    return {
        x: Floor((A_ScreenWidth - totalW) / 2),
        y: Floor((A_ScreenHeight - totalH) / 2)
    }
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
    gMarkerDot := gGui.AddPicture("x0 y0 w" MARKER_SIZE " h" MARKER_SIZE " Hidden", MARKER_PNG)
}
