#Requires AutoHotkey v2.0

; One-click access to the shortcuts you would otherwise have to remember a
; chord for: a ring of actions around the cursor, on the same engine as the
; client switcher (radial.ahk).
;
; Every entry is keyed by an existing hotkey action id, so this file owns no
; behaviour of its own — it points at handlers already registered elsewhere.
; That is what makes the ring correct for free in every build: an action whose
; addon isn't in this variant simply isn't in gHotkeyActions, and one disabled
; at runtime fails IsHotkeyActionEnabled(), so neither ever draws a spoke.

global QUICK_ACTIONS_RING := "actions"
global QUICK_ACTIONS_MAX := 10           ; more than ~10 spokes stops being quick

; Ordered, comma-separated action ids — the ring, and the order round it.
global _QuickActions_Ids := []
global _QuickActions_Prewarm := true
; The game window that was focused when the ring opened. Several handlers act
; on "the active client", and by the time one runs the ring has had focus.
global _QuickActions_Target := 0

global _QUICK_ACTIONS_DEFAULT := "viewModeToggle,battleSend,inventoryOpenClick,chatToggleAll"
    . ",windowLayoutPrimary,clientRosterToggle,poiToggleLayer,openSettings"

RegisterAddon(Map(
    "name",              "QuickActions",
    "settingsLabel",     "Quick Actions",
    "OnInit",            _QuickActions_OnInit,
    "OnTrayMenu",        _QuickActions_OnTrayMenu,
    "OnSettingsWeb",     _QuickActions_OnSettingsWeb,
    "OnSettingsWebSave", _QuickActions_OnSettingsWebSave
))

RegisterHotkeyAction(Map(
    "id", "quickActionsToggle",
    "label", "Open Quick Actions ring",
    "category", "Quick Actions",
    "default", "^!a",
    "addon", "QuickActions",
    ; Deliberately global rather than hotIfWinActive: the ring is how you reach
    ; the game's controls from outside it, so requiring focus would defeat it.
    "handler", (*) => _QuickActions_Toggle()
))

; ── Catalog ──────────────────────────────────────────────────────
; Everything the ring can offer, in the order the settings picker lists it.
;
;   needsGame  the handler acts on "the active client" and does nothing (or the
;              wrong thing) without one focused — see _QuickActions_OnSelect.
;              Set by reading each handler, NOT by copying its hotIfWinActive
;              flag: several actions carry that flag only to keep their chord
;              clear of the game and work perfectly well unfocused.
;   sends      the handler Sends keystrokes, so our own modifiers must be up.
;   ring       the handler opens another radial — must not run inside this
;              ring's close path.
;   state      an on/off badge, from a global the app already maintains.
;
; Icons must exist in the subsetted Material Symbols font (see the header of
; ui\common\style.css) — a name outside it renders as permanent tofu, not a
; fallback. `swords` and `my_location` are NOT in the subset today.
;
; Built once and kept: it is walked for every spoke on every open, and it never
; changes at runtime (what varies is which entries are *available*).
global _QuickActions_CatalogCache := 0
global _QuickActions_IndexCache := 0

_QuickActions_Catalog() {
    global _QuickActions_CatalogCache, _QuickActions_IndexCache
    if IsObject(_QuickActions_CatalogCache) {
        return _QuickActions_CatalogCache
    }
    _QuickActions_CatalogCache := _QuickActions_BuildCatalog()
    _QuickActions_IndexCache := Map()
    for entry in _QuickActions_CatalogCache {
        _QuickActions_IndexCache[entry["id"]] := entry
    }
    return _QuickActions_CatalogCache
}

