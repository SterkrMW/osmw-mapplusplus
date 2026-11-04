#Requires AutoHotkey v2.0

; Settings for osmw_chat.dll — the client mods' chat window feature (font, free
; placement, channel tabs, mentions, timestamps, wheel scroll, cooldown text,
; client-level name blocking).
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
;   1. The ini is the mods' own osmw.ini beside main.exe, reached only through
;      native_mods.ahk, never Maps++'s CONFIG_INI. They are separate products
;      with separate files: Maps++'s config.ini can even end up in %AppData%
;      when the game folder is not writable, and the DLL cannot follow it
;      there. addons\auto_login.ahk makes the same distinction for the roster.
;   2. Writes are per key. [Chat] also holds WinX/WinY/WinW/WinH, which the DLL
;      itself writes whenever the player finishes dragging or resizing the chat
;      window, plus chip/markup keys this tab never shows. Rewriting the
;      section would destroy the player's saved geometry and race the DLL for
;      the file.
;   3. Maps++ does not install, deploy or launch the mod, and never writes
;      osmw.ini on its own account. The tab is a convenience editor over a
;      file the mods own; when they are not installed it says so and offers
;      nothing. Every write here is a user pressing Save.
;
; [Addons] ChatMod is what decides whether the mod does anything. The tab shows
; it as a checkbox, so turning it off is a deliberate act rather than a side
; effect of disabling this addon in Maps++.

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
    Map("key", "RememberActiveTab", "kind", "bool"),
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
global _CHATMOD_DEFAULT_ON := ["TabStrip", "RememberActiveTab", "WheelScroll",
    "CooldownMessage", "HighlightMentions", "RememberGeometry"]

; Measured against the real renderer by the server repo's
; tools/client-mods/chatmod/tools/fontpresets.py;
; keep the two in step, and treat the tool as the source of truth because it
; measures rather than assumes.
global _CHATMOD_FACES := ["", "Consolas", "Cascadia Mono", "Courier New",
    "Lucida Console"]

; The four combinations that tool found and verified in game, mirroring its
; PRESETS table. Free-typed values are still allowed through the three fields
; below, but the client puts two ceilings on them that no setting can lift, so
; the tested set is what the panel offers first.
;
; Change fontpresets.py first and then this: it measures against the real
; renderer, while this is only a copy of what it found.
global _CHATMOD_FONT_PRESETS := [
    Map("label", "Default — the client's own Courier New",
        "face", "",              "size", 0,  "width", -1),
    Map("label", "Crisp — Consolas 16, stock size with cleaner glyphs",
        "face", "Consolas",      "size", 16, "width", -1),
    Map("label", "Large — Consolas 18, the largest drawn in full",
        "face", "Consolas",      "size", 18, "width", -1),
    Map("label", "Wide — Cascadia Mono 18, same size, wider face",
        "face", "Cascadia Mono", "size", 18, "width", -1),
    Map("label", "Custom — whatever the three fields below say",
        "face", "",              "size", 0,  "width", -1)   ; sentinel
]

; The Custom sentinel is always the last row. 0-based, as the field sends it.
_ChatMod_CustomPresetIndex() {
    global _CHATMOD_FONT_PRESETS
    return _CHATMOD_FONT_PRESETS.Length - 1
}

; 0-based index of the preset matching these three values, or -1 for none. The
; Custom sentinel is skipped: it matches everything on paper and nothing in
; meaning.
_ChatMod_PresetIndex(face, size, width) {
    global _CHATMOD_FONT_PRESETS
    wantFace := Trim(String(face))
    for i, p in _CHATMOD_FONT_PRESETS {
        if (i = _CHATMOD_FONT_PRESETS.Length)
            break
        if (p["face"] = wantFace && p["size"] = size && p["width"] = width)
            return i - 1
    }
    return -1
}

; The preset index _ChatMod_OnSettingsWeb last rendered.
;
; It is what lets the dropdown and the three font fields coexist without either
; silently destroying the other: on save the preset only wins if the user
; actually moved it. Without this, typing a font size while the dropdown
; happened to be sitting on "Large" would have the preset overwrite that number
; the moment Save was pressed.
;
; Safe because settings.ahk calls OnSettingsWeb exactly once per window open,
; and ResetAllSettingsToDefaults calls it immediately before the paired
; OnSettingsWebSave — a pairing that file documents and already depends on for
; Window Layout. A change that batched those calls would break this.
global _ChatMod_RenderedPreset := -1

; Set when a save drops a value, shown as an info field the next time the tab
; renders and cleared once read. OnSettingsWebSave runs inside the settings
; page's WebMessageReceived callback, where a blocking dialog can never be
; answered — so a rejection is reported this way and by TrayTip, never by
; ShowMessage. Broadcast reports its hotkey clashes through the same route.
global _ChatMod_LastRejection := ""

; A position-reset request is outstanding until this tick, or 0 for none. The
; DLL acknowledges by writing the key back to 0; see
; _ChatMod_RequestPositionReset.
global _ChatMod_ResetDeadline := 0
global _CHATMOD_RESET_POLL_MS := 400
; The mod re-reads its settings about every 2s, so allow three of those before
; deciding nothing heard.
global _CHATMOD_RESET_WAIT_MS := 6500

RegisterAddon(Map(
    "name",              "ChatMod",
    "settingsLabel", "Chat mod",
    "settingsGroup", "tools",
    "settingsIcon",  "chat",
    "description",   "The client mods' in-game chat window: font, tabs, timestamps, blocked names.",
    ; These settings are the mods' own (osmw.ini). Settings hands this addon
    ; only the keys the user changed and Reset to Defaults skips it, so a save
    ; elsewhere in the window can never undo an edit made in the game's F12
    ; panel. See the field contract at the end of settings.ahk.
    "nativeMods",        true,
    "OnInit",            _ChatMod_OnInit,
    "OnSettingsWeb",     _ChatMod_OnSettingsWeb,
    "OnSettingsWebSave", _ChatMod_OnSettingsWebSave,
    "OnSettingsAction",  _ChatMod_OnSettingsAction
))

_ChatMod_OnInit() {
    ; Deliberately empty. Enabling or disabling this addon changes whether
    ; Maps++ shows the tab, and nothing about the mod itself.
}

; ── The ini ─────────────────────────────────────────────────────

