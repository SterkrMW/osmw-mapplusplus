#Requires AutoHotkey v2.0

; Better Hotkeys keeps character-specific shortcuts outside the game client's
; own single-session setting. One transient registry action is contributed for
; each unique chord; the handler then dispatches by the active character name.

global _BH_PROCESS_ACCESS := 0x0008 | 0x0010 | 0x0020 | 0x0400
global _BH_ACTION_SKILL := 1
global _BH_ACTION_ID_PREFIX := "betterHotkeys."
global _BH_INI := ""
global _BH_Profiles := []
global _BH_Loaded := false
global _BH_LastNotice := Map()

global _BH_CLASS_NAMES := [
    "Male Human", "Female Human", "Male Centaur", "Female Centaur",
    "Male Mage", "Female Mage", "Male Borg", "Female Borg"
]

; Server/client enum order. Keep this exhaustive list rather than generating
; labels from arithmetic: it is the compatibility contract supplied by osMW.
global _BH_SKILL_NAMES := [
    "Frailty I", "Frailty II", "Frailty III", "Frailty IV",
    "Poison I", "Poison II", "Poison III", "Poison IV",
    "Chaos I", "Chaos II", "Chaos III", "Chaos IV",
    "Hypnotize I", "Hypnotize II", "Hypnotize III", "Hypnotize IV",
    "Stun I", "Stun II", "Stun III", "Stun IV",
    "Purge Chaos I", "Purge Chaos II", "Purge Chaos III", "Purge Chaos IV",
    "Un Stun I", "Un Stun II", "Un Stun III", "Un Stun IV",
    "Multi Shot I", "Multi Shot II", "Multi Shot III", "Multi Shot IV",
    "Blizzard I", "Blizzard II", "Blizzard III", "Blizzard IV",
    "Speed I", "Speed II", "Speed III", "Speed IV",
    "Heal Other I", "Heal Other II", "Heal Other III", "Heal Other IV",
    "Fire I", "Fire II", "Fire III", "Fire IV",
    "Ice I", "Ice II", "Ice III", "Ice IV",
    "Evil I", "Evil II", "Evil III", "Evil IV",
    "Flash I", "Flash II", "Flash III", "Flash IV",
    "Death I", "Death II", "Death III", "Death IV",
    "Enhance I", "Enhance II", "Enhance III", "Enhance IV",
    "Protect I", "Protect II", "Protect III", "Protect IV",
    "Drain I", "Drain II", "Drain III", "Drain IV",
    "Reflect I", "Reflect II", "Reflect III", "Reflect IV",
    "Repel I", "Repel II", "Repel III", "Repel IV"
]

global _BH_CLASS_SKILLS := Map(
    0, [0, 1, 2, 3, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19],
    1, [4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19],
    2, [20, 21, 22, 23, 28, 29, 30, 31, 36, 37, 38, 39, 40, 41, 42, 43],
    3, [24, 25, 26, 27, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43],
    4, [44, 45, 46, 47, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63],
    5, [48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63],
    6, [64, 65, 66, 67, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83],
    7, [68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83]
)

; Pet offsets and actor fields are registered now so adding pet skill discovery
; later does not require a new persistence or execution model. The first editor
; intentionally authors player bindings only.
;
; The two *action* fields are the same addresses battle_send uses, so they share
; its names rather than carrying a second alias each. Two names for one address
; meant the scanner resolved it twice and the two addons could end up writing to
; different places after a game patch. RegisterAddonOffset ignores a repeat
; registration, so both files declaring them is what keeps every variant working
; — battle.txt and full.txt ship both addons, lite.txt ships neither.
RegisterAddonOffset("BattleAction", 0x30B5F8)
RegisterAddonOffset("PetBattleAction", 0x3029C4)
RegisterAddonOffset("BetterHotkeysPlayerSkill", 0x30B604)
RegisterAddonOffset("BetterHotkeysPetSkill", 0x3029D0)

RegisterAddon(Map(
    "name", "BetterHotkeys",
    "settingsLabel", "Better Hotkeys",
    "OnInit", _BH_OnInit,
    "OnBeforeApplyHotkeys", _BH_OnBeforeApplyHotkeys,
    "OnTrayMenu", _BH_OnTrayMenu,
    "OnSnapshot", _BH_OnSnapshot
))

; ── Lifecycle and persistence ───────────────────────────────────

_BH_OnInit() {
    _BH_LoadProfiles()
}

_BH_OnBeforeApplyHotkeys() {
    _BH_LoadProfiles()
    _BH_SyncRegistryActions()
}

_BH_OnTrayMenu(trayGroups) {
    trayGroups["quickActions"].Add("Better Hotkeys…", (*) => _BH_OpenEditor())
}

_BH_Path() {
    global _BH_INI
    if (_BH_INI = "")
        _BH_INI := ResolveWritableIniPath("better_hotkeys.ini")
    return _BH_INI
}

_BH_LoadProfiles(force := false) {
    global _BH_Profiles, _BH_Loaded
    if (_BH_Loaded && !force)
        return _BH_Profiles

    profiles := []
    path := _BH_Path()
    count := 0
    if FileExist(path) {
        rawCount := Trim(IniRead(path, "Meta", "Count", "0"))
        if IsInteger(rawCount)
            count := Min(Max(Integer(rawCount), 0), 64)
    }

    Loop count {
        section := "Profile." A_Index
        name := _BH_SanitizeName(IniRead(path, section, "Name", ""))
        rawClass := Trim(IniRead(path, section, "Class", "-1"))
        if (name = "" || !IsInteger(rawClass))
            continue
        classId := Integer(rawClass)
        if (classId < 0 || classId > 7)
            continue

        bindings := []
        rawBindingCount := Trim(IniRead(path, section, "BindingCount", "0"))
        bindingCount := IsInteger(rawBindingCount)
            ? Min(Max(Integer(rawBindingCount), 0), 64) : 0
        seenChords := Map()
        Loop bindingCount {
            prefix := "Binding" A_Index
            chord := NormalizeHotkeyChord(IniRead(path, section, prefix "Chord", ""))
            actor := StrLower(Trim(IniRead(path, section, prefix "Actor", "player")))
            rawSkill := Trim(IniRead(path, section, prefix "Skill", "-1"))
            if (chord = "" || !IsHotkeyChordValid(chord) || !IsInteger(rawSkill))
                continue
            skill := Integer(rawSkill)
            ; Pet bindings are reserved for the future discovery flow and are
            ; not loaded as active bindings in this first release.
            if (actor != "player" || !_BH_IsSkillForClass(classId, skill))
                continue
            chordKey := StrLower(chord)
            if seenChords.Has(chordKey)
                continue
            seenChords[chordKey] := true
            bindings.Push(Map("chord", chord, "actor", actor, "skill", skill))
        }
        profiles.Push(Map("name", name, "classId", classId, "bindings", bindings))
    }

    ; Conflict checks depend on the complete central registry and its persisted
    ; overrides, which are loaded after addon OnInit. Keep every structurally
    ; valid profile here; _BH_SyncRegistryActions safely skips a conflicting
    ; chord, and the editor reports it on the next save.
    _BH_Profiles := profiles
    _BH_Loaded := true
    return _BH_Profiles
}

