#Requires AutoHotkey v2.0

global _WindowLayout_DefaultLayout := "Grid2x2"
global _WindowLayout_MainCharacter  := ""
global _WindowLayout_TargetMonitor  := 0  ; 0 = primary

; The built-in layouts. Custom (user-authored) layouts live in layouts.ini and
; are resolved by name too, so a custom layout may not reuse one of these names.
global _WindowLayout_PRESETS := ["Reset", "Single", "Grid2x2", "Grid3x2", "CenterFocus", "DiceLeft", "DiceRight"]

; Clients that don't fit the slot list are offset by this much per extra pass, so
; they cascade behind the others instead of hiding exactly underneath them.
global _WindowLayout_CASCADE_STEP := 28

; Where every participating window sat immediately before the last apply, so a
; layout that lands badly is one click away from being undone. Memory only.
global _WindowLayout_Undo := []

; Custom layout store. Path resolved lazily (it touches the disk); the caches
; are dropped on every write, mirroring the POI store.
global _WLayouts_Ini   := ""
global _WLayouts_Cache := Map()   ; id → layout object
global _WLayouts_Index := 0       ; 0 until loaded, then [{id, name}]

; The exact name array the open Settings window's layout dropdown was built from.
global _WindowLayout_SettingsNames := 0

; Native manager window state.
global _WindowLayout_MgrGui  := 0
global _WindowLayout_MgrList := 0
global _WindowLayout_MgrRows := []   ; ListView row → layout id

; WebView2 editor state. _WebChars is the character table the page was last sent;
; slots come back as 1-based indexes into it rather than as names.
global _WindowLayout_WebGui      := 0
global _WindowLayout_WebSelected := 0
global _WindowLayout_WebChars    := []

; Startup main-character prompt state. Keep this modeless so a frontend or
; message failure can never park the whole app inside WinWaitClose.
global _WindowLayout_PromptGui       := 0
global _WindowLayout_PromptNamesJson := "[]"

RegisterAddon(Map(
    "name",              "WindowLayout",
    "settingsLabel",     "Window Layout",
    "OnTrayMenu",        _WindowLayout_OnTrayMenu,
    "OnSettingsWeb",     _WindowLayout_OnSettingsWeb,
    "OnSettingsWebSave", _WindowLayout_OnSettingsWebSave,
    "OnInit",            _WindowLayout_OnInit
))

RegisterHotkeyAction(Map(
    "id", "windowLayoutPrimary",
    "label", "Apply default layout (primary monitor)",
    "category", "Window Layout",
    "default", "^+l",
    "addon", "WindowLayout",
    "handler", (*) => _WindowLayout_ApplyDefaultLayout(MonitorGetPrimary()),
    "hotIfWinActive", true
))
RegisterHotkeyAction(Map(
    "id", "windowLayoutSecondary",
    "label", "Apply default layout (secondary monitor)",
    "category", "Window Layout",
    "default", "^+k",
    "addon", "WindowLayout",
    "handler", (*) => _WindowLayout_ApplyDefaultLayout(GetSecondaryMonitorIndex()),
    "hotIfWinActive", true
))
RegisterHotkeyAction(Map(
    "id", "windowLayoutManage",
    "label", "Open window layouts",
    "category", "Window Layout",
    "default", "^+o",
    "addon", "WindowLayout",
    "handler", (*) => _WindowLayout_ShowManager()
))
RegisterHotkeyAction(Map(
    "id", "windowLayoutUndo",
    "label", "Undo last layout apply",
    "category", "Window Layout",
    "default", "^+z",
    "addon", "WindowLayout",
    "handler", (*) => _WindowLayout_UndoLastApply()
))

_WindowLayout_OnInit() {
    _WindowLayout_LoadConfig()
    ; Let startup and the first client snapshot settle before offering a choice.
    SetTimer(_WindowLayout_PromptMainCharacterIfUnset, -4000)
}

_WindowLayout_PromptMainCharacterIfUnset() {
    global _WindowLayout_MainCharacter
    if (_WindowLayout_MainCharacter = "")
        _WindowLayout_PromptMainCharacter()
}

_WindowLayout_OnTrayMenu(trayMenu) {
    ; Action items only — configuration (default layout, main character, target
    ; display) lives in the Settings window's Window Layout tab.
    layoutMenu := Menu()
    layoutMenu.Add("Apply (Primary)`t" GetHotkeyDisplay("windowLayoutPrimary"), (*) => _WindowLayout_ApplyDefaultLayout(MonitorGetPrimary()))
    layoutMenu.Add("Apply (Secondary)`t" GetHotkeyDisplay("windowLayoutSecondary"), (*) => _WindowLayout_ApplyDefaultLayout(GetSecondaryMonitorIndex()))

    applyMenu := Menu()
    for name in _WindowLayout_PRESETS
        applyMenu.Add(name, _WindowLayout_ApplyPreset.Bind(name))
    layoutMenu.Add("Apply Preset", applyMenu)

    ; Rebuilt on every right-click, so saved layouts stay in step for free.
    saved := _WLayouts_LoadIndex()
    if saved.Length {
        customMenu := Menu()
        for i, entry in saved {
            if (i > 20)
                break
            customMenu.Add(entry.name, _WindowLayout_ApplyNamed.Bind(entry.name, 0))
        }
        layoutMenu.Add("Apply Custom", customMenu)
    }

    layoutMenu.Add()
    layoutMenu.Add("Capture Current As…", (*) => _WindowLayout_CaptureAsPrompt())
    layoutMenu.Add("Layouts…`t" GetHotkeyDisplay("windowLayoutManage"), (*) => _WindowLayout_ShowManager())
    layoutMenu.Add("Undo Last Apply`t" GetHotkeyDisplay("windowLayoutUndo"), (*) => _WindowLayout_UndoLastApply())

    trayMenu.Add("Window Layout", layoutMenu)
}

_WindowLayout_OnSettingsWeb() {
    global _WindowLayout_DefaultLayout, _WindowLayout_MainCharacter, _WindowLayout_TargetMonitor
    global _WindowLayout_SettingsNames
    ; The dropdown reports a 0-based index, so the save has to read back the very
    ; same array — a layout deleted while Settings is open would otherwise shift
    ; every entry after it.
    presets := _WindowLayout_AllLayoutNames()
    _WindowLayout_SettingsNames := presets

    names := _WindowLayout_GetCharacterNames()
    if (_WindowLayout_MainCharacter != "" && !_WindowLayout_ArrayHas(names, _WindowLayout_MainCharacter))
        names.InsertAt(1, _WindowLayout_MainCharacter)

    monChoices := ["Primary (auto)"]
    Loop MonitorGetCount()
        monChoices.Push(_WindowLayout_DisplayLabel(A_Index))

    layoutIdx := _WindowLayout_IndexOf(presets, _WindowLayout_DefaultLayout, 3) - 1
    monIdx := (_WindowLayout_TargetMonitor >= 1 && _WindowLayout_TargetMonitor <= MonitorGetCount())
        ? _WindowLayout_TargetMonitor : 0

    return [
        Map("type", "dropdown", "id", "layoutIdx", "label", "Default layout:",
            "options", presets, "value", layoutIdx),
        Map("type", "combo", "id", "mainChar", "label", "Main character:",
            "options", names, "value", _WindowLayout_MainCharacter),
        Map("type", "dropdown", "id", "targetMonitorIdx", "label", "Target display:",
            "options", monChoices, "value", monIdx)
    ]
}

_WindowLayout_OnSettingsWebSave(values) {
    global _WindowLayout_SettingsNames
    presets := (IsObject(_WindowLayout_SettingsNames) && _WindowLayout_SettingsNames.Length)
        ? _WindowLayout_SettingsNames : _WindowLayout_AllLayoutNames()
    layoutIdx := values.Has("layoutIdx") ? Integer(values["layoutIdx"]) + 1 : 3
    if (layoutIdx < 1 || layoutIdx > presets.Length)
        layoutIdx := _WindowLayout_IndexOf(presets, "Grid2x2", 1)

    mainChar := values.Has("mainChar") ? Trim(values["mainChar"]) : ""
    targetMonIdx := values.Has("targetMonitorIdx") ? Integer(values["targetMonitorIdx"]) : 0

    _WindowLayout_ApplySettings(presets[layoutIdx], mainChar, targetMonIdx)
}

; Persists the values chosen in the Settings window's Window Layout tab.
_WindowLayout_ApplySettings(layout, mainChar, targetMonitor) {
    global _WindowLayout_DefaultLayout, _WindowLayout_MainCharacter, _WindowLayout_TargetMonitor
    _WindowLayout_DefaultLayout := layout
    _WindowLayout_MainCharacter := mainChar
    _WindowLayout_TargetMonitor := targetMonitor
    _WindowLayout_SaveConfig()
}

; 1-based position of needle in arr, or fallbackIdx when not present.
_WindowLayout_IndexOf(arr, needle, fallbackIdx) {
    for i, v in arr
        if (v = needle)
            return i
    return fallbackIdx
}

_WindowLayout_ArrayHas(arr, needle) {
    for v in arr
        if (v = needle)
            return true
    return false
}

