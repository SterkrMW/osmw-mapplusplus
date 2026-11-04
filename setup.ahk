#Requires AutoHotkey v2.0

; ── Setup guide ───────────────────────────────────────────────────
; The first thing a new install sees, and Help → Setup guide… afterwards. It
; asks the handful of things Maps++ cannot guess — where the game is, what Tab
; should do, one character or several — and leaves everything else at its
; default, saying where to change it later.
;
; Before this, a new user got an unexplained file picker for main.exe that
; blocked startup and came back on every launch until answered, then a
; default that launched five clients for someone who plays one.
;
; Two frontends, one writer. The WebView2 page (ui\setup\) is a stepped guide;
; the native one is a single form with the same choices. Both read
; _Setup_State() and both end in Setup_Apply(), which uses the same lower-level
; savers Settings does (never _Settings_HandleSave, which is bound to the
; Settings window).
;
; Who sees it (Setup_DetectFirstRun, run before anything this session writes):
;   [Setup] Completed set ("1", "skipped", "upgrade")  → nobody
;   unset, and an earlier install left a trace          → an upgrade: no guide,
;       the defaults this version changed are pinned to what they were, and
;       one notice says where the tray's things went
;   unset, no trace                                     → the guide
; Completed is read with a direct IniRead and kept out of defaults.ini, like
; SourceNoticeShown: it is a record of what happened, not a preference.

global gSetupGui := 0           ; the open guide's window, or 0
global gSetupMode := ""         ; "web" | "native" while one is open
global gSetupActive := false    ; on screen: other first-run prompts wait
global gSetupFirstRun := false  ; this session opened it as the first-run guide

; ── Who sees it ───────────────────────────────────────────────────

; "new", "upgrade" or "done". Must run before the first log line and before any
; config write this session, because both are how an earlier install is told
; apart from a fresh one.
Setup_DetectFirstRun() {
    global CONFIG_INI
    done := ""
    try done := Trim(IniRead(CONFIG_INI, "Setup", "Completed", ""))
    if (done != "")
        return "done"
    return _Setup_HasPriorTrace() ? "upgrade" : "new"
}

; Anything an earlier version would have left. File existence alone proves
; nothing: ResolveWritableIniPath creates the inis it resolves, empty, as it
; probes whether their folder is writable.
_Setup_HasPriorTrace() {
    global CONFIG_INI, SIGNATURES_INI, OFFSETS_CACHE_INI
    sections := ""
    try sections := Trim(IniRead(CONFIG_INI))
    if (sections != "")
        return true
    appData := A_AppData "\osMW Maps++\"
    for path in [SIGNATURES_INI, OFFSETS_CACHE_INI,
        A_ScriptDir "\mapsplusplus.log", appData "mapsplusplus.log",
        A_ScriptDir "\layouts.ini", appData "layouts.ini"] {
        size := 0
        try size := FileGetSize(path)
        if (size > 0)
            return true
    }
    return false
}

; An existing install meeting this version. Its config may never have saved
; the values whose shipped defaults changed, in which case it has been running
; on defaults.ini's — so those are written down as they were, before anything
; reads them, and nothing it does changes under the user.
_Setup_MarkUpgrade() {
    global CONFIG_INI, gHotkeyActions
    EnsureIniUtf16(CONFIG_INI)
    if (IniRead(CONFIG_INI, "Launcher", "MultiClientCount", "__MISSING__") = "__MISSING__")
        IniWrite("5", CONFIG_INI, "Launcher", "MultiClientCount")
    ; The old ring, but only where it showed anything: in Lite none of these
    ; actions exist, and pinning them would keep that build's ring empty.
    oldRing := ["viewModeToggle", "battleSend", "inventoryOpenClick", "chatToggleAll"]
    if (IniRead(CONFIG_INI, "QuickActions", "Actions", "__MISSING__") = "__MISSING__") {
        for id in oldRing {
            if gHotkeyActions.Has(id) {
                IniWrite("viewModeToggle,battleSend,inventoryOpenClick,chatToggleAll",
                    CONFIG_INI, "QuickActions", "Actions")
                break
            }
        }
    }
    IniWrite("upgrade", CONFIG_INI, "Setup", "Completed")
}

