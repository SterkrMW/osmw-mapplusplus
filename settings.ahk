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
    ; Logged before and after, so a report of Settings "not opening" can say
    ; whether the window was ever finished.
    started := A_TickCount
    if IsNativeInterface() {
        LogInfo("Settings", "Opening (native)")
        _Settings_BuildNative()
        LogInfo("Settings", "Open (native) after " (A_TickCount - started) " ms")
        return
    }
    LogInfo("Settings", "Opening (enhanced)")
    if _Settings_CanUseWebView() {
        ; Settings is where a user goes to fix things, so it must open even if
        ; WebView2 fails despite the startup probe.
        try {
            _Settings_BuildWebView()
        } catch as err {
            MarkWebView2Unavailable("Settings", err)
            if IsObject(gSettingsGui)
                try gSettingsGui.Destroy()
            gSettingsGui := 0
            _Settings_BuildNative()
        }
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
    g.WebMessageReceived(WebMsgHandler(_Settings_OnWebMessage))

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
        case "reset-defaults":
            _Settings_HandleResetDefaults()
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
        case "addon-action":
            _Settings_HandleAddonAction(msg)
        case "edit-accounts":
            ; Opens another WebView2 window, which cannot finish inside this
            ; message call.
            DeferFromWebMessage(_Settings_OpenAccounts)
    }
}

; Launching → Edit accounts…, shared by both frontends. The Accounts editor is
; AutoLogin's; a build or configuration without it gets no button.
_Settings_AccountsEditorAvailable() {
    return IsAddonEnabled("AutoLogin") && GetFuncByName("_AutoLogin_OpenEditor")
}

_Settings_OpenAccounts(*) {
    if (fn := GetFuncByName("_AutoLogin_OpenEditor"))
        fn()
}

; ── Send full state to JS ────────────────────────────────────

_Settings_SendState() {
    global gSettingsGui, gAddonHooks, gDisabledAddons
    global gGamePath, gGameArgs, gLaunchOnStartup, gLaunchAutoLogin, gMultiClientCount, gMultiClientDelay
    global gPrimaryMonitorOverride, gSecondaryMonitorOverride
    global gPrimaryLaunchLayout, gSecondaryLaunchLayout
    global gMinimapScale, gMinimapOpacity, gMinimapAnchor, gMinimapOffsetX, gMinimapOffsetY
    global gMinimapKeepOpen, OVERLAY_W, OVERLAY_H, gAccentScheme, gShowHoverCoords
    global gMinimapSource, gMinimapFollowGameScale, GAME_LOGICAL_W, GAME_LOGICAL_H
    global APP_VERSION, gVersionCheckEnabled, VERSION_CHECK_URL
    global gUpdateVersion, gUpdateNotes
    global gPrimaryProfileId, gSecondaryProfileId, gInterfaceMode, gDeveloperTools

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

    ; The game picture the minimap preview is drawn against.
    preview := GameContentRect()

    ; Addon enable/disable list
    addonsJson := _Settings_AddonsListToJson()

    ; Tab names, and the grouped sidebar they are drawn as
    tabNames := _Settings_GetTabNames()
    tabNamesJson := "["
    for i, name in tabNames {
        if (i > 1)
            tabNamesJson .= ","
        tabNamesJson .= _JSON_Str(name)
    }
    tabNamesJson .= "]"
    tabGroupsJson := _Settings_TabLayoutToJson()

    ; What the Game & startup tab says under the path.
    gameStatus := _Settings_GameStatus(gGamePath)

    json := '{"type":"settings-state"'
        . ',"appVersion":' _JSON_Str(APP_VERSION)
        . ',"tabNames":' tabNamesJson
        . ',"tabGroups":' tabGroupsJson
        . ',"launcher":{'
            . '"versionCheck":' (gVersionCheckEnabled ? "true" : "false")
            . ',"versionCheckAvailable":' (VERSION_CHECK_URL != "" ? "true" : "false")
            ; So reopening Settings still shows an update this session found.
            . ',"updateVersion":' _JSON_Str(gUpdateVersion)
            . ',"updateNotes":' _JSON_Str(gUpdateNotes)
            . ',"gamePath":' _JSON_Str(gGamePath)
            . ',"gameStatus":' _JSON_Str(gameStatus.text)
            . ',"gameStatusTone":' _JSON_Str(gameStatus.tone)
            . ',"accountsEditorAvailable":' (_Settings_AccountsEditorAvailable() ? "true" : "false")
            . ',"gameArgs":' _JSON_Str(gGameArgs)
            . ',"autoStart":' (IsRunOnStartupEnabled() ? "true" : "false")
            . ',"launchOnStartup":' (gLaunchOnStartup ? "true" : "false")
            . ',"launchAutoLogin":' (gLaunchAutoLogin ? "true" : "false")
            . ',"multiClientCount":' gMultiClientCount
            . ',"multiClientDelay":' gMultiClientDelay
            . ',"monitorChoices":' monArr
            . ',"primaryMonitorChoice":' _Settings_MonitorIndexToChoice(gPrimaryMonitorOverride) - 1
            . ',"secondaryMonitorChoice":' _Settings_MonitorIndexToChoice(gSecondaryMonitorOverride) - 1
            . ',"layoutAvailable":' (launchLayouts.available ? "true" : "false")
            . ',"launchLayoutOptions":' launchLayoutsJson
            . ',"primaryLaunchLayout":' _JSON_Str(primaryLaunchLayout)
            . ',"secondaryLaunchLayout":' _JSON_Str(secondaryLaunchLayout)
            . ',"profiles":' _Settings_ProfilesToJson(launchLayouts)
            . ',"primaryProfile":' _JSON_Str(gPrimaryProfileId)
            . ',"secondaryProfile":' _JSON_Str(gSecondaryProfileId)
            . ',"accountChoices":' _Settings_AccountChoicesToJson()
        . '}'
        . ',"minimap":{'
            . '"source":' _JSON_Str(gMinimapSource)
            . ',"openChordDisplay":' _JSON_Str(GetHotkeyDisplay("openMapsppMinimap"))
            . ',"scale":' gMinimapScale
            . ',"opacity":' gMinimapOpacity
            . ',"anchor":' _JSON_Str(gMinimapAnchor)
            . ',"offsetX":' gMinimapOffsetX
            . ',"offsetY":' gMinimapOffsetY
            . ',"keepOpen":' (gMinimapKeepOpen ? "true" : "false")
            . ',"showHoverCoords":' (gShowHoverCoords ? "true" : "false")
            . ',"followGameScale":' (gMinimapFollowGameScale ? "true" : "false")
            . ',"baseW":' OVERLAY_W
            . ',"baseH":' OVERLAY_H
            ; The preview frames the real game picture when a client is running
            ; (2× under the display module is 1600×1200), else the game's own
            ; unscaled 800×600 — never a made-up desktop size.
            . ',"clientW":' (preview.ok ? preview.w : GAME_LOGICAL_W)
            . ',"clientH":' (preview.ok ? preview.h : GAME_LOGICAL_H)
            . ',"contentScale":' (preview.ok ? preview.scale : 1)
        . '}'
        . ',"appearance":{"accentScheme":' _JSON_Str(gAccentScheme)
            . ',"interfaceMode":' _JSON_Str(gInterfaceMode)
            ; Enhanced can only be offered as working when the runtime is there;
            ; choosing it anyway is still allowed, as the retry it always was.
            . ',"runtimeAvailable":' (WebView2RuntimeAvailable() ? "true" : "false")
            . ',"developerTools":' (gDeveloperTools ? "true" : "false") '}'
        . ',"hotkeys":{"actions":' hotkeysJson '}'
        . ',"addonTabs":' addonTabsJson
        . ',"addons":' addonsJson
    . '}'

    _Settings_PostMessage(json)
}