_WindowLayout_LoadConfig() {
    global _WindowLayout_DefaultLayout, _WindowLayout_MainCharacter, _WindowLayout_TargetMonitor, CONFIG_INI
    _WindowLayout_DefaultLayout := Trim(IniRead(CONFIG_INI, "WindowLayout", "DefaultLayout", "Grid2x2"))
    _WindowLayout_MainCharacter := Trim(IniRead(CONFIG_INI, "WindowLayout", "MainCharacter", ""))
    _WindowLayout_TargetMonitor := Integer(IniRead(CONFIG_INI, "WindowLayout", "TargetMonitor", 0))
}

_WindowLayout_SaveConfig() {
    global _WindowLayout_DefaultLayout, _WindowLayout_MainCharacter, _WindowLayout_TargetMonitor, CONFIG_INI
    IniWrite(_WindowLayout_DefaultLayout, CONFIG_INI, "WindowLayout", "DefaultLayout")
    IniWrite(_WindowLayout_MainCharacter, CONFIG_INI, "WindowLayout", "MainCharacter")
    IniWrite(_WindowLayout_TargetMonitor, CONFIG_INI, "WindowLayout", "TargetMonitor")
}

_WindowLayout_PromptMainCharacter() {
    global _WindowLayout_MainCharacter, _WindowLayout_PromptGui, _WindowLayout_PromptNamesJson

    if IsObject(_WindowLayout_PromptGui) {
        try {
            _WindowLayout_PromptGui.Show()
            WinActivate("ahk_id " _WindowLayout_PromptGui.Hwnd)
        }
        return
    }

    if GetTopLevelGameWindows().Length = 0
        return

    names := _WindowLayout_GetCharacterNames()
    if names.Length = 0 {
        return
    }

    if IsNativeInterface() {
        dlg := Gui("+AlwaysOnTop -MinimizeBox", "Window Layout — Main Character")
        _WindowLayout_PromptGui := dlg
        dlg.SetFont("s9", "Segoe UI")
        dlg.Add("Text", "w240", "Select the main character:")
        ddl := dlg.Add("DropDownList", "w240", names)
        for i, name in names {
            if (name = _WindowLayout_MainCharacter) {
                ddl.Value := i
                break
            }
        }
        if (ddl.Value = 0)
            ddl.Value := 1
        dlg.Add("Button", "Default w90 xm y+12", "OK")
            .OnEvent("Click", (*) => _WindowLayout_ClosePrompt(ddl.Text))
        dlg.Add("Button", "w90 x+8", "Cancel").OnEvent("Click", (*) => _WindowLayout_ClosePrompt())
        dlg.OnEvent("Close", (*) => _WindowLayout_ClosePrompt())
        dlg.OnEvent("Escape", (*) => _WindowLayout_ClosePrompt())
        dlg.Show("AutoSize")
        return
    }

    dllDir := (A_PtrSize = 8) ? "64bit" : "32bit"
    dllPath := A_ScriptDir "\Lib\" dllDir "\WebView2Loader.dll"
    wvSettings := { DllPath: dllPath, DefaultWidth: 320, DefaultHeight: 200 }

    dlg := WebViewGui("-Caption +AlwaysOnTop -Resize", "Window Layout — Main Character",, wvSettings)
    _WindowLayout_PromptGui := dlg
    dlg.OnEvent("Close", (*) => _WindowLayout_ClosePrompt())

    ; Build separators from the output string. A compact same-line `if` here
    ; previously appended the comma unconditionally and produced `[,"Name"]`,
    ; which WebView2 correctly rejects as invalid JSON.
    namesJson := ""
    for n in names
        namesJson .= (namesJson = "" ? "" : ",") _JSON_Str(n)
    namesJson := "[" namesJson "]"
    _WindowLayout_PromptNamesJson := namesJson

    dlg.WebMessageReceived(_WindowLayout_OnPromptWebMsg)
    dlg.DOMContentLoaded((*) => SetTimer(_WindowLayout_SendPromptState, -50))

    dlg.Navigate("ui/window_layout/prompt.html")
    dlg.Show("w320 h200")
}

_WindowLayout_SendPromptState() {
    global _WindowLayout_MainCharacter, _WindowLayout_PromptGui, _WindowLayout_PromptNamesJson
    if !IsObject(_WindowLayout_PromptGui)
        return
    try _WindowLayout_PromptGui.PostWebMessageAsJson('{"type":"init-prompt"'
        . ',"names":' _WindowLayout_PromptNamesJson
        . ',"selectedName":' _JSON_Str(_WindowLayout_MainCharacter) '}')
}

_WindowLayout_OnPromptWebMsg(wv, args) {
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
            _WindowLayout_SendPromptState()
        case "accept":
            chosen := msg.Has("chosen") ? msg["chosen"] : ""
            _WindowLayout_ClosePrompt(chosen)
        case "cancel":
            _WindowLayout_ClosePrompt()
    }
}

_WindowLayout_ClosePrompt(chosen := "") {
    global _WindowLayout_MainCharacter, _WindowLayout_PromptGui, _WindowLayout_PromptNamesJson
    if (chosen != "") {
        _WindowLayout_MainCharacter := chosen
        _WindowLayout_SaveConfig()
    }
    if IsObject(_WindowLayout_PromptGui)
        try _WindowLayout_PromptGui.Destroy()
    _WindowLayout_PromptGui := 0
    _WindowLayout_PromptNamesJson := "[]"
}

_WindowLayout_GetCharacterNames() {
    seen  := Map()
    names := []
    for hwnd in GetTopLevelGameWindows() {
        ; Shared with the roster and Discord presence — one title format to fix.
        name := CharacterNameFromWindow(hwnd)
        if (name != "" && !seen.Has(name)) {
            seen[name] := true
            names.Push(name)
        }
    }
    return names
}

_WindowLayout_ApplyDefaultLayout(monitorIdx := unset) {
    global _WindowLayout_DefaultLayout
    _WindowLayout_ApplyNamed(_WindowLayout_DefaultLayout,
        (IsSet(monitorIdx) && monitorIdx is Integer) ? monitorIdx : 0)
}

_WindowLayout_ApplyPreset(layoutName, monitorIdx := unset, *) {
    if !_WindowLayout_IsPreset(layoutName)
        return
    monIdx := (IsSet(monitorIdx) && monitorIdx is Integer) ? monitorIdx : _WindowLayout_ConfiguredMonitor()
    windows := FilterWindowsOnMonitor(_WindowLayout_ArrangeableWindows(), monIdx)
    if (windows.Length = 0) {
        TrayTip("No game windows on " _WindowLayout_DisplayLabel(monIdx) "."
            . (_WindowLayout_MinimizedCount() ? "`nMinimized clients are left alone." : ""),
            "Window Layout", "Iconi")
        return
    }
    ordered := _WindowLayout_OrderWindows(windows)
    _WindowLayout_RecordUndo(ordered)

    if layoutName = "Reset" {
        MonitorGetWorkArea(monIdx, &wl, &wt)
        for hwnd in ordered
            WinMove(wl, wt, , , "ahk_id " hwnd)
        _WindowLayout_ActivateMainCharacter(ordered)
        return
    }

    ; Use the first window's size to compute positions — never resize.
    WinGetPos(, , &winW, &winH, "ahk_id " ordered[1])
    slots := _WindowLayout_ComputeSlots(layoutName, winW, winH, monIdx)
    if (slots.Length = 0)
        return

    Loop ordered.Length {
        hwnd := ordered[A_Index]
        slot := slots[Mod(A_Index - 1, slots.Length) + 1]
        ; Past the end of the slot list, cascade rather than hide a window
        ; exactly underneath the one already there.
        pass := (A_Index - 1) // slots.Length
        _WindowLayout_MoveInto(hwnd, slot.x, slot.y, monIdx, pass)
    }
    if (ordered.Length > slots.Length) {
        TrayTip(slots.Length " slots, " ordered.Length " clients — "
            . (ordered.Length - slots.Length) " offset behind the others.", "Window Layout", "Iconi")
    }
    _WindowLayout_ActivateMainCharacter(ordered)
}

_WindowLayout_IsPreset(name) {
    global _WindowLayout_PRESETS
    for n in _WindowLayout_PRESETS
        if (n = name)
            return true
    return false
}

; Moves a window to x/y, cascaded by `pass` steps and clamped so it always stays
; reachable inside the target monitor's work area. Never changes window size.
_WindowLayout_MoveInto(hwnd, x, y, monIdx, pass := 0) {
    global _WindowLayout_CASCADE_STEP
    if (pass > 0) {
        x += _WindowLayout_CASCADE_STEP * pass
        y += _WindowLayout_CASCADE_STEP * pass
    }
    WinGetPos(, , &w, &h, "ahk_id " hwnd)
    pt := _WindowLayout_ClampToWorkArea(x, y, w, h, monIdx)
    WinMove(pt.x, pt.y, , , "ahk_id " hwnd)
}

; Keeps a window's top-left inside the work area. A window wider or taller than
; the work area is pinned to the origin rather than pushed off the left/top.
_WindowLayout_ClampToWorkArea(x, y, w, h, monIdx) {
    MonitorGetWorkArea(monIdx, &wl, &wt, &wr, &wb)
    maxX := wr - w
    maxY := wb - h
    return {
        x: (maxX < wl) ? wl : Clamp(x, wl, maxX),
        y: (maxY < wt) ? wt : Clamp(y, wt, maxY)
    }
}

