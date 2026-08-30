#Requires AutoHotkey v2.0

; Settings for osmw_chat.dll — the injected chat window mod (font, free
; placement, channel tabs, mentions, timestamps, wheel scroll, cooldown text).
;
; This addon configures the DLL; it never talks to it. There is no channel back
; from the mod and nothing to restart: the DLL re-reads its own settings on a
; ~2s poll from the game thread and applies them live, font changes included.
; Write the key and you are done.
;
; It is an OnSettingsWeb tab rather than a window of its own. These are all
; ordinary preferences, so they belong beside every other preference, and going
; through the field-descriptor contract means the WebView2 and native frontends
; are both rendered from this one list — there is no second layout to keep in
; step, and no chance of the two drifting. Addons earn a separate window by
; needing an editor (Window Layout's canvas, the Character Vendor's price
; grid); a column of checkboxes does not.
;
; Three things here are load-bearing:
;
;   1. The ini is the GAME FOLDER's config.ini, resolved through
;      ResolveGameDirectory(), never Maps++'s CONFIG_INI. Those are the same
;      file in a normal install, but ResolveWritableIniPath() falls back to
;      %AppData% when the game folder is not writable, and the DLL cannot
;      follow it there. addons\auto_login.ahk makes the same distinction for
;      the same reason.
;   2. Writes are per key. [Chat] also holds WinX/WinY/WinW/WinH, which the DLL
;      itself writes whenever the player finishes dragging or resizing the chat
;      window, plus chip/markup keys this tab never shows. Rewriting the
;      section would destroy the player's saved geometry and race the DLL for
;      the file.
;   3. Launch is already wired: EnsureInjectPayload deploys osmw_chat.dll and
;      ResolveLaunchExecutable starts osmw_chat_launch.exe. Nothing here needs
;      to inject, and there is deliberately no attach-to-running-client path —
;      the chat window and its fonts are built during the client's startup
;      init, so injecting later is too late for anything font related.
;
; The launcher is dumb and always injects; [Addons] ChatMod is what decides
; whether the mod does anything, and the DLL reads that key itself.

; One row per key this tab owns. Field ids are the ini key names themselves, so
; the values coming back from OnSettingsWebSave need no translation table.
; kind: "str" | "bool" | "int". Ints are clamped to lo..hi on the way out — the
; number fields carry the same min/max, but a malformed message must not be
; able to hand the DLL a nonsense number.
global _CHATMOD_SPEC := [
    Map("key", "FontFace",          "kind", "str"),
    Map("key", "FontSize",          "kind", "int",  "lo", 0,  "hi", 18),
    Map("key", "FontWidth",         "kind", "int",  "lo", -1, "hi", 64),
    Map("key", "Tabs",              "kind", "str"),
    Map("key", "TabStrip",          "kind", "bool"),
    Map("key", "TabHotkey",         "kind", "str"),
    Map("key", "Timestamps",        "kind", "bool"),
    Map("key", "TimestampFormat",   "kind", "str"),
    Map("key", "WheelScroll",       "kind", "bool"),
    Map("key", "WheelScrollLines",  "kind", "int",  "lo", 0,  "hi", 30),
    Map("key", "CooldownMessage",   "kind", "bool"),
    Map("key", "CooldownFormat",    "kind", "str"),
    Map("key", "HighlightMentions", "kind", "bool"),
    Map("key", "MyName",            "kind", "str"),
    Map("key", "FreePosition",      "kind", "bool"),
    Map("key", "UnlockSize",        "kind", "bool"),
    Map("key", "RememberGeometry",  "kind", "bool"),
    Map("key", "ClearHotkey",       "kind", "str")
]

; Every key the DLL treats as on unless told otherwise. Timestamps is the only
; one that ships off, so it is the only one absent from here.
global _CHATMOD_DEFAULT_ON := ["TabStrip", "WheelScroll", "CooldownMessage",
    "HighlightMentions", "RememberGeometry"]

; Measured against the real renderer by patches\chatmod\tools\fontpresets.py;
; keep the two in step, and treat the tool as the source of truth because it
; measures rather than assumes.
global _CHATMOD_FACES := ["", "Consolas", "Cascadia Mono", "Courier New",
    "Lucida Console"]

