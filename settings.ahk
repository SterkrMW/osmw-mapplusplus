#Requires AutoHotkey v2.0

; ── Settings window ──────────────────────────────────────────
; A single tabbed window consolidating every user preference. Enhanced mode
; renders it via WebView2; Native mode uses settings_native.ahk. Both frontends
; feed the same state/save contract below.
;
; If the WebViewToo library or ui\ folder is missing, settings automatically
; fall back to the native frontend.

; ── Conditional include ──────────────────────────────────────
; WebViewToo.ahk is only loaded when the Lib folder exists.
; If it's missing, _Settings_CanUseWebView() returns false and
; ShowSettingsWindow() shows a fallback message.
#Include *i Lib\WebViewToo.ahk
#Include settings_native.ahk

; ── Public entry point ───────────────────────────────────────

; Opens the settings window, or focuses it if already open.
ShowSettingsWindow() {
    global gSettingsGui
    if IsObject(gSettingsGui) && gSettingsGui != 0 {
        try {
            gSettingsGui.Show()
            WinActivate("ahk_id " gSettingsGui.Hwnd)
            return
        }
    }
    if IsNativeInterface() {
        _Settings_BuildNative()
        return
    }
    if _Settings_CanUseWebView() {
        _Settings_BuildWebView()
    } else {
        TrayTip("WebView2 settings are unavailable, so the native settings window was opened instead.",
            "Maps++", "Icon!")
        _Settings_BuildNative()
    }
}

_Settings_CanUseWebView() {
    return FileExist(A_ScriptDir "\Lib\WebViewToo.ahk")
        && FileExist(A_ScriptDir "\ui\settings\index.html")
}

; ── WebView construction ─────────────────────────────────────

_Settings_BuildWebView() {
    global gSettingsGui, gAddonHooks, gDisabledAddons

    ; DllPath varies by architecture.
    dllDir := (A_PtrSize = 8) ? "64bit" : "32bit"
    dllPath := A_ScriptDir "\Lib\" dllDir "\WebView2Loader.dll"
    wvSettings := { DllPath: dllPath, DefaultWidth: 760, DefaultHeight: 560 }

    g := WebViewGui("-Caption +AlwaysOnTop -Resize", "osMW Maps++ — Settings",, wvSettings)
    gSettingsGui := g
    g.OnEvent("Close", (*) => _Settings_Close())

    ; Listen for messages from the JS frontend.
    g.WebMessageReceived(_Settings_OnWebMessage)

    ; Navigate to the settings page. WebViewToo routes via ahk.localhost.
    g.Navigate(UiPageUrl("ui/settings/index.html"))

    g.Show("w760 h560")
}

; ── Message router ───────────────────────────────────────────

_Settings_OnWebMessage(wv, args) {
    global gSettingsGui
    msgStr := ""
    try msgStr := args.TryGetWebMessageAsString()
    if (msgStr = "") {
        try msgStr := args.WebMessageAsJson
    }

    if (msgStr = "") {
        return
    }

    msg := _JSON_Parse(msgStr)
    if !IsObject(msg) || !msg.Has("type")
        return

    switch msg["type"] {
        case "init-request":
            _Settings_SendState()
        case "save":
            _Settings_HandleSave(msg)
        case "cancel":
            _Settings_Close()
        case "browse-game-path":
            _Settings_HandleBrowse()
        case "check-update":
            _Settings_HandleCheckUpdate()
        case "open-update-page":
            OpenUpdatePage()
        case "start-hotkey-capture":
            _Settings_HandleStartCapture(msg)
        case "cancel-hotkey-capture":
            CancelHotkeyCapture()
    }
}

; ── Send full state to JS ────────────────────────────────────