_BH_SaveProfiles(profiles) {
    global _BH_Profiles, _BH_Loaded
    checked := _BH_ValidateProfiles(profiles)
    if !checked.ok
        return checked

    path := _BH_Path()
    oldRaw := FileExist(path) ? Trim(IniRead(path, "Meta", "Count", "0")) : "0"
    oldCount := IsInteger(oldRaw) ? Min(Max(Integer(oldRaw), 0), 64) : 0
    clearCount := Max(oldCount, checked.profiles.Length)
    ; Before the IniDelete loop: on a first save that loop is what creates the
    ; file, and an ANSI file would mangle every non-ASCII character name written
    ; through it afterwards.
    EnsureIniUtf16(path)
    try {
        Loop clearCount
            try IniDelete(path, "Profile." A_Index)
        IniWrite("1", path, "Meta", "Version")
        IniWrite(checked.profiles.Length, path, "Meta", "Count")
        for profileIndex, profile in checked.profiles {
            section := "Profile." profileIndex
            IniWrite(profile["name"], path, section, "Name")
            IniWrite(profile["classId"], path, section, "Class")
            IniWrite(profile["bindings"].Length, path, section, "BindingCount")
            for bindingIndex, binding in profile["bindings"] {
                prefix := "Binding" bindingIndex
                IniWrite(binding["chord"], path, section, prefix "Chord")
                IniWrite(binding["actor"], path, section, prefix "Actor")
                IniWrite(binding["skill"], path, section, prefix "Skill")
            }
        }
    } catch as err {
        return {ok: false, reason: "Better Hotkeys could not save its profiles. " err.Message}
    }

    _BH_Profiles := checked.profiles
    _BH_Loaded := true
    ApplyAllHotkeys()
    return {ok: true, reason: "", profiles: _BH_Profiles}
}

_BH_CloneProfiles(profiles := 0) {
    global _BH_Profiles
    source := IsObject(profiles) ? profiles : _BH_Profiles
    out := []
    for profile in source {
        bindings := []
        for binding in profile["bindings"]
            bindings.Push(Map("chord", binding["chord"], "actor", binding["actor"], "skill", binding["skill"]))
        out.Push(Map("name", profile["name"], "classId", profile["classId"], "bindings", bindings))
    }
    return out
}

_BH_SanitizeName(name) {
    s := Trim(String(name))
    s := StrReplace(StrReplace(StrReplace(s, "`r", " "), "`n", " "), "`t", " ")
    while InStr(s, "  ")
        s := StrReplace(s, "  ", " ")
    return SubStr(Trim(s), 1, 48)
}

_BH_NormalizeName(name) {
    return StrLower(_BH_SanitizeName(name))
}

_BH_IsSkillForClass(classId, skill) {
    global _BH_CLASS_SKILLS
    if !_BH_CLASS_SKILLS.Has(classId)
        return false
    for allowed in _BH_CLASS_SKILLS[classId]
        if (allowed = skill)
            return true
    return false
}

_BH_ValidateProfiles(rawProfiles) {
    if !(rawProfiles is Array) || rawProfiles.Length > 64
        return {ok: false, reason: "Better Hotkeys supports up to 64 character profiles."}

    profiles := []
    seenNames := Map()
    for rawProfile in rawProfiles {
        if !(rawProfile is Map)
            return {ok: false, reason: "A character profile could not be read. Reopen the editor and try again."}
        name := _BH_SanitizeName(rawProfile.Has("name") ? rawProfile["name"] : "")
        if (name = "")
            return {ok: false, reason: "Enter a character name before saving."}
        nameKey := _BH_NormalizeName(name)
        if seenNames.Has(nameKey)
            return {ok: false, reason: "A profile for '" name "' already exists."}
        seenNames[nameKey] := true

        rawClass := rawProfile.Has("classId") ? rawProfile["classId"] : -1
        if !IsNumber(rawClass)
            return {ok: false, reason: "Choose a class for '" name "'."}
        classId := Integer(Round(rawClass))
        if (classId < 0 || classId > 7)
            return {ok: false, reason: "Choose a valid class for '" name "'."}

        rawBindings := rawProfile.Has("bindings") ? rawProfile["bindings"] : []
        if !(rawBindings is Array) || rawBindings.Length > 64
            return {ok: false, reason: "'" name "' supports up to 64 bindings."}
        bindings := []
        seenChords := Map()
        for rawBinding in rawBindings {
            if !(rawBinding is Map)
                return {ok: false, reason: "A binding for '" name "' could not be read."}
            chord := NormalizeHotkeyChord(rawBinding.Has("chord") ? rawBinding["chord"] : "")
            if (chord = "")
                return {ok: false, reason: "Set a hotkey for every binding on '" name "', or delete the unfinished row."}
            if !IsHotkeyChordValid(chord)
                return {ok: false, reason: "'" chord "' is not a valid hotkey for '" name "'."}
            chordKey := StrLower(chord)
            if seenChords.Has(chordKey)
                return {ok: false, reason: "'" FormatHotkeyDisplay(chord) "' is assigned twice on '" name "'."}
            seenChords[chordKey] := true

            conflict := _BH_GlobalConflict(chord)
            if (conflict != "")
                return {ok: false, reason: "'" FormatHotkeyDisplay(chord) "' is already used by " conflict "."}

            actor := StrLower(Trim(rawBinding.Has("actor") ? String(rawBinding["actor"]) : "player"))
            if (actor != "player")
                return {ok: false, reason: "Pet skill bindings are not available yet. Choose a player skill."}
            rawSkill := rawBinding.Has("skill") ? rawBinding["skill"] : -1
            if !IsNumber(rawSkill)
                return {ok: false, reason: "Choose a skill for '" name "'."}
            skill := Integer(Round(rawSkill))
            if !_BH_IsSkillForClass(classId, skill)
                return {ok: false, reason: _BH_SkillName(skill) " is not available to " _BH_ClassName(classId) "."}
            bindings.Push(Map("chord", chord, "actor", actor, "skill", skill))
        }
        profiles.Push(Map("name", name, "classId", classId, "bindings", bindings))
    }
    return {ok: true, reason: "", profiles: profiles}
}