; Set when a save drops a value, shown as an info field the next time the tab
; renders and cleared once read. OnSettingsWebSave runs inside the settings
; page's WebMessageReceived callback, where a blocking dialog can never be
; answered — so a rejection is reported this way and by TrayTip, never by
; ShowMessage. Broadcast reports its hotkey clashes through the same route.
global _ChatMod_LastRejection := ""

RegisterAddon(Map(
    "name",              "ChatMod",
    "settingsLabel",     "Chat Mod",
    "OnInit",            _ChatMod_OnInit,
    "OnSettingsWeb",     _ChatMod_OnSettingsWeb,
    "OnSettingsWebSave", _ChatMod_OnSettingsWebSave
))

_ChatMod_OnInit() {
    _ChatMod_SyncGate()
}

; ── The ini ─────────────────────────────────────────────────────

; The game folder's config.ini — what the DLL reads. "" when no game path is
; configured yet, which every caller must handle rather than writing to "\".
_ChatMod_IniPath() {
    gameDir := ResolveGameDirectory()
    return (gameDir = "") ? "" : gameDir "\config.ini"
}

; The chat mod's own shipped defaults, beside the client. Maps++ ships the same
; merged defaults.ini, so in a normal install this is one file documenting both.
_ChatMod_DefaultsPath() {
    gameDir := ResolveGameDirectory()
    return (gameDir = "") ? "" : gameDir "\defaults.ini"
}

_ChatMod_ReadDefault(keyName) {
    defPath := _ChatMod_DefaultsPath()
    if (defPath = "")
        return ""
    return IniRead(defPath, "Chat", keyName, "")
}

; config.ini → defaults.ini → "", the same chain ConfigRead() uses, but against
; the game folder's pair rather than Maps++'s own. Reading the shipped defaults
; means a fresh install shows what the DLL will actually do, not a guess.
_ChatMod_ReadKey(cfgPath, keyName) {
    fallback := _ChatMod_ReadDefault(keyName)
    if (cfgPath = "")
        return fallback
    return IniRead(cfgPath, "Chat", keyName, fallback)
}

_ChatMod_ReadSettings() {
    global _CHATMOD_SPEC
    cfgPath := _ChatMod_IniPath()
    out := Map()
    for spec in _CHATMOD_SPEC
        out[spec["key"]] := _ChatMod_ReadKey(cfgPath, spec["key"])
    return out
}

; Absent means "the DLL's own default", which is on for everything in
; _CHATMOD_DEFAULT_ON and off for the rest.
_ChatMod_AsBool(keyName, val) {
    global _CHATMOD_DEFAULT_ON
    if (val = "1")
        return true
    if (val = "0")
        return false
    for k in _CHATMOD_DEFAULT_ON {
        if (k = keyName)
            return true
    }
    return false
}

_ChatMod_AsInt(val, fallback) {
    text := Trim(String(val))
    return IsInteger(text) ? Integer(text) : fallback
}

; Coerces one incoming value to its declared kind. Returns false when the value
; is unusable, so the caller can skip and log it rather than writing a
; normalised guess the user never asked for. Checkbox fields arrive as
; true/false and number fields as integers, both of which String() renders as
; exactly the "1"/"0"/"18" the ini wants.
_ChatMod_Coerce(spec, raw, &outVal) {
    outVal := ""
    text := Trim(String(raw))
    switch spec["kind"] {
        case "bool":
            if (text != "0" && text != "1")
                return false
            outVal := text
            return true
        case "int":
            if !IsInteger(text)
                return false
            num := Integer(text)
            if (num < spec["lo"])
                num := spec["lo"]
            else if (num > spec["hi"])
                num := spec["hi"]
            outVal := String(num)
            return true
        default:
            ; Free text. Newlines would silently truncate the ini line.
            text := StrReplace(StrReplace(text, "`r", " "), "`n", " ")
            outVal := text
            return true
    }
}