_Settings_SendState() {
    global gSettingsGui, gAddonHooks, gDisabledAddons
    global gGamePath, gGameArgs, gLaunchOnStartup, gMultiClientCount, gMultiClientDelay
    global gPrimaryMonitorOverride, gSecondaryMonitorOverride
    global gPrimaryLaunchLayout, gSecondaryLaunchLayout
    global gMinimapScale, gMinimapOpacity, gMinimapAnchor, gMinimapOffsetX, gMinimapOffsetY
    global gMinimapKeepOpen, OVERLAY_W, OVERLAY_H, gAccentScheme, gShowHoverCoords
    global APP_VERSION, gVersionCheckEnabled, VERSION_CHECK_URL
    global gUpdateVersion, gUpdateNotes

    ; Monitor choices
    monChoices := _Settings_MonitorChoices()
    monArr := "["
    for i, lbl in monChoices {
        if (i > 1)
            monArr .= ","
        monArr .= _JSON_Str(lbl)
    }
    monArr .= "]"

    launchLayouts := _Settings_LaunchLayoutOptions()
    launchLayoutsJson := _Settings_LaunchLayoutOptionsToJson(launchLayouts)
    primaryLaunchLayout := _Settings_ValidateLaunchLayout(gPrimaryLaunchLayout, launchLayouts)
    secondaryLaunchLayout := _Settings_ValidateLaunchLayout(gSecondaryLaunchLayout, launchLayouts)

    ; Hotkey actions
    hotkeysJson := _Settings_HotkeysToJson()

    ; Addon tabs (dynamic settings)
    addonTabsJson := _Settings_AddonTabsToJson()

    ; Addon enable/disable list
    addonsJson := _Settings_AddonsListToJson()

    ; Tab names
    tabNames := _Settings_GetTabNames()
    tabNamesJson := "["
    for i, name in tabNames {
        if (i > 1)
            tabNamesJson .= ","
        tabNamesJson .= _JSON_Str(name)
    }
    tabNamesJson .= "]"

    json := '{"type":"settings-state"'
        . ',"appVersion":' _JSON_Str(APP_VERSION)
        . ',"tabNames":' tabNamesJson
        . ',"launcher":{'
            . '"versionCheck":' (gVersionCheckEnabled ? "true" : "false")
            . ',"versionCheckAvailable":' (VERSION_CHECK_URL != "" ? "true" : "false")
            ; So reopening Settings still shows an update this session found.
            . ',"updateVersion":' _JSON_Str(gUpdateVersion)
            . ',"updateNotes":' _JSON_Str(gUpdateNotes)
            . ',"gamePath":' _JSON_Str(gGamePath)
            . ',"gameArgs":' _JSON_Str(gGameArgs)
            . ',"autoStart":' (IsRunOnStartupEnabled() ? "true" : "false")
            . ',"launchOnStartup":' (gLaunchOnStartup ? "true" : "false")
            . ',"multiClientCount":' gMultiClientCount
            . ',"multiClientDelay":' gMultiClientDelay
            . ',"monitorChoices":' monArr
            . ',"primaryMonitorChoice":' _Settings_MonitorIndexToChoice(gPrimaryMonitorOverride) - 1
            . ',"secondaryMonitorChoice":' _Settings_MonitorIndexToChoice(gSecondaryMonitorOverride) - 1
            . ',"layoutAvailable":' (launchLayouts.available ? "true" : "false")
            . ',"launchLayoutOptions":' launchLayoutsJson
            . ',"primaryLaunchLayout":' _JSON_Str(primaryLaunchLayout)
            . ',"secondaryLaunchLayout":' _JSON_Str(secondaryLaunchLayout)
        . '}'
        . ',"minimap":{'
            . '"scale":' gMinimapScale
            . ',"opacity":' gMinimapOpacity
            . ',"anchor":' _JSON_Str(gMinimapAnchor)
            . ',"offsetX":' gMinimapOffsetX
            . ',"offsetY":' gMinimapOffsetY
            . ',"keepOpen":' (gMinimapKeepOpen ? "true" : "false")
            . ',"showHoverCoords":' (gShowHoverCoords ? "true" : "false")
            . ',"baseW":' OVERLAY_W
            . ',"baseH":' OVERLAY_H
            . ',"clientW":1024'
            . ',"clientH":768'
        . '}'
        . ',"appearance":{"accentScheme":' _JSON_Str(gAccentScheme) '}'
        . ',"hotkeys":{"actions":' hotkeysJson '}'
        . ',"addonTabs":' addonTabsJson
        . ',"addons":' addonsJson
    . '}'

    _Settings_PostMessage(json)
}

_Settings_GetTabNames() {
    global gAddonHooks
    tabNames := ["Launcher", "Minimap", "Appearance", "Hotkeys"]
    for _, am in gAddonHooks {
        if am.Has("OnSettingsWeb") {
            label := am.Has("settingsLabel") ? am["settingsLabel"]
                : (am.Has("name") ? am["name"] : "Addon")
            tabNames.Push(label)
        }
    }
    tabNames.Push("Addons")
    return tabNames
}

; ── Hotkeys → JSON ───────────────────────────────────────────