_BH_ClassName(classId) {
    global _BH_CLASS_NAMES
    return (classId >= 0 && classId < _BH_CLASS_NAMES.Length)
        ? _BH_CLASS_NAMES[classId + 1] : "Unknown class"
}

_BH_SkillName(skill) {
    global _BH_SKILL_NAMES
    return (skill >= 0 && skill < _BH_SKILL_NAMES.Length)
        ? _BH_SKILL_NAMES[skill + 1] : "Unknown skill"
}

; ── Central hotkey registry integration ─────────────────────────

_BH_IsOwnActionId(id) {
    global _BH_ACTION_ID_PREFIX
    return SubStr(String(id), 1, StrLen(_BH_ACTION_ID_PREFIX)) = _BH_ACTION_ID_PREFIX
}

_BH_GlobalConflict(chord) {
    global gHotkeyActions, gHotkeyReserved
    chordKey := StrLower(NormalizeHotkeyChord(chord))
    if (chordKey = "")
        return ""
    for reserved in gHotkeyReserved {
        if (StrLower(reserved) = chordKey)
            return "a reserved Maps++ shortcut"
    }
    for id, action in gHotkeyActions {
        if _BH_IsOwnActionId(id) || !IsHotkeyActionEnabled(action)
            continue
        if (StrLower(action["chord"]) = chordKey)
            return action.Has("label") ? action["label"] : id
    }
    return ""
}

_BH_SyncRegistryActions() {
    global gHotkeyActions, _BH_Profiles, _BH_ACTION_ID_PREFIX
    for id, _ in gHotkeyActions.Clone() {
        if _BH_IsOwnActionId(id)
            gHotkeyActions.Delete(id)
    }

    chords := Map()
    for profile in _BH_Profiles {
        for binding in profile["bindings"] {
            chord := binding["chord"]
            chordKey := StrLower(chord)
            if !chords.Has(chordKey) && _BH_GlobalConflict(chord) = ""
                chords[chordKey] := chord
        }
    }

    i := 0
    for _, chord in chords {
        i += 1
        RegisterHotkeyAction(Map(
            "id", _BH_ACTION_ID_PREFIX i,
            "label", "Better Hotkeys character skill",
            "category", "Better Hotkeys",
            "default", chord,
            "addon", "BetterHotkeys",
            "handler", _BH_Dispatch.Bind(chord),
            "hotIfWinActive", true,
            "hidden", true,
            "persist", false
        ))
    }
}

; ── Battle dispatch ─────────────────────────────────────────────

_BH_FindProfile(name) {
    global _BH_Profiles
    key := _BH_NormalizeName(name)
    for profile in _BH_Profiles
        if (_BH_NormalizeName(profile["name"]) = key)
            return profile
    return 0
}

; The action fields are shared with battle_send under its names; only the skill
; fields are this addon's own. See the RegisterAddonOffset block at the top.
_BH_ActionOffsets(actor) {
    if (actor = "pet")
        return {skill: "BetterHotkeysPetSkill", action: "PetBattleAction"}
    return {skill: "BetterHotkeysPlayerSkill", action: "BattleAction"}
}

_BH_Dispatch(chord) {
    global _BH_PROCESS_ACCESS, _BH_ACTION_SKILL
    hwnd := WinActive(GAME_WIN_FILTER)
    if !hwnd
        return
    charName := CharacterNameFromWindow(hwnd)
    if (charName = "")
        return
    profile := _BH_FindProfile(charName)
    if !IsObject(profile)
        return

    binding := 0
    for candidate in profile["bindings"] {
        if (StrLower(candidate["chord"]) = StrLower(chord)) {
            binding := candidate
            break
        }
    }
    if !IsObject(binding)
        return

    pid := 0
    try pid := WinGetPID("ahk_id " hwnd)
    if !pid
        return
    handle := DllCall("OpenProcess", "UInt", _BH_PROCESS_ACCESS,
        "Int", 0, "UInt", pid, "Ptr")
    if !handle {
        _BH_ReportOnce("open", "Better Hotkeys could not access the active game client.")
        return
    }

    try {
        modBase := GetModuleBaseAddress(handle, PROCESS_EXE)
        if !modBase {
            _BH_ReportOnce("module", "Better Hotkeys could not locate main.exe in the active client.")
            return
        }

        battleBuf := Buffer(4, 0)
        if !DllCall("ReadProcessMemory", "Ptr", handle,
            "Ptr", modBase + GetResolvedOffset("BATTLE_STATE_OFFSET"),
            "Ptr", battleBuf.Ptr, "UPtr", 4, "UPtr*", 0, "Int")
            return
        if (NumGet(battleBuf, 0, "Int") = 0)
            return

        classBuf := Buffer(4, 0)
        if DllCall("ReadProcessMemory", "Ptr", handle,
            "Ptr", modBase + GetResolvedOffset("CHAR_CLASS_OFFSET"),
            "Ptr", classBuf.Ptr, "UPtr", 4, "UPtr*", 0, "Int") {
            liveClass := NumGet(classBuf, 0, "Int")
            if (liveClass >= 0 && liveClass <= 7 && liveClass != profile["classId"]) {
                _BH_ReportOnce("class:" pid,
                    charName " is " _BH_ClassName(liveClass) ", but the saved Better Hotkeys profile is "
                    . _BH_ClassName(profile["classId"]) ". Update the profile before using it.")
                return
            }
        }

        offsets := _BH_ActionOffsets(binding["actor"])
        skillBuf := Buffer(4, 0)
        actionBuf := Buffer(4, 0)
        NumPut("Int", binding["skill"], skillBuf, 0)
        NumPut("Int", _BH_ACTION_SKILL, actionBuf, 0)
        skillOk := DllCall("WriteProcessMemory", "Ptr", handle,
            "Ptr", modBase + GetResolvedOffset(offsets.skill),
            "Ptr", skillBuf.Ptr, "UPtr", 4, "UPtr*", 0, "Int")
        actionOk := skillOk && DllCall("WriteProcessMemory", "Ptr", handle,
            "Ptr", modBase + GetResolvedOffset(offsets.action),
            "Ptr", actionBuf.Ptr, "UPtr", 4, "UPtr*", 0, "Int")
        if !actionOk
            _BH_ReportOnce("write:" pid, "Better Hotkeys could not set the selected skill in the active client.")
    } finally {
        DllCall("CloseHandle", "Ptr", handle)
    }
}