; Writes only the keys the caller supplied, one IniWrite each. Never IniDelete,
; never a section rewrite — see the header. Returns the number written.
_ChatMod_WriteSettings(values) {
    global _CHATMOD_SPEC
    if !IsObject(values)
        return 0
    cfgPath := _ChatMod_IniPath()
    if (cfgPath = "") {
        LogWarn("ChatMod", "No game path configured — nothing written")
        return 0
    }
    ; A no-op once the file exists, which it always does in a real install; the
    ; live one is ANSI because the C DLL created it, and it stays that way.
    EnsureIniUtf16(cfgPath)

    written := 0
    for spec in _CHATMOD_SPEC {
        keyName := spec["key"]
        if !values.Has(keyName)
            continue
        outVal := ""
        if !_ChatMod_Coerce(spec, values[keyName], &outVal) {
            LogWarn("ChatMod", "Skipped " keyName ": not a valid "
                . spec["kind"] " (" String(values[keyName]) ")")
            continue
        }
        try {
            IniWrite(outVal, cfgPath, "Chat", keyName)
            written++
        } catch as err {
            LogWarn("ChatMod", "Could not write " keyName ": " err.Message)
        }
    }
    LogInfo("ChatMod", "Wrote " written " [Chat] key(s) to " cfgPath)
    return written
}

; ── The [Addons] ChatMod gate ───────────────────────────────────

; SetAddonEnabled writes Maps++'s CONFIG_INI, which is not necessarily the file
; the DLL reads. Mirror the state into the game folder so a toggle in Settings
; actually reaches the mod. Idempotent, and a harmless rewrite of the same
; value when the two paths are the same file.
_ChatMod_SyncGate() {
    cfgPath := _ChatMod_IniPath()
    if (cfgPath = "")
        return
    if !FileExist(cfgPath)
        EnsureIniUtf16(cfgPath)
    gate := IsAddonEnabled("ChatMod") ? "1" : "0"
    try IniWrite(gate, cfgPath, "Addons", "ChatMod")
    catch as err
        LogWarn("ChatMod", "Could not write game ChatMod gate: " err.Message)
}