_Settings_HotkeysToJson() {
    json := "["
    first := true
    for action in GetHotkeyActionsForSettings() {
        if !first
            json .= ","
        first := false
        cat := action.Has("category") ? action["category"] : "Other"
        chord := action["chord"]
        defChord := action["default"]
        json .= '{'
            . '"id":' _JSON_Str(action["id"])
            . ',"label":' _JSON_Str(action["label"])
            . ',"category":' _JSON_Str(cat)
            . ',"chord":' _JSON_Str(chord)
            . ',"display":' _JSON_Str(FormatHotkeySettingsDisplay(chord))
            . ',"defaultChord":' _JSON_Str(defChord)
            . ',"defaultDisplay":' _JSON_Str(FormatHotkeySettingsDisplay(defChord))
        . '}'
    }
    json .= "]"
    return json
}

; ── Addon tabs → JSON ────────────────────────────────────────

_Settings_AddonTabsToJson() {
    global gAddonHooks
    json := "["
    first := true
    for _, am in gAddonHooks {
        if !am.Has("OnSettingsWeb")
            continue
        if !first
            json .= ","
        first := false

        label := am.Has("settingsLabel") ? am["settingsLabel"]
            : (am.Has("name") ? am["name"] : "Addon")
        addonName := am.Has("name") ? am["name"] : label

        ; Call the addon's OnSettingsWeb to get field descriptors.
        fields := []
        try fields := am["OnSettingsWeb"]()

        fieldsJson := "["
        fFirst := true
        for f in fields {
            if !fFirst
                fieldsJson .= ","
            fFirst := false
            fieldsJson .= _Settings_FieldToJson(f)
        }
        fieldsJson .= "]"

        json .= '{"label":' _JSON_Str(label)
            . ',"addonName":' _JSON_Str(addonName)
            . ',"fields":' fieldsJson '}'
    }
    json .= "]"
    return json
}

_Settings_FieldToJson(f) {
    ft := f.Has("type") ? f["type"] : "text"
    fid := f.Has("id") ? f["id"] : ""
    flabel := f.Has("label") ? f["label"] : ""
    ftext := f.Has("text") ? f["text"] : ""

    json := '{"type":' _JSON_Str(ft)
        . ',"id":' _JSON_Str(fid)
        . ',"label":' _JSON_Str(flabel)
        . ',"text":' _JSON_Str(ftext)

    ; Value
    if f.Has("value") {
        v := f["value"]
        if (v is Integer) || (v is Float)
            json .= ',"value":' v
        else if (v = true || v = false || v == "true" || v == "false")
            json .= ',"value":' (v ? "true" : "false")
        else
            json .= ',"value":' _JSON_Str(String(v))
    }

    ; Options array
    if f.Has("options") {
        opts := f["options"]
        optJson := "["
        oFirst := true
        for o in opts {
            if !oFirst
                optJson .= ","
            oFirst := false
            optJson .= _JSON_Str(String(o))
        }
        optJson .= "]"
        json .= ',"options":' optJson
    }

    ; Selectable rows for orderedlist fields
    if f.Has("items") {
        itemJson := "["
        for i, it in f["items"] {
            if (i > 1)
                itemJson .= ","
            itemJson .= '{"id":' _JSON_Str(it.Has("id") ? it["id"] : "")
                . ',"label":' _JSON_Str(it.Has("label") ? it["label"] : "")
                . ',"icon":' _JSON_Str(it.Has("icon") ? it["icon"] : "")
                . '}'
        }
        itemJson .= "]"
        json .= ',"items":' itemJson
    }

    ; Min / max for number fields, and the cap on an orderedlist's selection
    if f.Has("min")
        json .= ',"min":' f["min"]
    if f.Has("max")
        json .= ',"max":' f["max"]

    json .= "}"
    return json
}

; ── Addons list → JSON ───────────────────────────────────────

_Settings_AddonsListToJson() {
    global gAddonHooks, gDisabledAddons
    json := "["
    first := true
    for _, am in gAddonHooks {
        nm := am.Has("name") ? am["name"] : ""
        if (nm = "")
            continue
        if !first
            json .= ","
        first := false
        enabled := !(gDisabledAddons.Has(nm) && gDisabledAddons[nm])
        json .= '{"name":' _JSON_Str(nm) ',"enabled":' (enabled ? "true" : "false") '}'
    }
    json .= "]"
    return json
}

; ── Handle save ──────────────────────────────────────────────