; The mods' own settings, beside the client. See native_mods.ahk for the whole
; contract; both return "" when no game path is configured yet.
_ChatMod_IniPath() {
    return NativeModsIniPath()
}

_ChatMod_DefaultsPath() {
    return NativeModsDefaultsPath()
}

_ChatMod_ReadDefault(keyName) {
    defPath := _ChatMod_DefaultsPath()
    if (defPath = "")
        return ""
    return IniRead(defPath, "Chat", keyName, "")
}

; osmw.ini → osmw_defaults.ini → "", the chain the DLL itself uses — which is
; exactly NativeModsRead. Reading the shipped defaults means a fresh install
; shows what the DLL will actually do, not a guess. cfgPath is kept for the
; callers' sake; NativeModsRead resolves the same file.
_ChatMod_ReadKey(cfgPath, keyName) {
    return NativeModsRead("Chat", keyName, "")
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

; Writes only the keys the caller supplied, one NativeModsWrite each. Never IniDelete,
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

    ; One NativeModsWrite per key: that is the contract's one-key-per-write
    ; rule, and it is where the encoding and not-installed checks live.
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
        if NativeModsWrite("Chat", keyName, outVal)
            written++
    }
    LogInfo("ChatMod", "Wrote " written " [Chat] key(s) to " cfgPath)
    return written
}

; ── Tab sets ────────────────────────────────────────────────────
;
; The Tabs setting is a small language -- "All:*|Chat:*,-system,-info" -- and
; asking an international audience to type it was never reasonable. These are
; the parse and format halves that let an editor work in tabs and tick-boxes
; and hand back a string the DLL will read the same way.
;
; Every constant here mirrors one in the server repo's
; tools/client-mods/chatmod/src/tabs.c. They are the
; client's limits, not ours, and exceeding one is silent: a seventh tab is
; dropped, a sixteenth character of a name is cut, and a spec past the config
; buffer is truncated mid-channel.

global _CHATMOD_CHANNELS := ["local", "party", "pm", "guild", "market",
    "world", "system", "info", "fd", "gm"]      ; CHANNEL_NAMES, tabs.c:104
global _CHATMOD_MASK_ALL := 0x3FF               ; MASK_ALL, all ten bits
global _CHATMOD_MAX_TABS := 6                   ; MAX_TABS, tabs.c:90
global _CHATMOD_NAME_MAX := 15                  ; TAB_NAME_MAX 16, less the NUL
; g_tab_spec[256] in tabs.c, and mw_cfg_str's buffer is 256 to match. Both are
; sized in bytes and truncate silently, so the editor refuses instead.
global _CHATMOD_SPEC_MAX := 255

; BlockedNames: 20-char names, 32 of them, comma-separated. Matches block.c.
global _CHATMOD_BLOCK_CAP := 32
global _CHATMOD_BLOCK_NAME_MAX := 20

global _CHATMOD_DEFAULT_TABS := "All:*|Chat:*,-system,-info|System:system,info"

; Ready-made layouts the editor offers as a starting point.
_ChatMod_TabLayouts() {
    global _CHATMOD_DEFAULT_TABS
    return [
        Map("label", "Default — All / Chat / System",
            "spec", _CHATMOD_DEFAULT_TABS),
        Map("label", "With a Trade tab",
            "spec", "All:*|Chat:*,-system,-info,-market|Trade:market|System:system,info"),
        Map("label", "PMs on their own tab",
            "spec", "All:*|Chat:*,-system,-info,-pm|PMs:pm|System:system,info"),
        Map("label", "Everything in one tab", "spec", "All:*"),
        Map("label", "Split by channel",
            "spec", "All:*|Party:party,pm|Guild:guild|Trade:market|System:system,info")
    ]
}

; A faithful port of parse_tabs (tabs.c:129). It has to be faithful: an editor
; that disagreed with the DLL about what a spec means would show the user a set
; of tabs the game does not draw.
;
; Returns Map("rows", [Map("name", ..., "mask", ...)], "dropped", [...]).
_ChatMod_ParseTabSpec(spec) {
    global _CHATMOD_CHANNELS, _CHATMOD_MASK_ALL, _CHATMOD_MAX_TABS
    rows := []
    dropped := []

    for tabText in StrSplit(String(spec), "|") {
        if (rows.Length >= _CHATMOD_MAX_TABS) {
            trimmed := Trim(tabText)
            if (trimmed != "")
                dropped.Push("tab " trimmed " (past the limit of "
                    . _CHATMOD_MAX_TABS ")")
            continue
        }
        colon := InStr(tabText, ":")
        if (colon < 2)
            continue                    ; no name, or no channel list
        name := Trim(SubStr(tabText, 1, colon - 1))
        if (name = "")
            continue

        mask := 0
        for item in StrSplit(SubStr(tabText, colon + 1), ",") {
            token := Trim(item)
            remove := false
            if (SubStr(token, 1, 1) = "-") {
                remove := true
                token := SubStr(token, 2)
            }
            if (token = "*") {
                ; Order matters: "*,-system" is everything, then minus system.
                mask := remove ? 0 : _CHATMOD_MASK_ALL
                continue
            }
            bit := 0
            for i, ch in _CHATMOD_CHANNELS {
                if (ch = token) {
                    bit := 1 << (i - 1)
                    break
                }
            }
            if (bit = 0) {
                if (token != "")
                    dropped.Push('channel "' token '"')
                continue
            }
            mask := remove ? (mask & ~bit) : (mask | bit)
        }
        rows.Push(Map("name", name, "mask", mask))
    }
    return Map("rows", rows, "dropped", dropped)
}

; The inverse, written to stay short rather than literal.
;
; Compactness is a correctness requirement, not a nicety: the whole spec has to
; fit _CHATMOD_SPEC_MAX bytes, and spelling ten channels out per tab blows past
; that on the third tab. Writing "*" and "*,-system,-info" also reproduces the
; shipped default byte for byte, so opening the editor on a stock install and
; pressing OK rewrites nothing.
_ChatMod_FormatTabSpec(rows) {
    global _CHATMOD_CHANNELS, _CHATMOD_MASK_ALL
    parts := []
    for row in rows {
        mask := row["mask"] & _CHATMOD_MASK_ALL
        present := []
        absent := []
        for i, ch in _CHATMOD_CHANNELS {
            if (mask & (1 << (i - 1)))
                present.Push(ch)
            else
                absent.Push(ch)
        }
        if (present.Length = 0)
            continue                    ; a tab showing nothing is not a tab
        if (absent.Length = 0)
            channels := "*"
        else if (absent.Length < present.Length) {
            channels := "*"
            for ch in absent
                channels .= ",-" ch
        } else {
            channels := ""
            for ch in present
                channels .= (channels = "" ? "" : ",") ch
        }
        parts.Push(row["name"] ":" channels)
    }

    out := ""
    for part in parts
        out .= (out = "" ? "" : "|") part
    return out
}