_BH_ReportOnce(key, message) {
    global _BH_LastNotice
    now := A_TickCount
    if _BH_LastNotice.Has(key) && now - _BH_LastNotice[key] < 5000
        return
    _BH_LastNotice[key] := now
    TrayTip(message, "Better Hotkeys", "Iconx")
}

; Every dialog this addon raises goes through these three, so both the theming
; and the ownership are decided in one place. ShowMessage/ConfirmAction/
; PromptText (dialogs.ahk) draw the themed WebView2 dialog in enhanced mode and
; fall back to MsgBox/InputBox in native mode.
;
; The +OwnDialogs below only matters to that fallback: MsgBox and InputBox are
; unowned top-level windows, so against either frontend's +AlwaysOnTop they open
; *behind* the panel and make the application look locked. It lasts only for the
; current thread, hence setting it before every dialog.
_BH_DialogOpts(severity, extra := 0) {
    global _BH_PanelMode, _BH_WebGui, _BH_NativeGui
    o := IsObject(extra) ? extra : Map()
    o["severity"] := severity
    g := 0
    if (_BH_PanelMode = "web")
        g := _BH_WebGui
    else if (_BH_PanelMode = "native")
        g := _BH_NativeGui
    if (IsObject(g) && g.Hwnd) {
        try g.Opt("+OwnDialogs")
        o["owner"] := g.Hwnd
    }
    return o
}

_BH_Say(text, severity := "warn", title := "Better Hotkeys", opts := 0) {
    return ShowMessage(text, title, _BH_DialogOpts(severity, opts))
}

_BH_Ask(text, severity := "ask", title := "Better Hotkeys", opts := 0) {
    return ConfirmAction(text, title, _BH_DialogOpts(severity, opts))
}

_BH_Prompt(text, title, opts := 0, defaultValue := "") {
    return PromptText(text, title, defaultValue, _BH_DialogOpts("info", opts))
}

; ── Shared editor state and hotkey capture ───────────────────────

global _BH_PanelMode := ""
global _BH_WebGui := 0
global _BH_WebDirty := false
global _BH_CaptureHook := 0
global _BH_CaptureTarget := 0

_BH_CanUseWebView() {
    return FileExist(A_ScriptDir "\Lib\WebViewToo.ahk")
        && FileExist(A_ScriptDir "\ui\better_hotkeys\index.html")
}

_BH_OpenEditor() {
    global _BH_PanelMode, _BH_WebGui, _BH_NativeGui
    if (_BH_PanelMode = "web" && IsObject(_BH_WebGui) && _BH_WebGui.Hwnd) {
        try WinActivate("ahk_id " _BH_WebGui.Hwnd)
        return
    }
    if (_BH_PanelMode = "native" && IsObject(_BH_NativeGui) && _BH_NativeGui.Hwnd) {
        try WinActivate("ahk_id " _BH_NativeGui.Hwnd)
        return
    }
    _BH_LoadProfiles()
    if (IsNativeInterface() || !_BH_CanUseWebView())
        _BH_ShowNative()
    else
        _BH_ShowWeb()
}

; The native editor rebuilds its ListViews from scratch, which resets the
; selection and closes an open dropdown — doing that unconditionally once a
; second fought the user mid-edit. Only the live Online/Active column can change
; between polls, so the rebuild is skipped unless that column actually would.
global _BH_LastLiveSig := ""

_BH_OnSnapshot(snapshots) {
    global _BH_PanelMode, _BH_CaptureTarget, _BH_LastLiveSig
    if (_BH_PanelMode = "web") {
        _BH_WebPost(_BH_LiveStateJson())
        return
    }
    if (_BH_PanelMode != "native")
        return
    if (IsObject(_BH_CaptureTarget) && _BH_CaptureTarget["mode"] = "native")
        return
    sig := _BH_LiveSignature()
    if (sig = _BH_LastLiveSig)
        return
    _BH_LastLiveSig := sig
    _BH_NativeRefreshProfiles()
}

; Everything the native profile list renders from the live poll, as one string.
_BH_LiveSignature() {
    sig := ""
    for snap in GetClientSnapshots()
        sig .= snap.charName "|" snap.classId "|" (snap.isActive ? 1 : 0) "`n"
    return sig
}

_BH_LiveStateJson() {
    clientsJson := ""
    for snap in GetClientSnapshots() {
        if (snap.charName = "")
            continue
        clientsJson .= (clientsJson = "" ? "" : ",")
            . '{"name":' _JSON_Str(snap.charName)
            . ',"classId":' snap.classId
            . ',"inBattle":' (snap.inBattle ? "true" : "false")
            . ',"active":' (snap.isActive ? "true" : "false") '}'
    }
    return '{"type":"live-state","clients":[' clientsJson ']}'
}

_BH_StartCapture(target) {
    global _BH_CaptureHook, _BH_CaptureTarget
    _BH_CancelCapture(false)
    _BH_CaptureTarget := target
    if (target["mode"] = "web")
        _BH_WebPost('{"type":"capture-started","profileIndex":' target["profileIndex"]
            . ',"bindingIndex":' target["bindingIndex"] '}')
    else if target.Has("button")
        target["button"].Text := "Press shortcut…"

    ih := InputHook("L0")
    ih.KeyOpt("{All}", "N")
    ih.OnKeyDown := _BH_CaptureKeyDown
    ih.Start()
    _BH_CaptureHook := ih
}