_Settings_HandleSave(msg) {
    global gGamePath, gGameArgs, gLaunchOnStartup, gMultiClientCount, gMultiClientDelay
    global gPrimaryMonitorOverride, gSecondaryMonitorOverride
    global gPrimaryLaunchLayout, gSecondaryLaunchLayout
    global gMinimapScale, gMinimapOpacity, gMinimapAnchor, gMinimapOffsetX, gMinimapOffsetY
    global gMinimapKeepOpen, MINIMAP_ANCHORS, gAccentScheme, gShowHoverCoords
    global gAddonHooks, gHotkeyActions, gVersionCheckEnabled

    ; ── Launcher ──
    launcher := msg["launcher"]
    gGamePath := launcher["gamePath"]
    gGameArgs := Trim(launcher["gameArgs"])
    gLaunchOnStartup := launcher["launchOnStartup"] ? true : false
    cnt := launcher["multiClientCount"]
    dly := launcher["multiClientDelay"]
    if (!IsInteger(cnt) || Integer(cnt) < 1) {
        _Settings_SendSaveResult(false, "Clients per launch must be a whole number of 1 or more.")
        return false
    }
    if (!IsInteger(dly) || Integer(dly) < 0) {
        _Settings_SendSaveResult(false, "Launch delay must be 0 or a positive number of milliseconds.")
        return false
    }
    gMultiClientCount := Integer(cnt)
    gMultiClientDelay := Integer(dly)
    ; Monitor choices come as 0-based indexes from JS; convert to 1-based + apply offset.
    gPrimaryMonitorOverride := _Settings_ChoiceToMonitorIndex(Integer(launcher["primaryMonitor"]) + 1)
    gSecondaryMonitorOverride := _Settings_ChoiceToMonitorIndex(Integer(launcher["secondaryMonitor"]) + 1)
    launchLayouts := _Settings_LaunchLayoutOptions()
    if launchLayouts.available {
        gPrimaryLaunchLayout := _Settings_ValidateLaunchLayout(
            launcher.Has("primaryLaunchLayout") ? launcher["primaryLaunchLayout"] : gPrimaryLaunchLayout,
            launchLayouts)
        gSecondaryLaunchLayout := _Settings_ValidateLaunchLayout(
            launcher.Has("secondaryLaunchLayout") ? launcher["secondaryLaunchLayout"] : gSecondaryLaunchLayout,
            launchLayouts)
    }
    ; Absent key = an older page that doesn't render the toggle; keep the
    ; current value rather than silently switching the check off.
    if launcher.Has("versionCheck")
        gVersionCheckEnabled := launcher["versionCheck"] ? true : false
    SaveLauncherConfig()
    SetRunOnStartup(launcher["autoStart"] ? true : false)

    ; ── Minimap ──
    mm := msg["minimap"]
    offX := mm["offsetX"]
    offY := mm["offsetY"]
    if (!IsInteger(offX) || !IsInteger(offY)) {
        _Settings_SendSaveResult(false, "Minimap nudge X/Y must be whole numbers (negative is allowed).")
        return false
    }
    appearance := msg.Has("appearance") ? msg["appearance"] : Map()
    nextAccentScheme := NormalizeAccentScheme(
        IsObject(appearance) && appearance.Has("accentScheme")
            ? appearance["accentScheme"] : gAccentScheme)
    previousAccentScheme := gAccentScheme
    SetAccentScheme(nextAccentScheme)

    previousScale := gMinimapScale
    gMinimapScale := Integer(mm["scale"])
    gMinimapOpacity := Integer(mm["opacity"])
    gMinimapAnchor := mm["anchor"]
    gMinimapOffsetX := Integer(offX)
    gMinimapOffsetY := Integer(offY)
    gMinimapKeepOpen := mm["keepOpen"] ? true : false
    ; Absent key = an older page; keep the current value rather than
    ; silently switching the readout off.
    if mm.Has("showHoverCoords")
        gShowHoverCoords := mm["showHoverCoords"] ? true : false
    SaveMinimapConfig()
    if (gMinimapScale != previousScale || gAccentScheme != previousAccentScheme) {
        RebuildOverlayGui()
    } else {
        ApplyOverlayOpacity()
    }

    ; ── Hotkeys ──
    hotkeys := msg["hotkeys"]
    if IsObject(hotkeys) {
        _Settings_SetPendingHotkeyRows(hotkeys)
        for entry in hotkeys {
            id := entry["id"]
            chord := NormalizeHotkeyChord(entry["chord"])
            if !gHotkeyActions.Has(id) {
                _Settings_SendSaveResult(false, "Unknown hotkey action: " id ".")
                return false
            }
            if !IsHotkeyChordValid(chord) {
                actionLabel := gHotkeyActions[id]["label"]
                _Settings_SendSaveResult(false, "Invalid shortcut for " actionLabel ".")
                return false
            }
            conflict := GetHotkeyConflictAction(chord, id)
            if (conflict != "") {
                _Settings_SendSaveResult(false, FormatHotkeyDisplay(chord) " — already used by " conflict ".")
                return false
            }
        }
        for entry in hotkeys {
            gHotkeyActions[entry["id"]]["chord"] := NormalizeHotkeyChord(entry["chord"])
        }
        SaveHotkeyOverrides()
    }

    ; ── Addon enable/disable ──
    addonToggles := msg.Has("addons") ? msg["addons"] : Map()
    if IsObject(addonToggles) {
        for nm, enabled in addonToggles {
            SetAddonEnabled(nm, enabled ? true : false)
        }
    }

    ; ── Addon settings (dynamic) ──
    addonSettings := msg.Has("addonSettings") ? msg["addonSettings"] : Map()
    if IsObject(addonSettings) {
        for _, am in gAddonHooks {
            addonName := am.Has("name") ? am["name"] : ""
            if (addonName = "" || !am.Has("OnSettingsWebSave"))
                continue
            if addonSettings.Has(addonName) {
                try am["OnSettingsWebSave"](addonSettings[addonName])
            }
        }
    }

    ApplyAllHotkeys()
    RebuildTrayMenu()
    _Settings_SendSaveResult(true)
    ; Let the WebView acknowledge completion before closing. The settings are
    ; already applied; this short handoff only makes the successful state
    ; perceptible instead of destroying the window before JS receives it.
    SetTimer(() => _Settings_Close(), -380)
    return true
}