; After the tray exists. Each case is deferred: startup should finish, and an
; open guide must not race the tray's own prewarm.
Setup_AfterStartup(firstRunState) {
    global CONFIG_INI, gSetupFirstRun
    if (firstRunState = "new") {
        gSetupFirstRun := true
        LogInfo("Setup", "First run: opening the setup guide")
        SetTimer(Setup_Show, -1500)
    } else if (firstRunState = "upgrade") {
        LogInfo("Setup", "Existing install: setup guide not shown, changed defaults pinned")
        SetTimer(() => TrayTip("The tray menu has been reorganized. Help → Setup guide walks"
            . " through the main settings again if you want it.", "Maps++", "Iconi"), -4000)
    }
    ; "Launch the game now" from a guide that had to restart Maps++.
    if (IniRead(CONFIG_INI, "Setup", "LaunchAfterReload", "") = "1") {
        try IniDelete(CONFIG_INI, "Setup", "LaunchAfterReload")
        SetTimer(() => LaunchConfiguredClients("primary"), -2500)
    }
}

_Setup_DiagnosticsLabel() {
    global CONFIG_INI
    done := ""
    try done := Trim(IniRead(CONFIG_INI, "Setup", "Completed", ""))
    switch done {
        case "1":       return "guide finished"
        case "skipped": return "guide skipped"
        case "upgrade": return "existing install (guide not shown)"
        default:        return "not answered yet"
    }
}

; ── Opening and closing ───────────────────────────────────────────

Setup_Show(*) {
    global gSetupGui, gSetupMode, gSetupActive
    if (gSetupMode != "" && IsObject(gSetupGui) && gSetupGui.Hwnd) {
        try WinActivate("ahk_id " gSetupGui.Hwnd)
        return
    }
    gSetupActive := true
    if (IsNativeInterface() || !_Setup_CanUseWebView()) {
        _Setup_ShowNative()
        return
    }
    try {
        _Setup_ShowWeb()
    } catch as err {
        MarkWebView2Unavailable("Setup guide", err)
        _Setup_DestroyWindow()
        gSetupActive := true
        _Setup_ShowNative()
    }
}

_Setup_CanUseWebView() {
    return FileExist(A_ScriptDir "\Lib\WebViewToo.ahk")
        && FileExist(A_ScriptDir "\ui\setup\index.html")
}

; Closed without finishing. On the first run that still counts as answered —
; the guide is in the Help menu, and asking again at every start is how the
; old file picker behaved.
_Setup_Dismiss(*) {
    global gSetupFirstRun, CONFIG_INI
    if gSetupFirstRun {
        try IniWrite("skipped", CONFIG_INI, "Setup", "Completed")
        gSetupFirstRun := false
        LogInfo("Setup", "Setup guide skipped")
        TrayTip("Everything is at its default. Help → Setup guide in the tray menu"
            . " brings this back.", "Maps++", "Iconi")
    }
    _Setup_DestroyWindow()
}

_Setup_DestroyWindow() {
    global gSetupGui, gSetupMode, gSetupActive
    CancelHotkeyCapture()
    if IsObject(gSetupGui)
        try gSetupGui.Destroy()
    gSetupGui := 0
    gSetupMode := ""
    gSetupActive := false
}

; ── What both frontends show ──────────────────────────────────────