; "" when the rows are usable, otherwise the reason they are not.
_ChatMod_ValidateTabRows(rows) {
    global _CHATMOD_NAME_MAX, _CHATMOD_MAX_TABS, _CHATMOD_SPEC_MAX
    if (rows.Length = 0)
        return "Add at least one tab."
    if (rows.Length > _CHATMOD_MAX_TABS)
        return "The chat window can show at most " _CHATMOD_MAX_TABS " tabs."
    for i, row in rows {
        name := Trim(row["name"])
        if (name = "")
            return "Tab " i " has no name."
        if (StrLen(name) > _CHATMOD_NAME_MAX)
            return "Tab names can be at most " _CHATMOD_NAME_MAX
                . " characters. " name " is longer, and the chat window would cut it."
        if RegExMatch(name, "[:|,]")
            return "A tab name cannot contain : | or , — those separate the "
                . "parts of the setting. Rename " name "."
        if ((row["mask"] & 0x3FF) = 0)
            return "Tab " name " has no channels ticked, so it would show nothing."
    }
    spec := _ChatMod_FormatTabSpec(rows)
    if (StrLen(spec) > _CHATMOD_SPEC_MAX)
        return "That is " StrLen(spec) " characters and the chat mod reads at "
            . "most " _CHATMOD_SPEC_MAX ". Use fewer tabs, or shorter names."
    return ""
}

; ── Settings tab ────────────────────────────────────────────────