_Settings_SendSaveResult(ok, error := "") {
    global gSettingsGui
    if (!ok && error != "" && IsObject(gSettingsGui) && Type(gSettingsGui) = "Gui") {
        MsgBox(error, "Maps++ — Settings", "Icon!")
        return
    }
    json := '{"type":"save-result","ok":' (ok ? "true" : "false")
    if (error != "")
        json .= ',"error":' _JSON_Str(error)
    json .= '}'
    _Settings_PostMessage(json)
}

; ── Browse handler ───────────────────────────────────────────

; The manual update check. Runs regardless of the startup-check preference —
; pressing the button is consent — and always answers, including on failure,
; because a button that sometimes does nothing visible looks broken.
;
; The request is bounded by VERSION_CHECK_TIMEOUT_MS, so the brief block here is
; capped at a few seconds; the page disables its button meanwhile.
_Settings_HandleCheckUpdate() {
    global APP_VERSION
    res := CheckForUpdatesNow()
    _Settings_PostMessage('{"type":"update-check-result"'
        . ',"status":' _JSON_Str(res.status)
        . ',"version":' _JSON_Str(res.version)
        . ',"notes":' _JSON_Str(res.notes)
        . ',"reason":' _JSON_Str(res.reason)
        . ',"current":' _JSON_Str(APP_VERSION) '}')
}

_Settings_HandleBrowse() {
    global PROCESS_EXE
    selected := FileSelect(1, A_ScriptDir, "Locate " PROCESS_EXE " (game executable)", "Executables (*.exe)")
    if (selected = "")
        return
    _Settings_PostMessage('{"type":"browse-result","path":' _JSON_Str(selected) '}')
}

; ── Hotkey capture bridge ────────────────────────────────────

_Settings_HandleStartCapture(msg) {
    actionId := msg["actionId"]
    global gHotkeyActions, gSettingsGui, gSettingsHotkeyRows

    if !gHotkeyActions.Has(actionId)
        return

    if msg.Has("hotkeys")
        _Settings_SetPendingHotkeyRows(msg["hotkeys"])

    action := gHotkeyActions[actionId]
    allowMouse := action.Has("allowMouse") && action["allowMouse"]
    pendingChord := action["chord"]
    if IsObject(gSettingsHotkeyRows) {
        for pendingRow in gSettingsHotkeyRows {
            if pendingRow["id"] = actionId {
                pendingChord := pendingRow["pending"]
                break
            }
        }
    }

    ; Create a temporary row-like object that StartHotkeyCapture expects.
    fakeRow := Map(
        "id", actionId,
        "pending", pendingChord,
        "default", action["default"],
        "button", _Settings_FakeButton(),
        "allowMouse", allowMouse
    )

    CancelHotkeyCapture()

    ; Start an InputHook-based capture. On success, send result to JS.
    StartHotkeyCapture(fakeRow,
        (chord) => _Settings_OnCaptureDone(actionId, chord, fakeRow),
        () => _Settings_OnCaptureCancel(actionId))
}