; Where main.exe might be: beside Maps++, the saved path, and every running
; client (the most reliable of the three — it is the game the user plays).
_Setup_GameCandidates() {
    global gGamePath, PROCESS_EXE
    out := []
    seen := Map()
    add(path, source) {
        if (path = "" || !FileExist(path))
            return
        key := StrLower(path)
        if seen.Has(key)
            return
        seen[key] := true
        out.Push(Map("path", path, "source", source))
    }
    add(gGamePath, "Saved")
    add(A_ScriptDir "\" PROCESS_EXE, "Beside Maps++")
    for hwnd in GetTopLevelGameWindows() {
        exePath := ""
        try exePath := ProcessGetPath(WinGetPID("ahk_id " hwnd))
        add(exePath, "Running now")
    }
    return out
}

; Accounts saved for auto-login, for the Launching step's "N saved" line.
_Setup_AccountCount() {
    fn := GetFuncByName("_AutoLogin_LaunchChoices")
    if !(fn && IsAddonEnabled("AutoLogin"))
        return -1
    choices := ""
    try choices := fn()
    return IsObject(choices) ? choices["rows"].Length : 0
}

; Primary and secondary as the guide shows them: the bound profiles' counts,
; displays and layouts, with "several characters" assumed when the user
; already launches more than one client or has a second team bound.
_Setup_LaunchState() {
    global gPrimaryProfileId, gSecondaryProfileId, gMultiClientCount
    prim := ResolveLaunchProfile("primary")
    primCount := (prim["count"] >= 1) ? prim["count"] : Max(1, gMultiClientCount)
    secBound := (gSecondaryProfileId != "" && gSecondaryProfileId != gPrimaryProfileId)
    sec := secBound ? ResolveLaunchProfile("secondary") : ""
    return {
        style: (secBound || primCount > 1) ? "multi" : "solo",
        primaryCount: primCount,
        primaryMonitor: _Settings_MonitorIndexToChoice(prim["monitor"]) - 1,
        primaryLayout: prim["layout"],
        secondEnabled: secBound,
        secondCount: IsObject(sec) ? ((sec["count"] >= 1) ? sec["count"] : primCount) : primCount,
        ; 0 is Auto, which for the secondary launch means the first display
        ; that is not the primary one.
        secondMonitor: IsObject(sec) ? (_Settings_MonitorIndexToChoice(sec["monitor"]) - 1) : 0
    }
}

; The few keys worth knowing on day one, as they are bound now.
_Setup_Shortcuts() {
    global gHotkeyActions
    out := []
    for pair in [["toggleMinimap", "Minimap"], ["openMapsppMinimap", "Maps++ minimap"],
        ["launchPrimary", "Launch"], ["quickActionsToggle", "Action ring"],
        ["openSettings", "Settings"]] {
        if !gHotkeyActions.Has(pair[1]) || !IsHotkeyActionEnabled(gHotkeyActions[pair[1]])
            continue
        keys := GetHotkeyDisplay(pair[1])
        if (keys != "")
            out.Push(Map("label", pair[2], "keys", keys))
    }
    return out
}

_Setup_StateJson() {
    global gGamePath, gMinimapSource, MINIMAP_SOURCES, gLaunchAutoLogin
    global gInterfaceMode, gAccentScheme, gSetupFirstRun

    candidates := ""
    for c in _Setup_GameCandidates()
        candidates .= (candidates = "" ? "" : ",")
            . '{"path":' _JSON_Str(c["path"]) ',"source":' _JSON_Str(c["source"]) '}'
    status := _Settings_GameStatus(gGamePath)

    monitors := ""
    for i, lbl in _Settings_MonitorChoices()
        monitors .= (i > 1 ? "," : "") _JSON_Str(lbl)
    layouts := _Settings_LaunchLayoutOptions()
    launch := _Setup_LaunchState()

    features := ""
    for entry in _Settings_FeatureList()
        features .= (features = "" ? "" : ",")
            . '{"name":' _JSON_Str(entry["name"])
            . ',"label":' _JSON_Str(entry["label"])
            . ',"description":' _JSON_Str(entry["description"])
            . ',"group":' _JSON_Str(entry["group"])
            . ',"multibox":' (entry["multibox"] ? "true" : "false")
            . ',"enabled":' (entry["enabled"] ? "true" : "false") '}'

    shortcuts := ""
    for s in _Setup_Shortcuts()
        shortcuts .= (shortcuts = "" ? "" : ",")
            . '{"label":' _JSON_Str(s["label"]) ',"keys":' _JSON_Str(s["keys"]) '}'

    return '{"type":"setup-state"'
        . ',"firstRun":' (gSetupFirstRun ? "true" : "false")
        . ',"game":{"path":' _JSON_Str(gGamePath)
            . ',"status":' _JSON_Str(status.text) ',"tone":' _JSON_Str(status.tone)
            . ',"candidates":[' candidates ']}'
        . ',"minimap":{"source":' _JSON_Str(gMinimapSource)
            . ',"openChord":' _JSON_Str(GetHotkeyChord("openMapsppMinimap"))
            . ',"openChordDisplay":' _JSON_Str(GetHotkeyDisplay("openMapsppMinimap")) '}'
        . ',"launch":{"style":' _JSON_Str(launch.style)
            . ',"primaryCount":' launch.primaryCount
            . ',"primaryMonitor":' launch.primaryMonitor
            . ',"primaryLayout":' _JSON_Str(_Settings_ValidateLaunchLayout(launch.primaryLayout, layouts))
            . ',"secondEnabled":' (launch.secondEnabled ? "true" : "false")
            . ',"secondCount":' launch.secondCount
            . ',"secondMonitor":' launch.secondMonitor
            . ',"monitors":[' monitors ']'
            . ',"layoutAvailable":' (layouts.available ? "true" : "false")
            . ',"layouts":' _Settings_LaunchLayoutOptionsToJson(layouts)
            . ',"autoLogin":' (gLaunchAutoLogin ? "true" : "false")
            . ',"accountsAvailable":' (_Settings_AccountsEditorAvailable() ? "true" : "false")
            . ',"accountCount":' _Setup_AccountCount()
            . ',"modsInstalled":' (NativeModsInstalled() ? "true" : "false") '}'
        . ',"features":[' features ']'
        . ',"finish":{"interfaceMode":' _JSON_Str(gInterfaceMode)
            . ',"runtimeAvailable":' (WebView2RuntimeAvailable() ? "true" : "false")
            . ',"accent":' _JSON_Str(gAccentScheme)
            . ',"autoStart":' (IsRunOnStartupEnabled() ? "true" : "false")
            . ',"shortcuts":[' shortcuts ']}'
        . '}'
}

; ── Applying ──────────────────────────────────────────────────────

; Picked from the guide (Browse or a detected path). Saved at once rather than
; at Finish: the accounts editor, the client-mods check and the guide's own
; next steps all read the game folder from gGamePath.
_Setup_UseGamePath(path) {
    global gGamePath
    path := Trim(String(path))
    if (path = "" || !FileExist(path) || !(SubStr(path, -4) = ".exe"))
        return false
    gGamePath := path
    SaveGamePathToConfig(path)
    LogInfo("Setup", "Game path set: " path)
    return true
}

; The one writer. `c` is a Map of the choices, in the shape the page posts.
Setup_Apply(c) {
    global gLaunchProfiles, gPrimaryProfileId, gSecondaryProfileId, gLaunchAutoLogin
    global gHotkeyActions, CONFIG_INI, LAUNCH_LAYOUT_DEFAULT

    pick(key, fallback := "") => c.Has(key) ? c[key] : fallback

    if (pick("gamePath") != "")
        _Setup_UseGamePath(pick("gamePath"))

    ; ── Minimap ──
    if (pick("minimapSource") != "")
        SetMinimapSource(pick("minimapSource"))
    ; Only this one key is written: SaveHotkeyOverrides would freeze every
    ; other action's current default into config.ini as an explicit choice.
    if pick("openChordSet", false) {
        chord := NormalizeHotkeyChord(pick("openChord"))
        if (chord = "" || (IsHotkeyChordValid(chord)
            && GetHotkeyConflictAction(chord, "openMapsppMinimap", false) = "")) {
            gHotkeyActions["openMapsppMinimap"]["chord"] := chord
            EnsureIniUtf16(CONFIG_INI)
            IniWrite(chord, CONFIG_INI, "Hotkeys", "openMapsppMinimap")
        }
    }

    ; ── Launching ──
    prim := _Setup_PrimaryProfile()
    layouts := _Settings_LaunchLayoutOptions()
    if (pick("style") = "solo") {
        ; One character: one client, and no second launch in the tray.
        prim["count"] := 1
        gSecondaryProfileId := ""
    } else {
        prim["count"] := _Setup_Count(pick("primaryCount", 1))
        prim["monitor"] := _Settings_ChoiceToMonitorIndex(_Setup_Int(pick("primaryMonitor", 0)) + 1)
        if layouts.available
            prim["layout"] := _Settings_ValidateLaunchLayout(pick("primaryLayout", LAUNCH_LAYOUT_DEFAULT), layouts)
        if pick("secondEnabled", false) {
            sec := _Setup_SecondaryProfile()
            sec["count"] := _Setup_Count(pick("secondCount", 1))
            sec["monitor"] := _Settings_ChoiceToMonitorIndex(_Setup_Int(pick("secondMonitor", 1)) + 1)
        } else {
            gSecondaryProfileId := ""
        }
    }
    if c.Has("autoLogin")
        gLaunchAutoLogin := pick("autoLogin") ? true : false
    SaveLauncherConfig()

    ; ── Features ──
    ; A change only fully takes on a restart: an addon switched on has had no
    ; OnInit this session, and one switched off keeps its timers running.
    addonsChanged := false
    features := pick("features", Map())
    if IsObject(features) {
        for name, want in features {
            want := want ? true : false
            if (IsAddonEnabled(name) != want) {
                addonsChanged := true
                SetAddonEnabled(name, want)
            }
        }
    }

    ; ── Finish ──
    if (pick("accent") != "" && SetAccentScheme(pick("accent")))
        RebuildOverlayGui()
    if c.Has("autoStart")
        SetRunOnStartup(pick("autoStart") ? true : false)
    interfaceChanged := (pick("interfaceMode") != "") ? SetInterfaceMode(pick("interfaceMode"), false) : false

    IniWrite("1", CONFIG_INI, "Setup", "Completed")
    ApplyAllHotkeys()
    RebuildTrayMenu()
    LogInfo("Setup", "Setup guide finished (" pick("style") ")"
        . (addonsChanged ? ", features changed" : "") (interfaceChanged ? ", interface changed" : ""))

    restart := addonsChanged || interfaceChanged
    if pick("launchNow", false) {
        if restart
            IniWrite("1", CONFIG_INI, "Setup", "LaunchAfterReload")
        else
            SetTimer(() => LaunchConfiguredClients("primary"), -600)
    }
    if restart
        SetTimer(() => Reload(), -400)
    return restart
}

_Setup_Int(v) {
    return IsInteger(v) ? Integer(v) : 0
}

_Setup_Count(v) {
    n := _Setup_Int(v)
    return (n < 1) ? 1 : (n > 32 ? 32 : n)
}

_Setup_PrimaryProfile() {
    global gLaunchProfiles, gPrimaryProfileId, LAUNCH_LAYOUT_DEFAULT
    if gLaunchProfiles.Has(gPrimaryProfileId)
        return gLaunchProfiles[gPrimaryProfileId]
    id := NextLaunchProfileId()
    gLaunchProfiles[id] := Map("id", id, "name", "Primary", "accounts", "", "count", 1,
        "monitor", 0, "layout", LAUNCH_LAYOUT_DEFAULT)
    gPrimaryProfileId := id
    return gLaunchProfiles[id]
}

; The profile for a second team: the one already bound, or one named
; "Secondary", or a new one — never some other profile the user made for a
; different purpose.
_Setup_SecondaryProfile() {
    global gLaunchProfiles, gPrimaryProfileId, gSecondaryProfileId, LAUNCH_LAYOUT_DEFAULT
    if (gSecondaryProfileId != "" && gSecondaryProfileId != gPrimaryProfileId
        && gLaunchProfiles.Has(gSecondaryProfileId))
        return gLaunchProfiles[gSecondaryProfileId]
    for _, id in LaunchProfileIds() {
        if (id != gPrimaryProfileId && gLaunchProfiles[id]["name"] = "Secondary") {
            gSecondaryProfileId := id
            return gLaunchProfiles[id]
        }
    }
    id := NextLaunchProfileId()
    gLaunchProfiles[id] := Map("id", id, "name", "Secondary", "accounts", "", "count", 1,
        "monitor", 0, "layout", LAUNCH_LAYOUT_DEFAULT)
    gSecondaryProfileId := id
    return gLaunchProfiles[id]
}

; ── WebView2 guide ────────────────────────────────────────────────

_Setup_ShowWeb() {
    global gSetupGui, gSetupMode
    dllDir := (A_PtrSize = 8) ? "64bit" : "32bit"
    wvSettings := { DllPath: A_ScriptDir "\Lib\" dllDir "\WebView2Loader.dll",
        DefaultWidth: 640, DefaultHeight: 640 }
    g := WebViewGui("-Caption +AlwaysOnTop -Resize", "Maps++ — Setup", , wvSettings)
    gSetupGui := g
    gSetupMode := "web"
    g.OnEvent("Close", (*) => _Setup_Dismiss())
    g.WebMessageReceived(WebMsgHandler(_Setup_OnWebMessage))
    g.Navigate(UiPageUrl("ui/setup/index.html"))
    g.Show("w640 h640 Center")
}

_Setup_Post(json) {
    global gSetupGui, gSetupMode
    if (gSetupMode = "web" && IsObject(gSetupGui))
        try gSetupGui.PostWebMessageAsJson(json)
}

_Setup_OnWebMessage(wv, args) {
    global gSetupGui, PROCESS_EXE
    msgStr := ""
    try msgStr := args.TryGetWebMessageAsString()
    if (msgStr = "")
        try msgStr := args.WebMessageAsJson
    if (msgStr = "")
        return
    msg := _JSON_Parse(msgStr)
    if !IsObject(msg) || !msg.Has("type")
        return

    switch msg["type"] {
        case "init-request":
            _Setup_Post(_Setup_StateJson())
        case "browse-game-path":
            ; FileSelect is a plain Win32 dialog that needs nothing from this
            ; page, so it is safe here — Settings' Browse does the same.
            selected := FileSelect(1, A_ScriptDir, "Locate " PROCESS_EXE " (game executable)",
                "Executables (*.exe)")
            if (selected != "")
                _Setup_PostGame(_Setup_UseGamePath(selected))
        case "use-game-path":
            _Setup_PostGame(_Setup_UseGamePath(msg.Has("path") ? msg["path"] : ""))
        case "refresh":
            _Setup_Post(_Setup_StateJson())
        case "start-capture":
            _Setup_StartCapture()
        case "cancel-capture":
            CancelHotkeyCapture()
        case "edit-accounts":
            ; Another WebView2 window: it cannot be built inside this call.
            DeferFromWebMessage(_Settings_OpenAccounts)
        case "finish":
            DeferFromWebMessage(_Setup_FinishWeb, msg.Has("choices") ? msg["choices"] : Map())
        case "skip", "close":
            DeferFromWebMessage(_Setup_Dismiss)
    }
}

_Setup_PostGame(ok) {
    global gGamePath
    status := _Settings_GameStatus(gGamePath)
    _Setup_Post('{"type":"game-result","ok":' (ok ? "true" : "false")
        . ',"path":' _JSON_Str(gGamePath)
        . ',"status":' _JSON_Str(status.text) ',"tone":' _JSON_Str(status.tone)
        . ',"modsInstalled":' (NativeModsInstalled() ? "true" : "false")
        . ',"accountCount":' _Setup_AccountCount() '}')
}

; The key for the Maps++ minimap, captured the way Settings captures one but
; owned by this window and checked against saved chords only.
_Setup_StartCapture() {
    global gSetupGui
    row := Map(
        "id", "openMapsppMinimap",
        "pending", GetHotkeyChord("openMapsppMinimap"),
        "default", "",
        "button", _Settings_FakeButton(),
        "ownerHwnd", gSetupGui.Hwnd)
    StartHotkeyCapture(row,
        (chord) => _Setup_Post('{"type":"capture-result","chord":' _JSON_Str(chord)
            . ',"display":' _JSON_Str(FormatHotkeyDisplay(chord)) '}'),
        () => _Setup_Post('{"type":"capture-cancelled"}'))
}

_Setup_FinishWeb(choices) {
    global gSetupGui
    if !(IsObject(gSetupGui) && gSetupGui.Hwnd)
        return
    Setup_Apply(IsObject(choices) ? choices : Map())
    _Setup_DestroyWindow()
}

; ── Native guide ──────────────────────────────────────────────────
; One window rather than steps: the same choices, top to bottom, feeding the
; same Setup_Apply. The Maps++ minimap key is left to Settings → Hotkeys here.

_Setup_ShowNative() {
    global gSetupGui, gSetupMode, gGamePath, gMinimapSource, MINIMAP_SOURCES
    global MINIMAP_SOURCE_LABELS, gLaunchAutoLogin, gSetupFirstRun

    launch := _Setup_LaunchState()
    features := _Settings_FeatureList()

    g := Gui("+AlwaysOnTop -MinimizeBox", "Maps++ — Setup")
    gSetupGui := g
    gSetupMode := "native"
    g.SetFont("s9", "Segoe UI")
    g.OnEvent("Close", (*) => _Setup_Dismiss())
    g.OnEvent("Escape", (*) => _Setup_Dismiss())

    g.SetFont("s12 Bold")
    g.Add("Text", "x16 y14 w560", "Set up Maps++")
    g.SetFont("s9 Norm")
    g.Add("Text", "x16 y+6 w560",
        "A detailed minimap with your position, plus tools for running several clients."
        . " Everything here can be changed later in Settings.")

    ; Game
    g.SetFont("Bold")
    g.Add("Text", "x16 y+16 w560", "The game")
    g.SetFont("Norm")
    pathEdit := g.Add("Edit", "x16 y+6 w464 ReadOnly", gGamePath)
    g.Add("Button", "x+8 yp-1 w88", "Browse…").OnEvent("Click", Browse)
    statusText := g.Add("Text", "x16 y+6 w560 h30 c666666", _Settings_GameStatus(gGamePath).text)

    ; Minimap
    g.SetFont("Bold")
    g.Add("Text", "x16 y+8 w560", "When I press Tab")
    g.SetFont("Norm")
    sourceDdl := g.Add("DropDownList", "x16 y+6 w560", MINIMAP_SOURCE_LABELS)
    sourceDdl.Value := 1
    for i, src in MINIMAP_SOURCES {
        if (src = gMinimapSource)
            sourceDdl.Value := i
    }

    ; How you play
    g.SetFont("Bold")
    g.Add("Text", "x16 y+16 w560", "How I play")
    g.SetFont("Norm")
    soloRb := g.Add("Radio", "x16 y+6 w250 Group", "One character")
    multiRb := g.Add("Radio", "x+10 yp w300", "Several characters at once")
    if (launch.style = "multi")
        multiRb.Value := 1
    else
        soloRb.Value := 1
    g.Add("Text", "x34 y+10 w120", "Clients per launch:")
    countEdit := g.Add("Edit", "x160 yp-3 w60 Number", String(launch.primaryCount))
    g.Add("UpDown", "Range1-32", launch.primaryCount)
    g.Add("Text", "x236 yp+3 w50", "Display:")
    monitorDdl := g.Add("DropDownList", "x290 yp-3 w286", _Settings_MonitorChoices())
    monitorDdl.Value := launch.primaryMonitor + 1
    autoLoginCb := g.Add("CheckBox", "x34 y+10 w380", "Log launched clients in automatically")
    autoLoginCb.Value := gLaunchAutoLogin ? 1 : 0
    accountsBtn := 0
    if _Settings_AccountsEditorAvailable() {
        accountsBtn := g.Add("Button", "x+8 yp-4 w150", "Set up accounts…")
        accountsBtn.OnEvent("Click", (*) => _Settings_OpenAccounts())
    }
    soloRb.OnEvent("Click", StyleChanged)
    multiRb.OnEvent("Click", StyleChanged)

    ; Features
    g.SetFont("Bold")
    g.Add("Text", "x16 y+16 w560", "Features")
    g.SetFont("Norm")
    featureLv := g.Add("ListView", "x16 y+6 w560 h150 Checked -Multi NoSortHdr", ["Feature", "What it does"])
    for entry in features
        featureLv.Add(entry["enabled"] ? "Check" : "", entry["label"], entry["description"])
    featureLv.ModifyCol(1, 130)
    featureLv.ModifyCol(2, 400)

    autoStartCb := g.Add("CheckBox", "x16 y+12 w560", "Start Maps++ when Windows starts")
    autoStartCb.Value := IsRunOnStartupEnabled() ? 1 : 0
    launchNowCb := g.Add("CheckBox", "x16 y+6 w560", "Launch the game when I click Finish")

    g.Add("Button", "x312 y+16 w128", gSetupFirstRun ? "Skip setup" : "Cancel")
        .OnEvent("Click", (*) => _Setup_Dismiss())
    g.Add("Button", "x+8 yp w128 Default", "Finish").OnEvent("Click", Finish)

    StyleChanged()
    g.Show("w592 AutoSize Center")

    Browse(*) {
        global PROCESS_EXE
        selected := FileSelect(1, A_ScriptDir, "Locate " PROCESS_EXE " (game executable)",
            "Executables (*.exe)")
        if (selected != "" && _Setup_UseGamePath(selected)) {
            pathEdit.Value := selected
            statusText.Text := _Settings_GameStatus(selected).text
        }
    }

    ; One character leaves the multi-boxing features unticked and the count
    ; at one; several puts them back. Only the multibox rows move, so a
    ; choice made elsewhere in the list survives switching back and forth.
    StyleChanged(*) {
        multi := multiRb.Value ? true : false
        for ctrl in [countEdit, monitorDdl]
            ctrl.Enabled := multi
        for i, entry in features {
            if entry["multibox"]
                featureLv.Modify(i, multi ? "Check" : "-Check")
        }
    }

    Finish(*) {
        multi := multiRb.Value ? true : false
        checked := Map()
        rowNum := 0
        loop {
            rowNum := featureLv.GetNext(rowNum, "C")
            if !rowNum
                break
            checked[rowNum] := true
        }
        picks := Map()
        for i, entry in features
            picks[entry["name"]] := checked.Has(i)
        Setup_Apply(Map(
            "style", multi ? "multi" : "solo",
            "minimapSource", MINIMAP_SOURCES[sourceDdl.Value],
            "primaryCount", Trim(countEdit.Value),
            "primaryMonitor", monitorDdl.Value - 1,
            ; This form has no second-team controls, so whatever is bound
            ; stays bound (Settings → Launching edits it).
            "secondEnabled", launch.secondEnabled,
            "secondCount", launch.secondCount,
            "secondMonitor", launch.secondMonitor,
            "autoLogin", autoLoginCb.Value ? true : false,
            "features", picks,
            "autoStart", autoStartCb.Value ? true : false,
            "launchNow", launchNowCb.Value ? true : false))
        _Setup_DestroyWindow()
    }
}