_BH_CancelCapture(notify := true) {
    global _BH_CaptureHook, _BH_CaptureTarget
    target := _BH_CaptureTarget
    if IsObject(_BH_CaptureHook) {
        try _BH_CaptureHook.Stop()
        _BH_CaptureHook := 0
    }
    _BH_CaptureTarget := 0
    if !notify || !IsObject(target)
        return
    if (target["mode"] = "web")
        _BH_WebPost('{"type":"capture-cancelled","profileIndex":' target["profileIndex"]
            . ',"bindingIndex":' target["bindingIndex"] '}')
    else
        _BH_NativeRefreshBindings()
}

_BH_CaptureKeyDown(hook, vk, sc) {
    global _BH_CaptureHook, _BH_CaptureTarget, _BH_WebGui, _BH_NativeGui
    if !IsObject(_BH_CaptureTarget)
        return
    mode := _BH_CaptureTarget["mode"]
    guiObj := (mode = "web") ? _BH_WebGui : _BH_NativeGui
    if !IsObject(guiObj) || !WinActive("ahk_id " guiObj.Hwnd)
        return
    if (vk = 0x1B) {
        _BH_CancelCapture()
        return
    }

    mods := ""
    if GetKeyState("Ctrl")
        mods .= "^"
    if GetKeyState("Alt")
        mods .= "!"
    if GetKeyState("Shift")
        mods .= "+"
    if GetKeyState("LWin") || GetKeyState("RWin")
        mods .= "#"
    keyName := GetKeyName(Format("vk{:02X}", vk))
    if (keyName = "" || RegExMatch(keyName,
        "i)^(Control|Alt|Shift|LShift|RShift|LControl|RControl|LAlt|RAlt|LWin|RWin)$"))
        return
    if RegExMatch(keyName, "i)Button$")
        return
    chord := mods keyName
    if !IsHotkeyChordValid(chord)
        return
    conflict := _BH_GlobalConflict(chord)
    if (conflict != "") {
        TrayTip("Already used by " conflict ".", "Better Hotkeys", "Iconx")
        return
    }

    target := _BH_CaptureTarget
    hook.Stop()
    _BH_CaptureHook := 0
    _BH_CaptureTarget := 0
    if (mode = "web") {
        _BH_WebPost('{"type":"hotkey-captured","profileIndex":' target["profileIndex"]
            . ',"bindingIndex":' target["bindingIndex"]
            . ',"chord":' _JSON_Str(chord)
            . ',"display":' _JSON_Str(FormatHotkeyDisplay(chord)) '}')
    } else {
        _BH_NativeApplyCapturedChord(chord)
    }
}

; ── WebView2 editor ─────────────────────────────────────────────

_BH_ShowWeb() {
    global _BH_WebGui, _BH_PanelMode, _BH_WebDirty
    dllDir := (A_PtrSize = 8) ? "64bit" : "32bit"
    settings := {DllPath: A_ScriptDir "\Lib\" dllDir "\WebView2Loader.dll",
        DefaultWidth: 1020, DefaultHeight: 700}
    g := WebViewGui("-Caption +AlwaysOnTop +Resize", "Maps++ — Better Hotkeys", , settings)
    g.OnEvent("Close", (*) => _BH_WebRequestClose())
    g.WebMessageReceived(_BH_OnWebMessage)
    g.DOMContentLoaded((*) => SetTimer(_BH_SendWebState, -50))
    g.Navigate(UiPageUrl("ui/better_hotkeys/index.html"))
    _BH_WebGui := g
    _BH_PanelMode := "web"
    _BH_WebDirty := false
    g.Show("w1020 h700 Center")
}

_BH_WebPost(json) {
    global _BH_WebGui
    if IsObject(_BH_WebGui)
        try _BH_WebGui.PostWebMessageAsJson(json)
}

_BH_SendWebState() {
    global _BH_Profiles, _BH_CLASS_NAMES, _BH_CLASS_SKILLS
    profilesJson := ""
    for profile in _BH_Profiles {
        bindingsJson := ""
        for binding in profile["bindings"] {
            bindingsJson .= (bindingsJson = "" ? "" : ",")
                . '{"chord":' _JSON_Str(binding["chord"])
                . ',"display":' _JSON_Str(FormatHotkeyDisplay(binding["chord"]))
                . ',"actor":' _JSON_Str(binding["actor"])
                . ',"skill":' binding["skill"] '}'
        }
        profilesJson .= (profilesJson = "" ? "" : ",")
            . '{"name":' _JSON_Str(profile["name"])
            . ',"classId":' profile["classId"]
            . ',"bindings":[' bindingsJson ']}'
    }

    classesJson := ""
    Loop _BH_CLASS_NAMES.Length {
        classId := A_Index - 1
        skillsJson := ""
        for enum in _BH_CLASS_SKILLS[classId] {
            skillsJson .= (skillsJson = "" ? "" : ",")
                . '{"enum":' enum ',"name":' _JSON_Str(_BH_SkillName(enum))
                . ',"tier":' (Mod(enum, 4) + 1) '}'
        }
        classesJson .= (classesJson = "" ? "" : ",")
            . '{"id":' classId ',"name":' _JSON_Str(_BH_CLASS_NAMES[A_Index])
            . ',"skills":[' skillsJson ']}'
    }

    _BH_WebPost('{"type":"editor-state","profiles":[' profilesJson ']'
        . ',"classes":[' classesJson ']}')
    _BH_WebPost(_BH_LiveStateJson())
}

_BH_OnWebMessage(wv, args) {
    global _BH_WebDirty
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
            _BH_SendWebState()
        case "dirty-state":
            _BH_WebDirty := msg.Has("dirty") && msg["dirty"]
        case "start-capture":
            profileIndex := msg.Has("profileIndex") && IsNumber(msg["profileIndex"])
                ? Integer(Round(msg["profileIndex"])) : 0
            bindingIndex := msg.Has("bindingIndex") && IsNumber(msg["bindingIndex"])
                ? Integer(Round(msg["bindingIndex"])) : 0
            if (profileIndex > 0 && bindingIndex > 0)
                _BH_StartCapture(Map("mode", "web", "profileIndex", profileIndex,
                    "bindingIndex", bindingIndex))
        case "cancel-capture":
            _BH_CancelCapture()
        case "save":
            _BH_WebSave(msg)
        case "close":
            _BH_WebRequestClose()
    }
}