; A minimal object that satisfies the button.Text reads/writes
; in the hotkey capture system without an actual Gui control.
class _Settings_FakeButton {
    Text := ""
}

_Settings_OnCaptureDone(actionId, chord, row) {
    display := FormatHotkeySettingsDisplay(chord)
    _Settings_PostMessage('{"type":"hotkey-captured"'
        . ',"actionId":' _JSON_Str(actionId)
        . ',"chord":' _JSON_Str(chord)
        . ',"display":' _JSON_Str(display)
        . ',"ok":true}')
}

; Keep AHK's conflict checks aligned with the unsaved values shown by the web
; UI. This makes it possible to unbind one action and immediately reuse that
; chord for another before pressing Save.
_Settings_SetPendingHotkeyRows(hotkeys) {
    global gSettingsHotkeyRows, gHotkeyActions
    rows := []
    if IsObject(hotkeys) {
        for entry in hotkeys {
            if !entry.Has("id") || !gHotkeyActions.Has(entry["id"])
                continue
            id := entry["id"]
            chord := entry.Has("chord") ? NormalizeHotkeyChord(entry["chord"]) : ""
            rows.Push(Map(
                "id", id,
                "pending", chord,
                "action", gHotkeyActions[id]
            ))
        }
    }
    gSettingsHotkeyRows := rows
}

_Settings_OnCaptureCancel(actionId) {
    _Settings_PostMessage('{"type":"hotkey-capture-cancelled"'
        . ',"actionId":' _JSON_Str(actionId) '}')
}

; ── Close ────────────────────────────────────────────────────

_Settings_Close() {
    global gSettingsGui, gSettingsHotkeyRows
    CancelHotkeyCapture()
    gSettingsHotkeyRows := 0
    if IsObject(gSettingsGui) {
        try gSettingsGui.Destroy()
    }
    gSettingsGui := 0
}

; ── PostMessage helper ───────────────────────────────────────

_Settings_PostMessage(json) {
    global gSettingsGui
    if !IsObject(gSettingsGui) || gSettingsGui = 0
        return
    try gSettingsGui.PostWebMessageAsJson(json)
}

; ── Monitor choice helpers (preserved from original) ─────────

; Dropdown choices for the monitor pickers: "Auto" followed by each display.
_Settings_MonitorChoices() {
    choices := ["Auto"]
    Loop MonitorGetCount() {
        MonitorGet(A_Index, &l, &t, &r, &b)
        lbl := "Display " A_Index " — " (r - l) "×" (b - t)
        if (A_Index = MonitorGetPrimary())
            lbl .= " (Primary)"
        choices.Push(lbl)
    }
    return choices
}

; Override value (0 = auto, k = monitor k) → 1-based dropdown choice.
_Settings_MonitorIndexToChoice(idx) {
    if (idx < 1 || idx > MonitorGetCount())
        return 1
    return idx + 1
}

; 1-based dropdown choice → override value (choice 1 = Auto = 0).
_Settings_ChoiceToMonitorIndex(choice) {
    if (choice <= 1)
        return 0
    return choice - 1
}

; Layout choices shared by the WebView2 and native Launcher tabs. The Default
; entry is a live alias; the concrete entries below it pin a layout by name.
_Settings_LaunchLayoutOptions() {
    global LAUNCH_LAYOUT_DEFAULT
    namesFn := GetFuncByName("_WindowLayout_AllLayoutNames")
    defaultFn := GetFuncByName("_WindowLayout_GetDefaultLayoutName")
    if (!namesFn || !defaultFn)
        return { available: false, items: [] }

    defaultName := defaultFn()
    items := [
        { value: "", label: "None — center clients only" },
        { value: LAUNCH_LAYOUT_DEFAULT, label: "Default — " defaultName }
    ]
    for name in namesFn()
        items.Push({ value: name, label: name })
    return { available: true, items: items }
}

_Settings_LaunchLayoutOptionsToJson(options) {
    json := "["
    for i, item in options.items {
        if (i > 1)
            json .= ","
        json .= '{"value":' _JSON_Str(item.value) ',"label":' _JSON_Str(item.label) '}'
    }
    return json "]"
}

; Keeps a valid configured value, otherwise falls back to the live Default alias.
; When WindowLayout is absent, preserve the raw value for users who share one
; config file between variants.
_Settings_ValidateLaunchLayout(value, options) {
    global LAUNCH_LAYOUT_DEFAULT
    value := Trim(String(value))
    if !options.available
        return value
    for item in options.items {
        if (item.value = value)
            return value
    }
    return LAUNCH_LAYOUT_DEFAULT
}