_ChatMod_OnSettingsWeb() {
    global _CHATMOD_FACES, _ChatMod_LastRejection
    global _CHATMOD_FONT_PRESETS, _ChatMod_RenderedPreset
    fields := []

    ; Report a value the last save refused, once, at the top where it is read.
    if (_ChatMod_LastRejection != "") {
        fields.Push(Map("type", "info", "text", _ChatMod_LastRejection))
        _ChatMod_LastRejection := ""
    }

    if (_ChatMod_IniPath() = "") {
        fields.Push(Map("type", "info", "text",
            "Set the game path under Game & startup first. These settings live in the "
            . "osmw.ini next to main.exe, which is the file the chat mod reads, "
            . "so there is nowhere to save them until Maps++ knows where the game is."))
        return fields
    }

    if !NativeModsInstalled() {
        fields.Push(Map("type", "info", "text",
            "The osMW client mods are not installed in this game folder "
            . "(no osmw_core.dll), so there is nothing for these settings to "
            . "configure. Maps++ works either way; it does not install them."))
        return fields
    }

    ; The mod's own gate, shown rather than synced: the in-game panel and a
    ; text editor can both change it, and Maps++ must not quietly win. It is
    ; [Addons] ChatMod in osmw.ini, which is a different file and a different
    ; product from the ChatMod row in Maps++'s own Addons tab.
    fields.Push(Map("type", "checkbox", "id", "__Gate",
        "label", "Enable the chat mod in game",
        "value", NativeModsRead("Addons", "ChatMod", "1") != "0",
        "default", true))

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
        . "Changes apply within about two seconds — nothing to restart. These "
        . "settings belong to the game's client mods (osmw.ini), and the in-game "
        . "F12 panel edits the same ones. Turning this addon off under Addons only "
        . "hides this tab; the switch below is what turns the chat mod itself off."))

    ; The preset picker sits above the three fields it drives. Both are shown
    ; at once on purpose: neither frontend can hide a field (native lays its
    ; column out with absolute y at build time), and seeing Consolas / 18 / -1
    ; appear in the boxes is the clearest possible explanation of what a preset
    ; actually is.
    presetLabels := []
    for p in _CHATMOD_FONT_PRESETS
        presetLabels.Push(p["label"])
    curPreset := _ChatMod_PresetIndex(s["FontFace"],
        _ChatMod_AsInt(s["FontSize"], 0), _ChatMod_AsInt(s["FontWidth"], -1))
    if (curPreset < 0)
        curPreset := _ChatMod_CustomPresetIndex()
    _ChatMod_RenderedPreset := curPreset

    defPreset := _ChatMod_PresetIndex(_ChatMod_ReadDefault("FontFace"),
        _ChatMod_AsInt(_ChatMod_ReadDefault("FontSize"), 0),
        _ChatMod_AsInt(_ChatMod_ReadDefault("FontWidth"), -1))
    if (defPreset < 0)
        defPreset := _ChatMod_CustomPresetIndex()

    fields.Push(Map("type", "dropdown", "id", "FontPreset",
        "label", "Font preset:",
        "options", presetLabels,
        "value", curPreset,
        "default", defPreset))
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
        "Pick a preset, or set the three fields yourself and leave the preset "
        . "on Custom. Moving the preset overwrites all three; changing the "
        . "three by hand leaves the preset alone.`n`n"
        . "The client puts two ceilings on this, both measured against the real "
        . "renderer, and they are why the presets stop where they do.`n`n"
        . "18 is the hard ceiling: above it the bottom of every line is cut, "
        . "underscores first, then the tails of y, g and q. Nothing avoids it.`n`n"
        . "And it breaks lines assuming every character is 8 pixels wide and "
        . "never re-checks, so a wide face runs off the right edge unless the "
        . "character width is pinned to 8.`n`n"
        . "Leave the font blank for the client's own Courier New."))

    fields.Push(Map("type", "combo", "id", "Tabs", "label", "Tab set:",
        "options", tabSets,
        "value", s["Tabs"],
        "default", _ChatMod_ReadDefault("Tabs")))
    fields.Push(Map("type", "checkbox", "id", "TabStrip",
        "label", "Show the clickable tab strip in the chat window",
        "value", _ChatMod_AsBool("TabStrip", s["TabStrip"]),
        "default", _ChatMod_AsBool("TabStrip", _ChatMod_ReadDefault("TabStrip"))))
    fields.Push(Map("type", "checkbox", "id", "RememberActiveTab",
        "label", "Remember the active tab between character logins",
        "value", _ChatMod_AsBool("RememberActiveTab", s["RememberActiveTab"]),
        "default", _ChatMod_AsBool("RememberActiveTab",
            _ChatMod_ReadDefault("RememberActiveTab"))))
    fields.Push(Map("type", "button", "id", "editTabs",
        "label", "Edit tabs…",
        "hint", "Name each tab and tick the channels it shows, with no syntax "
              . "to learn. It starts from the saved tab set, and what it "
              . "produces lands in the box above — press Save to keep it."))
    fields.Push(Map("type", "info", "text",
        "The box above is the same thing written out. A tab is name:channels, "
        . "tabs are separated by | and channels by commas. * is every channel "
        . "and a leading - removes one, so *,-system reads as everything "
        . "except system. Up to 6 tabs, names up to 15 characters.`n`n"
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
    fields.Push(Map("type", "button", "id", "editBlock",
        "label", "Edit blocked names…",
        "hint", "Hide a character's chat on this PC, on every character using "
              . "this game folder. Ctrl+click a sender name in the chat window "
              . "also adds them. Unblock is here. It writes immediately — it "
              . "does not wait for Save."))
    fields.Push(Map("type", "info", "text",
        "Blocked names never appear in the chat window or the in-game overlay. "
        . "Messages already on the overlay age out of the client's history; "
        . "new ones are dropped as they arrive. System and Info announcements "
        . "are never hidden, even if the name matches. The list is shared by "
        . "every client using this game folder."))

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
    fields.Push(Map("type", "button", "id", "resetPosition",
        "label", "Reset chat window position",
        "hint", "Forgets where the chat window was put and docks it against "
              . "its own client again. It acts on the client at the game path "
              . "set under Game & startup, does not wait for Save, and reports back "
              . "once a running client has done it."))
    fields.Push(Map("type", "info", "text",
        "The position is remembered relative to the game window, so each "
        . "client's chat window comes back beside its own client rather than "
        . "on top of the others. With nothing remembered, the chat window "
        . "docks against the right edge of its client the way it does with no "
        . "mod at all."))

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
    _ChatMod_ApplyFontPreset(values)
    problem := _ChatMod_ValidateCooldown(values)
    if (problem != "") {
        ; Drop just the bad key; everything else the user changed still saves.
        values.Delete("CooldownFormat")
        _ChatMod_LastRejection := problem
        LogWarn("ChatMod", "Rejected CooldownFormat: " problem)
        TrayTip(problem, "Chat Mod", "Iconx")
    }
    if values.Has("__Gate") {
        NativeModsWrite("Addons", "ChatMod", values["__Gate"] ? "1" : "0")
        values.Delete("__Gate")
    }
    _ChatMod_WriteSettings(values)
}

; ── Tab editor ──────────────────────────────────────────────────
;
; The editor works in tabs and tick-boxes and never shows the user the spec
; language. It reads the SAVED tab set rather than whatever is currently typed
; in the panel's Tab set box, and hands its result back to the panel as a draft
; value — so the settings window's own Save writes it, and its Cancel discards
; it, exactly as if the string had been typed by hand.
;
; It blocks in WinWaitClose. That is safe because settings.ahk runs addon
; actions outside the WebView message callback; it would hang if it were ever
; called from inside one. The settings window is disabled for the duration for
; the same reason dialogs.ahk disables its owner: a panel that keeps handling
; its own messages behind a modal window can have its Save pressed while the
; modal is up.
;
; Returns Map("Tabs", spec) on OK, or "" on cancel.
_ChatMod_EditTabs() {
    global _CHATMOD_DEFAULT_TABS, gSettingsGui

    cfgPath := _ChatMod_IniPath()
    if (cfgPath = "") {
        TrayTip("Set the game path under Settings → Game & startup first.", "Chat mod", "Iconx")
        return ""
    }

    current := _ChatMod_ReadKey(cfgPath, "Tabs")
    if (Trim(current) = "")
        current := _CHATMOD_DEFAULT_TABS

    parsed := _ChatMod_ParseTabSpec(current)
    rows := parsed["rows"]
    notice := _ChatMod_TabNotice(current, rows, parsed["dropped"])

    ; Disabled, not just owned: the settings page behind this keeps pumping its
    ; own WebView messages, so without this its Save is still clickable.
    owner := (IsObject(gSettingsGui) && gSettingsGui.Hwnd) ? gSettingsGui.Hwnd : 0
    if (owner)
        try DllCall("EnableWindow", "Ptr", owner, "Int", 0)

    spec := ""
    try {
        if (IsNativeInterface() || !_ChatMod_TabsCanUseWebView())
            spec := _ChatMod_EditTabsNative(rows, notice)
        else
            spec := _ChatMod_EditTabsWeb(rows, notice)
    } finally {
        ; Not optional: a throw that left the settings window disabled would
        ; look exactly like the whole app having frozen.
        if (owner && WinExist("ahk_id " owner))
            try DllCall("EnableWindow", "Ptr", owner, "Int", 1)
    }

    if (spec = "")
        return ""
    LogInfo("ChatMod", "Tab editor produced: " spec)
    return Map("Tabs", spec)
}

; What the editor cannot promise to leave untouched, said plainly up front.
_ChatMod_TabNotice(current, rows, dropped) {
    notice := ""
    if (dropped.Length > 0) {
        list := ""
        for d in dropped
            list .= (list = "" ? "" : ", ") d
        notice := "The chat mod already ignores " list " in the saved tab set."
    }
    reformatted := _ChatMod_FormatTabSpec(rows)
    if (reformatted != Trim(current)) {
        notice .= (notice = "" ? "" : "  ")
            . "Saving will write this the short way: " reformatted
    }
    return notice
}

_ChatMod_TabsCanUseWebView() {
    return FileExist(A_ScriptDir "\Lib\WebViewToo.ahk")
        && FileExist(A_ScriptDir "\ui\chat_tabs\index.html")
}

; Rows as the page wants them: a name and ten booleans, never a mask and never
; the spec language.
_ChatMod_RowsToJson(rows) {
    global _CHATMOD_CHANNELS
    out := "["
    for i, row in rows {
        if (i > 1)
            out .= ","
        out .= '{"name":' _JSON_Str(row["name"]) ',"channels":['
        for c, _ in _CHATMOD_CHANNELS
            out .= (c > 1 ? "," : "")
                . ((row["mask"] & (1 << (c - 1))) ? "true" : "false")
        out .= "]}"
    }
    return out "]"
}

; And back. Anything malformed becomes no row rather than an exception: this is
; parsing a message, and the validator downstream is what judges it.
_ChatMod_RowsFromMsg(msg) {
    global _CHATMOD_CHANNELS
    rows := []
    if (!IsObject(msg) || !msg.Has("rows") || !IsObject(msg["rows"]))
        return rows
    for entry in msg["rows"] {
        if !IsObject(entry)
            continue
        name := entry.Has("name") ? Trim(String(entry["name"])) : ""
        mask := 0
        if (entry.Has("channels") && IsObject(entry["channels"])) {
            for c, on in entry["channels"] {
                if (c > _CHATMOD_CHANNELS.Length)
                    break
                if (on = true || on = 1)
                    mask |= 1 << (c - 1)
            }
        }
        if (name = "" && mask = 0)
            continue                    ; an untouched blank row is not a tab
        rows.Push(Map("name", name, "mask", mask))
    }
    return rows
}

_ChatMod_PostMsg(dlg, json) {
    if !IsObject(dlg)
        return
    try dlg.PostWebMessageAsJson(json)
}

; The live readout the page shows under the rows: the spec the current rows
; would produce, its length against the cap, and why it is not acceptable yet.
; Computed here so the page never implements the formatting rules — one
; formatter, and it is the one the OK button uses.
_ChatMod_TabPreviewJson(rows) {
    global _CHATMOD_SPEC_MAX
    spec := _ChatMod_FormatTabSpec(rows)
    problem := _ChatMod_ValidateTabRows(rows)
    return '{"type":"tabs-preview","spec":' _JSON_Str(spec)
        . ',"length":' StrLen(spec)
        . ',"max":' _CHATMOD_SPEC_MAX
        . ',"error":' _JSON_Str(problem) '}'
}

_ChatMod_EditTabsWeb(rows, notice) {
    global _CHATMOD_CHANNELS, _CHATMOD_MAX_TABS, _CHATMOD_NAME_MAX

    result := ""
    dllDir := (A_PtrSize = 8) ? "64bit" : "32bit"
    dllPath := A_ScriptDir "\Lib\" dllDir "\WebView2Loader.dll"
    wvSettings := { DllPath: dllPath, DefaultWidth: 780, DefaultHeight: 620 }

    dlg := WebViewGui("-Caption +AlwaysOnTop -Resize", "Chat tabs",, wvSettings)
    ; This blocks in WinWaitClose below, so every way the window can go away
    ; has to destroy it — including Alt+F4, which the page never sees.
    dlg.OnEvent("Close", (*) => dlg.Destroy())

    channelsJson := "["
    for i, ch in _CHATMOD_CHANNELS
        channelsJson .= (i > 1 ? "," : "") _JSON_Str(ch)
    channelsJson .= "]"

    layoutsJson := "["
    for i, layout in _ChatMod_TabLayouts() {
        layoutsJson .= (i > 1 ? "," : "")
            . '{"label":' _JSON_Str(layout["label"])
            . ',"spec":' _JSON_Str(layout["spec"]) '}'
    }
    layoutsJson .= "]"

    initJson := '{"type":"init-tabs"'
        . ',"rows":' _ChatMod_RowsToJson(rows)
        . ',"channels":' channelsJson
        . ',"layouts":' layoutsJson
        . ',"maxTabs":' _CHATMOD_MAX_TABS
        . ',"nameMax":' _CHATMOD_NAME_MAX
        . ',"notice":' _JSON_Str(notice) '}'

    dlg.WebMessageReceived(WebMsgHandler(OnWebMsg))
    dlg.DOMContentLoaded((*) => _ChatMod_PostMsg(dlg, initJson))
    dlg.Navigate(UiPageUrl("ui/chat_tabs/index.html"))
    dlg.Show("w780 h620")

    OnWebMsg(wv, args) {
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
                _ChatMod_PostMsg(dlg, initJson)
            case "preview":
                ; Recomputed here on every edit, so the page never has to know
                ; how a spec is written or how long it is allowed to be.
                _ChatMod_PostMsg(dlg,
                    _ChatMod_TabPreviewJson(_ChatMod_RowsFromMsg(msg)))
            case "layout":
                idx := msg.Has("index") ? Integer(msg["index"]) : 0
                layouts := _ChatMod_TabLayouts()
                if (idx >= 1 && idx <= layouts.Length) {
                    picked := _ChatMod_ParseTabSpec(layouts[idx]["spec"])["rows"]
                    _ChatMod_PostMsg(dlg, '{"type":"set-rows","rows":'
                        . _ChatMod_RowsToJson(picked) '}')
                }
            case "accept":
                edited := _ChatMod_RowsFromMsg(msg)
                problem := _ChatMod_ValidateTabRows(edited)
                if (problem != "") {
                    ; Refuse and stay open — the page shows the reason.
                    _ChatMod_PostMsg(dlg, _ChatMod_TabPreviewJson(edited))
                    return
                }
                result := _ChatMod_FormatTabSpec(edited)
                dlg.Destroy()
            case "cancel":
                dlg.Destroy()
        }
    }

    WinWaitClose("ahk_id " dlg.Hwnd)
    return result
}

