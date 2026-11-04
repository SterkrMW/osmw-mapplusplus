#Requires AutoHotkey v2.0
;@Ahk2Exe-SetName Maps++
;@Ahk2Exe-SetDescription Maps++
;@Ahk2Exe-SetProductName Maps++
;@Ahk2Exe-SetCompanyName osMW
; Must match APP_VERSION in variables.ahk — build.ps1 fails the build if it does not.
;@Ahk2Exe-SetVersion 1.4.0
#SingleInstance Force
#Warn

Persistent
SetWorkingDir(A_ScriptDir)
SetTitleMatchMode(2)

#Include variables.ahk
#Include functions.ahk
#Include native_mods.ahk
#Include settings.ahk
#Include dialogs.ahk
#Include setup.ahk
#Include radial.ahk
#Include hotkeys.ahk
#Include *i _addons.ahk

; First, before the log line below and before anything writes config.ini: both
; are how an earlier install is told apart from a fresh one. An upgrade has the
; defaults this version changed pinned here, before the loaders read them.
gSetupRun := Setup_DetectFirstRun()
if (gSetupRun = "upgrade")
    _Setup_MarkUpgrade()

InitAppNotificationRegistration()

; ── Launcher startup ─────────────────────────────────────────────

; Before any config work, so a session that fails during it has still
; identified itself in the log.
LogInfo("Startup", "Maps++ " APP_VERSION (A_IsCompiled ? " (compiled)" : " (source)")
    . " — AHK " A_AhkVersion ", " (A_PtrSize = 8 ? "64-bit" : "32-bit"))

; ResolveWritableIniPath's probe leaves a fresh install with an empty config.ini,
; and whichever IniWrite reaches it first would fix it as ANSI for good.
EnsureIniUtf16(CONFIG_INI)

LoadLauncherConfig()
LoadMinimapConfig()
LoadNpcNextId()
LogInfo("Startup", "Config loaded — interface " gInterfaceMode
    . ", " gAddonHooks.Length " addon(s) registered")
; Probed here, before anything builds a window, so a machine without the WebView2
; Runtime gets one notice at startup and native panels throughout — rather than
; an error the first time the tray is right-clicked.
if (gInterfaceMode = "webview" && !WebView2RuntimeAvailable())
    MarkWebView2Unavailable("Startup")
if !A_IsCompiled
    GenerateAddonIncludes()
LoadAddonEnabledStates()
; Addons load their own config here rather than at include time: gInterfaceMode
; is set by LoadLauncherConfig above, and the enabled/disabled state is known, so
; a disabled addon's OnInit is correctly skipped.
FireAddonHook("OnInit")
RegisterCoreHotkeyActions()
LoadHotkeyOverrides()
ApplyAllHotkeys()
SetTimer(UpdateMapState, 200)
SetTimer(CloseOverlayIfFocusLeftGame, 200)
SetTimer(UpdateClientSnapshots, CLIENT_SNAPSHOT_INTERVAL)
RegisterOverlayMouseHandlers()
CheckForUpdateAsync()
OnExit((*) => (ReleaseCachedProcessHandle(), ReleaseAllClientProcesses()))

; Auto-launch a game instance on startup / re-launch.
if (gLaunchOnStartup) {
    LaunchGameInstance("primary")
}

; ── Startup checks ───────────────────────────────────────────────

if !DirExist(MAP_DIR) {
    TrayTip("Map folder missing:`n" MAP_DIR, "Maps++", "Iconi")
}
if !FileExist(MARKER_PNG) {
    TrayTip("marker.png missing next to script — position marker disabled.`n" MARKER_PNG, "Maps++", "Iconi")
}

; ── Fixed hotkeys (not rebindable) ───────────────────────────────
; Keep gHotkeyReserved in hotkeys.ahk in sync with this list — it is what stops
; a rebindable action from being assigned one of these chords. They are static
; on purpose: Reload/Exit must survive a broken hotkey registry, and the
; calibration captures read the cursor over the minimap, so they cannot be
; buttons. (This block was lost once, in the tray-menu rewrite, and nothing
; noticed for months because the calibration panel kept telling users to press
; keys that did nothing — keep it.)

^!r:: Reload()
^!q:: ExitApp()
^!d:: ShowDebugState()
^!s:: CalibrateSignaturesNow()
^!v:: VerifyResolution()
^!1:: CaptureCalibrationPoint(1)
^!2:: CaptureCalibrationPoint(2)
^!3:: ApplyCalibrationFromPoints()
^!4:: ExportCurrentCalibrationToFile()

; ── Tray menu ────────────────────────────────────────────────────

RebuildTrayMenu()
; Both modes listen to the tray icon now: the right-button release is where a
; held Shift is noticed and the native menu rebuilt to match, and in enhanced
; mode where the web menu replaces it.
OnMessage(0x0404, _OnTrayNotify)
; Same reason as radial.ahk's _Radial_OnExit: tearing the menu window down on
; exit emits a WM_ACTIVATE that would otherwise reach the handler after the
; globals it reads are gone.
OnExit(_Tray_UnhookMessages)
if IsWebViewInterface() {
    OnMessage(0x0006, _OnTrayWmActivate)
    ; Enhanced mode intentionally pays the memory cost up front so the first
    ; right-click is as quick as every one after it.
    SetTimer(_PrewarmWebTrayMenu, -6000)
}

; The setup guide on a fresh install, the one-time notice on an upgrade, and a
; "launch now" carried across the restart a finished guide may have needed.
Setup_AfterStartup(gSetupRun)

_Tray_UnhookMessages(*) {
    try OnMessage(0x0404, _OnTrayNotify, 0)
    try OnMessage(0x0006, _OnTrayWmActivate, 0)
}

_OnTrayNotify(wParam, lParam, msg, hwnd) {
    global gTrayDevReveal
    ; 0x0205 = WM_RBUTTONUP (right-click on system tray icon). Everything is
    ; done here, on the release, and nothing on the press: a message monitor
    ; that is still running when the next message for it arrives never sees
    ; that message, so rebuilding on the press could swallow the release, and
    ; with it the menu — no menu at all for a quick click.
    if (lParam = 0x0205) {
        ; Still held as the button comes up: Shift+right-click shows the
        ; Developer group for this one open.
        gTrayDevReveal := GetKeyState("Shift") ? true : false
        ; Native: rebuild, then return nothing so AHK shows the menu it now
        ; holds. (The web menu rebuilds itself as it opens.)
        if !IsWebViewInterface() {
            RebuildTrayMenu()
            return
        }
        ; Returning nothing lets AHK show the native menu, which is the
        ; fallback both for a session already switched to native and for a
        ; WebView2 that fails here despite the startup probe. Uncaught, that
        ; failure was an error dialog on every right-click — and since these
        ; arrive whenever the thread pumps messages, often mid-launch.
        if !IsWebViewInterface() || !ShowWebTrayMenu()
            return
        return 0 ; Suppress native Win32 context menu
    }
}


_OnTrayWmActivate(wParam, lParam, msg, hwnd) {
    global gWebTrayGui, gWebTrayShown
    ; wParam = 0 (WA_INACTIVE). The window outlives any single open now, so this
    ; must only fire for a menu that is actually on screen.
    if (!IsSet(gWebTrayGui) || !IsSet(gWebTrayShown)) {
        return
    }
    if (wParam = 0 && gWebTrayShown && IsObject(gWebTrayGui) && gWebTrayGui.Hwnd
        && hwnd = gWebTrayGui.Hwnd) {
        SetTimer(_CloseWebTrayMenu, -10)
    }
}

global gWebTrayGui := 0, gWebTrayCallbacks := Map()
; Same approach as the radial menu: standing up a WebView2 and loading the page
; costs hundreds of milliseconds, so the window is built once and parked
; off-screen between uses, and it is only moved into view after the page says
; it has drawn the items. Recreating it per right-click is what made the menu
; feel slow and flash an unstyled page first.
global gWebTrayItems := []
global gWebTrayPos := 0
global gWebTrayShown := false     ; on screen right now
global gWebTrayPending := false   ; an open is waiting for the page
global gWebTrayFocusMisses := 0   ; consecutive watchdog polls without focus
global TRAY_MENU_W := 320, TRAY_MENU_H := 390
global TRAY_FOCUS_POLL_MS := 100, TRAY_FOCUS_MISS_LIMIT := 2

_EnsureWebTrayGui() {
    global gWebTrayGui, TRAY_MENU_W, TRAY_MENU_H

    if IsObject(gWebTrayGui) && gWebTrayGui.Hwnd {
        return
    }

    dllDir := (A_PtrSize = 8) ? "64bit" : "32bit"
    dllPath := A_ScriptDir "\Lib\" dllDir "\WebView2Loader.dll"
    wvSettings := { DllPath: dllPath, DefaultWidth: TRAY_MENU_W, DefaultHeight: TRAY_MENU_H }

    g := WebViewGui("-Caption +AlwaysOnTop +ToolWindow -MaximizeBox", "Maps++ Menu", , wvSettings)
    gWebTrayGui := g

    g.OnEvent("Close", (*) => _CloseWebTrayMenu())
    g.WebMessageReceived(WebMsgHandler(_OnWebTrayMessage))
    g.DOMContentLoaded((*) => SetTimer(_PushTrayMenuState, -50))
    g.Navigate(UiPageUrl("ui/tray/index.html"))

    g.Show("x-30000 y-30000 w" TRAY_MENU_W " h" TRAY_MENU_H " NoActivate")
}

; Warms the menu up shortly after launch so the first right-click is quick.
_PrewarmWebTrayMenu() {
    if !IsWebViewInterface()
        return
    try {
        _EnsureWebTrayGui()
    } catch as err {
        ; Said now rather than on the first right-click, which would only
        ; fall back to the native menu without explaining why.
        MarkWebView2Unavailable("Tray menu prewarm", err)
    }
}