_WindowLayout_ComputeSlots(layoutName, winW, winH, monIdx) {
    MonitorGetWorkArea(monIdx, &wl, &wt, &wr, &wb)
    sw := wr - wl   ; usable width  (excludes taskbar)
    sh := wb - wt   ; usable height (excludes taskbar)
    ox := wl        ; x origin — non-zero when taskbar is on the left
    oy := wt        ; y origin — non-zero when taskbar is on the top
    if layoutName = "Single"
        return [{x: ox + (sw - winW) // 2, y: oy + (sh - winH) // 2}]
    if layoutName = "Grid2x2"
        return [
            {x: ox,        y: oy},
            {x: ox + winW, y: oy},
            {x: ox,        y: oy + winH},
            {x: ox + winW, y: oy + winH}
        ]
    if layoutName = "Grid3x2" {
        pad  := 8
        gridW := 3 * winW + 2 * pad
        gridH := 2 * winH + pad
        bx   := ox + Max(0, (sw - gridW) // 2)
        by   := oy + Max(0, (sh - gridH) // 2)
        col1 := bx
        col2 := bx + winW + pad
        col3 := bx + 2 * (winW + pad)
        row2 := by + winH + pad
        return [
            {x: col1, y: by},   {x: col2, y: by},   {x: col3, y: by},
            {x: col1, y: row2}, {x: col2, y: row2}, {x: col3, y: row2}
        ]
    }
    if layoutName = "CenterFocus"
        return [
            {x: ox + (sw - winW) // 2, y: oy + (sh - winH) // 2},  ; center — main char
            {x: ox,                    y: oy},                        ; top-left
            {x: ox + sw - winW,        y: oy},                        ; top-right
            {x: ox,                    y: oy + sh - winH},            ; bottom-left
            {x: ox + sw - winW,        y: oy + sh - winH}             ; bottom-right
        ]
    if layoutName = "DiceLeft" || layoutName = "DiceRight" {
        clusterW := 2 * winW
        bx := (layoutName = "DiceLeft") ? ox : ox + sw - clusterW
        topY    := oy
        bottomY := oy + sh - winH
        centerY := oy + (sh - winH) // 2
        return [
            {x: bx + winW // 2, y: centerY},  ; center — main char (drawn on top)
            {x: bx,             y: topY},     ; top-left
            {x: bx + winW,      y: topY},     ; top-right
            {x: bx,             y: bottomY},  ; bottom-left
            {x: bx + winW,      y: bottomY}   ; bottom-right
        ]
    }
    return []
}

; Main character first, then everyone else sorted by their current on-screen
; position (top to bottom, then left to right). Raw WinGetList order is z-order,
; which changes on every alt-tab, so sorting is what makes a repeated apply put
; the same client back in the same slot.
_WindowLayout_OrderWindows(windows) {
    global _WindowLayout_MainCharacter
    mainHwnd := _WindowLayout_WindowForCharacter(windows, _WindowLayout_MainCharacter)

    others := []
    for hwnd in windows {
        if (hwnd != mainHwnd)
            others.Push(hwnd)
    }
    others := _WindowLayout_SortByPosition(others)

    result := []
    if mainHwnd
        result.Push(mainHwnd)
    for hwnd in others
        result.Push(hwnd)
    return result
}

; Insertion sort by top-left, banding y so windows roughly on the same row are
; ordered left to right rather than by a few pixels of vertical drift.
_WindowLayout_SortByPosition(windows) {
    static ROW_BAND := 40
    keyed := []
    for hwnd in windows {
        try WinGetPos(&x, &y, , , "ahk_id " hwnd)
        catch
            continue
        keyed.Push({hwnd: hwnd, x: x, y: y})
    }
    Loop keyed.Length - 1 {
        i := A_Index + 1
        item := keyed[i]
        j := i - 1
        while (j >= 1) {
            prev := keyed[j]
            sameRow := Abs(prev.y - item.y) <= ROW_BAND
            after := sameRow ? (prev.x > item.x) : (prev.y > item.y)
            if !after
                break
            keyed[j + 1] := prev
            j -= 1
        }
        keyed[j + 1] := item
    }
    result := []
    for entry in keyed
        result.Push(entry.hwnd)
    return result
}

; Exact character-name match, not a title substring — "Bob" must not claim
; "Bobby"'s window. AHK's `=` is already case-insensitive.
_WindowLayout_WindowForCharacter(windows, name) {
    if (name = "")
        return 0
    for hwnd in windows {
        if (CharacterNameFromWindow(hwnd) = name)
            return hwnd
    }
    return 0
}

_WindowLayout_ActivateMainCharacter(ordered) {
    global _WindowLayout_MainCharacter
    if (_WindowLayout_MainCharacter = "" || ordered.Length = 0)
        return
    hwnd := _WindowLayout_WindowForCharacter(ordered, _WindowLayout_MainCharacter)
    if hwnd
        try WinActivate("ahk_id " hwnd)
}

; ── Window selection ─────────────────────────────────────────────

; Windows reports a minimized window as 160x28 sitting at -32000,-32000, which
; would capture as a wildly out-of-range position and, on apply, would eat a slot
; without anything visibly moving. A minimized client sits out instead.
_WindowLayout_IsMinimized(hwnd) {
    try return WinGetMinMax("ahk_id " hwnd) = -1
    ; Unreadable means gone or going — treat it as out either way.
    return true
}

; Top-level game windows that can actually be measured and moved.
_WindowLayout_ArrangeableWindows() {
    result := []
    for hwnd in GetTopLevelGameWindows() {
        if !_WindowLayout_IsMinimized(hwnd)
            result.Push(hwnd)
    }
    return result
}

_WindowLayout_MinimizedCount() {
    n := 0
    for hwnd in GetTopLevelGameWindows() {
        if _WindowLayout_IsMinimized(hwnd)
            n += 1
    }
    return n
}

; ── Custom layout store ──────────────────────────────────────────
;
; layouts.ini sits next to config.ini (never in maps\, which build.ps1 copies
; into releases\ and would clobber). Two levels: an [Index] section mapping a
; numeric id to the display name, and one [Layout.<id>] section per layout. The
; indirection means renaming never touches slot data and a name may contain
; anything the sanitiser allows.
;
;   [Index]
;   1=Six Box — Main Left
;
;   [Layout.1]
;   Monitor=1|3840x2160|3840x2120|0,0
;   WinSize=1024x768
;   Slot1=0.0000|0.0000|Sterkr|1
;
; Slot positions are fractions of the authoring monitor's *work area*, so they
; survive a resolution change; WinSize is only ever used to draw boxes to scale
; in the editor and to clamp on apply — window size is never applied.

_WLayouts_Path() {
    global _WLayouts_Ini
    if (_WLayouts_Ini = "")
        _WLayouts_Ini := ResolveWritableIniPath("layouts.ini")
    return _WLayouts_Ini
}

_WLayouts_InvalidateCache() {
    global _WLayouts_Cache, _WLayouts_Index
    _WLayouts_Cache := Map()
    _WLayouts_Index := 0
}

; [{id, name}] ordered by id. Cached until a write.
_WLayouts_LoadIndex() {
    global _WLayouts_Index
    if IsObject(_WLayouts_Index)
        return _WLayouts_Index

    list := []
    path := _WLayouts_Path()
    if FileExist(path) {
        try section := IniRead(path, "Index")
        catch
            section := ""
        loop parse, section, "`n", "`r" {
            line := Trim(A_LoopField)
            eq := InStr(line, "=")
            if (line = "" || !eq)
                continue
            id := Trim(SubStr(line, 1, eq - 1))
            name := Trim(SubStr(line, eq + 1))
            if (!IsInteger(id) || Integer(id) < 1 || name = "")
                continue
            list.Push({id: Integer(id), name: name})
        }
    }
    ; Numeric order, so the manager list matches creation order.
    Loop list.Length - 1 {
        i := A_Index + 1
        item := list[i]
        j := i - 1
        while (j >= 1 && list[j].id > item.id) {
            list[j + 1] := list[j]
            j -= 1
        }
        list[j + 1] := item
    }
    _WLayouts_Index := list
    return list
}

_WLayouts_NameForId(id) {
    for entry in _WLayouts_LoadIndex() {
        if (entry.id = id)
            return entry.name
    }
    return ""
}

; A layout object, or 0 when the id is unknown. Malformed slot lines are skipped
; rather than failing the whole layout.
_WLayouts_Load(id) {
    global _WLayouts_Cache
    if (!IsInteger(id) || id < 1)
        return 0
    id := Integer(id)
    if _WLayouts_Cache.Has(id)
        return _WLayouts_Cache[id]

    name := _WLayouts_NameForId(id)
    if (name = "")
        return 0

    layout := {id: id, name: name, monIndex: 0, fingerprint: "", winW: 0, winH: 0, slots: []}
    path := _WLayouts_Path()
    try section := IniRead(path, "Layout." id)
    catch
        section := ""

    bySlot := Map()
    maxSlot := 0
    loop parse, section, "`n", "`r" {
        line := Trim(A_LoopField)
        eq := InStr(line, "=")
        if (line = "" || !eq)
            continue
        key := Trim(SubStr(line, 1, eq - 1))
        value := SubStr(line, eq + 1)

        if (key = "Monitor") {
            layout.fingerprint := Trim(value)
            parts := StrSplit(layout.fingerprint, "|")
            if (parts.Length >= 1 && IsInteger(Trim(parts[1])))
                layout.monIndex := Integer(Trim(parts[1]))
            continue
        }
        if (key = "WinSize") {
            dims := StrSplit(Trim(value), "x")
            if (dims.Length = 2 && IsInteger(Trim(dims[1])) && IsInteger(Trim(dims[2]))) {
                layout.winW := Integer(Trim(dims[1]))
                layout.winH := Integer(Trim(dims[2]))
            }
            continue
        }
        if (SubStr(key, 1, 4) != "Slot")
            continue
        idx := SubStr(key, 5)
        if (!IsInteger(idx) || Integer(idx) < 1)
            continue

        ; fx|fy|charName|flags — "|" so names may contain commas.
        parts := StrSplit(value, "|")
        if (parts.Length < 2)
            continue
        fx := Trim(parts[1])
        fy := Trim(parts[2])
        if (!IsNumber(fx) || !IsNumber(fy))
            continue
        flags := (parts.Length >= 4 && IsInteger(Trim(parts[4]))) ? Integer(Trim(parts[4])) : 0
        bySlot[Integer(idx)] := {
            fx: Float(fx),
            fy: Float(fy),
            charName: (parts.Length >= 3) ? Trim(parts[3]) : "",
            focus: (flags & 1) ? true : false
        }
        maxSlot := Max(maxSlot, Integer(idx))
    }
    ; Key order from IniRead is file order; walk the indexes so a hand-edited
    ; file still produces slots in the numbering the user wrote.
    Loop maxSlot {
        if bySlot.Has(A_Index)
            layout.slots.Push(bySlot[A_Index])
    }

    _WLayouts_Cache[id] := layout
    return layout
}

_WLayouts_LoadByName(name) {
    if (name = "")
        return 0
    for entry in _WLayouts_LoadIndex() {
        if (entry.name = name)
            return _WLayouts_Load(entry.id)
    }
    return 0
}

_WLayouts_NextId() {
    next := 1
    for entry in _WLayouts_LoadIndex()
        next := Max(next, entry.id + 1)
    return next
}

; Writes the layout, allocating an id when it is 0. Returns the id, or 0 on a
; write failure. Rewrites the whole section so removed slots really disappear.
_WLayouts_Save(layout) {
    path := _WLayouts_Path()
    id := (layout.HasProp("id") && IsInteger(layout.id) && layout.id >= 1) ? Integer(layout.id) : 0
    if (id = 0)
        id := _WLayouts_NextId()

    try {
        IniWrite(layout.name, path, "Index", String(id))
        try IniDelete(path, "Layout." id)
        IniWrite(layout.fingerprint, path, "Layout." id, "Monitor")
        IniWrite(layout.winW "x" layout.winH, path, "Layout." id, "WinSize")
        for i, slot in layout.slots {
            flags := slot.focus ? 1 : 0
            IniWrite(Format("{:.4f}|{:.4f}|{}|{}", slot.fx, slot.fy,
                StrReplace(slot.charName, "|", " "), flags), path, "Layout." id, "Slot" i)
        }
    } catch as err {
        TrayTip("Could not write layouts.ini: " err.Message, "Window Layout", "Iconx")
        return 0
    }
    layout.id := id
    _WLayouts_InvalidateCache()
    return id
}

_WLayouts_Delete(id) {
    path := _WLayouts_Path()
    try IniDelete(path, "Layout." id)
    try IniDelete(path, "Index", String(id))
    _WLayouts_InvalidateCache()
}

_WLayouts_Rename(id, newName) {
    layout := _WLayouts_Load(id)
    if !IsObject(layout)
        return "That layout no longer exists."
    clean := _WLayouts_SanitizeName(newName)
    if (err := _WLayouts_ValidateName(clean, id))
        return err
    try IniWrite(clean, _WLayouts_Path(), "Index", String(id))
    catch as e
        return "Could not write layouts.ini: " e.Message
    _WLayouts_InvalidateCache()
    return ""
}

_WLayouts_Duplicate(id) {
    layout := _WLayouts_Load(id)
    if !IsObject(layout)
        return 0
    copy := {
        id: 0,
        name: _WLayouts_UniqueName(layout.name),
        fingerprint: layout.fingerprint,
        monIndex: layout.monIndex,
        winW: layout.winW,
        winH: layout.winH,
        slots: []
    }
    for slot in layout.slots
        copy.slots.Push({fx: slot.fx, fy: slot.fy, charName: slot.charName, focus: slot.focus})
    return _WLayouts_Save(copy)
}

; Trimmed, length-capped, and stripped of the characters that would break the
; ini's own structure. "=" is safe — parsing splits on the first one only.
_WLayouts_SanitizeName(name) {
    s := Trim(name)
    for ch in ["|", "`r", "`n", "`t"]
        s := StrReplace(s, ch, " ")
    s := StrReplace(StrReplace(s, "[", "("), "]", ")")
    if (StrLen(s) > 48)
        s := SubStr(s, 1, 48)
    return Trim(s)
}

; "" when the name is usable, otherwise the reason it isn't.
_WLayouts_ValidateName(name, exceptId := 0) {
    if (name = "")
        return "Enter a name for the layout."
    if _WindowLayout_IsPreset(name)
        return "“" name "” is a built-in preset name — pick another."
    for entry in _WLayouts_LoadIndex() {
        if (entry.id != exceptId && entry.name = name)
            return "A layout named “" name "” already exists."
    }
    return ""
}

_WLayouts_UniqueName(base) {
    clean := _WLayouts_SanitizeName(base)
    if (clean = "")
        clean := "Layout"
    if (_WLayouts_ValidateName(clean) = "")
        return clean
    Loop 99 {
        candidate := _WLayouts_SanitizeName(clean " (" (A_Index + 1) ")")
        if (_WLayouts_ValidateName(candidate) = "")
            return candidate
    }
    return clean " (" A_TickCount ")"
}

; ── Geometry ─────────────────────────────────────────────────────

; index|boundsWxboundsH|workWxworkH|boundsLeft,boundsTop — enough to recognise
; the same physical display after the indexes get reshuffled.
_WindowLayout_MonitorFingerprint(monIdx) {
    MonitorGet(monIdx, &l, &t, &r, &b)
    MonitorGetWorkArea(monIdx, &wl, &wt, &wr, &wb)
    return monIdx "|" (r - l) "x" (b - t) "|" (wr - wl) "x" (wb - wt) "|" l "," t
}

; The connected monitor this fingerprint describes, or 0. An exact match (size
; and origin) wins; failing that, any display of the same size.
_WindowLayout_MatchFingerprint(fingerprint) {
    parts := StrSplit(Trim(fingerprint), "|")
    if (parts.Length < 4)
        return 0
    wantBounds := Trim(parts[2])
    wantOrigin := Trim(parts[4])
    sizeOnly := 0
    Loop MonitorGetCount() {
        MonitorGet(A_Index, &l, &t, &r, &b)
        if ((r - l) "x" (b - t) != wantBounds)
            continue
        if (l "," t = wantOrigin)
            return A_Index
        if !sizeOnly
            sizeOnly := A_Index
    }
    return sizeOnly
}

_WindowLayout_PointToFrac(x, y, monIdx) {
    MonitorGetWorkArea(monIdx, &wl, &wt, &wr, &wb)
    sw := Max(1, wr - wl)
    sh := Max(1, wb - wt)
    return {fx: (x - wl) / sw, fy: (y - wt) / sh}
}

_WindowLayout_FracToPoint(slot, monIdx) {
    MonitorGetWorkArea(monIdx, &wl, &wt, &wr, &wb)
    sw := wr - wl
    sh := wb - wt
    return {x: wl + Round(slot.fx * sw), y: wt + Round(slot.fy * sh)}
}

; override → exact/size fingerprint match → stored index → configured target.
_WindowLayout_ResolveLayoutMonitor(layout, override := 0) {
    if (IsInteger(override) && override >= 1 && override <= MonitorGetCount())
        return override
    if (matched := _WindowLayout_MatchFingerprint(layout.fingerprint))
        return matched
    if (layout.monIndex >= 1 && layout.monIndex <= MonitorGetCount())
        return layout.monIndex
    return _WindowLayout_ConfiguredMonitor()
}

; ── Capture ──────────────────────────────────────────────────────

; An unsaved layout (id 0) describing where the clients on this monitor sit now.
; Returns 0 when there is nothing to capture.
_WindowLayout_CaptureCurrent(monIdx) {
    global _WindowLayout_MainCharacter
    windows := FilterWindowsOnMonitor(_WindowLayout_ArrangeableWindows(), monIdx)
    if (windows.Length = 0)
        return 0
    ordered := _WindowLayout_OrderWindows(windows)

    layout := {
        id: 0,
        name: "",
        monIndex: monIdx,
        fingerprint: _WindowLayout_MonitorFingerprint(monIdx),
        winW: 0,
        winH: 0,
        slots: [],
        unassigned: 0
    }
    for hwnd in ordered {
        try WinGetPos(&x, &y, &w, &h, "ahk_id " hwnd)
        catch
            continue
        if (layout.winW = 0) {
            layout.winW := w
            layout.winH := h
        }
        frac := _WindowLayout_PointToFrac(x, y, monIdx)
        charName := CharacterNameFromWindow(hwnd)
        if (charName = "")
            layout.unassigned += 1
        layout.slots.Push({
            fx: frac.fx,
            fy: frac.fy,
            charName: charName,
            focus: (charName != "" && charName = _WindowLayout_MainCharacter)
        })
    }
    return (layout.slots.Length > 0) ? layout : 0
}

; ── Apply ────────────────────────────────────────────────────────

; Which window goes in which slot. Slots naming a character claim that client
; first; whatever is left fills the unbound slots in on-screen order; anything
; still over cascades across the whole slot list instead of stacking.
_WindowLayout_AssignWindows(layout, ordered) {
    result := []
    total := layout.slots.Length
    if (total = 0)
        return result

    claimed := Map()
    taken := Map()
    for i, slot in layout.slots {
        if (slot.charName = "")
            continue
        for hwnd in ordered {
            if claimed.Has(hwnd)
                continue
            if (CharacterNameFromWindow(hwnd) = slot.charName) {
                claimed[hwnd] := true
                taken[i] := true
                result.Push({hwnd: hwnd, slotIndex: i, pass: 0})
                break
            }
        }
    }

    free := []
    Loop total {
        if !taken.Has(A_Index)
            free.Push(A_Index)
    }

    n := 0
    for hwnd in ordered {
        if claimed.Has(hwnd)
            continue
        n += 1
        if (n <= free.Length) {
            result.Push({hwnd: hwnd, slotIndex: free[n], pass: 0})
            continue
        }
        ; Past every free slot: reuse the full list, offset a pass further each
        ; time around so nothing ends up exactly behind another window.
        over := n - free.Length
        result.Push({
            hwnd: hwnd,
            slotIndex: Mod(over - 1, total) + 1,
            pass: (over - 1) // total + 1
        })
    }
    return result
}

; Applies a saved layout. Returns {moved, overflow, monIdx}.
_WindowLayout_ApplyCustom(layout, monIdxOverride := 0) {
    monIdx := _WindowLayout_ResolveLayoutMonitor(layout, monIdxOverride)

    ; Clients on the target display, plus any client this layout names outright —
    ; an explicit binding should pull its window back even from another monitor.
    all := _WindowLayout_ArrangeableWindows()
    windows := FilterWindowsOnMonitor(all, monIdx)
    seen := Map()
    for hwnd in windows
        seen[hwnd] := true
    for slot in layout.slots {
        if (slot.charName = "")
            continue
        hwnd := _WindowLayout_WindowForCharacter(all, slot.charName)
        if (hwnd && !seen.Has(hwnd)) {
            seen[hwnd] := true
            windows.Push(hwnd)
        }
    }
    if (windows.Length = 0) {
        TrayTip("No game windows on " _WindowLayout_DisplayLabel(monIdx) ".", "Window Layout", "Iconi")
        return {moved: 0, overflow: 0, monIdx: monIdx}
    }

    ordered := _WindowLayout_OrderWindows(windows)
    _WindowLayout_RecordUndo(ordered)

    assignments := _WindowLayout_AssignWindows(layout, ordered)
    moved := 0
    overflow := 0
    focusHwnd := 0
    for a in assignments {
        slot := layout.slots[a.slotIndex]
        pt := _WindowLayout_FracToPoint(slot, monIdx)
        _WindowLayout_MoveInto(a.hwnd, pt.x, pt.y, monIdx, a.pass)
        moved += 1
        if (a.pass > 0)
            overflow += 1
        if (slot.focus && a.pass = 0 && !focusHwnd)
            focusHwnd := a.hwnd
    }

    if focusHwnd {
        try WinActivate("ahk_id " focusHwnd)
    } else {
        _WindowLayout_ActivateMainCharacter(ordered)
    }

    _WindowLayout_ReportApply(layout, monIdx, moved, overflow)
    return {moved: moved, overflow: overflow, monIdx: monIdx}
}

_WindowLayout_ReportApply(layout, monIdx, moved, overflow) {
    notes := []
    if overflow
        notes.Push(layout.slots.Length " slots, " moved " clients — " overflow " offset behind the others.")
    ; Fractions rescale silently, but a size change is worth saying out loud.
    if (layout.fingerprint != "" && layout.fingerprint != _WindowLayout_MonitorFingerprint(monIdx)) {
        parts := StrSplit(layout.fingerprint, "|")
        was := (parts.Length >= 2) ? Trim(parts[2]) : "another display"
        MonitorGet(monIdx, &l, &t, &r, &b)
        now := (r - l) "x" (b - t)
        if (was != now)
            notes.Push("Authored for " was ", rescaled to " now ".")
    }
    if (notes.Length = 0)
        return
    text := ""
    for note in notes
        text .= (text = "" ? "" : "`n") note
    TrayTip(text, "Window Layout — " layout.name, "Iconi")
}

; The one entry point for "apply the layout called X", preset or custom.
; monIdxOverride 0 means "let the layout decide".
_WindowLayout_ApplyNamed(name, monIdxOverride := 0, *) {
    if (name = "")
        return
    if _WindowLayout_IsPreset(name) {
        if (IsInteger(monIdxOverride) && monIdxOverride >= 1)
            _WindowLayout_ApplyPreset(name, monIdxOverride)
        else
            _WindowLayout_ApplyPreset(name)
        return
    }
    layout := _WLayouts_LoadByName(name)
    if !IsObject(layout) {
        TrayTip("Layout “" name "” no longer exists.", "Window Layout", "Iconx")
        return
    }
    _WindowLayout_ApplyCustom(layout, monIdxOverride)
}

; ── Names ────────────────────────────────────────────────────────

_WindowLayout_AllLayoutNames() {
    global _WindowLayout_PRESETS
    names := []
    for n in _WindowLayout_PRESETS
        names.Push(n)
    for entry in _WLayouts_LoadIndex()
        names.Push(entry.name)
    return names
}

; Every character a slot could be bound to: the ones running now, plus the ones
; saved layouts already mention, so an offline alt stays selectable.
_WindowLayout_CharacterTable() {
    table := []
    seen := Map()
    for name in _WindowLayout_GetCharacterNames() {
        seen[name] := true
        table.Push({name: name, running: true})
    }
    for entry in _WLayouts_LoadIndex() {
        layout := _WLayouts_Load(entry.id)
        if !IsObject(layout)
            continue
        for slot in layout.slots {
            if (slot.charName = "" || seen.Has(slot.charName))
                continue
            seen[slot.charName] := true
            table.Push({name: slot.charName, running: false})
        }
    }
    return table
}

; ── Capture entry points ─────────────────────────────────────────

; Where "capture what I see" should look: whichever display is holding the most
; game windows, so it works without first setting a target display.
_WindowLayout_CaptureMonitor() {
    counts := Map()
    best := 0
    bestCount := 0
    for hwnd in _WindowLayout_ArrangeableWindows() {
        idx := GetWindowMonitorIndex(hwnd)
        counts[idx] := counts.Has(idx) ? counts[idx] + 1 : 1
        if (counts[idx] > bestCount) {
            bestCount := counts[idx]
            best := idx
        }
    }
    return best ? best : _WindowLayout_ConfiguredMonitor()
}

; Captures the current arrangement under a name the user types. Returns the new
; layout id, or 0 when cancelled or nothing could be captured.
_WindowLayout_CaptureAsPrompt(suggested := "") {
    monIdx := _WindowLayout_CaptureMonitor()
    layout := _WindowLayout_CaptureCurrent(monIdx)
    if !IsObject(layout) {
        TrayTip("No game windows to capture."
            . (_WindowLayout_MinimizedCount() ? "`nRestore your minimized clients first." : ""),
            "Window Layout", "Iconx")
        return 0
    }

    if (suggested = "")
        suggested := _WLayouts_UniqueName(layout.slots.Length " Client Layout")

    Loop {
        prompt := InputBox("Name this layout — " layout.slots.Length " window"
            . (layout.slots.Length = 1 ? "" : "s") " on " _WindowLayout_DisplayLabel(monIdx) ".",
            "Window Layout — Capture", "w360 h150", suggested)
        if (prompt.Result != "OK")
            return 0
        name := _WLayouts_SanitizeName(prompt.Value)
        if (err := _WLayouts_ValidateName(name)) {
            MsgBox(err, "Window Layout", "Icon!")
            suggested := name
            continue
        }
        layout.name := name
        break
    }

    id := _WLayouts_Save(layout)
    if !id
        return 0
    msg := "Captured " layout.slots.Length " slot" (layout.slots.Length = 1 ? "" : "s") " as “" layout.name "”."
    if layout.unassigned {
        msg .= "`n" layout.unassigned " client" (layout.unassigned = 1 ? " isn't" : "s aren't")
            . " logged in yet, so those slots were saved as “any client”."
    }
    TrayTip(msg, "Window Layout", "Iconi")
    return id
}

; ── Manager ──────────────────────────────────────────────────────

_WindowLayout_CanUseWebView() {
    return FileExist(A_ScriptDir "\Lib\WebViewToo.ahk")
        && FileExist(A_ScriptDir "\ui\window_layout\index.html")
}

_WindowLayout_ShowManager() {
    if (IsNativeInterface() || !_WindowLayout_CanUseWebView()) {
        _WindowLayout_ShowManagerNative()
        return
    }
    _WindowLayout_ShowManagerWeb()
}

; Native mode gets the manager without the canvas: capture is the primary
; authoring path either way, and visual editing needs the WebView2 frontend.
_WindowLayout_ShowManagerNative() {
    global _WindowLayout_MgrGui, _WindowLayout_MgrList

    if (IsObject(_WindowLayout_MgrGui) && _WindowLayout_MgrGui.Hwnd) {
        try WinActivate("ahk_id " _WindowLayout_MgrGui.Hwnd)
        return
    }

    g := Gui("+AlwaysOnTop -MinimizeBox", "Maps++ — Window Layouts")
    g.MarginX := 10
    g.MarginY := 10
    g.SetFont("s9", "Segoe UI")

    lv := g.Add("ListView", "w560 r10 -Multi NoSortHdr Grid", ["Name", "Slots", "Display", "Default"])
    lv.OnEvent("DoubleClick", (*) => _WindowLayout_MgrApply())

    g.Add("Button", "xm y+10 w120", "Capture current…").OnEvent("Click", (*) => _WindowLayout_MgrCapture())
    g.Add("Button", "x+6 w70", "Apply").OnEvent("Click", (*) => _WindowLayout_MgrApply())
    g.Add("Button", "x+6 w70", "Rename…").OnEvent("Click", (*) => _WindowLayout_MgrRename())
    g.Add("Button", "x+6 w70", "Delete").OnEvent("Click", (*) => _WindowLayout_MgrDelete())
    g.Add("Button", "x+6 w100", "Set as default").OnEvent("Click", (*) => _WindowLayout_MgrSetDefault())
    g.Add("Button", "x+6 w100", "Undo last apply").OnEvent("Click", (*) => _WindowLayout_UndoLastApply())

    g.SetFont("s8")
    g.Add("Text", "xm y+10 w560 cGray",
        "Drag your clients into place, then Capture. Visual editing needs the "
        . "WebView2 interface (Tray → Interface).")
    g.SetFont("s9")

    g.OnEvent("Close", (*) => _WindowLayout_MgrClose())
    g.OnEvent("Escape", (*) => _WindowLayout_MgrClose())

    _WindowLayout_MgrGui := g
    _WindowLayout_MgrList := lv
    _WindowLayout_MgrRefresh()
    g.Show("AutoSize")
}

_WindowLayout_MgrClose() {
    global _WindowLayout_MgrGui, _WindowLayout_MgrList, _WindowLayout_MgrRows
    if IsObject(_WindowLayout_MgrGui)
        try _WindowLayout_MgrGui.Destroy()
    _WindowLayout_MgrGui := 0
    _WindowLayout_MgrList := 0
    _WindowLayout_MgrRows := []
}

_WindowLayout_MgrRefresh(selectId := 0) {
    global _WindowLayout_MgrList, _WindowLayout_MgrRows, _WindowLayout_DefaultLayout
    if !IsObject(_WindowLayout_MgrList)
        return

    lv := _WindowLayout_MgrList
    lv.Delete()
    _WindowLayout_MgrRows := []
    selectRow := 0

    for entry in _WLayouts_LoadIndex() {
        layout := _WLayouts_Load(entry.id)
        if !IsObject(layout)
            continue
        monIdx := _WindowLayout_MatchFingerprint(layout.fingerprint)
        display := monIdx ? _WindowLayout_DisplayLabel(monIdx) : "not connected"
        row := lv.Add(, layout.name, layout.slots.Length, display,
            (layout.name = _WindowLayout_DefaultLayout) ? "●" : "")
        _WindowLayout_MgrRows.Push(entry.id)
        if (entry.id = selectId)
            selectRow := row
    }

    if (_WindowLayout_MgrRows.Length = 0)
        lv.Add(, "No saved layouts yet — use Capture current…", "", "", "")
    Loop 4
        lv.ModifyCol(A_Index, "AutoHdr")
    if selectRow
        lv.Modify(selectRow, "Select Focus")
}

; The layout id under the selection, or 0.
_WindowLayout_MgrSelectedId() {
    global _WindowLayout_MgrList, _WindowLayout_MgrRows
    if !IsObject(_WindowLayout_MgrList)
        return 0
    row := _WindowLayout_MgrList.GetNext(0, "F")
    if (row < 1 || row > _WindowLayout_MgrRows.Length)
        return 0
    return _WindowLayout_MgrRows[row]
}

_WindowLayout_MgrCapture() {
    if (id := _WindowLayout_CaptureAsPrompt())
        _WindowLayout_MgrRefresh(id)
}

_WindowLayout_MgrApply() {
    id := _WindowLayout_MgrSelectedId()
    if !id
        return
    layout := _WLayouts_Load(id)
    if IsObject(layout)
        _WindowLayout_ApplyCustom(layout)
}

_WindowLayout_MgrRename() {
    global _WindowLayout_MgrGui
    id := _WindowLayout_MgrSelectedId()
    if !id
        return
    layout := _WLayouts_Load(id)
    if !IsObject(layout)
        return
    current := layout.name
    Loop {
        prompt := InputBox("New name for “" current "”:", "Window Layout — Rename", "w320 h130", layout.name)
        if (prompt.Result != "OK")
            return
        err := _WLayouts_Rename(id, prompt.Value)
        if (err = "")
            break
        MsgBox(err, "Window Layout", "Icon!")
    }
    _WindowLayout_MgrRenamedDefault(current, _WLayouts_NameForId(id))
    _WindowLayout_MgrRefresh(id)
}

; DefaultLayout stores a name, so a rename has to follow it or the default
; silently stops resolving.
_WindowLayout_MgrRenamedDefault(oldName, newName) {
    global _WindowLayout_DefaultLayout
    if (oldName = "" || newName = "" || _WindowLayout_DefaultLayout != oldName)
        return
    _WindowLayout_DefaultLayout := newName
    _WindowLayout_SaveConfig()
}

_WindowLayout_MgrDelete() {
    global _WindowLayout_DefaultLayout
    id := _WindowLayout_MgrSelectedId()
    if !id
        return
    layout := _WLayouts_Load(id)
    if !IsObject(layout)
        return
    if (MsgBox("Delete the layout “" layout.name "”?", "Window Layout", "YesNo Icon? 0x40000") != "Yes")
        return
    wasDefault := (_WindowLayout_DefaultLayout = layout.name)
    _WLayouts_Delete(id)
    if wasDefault {
        _WindowLayout_DefaultLayout := "Grid2x2"
        _WindowLayout_SaveConfig()
    }
    _WindowLayout_MgrRefresh()
}

_WindowLayout_MgrSetDefault() {
    global _WindowLayout_DefaultLayout
    id := _WindowLayout_MgrSelectedId()
    if !id
        return
    layout := _WLayouts_Load(id)
    if !IsObject(layout)
        return
    _WindowLayout_DefaultLayout := layout.name
    _WindowLayout_SaveConfig()
    _WindowLayout_MgrRefresh(id)
    TrayTip("“" layout.name "” is now applied by " GetHotkeyDisplay("windowLayoutPrimary") ".",
        "Window Layout", "Iconi")
}

; ── Manager (WebView2 editor) ────────────────────────────────────
;
; The stage draws the target monitor's work area to scale and the slots as boxes
; sized from the layout's WinSize; dragging a box edits its fraction pair. AHK
; owns the store, so every mutation round-trips through here rather than being
; applied client-side and saved later.

_WindowLayout_ShowManagerWeb() {
    global _WindowLayout_WebGui, _WindowLayout_WebSelected

    if (IsObject(_WindowLayout_WebGui) && _WindowLayout_WebGui.Hwnd) {
        try WinActivate("ahk_id " _WindowLayout_WebGui.Hwnd)
        return
    }

    dllDir := (A_PtrSize = 8) ? "64bit" : "32bit"
    dllPath := A_ScriptDir "\Lib\" dllDir "\WebView2Loader.dll"
    wvSettings := { DllPath: dllPath, DefaultWidth: 960, DefaultHeight: 640 }

    g := WebViewGui("-Caption +AlwaysOnTop +Resize", "Maps++ — Window Layouts", , wvSettings)
    g.OnEvent("Close", (*) => _WindowLayout_WebClose())
    g.WebMessageReceived(_WindowLayout_OnManagerWebMsg)
    g.DOMContentLoaded((*) => SetTimer(_WindowLayout_SendState, -50))
    g.Navigate("ui/window_layout/index.html")

    _WindowLayout_WebGui := g
    ; Reopen on whatever the store considers first, so the stage is never blank
    ; when there is something to show.
    saved := _WLayouts_LoadIndex()
    _WindowLayout_WebSelected := saved.Length ? saved[1].id : 0
    g.Show("w960 h640 Center")
}

_WindowLayout_WebClose() {
    global _WindowLayout_WebGui
    if IsObject(_WindowLayout_WebGui)
        try _WindowLayout_WebGui.Destroy()
    _WindowLayout_WebGui := 0
}

_WindowLayout_WebPost(json) {
    global _WindowLayout_WebGui
    if !IsObject(_WindowLayout_WebGui)
        return
    try _WindowLayout_WebGui.PostWebMessageAsJson(json)
}

_WindowLayout_WebToast(level, text) {
    _WindowLayout_WebPost('{"type":"toast","level":' _JSON_Str(level) ',"text":' _JSON_Str(text) '}')
}

_WindowLayout_SendState() {
    global _WindowLayout_WebSelected, _WindowLayout_DefaultLayout, _WindowLayout_WebChars
    global _WindowLayout_PRESETS

    layoutsJson := ""
    ids := Map()
    for entry in _WLayouts_LoadIndex() {
        layout := _WLayouts_Load(entry.id)
        if !IsObject(layout)
            continue
        ids[entry.id] := true
        monIdx := _WindowLayout_MatchFingerprint(layout.fingerprint)
        layoutsJson .= (layoutsJson = "" ? "" : ",")
            . '{"id":' entry.id
            . ',"name":' _JSON_Str(layout.name)
            . ',"slotCount":' layout.slots.Length
            . ',"monitorLabel":' _JSON_Str(monIdx ? _WindowLayout_DisplayLabel(monIdx) : "Display not connected")
            . ',"connected":' (monIdx ? "true" : "false")
            . ',"isDefault":' ((layout.name = _WindowLayout_DefaultLayout) ? "true" : "false")
            . '}'
    }
    if (_WindowLayout_WebSelected && !ids.Has(_WindowLayout_WebSelected))
        _WindowLayout_WebSelected := 0

    ; Names never travel back from JS — a slot reports the index into this table
    ; instead, because _JSON_Parse does not decode \uXXXX escapes.
    _WindowLayout_WebChars := _WindowLayout_CharacterTable()
    charsJson := ""
    for c in _WindowLayout_WebChars {
        charsJson .= (charsJson = "" ? "" : ",")
            . '{"name":' _JSON_Str(c.name) ',"running":' (c.running ? "true" : "false") '}'
    }

    monsJson := ""
    Loop MonitorGetCount() {
        MonitorGetWorkArea(A_Index, &wl, &wt, &wr, &wb)
        monsJson .= (monsJson = "" ? "" : ",")
            . '{"index":' A_Index
            . ',"label":' _JSON_Str(_WindowLayout_DisplayLabel(A_Index))
            . ',"workW":' (wr - wl) ',"workH":' (wb - wt)
            . ',"isPrimary":' ((A_Index = MonitorGetPrimary()) ? "true" : "false")
            . '}'
    }

    presetsJson := ""
    for p in _WindowLayout_PRESETS {
        if (p = "Reset")
            continue
        presetsJson .= (presetsJson = "" ? "" : ",") _JSON_Str(p)
    }

    _WindowLayout_WebPost('{"type":"layouts-state"'
        . ',"layouts":[' layoutsJson ']'
        . ',"selectedId":' _WindowLayout_WebSelected
        . ',"characters":[' charsJson ']'
        . ',"monitors":[' monsJson ']'
        . ',"presets":[' presetsJson ']'
        . ',"clientCount":' _WindowLayout_ArrangeableWindows().Length
        . ',"defaultLayoutName":' _JSON_Str(_WindowLayout_DefaultLayout)
        . '}')

    if _WindowLayout_WebSelected
        _WindowLayout_SendDetail(_WindowLayout_WebSelected)
    else
        _WindowLayout_WebPost('{"type":"layout-detail","id":0,"slots":[]}')
}

_WindowLayout_SendDetail(id) {
    global _WindowLayout_WebChars
    layout := _WLayouts_Load(id)
    if !IsObject(layout) {
        _WindowLayout_WebPost('{"type":"layout-detail","id":0,"slots":[]}')
        return
    }
    monIdx := _WindowLayout_ResolveLayoutMonitor(layout)
    MonitorGetWorkArea(monIdx, &wl, &wt, &wr, &wb)

    slotsJson := ""
    for slot in layout.slots {
        slotsJson .= (slotsJson = "" ? "" : ",")
            . '{"fx":' Format("{:.4f}", slot.fx)
            . ',"fy":' Format("{:.4f}", slot.fy)
            . ',"charIndex":' _WindowLayout_CharIndex(slot.charName)
            . ',"focus":' (slot.focus ? "true" : "false")
            . '}'
    }

    ; A layout captured before any client was running has no reference size; fall
    ; back so the editor still draws boxes of a believable shape.
    winW := layout.winW ? layout.winW : 1024
    winH := layout.winH ? layout.winH : 768

    _WindowLayout_WebPost('{"type":"layout-detail"'
        . ',"id":' layout.id
        . ',"name":' _JSON_Str(layout.name)
        . ',"monitorIndex":' monIdx
        . ',"fingerprintOk":' ((layout.fingerprint = _WindowLayout_MonitorFingerprint(monIdx)) ? "true" : "false")
        . ',"workW":' (wr - wl) ',"workH":' (wb - wt)
        . ',"winW":' winW ',"winH":' winH
        . ',"slots":[' slotsJson ']'
        . '}')
}

_WindowLayout_CharIndex(name) {
    global _WindowLayout_WebChars
    if (name = "")
        return 0
    for i, c in _WindowLayout_WebChars {
        if (c.name = name)
            return i
    }
    return 0
}

_WindowLayout_CharName(index) {
    global _WindowLayout_WebChars
    if (!IsInteger(index) || index < 1 || index > _WindowLayout_WebChars.Length)
        return ""
    return _WindowLayout_WebChars[index].name
}

_WindowLayout_OnManagerWebMsg(wv, args) {
    global _WindowLayout_WebSelected

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
            _WindowLayout_SendState()

        case "select-layout":
            _WindowLayout_WebSelected := _WindowLayout_MsgInt(msg, "id")
            _WindowLayout_SendDetail(_WindowLayout_WebSelected)

        case "new-layout":
            _WindowLayout_WebNew(msg)

        case "capture-current":
            _WindowLayout_WebCapture(msg)

        case "save-layout":
            _WindowLayout_WebSave(msg)

        case "rename-layout":
            id := _WindowLayout_MsgInt(msg, "id")
            old := _WLayouts_NameForId(id)
            if (err := _WLayouts_Rename(id, msg.Has("name") ? msg["name"] : "")) {
                _WindowLayout_WebToast("error", err)
            } else {
                _WindowLayout_MgrRenamedDefault(old, _WLayouts_NameForId(id))
            }
            _WindowLayout_SendState()

        case "duplicate-layout":
            if (newId := _WLayouts_Duplicate(_WindowLayout_MsgInt(msg, "id")))
                _WindowLayout_WebSelected := newId
            _WindowLayout_SendState()

        case "delete-layout":
            _WindowLayout_WebDelete(_WindowLayout_MsgInt(msg, "id"))

        case "apply-layout":
            _WindowLayout_WebApply(msg)

        case "undo-apply":
            _WindowLayout_UndoLastApply()
            _WindowLayout_WebPost('{"type":"apply-result","ok":true,"message":"Restored the previous positions."}')

        case "set-default":
            _WindowLayout_WebSetDefault(_WindowLayout_MsgInt(msg, "id"))

        case "close":
            _WindowLayout_WebClose()
    }
}

_WindowLayout_MsgInt(msg, key, fallback := 0) {
    if (!msg.Has(key) || !IsNumber(msg[key]))
        return fallback
    return Integer(msg[key])
}

_WindowLayout_MsgNum(msg, key, fallback := 0.0) {
    if (!msg.Has(key) || !IsNumber(msg[key]))
        return fallback
    ; Guards against a stray 1e+30 turning into a coordinate nothing can recover
    ; from; anything sane is well inside this range.
    return Clamp(Float(msg[key]), -2.0, 3.0)
}

; A reference client window size for a layout with no clients to measure.
_WindowLayout_ReferenceWindowSize(monIdx) {
    all := _WindowLayout_ArrangeableWindows()
    for hwnd in FilterWindowsOnMonitor(all, monIdx) {
        try WinGetPos(, , &w, &h, "ahk_id " hwnd)
        catch
            continue
        if (w > 0 && h > 0)
            return {w: w, h: h}
    }
    for hwnd in all {
        try WinGetPos(, , &w, &h, "ahk_id " hwnd)
        catch
            continue
        if (w > 0 && h > 0)
            return {w: w, h: h}
    }
    return {w: 1024, h: 768}
}

_WindowLayout_WebNew(msg) {
    global _WindowLayout_WebSelected
    monIdx := _WindowLayout_MsgInt(msg, "monitorIndex", _WindowLayout_CaptureMonitor())
    if (monIdx < 1 || monIdx > MonitorGetCount())
        monIdx := _WindowLayout_ConfiguredMonitor()

    name := _WLayouts_SanitizeName(msg.Has("name") ? msg["name"] : "")
    if (err := _WLayouts_ValidateName(name)) {
        _WindowLayout_WebToast("error", err)
        return
    }

    ref := _WindowLayout_ReferenceWindowSize(monIdx)
    layout := {id: 0, name: name, monIndex: monIdx,
        fingerprint: _WindowLayout_MonitorFingerprint(monIdx),
        winW: ref.w, winH: ref.h, slots: []}

    seed := msg.Has("seedPreset") ? msg["seedPreset"] : ""
    if (seed != "" && _WindowLayout_IsPreset(seed)) {
        for pt in _WindowLayout_ComputeSlots(seed, ref.w, ref.h, monIdx) {
            frac := _WindowLayout_PointToFrac(pt.x, pt.y, monIdx)
            layout.slots.Push({fx: frac.fx, fy: frac.fy, charName: "", focus: false})
        }
    }
    if (layout.slots.Length = 0)
        layout.slots.Push({fx: 0.0, fy: 0.0, charName: "", focus: false})

    if (id := _WLayouts_Save(layout))
        _WindowLayout_WebSelected := id
    _WindowLayout_SendState()
}

_WindowLayout_WebCapture(msg) {
    global _WindowLayout_WebSelected
    monIdx := _WindowLayout_MsgInt(msg, "monitorIndex", 0)
    if (monIdx < 1 || monIdx > MonitorGetCount())
        monIdx := _WindowLayout_CaptureMonitor()

    captured := _WindowLayout_CaptureCurrent(monIdx)
    if !IsObject(captured) {
        why := "No game windows on " _WindowLayout_DisplayLabel(monIdx) "."
        if _WindowLayout_MinimizedCount()
            why .= " Restore your minimized clients first."
        _WindowLayout_WebPost('{"type":"capture-result","ok":false,"message":' _JSON_Str(why) '}')
        return
    }

    id := _WindowLayout_MsgInt(msg, "id")
    existing := _WLayouts_Load(id)
    if IsObject(existing) {
        ; Replace the geometry, keep the name the user already chose.
        captured.id := existing.id
        captured.name := existing.name
    } else {
        captured.id := 0
        captured.name := _WLayouts_UniqueName(captured.slots.Length " Client Layout")
    }

    if (newId := _WLayouts_Save(captured))
        _WindowLayout_WebSelected := newId

    note := "Captured " captured.slots.Length " slot" (captured.slots.Length = 1 ? "" : "s") "."
    if captured.unassigned {
        note .= " " captured.unassigned " client" (captured.unassigned = 1 ? " isn't" : "s aren't")
            . " logged in yet, so those slots are “any client”."
    }
    _WindowLayout_SendState()
    _WindowLayout_WebPost('{"type":"capture-result","ok":true,"slotCount":' captured.slots.Length
        . ',"unassignedCount":' captured.unassigned ',"message":' _JSON_Str(note) '}')
}

_WindowLayout_WebSave(msg) {
    global _WindowLayout_WebSelected
    id := _WindowLayout_MsgInt(msg, "id")
    existing := _WLayouts_Load(id)
    if !IsObject(existing) {
        _WindowLayout_WebToast("error", "That layout no longer exists.")
        _WindowLayout_SendState()
        return
    }

    name := _WLayouts_SanitizeName(msg.Has("name") ? msg["name"] : existing.name)
    if (err := _WLayouts_ValidateName(name, id)) {
        _WindowLayout_WebToast("error", err)
        return
    }

    monIdx := _WindowLayout_MsgInt(msg, "monitorIndex", existing.monIndex)
    if (monIdx < 1 || monIdx > MonitorGetCount())
        monIdx := _WindowLayout_ResolveLayoutMonitor(existing)

    slots := []
    if (msg.Has("slots") && msg["slots"] is Array) {
        for raw in msg["slots"] {
            if !IsObject(raw)
                continue
            slots.Push({
                fx: _WindowLayout_MsgNum(raw, "fx"),
                fy: _WindowLayout_MsgNum(raw, "fy"),
                charName: _WindowLayout_CharName(_WindowLayout_MsgInt(raw, "charIndex")),
                focus: raw.Has("focus") && raw["focus"] ? true : false
            })
        }
    }
    if (slots.Length = 0) {
        _WindowLayout_WebToast("error", "A layout needs at least one slot.")
        return
    }

    ; The monitor may have changed since capture, so re-fingerprint on save.
    updated := {id: id, name: name, monIndex: monIdx,
        fingerprint: _WindowLayout_MonitorFingerprint(monIdx),
        winW: existing.winW, winH: existing.winH, slots: slots}
    _WindowLayout_MgrRenamedDefault(existing.name, name)
    _WLayouts_Save(updated)
    _WindowLayout_WebSelected := id
    _WindowLayout_SendState()
    _WindowLayout_WebToast("info", "Saved “" name "”.")
}

_WindowLayout_WebDelete(id) {
    global _WindowLayout_WebSelected, _WindowLayout_DefaultLayout
    layout := _WLayouts_Load(id)
    if !IsObject(layout)
        return
    if (_WindowLayout_DefaultLayout = layout.name) {
        _WindowLayout_DefaultLayout := "Grid2x2"
        _WindowLayout_SaveConfig()
    }
    _WLayouts_Delete(id)
    if (_WindowLayout_WebSelected = id)
        _WindowLayout_WebSelected := 0
    _WindowLayout_SendState()
}

_WindowLayout_WebApply(msg) {
    layout := _WLayouts_Load(_WindowLayout_MsgInt(msg, "id"))
    if !IsObject(layout) {
        _WindowLayout_WebPost('{"type":"apply-result","ok":false,"message":'
            . _JSON_Str("That layout no longer exists.") '}')
        return
    }
    monIdx := _WindowLayout_MsgInt(msg, "monitorIndex", 0)
    res := _WindowLayout_ApplyCustom(layout, monIdx)
    text := res.moved
        ? ("Moved " res.moved " window" (res.moved = 1 ? "" : "s") " on " _WindowLayout_DisplayLabel(res.monIdx) ".")
        : "No windows to move."
    if res.overflow
        text .= " " res.overflow " cascaded past the last slot."
    _WindowLayout_WebPost('{"type":"apply-result","ok":' (res.moved ? "true" : "false")
        . ',"moved":' res.moved ',"overflow":' res.overflow ',"message":' _JSON_Str(text) '}')
}

_WindowLayout_WebSetDefault(id) {
    global _WindowLayout_DefaultLayout
    layout := _WLayouts_Load(id)
    if !IsObject(layout)
        return
    _WindowLayout_DefaultLayout := layout.name
    _WindowLayout_SaveConfig()
    _WindowLayout_SendState()
    _WindowLayout_WebToast("info", "“" layout.name "” is now applied by " GetHotkeyDisplay("windowLayoutPrimary") ".")
}

; ── Undo ─────────────────────────────────────────────────────────

_WindowLayout_RecordUndo(windows) {
    global _WindowLayout_Undo
    saved := []
    for hwnd in windows {
        try WinGetPos(&x, &y, , , "ahk_id " hwnd)
        catch
            continue
        saved.Push({hwnd: hwnd, x: x, y: y})
    }
    _WindowLayout_Undo := saved
}

_WindowLayout_UndoLastApply() {
    global _WindowLayout_Undo
    if (_WindowLayout_Undo.Length = 0) {
        TrayTip("Nothing to undo.", "Window Layout", "Iconi")
        return
    }
    restored := 0
    for entry in _WindowLayout_Undo {
        if !WinExist("ahk_id " entry.hwnd)
            continue
        try {
            WinMove(entry.x, entry.y, , , "ahk_id " entry.hwnd)
            restored += 1
        }
    }
    _WindowLayout_Undo := []
    TrayTip(restored " window" (restored = 1 ? "" : "s") " restored.", "Window Layout", "Iconi")
}

_WindowLayout_DisplayLabel(idx) {
    if idx = 0
        return "Primary (auto)"
    try {
        MonitorGet(idx, &l, &t, &r, &b)
        label := "Display " idx " — " (r - l) "×" (b - t)
        if idx = MonitorGetPrimary()
            label .= " (Primary)"
        return label
    }
    return "Display " idx
}

; The display a layout targets. With no argument this answers for the configured
; default layout, which is what LaunchClientsAndApplyLayout (functions.ahk) wants
; when it centers freshly launched clients before arranging them.
_WindowLayout_ResolveMonitor(layoutName := "") {
    global _WindowLayout_DefaultLayout
    if (layoutName = "")
        layoutName := _WindowLayout_DefaultLayout
    if (layoutName != "" && !_WindowLayout_IsPreset(layoutName)) {
        layout := _WLayouts_LoadByName(layoutName)
        if IsObject(layout)
            return _WindowLayout_ResolveLayoutMonitor(layout)
    }
    return _WindowLayout_ConfiguredMonitor()
}

; Settings → Target display, clamped, falling back to the primary.
_WindowLayout_ConfiguredMonitor() {
    global _WindowLayout_TargetMonitor
    idx := _WindowLayout_TargetMonitor
    if (idx < 1 || idx > MonitorGetCount())
        idx := MonitorGetPrimary()
    return idx
}