_QuickActions_BuildCatalog() {
    return [
        Map("id", "toggleMinimap",           "label", "Minimap",       "icon", "map",
            "needsGame", true, "state", "overlay"),
        Map("id", "viewModeToggle",          "label", "View mode",     "icon", "visibility"),
        Map("id", "viewModeCycle",           "label", "Cycle view",    "icon", "visibility"),
        ; TODO: `swords` once the icon subset is next regenerated.
        Map("id", "battleSend",              "label", "Battle send",   "icon", "track_changes"),
        Map("id", "inventoryOpenClick",      "label", "Inventory",     "icon", "inventory_2",
            "needsGame", true),
        Map("id", "inventoryOpenSend",       "label", "Inventory (key)", "icon", "inventory_2",
            "needsGame", true, "sends", true),
        Map("id", "chatToggleActive",        "label", "Chat (this)",   "icon", "chat",
            "needsGame", true),
        Map("id", "chatToggleAll",           "label", "Chat (others)", "icon", "chat",
            "needsGame", true),
        Map("id", "chatToggleSize",          "label", "Chat size",     "icon", "chat"),
        Map("id", "windowLayoutPrimary",     "label", "Layout",        "icon", "grid_view"),
        Map("id", "windowLayoutUndo",        "label", "Undo layout",   "icon", "refresh"),
        Map("id", "windowLayoutManage",      "label", "Layouts",       "icon", "format_list_bulleted"),
        Map("id", "clientRosterToggle",      "label", "Clients",       "icon", "group",
            "ring", true),
        Map("id", "poiToggleLayer",          "label", "POIs",          "icon", "location_on",
            "state", "pois"),
        Map("id", "poiAddHere",              "label", "Add POI",       "icon", "place"),
        Map("id", "partyMarkersToggleLayer", "label", "Party",         "icon", "shield",
            "state", "party"),
        Map("id", "sendEnterUntilReady",     "label", "Send Enter",    "icon", "keyboard",
            "needsGame", true),
        Map("id", "launchPrimary",           "label", "Launch primary", "icon", "rocket_launch"),
        Map("id", "launchSecondary",         "label", "Launch secondary", "icon", "rocket_launch"),
        Map("id", "openSettings",            "label", "Settings",      "icon", "settings")
    ]
}

_QuickActions_CatalogEntry(id) {
    global _QuickActions_IndexCache
    _QuickActions_Catalog()          ; builds the index on first use
    return _QuickActions_IndexCache.Has(id) ? _QuickActions_IndexCache[id] : 0
}

; An action is offerable only when its addon shipped in this build and is
; enabled right now.
_QuickActions_IsAvailable(id) {
    global gHotkeyActions
    if !gHotkeyActions.Has(id) {
        return false
    }
    return IsHotkeyActionEnabled(gHotkeyActions[id])
}

; ── Lifecycle ────────────────────────────────────────────────────

_QuickActions_OnInit() {
    global QUICK_ACTIONS_RING, _QuickActions_Prewarm
    _QuickActions_LoadConfig()
    RadialRegister(Map(
        "name",     QUICK_ACTIONS_RING,
        "page",     "ui/radial/index.html",
        "title",    "Maps++ Radial — Quick Actions",
        "onBeforeOpen", _QuickActions_OnBeforeOpen,
        "getItems", _QuickActions_Items,
        "getHub",   _QuickActions_Hub,
        "onSelect", _QuickActions_OnSelect,
        "onHub",    _QuickActions_OnHub
    ))
    if _QuickActions_Prewarm {
        ; Staggered past the client roster's -5000 so startup doesn't stand up
        ; two WebView2 instances at the same moment.
        SetTimer(_QuickActions_DoPrewarm, -7000)
    }
}

_QuickActions_DoPrewarm() {
    global gDisabledAddons, QUICK_ACTIONS_RING
    if (gDisabledAddons.Has("QuickActions") && gDisabledAddons["QuickActions"]) {
        return
    }
    RadialPrewarm(QUICK_ACTIONS_RING)
}

_QuickActions_OnTrayMenu(trayGroups) {
    trayGroups["quickActions"].Add("Open Action Ring`t" GetHotkeyDisplay("quickActionsToggle"),
        (*) => _QuickActions_Toggle())
}

_QuickActions_Toggle() {
    global QUICK_ACTIONS_RING, _QuickActions_Target
    if IsNativeInterface() {
        ; Menu.Show() blocks and its callbacks run after it closes, so the
        ; target has to be taken now, while the game still has focus.
        _QuickActions_Target := _QuickActions_CaptureTarget()
        _QuickActions_ShowNativeMenu()
        return
    }
    RadialToggle(QUICK_ACTIONS_RING)
}

; RadialOpen calls this before the ring takes activation, which is the last
; moment the game is still focused — so it is the right and only place to note
; which client an action should act on.
_QuickActions_OnBeforeOpen() {
    global _QuickActions_Target
    _QuickActions_Target := _QuickActions_CaptureTarget()
}

; The client an action should act on: whatever is focused, else the client the
; minimap is tracking, else any running one.
_QuickActions_CaptureTarget() {
    global gTrackedGameHwnd
    if (h := WinActive(GAME_WIN_FILTER)) {
        return h
    }
    if (gTrackedGameHwnd && WinExist("ahk_id " gTrackedGameHwnd)) {
        return gTrackedGameHwnd
    }
    wins := GetTopLevelGameWindows()
    return wins.Length ? wins[1] : 0
}