_BH_WebSave(msg) {
    global _BH_WebDirty
    if !msg.Has("profiles") {
        _BH_WebPost('{"type":"save-result","ok":false,"message":"No profiles were received."}')
        return
    }
    result := _BH_SaveProfiles(msg["profiles"])
    if !result.ok {
        _BH_WebPost('{"type":"save-result","ok":false,"message":' _JSON_Str(result.reason) '}')
        return
    }
    _BH_WebDirty := false
    _BH_WebPost('{"type":"save-result","ok":true,"message":"Hotkeys saved and active."}')
    _BH_SendWebState()
}

_BH_WebRequestClose() {
    global _BH_WebGui, _BH_WebDirty, _BH_PanelMode
    if _BH_WebDirty {
        if !_BH_Ask("Close and discard your unsaved Better Hotkeys changes?")
            return 1
    }
    _BH_CancelCapture(false)
    if IsObject(_BH_WebGui)
        try _BH_WebGui.Destroy()
    _BH_WebGui := 0
    _BH_WebDirty := false
    _BH_PanelMode := ""
    return 1
}

; ── Native editor ────────────────────────────────────────────────

global _BH_NativeGui := 0
global _BH_NativeProfiles := 0
global _BH_NativeBindings := 0
global _BH_NativeClass := 0
global _BH_NativeSkill := 0
global _BH_NativeHotkey := 0
global _BH_NativeStatus := 0
global _BH_NativeDraft := []
global _BH_NativeProfileIndex := 0
global _BH_NativeBindingIndex := 0
global _BH_NativeSkillEnums := []
global _BH_NativeDirty := false
global _BH_NativeRefreshing := false

_BH_ShowNative() {
    global _BH_PanelMode, _BH_NativeGui, _BH_NativeDraft, _BH_NativeDirty, _BH_LastLiveSig
    _BH_NativeEnsureGui()
    _BH_NativeDraft := _BH_CloneProfiles()
    _BH_NativeDirty := false
    _BH_PanelMode := "native"
    ; The refresh below brings the list up to date; record what it reflects so
    ; the first poll doesn't immediately rebuild it again.
    _BH_LastLiveSig := _BH_LiveSignature()
    _BH_NativeRefreshProfiles()
    _BH_NativeGui.Show("w900 h560 Center")
}

_BH_NativeEnsureGui() {
    global _BH_NativeGui, _BH_NativeProfiles, _BH_NativeBindings
    global _BH_NativeClass, _BH_NativeSkill, _BH_NativeHotkey, _BH_NativeStatus
    global _BH_CLASS_NAMES
    if IsObject(_BH_NativeGui) && _BH_NativeGui.Hwnd
        return
    g := Gui("+AlwaysOnTop -MaximizeBox", "Maps++ — Better Hotkeys")
    g.MarginX := 12
    g.MarginY := 12
    g.SetFont("s9", "Segoe UI")

    g.Add("Text", "x12 y14 w250", "Character profiles")
    _BH_NativeProfiles := g.Add("ListView", "x12 y36 w270 r18 Grid -Multi NoSortHdr",
        ["Character", "Class", "State", "Keys"])
    _BH_NativeProfiles.OnEvent("ItemSelect", _BH_NativeOnProfileSelect)
    g.Add("Button", "x12 y+8 w82", "Create…").OnEvent("Click", (*) => _BH_NativeCreateProfile())
    g.Add("Button", "x+6 yp w82", "Delete").OnEvent("Click", (*) => _BH_NativeDeleteProfile())

    g.Add("Text", "x302 y14 w80", "Class:")
    _BH_NativeClass := g.Add("DropDownList", "x350 y10 w190")
    _BH_NativeClass.Add(_BH_CLASS_NAMES)
    _BH_NativeClass.OnEvent("Change", (*) => _BH_NativeChangeClass())
    _BH_NativeStatus := g.Add("Text", "x552 y14 w330", "Create or select a character profile.")

    _BH_NativeBindings := g.Add("ListView", "x302 y36 w580 r15 Grid -Multi NoSortHdr",
        ["Hotkey", "Skill", "Actor"])
    _BH_NativeBindings.OnEvent("ItemSelect", _BH_NativeOnBindingSelect)
    g.Add("Button", "x302 y+8 w96", "Add binding").OnEvent("Click", (*) => _BH_NativeAddBinding())
    g.Add("Button", "x+6 yp w96", "Delete binding").OnEvent("Click", (*) => _BH_NativeDeleteBinding())

    g.Add("Text", "x302 y+18 w52", "Hotkey:")
    _BH_NativeHotkey := g.Add("Button", "x360 yp-4 w150", "Select a binding")
    _BH_NativeHotkey.OnEvent("Click", (*) => _BH_NativeCaptureSelected())
    g.Add("Text", "x528 yp+4 w38", "Skill:")
    _BH_NativeSkill := g.Add("DropDownList", "x570 yp-4 w250")
    _BH_NativeSkill.OnEvent("Change", (*) => _BH_NativeChangeSkill())

    g.Add("Text", "x12 y520 w570", "Shortcuts apply only to the active character while that client is in battle.")
    g.Add("Button", "x626 y512 w120 Default", "Save hotkeys").OnEvent("Click", (*) => _BH_NativeSave())
    g.Add("Button", "x+8 yp w100", "Close").OnEvent("Click", (*) => _BH_NativeClose())

    g.OnEvent("Close", (*) => _BH_NativeClose())
    g.OnEvent("Escape", (*) => _BH_NativeClose())
    _BH_NativeGui := g
}

_BH_NativeProfileOnline(name) {
    key := _BH_NormalizeName(name)
    for snap in GetClientSnapshots()
        if (snap.charName != "" && _BH_NormalizeName(snap.charName) = key)
            return snap
    return 0
}