ShowWebTrayMenu() {
    global gWebTrayGui, gWebTrayCallbacks, gWebTrayItems, gWebTrayPos, gWebTrayPending
    global TRAY_MENU_W, TRAY_MENU_H

    RebuildTrayMenu()
    gWebTrayCallbacks := Map()
    gWebTrayItems := _ConvertHmenuToWebItems(A_TrayMenu.Handle)

    ; Only the window build is caught, so that a WebView2 failure falls back to
    ; the native menu while a bug in the menu code above still surfaces as one.
    try {
        _EnsureWebTrayGui()
    } catch as err {
        MarkWebView2Unavailable("Tray menu", err)
        return false
    }
    if !IsObject(gWebTrayGui) {
        return false
    }

    mW := TRAY_MENU_W, mH := TRAY_MENU_H

    CoordMode("Mouse", "Screen")
    MouseGetPos(&mX, &mY)
    ; Clamp against the display the tray icon was clicked on, not the primary.
    MonitorGetWorkArea(GetMonitorIndexAtPoint(mX, mY), &sLeft, &sTop, &sRight, &sBottom)

    wX := mX - mW + 20
    wY := mY - mH - 10
    if (wX + mW > sRight - 10)
        wX := sRight - mW - 10
    if (wX < sLeft + 10)
        wX := sLeft + 10
    if (wY < sTop + 10)
        wY := mY + 10
    if (wY + mH > sBottom - 10)
        wY := sBottom - mH - 10

    gWebTrayPos := { x: wX, y: wY, w: mW, h: mH }
    gWebTrayPending := true

    ; Re-render with the menu as it stands now; the page answers with "rendered"
    ; and that is what brings the window into view.
    _PushTrayMenuState()
    ; Backstop for a page that fails to load and never reports.
    SetTimer(_RevealWebTrayMenu, -600)
    return true
}

_RevealWebTrayMenu() {
    global gWebTrayGui, gWebTrayPos, gWebTrayShown, gWebTrayPending
    global gWebTrayFocusMisses, TRAY_FOCUS_POLL_MS
    if !gWebTrayPending || !IsObject(gWebTrayGui) || !IsObject(gWebTrayPos) {
        return
    }
    gWebTrayPending := false
    gWebTrayShown := true
    SetTimer(_RevealWebTrayMenu, 0)

    p := gWebTrayPos
    hwnd := gWebTrayGui.Hwnd
    WinMove(p.x, p.y, p.w, p.h, "ahk_id " hwnd)
    ; Parked NoActivate; activate now, since losing activation dismisses it.
    try WinActivate("ahk_id " hwnd)
    ; WM_ACTIVATE normally dismisses the menu. Some shell/no-activate windows
    ; do not produce that exact message path, so poll foreground ownership as
    ; a cheap backstop while (and only while) the menu is on screen.
    gWebTrayFocusMisses := 0
    SetTimer(_WebTrayFocusTick, TRAY_FOCUS_POLL_MS)
}

_WebTrayFocusTick() {
    global gWebTrayGui, gWebTrayShown, gWebTrayFocusMisses
    global TRAY_FOCUS_MISS_LIMIT

    if !gWebTrayShown {
        SetTimer(_WebTrayFocusTick, 0)
        gWebTrayFocusMisses := 0
        return
    }
    if !IsObject(gWebTrayGui) || !gWebTrayGui.Hwnd {
        _CloseWebTrayMenu()
        return
    }

    foreground := DllCall("GetForegroundWindow", "Ptr")
    foregroundRoot := foreground ? DllCall("GetAncestor", "Ptr", foreground, "UInt", 2, "Ptr") : 0
    menuRoot := DllCall("GetAncestor", "Ptr", gWebTrayGui.Hwnd, "UInt", 2, "Ptr")
    if (foregroundRoot = menuRoot) {
        gWebTrayFocusMisses := 0
        return
    }

    ; Require two consecutive misses so a transient activation hand-off while
    ; the WebView receives focus cannot immediately close a freshly shown menu.
    gWebTrayFocusMisses += 1
    if (gWebTrayFocusMisses >= TRAY_FOCUS_MISS_LIMIT)
        _CloseWebTrayMenu()
}

_PushTrayMenuState() {
    global gWebTrayGui, gWebTrayItems
    if !IsObject(gWebTrayGui)
        return
    itemsJson := _TrayItemsToJson(gWebTrayItems)
    try gWebTrayGui.PostWebMessageAsJson('{"type":"tray-menu-state","items":' itemsJson '}')
}

_OnWebTrayMessage(wv, args) {
    global gWebTrayGui, gWebTrayCallbacks, gWebTrayItems
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
            if IsObject(gWebTrayGui) {
                RebuildTrayMenu()
                gWebTrayItems := _ConvertHmenuToWebItems(A_TrayMenu.Handle)
                _PushTrayMenuState()
            }
        case "rendered":
            ; The page has drawn the items, so there is something worth showing.
            _RevealWebTrayMenu()
        case "execute-item":
            cb := 0
            if msg.Has("id") && gWebTrayCallbacks.Has(msg["id"]) {
                cb := gWebTrayCallbacks[msg["id"]]
            }
            ; An activation always dismisses, even if a concurrent menu rebuild
            ; made this page's item id stale before its message arrived.
            _CloseWebTrayMenu()
            if IsObject(cb) {
                try cb()
            }
        case "dismiss":
            _CloseWebTrayMenu()
    }
}

; Dismisses the menu by parking it off-screen again. The window and its loaded
; page survive, which is what makes the next right-click instant.
_CloseWebTrayMenu() {
    global gWebTrayGui, gWebTrayShown, gWebTrayPending, gWebTrayPos
    global gWebTrayFocusMisses
    SetTimer(_RevealWebTrayMenu, 0)
    SetTimer(_WebTrayFocusTick, 0)
    if IsObject(gWebTrayGui) && gWebTrayGui.Hwnd {
        try WinMove(-30000, -30000, , , "ahk_id " gWebTrayGui.Hwnd)
    }
    gWebTrayShown := false
    gWebTrayPending := false
    gWebTrayPos := 0
    gWebTrayFocusMisses := 0
}

_TrayItemsToJson(items) {
    json := "["
    first := true
    for item in items {
        if !first
            json .= ","
        first := false
        if item.Has("isDivider") && item["isDivider"] {
            json .= '{"isDivider":true}'
            continue
        }
        json .= '{'
            . '"id":' _JSON_Str(item.Has("id") ? item["id"] : "")
            . ',"label":' _JSON_Str(item.Has("label") ? item["label"] : "")
            . ',"icon":' _JSON_Str(item.Has("icon") ? item["icon"] : "")
            . ',"shortcut":' _JSON_Str(item.Has("shortcut") ? item["shortcut"] : "")
            . ',"isDefault":' (item.Has("isDefault") && item["isDefault"] ? "true" : "false")
            . ',"isExit":' (item.Has("isExit") && item["isExit"] ? "true" : "false")
            . ',"isSelected":' (item.Has("isSelected") && item["isSelected"] ? "true" : "false")
            . ',"state":' _JSON_Str(item.Has("state") ? item["state"] : "")
        if item.Has("children") {
            json .= ',"children":' _TrayItemsToJson(item["children"])
        }
        json .= '}'
    }
    json .= "]"
    return json
}

_ConvertHmenuToWebItems(hMenu, idPrefix := "menu_") {
    global gWebTrayCallbacks, gTrayMeta
    items := []
    count := DllCall("GetMenuItemCount", "Ptr", hMenu, "Int")
    if (count <= 0)
        return items

    loop count {
        idx := A_Index - 1
        meta := _GetHmenuItemMeta(hMenu, idx)
        if meta.isSeparator {
            items.Push(Map("isDivider", true))
            continue
        }

        bufLen := DllCall("GetMenuString", "Ptr", hMenu, "UInt", idx, "Ptr", 0, "Int", 0, "UInt", 0x400)
        if (bufLen <= 0) {
            items.Push(Map("isDivider", true))
            continue
        }
        buf := Buffer((bufLen + 1) * 2, 0)
        DllCall("GetMenuString", "Ptr", hMenu, "UInt", idx, "Ptr", buf, "Int", bufLen + 1, "UInt", 0x400)
        str := StrGet(buf)

        parts := StrSplit(str, "`t", , 2)
        ; "&&" is how TrayGroup keeps a literal ampersand (a profile named
        ; "Tom & Jerry") from becoming a keyboard mnemonic natively.
        label := StrReplace(parts[1], "&&", "&")
        shortcut := (parts.Length >= 2) ? parts[2] : ""

        ; What the native menu cannot say, TrayGroup recorded by position.
        info := gTrayMeta.Has(hMenu "|" idx) ? gTrayMeta[hMenu "|" idx] : Map()
        icon := info.Has("icon") ? info["icon"] : ""
        stateText := info.Has("state") ? info["state"] : ""
        ; A toggle's check mark is only how the native menu shows its state;
        ; the page shows the ON/OFF pill instead, so only a radio choice (or an
        ; unannotated check) keeps the ✓.
        isSelected := info.Has("selected") ? (info["selected"] ? true : false)
            : (stateText = "" && meta.isChecked)
        isExit := info.Has("exit") && info["exit"]

        hSub := DllCall("GetSubMenu", "Ptr", hMenu, "Int", idx, "Ptr")
        if (hSub) {
            children := _ConvertHmenuToWebItems(hSub, idPrefix idx "_")
            items.Push(Map("label", label, "icon", icon, "children", children,
                "isSelected", isSelected, "state", stateText))
        } else {
            itemId := idPrefix idx
            cmdId := DllCall("GetMenuItemID", "Ptr", hMenu, "Int", idx, "UInt")
            gWebTrayCallbacks[itemId] := _MakeHmenuCallback(hMenu, cmdId)
            items.Push(Map("id", itemId, "label", label, "icon", icon, "shortcut", shortcut,
                "isDefault", meta.isDefault, "isExit", isExit, "isSelected", isSelected,
                "state", stateText))
        }
    }
    return items
}