; Called from BuildGameLaunchCommand via GetFuncByName, so the gate still
; reaches the DLL in a build that does not bundle this addon and after the user
; has disabled it (a disabled addon's OnInit never fires).
_ChatMod_BeforeLaunch(gameDir) {
    if (gameDir = "")
        return
    _ChatMod_SyncGate()
}

; ── Settings tab ────────────────────────────────────────────────

_ChatMod_OnSettingsWeb() {
    global _CHATMOD_FACES, _ChatMod_LastRejection
    fields := []

    ; Report a value the last save refused, once, at the top where it is read.
    if (_ChatMod_LastRejection != "") {
        fields.Push(Map("type", "info", "text", _ChatMod_LastRejection))
        _ChatMod_LastRejection := ""
    }

    if (_ChatMod_IniPath() = "") {
        fields.Push(Map("type", "info", "text",
            "Set the game path under Launcher first. These settings live in the "
            . "config.ini next to main.exe, which is the file the chat mod reads, "
            . "so there is nowhere to save them until Maps++ knows where the game is."))
        return fields
    }

    s := _ChatMod_ReadSettings()

    ; Offer the characters running now, so the usual case is a pick not a typo.
    running := []
    for snap in GetClientSnapshots() {
        if (snap.charName != "")
            running.Push(snap.charName)
    }

    stampFormats := ["#c808080[%H:%M]#N", "#c808080%H:%M#N", "%H:%M",
        "#c808080[%H:%M:%S]#N"]
    tabSets := ["All:*|Chat:*,-system,-info|System:system,info",
        "All:*|Chat:*,-system,-info,-market|Trade:market|System:system,info",
        "All:*"]
    cooldownWordings := ["#R#S Wait %d more second%s before posting in %s again!#N",
        "#R Wait %d more second%s (%s)#N"]

    fields.Push(Map("type", "info", "text",
        "Improves the game's own chat window: a readable font, channel tabs, "
        . "mention highlighting, timestamps and wheel scrolling.`n`n"
        . "Changes apply within about two seconds — nothing to restart. Turning "
        . "this addon off under Addons leaves the chat completely stock."))

    fields.Push(Map("type", "combo", "id", "FontFace", "label", "Chat font:",
        "options", _CHATMOD_FACES,
        "value", s["FontFace"],
        "default", _ChatMod_ReadDefault("FontFace")))
    fields.Push(Map("type", "number", "id", "FontSize",
        "label", "Font size (0 = the client's own):",
        "value", _ChatMod_AsInt(s["FontSize"], 0), "min", 0, "max", 18,
        "default", _ChatMod_AsInt(_ChatMod_ReadDefault("FontSize"), 0)))
    fields.Push(Map("type", "number", "id", "FontWidth",
        "label", "Character width (-1 fits the font):",
        "value", _ChatMod_AsInt(s["FontWidth"], -1), "min", -1, "max", 64,
        "default", _ChatMod_AsInt(_ChatMod_ReadDefault("FontWidth"), -1)))
    fields.Push(Map("type", "info", "text",
        "The client puts two ceilings on this, both measured against the real "
        . "renderer.`n`n"
        . "18 is the hard ceiling: above it the bottom of every line is cut, "
        . "underscores first, then the tails of y, g and q. Nothing avoids it.`n`n"
        . "And it breaks lines assuming every character is 8 pixels wide and "
        . "never re-checks, so a wide face runs off the right edge unless the "
        . "character width is pinned to 8.`n`n"
        . "Known good: Consolas at 18, or Cascadia Mono at 18 for a wider look. "
        . "Leave the font blank for the client's own Courier New."))

    fields.Push(Map("type", "combo", "id", "Tabs", "label", "Tab set:",
        "options", tabSets,
        "value", s["Tabs"],
        "default", _ChatMod_ReadDefault("Tabs")))
    fields.Push(Map("type", "checkbox", "id", "TabStrip",
        "label", "Show the clickable tab strip in the chat window",
        "value", _ChatMod_AsBool("TabStrip", s["TabStrip"]),
        "default", _ChatMod_AsBool("TabStrip", _ChatMod_ReadDefault("TabStrip"))))
    fields.Push(Map("type", "info", "text",
        "A tab is name:channels, tabs are separated by | and channels by "
        . "commas. * is every channel and a leading - removes one, so "
        . "*,-system reads as everything except system. Up to 6 tabs.`n`n"
        . "Channels: local, party, pm, guild, market, world, system, info, fd, gm."))

    fields.Push(Map("type", "checkbox", "id", "Timestamps",
        "label", "Show the time each message arrived",
        "value", _ChatMod_AsBool("Timestamps", s["Timestamps"]),
        "default", _ChatMod_AsBool("Timestamps", _ChatMod_ReadDefault("Timestamps"))))
    fields.Push(Map("type", "combo", "id", "TimestampFormat",
        "label", "Timestamp format:",
        "options", stampFormats,
        "value", s["TimestampFormat"],
        "default", _ChatMod_ReadDefault("TimestampFormat")))
    fields.Push(Map("type", "info", "text",
        "%H hours, %M minutes, %S seconds. The stamp goes at the start of the "
        . "line, before the sender's name, so every channel lines up the same "
        . "way. #cRRGGBB sets a colour and #N returns to normal — colour only, "
        . "the client's markup has no size code.`n`n"
        . "Messages already in the chat when the client started have no observed "
        . "time and stay unstamped rather than being given a made-up one."))

    fields.Push(Map("type", "checkbox", "id", "WheelScroll",
        "label", "Scroll the chat with the mouse wheel",
        "value", _ChatMod_AsBool("WheelScroll", s["WheelScroll"]),
        "default", _ChatMod_AsBool("WheelScroll", _ChatMod_ReadDefault("WheelScroll"))))
    fields.Push(Map("type", "number", "id", "WheelScrollLines",
        "label", "Lines per wheel notch (0 follows Windows):",
        "value", _ChatMod_AsInt(s["WheelScrollLines"], 0), "min", 0, "max", 30,
        "default", _ChatMod_AsInt(_ChatMod_ReadDefault("WheelScrollLines"), 0)))

    fields.Push(Map("type", "checkbox", "id", "CooldownMessage",
        "label", 'Say how many seconds are left instead of "Please do not flood !"',
        "value", _ChatMod_AsBool("CooldownMessage", s["CooldownMessage"]),
        "default", _ChatMod_AsBool("CooldownMessage", _ChatMod_ReadDefault("CooldownMessage"))))
    fields.Push(Map("type", "combo", "id", "CooldownFormat",
        "label", "Cooldown wording:",
        "options", cooldownWordings,
        "value", s["CooldownFormat"],
        "default", _ChatMod_ReadDefault("CooldownFormat")))
    fields.Push(Map("type", "info", "text",
        "The wording takes three placeholders and they must stay in this order: "
        . "%d the seconds left, %s the plural s, %s the channel name. It is "
        . "passed to a printf inside the game, so a different order is rejected "
        . "on save rather than sent.`n`n"
        . "The numbers come from the client's own gate, so this only reports "
        . "what the client already decided; the server enforces the cooldowns "
        . "either way."))

    fields.Push(Map("type", "checkbox", "id", "HighlightMentions",
        "label", "Mark messages that name me, and any PM",
        "value", _ChatMod_AsBool("HighlightMentions", s["HighlightMentions"]),
        "default", _ChatMod_AsBool("HighlightMentions", _ChatMod_ReadDefault("HighlightMentions"))))
    fields.Push(Map("type", "combo", "id", "MyName",
        "label", "My character name (blank = read from the game window):",
        "options", running,
        "value", s["MyName"],
        "default", _ChatMod_ReadDefault("MyName")))

    fields.Push(Map("type", "checkbox", "id", "FreePosition",
        "label", "Let me move the chat window freely",
        "value", _ChatMod_AsBool("FreePosition", s["FreePosition"]),
        "default", _ChatMod_AsBool("FreePosition", _ChatMod_ReadDefault("FreePosition"))))
    fields.Push(Map("type", "checkbox", "id", "UnlockSize",
        "label", "Let me resize the chat window",
        "value", _ChatMod_AsBool("UnlockSize", s["UnlockSize"]),
        "default", _ChatMod_AsBool("UnlockSize", _ChatMod_ReadDefault("UnlockSize"))))
    fields.Push(Map("type", "checkbox", "id", "RememberGeometry",
        "label", "Remember where I put it",
        "value", _ChatMod_AsBool("RememberGeometry", s["RememberGeometry"]),
        "default", _ChatMod_AsBool("RememberGeometry", _ChatMod_ReadDefault("RememberGeometry"))))

    fields.Push(Map("type", "combo", "id", "ClearHotkey",
        "label", "Hotkey to clear the chat:",
        "options", ["", "CTRL+SHIFT+C", "CTRL+DELETE", "ALT+F1"],
        "value", s["ClearHotkey"],
        "default", _ChatMod_ReadDefault("ClearHotkey")))
    fields.Push(Map("type", "combo", "id", "TabHotkey",
        "label", "Hotkey to cycle tabs:",
        "options", ["", "CTRL+SHIFT+X", "CTRL+TAB", "ALT+F2"],
        "value", s["TabHotkey"],
        "default", _ChatMod_ReadDefault("TabHotkey")))
    fields.Push(Map("type", "info", "text",
        "These two are registered by the chat mod itself, not through Hotkeys, "
        . "and they are system-wide — taken from every application, not just the "
        . "game. Leave them blank for none, and do not bind the same chord under "
        . "Hotkeys as well or one of the two will silently fail to register."))

    return fields
}

_ChatMod_OnSettingsWebSave(values) {
    global _ChatMod_LastRejection
    if !IsObject(values)
        return
    problem := _ChatMod_ValidateCooldown(values)
    if (problem != "") {
        ; Drop just the bad key; everything else the user changed still saves.
        values.Delete("CooldownFormat")
        _ChatMod_LastRejection := problem
        LogWarn("ChatMod", "Rejected CooldownFormat: " problem)
        TrayTip(problem, "Chat Mod", "Iconx")
    }
    _ChatMod_WriteSettings(values)
    _ChatMod_SyncGate()
}

; The wording string is handed to a sprintf inside the game process, so the
; specifier list has to match the argument list the DLL passes: an int, then
; two strings. Wrong order is a crash, not a cosmetic problem, which is why
; this is rejected rather than warned about.
_ChatMod_ValidateCooldown(values) {
    if !values.Has("CooldownFormat")
        return ""
    found := ""
    pos := 1
    while (pos := RegExMatch(values["CooldownFormat"], "%[a-zA-Z]", &m, pos)) {
        found .= m[0]
        pos += m.Len[0]
    }
    if (found = "%d%s%s")
        return ""
    if (found = "")
        return "The cooldown wording needs placeholders, so it was left unchanged.`n`nUse %d for the seconds, then %s for the plural, then %s for the channel."
    return "The cooldown wording must use %d, then %s, then %s in that order, so it was left unchanged.`n`nFound: " found
}