; ── Minimal JSON helpers ─────────────────────────────────────
; These produce JSON strings without external dependencies.

_JSON_Str(s) {
    s := String(s)
    s := StrReplace(s, "\", "\\")
    s := StrReplace(s, "`"", "\`"")
    s := StrReplace(s, "`n", "\n")
    s := StrReplace(s, "`r", "\r")
    s := StrReplace(s, "`t", "\t")
    return "`"" s "`""
}

; Minimal JSON parser — handles the subset of JSON that our JS frontend sends.
; Supports objects, arrays, strings, numbers, booleans, null.
_JSON_Parse(str) {
    static _ := 0
    str := Trim(str)
    if (str = "")
        return ""
    ; Use the Jxon parser approach: evaluate via recursive descent.
    pos := 1
    return _JSON_ParseValue(str, &pos)
}

_JSON_ParseValue(str, &pos) {
    _JSON_SkipWhitespace(str, &pos)
    if (pos > StrLen(str))
        return ""
    ch := SubStr(str, pos, 1)
    if (ch = "`"")
        return _JSON_ParseString(str, &pos)
    if (ch = "{")
        return _JSON_ParseObject(str, &pos)
    if (ch = "[")
        return _JSON_ParseArray(str, &pos)
    if (ch = "t" && SubStr(str, pos, 4) = "true") {
        pos += 4
        return true
    }
    if (ch = "f" && SubStr(str, pos, 5) = "false") {
        pos += 5
        return false
    }
    if (ch = "n" && SubStr(str, pos, 4) = "null") {
        pos += 4
        return ""
    }
    ; Number
    return _JSON_ParseNumber(str, &pos)
}

_JSON_ParseString(str, &pos) {
    pos++ ; skip opening quote
    result := ""
    while (pos <= StrLen(str)) {
        ch := SubStr(str, pos, 1)
        if (ch = "`"") {
            pos++
            return result
        }
        if (ch = "\") {
            pos++
            esc := SubStr(str, pos, 1)
            pos++
            switch esc {
                case "`"": result .= "`""
                case "\": result .= "\"
                case "/": result .= "/"
                case "n": result .= "`n"
                case "r": result .= "`r"
                case "t": result .= "`t"
                case "b": result .= "`b"
                case "u": result .= _JSON_ParseUnicodeEscape(str, &pos)
                default: result .= esc
            }
        } else {
            result .= ch
            pos++
        }
    }
    return result
}

; \uXXXX, called with `pos` on the first hex digit and leaving it just past the
; last one consumed.
;
; Without this the escape fell through to the switch's `default` and a "é"
; landed in the string as a literal "u00e9" — every non-ASCII character coming
; back from a WebView2 panel was silently mangled. Two addons worked around it
; by refusing to accept names from JS at all (see the comments in shop_prices
; and window_layout); map_pois did not, and corrupted POI labels.
;
; JS emits astral characters as a surrogate pair, so a high surrogate followed
; by "\uDCxx" is rejoined into the one code point Chr() expects.
_JSON_ParseUnicodeEscape(str, &pos) {
    hi := _JSON_ParseHex4(str, &pos)
    if (hi < 0)
        return "u"                      ; malformed — keep the old literal behaviour
    if (hi < 0xD800 || hi > 0xDBFF)
        return (hi >= 0xDC00 && hi <= 0xDFFF) ? Chr(0xFFFD) : Chr(hi)

    ; High surrogate: only consume the low half if it is really there.
    if (SubStr(str, pos, 2) != "\u")
        return Chr(0xFFFD)
    after := pos + 2
    lo := _JSON_ParseHex4(str, &after)
    if (lo < 0xDC00 || lo > 0xDFFF)
        return Chr(0xFFFD)
    pos := after
    return Chr(0x10000 + ((hi - 0xD800) << 10) + (lo - 0xDC00))
}

; Four hex digits at `pos` as an integer, advancing past them. -1 when the text
; is not four hex digits, in which case `pos` is left alone.
_JSON_ParseHex4(str, &pos) {
    digits := SubStr(str, pos, 4)
    if (StrLen(digits) < 4 || !RegExMatch(digits, "^[0-9A-Fa-f]{4}$"))
        return -1
    pos += 4
    return Integer("0x" digits)
}

_JSON_ParseNumber(str, &pos) {
    start := pos
    if (SubStr(str, pos, 1) = "-")
        pos++
    while (pos <= StrLen(str) && RegExMatch(SubStr(str, pos, 1), "[0-9.eE+\-]"))
        pos++
    numStr := SubStr(str, start, pos - start)
    ; A value that is not a number at all leaves numStr empty, and Integer("")
    ; throws. Fall back to 0 instead: the caller's validation is what should
    ; reject a malformed body, not an exception from three frames down.
    if (numStr = "" || !IsNumber(numStr))
        return 0
    if InStr(numStr, ".") || InStr(numStr, "e") || InStr(numStr, "E")
        return Float(numStr)
    return Integer(numStr)
}

_JSON_ParseObject(str, &pos) {
    pos++ ; skip {
    obj := Map()
    _JSON_SkipWhitespace(str, &pos)
    if (SubStr(str, pos, 1) = "}") {
        pos++
        return obj
    }
    ; Every exit below is a bail-out on malformed input. Without them this loop
    ; never terminates on a truncated body — `{"version":` used to spin forever,
    ; hanging the whole app, because a value parsed past the end of the string
    ; advances nothing and nothing checked for it. Returning what was parsed so
    ; far lets the caller's own validation reject it.
    loop {
        _JSON_SkipWhitespace(str, &pos)
        if (pos > StrLen(str) || SubStr(str, pos, 1) != '"')
            return obj                      ; truncated, or not a key
        key := _JSON_ParseString(str, &pos)
        _JSON_SkipWhitespace(str, &pos)
        if (SubStr(str, pos, 1) != ":")
            return obj
        pos++ ; skip :
        value := _JSON_ParseValue(str, &pos)
        obj[key] := value
        _JSON_SkipWhitespace(str, &pos)
        ch := SubStr(str, pos, 1)
        if (ch = "}") {
            pos++
            return obj
        }
        if (ch != ",")
            return obj                      ; ran off the end
        pos++ ; skip ,
    }
}

_JSON_ParseArray(str, &pos) {
    pos++ ; skip [
    arr := []
    _JSON_SkipWhitespace(str, &pos)
    if (SubStr(str, pos, 1) = "]") {
        pos++
        return arr
    }
    ; Same termination guards as _JSON_ParseObject — see the note there.
    loop {
        _JSON_SkipWhitespace(str, &pos)
        if (pos > StrLen(str))
            return arr
        value := _JSON_ParseValue(str, &pos)
        arr.Push(value)
        _JSON_SkipWhitespace(str, &pos)
        ch := SubStr(str, pos, 1)
        if (ch = "]") {
            pos++
            return arr
        }
        if (ch != ",")
            return arr
        pos++ ; skip ,
    }
}

_JSON_SkipWhitespace(str, &pos) {
    while (pos <= StrLen(str) && RegExMatch(SubStr(str, pos, 1), "[ \t\n\r]"))
        pos++
}

; ── OnSettings hook contract (legacy, for documentation) ─────
; The old OnSettings hook (native Gui) is no longer called.
; Addons should migrate to OnSettingsWeb + OnSettingsWebSave.
;
; ── OnSettingsWeb hook contract ──────────────────────────────
; An addon opts into the web-based settings window by adding to its RegisterAddon map:
;     "OnSettingsWeb",      MyAddon_OnSettingsWeb      ; fn() → Array of field Maps
;     "OnSettingsWebSave",  MyAddon_OnSettingsWebSave   ; fn(values) — receives saved fields
;     "settingsLabel",      "My Addon"                  ; optional tab title
;
; OnSettingsWeb returns an Array of field descriptors (Maps):
;   Map("type","info",     "text","Description text here")
;   Map("type","checkbox", "id","enabled", "label","Enable feature", "value",true)
;   Map("type","dropdown", "id","mode",    "label","Mode", "options",["A","B"], "value",0)
;   Map("type","combo",    "id","name",    "label","Name", "options",["x","y"], "value","x")
;   Map("type","number",   "id","count",   "label","Count","value",5, "min",0, "max",100)
;   Map("type","orderedlist", "id","picks", "label","Shown, in order",
;       "items",[Map("id","a","label","Alpha","icon","map"), …],  ; everything offerable
;       "value","b,a",                                            ; chosen ids, in order
;       "max",10)                                                 ; optional cap
;
; OnSettingsWebSave receives a Map of { fieldId: value } for the addon's fields.
; Boolean fields arrive as true/false. Dropdown fields arrive as 0-based index.
; Orderedlist fields arrive as a comma-separated id string — from both the web
; and the native frontend, so an addon has one save path to write.