_BH_NativeRefreshProfiles() {
    global _BH_NativeProfiles, _BH_NativeDraft, _BH_NativeProfileIndex, _BH_NativeRefreshing
    if !IsObject(_BH_NativeProfiles)
        return
    _BH_NativeRefreshing := true
    selected := _BH_NativeProfileIndex
    _BH_NativeProfiles.Opt("-Redraw")
    _BH_NativeProfiles.Delete()
    for profile in _BH_NativeDraft {
        live := _BH_NativeProfileOnline(profile["name"])
        state := IsObject(live) ? (live.isActive ? "Active" : "Online") : "Offline"
        _BH_NativeProfiles.Add(, profile["name"], _BH_ClassName(profile["classId"]),
            state, profile["bindings"].Length)
    }
    loop 4
        _BH_NativeProfiles.ModifyCol(A_Index, "AutoHdr")
    if (selected > 0 && selected <= _BH_NativeDraft.Length)
        _BH_NativeProfiles.Modify(selected, "Select Focus Vis")
    else if _BH_NativeDraft.Length {
        selected := 1
        _BH_NativeProfileIndex := 1
        _BH_NativeProfiles.Modify(1, "Select Focus Vis")
    }
    _BH_NativeProfiles.Opt("+Redraw")
    _BH_NativeRefreshing := false
    _BH_NativeRefreshBindings()
}

_BH_NativeOnProfileSelect(ctrl, row, selected) {
    global _BH_NativeProfileIndex, _BH_NativeBindingIndex, _BH_NativeRefreshing
    if !selected || _BH_NativeRefreshing
        return
    _BH_NativeProfileIndex := row
    _BH_NativeBindingIndex := 0
    _BH_NativeRefreshBindings()
}

_BH_NativeRefreshBindings() {
    global _BH_NativeDraft, _BH_NativeProfileIndex, _BH_NativeBindingIndex
    global _BH_NativeBindings, _BH_NativeClass, _BH_NativeSkill, _BH_NativeHotkey
    global _BH_NativeStatus, _BH_NativeSkillEnums, _BH_NativeRefreshing
    global _BH_CLASS_SKILLS
    if !IsObject(_BH_NativeBindings)
        return
    _BH_NativeRefreshing := true
    _BH_NativeBindings.Delete()
    _BH_NativeSkill.Delete()
    _BH_NativeSkillEnums := []
    if (_BH_NativeProfileIndex < 1 || _BH_NativeProfileIndex > _BH_NativeDraft.Length) {
        _BH_NativeClass.Choose(0)
        _BH_NativeHotkey.Text := "Select a binding"
        _BH_NativeStatus.Text := "Create or select a character profile."
        _BH_NativeRefreshing := false
        return
    }
    profile := _BH_NativeDraft[_BH_NativeProfileIndex]
    _BH_NativeClass.Choose(profile["classId"] + 1)
    live := _BH_NativeProfileOnline(profile["name"])
    _BH_NativeStatus.Text := IsObject(live)
        ? (live.classId >= 0 && live.classId != profile["classId"]
            ? "Class differs from the running client."
            : (live.isActive ? "Active client" : "Client online"))
        : "Offline — profile remains editable"
    for binding in profile["bindings"]
        _BH_NativeBindings.Add(, FormatHotkeySettingsDisplay(binding["chord"]),
            _BH_SkillName(binding["skill"]), "Player")
    loop 3
        _BH_NativeBindings.ModifyCol(A_Index, "AutoHdr")

    skills := _BH_CLASS_SKILLS[profile["classId"]]
    labels := []
    for enum in skills {
        _BH_NativeSkillEnums.Push(enum)
        labels.Push(_BH_SkillName(enum))
    }
    _BH_NativeSkill.Add(labels)
    if (_BH_NativeBindingIndex > 0 && _BH_NativeBindingIndex <= profile["bindings"].Length) {
        binding := profile["bindings"][_BH_NativeBindingIndex]
        _BH_NativeBindings.Modify(_BH_NativeBindingIndex, "Select Focus Vis")
        _BH_NativeHotkey.Text := binding["chord"] = ""
            ? "Press to set…" : FormatHotkeyDisplay(binding["chord"])
        for i, enum in _BH_NativeSkillEnums
            if (enum = binding["skill"])
                _BH_NativeSkill.Choose(i)
    } else {
        _BH_NativeBindingIndex := 0
        _BH_NativeHotkey.Text := "Select a binding"
        _BH_NativeSkill.Choose(0)
    }
    _BH_NativeRefreshing := false
}

_BH_NativeOnBindingSelect(ctrl, row, selected) {
    global _BH_NativeBindingIndex, _BH_NativeRefreshing
    if !selected || _BH_NativeRefreshing
        return
    _BH_NativeBindingIndex := row
    _BH_NativeRefreshBindings()
}

_BH_NativeCreateProfile() {
    global _BH_NativeDraft, _BH_NativeProfileIndex, _BH_NativeDirty
    ib := _BH_Prompt("Enter the character name exactly as it appears in the game client.",
        "Better Hotkeys — Create Profile",
        Map("inputLabel", "Character name", "okLabel", "Create"))
    if (ib.Result != "OK")
        return
    name := _BH_SanitizeName(ib.Value)
    if (name = "") {
        _BH_Say("Enter a character name before creating the profile.")
        return
    }
    for profile in _BH_NativeDraft {
        if (_BH_NormalizeName(profile["name"]) = _BH_NormalizeName(name)) {
            _BH_Say("A profile for '" name "' already exists.")
            return
        }
    }
    live := _BH_NativeProfileOnline(name)
    if (IsObject(live) && live.classId >= 0) {
        classId := live.classId
    } else {
        ; The table is two aligned columns, so it goes in the monospace detail
        ; block rather than the message, where a proportional font would stagger
        ; it.
        classTable := "0  Male Human      1  Female Human`n"
            . "2  Male Centaur    3  Female Centaur`n"
            . "4  Male Mage       5  Female Mage`n"
            . "6  Male Borg       7  Female Borg"
        cb := _BH_Prompt(name " is not logged in, so the class could not be read."
            . " Enter it from the list below.",
            "Better Hotkeys — Character Class",
            Map("inputLabel", "Class ID", "detail", classTable), "0")
        if (cb.Result != "OK")
            return
        if !IsInteger(Trim(cb.Value)) || Integer(Trim(cb.Value)) < 0 || Integer(Trim(cb.Value)) > 7 {
            _BH_Say("Class ID must be a number from 0 to 7.")
            return
        }
        classId := Integer(Trim(cb.Value))
    }
    _BH_NativeDraft.Push(Map("name", name, "classId", classId, "bindings", []))
    _BH_NativeProfileIndex := _BH_NativeDraft.Length
    _BH_NativeDirty := true
    _BH_NativeRefreshProfiles()
}