; The same editor as plain controls: a name box and ten tick-boxes per row.
; It drives the same parse/format/validate functions, so it cannot produce a
; different string from the same choices.
_ChatMod_EditTabsNative(rows, notice) {
    global _CHATMOD_CHANNELS, _CHATMOD_MAX_TABS, _CHATMOD_NAME_MAX

    result := ""
    colX := 130
    colW := 52

    g := Gui("+AlwaysOnTop -MinimizeBox", "Chat tabs")
    g.SetFont("s9", "Segoe UI")

    g.Add("Text", "x12 y12 w640",
        "Name each tab and tick the channels it shows. Up to "
        . _CHATMOD_MAX_TABS " tabs, names up to " _CHATMOD_NAME_MAX
        . " characters. Leave a row blank to drop that tab.")
    if (notice != "")
        g.Add("Text", "x12 y+6 w640", notice)

    headY := 62
    g.Add("Text", "x12 y" headY " w110", "Tab name")
    for i, ch in _CHATMOD_CHANNELS
        g.Add("Text", "x" (colX + (i - 1) * colW) " y" headY " w" colW, ch)

    nameEdits := []
    checkGrid := []
    Loop _CHATMOD_MAX_TABS {
        r := A_Index
        rowY := headY + 22 + (r - 1) * 28
        seedRow := rows.Has(r) ? rows[r] : 0
        nameBox := g.Add("Edit", "x12 y" rowY " w110 Limit" _CHATMOD_NAME_MAX,
            IsObject(seedRow) ? seedRow["name"] : "")
        nameBox.OnEvent("Change", (*) => RefreshPreview())
        nameEdits.Push(nameBox)

        boxes := []
        for chIndex, _ in _CHATMOD_CHANNELS {
            box := g.Add("CheckBox", "x" (colX + (chIndex - 1) * colW + 16)
                . " y" (rowY + 2) " w20 h20")
            box.Value := (IsObject(seedRow)
                && (seedRow["mask"] & (1 << (chIndex - 1)))) ? 1 : 0
            box.OnEvent("Click", (*) => RefreshPreview())
            boxes.Push(box)
        }
        checkGrid.Push(boxes)
    }

    footY := headY + 22 + _CHATMOD_MAX_TABS * 28 + 12
    g.Add("Text", "x12 y" (footY + 4) " w70", "Start from:")
    layouts := _ChatMod_TabLayouts()
    layoutLabels := []
    for layout in layouts
        layoutLabels.Push(layout["label"])
    layoutDdl := g.Add("DropDownList", "x86 y" footY " w330", layoutLabels)
    layoutDdl.Value := 1
    g.Add("Button", "x424 y" (footY - 1) " w70", "Use")
        .OnEvent("Click", (*) => ApplyLayout())

    previewText := g.Add("Text", "x12 y" (footY + 36) " w640 h34", "")

    g.Add("Button", "x472 y" (footY + 78) " w90 Default", "OK")
        .OnEvent("Click", (*) => Accept())
    g.Add("Button", "x+8 yp w90", "Cancel").OnEvent("Click", (*) => g.Destroy())
    g.OnEvent("Close", (*) => g.Destroy())
    g.OnEvent("Escape", (*) => g.Destroy())

    RefreshPreview()
    g.Show("w674 h" (footY + 118))
    WinWaitClose("ahk_id " g.Hwnd)
    return result

    ReadRows() {
        collected := []
        for rowIndex, box in nameEdits {
            rowName := Trim(box.Value)
            rowMask := 0
            for chIndex, check in checkGrid[rowIndex] {
                if (check.Value)
                    rowMask |= 1 << (chIndex - 1)
            }
            if (rowName = "" && rowMask = 0)
                continue
            collected.Push(Map("name", rowName, "mask", rowMask))
        }
        return collected
    }

    WriteRows(newRows) {
        for rowIndex, box in nameEdits {
            srcRow := newRows.Has(rowIndex) ? newRows[rowIndex] : 0
            box.Value := IsObject(srcRow) ? srcRow["name"] : ""
            for chIndex, check in checkGrid[rowIndex] {
                check.Value := (IsObject(srcRow)
                    && (srcRow["mask"] & (1 << (chIndex - 1)))) ? 1 : 0
            }
        }
        RefreshPreview()
    }

    ApplyLayout() {
        idx := layoutDdl.Value
        if (idx >= 1 && idx <= layouts.Length)
            WriteRows(_ChatMod_ParseTabSpec(layouts[idx]["spec"])["rows"])
    }

    RefreshPreview() {
        liveRows := ReadRows()
        liveProblem := _ChatMod_ValidateTabRows(liveRows)
        if (liveProblem != "") {
            previewText.Value := liveProblem
            return
        }
        stripText := ""
        for liveRow in liveRows
            stripText .= (stripText = "" ? "" : "  |  ") liveRow["name"]
        previewText.Value := "Tab strip:  " stripText
    }

    Accept() {
        edited := ReadRows()
        editProblem := _ChatMod_ValidateTabRows(edited)
        if (editProblem != "") {
            RefreshPreview()
            TrayTip(editProblem, "Chat Mod", "Iconx")
            return
        }
        result := _ChatMod_FormatTabSpec(edited)
        g.Destroy()
    }
}