; ── Items ────────────────────────────────────────────────────────

_QuickActions_Items() {
    global gHotkeyActions, _QuickActions_Ids, QUICK_ACTIONS_MAX
    items := []
    for id in _QuickActions_Ids {
        if (items.Length >= QUICK_ACTIONS_MAX) {
            break
        }
        if !_QuickActions_IsAvailable(id) {
            continue
        }
        entry := _QuickActions_CatalogEntry(id)
        if !IsObject(entry) {
            continue
        }
        chord := GetHotkeyDisplay(id)
        items.Push(Map(
            "key",     id,
            "kind",    "icon",
            "label",   entry["label"],
            "icon",    entry["icon"],
            "state",   entry.Has("state") ? _QuickActions_State(entry["state"]) : "",
            "tooltip", gHotkeyActions[id]["label"] . (chord != "" ? " — " chord : "")
        ))
    }
    return items
}

_QuickActions_Hub() {
    empty := (_QuickActions_Items().Length = 0)
    return Map(
        "title",     empty ? "Nothing" : "Quick",
        "sub",       empty ? "configured" : "actions",
        "hoverIcon", "tune",
        "hoverSub",  "Customise"
    )
}

; On/off for the toggles the app already tracks in a global. Actions whose
; state lives in the game's memory (view mode, chat size) are deliberately
; stateless — reading them would mean a ReadProcessMemory pass per open, for a
; badge, and the ring has to feel instant.
;
; A branch is only reachable when its action id is in gHotkeyActions, i.e. when
; that addon shipped in this variant and is enabled — which is exactly when its
; global exists. IsSet() says so in code rather than only in this comment: a
; variant without map_pois.ahk has no _Pois_LayerVisible at all, and a bare
; read of it would throw rather than just be wrong.
_QuickActions_State(stateKey) {
    switch stateKey {
        case "overlay":
        {
            global gOverlayVisible
            return (IsSet(gOverlayVisible) && gOverlayVisible) ? "on" : "off"
        }
        case "pois":
        {
            global _Pois_LayerVisible
            return (IsSet(_Pois_LayerVisible) && _Pois_LayerVisible) ? "on" : "off"
        }
        case "party":
        {
            global _PartyMarkers_LayerVisible
            return (IsSet(_PartyMarkers_LayerVisible) && _PartyMarkers_LayerVisible) ? "on" : "off"
        }
    }
    return ""
}

; ── Firing an action ─────────────────────────────────────────────

_QuickActions_OnSelect(item, index) {
    global gHotkeyActions, _QuickActions_Target
    entry := _QuickActions_CatalogEntry(item["key"])
    if !IsObject(entry) || !gHotkeyActions.Has(entry["id"]) {
        return
    }
    handler := gHotkeyActions[entry["id"]]["handler"]

    ; Opening another ring from inside this one's close path would have the two
    ; fighting over the mouse grab; let this close finish first.
    if (entry.Has("ring") && entry["ring"]) {
        SetTimer(() => handler.Call(), -1)
        return
    }

    if (entry.Has("needsGame") && entry["needsGame"]) {
        if !(_QuickActions_Target && WinExist("ahk_id " _QuickActions_Target)) {
            TrayTip("No game client is running.", "Quick Actions", "Icon!")
            return
        }
        WinActivate("ahk_id " _QuickActions_Target)
        ; Bail rather than let a click or a keystroke land in the wrong window.
        if !WinWaitActive("ahk_id " _QuickActions_Target, , 0.4) {
            TrayTip("Could not focus the game client.", "Quick Actions", "Iconx")
            return
        }
        ; UpdateMapState blanks the resolved map name on its own timer whenever
        ; no game window is active, so anything reading it needs a fresh pass
        ; now that one is.
        UpdateMapState()
    }
    ; Our own chord is Ctrl+Alt+A; a handler that Sends must not inherit it.
    if (entry.Has("sends") && entry["sends"]) {
        _QuickActions_ReleaseModifiers()
    }
    handler.Call()
}

_QuickActions_ReleaseModifiers() {
    for key in ["Control", "Alt", "Shift"] {
        KeyWait(key, "T0.3")
    }
}

_QuickActions_OnHub() {
    ShowSettingsWindow()
}

; ── Native interface mode ────────────────────────────────────────
; The ring is WebView2-only, like the client switcher's. Rather than leave the
; feature dead in low-memory mode, the same filtered list is offered as a
; plain popup menu at the cursor.