_BH_NativeDeleteProfile() {
    global _BH_NativeDraft, _BH_NativeProfileIndex, _BH_NativeBindingIndex, _BH_NativeDirty
    if (_BH_NativeProfileIndex < 1 || _BH_NativeProfileIndex > _BH_NativeDraft.Length)
        return
    _BH_NativeDraft.RemoveAt(_BH_NativeProfileIndex)
    _BH_NativeProfileIndex := Min(_BH_NativeProfileIndex, _BH_NativeDraft.Length)
    _BH_NativeBindingIndex := 0
    _BH_NativeDirty := true
    _BH_NativeRefreshProfiles()
}

_BH_NativeChangeClass() {
    global _BH_NativeDraft, _BH_NativeProfileIndex, _BH_NativeClass
    global _BH_NativeDirty, _BH_NativeRefreshing, _BH_NativeBindingIndex
    if _BH_NativeRefreshing || _BH_NativeProfileIndex < 1
        return
    profile := _BH_NativeDraft[_BH_NativeProfileIndex]
    newClass := _BH_NativeClass.Value - 1
    if (newClass = profile["classId"])
        return
    ; Skills are class-specific, so the bindings cannot survive the change. That
    ; is a destructive edit reachable by a stray scroll over the dropdown, so it
    ; is confirmed — and refused cleanly, with the dropdown put back.
    if (profile["bindings"].Length > 0) {
        if !_BH_Ask("Changing class clears all " profile["bindings"].Length
            . " binding(s) on '" profile["name"] "', because skills are class-specific."
            . "`n`nChange the class anyway?") {
            _BH_NativeRefreshBindings()      ; restores the dropdown selection
            return
        }
    }
    profile["classId"] := newClass
    profile["bindings"] := []
    _BH_NativeBindingIndex := 0
    _BH_NativeDirty := true
    _BH_NativeRefreshProfiles()
}

_BH_NativeAddBinding() {
    global _BH_NativeDraft, _BH_NativeProfileIndex, _BH_NativeBindingIndex, _BH_NativeDirty
    global _BH_CLASS_SKILLS
    if (_BH_NativeProfileIndex < 1 || _BH_NativeProfileIndex > _BH_NativeDraft.Length)
        return
    profile := _BH_NativeDraft[_BH_NativeProfileIndex]
    if (profile["bindings"].Length >= 64) {
        _BH_Say("A character profile can contain up to 64 bindings.")
        return
    }
    firstSkill := _BH_CLASS_SKILLS[profile["classId"]][1]
    profile["bindings"].Push(Map("chord", "", "actor", "player", "skill", firstSkill))
    _BH_NativeBindingIndex := profile["bindings"].Length
    _BH_NativeDirty := true
    _BH_NativeRefreshProfiles()
    _BH_NativeCaptureSelected()
}

_BH_NativeDeleteBinding() {
    global _BH_NativeDraft, _BH_NativeProfileIndex, _BH_NativeBindingIndex, _BH_NativeDirty
    if (_BH_NativeProfileIndex < 1 || _BH_NativeBindingIndex < 1)
        return
    bindings := _BH_NativeDraft[_BH_NativeProfileIndex]["bindings"]
    if (_BH_NativeBindingIndex > bindings.Length)
        return
    bindings.RemoveAt(_BH_NativeBindingIndex)
    _BH_NativeBindingIndex := Min(_BH_NativeBindingIndex, bindings.Length)
    _BH_NativeDirty := true
    _BH_NativeRefreshProfiles()
}

_BH_NativeCaptureSelected() {
    global _BH_NativeProfileIndex, _BH_NativeBindingIndex, _BH_NativeHotkey
    if (_BH_NativeProfileIndex < 1 || _BH_NativeBindingIndex < 1)
        return
    _BH_StartCapture(Map("mode", "native", "button", _BH_NativeHotkey))
}

_BH_NativeApplyCapturedChord(chord) {
    global _BH_NativeDraft, _BH_NativeProfileIndex, _BH_NativeBindingIndex, _BH_NativeDirty
    profile := _BH_NativeDraft[_BH_NativeProfileIndex]
    for i, binding in profile["bindings"] {
        if (i != _BH_NativeBindingIndex && StrLower(binding["chord"]) = StrLower(chord)) {
            TrayTip("That shortcut is already assigned on " profile["name"] ".",
                "Better Hotkeys", "Iconx")
            _BH_NativeRefreshBindings()
            return
        }
    }
    profile["bindings"][_BH_NativeBindingIndex]["chord"] := chord
    _BH_NativeDirty := true
    _BH_NativeRefreshProfiles()
}

_BH_NativeChangeSkill() {
    global _BH_NativeDraft, _BH_NativeProfileIndex, _BH_NativeBindingIndex
    global _BH_NativeSkill, _BH_NativeSkillEnums, _BH_NativeDirty, _BH_NativeRefreshing
    if _BH_NativeRefreshing || _BH_NativeProfileIndex < 1 || _BH_NativeBindingIndex < 1
        return
    idx := _BH_NativeSkill.Value
    if (idx < 1 || idx > _BH_NativeSkillEnums.Length)
        return
    _BH_NativeDraft[_BH_NativeProfileIndex]["bindings"][_BH_NativeBindingIndex]["skill"] := _BH_NativeSkillEnums[idx]
    _BH_NativeDirty := true
    _BH_NativeRefreshBindings()
}

_BH_NativeSave() {
    global _BH_NativeDraft, _BH_NativeDirty
    result := _BH_SaveProfiles(_BH_NativeDraft)
    if !result.ok {
        _BH_Say(result.reason)
        return
    }
    _BH_NativeDraft := _BH_CloneProfiles(result.profiles)
    _BH_NativeDirty := false
    _BH_NativeRefreshProfiles()
    TrayTip("Hotkeys saved and active.", "Better Hotkeys", "Iconi")
}

_BH_NativeClose() {
    global _BH_NativeGui, _BH_NativeDirty, _BH_PanelMode
    if _BH_NativeDirty {
        if !_BH_Ask("Close and discard your unsaved Better Hotkeys changes?")
            return 1
    }
    _BH_CancelCapture(false)
    if IsObject(_BH_NativeGui)
        try _BH_NativeGui.Hide()
    _BH_PanelMode := ""
    _BH_NativeDirty := false
    return 1
}