; ── Blocked names ───────────────────────────────────────────────
;
; The list is not a settings-tab field: Ctrl+click in the game writes the same
; key, and a delayed Save would clobber it. OK writes BlockedNames immediately.
; Cancel discards. Empty list writes an empty value, never IniDelete.

_ChatMod_ParseBlocked(str) {
    global _CHATMOD_BLOCK_CAP, _CHATMOD_BLOCK_NAME_MAX
    names := []
    seen := Map()
    for part in StrSplit(String(str), ",") {
        n := Trim(part)
        if (n = "" || InStr(n, ",") || StrLen(n) > _CHATMOD_BLOCK_NAME_MAX)
            continue
        key := StrLower(n)
        if seen.Has(key)
            continue
        seen[key] := true
        names.Push(n)
        if (names.Length >= _CHATMOD_BLOCK_CAP)
            break
    }
    return names
}

_ChatMod_FormatBlocked(names) {
    out := ""
    if !IsObject(names)
        return out
    for n in names {
        t := Trim(String(n))
        if (t = "")
            continue
        out .= (out = "" ? "" : ",") t
    }
    return out
}

_ChatMod_ValidateBlocked(names) {
    global _CHATMOD_BLOCK_CAP, _CHATMOD_BLOCK_NAME_MAX
    if !IsObject(names)
        return "The name list was unreadable."
    if (names.Length > _CHATMOD_BLOCK_CAP)
        return "At most " _CHATMOD_BLOCK_CAP " names."
    seen := Map()
    for n in names {
        t := Trim(String(n))
        if (t = "")
            continue
        if InStr(t, ",")
            return "Names cannot contain commas."
        if (StrLen(t) > _CHATMOD_BLOCK_NAME_MAX)
            return "'" t "' is longer than " _CHATMOD_BLOCK_NAME_MAX " characters."
        key := StrLower(t)
        if seen.Has(key)
            return "'" t "' is listed twice."
        seen[key] := true
    }
    return ""
}

_ChatMod_NamesToJson(names) {
    out := "["
    for i, n in names
        out .= (i > 1 ? "," : "") _JSON_Str(n)
    return out "]"
}

_ChatMod_NamesFromMsg(msg) {
    names := []
    if (!IsObject(msg) || !msg.Has("names") || !IsObject(msg["names"]))
        return names
    for n in msg["names"]
        names.Push(Trim(String(n)))
    return names
}