; The sidebar, as groups of tabs. Core tabs come first in their group; each
; addon's tab joins the group its RegisterAddon map names ("settingsGroup":
; map | multibox | tools, tools when absent) and is sorted by label there, so a
; build's include order does not decide where it lands. Features is last.
;
; Both frontends, Save and Reset read tab names from this one model — the
; native window used to keep its own copy of the list.
_Settings_GetTabLayout() {
    global gAddonHooks
    groups := [
        Map("group", "General", "key", "general", "tabs", [
            Map("name", "Game & startup", "icon", "folder_open"),
            Map("name", "Launching", "icon", "rocket_launch"),
            Map("name", "Appearance", "icon", "visibility"),
            Map("name", "Hotkeys", "icon", "keyboard")]),
        Map("group", "Map", "key", "map", "tabs", [
            Map("name", "Minimap", "icon", "map")]),
        Map("group", "Multi-boxing", "key", "multibox", "tabs", []),
        Map("group", "Tools", "key", "tools", "tabs", [])]
    for _, am in gAddonHooks {
        if !_Settings_AddonHasTab(am)
            continue
        key := am.Has("settingsGroup") ? am["settingsGroup"] : "tools"
        target := groups[groups.Length]
        for g in groups {
            if (g["key"] = key) {
                target := g
                break
            }
        }
        tab := Map("name", _Settings_AddonTabLabel(am), "addon", true,
            "icon", am.Has("settingsIcon") ? am["settingsIcon"] : "tune")
        ; After the core tabs, alphabetical among the addon ones.
        tabs := target["tabs"]
        at := tabs.Length + 1
        for i, existing in tabs {
            if (existing.Has("addon") && StrCompare(existing["name"], tab["name"]) > 0) {
                at := i
                break
            }
        }
        tabs.InsertAt(at, tab)
    }
    out := []
    for g in groups {
        if g["tabs"].Length
            out.Push(g)
    }
    out.Push(Map("group", "", "key", "features", "tabs", [
        Map("name", "Features", "icon", "extension")]))
    return out
}

_Settings_GetTabNames() {
    names := []
    for g in _Settings_GetTabLayout() {
        for t in g["tabs"]
            names.Push(t["name"])
    }
    return names
}

_Settings_TabLayoutToJson() {
    json := "["
    for gi, g in _Settings_GetTabLayout() {
        json .= (gi > 1 ? "," : "") '{"group":' _JSON_Str(g["group"]) ',"tabs":['
        for ti, t in g["tabs"]
            json .= (ti > 1 ? "," : "") '{"name":' _JSON_Str(t["name"]) ',"icon":' _JSON_Str(t["icon"]) '}'
        json .= "]}"
    }
    return json "]"
}

_Settings_AddonTabLabel(am) {
    return am.Has("settingsLabel") ? am["settingsLabel"]
        : (am.Has("name") ? am["name"] : "Addon")
}