; GetMenuState() packs a popup submenu's item count into its high byte, so a
; submenu with eight children also has bit 0x800 set and looks like
; MF_SEPARATOR. GetMenuItemInfoW keeps type and state in distinct fields.
_GetHmenuItemMeta(hMenu, idx) {
    static MIIM_STATE := 0x1, MIIM_FTYPE := 0x100
    static MFT_SEPARATOR := 0x800, MFS_DEFAULT := 0x1000
    infoSize := (A_PtrSize = 8) ? 80 : 48
    info := Buffer(infoSize, 0)
    NumPut("UInt", infoSize, info, 0)
    NumPut("UInt", MIIM_STATE | MIIM_FTYPE, info, 4)
    if !DllCall("GetMenuItemInfoW", "Ptr", hMenu, "UInt", idx, "Int", true,
        "Ptr", info.Ptr, "Int")
        return {isSeparator: false, isDefault: false, isChecked: false}
    itemType := NumGet(info, 8, "UInt")
    itemState := NumGet(info, 12, "UInt")
    return {
        isSeparator: (itemType & MFT_SEPARATOR) ? true : false,
        isDefault: (itemState & MFS_DEFAULT) ? true : false,
        isChecked: (itemState & 0x8) ? true : false
    }
}

_MakeHmenuCallback(hMenu, cmdId) {
    return (*) => DllCall("PostMessage", "Ptr", A_ScriptHwnd, "UInt", 0x0111, "Ptr", cmdId, "Ptr", hMenu)
}

; ── Tray groups ──────────────────────────────────────────────────
;
; What OnTrayMenu handlers are handed, one per group of the tray menu. It has
; Menu's shape — Add() for a separator, Add(label, callbackOrMenu), Check(label)
; — so a handler written against a raw Menu still works, but nothing is built
; until RebuildTrayMenu has heard from every addon. That is what lets core put
; the items in a fixed order with separators between sections, regardless of
; which addons a build has or what order they were included in, and record the
; metadata the enhanced tray needs (see gTrayMeta).
;
; opts (a Map, all optional):
;   section   which section of the group the item belongs to; the group's
;             constructor lists them in display order, and a separator goes
;             between two sections — no handler has to know its neighbours
;   order     position within the section (default 100; ties keep add order)
;   hotkey    a hotkey action id: its current chord is shown as the shortcut
;   shortcut  literal shortcut text, for the fixed (non-rebindable) keys
;   icon      Material Symbols name — only top-level rows draw one, and the
;             name must be in the subset listed in ui\common\style.css
;   toggle    true/false: a check mark natively, an ON/OFF pill on the web.
;             The label stays plain ("Party markers", not "... — On").
;   selected  a radio choice that is current
;   default   the menu's default item (bold; the tray icon's double-click)
;   exit      rendered in the web menu's footer
;
; Labels are plain text: "&" is escaped here, and two items with the same label
; are told apart rather than collapsing into one (Menu.Add treats a repeated
; name as "modify that item").
class TrayGroup {
    __New(sectionOrder := "") {
        this.items := []
        this.sectionOrder := IsObject(sectionOrder) ? sectionOrder : []
    }

    Add(label := "", target := "", opts := "") {
        item := Map("label", String(label), "target", target, "seq", this.items.Length + 1)
        if (item["label"] = "")
            item["separator"] := true
        if (opts is Map) {
            for k, v in opts
                item[k] := v
        }
        this.items.Push(item)
        return this
    }

    ; Menu.Check's shape, for a handler that adds an item and then checks it.
    Check(label) {
        for item in this.items {
            if (item["label"] = label)
                item["checked"] := true
        }
    }

    HasItems() {
        for item in this.items {
            if !item.Has("separator")
                return true
        }
        return false
    }

    ; Builds the native menu — into `into` when given (A_TrayMenu), else a new
    ; one — and records each row's metadata. Returns the menu, or "" if the
    ; group turned out to have nothing in it.
    Build(into := "") {
        global gTrayMeta
        if !this.HasItems()
            return ""
        m := IsObject(into) ? into : Menu()
        used := Map()
        pos := 0
        pendingSep := false
        lastRank := 0
        for item in this._Sorted() {
            if item.Has("separator") {
                pendingSep := (pos > 0)
                continue
            }
            target := item["target"]
            if (target is TrayGroup) {
                target := target.Build()
                if (target = "")
                    continue
            }
            rank := this._Rank(item)
            if (pos > 0 && rank != lastRank)
                pendingSep := true
            lastRank := rank
            if pendingSep {
                m.Add()
                pos++
                pendingSep := false
            }

            text := _TrayItemText(used, item)
            m.Add(text, target)
            info := Map()
            if item.Has("icon")
                info["icon"] := item["icon"]
            if item.Has("toggle") {
                info["state"] := item["toggle"] ? "On" : "Off"
                if item["toggle"]
                    m.Check(text)
            }
            if (item.Has("selected") && item["selected"]) || (item.Has("checked") && item["checked"]) {
                info["selected"] := true
                m.Check(text)
            }
            if (item.Has("exit") && item["exit"])
                info["exit"] := true
            if (item.Has("default") && item["default"])
                m.Default := text
            gTrayMeta[m.Handle "|" pos] := info
            pos++
        }
        return m
    }

    _Rank(item) {
        sec := item.Has("section") ? item["section"] : ""
        for i, name in this.sectionOrder {
            if (name = sec)
                return i
        }
        ; An unknown or missing section goes last, in the order it was added.
        return this.sectionOrder.Length + 1
    }

    _Key(item) {
        o := item.Has("order") ? item["order"] : 100
        return [this._Rank(item), o, item["seq"]]
    }

    ; Insertion sort on (section, order, add order): a handful of items, and
    ; it keeps equal keys stable without a comparator library.
    _Sorted() {
        out := []
        for item in this.items {
            k := this._Key(item)
            i := out.Length
            while (i >= 1 && _TrayKeyLess(k, this._Key(out[i])))
                i--
            out.InsertAt(i + 1, item)
        }
        return out
    }
}

_TrayKeyLess(a, b) {
    loop a.Length {
        if (a[A_Index] != b[A_Index])
            return a[A_Index] < b[A_Index]
    }
    return false
}

; The native menu text for an item: label escaped and made unique, then the
; shortcut after a tab. A legacy label may already carry "`t<shortcut>".
_TrayItemText(used, item) {
    parts := StrSplit(item["label"], "`t", , 2)
    base := StrReplace(parts[1], "&", "&&")
    if used.Has(base) {
        used[base] += 1
        base .= " (" used[base] ")"
    } else {
        used[base] := 1
    }
    shortcut := ""
    if item.Has("hotkey")
        shortcut := GetHotkeyDisplay(item["hotkey"])
    else if item.Has("shortcut")
        shortcut := item["shortcut"]
    else if (parts.Length >= 2)
        shortcut := parts[2]
    return (shortcut != "") ? (base "`t" shortcut) : base
}