_QuickActions_ShowNativeMenu() {
    global gHotkeyActions
    items := _QuickActions_Items()
    if (items.Length = 0) {
        TrayTip("No quick actions are configured.", "Quick Actions", "Iconi")
        return
    }
    m := Menu()
    for item in items {
        id := item["key"]
        chord := GetHotkeyDisplay(id)
        label := gHotkeyActions[id]["label"] . (chord != "" ? "`t" chord : "")
        m.Add(label, _QuickActions_MakeMenuHandler(item))
    }
    m.Add()
    m.Add("Customise…", (*) => ShowSettingsWindow())
    m.Show()
}

_QuickActions_MakeMenuHandler(item) {
    return (*) => _QuickActions_OnSelect(item, 0)
}

; ── Settings ─────────────────────────────────────────────────────

_QuickActions_OnSettingsWeb() {
    global _QuickActions_Ids, _QuickActions_Prewarm, QUICK_ACTIONS_MAX
    choices := []
    for entry in _QuickActions_Catalog() {
        if !_QuickActions_IsAvailable(entry["id"]) {
            continue      ; not in this build, or its addon is switched off
        }
        choices.Push(Map(
            "id",    entry["id"],
            "label", entry["label"],
            "icon",  entry["icon"]
        ))
    }
    return [
        Map("type", "info", "text",
            "A ring of one-click shortcuts around the cursor.`n"
            . "Open it with " GetHotkeyDisplay("quickActionsToggle") " or from the tray menu, "
            . "then click an action — or press Escape to dismiss it.`n"
            . "Only actions from addons you have enabled are listed."),
        Map("type", "orderedlist", "id", "actions",
            "label", "Actions shown in the ring",
            "items", choices,
            "value", _QuickActions_JoinIds(_QuickActions_Ids),
            "max", QUICK_ACTIONS_MAX),
        Map("type", "checkbox", "id", "prewarm",
            "label", "Load the ring at startup (faster first open, more memory)",
            "value", _QuickActions_Prewarm ? true : false)
    ]
}

_QuickActions_OnSettingsWebSave(values) {
    _QuickActions_ApplySettings(
        values.Has("actions") ? values["actions"] : "",
        values.Has("prewarm") && values["prewarm"] ? true : false)
}

_QuickActions_ApplySettings(actionsCsv, prewarm) {
    global _QuickActions_Ids, _QuickActions_Prewarm, QUICK_ACTIONS_RING
    _QuickActions_Ids := _QuickActions_ParseIds(actionsCsv)
    _QuickActions_Prewarm := prewarm
    _QuickActions_SaveConfig()
    ; If the ring happens to be up, show the new set straight away.
    RadialRefresh(QUICK_ACTIONS_RING)
}

; ── Config ───────────────────────────────────────────────────────

_QuickActions_LoadConfig() {
    global _QuickActions_Ids, _QuickActions_Prewarm, CONFIG_INI, _QUICK_ACTIONS_DEFAULT
    _QuickActions_Ids := _QuickActions_ParseIds(
        IniRead(CONFIG_INI, "QuickActions", "Actions", _QUICK_ACTIONS_DEFAULT))
    _QuickActions_Prewarm := (Trim(IniRead(CONFIG_INI, "QuickActions", "Prewarm", "1")) != "0")
}

_QuickActions_SaveConfig() {
    global _QuickActions_Ids, _QuickActions_Prewarm, CONFIG_INI
    IniWrite(_QuickActions_JoinIds(_QuickActions_Ids), CONFIG_INI, "QuickActions", "Actions")
    IniWrite(_QuickActions_Prewarm ? "1" : "0", CONFIG_INI, "QuickActions", "Prewarm")
}

; Unknown ids are dropped rather than rejected, so one config file stays valid
; across variants — an id for an addon this build doesn't ship just vanishes.
; Duplicates are dropped too; two spokes firing the same action is never meant.
_QuickActions_ParseIds(csv) {
    ids := []
    seen := Map()
    for part in StrSplit(csv, ",") {
        id := Trim(part)
        ; Migrate the removed combined action to the new configurable primary
        ; launch action, which now owns the same multi-client/layout behavior.
        if (id = "launchClientsLayout")
            id := "launchPrimary"
        if (id = "" || seen.Has(id)) {
            continue
        }
        if !IsObject(_QuickActions_CatalogEntry(id)) {
            continue
        }
        seen[id] := true
        ids.Push(id)
    }
    return ids
}

_QuickActions_JoinIds(ids) {
    out := ""
    for id in ids {
        out .= (out = "" ? "" : ",") id
    }
    return out
}