_ChatMod_WriteBlocked(names) {
    cfgPath := _ChatMod_IniPath()
    if (cfgPath = "") {
        TrayTip("Set the game path under Settings → Game & startup first.", "Chat mod", "Iconx")
        return false
    }
    problem := _ChatMod_ValidateBlocked(names)
    if (problem != "") {
        TrayTip(problem, "Chat Mod", "Iconx")
        return false
    }
    if !NativeModsWrite("Chat", "BlockedNames", _ChatMod_FormatBlocked(names)) {
        TrayTip("Could not write to " cfgPath ".", "Chat Mod", "Iconx")
        return false
    }
    LogInfo("ChatMod", "Wrote BlockedNames (" names.Length " name(s)) to " cfgPath)
    return true
}

_ChatMod_BlockCanUseWebView() {
    return FileExist(A_ScriptDir "\Lib\WebViewToo.ahk")
        && FileExist(A_ScriptDir "\ui\chat_block\index.html")
}

_ChatMod_EditBlock() {
    global gSettingsGui

    cfgPath := _ChatMod_IniPath()
    if (cfgPath = "") {
        TrayTip("Set the game path under Settings → Game & startup first.", "Chat mod", "Iconx")
        return
    }

    names := _ChatMod_ParseBlocked(_ChatMod_ReadKey(cfgPath, "BlockedNames"))

    owner := (IsObject(gSettingsGui) && gSettingsGui.Hwnd) ? gSettingsGui.Hwnd : 0
    if (owner)
        try DllCall("EnableWindow", "Ptr", owner, "Int", 0)

    accepted := ""
    try {
        if (IsNativeInterface() || !_ChatMod_BlockCanUseWebView())
            accepted := _ChatMod_EditBlockNative(names)
        else
            accepted := _ChatMod_EditBlockWeb(names)
    } finally {
        if (owner && WinExist("ahk_id " owner))
            try DllCall("EnableWindow", "Ptr", owner, "Int", 1)
    }

    if !IsObject(accepted)
        return
    _ChatMod_WriteBlocked(accepted)
}

_ChatMod_EditBlockWeb(names) {
    global _CHATMOD_BLOCK_CAP, _CHATMOD_BLOCK_NAME_MAX

    result := ""
    dllDir := (A_PtrSize = 8) ? "64bit" : "32bit"
    dllPath := A_ScriptDir "\Lib\" dllDir "\WebView2Loader.dll"
    wvSettings := { DllPath: dllPath, DefaultWidth: 440, DefaultHeight: 520 }

    dlg := WebViewGui("-Caption +AlwaysOnTop -Resize", "Blocked names",, wvSettings)
    dlg.OnEvent("Close", (*) => dlg.Destroy())

    initJson := '{"type":"init-block"'
        . ',"names":' _ChatMod_NamesToJson(names)
        . ',"nameMax":' _CHATMOD_BLOCK_NAME_MAX
        . ',"maxNames":' _CHATMOD_BLOCK_CAP
        . ',"notice":""}'

    dlg.WebMessageReceived(WebMsgHandler(OnWebMsg))
    dlg.DOMContentLoaded((*) => _ChatMod_PostMsg(dlg, initJson))
    dlg.Navigate(UiPageUrl("ui/chat_block/index.html"))
    dlg.Show("w440 h520")

    OnWebMsg(wv, args) {
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
                _ChatMod_PostMsg(dlg, initJson)
            case "accept":
                edited := _ChatMod_NamesFromMsg(msg)
                problem := _ChatMod_ValidateBlocked(edited)
                if (problem != "") {
                    TrayTip(problem, "Chat Mod", "Iconx")
                    return
                }
                result := edited
                dlg.Destroy()
            case "cancel":
                dlg.Destroy()
        }
    }

    WinWaitClose("ahk_id " dlg.Hwnd)
    return result
}

_ChatMod_EditBlockNative(names) {
    global _CHATMOD_BLOCK_CAP, _CHATMOD_BLOCK_NAME_MAX

    result := ""
    live := []
    for n in names
        live.Push(n)

    g := Gui("+AlwaysOnTop -MinimizeBox", "Blocked names")
    g.SetFont("s9", "Segoe UI")
    g.Add("Text", "x12 y12 w320",
        "Names on this list are hidden in the chat window and the overlay, "
        . "on every character using this game folder. Ctrl+click a sender "
        . "name in chat also adds them. System and Info are never hidden.")

    lb := g.Add("ListBox", "x12 y72 w320 h220")
    nameEdit := g.Add("Edit", "x12 y300 w230 Limit" _CHATMOD_BLOCK_NAME_MAX)
    g.Add("Button", "x250 y298 w82", "Add").OnEvent("Click", (*) => AddName())
    g.Add("Button", "x12 y334 w82", "Remove").OnEvent("Click", (*) => RemoveName())
    budget := g.Add("Text", "x102 y338 w230", "")
    g.Add("Button", "x152 y372 w90 Default", "OK").OnEvent("Click", (*) => Accept())
    g.Add("Button", "x+8 yp w90", "Cancel").OnEvent("Click", (*) => g.Destroy())
    g.OnEvent("Close", (*) => g.Destroy())
    g.OnEvent("Escape", (*) => g.Destroy())

    Refresh()
    g.Show("w344 h416")
    WinWaitClose("ahk_id " g.Hwnd)
    return result

    Refresh() {
        lb.Delete()
        for n in live
            lb.Add([n])
        budget.Value := live.Length " of " _CHATMOD_BLOCK_CAP
    }

    AddName() {
        n := Trim(nameEdit.Value)
        if (n = "")
            return
        probe := live.Clone()
        probe.Push(n)
        problem := _ChatMod_ValidateBlocked(probe)
        if (problem != "") {
            TrayTip(problem, "Chat Mod", "Iconx")
            return
        }
        live.Push(n)
        nameEdit.Value := ""
        Refresh()
    }

    RemoveName() {
        sel := lb.Text
        if (sel = "")
            return
        for i, n in live {
            if (n = sel) {
                live.RemoveAt(i)
                break
            }
        }
        Refresh()
    }

    Accept() {
        problem := _ChatMod_ValidateBlocked(live)
        if (problem != "") {
            TrayTip(problem, "Chat Mod", "Iconx")
            return
        }
        result := live
        g.Destroy()
    }
}