; ── Tray menu ────────────────────────────────────────────────────
;
; Everything is one level of submenu at most. Lists that used to nest deeper
; now live in the windows that manage them: layout presets in Window layouts,
; per-account launches in Accounts, POI sharing in Settings → Map POIs.
RebuildTrayMenu() {
    global gUpdateVersion, gTrayMeta, gTrayDevReveal, gDeveloperTools
    global gPrimaryProfileId, gSecondaryProfileId
    gTrayMeta := Map()

    groups := Map(
        "minimap",   TrayGroup(["view", "pois", "markers"]),
        "clients",   TrayGroup(["status", "layout", "chat", "accounts", "broadcast"]),
        "actions",   TrayGroup(["ring", "game", "tools"]),
        "help",      TrayGroup(["guide", "support", "about"]),
        "developer", TrayGroup(["state", "signatures", "tools", "app"]))
    ; The names before the regroup, for an addon not yet moved. A handler that
    ; throws on a missing key counts toward quarantine, which takes the addon's
    ; hotkeys down with it for the session.
    groups["map"] := groups["minimap"]
    groups["quickActions"] := groups["actions"]
    groups["debug"] := groups["developer"]

    ; Core entries. Addons add theirs to the same groups below, and the
    ; sections decide where each lands, so the order here does not matter.
    groups["minimap"].Add("Open Maps++ minimap",
        (*) => (FocusTrackedGameClient(), ToggleMapsppMinimap()),
        Map("section", "view", "order", 10, "hotkey", "openMapsppMinimap"))
    groups["minimap"].Add("Calibrate this map…", (*) => ShowCalibrationPanel(),
        Map("section", "view", "order", 20))

    groups["actions"].Add("Send Enter until ready", (*) => SendEnterUntilReady(),
        Map("section", "game", "order", 10, "hotkey", "sendEnterUntilReady"))

    groups["help"].Add("Setup guide…", (*) => Setup_Show(), Map("section", "guide"))
    groups["help"].Add("Copy diagnostics", (*) => CopyDiagnosticsReport(),
        Map("section", "support", "order", 10))
    groups["help"].Add("Open log folder", (*) => OpenLogFolder(),
        Map("section", "support", "order", 20))
    groups["help"].Add("Client mods status…", (*) => ShowClientModsStatus(),
        Map("section", "support", "order", 30))
    ; The client mods' own logs, off by default.
    groups["help"].Add("Client mod logging", (*) => ToggleModLogging(),
        Map("section", "support", "order", 40, "toggle", ModLoggingEnabled()))
    groups["help"].Add("About Maps++…", (*) => ShowAboutDialog(), Map("section", "about"))

    groups["developer"].Add("Debug state", (*) => ShowDebugState(),
        Map("section", "state", "shortcut", "Ctrl+Alt+D"))
    groups["developer"].Add("Calibrate signatures", (*) => CalibrateSignaturesNow(),
        Map("section", "signatures", "order", 10, "shortcut", "Ctrl+Alt+S"))
    groups["developer"].Add("Verify signatures", (*) => VerifyResolution(),
        Map("section", "signatures", "order", 20, "shortcut", "Ctrl+Alt+V"))
    groups["developer"].Add("Reload Maps++", (*) => Reload(),
        Map("section", "app", "shortcut", "Ctrl+Alt+R"))

    FireAddonHook("OnTrayMenu", groups)

    top := TrayGroup(["update", "launch", "groups", "settings", "exit"])
    ; Only present when a newer version was actually seen, so the notification
    ; is not the user's only chance to act on it.
    if (gUpdateVersion != "")
        top.Add("Get the update — " gUpdateVersion, (*) => OpenUpdatePage(),
            Map("section", "update", "icon", "download"))

    ; The launch rows are named after the profiles they run, so "Launch Alts"
    ; says what it does; the profiles themselves are edited in Settings.
    primary := LaunchProfileById(gPrimaryProfileId)
    primaryName := IsObject(primary) ? primary["name"] : "Primary"
    top.Add("Launch " _TrayClip(primaryName), (*) => LaunchConfiguredClients("primary"),
        Map("section", "launch", "order", 10, "hotkey", "launchPrimary",
            "icon", "rocket_launch", "default", true))
    secondary := (gSecondaryProfileId != "") ? LaunchProfileById(gSecondaryProfileId) : ""
    if IsObject(secondary) {
        ; One profile can back both launches (its display decides where);
        ; the label must still differ from the row above.
        secondaryLabel := (gSecondaryProfileId = gPrimaryProfileId)
            ? "Launch " _TrayClip(primaryName) " (secondary)"
            : "Launch " _TrayClip(secondary["name"])
        top.Add(secondaryLabel, (*) => LaunchConfiguredClients("secondary"),
            Map("section", "launch", "order", 20, "hotkey", "launchSecondary",
                "icon", "rocket_launch"))
    }
    top.Add("Launch other", _BuildLaunchOtherGroup(),
        Map("section", "launch", "order", 30, "icon", "format_list_bulleted"))

    top.Add("Minimap", groups["minimap"], Map("section", "groups", "order", 10, "icon", "map"))
    top.Add("Clients", groups["clients"], Map("section", "groups", "order", 20, "icon", "group"))
    top.Add("Actions", groups["actions"], Map("section", "groups", "order", 30, "icon", "tune"))

    top.Add("Settings…", (*) => ShowSettingsWindow(),
        Map("section", "settings", "order", 10, "hotkey", "openSettings", "icon", "settings"))
    top.Add("Help", groups["help"], Map("section", "settings", "order", 20, "icon", "help"))
    if (gDeveloperTools || gTrayDevReveal)
        top.Add("Developer", groups["developer"],
            Map("section", "settings", "order", 30, "icon", "bug_report"))

    top.Add("Exit", (*) => ExitApp(),
        Map("section", "exit", "shortcut", "Ctrl+Alt+Q", "icon", "power_settings_new", "exit", true))

    A_TrayMenu.Delete()
    top.Build(A_TrayMenu)
    A_IconTip := "osMW Maps++"
}

; A profile name is user text with a 40-character ceiling; the tray row is a
; fixed width.
_TrayClip(text, maxChars := 28) {
    s := Trim(String(text))
    return (StrLen(s) <= maxChars) ? s : (SubStr(s, 1, maxChars - 1) "…")
}

; Launch other: the profiles not bound to a launch action, then the roster's
; account groups. Per-account launches moved into the Accounts editor —
; listing every account twice over (start-at and only), once per display, is
; what made this the deepest corner of the old menu.
_BuildLaunchOtherGroup() {
    global gPrimaryProfileId, gSecondaryProfileId
    g := TrayGroup(["profiles", "groups"])

    for _, id in LaunchProfileIds() {
        ; The bound profiles have rows of their own just above.
        if (id = gPrimaryProfileId || id = gSecondaryProfileId)
            continue
        prof := LaunchProfileById(id)
        if !IsObject(prof)
            continue
        ; An unbound profile carries its own display, so "primary" here only
        ; decides where one that expressed no preference lands.
        g.Add(_ClientLaunchProfileLabel(prof), _ClientLaunchRunProfile.Bind(prof, "primary"),
            Map("section", "profiles"))
    }

    choicesFn := GetFuncByName("_AutoLogin_LaunchChoices")
    specFn := GetFuncByName("_AutoLogin_LaunchSpec")
    if (choicesFn && specFn && IsAddonEnabled("AutoLogin")) {
        choices := ""
        try choices := choicesFn()
        if IsObject(choices) {
            for grp in choices["groups"]
                g.Add("Group: " grp["name"] " — " grp["count"] " account" (grp["count"] = 1 ? "" : "s"),
                    specFn.Bind("primary", grp["spec"]), Map("section", "groups"))
        }
    }
    return g
}

; Menu callbacks are handed three arguments of their own (item name, position,
; menu), so anything bound into a menu has to tolerate them — hence the trailing
; `*` here rather than binding LaunchWithProfile directly and letting stray
; arguments reach a core entry point.
_ClientLaunchRunProfile(profile, monitorWhich, *) {
    LaunchWithProfile(profile, monitorWhich)
}

; What a profile will actually do, counted rather than assumed: a group of three
; accounts launches three clients, not the configured five.
_ClientLaunchProfileLabel(prof) {
    global gMultiClientCount
    n := (prof["count"] >= 1) ? prof["count"] : gMultiClientCount
    plan := ResolveAccountPlan(prof["accounts"], n)
    if (IsObject(plan) && plan.Length)
        n := plan.Length
    return _TrayClip(prof["name"]) " — " n " client" (n = 1 ? "" : "s")
}