; One line under the game path: whether it points at something, and whether
; the client mods are beside it (auto-login and the chat mod need them). Takes
; the path rather than reading gGamePath, so a path just picked with Browse —
; not saved yet — is described too.
_Settings_GameStatus(path) {
    global NATIVE_MODS_CORE_DLL
    if (path = "")
        return {text: "Not set — Maps++ can't launch clients until it is.", tone: "warn"}
    if !FileExist(path)
        return {text: "That file no longer exists. Browse to main.exe again.", tone: "warn"}
    dir := ""
    SplitPath(path, , &dir)
    if FileExist(dir "\" NATIVE_MODS_CORE_DLL)
        return {text: "Found. The osMW client mods are installed in this folder.", tone: "ok"}
    return {text: "Found. The osMW client mods are not in this folder, so auto-login"
        . " and the chat mod are unavailable.", tone: "info"}
}

; Whether an addon gets a Settings tab. One rule for the tab names, both
; frontends' tab bodies, Save and Reset to Defaults, so they cannot drift.
;
; A disabled addon gets none. Before this its tab was still shown and still
; saved, which for Chat Mod meant "turn the addon off" did not stop Maps++
; writing the game's osmw.ini — the opposite of what the Features tab promises.
_Settings_AddonHasTab(am) {
    if !am.Has("OnSettingsWeb")
        return false
    name := am.Has("name") ? am["name"] : ""
    return (name = "") || IsAddonEnabled(name)
}

; Calls the addon's OnSettingsWeb — exactly once per window open, which Chat
; Mod's _ChatMod_RenderedPreset depends on — and remembers what every
; value-bearing field showed, so Save can tell what the user actually changed.
_Settings_AddonFieldsFor(am) {
    global gSettingsRenderedAddonValues
    fields := []
    try fields := am["OnSettingsWeb"]()
    if !IsObject(fields)
        fields := []
    rendered := Map()
    for f in fields {
        if !IsObject(f) || !f.Has("id") || f["id"] = ""
            continue
        fieldType := f.Has("type") ? f["type"] : "text"
        if (fieldType = "info" || fieldType = "button")
            continue
        rendered[f["id"]] := { type: fieldType, value: f.Has("value") ? f["value"] : "" }
    }
    gSettingsRenderedAddonValues[_Settings_AddonKey(am)] := rendered
    return fields
}

; The key both frontends file an addon's fields under in the save message.
_Settings_AddonKey(am) {
    if am.Has("name")
        return am["name"]
    return am.Has("settingsLabel") ? am["settingsLabel"] : "Addon"
}

; The subset of `incoming` that differs from what the window rendered. A field
; with no snapshot counts as changed, so an action that grew the form, or a
; frontend that rendered nothing, can never silently lose a value.
;
; Both frontends send every field on every Save — the page because its draft is
; the whole form, the native panel because it reads every control back. Without
; this, saving the minimap opacity also wrote every Chat Mod key with the values
; Settings had been opened with, undoing any change made since in the game's own
; F12 panel.
_Settings_ChangedAddonValues(addonKey, incoming) {
    global gSettingsRenderedAddonValues
    rendered := gSettingsRenderedAddonValues.Has(addonKey)
        ? gSettingsRenderedAddonValues[addonKey] : Map()
    changed := Map()
    for fieldId, value in incoming {
        if !rendered.Has(fieldId) {
            changed[fieldId] := value
            continue
        }
        r := rendered[fieldId]
        if !_Settings_FieldValueEqual(r.type, r.value, value)
            changed[fieldId] := value
    }
    return changed
}

; Compared the way each frontend reports the type: checkboxes as booleans
; (true/false from JSON, 1/0 from AHK, "1"/"0" from an ini), dropdowns and
; numbers numerically, everything else as text. Text is case-sensitive on
; purpose — a renamed tab "chat" → "Chat" is a change.
_Settings_FieldValueEqual(fieldType, a, b) {
    switch fieldType {
        case "checkbox":
            return _Settings_AsBool(a) = _Settings_AsBool(b)
        case "dropdown", "number":
            if (IsNumber(a) && IsNumber(b))
                return Number(a) = Number(b)
            return Trim(String(a)) == Trim(String(b))
        case "orderedlist":
            return RegExReplace(Trim(String(a)), "\s*,\s*", ",")
                == RegExReplace(Trim(String(b)), "\s*,\s*", ",")
        default:
            return Trim(String(a)) == Trim(String(b))
    }
}

_Settings_AsBool(v) {
    if (v is Integer) || (v is Float)
        return v != 0
    s := StrLower(Trim(String(v)))
    return !(s = "" || s = "0" || s = "false")
}

; Addons that edit a file the game's client mods own (Chat Mod → osmw.ini)
; declare "nativeMods", true. They are handed only the keys the user changed,
; and Reset to Defaults leaves them alone: native_mods.ahk's rule is that Maps++
; writes the mods' settings only when the user asked for that specific change.
_Settings_AddonIsNativeMods(am) {
    return am.Has("nativeMods") && am["nativeMods"]
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
    global gAddonHooks, gSettingsRenderedAddonValues
    gSettingsRenderedAddonValues := Map()
    json := "["
    first := true
    for _, am in gAddonHooks {
        if !_Settings_AddonHasTab(am)
            continue
        if !first
            json .= ","
        first := false

        label := am.Has("settingsLabel") ? am["settingsLabel"]
            : (am.Has("name") ? am["name"] : "Addon")
        addonName := _Settings_AddonKey(am)

        ; Field descriptors, with what they showed recorded for Save.
        fields := _Settings_AddonFieldsFor(am)

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

    ; The value a fresh install would hold. Emitted only when the addon
    ; supplied one, so the page can tell "no default" from a default that is
    ; itself "", 0 or false — all three are real defaults on the Chat Mod tab.
    ; Booleans reach here as AHK integers 1/0 and are compared loosely on the
    ; page, so they need no special case beyond the one "value" already has.
    if f.Has("default") {
        d := f["default"]
        if (d is Integer) || (d is Float)
            json .= ',"default":' d
        else if (d == "true" || d == "false")
            json .= ',"default":' d
        else
            json .= ',"default":' _JSON_Str(String(d))
    }

    ; Explanatory line under a button field
    if f.Has("hint")
        json .= ',"hint":' _JSON_Str(String(f["hint"]))

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

; The Features tab: every addon in this build, with the group and one-line
; description its RegisterAddon map gives. Developer-only addons ("devOnly")
; are listed only while developer tools are on, so the list a player sees is
; the list of things meant for players.
_Settings_AddonsListToJson() {
    json := "["
    first := true
    for entry in _Settings_FeatureList() {
        if !first
            json .= ","
        first := false
        json .= '{"name":' _JSON_Str(entry["name"])
            . ',"label":' _JSON_Str(entry["label"])
            . ',"description":' _JSON_Str(entry["description"])
            . ',"group":' _JSON_Str(entry["group"])
            . ',"enabled":' (entry["enabled"] ? "true" : "false") '}'
    }
    json .= "]"
    return json
}

; Shared by both Settings frontends and the setup guide.
_Settings_FeatureList() {
    global gAddonHooks, gDisabledAddons, gDeveloperTools
    out := []
    for _, am in gAddonHooks {
        nm := am.Has("name") ? am["name"] : ""
        if (nm = "")
            continue
        if (am.Has("devOnly") && am["devOnly"] && !gDeveloperTools)
            continue
        out.Push(Map(
            "name", nm,
            "label", am.Has("featureLabel") ? am["featureLabel"]
                : (am.Has("settingsLabel") ? am["settingsLabel"] : nm),
            "description", am.Has("description") ? am["description"] : "",
            "group", am.Has("settingsGroup") ? am["settingsGroup"] : "tools",
            ; Only for playing several characters at once: the setup guide
            ; leaves these off for someone who plays one.
            "multibox", (am.Has("multibox") && am["multibox"]) ? true : false,
            "enabled", !(gDisabledAddons.Has(nm) && gDisabledAddons[nm])))
    }
    return out
}

; Launch profiles for the page. Monitors go out as the same 0-based choice index
; the existing display dropdowns use, so the page's two select helpers work
; unchanged and the conversion back stays in one place on save.
_Settings_ProfilesToJson(launchLayouts) {
    json := "["
    first := true
    for _, id in LaunchProfileIds() {
        prof := LaunchProfileById(id)
        if !IsObject(prof)
            continue
        if !first
            json .= ","
        first := false
        json .= '{"id":' _JSON_Str(id)
            . ',"name":' _JSON_Str(prof["name"])
            . ',"accounts":' _JSON_Str(prof["accounts"])
            . ',"count":' prof["count"]
            . ',"monitorChoice":' (_Settings_MonitorIndexToChoice(prof["monitor"]) - 1)
            . ',"layout":' _JSON_Str(_Settings_ValidateLaunchLayout(prof["layout"], launchLayouts))
            . '}'
    }
    return json "]"
}

; What a profile's Accounts dropdown offers, shared by both frontends. Only
; "next free" when the AutoLogin addon is off or the roster is empty — a profile
; can still pin a display, a count and a layout without naming accounts.
_Settings_AccountChoices() {
    out := [Map("value", "", "label", "Next free roster row")]
    choicesFn := GetFuncByName("_AutoLogin_LaunchChoices")
    if !(choicesFn && IsAddonEnabled("AutoLogin"))
        return out
    choices := ""
    try choices := choicesFn()
    if !IsObject(choices)
        return out
    ; fixedCount: a group launches exactly its rows and "only" exactly one, so
    ; the page disables the profile's Clients field for those and says why.
    for grp in choices["groups"]
        out.Push(Map("value", grp["spec"], "fixedCount", grp["count"],
            "label", "Group: " grp["name"] " (" grp["count"] ")"))
    for row in choices["rows"]
        out.Push(Map("value", row["startSpec"],
            "label", "Start at: " row["account"] " (row " row["index"] ")"))
    for row in choices["rows"]
        out.Push(Map("value", row["onlySpec"], "fixedCount", 1,
            "label", "Only: " row["account"] " (" row["character"] ")"))
    return out
}

_Settings_AccountChoicesToJson() {
    json := "["
    first := true
    for choice in _Settings_AccountChoices() {
        if !first
            json .= ","
        first := false
        json .= '{"value":' _JSON_Str(choice["value"])
            . ',"label":' _JSON_Str(choice["label"])
            . (choice.Has("fixedCount") ? ',"fixedCount":' choice["fixedCount"] : "") '}'
    }
    return json "]"
}

; Rebuilds gLaunchProfiles from the page. Returns "" on success or a message to
; show the user. Nothing is committed until every row has validated, so a bad
; entry cannot leave half a profile set behind.
_Settings_ApplyProfiles(launcher, launchLayouts) {
    global gLaunchProfiles, gPrimaryProfileId, gSecondaryProfileId, LAUNCH_LAYOUT_DEFAULT
    incoming := launcher["profiles"]
    if (!IsObject(incoming) || Type(incoming) != "Array")
        return "Launch profiles were unreadable."
    if (incoming.Length = 0)
        return "Keep at least one launch profile — Launch (Primary) has to run something."

    built := Map()
    for prof in incoming {
        if !IsObject(prof)
            return "A launch profile was unreadable."
        id := Trim(String(prof.Has("id") ? prof["id"] : ""))
        if !(IsInteger(id) && Integer(id) >= 1)
            return "A launch profile has an invalid id."
        if built.Has(id)
            return "Two launch profiles share an id."
        name := Trim(String(prof.Has("name") ? prof["name"] : ""))
        if (name = "")
            name := "Profile " id
        pcount := prof.Has("count") ? prof["count"] : 0
        if (!IsInteger(pcount) || Integer(pcount) < 0)
            return "A launch profile's client count must be 0 (auto) or a positive whole number."
        choice := prof.Has("monitorChoice") ? prof["monitorChoice"] : 0
        if !IsInteger(choice)
            choice := 0
        ; With Window Layout absent (the Battle variant) the page renders no
        ; layout control and sends nothing meaningful, so keep whatever is
        ; stored rather than letting a save from that build wipe the choice a
        ; Full build made. gLaunchProfiles is still the old map here.
        layout := prof.Has("layout") ? prof["layout"] : LAUNCH_LAYOUT_DEFAULT
        if !launchLayouts.available {
            stored := LaunchProfileById(id)
            layout := IsObject(stored) ? stored["layout"] : LAUNCH_LAYOUT_DEFAULT
        }
        built[id] := Map(
            "id", id,
            "name", _Settings_ClipText(name, 40),
            "accounts", Trim(String(prof.Has("accounts") ? prof["accounts"] : "")),
            "count", Integer(pcount),
            ; Back to a 1-based monitor index the same way the flat keys convert.
            "monitor", _Settings_ChoiceToMonitorIndex(Integer(choice) + 1),
            "layout", _Settings_ValidateLaunchLayout(layout, launchLayouts))
    }

    gLaunchProfiles := built
    primary := Trim(String(launcher.Has("primaryProfile") ? launcher["primaryProfile"] : ""))
    ids := LaunchProfileIds()
    gPrimaryProfileId := built.Has(primary) ? primary : ids[1]
    ; Secondary may run nothing ("" from the page's None): the tray then has
    ; no second launch row and its hotkey says it is not set up. A page that
    ; did not send the key at all keeps what is bound.
    if launcher.Has("secondaryProfile") {
        secondary := Trim(String(launcher["secondaryProfile"]))
        if (secondary = "")
            gSecondaryProfileId := ""
        else
            gSecondaryProfileId := built.Has(secondary) ? secondary : ids[ids.Length >= 2 ? 2 : 1]
    } else if (gSecondaryProfileId != "" && !built.Has(gSecondaryProfileId)) {
        gSecondaryProfileId := ids[ids.Length >= 2 ? 2 : 1]
    }
    ; SaveLauncherConfig writes the profiles and re-syncs the mirrored globals,
    ; so nothing here needs to touch gPrimaryMonitorOverride and friends.
    return ""
}

_Settings_ClipText(text, maxChars) {
    s := Trim(String(text))
    return (StrLen(s) <= maxChars) ? s : SubStr(s, 1, maxChars)
}

; ── Handle save ──────────────────────────────────────────────

_Settings_HandleSave(msg) {
    global gGamePath, gGameArgs, gLaunchOnStartup, gLaunchAutoLogin, gMultiClientCount, gMultiClientDelay
    global gPrimaryMonitorOverride, gSecondaryMonitorOverride
    global gPrimaryLaunchLayout, gSecondaryLaunchLayout
    global gMinimapScale, gMinimapOpacity, gMinimapAnchor, gMinimapOffsetX, gMinimapOffsetY
    global gMinimapKeepOpen, MINIMAP_ANCHORS, gAccentScheme, gShowHoverCoords
    global gAddonHooks, gHotkeyActions, gVersionCheckEnabled, gMinimapSource
    global gMinimapFollowGameScale, gInterfaceMode, gDeveloperTools, CONFIG_INI

    ; ── Launcher ──
    ; Every field is optional: a frontend that no longer renders one (Clients
    ; per launch left the page when profiles took over the count) keeps the
    ; stored value instead of throwing on the missing key.
    launcher := msg["launcher"]
    if launcher.Has("gamePath")
        gGamePath := launcher["gamePath"]
    if launcher.Has("gameArgs")
        gGameArgs := Trim(launcher["gameArgs"])
    if launcher.Has("launchOnStartup")
        gLaunchOnStartup := launcher["launchOnStartup"] ? true : false
    if launcher.Has("launchAutoLogin")
        gLaunchAutoLogin := launcher["launchAutoLogin"] ? true : false
    cnt := launcher.Has("multiClientCount") ? launcher["multiClientCount"] : gMultiClientCount
    dly := launcher.Has("multiClientDelay") ? launcher["multiClientDelay"] : gMultiClientDelay
    if (!IsInteger(cnt) || Integer(cnt) < 1) {
        _Settings_SendSaveResult(false, "Clients per launch must be a whole number of 1 or more.")
        return false
    }
    if (!IsInteger(dly) || Integer(dly) < 0) {
        _Settings_SendSaveResult(false, "Delay between clients must be 0 or a positive number of milliseconds.")
        return false
    }
    gMultiClientCount := Integer(cnt)
    gMultiClientDelay := Integer(dly)
    launchLayouts := _Settings_LaunchLayoutOptions()
    ; Display and layout arrive inside the profiles now. An older page that
    ; still sends the flat keys is honoured rather than ignored, so a stale
    ; cached page cannot silently discard the user's display choice.
    if launcher.Has("primaryMonitor")
        gPrimaryMonitorOverride := _Settings_ChoiceToMonitorIndex(Integer(launcher["primaryMonitor"]) + 1)
    if launcher.Has("secondaryMonitor")
        gSecondaryMonitorOverride := _Settings_ChoiceToMonitorIndex(Integer(launcher["secondaryMonitor"]) + 1)
    if launchLayouts.available {
        if launcher.Has("primaryLaunchLayout")
            gPrimaryLaunchLayout := _Settings_ValidateLaunchLayout(launcher["primaryLaunchLayout"], launchLayouts)
        if launcher.Has("secondaryLaunchLayout")
            gSecondaryLaunchLayout := _Settings_ValidateLaunchLayout(launcher["secondaryLaunchLayout"], launchLayouts)
    }
    if launcher.Has("profiles") {
        applied := _Settings_ApplyProfiles(launcher, launchLayouts)
        if (applied != "") {
            _Settings_SendSaveResult(false, applied)
            return false
        }
    }
    ; Absent key = an older page that doesn't render the toggle; keep the
    ; current value rather than silently switching the check off.
    if launcher.Has("versionCheck")
        gVersionCheckEnabled := launcher["versionCheck"] ? true : false
    SaveLauncherConfig()
    if launcher.Has("autoStart")
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
    if (IsObject(appearance) && appearance.Has("developerTools")) {
        gDeveloperTools := appearance["developerTools"] ? true : false
        IniWrite(gDeveloperTools ? "1" : "0", CONFIG_INI, "UI", "DeveloperTools")
    }
    ; Applied after Save has finished and the window is gone, because changing
    ; it restarts Maps++. Choosing Enhanced again while the runtime is missing
    ; is the retry the tray's Interface menu used to offer.
    nextInterface := ""
    if (IsObject(appearance) && appearance.Has("interfaceMode")) {
        mode := (StrLower(appearance["interfaceMode"]) = "native") ? "native" : "webview"
        if (mode != gInterfaceMode || (mode = "webview" && !WebView2RuntimeAvailable()))
            nextInterface := mode
    }

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
    if mm.Has("source")
        gMinimapSource := NormalizeMinimapSource(mm["source"])
    previousFollow := gMinimapFollowGameScale
    if mm.Has("followGameScale")
        gMinimapFollowGameScale := mm["followGameScale"] ? true : false
    SaveMinimapConfig()
    if (gMinimapScale != previousScale || gAccentScheme != previousAccentScheme
        || gMinimapFollowGameScale != previousFollow) {
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
        ; Only chords that changed in this Save are checked against the keys
        ; the client mods use in game, so an overlap the player already
        ; accepted is not raised again every time anything is saved. Warned,
        ; never refused — see NativeModsReservedKeys.
        modKeys := 0
        overlaps := ""
        for entry in hotkeys {
            action := gHotkeyActions[entry["id"]]
            nextChord := NormalizeHotkeyChord(entry["chord"])
            if (nextChord != "" && CanonicalHotkeyChord(nextChord) != CanonicalHotkeyChord(action["chord"])) {
                if !IsObject(modKeys)
                    modKeys := NativeModsReservedKeys()
                if ((owner := NativeModsKeyConflict(nextChord, modKeys)) != "")
                    overlaps .= "`n" FormatHotkeyDisplay(nextChord) " ("
                        . (action.Has("label") ? action["label"] : entry["id"]) ") is also " owner
            }
            action["chord"] := nextChord
        }
        SaveHotkeyOverrides()
        if (overlaps != "") {
            LogWarn("Hotkeys", "Saved chords that the client mods also use in game:" overlaps)
            TrayTip("Saved. These keys also do something in game:" overlaps, "Hotkeys", "Icon!")
        }
    }

    ; ── Addon enable/disable ──
    ; An addon switched on has had no OnInit this session and one switched off
    ; keeps its timers running, so either way the change only fully takes after
    ; a restart — offered once the window has closed.
    addonsChanged := false
    addonToggles := msg.Has("addons") ? msg["addons"] : Map()
    if IsObject(addonToggles) {
        for nm, enabled in addonToggles {
            want := enabled ? true : false
            if (IsAddonEnabled(nm) != want)
                addonsChanged := true
            SetAddonEnabled(nm, want)
        }
    }

    ; ── Addon settings (dynamic) ──
    ; Only addons whose fields the user changed are saved at all. An addon that
    ; edits the game's client-mod files ("nativeMods") is handed just those
    ; changed keys; every other addon gets its full field map as before, since
    ; most of them read a key that is absent as "off". The enable/disable
    ; toggles above have already been applied, so an addon switched off in this
    ; same Save is skipped along with its unsaved edits.
    addonSettings := msg.Has("addonSettings") ? msg["addonSettings"] : Map()
    if IsObject(addonSettings) {
        for _, am in gAddonHooks {
            if (!am.Has("OnSettingsWebSave") || !_Settings_AddonHasTab(am))
                continue
            addonKey := _Settings_AddonKey(am)
            if !addonSettings.Has(addonKey) || !IsObject(addonSettings[addonKey])
                continue
            incoming := addonSettings[addonKey]
            changed := _Settings_ChangedAddonValues(addonKey, incoming)
            if (changed.Count = 0)
                continue
            payload := _Settings_AddonIsNativeMods(am) ? changed : incoming
            try {
                am["OnSettingsWebSave"](payload)
            } catch as err {
                LogWarn("Settings", "Saving " addonKey " settings failed: " err.Message)
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
    ; Both run after the close, outside this (possibly WebView2) message call:
    ; one restarts, the other asks first.
    if (nextInterface != "")
        SetTimer(() => SetInterfaceMode(nextInterface), -600)
    else if addonsChanged
        SetTimer(_Settings_OfferRestart, -600)
    return true
}

_Settings_OfferRestart() {
    if ConfirmAction("Restart Maps++ now to finish turning features on and off?"
        . "`n`nUntil it restarts, a feature you turned on is not running yet, and one"
        . " you turned off may still be.",
        "Maps++ — Settings",
        Map("severity", "ask", "okLabel", "Restart now", "cancelLabel", "Later"))
        Reload()
}

; ── Reset to Defaults ────────────────────────────────────────
;
; Does exactly what manually setting every field back to its default and
; pressing Save would do — reusing the existing save/apply functions (and
; each addon's own OnSettingsWebSave) instead of a parallel code path.
;
; Deliberately NOT touched: the Windows "run on login" registry entry
; (autoStart appears on the Game & startup tab but registry is intentionally left
; alone) and the WebView-vs-Native interface mode. A reset scoped to settings
; defaults shouldn't reach into either.
ResetAllSettingsToDefaults() {
    global gGameArgs, gLaunchOnStartup, gLaunchAutoLogin, gMultiClientCount, gMultiClientDelay
    global gPrimaryMonitorOverride, gSecondaryMonitorOverride
    global gPrimaryLaunchLayout, gSecondaryLaunchLayout, gVersionCheckEnabled
    global gMinimapScale, gMinimapOpacity, gMinimapAnchor, gMinimapOffsetX, gMinimapOffsetY
    global gMinimapKeepOpen, gShowHoverCoords, gMinimapSource, gMinimapFollowGameScale
    global gAddonHooks, gHotkeyActions, LAUNCH_LAYOUT_DEFAULT
    global gLaunchProfiles, gPrimaryProfileId, gSecondaryProfileId
    global gDeveloperTools, CONFIG_INI

    ; ── Launcher ──
    gGameArgs := DefaultRead("Launcher", "GameArgs", "")
    gLaunchOnStartup := (DefaultRead("Launcher", "LaunchOnStartup", "0") = "1")
    gLaunchAutoLogin := (DefaultRead("Launcher", "LaunchAutoLogin", "1") != "0")
    gMultiClientCount := Integer(DefaultRead("Launcher", "MultiClientCount", "1"))
    gMultiClientDelay := Integer(DefaultRead("Launcher", "MultiClientDelay", "0"))
    gPrimaryMonitorOverride := Integer(DefaultRead("Launcher", "PrimaryMonitor", "0"))
    gSecondaryMonitorOverride := Integer(DefaultRead("Launcher", "SecondaryMonitor", "0"))
    gPrimaryLaunchLayout := DefaultRead("Launcher", "PrimaryLaunchLayout", LAUNCH_LAYOUT_DEFAULT)
    gSecondaryLaunchLayout := DefaultRead("Launcher", "SecondaryLaunchLayout", LAUNCH_LAYOUT_DEFAULT)
    gVersionCheckEnabled := (DefaultRead("Launcher", "VersionCheck", "1") != "0")
    ; Profiles own display and layout now, so resetting the four mirrored
    ; globals is not enough: SaveLauncherConfig re-syncs them *from* the
    ; profiles, which would put the old values straight back. Rebuild the two
    ; seeded profiles instead, and SaveLaunchProfiles drops any others' sections.
    ; Counts are explicit: Settings no longer shows the global fallback that a
    ; count of 0 used to defer to.
    gLaunchProfiles := Map(
        "1", Map("id", "1", "name", "Primary", "accounts", "", "count", gMultiClientCount,
            "monitor", gPrimaryMonitorOverride, "layout", gPrimaryLaunchLayout),
        "2", Map("id", "2", "name", "Secondary", "accounts", "", "count", gMultiClientCount,
            "monitor", gSecondaryMonitorOverride, "layout", gSecondaryLaunchLayout))
    gPrimaryProfileId := "1"
    gSecondaryProfileId := "2"
    SaveLauncherConfig()

    ; ── Minimap ──
    gMinimapScale := Integer(DefaultRead("Minimap", "Scale", "100"))
    gMinimapOpacity := Integer(DefaultRead("Minimap", "Opacity", "100"))
    gMinimapAnchor := DefaultRead("Minimap", "Anchor", "Center")
    gMinimapOffsetX := Integer(DefaultRead("Minimap", "OffsetX", "0"))
    gMinimapOffsetY := Integer(DefaultRead("Minimap", "OffsetY", "0"))
    gMinimapKeepOpen := (DefaultRead("Minimap", "KeepOpenOnFocusLoss", "0") = "1")
    gShowHoverCoords := (DefaultRead("Minimap", "ShowHoverCoords", "1") != "0")
    gMinimapSource := NormalizeMinimapSource(DefaultRead("Minimap", "Source", "game"))
    gMinimapFollowGameScale := (DefaultRead("Minimap", "FollowGameScale", "1") != "0")
    SaveMinimapConfig()

    ; ── Appearance ── (accent before overlay rebuild, same order as save)
    SetAccentScheme(DefaultRead("UI", "AccentScheme", "amber"))
    RebuildOverlayGui()
    gDeveloperTools := (DefaultRead("UI", "DeveloperTools", "0") = "1")
    IniWrite(gDeveloperTools ? "1" : "0", CONFIG_INI, "UI", "DeveloperTools")

    ; ── Hotkeys ── (mirrors SaveHotkeyOverrides' own persist:false skip)
    for id, action in gHotkeyActions {
        if action.Has("persist") && !action["persist"]
            continue
        action["chord"] := action["default"]
    }
    SaveHotkeyOverrides()

    ; Which addons were live before the reset flips them. One that was off has
    ; had no OnInit this session, so its OnSettingsWeb cannot be trusted to
    ; describe its own fields; it keeps its settings and is reset only by name.
    wasEnabled := Map()
    for _, am in gAddonHooks {
        if _Settings_AddonHasTab(am)
            wasEnabled[_Settings_AddonKey(am)] := true
    }

    ; ── Addon enable/disable ──
    for _, am in gAddonHooks {
        name := am.Has("name") ? am["name"] : ""
        if (name = "")
            continue
        SetAddonEnabled(name, DefaultRead("Addons", name, "1") = "1")
    }

    ; ── Per-addon settings ──
    ; OnSettingsWeb is called immediately before OnSettingsWebSave for the
    ; same addon (not batched) — Window Layout's OnSettingsWebSave reads back
    ; a global (_WindowLayout_SettingsNames) that only OnSettingsWeb sets, so
    ; the two must run as a pair in this order.
    ;
    ; Addons that edit the game's client-mod files are skipped: those settings
    ; belong to the mods, are shared with the in-game F12 panel, and "reset
    ; every Maps++ setting" is not a request to rewrite them.
    for _, am in gAddonHooks {
        if (!am.Has("OnSettingsWeb") || !am.Has("OnSettingsWebSave"))
            continue
        if _Settings_AddonIsNativeMods(am) || !wasEnabled.Has(_Settings_AddonKey(am))
            continue
        fields := []
        try fields := am["OnSettingsWeb"]()
        defaults := Map()
        for f in fields {
            if (IsObject(f) && f.Has("id") && f["id"] != "" && f.Has("default"))
                defaults[f["id"]] := f["default"]
        }
        if (defaults.Count > 0)
            try am["OnSettingsWebSave"](defaults)
    }

    ApplyAllHotkeys()
    RebuildTrayMenu()
}

_Settings_SendSaveResult(ok, error := "") {
    global gSettingsGui
    ; The native panel has nowhere in itself to put a save error, so it gets a
    ; dialog; the WebView panel shows it inline and falls through below.
    if (!ok && error != "" && IsObject(gSettingsGui) && Type(gSettingsGui) = "Gui") {
        try gSettingsGui.Opt("+OwnDialogs")
        ShowMessage(error, "Maps++ — Settings",
            Map("severity", "warn", "owner", gSettingsGui.Hwnd))
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

; Shared by both frontends (the WebView button posts "reset-defaults"; the
; Native button's Click handler calls this directly) so the confirmation and
; behavior can never drift between them.
;
; WebView posts arrive inside WebMessageReceived. ConfirmAction's dialog is
; another WebView whose Yes/No also arrives via WebMessageReceived — and that
; nested COM callback does not run while the settings handler is still on the
; stack, even across Sleep. The confirm wait then never ends, and the settings
; owner stays EnableWindow(false). Leave the settings callback immediately and
; run confirm + reset on a timer (native Click is already outside that path,
; so the timer is a no-op delay there).
_Settings_HandleResetDefaults() {
    SetTimer(_Settings_ResetDefaultsBegin, -1)
}

_Settings_ResetDefaultsBegin() {
    global gSettingsGui
    if !(IsObject(gSettingsGui) && gSettingsGui.Hwnd)
        return
    owner := gSettingsGui.Hwnd
    if !ConfirmAction(
        "Reset every Maps++ setting to its default? This includes addon "
        . "on/off state and all keyboard shortcuts.`n`n"
        . "The Chat mod tab is left alone: those settings belong to the game's "
        . "client mods (osmw.ini), where the in-game F12 panel edits them too.",
        "Maps++ — Settings",
        Map("severity", "danger", "danger", true, "owner", owner))
    {
        return
    }
    ResetAllSettingsToDefaults()
    _Settings_Close()
    ShowSettingsWindow()
}

; ── Addon button fields ────────────────────────────────────────────
;
; A "button" field carries no value; clicking it asks the addon to *do*
; something — open its own editor, reset something it owns. The work runs
; OUTSIDE this callback for the reason documented above
; _Settings_HandleResetDefaults: opening a WebView2 window or raising a
; blocking dialog from inside WebMessageReceived can never complete, because
; the STA will not deliver the new window's messages while this COM call is
; still on the stack. The native frontend's Click is already outside that
; path, so it calls _Settings_RunAddonAction directly.
_Settings_HandleAddonAction(msg) {
    addonName := msg.Has("addon") ? msg["addon"] : ""
    actionId := msg.Has("action") ? msg["action"] : ""
    if (addonName = "" || actionId = "")
        return
    DeferFromWebMessage(_Settings_RunAddonAction, addonName, actionId)
}

_Settings_RunAddonAction(addonName, actionId) {
    global gAddonHooks
    for _, am in gAddonHooks {
        if (!am.Has("name") || am["name"] != addonName)
            continue
        if !am.Has("OnSettingsAction")
            return
        try {
            updates := am["OnSettingsAction"](actionId)
            if (IsObject(updates) && updates.Count > 0)
                _Settings_PushAddonFieldValues(addonName, updates)
        } catch as err {
            LogWarn("Settings", "Addon action " addonName "/" actionId
                . " failed: " err.Message)
        }
        return
    }
}

; An action may change fields the panel is already showing — the tab editor
; hands back a new Tabs string. Push it into the live draft rather than writing
; it to disk from inside the action, so Cancel still means cancel and a later
; Save cannot overwrite the editor's work with the value the panel rendered
; with. One function for both frontends, so the two cannot drift.
_Settings_PushAddonFieldValues(addonName, updates) {
    global gSettingsNativeFieldSetter
    if (gSettingsNativeFieldSetter != 0) {
        for fieldId, value in updates
            try gSettingsNativeFieldSetter.Call(addonName, fieldId, value)
        return
    }
    json := '{"type":"addon-field-update","addonName":' _JSON_Str(addonName)
        . ',"values":{'
    first := true
    for fieldId, value in updates {
        if !first
            json .= ","
        first := false
        json .= _JSON_Str(String(fieldId)) ":"
        if (value is Integer) || (value is Float)
            json .= value
        else
            json .= _JSON_Str(String(value))
    }
    json .= "}}"
    _Settings_PostMessage(json)
}

_Settings_HandleBrowse() {
    global PROCESS_EXE
    selected := FileSelect(1, A_ScriptDir, "Locate " PROCESS_EXE " (game executable)", "Executables (*.exe)")
    if (selected = "")
        return
    status := _Settings_GameStatus(selected)
    _Settings_PostMessage('{"type":"browse-result","path":' _JSON_Str(selected)
        . ',"status":' _JSON_Str(status.text) ',"tone":' _JSON_Str(status.tone) '}')
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
    global gSettingsGui, gSettingsHotkeyRows, gSettingsNativeFieldSetter
    global gSettingsRenderedAddonValues
    CancelHotkeyCapture()
    gSettingsHotkeyRows := 0
    ; The controls it closes over are about to be destroyed.
    gSettingsNativeFieldSetter := 0
    gSettingsRenderedAddonValues := Map()
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

    ; Built-in presets are stored by key ("Grid2x2") but shown by name.
    labelFn := GetFuncByName("_WindowLayout_PresetLabel")
    defaultName := defaultFn()
    items := [
        { value: "", label: "None — center clients only" },
        { value: LAUNCH_LAYOUT_DEFAULT, label: "Default — " (labelFn ? labelFn(defaultName) : defaultName) }
    ]
    for name in namesFn()
        items.Push({ value: name, label: labelFn ? labelFn(name) : name })
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
;   Map("type","button", "id","editTabs", "label","Edit tabs…", "hint","…")
;
; OnSettingsWebSave receives a Map of { fieldId: value } for the addon's fields.
; Boolean fields arrive as true/false. Dropdown fields arrive as 0-based index.
; Orderedlist fields arrive as a comma-separated id string — from both the web
; and the native frontend, so an addon has one save path to write.
;
; It is called only when at least one of the addon's fields differs from what
; the window rendered, and never for an addon that is disabled (a disabled
; addon gets no tab at all). By default it still receives every field, because
; most addons treat an absent key as "off". An addon that edits a file the
; game's client mods own must instead declare
;     "nativeMods",         true
; and then receives ONLY the changed keys, and is skipped by Reset to Defaults.
; That is native_mods.ahk's rule 1: Maps++ writes the mods' settings only when
; the user asked for that particular change.
;
; A "button" field carries no value and never appears in that Map. Clicking it
; calls the addon's optional OnSettingsAction(actionId) hook with the field's
; id, from outside the WebView message callback — so it is the one field type
; that may open a window or raise a blocking dialog. Register it alongside the
; others:
;     "OnSettingsAction",   MyAddon_OnSettingsAction   ; fn(actionId)
; A button acts immediately on what it owns; it does not wait for Save, so say
; so in its label or an adjacent info field.
;
; Any value-bearing field may also carry "default", the value the field should
; hold in a fresh install. Both frontends then offer a per-field revert control
; that appears only while the current value differs from it. The global Reset to
; Defaults harvests the same key. A field with no "default" simply gets no
; revert control, so this is opt-in per addon. ("nativeMods" addons keep their
; per-field revert but are never touched by the global reset.)