; ── Settings buttons ────────────────────────────────────────────
;
; Reached from a "button" field, which settings.ahk runs outside the WebView
; message callback for us — so opening a window here is safe, and so is
; blocking in it.
;
; The return value is fields to put back into the panel's live draft, or "" for
; none. The tab editor uses it so its result is saved by the settings window's
; own Save: writing the ini here instead would mean OK-then-Cancel left a
; change applied, and a later Save would overwrite the new tabs with the string
; the panel had rendered with.
_ChatMod_OnSettingsAction(actionId) {
    if (actionId = "editTabs")
        return _ChatMod_EditTabs()
    if (actionId = "editBlock") {
        _ChatMod_EditBlock()
        return ""
    }
    if (actionId = "resetPosition") {
        _ChatMod_RequestPositionReset()
        return ""
    }
    return ""
}

; Asks the DLL to forget the saved chat window position and dock the window
; against its client again.
;
; A one-shot command rather than a setting, and deliberately NOT in
; _CHATMOD_SPEC: a spec entry would make it a field, and the tab would re-issue
; the command on every Save. It is also why Maps++ never clears WinRelX/WinRelY
; itself — the DLL owns those keys and this addon never IniDeletes in [Chat].
;
; The DLL writing the key back to 0 is an ACKNOWLEDGEMENT, and this watches for
; it rather than assuming. The first version simply wrote the key and announced
; success, which is wrong in three situations a user will actually hit: no
; client is running, the game path points at an install they are not looking
; at, or the deployed mod predates this command and cannot understand it. All
; three looked identical to "it worked", and the window not moving was the only
; clue. Waiting for the ack tells them which it was.
;
; It doubles as the button's cooldown: while a request is outstanding the
; button says so instead of queuing another write.
_ChatMod_RequestPositionReset() {
    global _ChatMod_ResetDeadline, _CHATMOD_RESET_WAIT_MS, _CHATMOD_RESET_POLL_MS

    if (_ChatMod_ResetDeadline != 0) {
        TrayTip("Already asked — still waiting for a client to pick it up.",
            "Chat Mod")
        return false
    }

    cfgPath := _ChatMod_IniPath()
    if (cfgPath = "") {
        TrayTip("Set the game path under Settings → Game & startup first.", "Chat mod", "Iconx")
        return false
    }
    if !NativeModsWrite("Chat", "ResetPosition", "1") {
        TrayTip("Could not write to " cfgPath ".", "Chat Mod", "Iconx")
        return false
    }

    LogInfo("ChatMod", "Requested a chat window position reset in " cfgPath)
    _ChatMod_ResetDeadline := A_TickCount + _CHATMOD_RESET_WAIT_MS
    TrayTip("Asking the chat mod to re-dock the window…", "Chat Mod")
    SetTimer(_ChatMod_PollPositionReset, _CHATMOD_RESET_POLL_MS)
    return true
}

; Watches for the DLL to zero the key. Success is the mod telling us it acted;
; a timeout is the mod never having heard, which is a different problem and
; gets a different message.
_ChatMod_PollPositionReset() {
    global _ChatMod_ResetDeadline

    if (_ChatMod_ResetDeadline = 0) {
        SetTimer(_ChatMod_PollPositionReset, 0)
        return
    }
    cfgPath := _ChatMod_IniPath()
    if (cfgPath = "") {
        _ChatMod_EndPositionReset()
        return
    }

    if (IniRead(cfgPath, "Chat", "ResetPosition", "0") != "1") {
        _ChatMod_EndPositionReset()
        LogInfo("ChatMod", "Position reset acknowledged by the chat mod")
        TrayTip("Done — the chat window is back beside its client.", "Chat Mod")
        return
    }

    if (A_TickCount < _ChatMod_ResetDeadline)
        return

    ; The request stays in the file on purpose: the next client to start will
    ; consume it, which is the documented behaviour and usually what was meant.
    _ChatMod_EndPositionReset()
    LogWarn("ChatMod", "Position reset was not picked up by any running client")
    TrayTip(_ChatMod_ResetUnheardText(), "Chat Mod", "Iconi")
}

_ChatMod_EndPositionReset() {
    global _ChatMod_ResetDeadline
    _ChatMod_ResetDeadline := 0
    SetTimer(_ChatMod_PollPositionReset, 0)
}

; Why nothing answered, in the order worth checking. A mod older than this
; command silently ignores the key, which is indistinguishable from no client
; running unless we say so. Maps++ does not version the mods, so it can only
; report the two things it does know.
_ChatMod_ResetUnheardText() {
    if !NativeModsInstalled() {
        return "The osMW client mods are not installed in this game folder, "
            . "so nothing is listening. The request is saved and will be "
            . "picked up if they are installed later."
    }
    return "No running client picked this up — check the game path under "
        . "Game & startup points at the client you are looking at, and that its "
        . "client mods are up to date. The request is saved, so the next "
        . "client you start will do it."
}

; Turns a moved preset into the three font keys, in place.
;
; "Moved" is the whole rule. The dropdown always shows *something*, so a preset
; that always won would overwrite a hand-typed size every time Save was pressed
; — the user would set 16, the dropdown would still read "Large", and 18 would
; be written back. Comparing against the index the panel rendered with means the
; preset only speaks when the user changed it, and the three fields speak
; otherwise.
;
; FontPreset is not in _CHATMOD_SPEC, so it can never reach the ini on its own;
; deleting it here is tidiness, not a safeguard.
_ChatMod_ApplyFontPreset(values) {
    global _CHATMOD_FONT_PRESETS, _ChatMod_RenderedPreset
    if !values.Has("FontPreset")
        return
    idx := _ChatMod_AsInt(values["FontPreset"], -1)
    values.Delete("FontPreset")

    if (idx < 0 || idx >= _CHATMOD_FONT_PRESETS.Length)
        return                                  ; malformed — leave the fields
    if (idx = _ChatMod_CustomPresetIndex())
        return                                  ; the fields are the answer
    if (idx = _ChatMod_RenderedPreset)
        return                                  ; untouched — the fields win

    p := _CHATMOD_FONT_PRESETS[idx + 1]
    values["FontFace"] := p["face"]
    values["FontSize"] := p["size"]
    values["FontWidth"] := p["width"]
    LogInfo("ChatMod", "Applied font preset " p["label"])
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