HandleTab() {
    if gOverlayVisible {
        CloseOverlay()
        return
    }
    ; From the Tab hotkey this is always true — HotIfToggleMinimap already asked.
    ; From Quick Actions it may not be, and then the ring's "Minimap" should do
    ; what Tab would have: hand the key to the game, so its own minimap opens.
    ; ($Tab's hook ignores keys this script sends, so this cannot re-enter.)
    if TabOpensMapsppMinimap() {
        ShowOrToggleOverlay(gResolvedMapName, gResolvedMapPath)
        return
    }
    if WinActive(GAME_WIN_FILTER)
        SendEvent("{Tab}")
}

; Opens (or closes) the Maps++ overlay whatever Tab is set to do — the way to its
; POIs, party markers and waypoints on a map where the game has a minimap too.
; Still refuses where the overlay could not work at all: no image for this map,
; a battle, a loading screen.
ToggleMapsppMinimap() {
    global gOverlayVisible, gCanOverride, gResolvedMapName, gResolvedMapPath
    if gOverlayVisible {
        CloseOverlay()
        return
    }
    if !gCanOverride {
        TrayTip((gResolvedMapName = "")
            ? "Focus a game client in the world first."
            : "Maps++ has no map image for " gResolvedMapName ".",
            "Maps++", "Iconi")
        return
    }
    if !IsMinimapAllowed() {
        TrayTip("The minimap is not available right now (battle or loading screen).",
            "Maps++", "Iconi")
        return
    }
    ShowOrToggleOverlay(gResolvedMapName, gResolvedMapPath)
}

; For tray entries: the tray takes focus from the game, and UpdateMapState
; forgets the map the moment the game is not focused. Hand focus back to the
; client being tracked and re-read before acting on "the current map".
FocusTrackedGameClient() {
    global gTrackedGameHwnd
    hwnd := (gTrackedGameHwnd && WinExist("ahk_id " gTrackedGameHwnd)) ? gTrackedGameHwnd : 0
    if !hwnd {
        windows := GetTopLevelGameWindows()
        hwnd := windows.Length ? windows[1] : 0
    }
    if !hwnd
        return false
    try WinActivate("ahk_id " hwnd)
    try WinWaitActive("ahk_id " hwnd, , 0.4)
    UpdateMapState()
    return WinActive("ahk_id " hwnd) ? true : false
}

; Settings → Minimap's Tab choice, and the setup guide's. Saved immediately.
SetMinimapSource(src, *) {
    global gMinimapSource, CONFIG_INI
    gMinimapSource := NormalizeMinimapSource(src)
    try IniWrite(gMinimapSource, CONFIG_INI, "Minimap", "Source")
    LogInfo("Minimap", "Tab minimap source set to " gMinimapSource)
    SetTimer(RebuildTrayMenu, -1)
}

; Once, ever: the first time Tab goes to the game on a map Maps++ also has an
; image for. Every shipped map is such a map on a patched client, so without
; this a user who relied on POIs or party markers would just find them gone.
_MaybeShowMinimapSourceNotice(st, mapPath) {
    global gMinimapSource, gMinimapSourceNoticeShown, CONFIG_INI
    if (gMinimapSourceNoticeShown || gMinimapSource != "game" || mapPath = "")
        return
    if !(IsObject(st) && st.available)
        return
    gMinimapSourceNoticeShown := true
    try IniWrite("1", CONFIG_INI, "Minimap", "SourceNoticeShown")
    chord := GetHotkeyDisplay("openMapsppMinimap")
    TrayTip("Tab now opens the game's own minimap here.`n"
        . "For the Maps++ map (POIs, party markers, waypoints) use "
        . (chord != "" ? chord : "Open Maps++ minimap — bind it under Settings → Hotkeys")
        . ", or Tray → Minimap.", "Maps++", "Iconi")
}

; Once per client, the first time it is focused: did the client mods load in
; it? Only a refused core is worth saying anything about — see
; NativeModsCoreLoadState for why the client cannot say it itself.
;
; Shown once, and not again while the situation is unchanged; a client seen
; with the mods loaded clears that, so a later refusal is news again. That is
; the same rule the core applies to its own notice ([Core] BlockedNotice), kept
; in Maps++'s config.ini because osmw.ini is not Maps++'s to write.
_CheckClientModsLoaded() {
    global gCoreRefusedNoticeShown, CONFIG_INI
    static loaded := false, checkedPid := 0, triedPid := 0, unknownTries := 0
    static shownThisSession := false
    if !loaded {
        loaded := true
        gCoreRefusedNoticeShown := (Trim(IniRead(CONFIG_INI, "ClientMods", "CoreRefusedNoticeShown", "0")) = "1")
    }
    proc := GetCachedProcessHandleAndBase()
    if (!proc.ok || proc.pid = checkedPid)
        return
    if (proc.pid != triedPid) {
        triedPid := proc.pid
        unknownTries := 0
    }
    state := NativeModsCoreLoadState(proc, proc.pid)
    ; A read that fails now usually works a tick later. One that never does
    ; must not cost a module walk five times a second for the client's life.
    if (state = "unknown" && ++unknownTries < 10)
        return
    checkedPid := proc.pid
    if (state = "loaded" && gCoreRefusedNoticeShown) {
        gCoreRefusedNoticeShown := false
        try IniWrite("0", CONFIG_INI, "ClientMods", "CoreRefusedNoticeShown")
        return
    }
    ; shownThisSession: two clients from different folders, one loading the
    ; mods and one not, would otherwise clear and re-raise this on every switch.
    if (state != "refused" || gCoreRefusedNoticeShown || shownThisSession)
        return
    gCoreRefusedNoticeShown := true
    shownThisSession := true
    try IniWrite("1", CONFIG_INI, "ClientMods", "CoreRefusedNoticeShown")
    ; Off this timer thread: the dialog waits for the user.
    SetTimer(ShowCoreRefusedNotice, -1)
}

ShowCoreRefusedNotice() {
    sac := SmartAppControlState()
    text := "This game client is running without the osMW client mods. Windows refused "
        . "to load osmw_core.dll, so none of them started — no improved chat, auto-login, "
        . "F12 settings panel or display scaling.`n`n"
    if (sac = "on") {
        text .= "Smart App Control is on, and it refuses files that are not code-signed. "
            . "The mods are not signed, but they are not damaged. They can only load with "
            . "Smart App Control turned off (Windows Security → App & browser control → "
            . "Smart App Control settings)."
    } else {
        text .= "Smart App Control is not what stopped it here. On a work or school PC a "
            . "security policy may block it; otherwise antivirus or a damaged "
            . "osmw_core.dll is likely, and reinstalling the client mods is the first "
            . "thing to try."
    }
    text .= "`n`nMaps++ works the same either way. This message is not shown again until "
        . "a client has loaded the mods successfully."
    ShowMessage(text, "Maps++ — Client mods did not load", Map("severity", "warn"))
}

CloseOverlay() {
    global gOverlayVisible, gGui, gOverlayHover
    if gOverlayVisible && IsObject(gGui) {
        gGui.Hide()
        gOverlayVisible := false
        ; The hover timer stops with the overlay, so clear the state rather
        ; than leaving it stuck on for the next time it opens.
        gOverlayHover := false
        SetTimer(UpdateMarkerPosition, 0)
        FireAddonHook("OnOverlayHide")
    }
}

; Hide minimap when focus leaves the game and this overlay (e.g. Alt+Tab to another app). Game or minimap Gui keeps it open.
; Pin mode ("Keep the minimap open when the game loses focus") opts out of this
; check only — the battle / loading / unsupported-map closes in UpdateMapState
; still apply, so a pinned overlay never floats over a game that isn't there.
CloseOverlayIfFocusLeftGame() {
    global gOverlayVisible, gGui, gMinimapKeepOpen, gOverlayDragging
    if !gOverlayVisible || !IsObject(gGui) || !gGui.Hwnd {
        return
    }
    if (gMinimapKeepOpen || gOverlayDragging) {
        return
    }
    if WinActive(GAME_WIN_FILTER) {
        return
    }
    if WinActive("ahk_id " gGui.Hwnd) {
        return
    }
    CloseOverlay()
}

UpdateMapState() {
    global gCanOverride, gResolvedMapName, gResolvedMapPath, gTrackedGameHwnd
    global gOverlayVisible, gCurrentMapPath, gCurrentMapName, gPic, gNativeMinimap
    static sPrevMapName := ""

    activeHwnd := WinActive(GAME_WIN_FILTER)
    if !activeHwnd {
        gCanOverride := false
        gResolvedMapName := ""
        gResolvedMapPath := ""
        return
    }

    ; Auto-close overlay when minimap is no longer allowed (battle, loading screen, etc.)
    if gOverlayVisible && !IsMinimapAllowed() {
        CloseOverlay()
    }

    gTrackedGameHwnd := activeHwnd
    _CheckClientModsLoaded()
    ; Here rather than at open time: a change drops the overlay Gui, and doing
    ; that from inside ShowOrToggleOverlay would re-enter it mid-toggle.
    RefreshGameContentScale(activeHwnd)
    mapName := ReadCurrentMapName()
    ; For the debug views and the one-time notice only. Tab's own decision is
    ; re-read at the keypress (TabOpensMapsppMinimap), never taken from here.
    gNativeMinimap := (mapName = "") ? 0 : ReadNativeMinimapState()
    if (mapName = "") {
        gCanOverride := false
        gResolvedMapName := ""
        gResolvedMapPath := ""
        if (sPrevMapName != "") {
            prev := sPrevMapName
            sPrevMapName := ""
            FireAddonHook("OnMapChange", "", prev)
        }
        ; Scene changed to one with no readable map name — close overlay.
        if gOverlayVisible {
            CloseOverlay()
        }
        return
    }

    mapPath := ResolveMapPath(mapName)
    if (mapPath = "") {
        gCanOverride := false
        gResolvedMapName := mapName
        gResolvedMapPath := ""
        if (mapName != sPrevMapName) {
            prev := sPrevMapName
            sPrevMapName := mapName
            FireAddonHook("OnMapChange", mapName, prev)
        }
        ; New scene has no custom minimap image — close overlay.
        if gOverlayVisible {
            CloseOverlay()
        }
        return
    }

    gCanOverride := true
    gResolvedMapName := mapName
    gResolvedMapPath := mapPath
    if (mapName != sPrevMapName) {
        prev := sPrevMapName
        sPrevMapName := mapName
        FireAddonHook("OnMapChange", mapName, prev)
    }
    if (!gMinimapSourceNoticeShown && IsMinimapAllowed())
        _MaybeShowMinimapSourceNotice(gNativeMinimap, mapPath)

    ; Hot-swap the minimap image if the scene changed to a different map.
    if gOverlayVisible && (gCurrentMapPath != mapPath) {
        gPic.Value := mapPath
        gCurrentMapName := mapName
        gCurrentMapPath := mapPath
    }
}

; ── Verify Battle Stats ──────────────────────────────────────────
;
; The direct analogue of Character Vendor's Verify Slot Mapping, and here for
; the same reason: the battle actor layout rests on assumptions that can only be
; checked against a live fight.
;
; It answers the one thing still open — whether the live pool is the block at
; +0x00 or the copy at +0x24 — by showing both. Take a reading, take damage,
; take another: the block that moved is the real one. It also shows every
; location, so where the party ends and the enemy begins is visible rather than
; assumed, and it stays in the build afterwards as the check to run after a
; client patch.

_BattleJoin(arr) {
    out := ""
    for v in arr
        out .= (out = "" ? "" : ", ") v
    return out
}

ShowAboutDialog() {
    global APP_VERSION, gInterfaceMode, gAddonHooks, gDisabledAddons
    global gUpdateVersion, gUpdateNotes, VERSION_DOWNLOAD_URL
    active := 0
    for _, am in gAddonHooks {
        name := am.Has("name") ? am["name"] : ""
        if !(name != "" && gDisabledAddons.Has(name) && gDisabledAddons[name])
            active++
    }
    updateLine := (gUpdateVersion != "")
        ? ("`n" gUpdateVersion " is available"
            . (gUpdateNotes != "" ? " — " gUpdateNotes : "")
            . "`n" VERSION_DOWNLOAD_URL "`n")
        : ""
    ShowMessage("osMW Maps++`n`n"
        . "Version " APP_VERSION (A_IsCompiled ? "" : "  (running from source)") "`n"
        . "AutoHotkey " A_AhkVersion "`n"
        . "Interface: " (IsWebViewInterface() ? "WebView2 (enhanced)"
            : gInterfaceMode = "webview" ? "Native — WebView2 Runtime unavailable"
            : "Native (low memory)") "`n"
        . active " of " gAddonHooks.Length " addons active`n"
        . updateLine
        . "`nReporting a problem? Tray → Help → Copy diagnostics`n"
        . "collects everything needed, ready to paste.",
        "About Maps++")
}

OpenLogFolder() {
    path := LogPath()
    SplitPath(path, , &dir)
    if !DirExist(dir) {
        TrayTip("No log folder yet — nothing has been logged.", "Maps++", "Iconi")
        return
    }
    ; Select the file when it exists so the user does not have to hunt for it.
    try {
        if FileExist(path)
            Run('explorer.exe /select,"' path '"')
        else
            Run('explorer.exe "' dir '"')
    } catch as err {
        TrayTip("Could not open " dir "`n" err.Message, "Maps++", "Iconx")
    }
}

; The client mods write nothing unless this is on, so this is the "turn it on,
; reproduce it, send me the files" step of a support conversation rather than
; something anyone leaves enabled. Each DLL reads the key once, as the client
; starts, so clients already running keep whatever they started with — hence
; the relaunch in the notification. The key lives in the mods' own osmw.ini;
; see native_mods.ahk.
ToggleModLogging() {
    want := !ModLoggingEnabled()
    if !SetModLogging(want) {
        gameDir := ResolveGameDirectory()
        TrayTip((gameDir = "")
            ? "Could not write the game folder's osmw.ini.`n"
              . "Set a game path in Settings → Game & startup first."
            : !NativeModsInstalled()
            ? "The osMW client mods are not installed in`n" gameDir
              . "`nso there is no mod logging to turn on."
            : "Could not write " NativeModsIniPath() ".`nSee the Maps++ log for why.",
            "Maps++", "Iconx")
        return
    }
    ; After the callback returns, so the native tray's label and checkmark
    ; cannot stay stale. The enhanced tray also rebuilds on open.
    SetTimer(RebuildTrayMenu, -1)
    TrayTip(want
        ? "Client mod logging ON.`nRelaunch your clients, reproduce the problem, "
          . "then send the osmw_*.log files from the game folder."
        : "Client mod logging OFF.`nExisting logs are cleared as clients relaunch.",
        "Maps++", "Iconi")
}

; Tray → Help → Client mods status. What osmw_status.ini says about the client
; mods: which features are running, which stood down and why, and whether
; Windows blocked any. Read-only — the file is the core's. Reached from the tray,
; so a blocking dialog is fine here (see DeferFromWebMessage for when it isn't).
ShowClientModsStatus() {
    st := NativeModsReadStatus()
    summary := NativeModsStatusSummary(st)
    opts := Map("severity", summary.severity)
    table := NativeModsStatusTable(st)
    if (st.ok && table != "")
        opts["detail"] := table
    text := summary.text
    ; A client whose core was refused wrote no report, so the file can only
    ; describe some earlier client. Say that first, or the report reads as
    ; "all fine" about a client running stock.
    refused := RunningClientsWithoutMods()
    if refused.Length {
        text := "Running without the mods: " _NativeModsJoin(refused) ". Windows refused "
            . "osmw_core.dll in " (refused.Length = 1 ? "that client" : "those clients")
            . ", so the report below is from an earlier one."
            . (SmartAppControlState() = "on" ? " Smart App Control is on." : "")
            . "`n`n" text
        opts["severity"] := "danger"
    }
    ShowMessage(text, "Maps++ — Client Mods Status", opts)
}

; Every running client whose core was refused, by character name or PID.
RunningClientsWithoutMods() {
    out := []
    seen := Map()
    for hwnd in GetTopLevelGameWindows() {
        pid := 0
        try pid := WinGetPID("ahk_id " hwnd)
        if (!pid || seen.Has(pid))
            continue
        seen[pid] := true
        proc := GetClientProcess(pid)
        if (proc.ok && NativeModsCoreLoadState(proc, pid) = "refused") {
            who := CharacterNameFromWindow(hwnd)
            out.Push(who = "" ? "PID " pid : who)
        }
    }
    return out
}

ShowDebugState() {
    global gCanOverride, gResolvedMapName, gResolvedMapPath, gLastReadStatus, gLastPosStatus
    global gMinimapSource
    global gLastRawX, gLastRawY, gResolvedOffsets, gResolvedBuildStamp, gFallbackOffsets
    global GAME_COORD_DIV_X, GAME_COORD_DIV_Y
    ; Re-read rather than taken from UpdateMapState: opening this dialog already
    ; took focus from the game, and that is when the cached state goes blank.
    nativeMm := ReadNativeMinimapState()
    msg := "CanOverride: " gCanOverride "`n"
        . "Tab source: " gMinimapSource " — Tab "
            . (!gCanOverride || gMinimapSource = "off" ? "reaches the game"
            : gMinimapSource = "mapspp" ? "opens Maps++"
            : nativeMm.available ? "reaches the game (it has a minimap here)"
            : "opens Maps++ (the game has none here)") "`n"
        . "Game minimap: " NativeMinimapSummary(nativeMm) "`n"
        . "Display: " GameDisplaySummary(GameContentRect()) "`n"
        . "ReadStatus: " gLastReadStatus "`n"
        . "PosStatus: " gLastPosStatus "`n"
        . "RawX: " gLastRawX " RawY: " gLastRawY "`n"
        . "MapName: " (gResolvedMapName = "" ? "<empty>" : gResolvedMapName) "`n"
        . "MapPath: " (gResolvedMapPath = "" ? "<missing>" : gResolvedMapPath) "`n`n"
        . "Build: " (gResolvedBuildStamp ? Format("0x{:08X}", gResolvedBuildStamp) : "<unresolved>") "`n"
    for _, name in SIGNATURE_NAMES {
        rva := GetResolvedOffset(name)
        msg .= "  " name ": " Format("0x{:08X}", rva) " (" OffsetSourceLabel(name) ")`n"
    }
    ; The whole position chain, so a "the coordinates are wrong" report can be
    ; resolved without guessing which link broke. Compare "game coords" against
    ; the numbers the game's own HUD is showing at the same moment:
    ;   they match      → the divisors and the position read are right, and any
    ;                     hover-readout discrepancy was where the cursor was
    ;   they differ     → GAME_COORD_DIV_X/Y or the position offsets are wrong
    ; "pixel -> back" re-inverts the marker's own pixel; it must land within a
    ; unit or two of the raw values above, whatever the calibration says.
    ; Opening this dialog means the tray menu already took focus from the game,
    ; and UpdateMapState blanks gResolvedMapName the moment that happens — so
    ; neither the map name nor the marker timer's last sample can be relied on
    ; here. Both are re-read from the process, which does not need focus.
    livePos := ReadRawPlayerPosition()

    chainMap := gResolvedMapName
    if (chainMap = "")
        chainMap := gCurrentMapName
    if (chainMap = "")
        chainMap := ResolveMapPath(ReadCurrentMapName()) != "" ? ReadCurrentMapName() : ""

    msg .= "`nPosition chain (" (chainMap = "" ? "no map resolved" : chainMap) ")`n"
    if livePos.ok {
        msg .= "  raw          " livePos.x ", " livePos.y "`n"
            . "  game coords  " GameCoordText(livePos.x, livePos.y) "`n"
            . "  << compare 'game coords' with the number on the game's own HUD >>`n"
    } else {
        msg .= "  raw          <could not read position>`n"
    }
    if (chainMap != "" && livePos.ok) {
        cal := GetCalibration(chainMap)
        px := WorldToOverlayPixels(livePos.x, livePos.y, chainMap)
        back := OverlayPixelsToWorld(px.x, px.y, chainMap)
        msg .= "  base pixel   " px.x ", " px.y "`n"
            . "  pixel -> back " (back.ok
                ? (back.x ", " back.y "   game " GameCoordText(back.x, back.y))
                : "<no usable calibration>") "`n"
            . "  calibration  multX=" Format("{:.10f}", cal.multX)
            . " addX=" Format("{:.4f}", cal.addX) "`n"
            . "               multY=" Format("{:.10f}", cal.multY)
            . " addY=" Format("{:.4f}", cal.addY) "`n"
    }
    ; Per-client reads. Mainly here to check the character class against what
    ; the game shows — classId is what picks avatars\c<N>.png in the radial
    ; menu, and -1 means "unreadable or not logged in yet".
    UpdateClientSnapshots()
    snapshots := GetClientSnapshots()
    msg .= "`nClients: " snapshots.Length "`n"
    for _, snap in snapshots {
        msg .= "  " (snap.charName = "" ? "PID " snap.pid : snap.charName)
            . " — class " snap.classId ", state " snap.gameState "`n"
    }
    ; Column-aligned offsets and a position chain — monospace or it is
    ; unreadable — so the dump is the dialog's detail block and the message
    ; above it says what to look at first.
    ShowMessage("Live state read from the game process."
        . "`n`nIf the coordinates look wrong, compare “game coords” in the"
        . " position chain against the numbers on the game's own HUD right now.",
        "Maps++ — Debug State", Map("detail", msg))
}

; ── Calibration handlers ─────────────────────────────────────────

CaptureCalibrationPoint(index) {
    global gOverlayVisible, gGui, gResolvedMapName, MINIMAP_MAP_INSET
    global gCalibrationPoint1, gCalibrationPoint2

    if !gOverlayVisible || !IsObject(gGui) || !gGui.Hwnd {
        ShowMessage("Open the Maps++ minimap first, then hover a landmark and capture again."
            . "`n`nOn a map where Tab opens the game's own minimap, use Open Maps++ minimap"
            . " (Tray → Minimap, or its hotkey) instead.",
            "Calibration", Map("severity", "warn"))
        return
    }
    if (gResolvedMapName = "") {
        ShowMessage("No resolved map name available yet.", "Calibration", Map("severity", "warn"))
        return
    }

    rawPos := ReadRawPlayerPosition()
    if !rawPos.ok {
        ShowMessage("Failed to read raw position from memory.", "Calibration",
            Map("severity", "danger"))
        return
    }

    CoordMode("Mouse", "Screen")
    MouseGetPos(&mx, &my)
    ; Client (0,0); map image origin is MINIMAP_MAP_INSET from client top-left.
    pt := Buffer(8, 0)
    DllCall("user32\ClientToScreen", "Ptr", gGui.Hwnd, "Ptr", pt)
    clx := NumGet(pt, 0, "Int")
    cly := NumGet(pt, 4, "Int")
    relX := mx - clx - MINIMAP_MAP_INSET
    relY := my - cly - MINIMAP_MAP_INSET
    if (relX < 0 || relY < 0 || relX >= MinimapDisplayW() || relY >= MinimapDisplayH()) {
        ShowMessage("Place your mouse over the minimap image before capturing.",
            "Calibration", Map("severity", "warn"))
        return
    }
    ; Calibration is stored in base (unscaled) map space, so a user calibrating
    ; at 150 % writes the same numbers as one calibrating at 100 %.
    scale := MinimapScaleFactor()
    relX := Round(relX / scale)
    relY := Round(relY / scale)

    point := {
        mapName: gResolvedMapName,
        rawX: rawPos.x,
        rawY: rawPos.y,
        px: relX,
        py: relY
    }

    if (index = 1) {
        gCalibrationPoint1 := point
    } else {
        gCalibrationPoint2 := point
    }

    ShowMessage(
        "Captured point " index " on " point.mapName ".",
        "Calibration",
        Map("severity", "success",
            "detail", "raw    " point.rawX ", " point.rawY "`n"
                    . "pixel  " point.px ", " point.py)
    )
}

ApplyCalibrationFromPoints() {
    global gCalibrationPoint1, gCalibrationPoint2, MAP_DIR

    if !IsObject(gCalibrationPoint1) || !IsObject(gCalibrationPoint2) {
        ShowMessage("Capture two points first (`Ctrl+Alt+1` and `Ctrl+Alt+2`).",
            "Calibration", Map("severity", "warn"))
        return
    }
    if (gCalibrationPoint1.mapName != gCalibrationPoint2.mapName) {
        ShowMessage("Points are from different maps. Recapture both points on the same map.",
            "Calibration", Map("severity", "warn"))
        return
    }

    dxRaw := gCalibrationPoint2.rawX - gCalibrationPoint1.rawX
    dyRaw := gCalibrationPoint2.rawY - gCalibrationPoint1.rawY
    dxPx := gCalibrationPoint2.px - gCalibrationPoint1.px
    dyPx := gCalibrationPoint2.py - gCalibrationPoint1.py
    if (dxRaw = 0 || dyRaw = 0) {
        ShowMessage("Captured points are invalid (raw delta is zero). Choose two separated landmarks.",
            "Calibration", Map("severity", "warn"))
        return
    }

    multX := dxPx / dxRaw
    multY := dyPx / dyRaw
    addX := gCalibrationPoint1.px - (gCalibrationPoint1.rawX * multX)
    addY := gCalibrationPoint1.py - (gCalibrationPoint1.rawY * multY)

    mapName := gCalibrationPoint1.mapName
    gCalibrationCache.Delete(mapName)
    dims := GetImageDimensionsFromFile(MAP_DIR "\" mapName)
    SaveExplicitCalibrationToIni(mapName, multX, addX, multY, addY, dims.w, dims.h)

    calibText := "[" mapName "]`n"
        . "mode=explicit`n"
        . "multX=" Format("{:.10f}", multX) "`n"
        . "addX=" Format("{:.10f}", addX) "`n"
        . "multY=" Format("{:.10f}", multY) "`n"
        . "addY=" Format("{:.10f}", addY) "`n"
    if (dims.w > 0 && dims.h > 0) {
        calibText .= "sourceW=" dims.w "`nsourceH=" dims.h "`n"
    }

    A_Clipboard := calibText
    ShowMessage(
        "Calibration saved for " mapName ".`n`n"
        . CombinedCalibrationPath() "`n`n"
        . "The section below is on your clipboard too.",
        "Calibration",
        Map("severity", "success", "detail", calibText)
    )
}

; ── Calibration panel ────────────────────────────────────────────
;
; Calibration already worked; it was just invisible. The two-point capture is
; Ctrl+Alt+1 / Ctrl+Alt+2 and nothing in the UI ever said so, which is why 28
; maps ship calibrated and no user has ever added a 29th.
;
; This panel is a frontend over that same flow — CaptureCalibrationPoint and
; ApplyCalibrationFromPoints are untouched. Capturing still happens by hotkey
; rather than a button, and deliberately so: the capture reads the cursor's
; position over the *minimap*, so a button that had to be clicked would move the
; mouse off the very point being captured.
;
; It also verifies. The panel shows the player's position as this app would
; display it, next to a reminder to compare it with the game's own HUD — the
; check that would have caught the missing coordinate offsets immediately.

global gCalibGui := 0
global gCalibIsNative := false
global CALIB_PUSH_MS := 250

ShowCalibrationPanel() {
    global gCalibGui, gCalibIsNative, CALIB_PUSH_MS

    if (IsObject(gCalibGui) && gCalibGui.Hwnd) {
        try WinActivate("ahk_id " gCalibGui.Hwnd)
        return
    }
    if (IsNativeInterface() || !_Calib_CanUseWebView()) {
        _Calib_ShowNative()
        return
    }

    dllDir := (A_PtrSize = 8) ? "64bit" : "32bit"
    wvSettings := { DllPath: A_ScriptDir "\Lib\" dllDir "\WebView2Loader.dll",
                    DefaultWidth: 560, DefaultHeight: 640 }
    g := WebViewGui("-Caption +AlwaysOnTop +Resize", "Maps++ — Map Calibration", , wvSettings)
    g.OnEvent("Close", (*) => _Calib_Close())
    g.WebMessageReceived(WebMsgHandler(_Calib_OnWebMessage))
    g.DOMContentLoaded((*) => SetTimer(_Calib_Push, -50))
    g.Navigate(UiPageUrl("ui/calibration/index.html"))
    gCalibGui := g
    gCalibIsNative := false
    g.Show("w560 h640 Center")
    ; Live state: the captured points, the player's position and whatever the
    ; cursor is over all change while the panel sits open.
    SetTimer(_Calib_Push, CALIB_PUSH_MS)
}

_Calib_CanUseWebView() {
    return FileExist(A_ScriptDir "\Lib\WebViewToo.ahk")
        && FileExist(A_ScriptDir "\ui\calibration\index.html")
}

_Calib_Close() {
    global gCalibGui, gCalibIsNative, gCalibNativeText
    SetTimer(_Calib_Push, 0)
    if IsObject(gCalibGui)
        try gCalibGui.Destroy()
    gCalibGui := 0
    gCalibIsNative := false
    gCalibNativeText := 0
}

; Everything the panel renders, gathered in one place so the native and web
; frontends cannot drift.
_Calib_State() {
    global gResolvedMapName, gCurrentMapName, gOverlayVisible, gOverlayHover
    global gCalibrationPoint1, gCalibrationPoint2, MAP_DIR

    mapName := gResolvedMapName != "" ? gResolvedMapName : gCurrentMapName
    imagePath := (mapName != "") ? ResolveMapPath(mapName) : ""
    hasCal := (mapName != "") && (Type(LoadCalibrationFromIni(mapName)) = "Map")

    pos := ReadRawPlayerPosition()
    st := {
        mapName: mapName,
        hasImage: (imagePath != ""),
        hasCalibration: hasCal,
        overlayOpen: gOverlayVisible ? true : false,
        hovering: gOverlayHover ? true : false,
        posOk: pos.ok ? true : false,
        rawX: pos.ok ? pos.x : 0,
        rawY: pos.ok ? pos.y : 0,
        gameText: pos.ok ? GameCoordText(pos.x, pos.y) : "",
        p1: _Calib_PointInfo(gCalibrationPoint1, mapName),
        p2: _Calib_PointInfo(gCalibrationPoint2, mapName),
        calText: ""
    }
    if hasCal {
        cal := GetCalibration(mapName)
        st.calText := Format("multX {:.6f}  addX {:.2f}   multY {:.6f}  addY {:.2f}",
            cal.multX, cal.addX, cal.multY, cal.addY)
    }
    ; Both points must belong to the map being calibrated, and two points on top
    ; of each other cannot define a scale — ApplyCalibrationFromPoints refuses
    ; that, so the panel refuses it first and says why.
    st.canApply := false
    st.blocker := ""
    if (!st.p1.captured || !st.p2.captured) {
        st.blocker := "Capture both points first."
    } else if (st.p1.mapName != st.p2.mapName) {
        st.blocker := "The two points are from different maps. Recapture both here."
    } else if (st.p1.rawX = st.p2.rawX || st.p1.rawY = st.p2.rawY) {
        st.blocker := "Both points share an X or Y position. Stand somewhere diagonally apart."
    } else {
        st.canApply := true
    }
    return st
}

_Calib_PointInfo(pt, mapName) {
    if !IsObject(pt) {
        return { captured: false, mapName: "", rawX: 0, rawY: 0, px: 0, py: 0, text: "" }
    }
    return {
        captured: true,
        mapName: pt.mapName,
        rawX: pt.rawX, rawY: pt.rawY, px: pt.px, py: pt.py,
        text: "raw " pt.rawX ", " pt.rawY "   ->   pixel " pt.px ", " pt.py
            . (pt.mapName != mapName ? "   (from " pt.mapName ")" : "")
    }
}

; The one timer target for both frontends, so neither can be left un-refreshed.
_Calib_Push() {
    global gCalibGui, gCalibIsNative
    if !IsObject(gCalibGui)
        return
    if gCalibIsNative {
        _Calib_PushNative()
        return
    }
    st := _Calib_State()
    try gCalibGui.PostWebMessageAsJson('{"type":"calib-state"'
        . ',"mapName":' _JSON_Str(st.mapName)
        . ',"hasImage":' (st.hasImage ? "true" : "false")
        . ',"hasCalibration":' (st.hasCalibration ? "true" : "false")
        . ',"overlayOpen":' (st.overlayOpen ? "true" : "false")
        . ',"hovering":' (st.hovering ? "true" : "false")
        . ',"posOk":' (st.posOk ? "true" : "false")
        . ',"rawText":' _JSON_Str(st.posOk ? (st.rawX ", " st.rawY) : "")
        . ',"gameText":' _JSON_Str(st.gameText)
        . ',"calText":' _JSON_Str(st.calText)
        . ',"p1":' _JSON_Str(st.p1.text)
        . ',"p2":' _JSON_Str(st.p2.text)
        . ',"canApply":' (st.canApply ? "true" : "false")
        . ',"blocker":' _JSON_Str(st.blocker) '}')
}

_Calib_OnWebMessage(wv, args) {
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
            _Calib_Push()
        ; Applying always ends in a ShowMessage reporting the round-trip, and a
        ; dialog raised from inside this COM callback could never be answered —
        ; see the dialogs.ahk header.
        case "apply":
            DeferFromWebMessage(_Calib_Apply)
        case "reset":
            _Calib_ResetPoints()
        case "open-maps-folder":
            try Run('explorer.exe "' MAP_DIR '"')
        case "close":
            _Calib_Close()
    }
}

_Calib_ResetPoints() {
    global gCalibrationPoint1, gCalibrationPoint2
    gCalibrationPoint1 := 0
    gCalibrationPoint2 := 0
    _Calib_Push()
}

; Applies and then immediately proves the result, rather than trusting it: the
; player's own position is projected through the new calibration and inverted
; back, so a calibration that does not round-trip is visible at once.
_Calib_Apply() {
    global gCalibGui, gCalibIsNative
    ; The WebView entry arrives a tick late, by which time the panel may be gone.
    if !(IsObject(gCalibGui) && gCalibGui.Hwnd)
        return
    st := _Calib_State()
    if !st.canApply {
        _Calib_Toast("error", st.blocker)
        return
    }
    ApplyCalibrationFromPoints()
    _Calib_ResetPoints()

    verdict := "Saved."
    pos := ReadRawPlayerPosition()
    if (pos.ok && st.mapName != "") {
        px := WorldToOverlayPixels(pos.x, pos.y, st.mapName)
        back := OverlayPixelsToWorld(px.x, px.y, st.mapName)
        if back.ok {
            verdict .= " Your position now reads as " GameCoordText(back.x, back.y)
                . " — compare that with the game's own coordinates."
        }
    }
    LogInfo("Calibration", "Saved calibration for " st.mapName)
    _Calib_Toast("info", verdict)
    _Calib_Push()
}

_Calib_Toast(level, text) {
    global gCalibGui, gCalibIsNative
    if (IsObject(gCalibGui) && !gCalibIsNative) {
        try gCalibGui.PostWebMessageAsJson('{"type":"toast","level":' _JSON_Str(level)
            . ',"text":' _JSON_Str(text) '}')
        return
    }
    TrayTip(text, "Map Calibration", level = "error" ? "Iconx" : "Iconi")
}

; ── Calibration panel (native) ───────────────────────────────────
; Same information, plain controls. Refreshed on the same timer.

global gCalibNativeText := 0

_Calib_ShowNative() {
    global gCalibGui, gCalibIsNative, gCalibNativeText, CALIB_PUSH_MS
    g := Gui("+AlwaysOnTop -MaximizeBox", "Maps++ — Map Calibration")
    g.MarginX := 12
    g.MarginY := 12
    g.SetFont("s9", "Segoe UI")
    g.Add("Text", "w440",
        "Stand at a landmark in game, put the mouse on that same spot on the "
        . "minimap, then press Ctrl+Alt+1. Do it again somewhere diagonally "
        . "away with Ctrl+Alt+2, then Apply.")
    gCalibNativeText := g.Add("Text", "xm y+10 w440 r12", "")
    g.Add("Button", "xm y+10 w110 Default", "Apply").OnEvent("Click", (*) => _Calib_Apply())
    g.Add("Button", "x+8 w110", "Reset points").OnEvent("Click", (*) => _Calib_ResetPoints())
    g.Add("Button", "x+8 w110", "Open maps folder")
        .OnEvent("Click", (*) => _Calib_OpenMapsFolder())
    g.Add("Button", "x+8 w80", "Close").OnEvent("Click", (*) => _Calib_Close())
    g.OnEvent("Close", (*) => _Calib_Close())
    g.OnEvent("Escape", (*) => _Calib_Close())
    gCalibGui := g
    gCalibIsNative := true
    _Calib_PushNative()
    g.Show("AutoSize")
    SetTimer(_Calib_Push, CALIB_PUSH_MS)
}

_Calib_OpenMapsFolder() {
    global MAP_DIR
    try Run('explorer.exe "' MAP_DIR '"')
}

_Calib_PushNative() {
    global gCalibNativeText
    if !IsObject(gCalibNativeText)
        return
    st := _Calib_State()
    txt := "Map: " (st.mapName = "" ? "<none — open the minimap on a map first>" : st.mapName) "`n"
        . "Image: " (st.hasImage ? "found" : "MISSING from maps\") "`n"
        . "Calibration: " (st.hasCalibration ? st.calText : "none yet") "`n`n"
        . "Point 1: " (st.p1.captured ? st.p1.text : "<not captured>") "`n"
        . "Point 2: " (st.p2.captured ? st.p2.text : "<not captured>") "`n`n"
        . "Your position: " (st.posOk ? ("raw " st.rawX ", " st.rawY "   shown as " st.gameText)
                                      : "<could not read>") "`n"
        . "  (compare 'shown as' with the coordinates on the game's own HUD)`n`n"
        . (st.canApply ? "Ready to apply." : st.blocker)
    gCalibNativeText.Value := txt
}

ExportCurrentCalibrationToFile() {
    global gResolvedMapName
    if (gResolvedMapName = "") {
        ShowMessage("Enter a map with a custom minimap first.", "Calibration",
            Map("severity", "warn"))
        return
    }
    global MAP_DIR
    cal := GetCalibration(gResolvedMapName)
    dims := GetImageDimensionsFromFile(MAP_DIR "\" gResolvedMapName)
    SaveExplicitCalibrationToIni(gResolvedMapName, cal.multX, cal.addX, cal.multY, cal.addY, dims.w, dims.h)
    gCalibrationCache.Delete(gResolvedMapName)
    ShowMessage("Wrote section [" gResolvedMapName "] in " CombinedCalibrationPath(),
        "Calibration", Map("severity", "success"))
}

; ── Overlay display ──────────────────────────────────────────────

ShowOrToggleOverlay(mapName, mapPath) {
    global gOverlayVisible, gCurrentMapName, gCurrentMapPath, gGui, gPic, gTrackedGameHwnd
    global MINIMAP_MAP_INSET, MINIMAP_COLOR_GOLD
    if WinActive(GAME_WIN_FILTER) {
        gTrackedGameHwnd := WinActive(GAME_WIN_FILTER)
    }

    pos := GetOverlayPositionForGameWindow()
    xPos := pos.x
    yPos := pos.y
    ; Displayed size (base map space × user scale).
    mapW := MinimapDisplayW()
    mapH := MinimapDisplayH()
    ; Explicit client size — otherwise Gui auto-size can omit edges.
    totalW := mapW + 2 * MINIMAP_MAP_INSET
    totalH := mapH + 2 * MINIMAP_MAP_INSET
    showOpts := "x" xPos " y" yPos " w" totalW " h" totalH " NoActivate"

    if !IsObject(gGui) {
        ; WS_EX_NOACTIVATE: clicks on the minimap do not steal focus from the game.
        gGui := Gui("+AlwaysOnTop -Caption +ToolWindow +E0x08000000")
        gGui.MarginX := 0
        gGui.MarginY := 0
        ; Outer 1px accent shows as Gui background; black rect at (1,1) leaves that ring; map covers center.
        gGui.BackColor := MINIMAP_COLOR_GOLD
        borderBlack := gGui.AddText(
            "x1 y1 w" (totalW - 2) " h" (totalH - 2) " Background000000",
            ""
        )
        DllCall("uxtheme\SetWindowTheme", "Ptr", borderBlack.Hwnd, "WStr", "", "WStr", "")
        gPic := gGui.AddPicture(
            "x" MINIMAP_MAP_INSET " y" MINIMAP_MAP_INSET " w" mapW " h" mapH,
            mapPath
        )
        EnsureMarkerControl()
        gGui.Show(showOpts)
        ApplyOverlayOpacity()
        gOverlayVisible := true
        gCurrentMapName := mapName
        gCurrentMapPath := mapPath
        SetTimer(UpdateMarkerPosition, 60)
        UpdateMarkerPosition()
        FireAddonHook("OnOverlayShow", mapName)
        return
    }

    if (gCurrentMapPath != mapPath) {
        gPic.Value := mapPath
        gCurrentMapName := mapName
        gCurrentMapPath := mapPath
    }

    if gOverlayVisible {
        gGui.Hide()
        gOverlayVisible := false
        SetTimer(UpdateMarkerPosition, 0)
    } else {
        gGui.Show(showOpts)
        ApplyOverlayOpacity()
        gOverlayVisible := true
        SetTimer(UpdateMarkerPosition, 60)
        UpdateMarkerPosition()
        FireAddonHook("OnOverlayShow", mapName)
    }
}

UpdateMarkerPosition() {
    global gOverlayVisible, gMarkerDot, gLastPosStatus, gLastRawX, gLastRawY
    global MINIMAP_MAP_INSET, gOverlayDragging

    if !IsObject(gGui) || !gGui.Hwnd {
        return
    }

    UpdateOverlayHoverState()
    UpdateCoordReadout()
    UpdateWaypoint()

    ; Keep the overlay anchored to the game window as it moves — but never
    ; while the user is dragging it, or the drag fights this timer.
    if !gOverlayDragging {
        pos := GetOverlayPositionForGameWindow()
        gGui.Move(pos.x, pos.y)
    }

    EnsureMarkerControl()
    if !IsObject(gMarkerDot) {
        return
    }

    cached := GetCachedProcessHandleAndBase()
    if !cached.ok {
        gLastPosStatus := "pos_process_not_found"
        return
    }

    ; X and Y are contiguous (4 bytes apart) — read both in one 8-byte call.
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
        gLastPosStatus := "pos_read_failed"
        ReleaseCachedProcessHandle()
        return
    }

    rawX := NumGet(posBuf, 0, "Int")
    rawY := NumGet(posBuf, 4, "Int")
    gLastRawX := rawX
    gLastRawY := rawY
    ; Calibration yields base-space pixels; scale them for the displayed size.
    markerPos := WorldToOverlayPixels(rawX, rawY, gCurrentMapName)
    scale := MinimapScaleFactor()
    size := MinimapMarkerSize()
    px := Round(markerPos.x * scale)
    py := Round(markerPos.y * scale)

    px := Clamp(px, 0, MinimapDisplayW() - size)
    py := Clamp(py, 0, MinimapDisplayH() - size)
    gMarkerDot.Move(px + MINIMAP_MAP_INSET, py + MINIMAP_MAP_INSET, size, size)
    gMarkerDot.Visible := true
    gLastPosStatus := "ok x=" rawX " y=" rawY
}
