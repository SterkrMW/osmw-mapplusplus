#Requires AutoHotkey v2.0

; ── Settings window ──────────────────────────────────────────────
; A single tabbed window consolidating every user preference. Core builds the
; Launcher and Addons tabs; each addon that registers an "OnSettings" hook gets
; its own tab (see the hook contract at the bottom of this file).

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
    _Settings_Build()
}

_Settings_Build() {
    global gSettingsGui, gAddonHooks, gDisabledAddons
    global gGamePath, gGameArgs, gLaunchOnStartup, gMultiClientCount, gMultiClientDelay
    global gPrimaryMonitorOverride, gSecondaryMonitorOverride

    ; Addons that contribute a settings tab.
    contributors := []
    for _, am in gAddonHooks {
        if am.Has("OnSettings") {
            label := am.Has("settingsLabel") ? am["settingsLabel"]
                : (am.Has("name") ? am["name"] : "Addon")
            contributors.Push({ map: am, label: label })
        }
    }

    tabNames := ["Launcher"]
    for c in contributors
        tabNames.Push(c.label)
    tabNames.Push("Addons")
    addonsTabIndex := tabNames.Length

    g := Gui("+AlwaysOnTop -MinimizeBox", "osMW Maps++ — Settings")
    gSettingsGui := g
    g.OnEvent("Close", (*) => _Settings_Close())
    g.OnEvent("Escape", (*) => _Settings_Close())

    tab := g.Add("Tab3", "w470 h330", tabNames)
    ; Absolute top-left of each tab's content area. Every tab's FIRST control must
    ; anchor here: xp/yp would reference the previous tab's last control (which
    ; sits at the bottom), so switching tabs alone does not reset the cursor.
    tab.GetPos(&_tabX, &_tabY, &_tabW, &_tabH)
    contentX := _tabX + 16
    contentY := _tabY + 42

    ; ---- Launcher tab ----
    tab.UseTab(1)
    g.Add("Text", "x" contentX " y" contentY " Section w130", "Game path:")
    gamePathEdit := g.Add("Edit", "x+10 yp-3 w220 ReadOnly", gGamePath)
    g.Add("Button", "x+6 yp-1 w60", "Browse…").OnEvent("Click", _Settings_BrowseGamePath.Bind(gamePathEdit))

    g.Add("Text", "xs y+14 w130", "Game args:")
    gameArgsEdit := g.Add("Edit", "x+10 yp-3 w286", gGameArgs)

    autoStartCb := g.Add("CheckBox", "xs y+16 w340", "Start Maps++ automatically when Windows starts")
    autoStartCb.Value := IsRunOnStartupEnabled() ? 1 : 0

    startupCb := g.Add("CheckBox", "xs y+8 w340", "Launch a game client when Maps++ starts")
    startupCb.Value := gLaunchOnStartup ? 1 : 0

    g.Add("Text", "xs y+16 w130", "Multi-client count:")
    countEdit := g.Add("Edit", "x+10 yp-3 w80 Number", String(gMultiClientCount))

    g.Add("Text", "xs y+14 w130", "Launch delay (ms):")
    delayEdit := g.Add("Edit", "x+10 yp-3 w80 Number", String(gMultiClientDelay))

    g.Add("Text", "xs y+16 w130", "Primary monitor:")
    primaryDdl := g.Add("DropDownList", "x+10 yp-3 w220", _Settings_MonitorChoices())
    primaryDdl.Value := _Settings_MonitorIndexToChoice(gPrimaryMonitorOverride)

    g.Add("Text", "xs y+14 w130", "Secondary monitor:")
    secondaryDdl := g.Add("DropDownList", "x+10 yp-3 w220", _Settings_MonitorChoices())
    secondaryDdl.Value := _Settings_MonitorIndexToChoice(gSecondaryMonitorOverride)

    ; ---- Addon-contributed tabs ----
    saveHandlers := []
    ctx := { gui: g, tab: tab, saveHandlers: saveHandlers }
    for i, c in contributors {
        tab.UseTab(1 + i)
        ; Section anchor (absolute) so addons position with xs/ys, never tab-relative coords.
        g.Add("Text", "x" contentX " y" contentY " Section w0 h0")
        try {
            c.map["OnSettings"](ctx)
        } catch as err {
            g.Add("Text", "xs y+8 w430", "Settings failed to load: " err.Message)
        }
    }

    ; ---- Addons tab ----
    tab.UseTab(addonsTabIndex)
    g.Add("Text", "x" contentX " y" contentY " Section w430",
        "Enable or disable addons. Reload (Ctrl+Alt+R) to fully apply changes.")
    addonChecks := []
    for _, am in gAddonHooks {
        nm := am.Has("name") ? am["name"] : ""
        if (nm = "")
            continue
        cb := g.Add("CheckBox", "xs y+12 w320", nm)
        cb.Value := (gDisabledAddons.Has(nm) && gDisabledAddons[nm]) ? 0 : 1
        addonChecks.Push({ name: nm, ctrl: cb })
    }

    ; ---- Buttons ----
    tab.UseTab()
    g.Add("Button", "x291 y348 w90 Default", "OK").OnEvent("Click", DoSave)
    g.Add("Button", "x389 y348 w90", "Cancel").OnEvent("Click", (*) => _Settings_Close())

    g.Show("AutoSize")

    DoSave(*) {
        global gGamePath, gGameArgs, gLaunchOnStartup, gMultiClientCount, gMultiClientDelay
        global gPrimaryMonitorOverride, gSecondaryMonitorOverride

        cntTxt := Trim(countEdit.Value)
        if (!IsInteger(cntTxt) || Integer(cntTxt) < 1) {
            TrayTip("AHK Minimap", "Multi-client count must be a whole number of 1 or more.", "Iconx")
            return
        }
        dlyTxt := Trim(delayEdit.Value)
        if (!IsInteger(dlyTxt) || Integer(dlyTxt) < 0) {
            TrayTip("AHK Minimap", "Launch delay must be 0 or a positive number of milliseconds.", "Iconx")
            return
        }

        gGamePath := gamePathEdit.Value
        gGameArgs := Trim(gameArgsEdit.Value)
        gLaunchOnStartup := startupCb.Value ? true : false
        gMultiClientCount := Integer(cntTxt)
        gMultiClientDelay := Integer(dlyTxt)
        gPrimaryMonitorOverride := _Settings_ChoiceToMonitorIndex(primaryDdl.Value)
        gSecondaryMonitorOverride := _Settings_ChoiceToMonitorIndex(secondaryDdl.Value)
        SaveLauncherConfig()
        SetRunOnStartup(autoStartCb.Value ? true : false)

        for entry in addonChecks
            SetAddonEnabled(entry.name, entry.ctrl.Value ? true : false)

        for handler in saveHandlers {
            try handler()
        }

        _Settings_Close()
        TrayTip("AHK Minimap", "Settings saved.", "Iconi")
    }
}

_Settings_Close() {
    global gSettingsGui
    if IsObject(gSettingsGui) {
        try gSettingsGui.Destroy()
    }
    gSettingsGui := 0
}

_Settings_BrowseGamePath(editCtrl, *) {
    global PROCESS_EXE
    selected := FileSelect(1, A_ScriptDir, "Locate " PROCESS_EXE " (game executable)", "Executables (*.exe)")
    if (selected = "")
        return
    editCtrl.Value := selected
}

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

; ── OnSettings hook contract ─────────────────────────────────────
; An addon opts into the settings window by adding to its RegisterAddon map:
;     "OnSettings",    MyAddon_OnSettings   ; fn(ctx) — adds controls, pushes a save handler
;     "settingsLabel", "My Addon"           ; optional tab title; defaults to addon "name"
; ctx = { gui, tab, saveHandlers }. The core selects the addon's tab and adds a
; Section anchor before calling, so the addon positions controls with xs/ys
; (e.g. gui.Add("Text", "xs y+8 ...")). The addon captures its control refs and
; does ctx.saveHandlers.Push(() => /* persist its controls */). The save handler
; runs when the user clicks OK. No core change is needed for a new addon to appear.
