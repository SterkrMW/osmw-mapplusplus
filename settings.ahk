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

    tabNames := ["Launcher", "Hotkeys"]
    for c in contributors
        tabNames.Push(c.label)
    tabNames.Push("Addons")
    hotkeysTabIndex := 2
    addonsTabIndex := tabNames.Length

    g := Gui("+AlwaysOnTop -MinimizeBox", "osMW Maps++ — Settings")
    gSettingsGui := g
    g.OnEvent("Close", (*) => _Settings_Close())
    g.OnEvent("Escape", (*) => _Settings_Close())

    tab := g.Add("Tab3", "w470 h520", tabNames)
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

    secondaryDdl.Value := _Settings_MonitorIndexToChoice(gSecondaryMonitorOverride)

    ; ---- Hotkeys tab ----
    tab.UseTab(hotkeysTabIndex)
    hotkeyRows := _Settings_BuildHotkeysTab(g, contentX, contentY)

    ; ---- Addon-contributed tabs ----
    saveHandlers := []
    ctx := { gui: g, tab: tab, saveHandlers: saveHandlers }
    for i, c in contributors {
        tab.UseTab(2 + i)
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
    g.Add("Button", "x291 y538 w90 Default", "OK").OnEvent("Click", DoSave)
    g.Add("Button", "x389 y538 w90", "Cancel").OnEvent("Click", (*) => _Settings_Close())

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

        if IsObject(hotkeyRows) && hotkeyRows.Length {
            if !_Settings_SaveHotkeys(hotkeyRows) {
                return
            }
        }

        for entry in addonChecks
            SetAddonEnabled(entry.name, entry.ctrl.Value ? true : false)

        for handler in saveHandlers {
            try handler()
        }

        ApplyAllHotkeys()
        RebuildTrayMenu()

        _Settings_Close()
        TrayTip("AHK Minimap", "Settings saved.", "Iconi")
    }
}

_Settings_Close() {
    global gSettingsGui, gSettingsHotkeyRows
    CancelHotkeyCapture()
    gSettingsHotkeyRows := 0
    if IsObject(gSettingsGui) {
        try gSettingsGui.Destroy()
    }
    gSettingsGui := 0
}

_Settings_BuildHotkeysTab(g, contentX, contentY) {
    g.SetFont()
    g.Add("Text", "x" contentX " y" contentY " w430",
        "Click a shortcut, then press the new keys. Esc cancels.")
    rows := []
    prevCategory := ""
    y := contentY + 28
    for action in GetHotkeyActionsForSettings() {
        cat := action.Has("category") ? action["category"] : "Other"
        if (cat != prevCategory) {
            if (prevCategory != "")
                y += 10
            g.SetFont("s9 Bold")
            g.Add("Text", "x" contentX " y" y " w430 c333333", cat)
            g.SetFont()
            y += 22
            prevCategory := cat
        }
        g.Add("Text", "x" (contentX + 8) " y" y " w232", action["label"])
        btn := g.Add("Button", "x" (contentX + 250) " y" (y - 3) " w120", FormatHotkeyDisplay(action["chord"]))
        resetBtn := g.Add("Button", "x" (contentX + 376) " y" (y - 3) " w54", "Reset")
        row := Map(
            "id", action["id"],
            "action", action,
            "pending", action["chord"],
            "default", action["default"],
            "button", btn,
            "allowMouse", action.Has("allowMouse") && action["allowMouse"]
        )
        btn.OnEvent("Click", _Settings_HotkeyBtnClick.Bind(row))
        resetBtn.OnEvent("Click", _Settings_HotkeyReset.Bind(row))
        rows.Push(row)
        y += 28
    }
    resetAllBtn := g.Add("Button", "x" contentX " y" (y + 10) " w140", "Reset all to defaults")
    resetAllBtn.OnEvent("Click", _Settings_HotkeyResetAll.Bind(rows))
    global gSettingsHotkeyRows := rows
    return rows
}

_Settings_HotkeyBtnClick(row, *) {
    StartHotkeyCapture(row, (chord) => row["pending"] := chord)
}

_Settings_HotkeyReset(row, *) {
    row["pending"] := row["default"]
    row["button"].Text := FormatHotkeyDisplay(row["default"])
}

_Settings_HotkeyResetAll(rows, *) {
    for row in rows {
        row["pending"] := row["default"]
        row["button"].Text := FormatHotkeyDisplay(row["default"])
    }
}

_Settings_SaveHotkeys(rows) {
    global gHotkeyActions, gHotkeyReserved
    seen := Map()
    for row in rows {
        chord := NormalizeHotkeyChord(row["pending"])
        if !IsHotkeyChordValid(chord) {
            TrayTip("Hotkeys", "Invalid shortcut for " row["action"]["label"] ".", "Iconx")
            return false
        }
        for reserved in gHotkeyReserved {
            if (StrLower(chord) = StrLower(reserved)) {
                TrayTip("Hotkeys", FormatHotkeyDisplay(chord) " is reserved (Reload, Exit, or debug/calibration).", "Iconx")
                return false
            }
        }
        if seen.Has(chord) {
            TrayTip("Hotkeys", "Duplicate shortcut: " FormatHotkeyDisplay(chord), "Iconx")
            return false
        }
        seen[chord] := row["action"]["label"]
    }
    for row in rows {
        gHotkeyActions[row["id"]]["chord"] := row["pending"]
    }
    SaveHotkeyOverrides()
    return true
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
