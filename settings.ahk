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

    tabNames := ["Launcher", "Minimap", "Hotkeys"]
    for c in contributors
        tabNames.Push(c.label)
    tabNames.Push("Addons")
    minimapTabIndex := 2
    hotkeysTabIndex := 3
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

    ; The Hotkeys tab's row count grows as addons register more hotkey actions
    ; (Tab3 clips children past its own rectangle, so a fixed height silently
    ; hides whatever category doesn't fit — that's what happened to Chat and
    ; View Mode once enough rows were registered). Grow the tab — and the
    ; OK/Cancel row below it — to fit however many rows currently exist.
    hotkeysBottomY := _Settings_HotkeysContentBottom(contentY) + 24 + 14
    tabHeight := Max(_tabH, hotkeysBottomY - _tabY)
    if (tabHeight != _tabH)
        tab.Move(, , , tabHeight)
    buttonY := _tabY + tabHeight + 18

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

    ; ---- Minimap tab ----
    tab.UseTab(minimapTabIndex)
    minimapCtrls := _Settings_BuildMinimapTab(g, contentX, contentY)

    ; ---- Hotkeys tab ----
    tab.UseTab(hotkeysTabIndex)
    hotkeyRows := _Settings_BuildHotkeysTab(g, contentX, contentY)

    ; ---- Addon-contributed tabs ----
    saveHandlers := []
    ctx := { gui: g, tab: tab, saveHandlers: saveHandlers }
    for i, c in contributors {
        tab.UseTab(hotkeysTabIndex + i)
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
    g.Add("Button", "x291 y" buttonY " w90 Default", "OK").OnEvent("Click", DoSave)
    g.Add("Button", "x389 y" buttonY " w90", "Cancel").OnEvent("Click", (*) => _Settings_Close())

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

        if !_Settings_SaveMinimap(minimapCtrls) {
            return
        }

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

; Walks hotkey actions in Settings display order, starting at absolute y
; `startY`. Calls onCategory(cat, y) once per new category header and
; onAction(action, y) once per hotkey row (either may be 0 to skip). Returns
; the y just past the last row. Shared by _Settings_BuildHotkeysTab (which
; draws controls) and _Settings_HotkeysContentBottom (which only measures), so
; the tab's actual layout and its computed height can never drift apart.
_Settings_WalkHotkeyLayout(startY, onCategory, onAction) {
    y := startY
    prevCategory := ""
    for action in GetHotkeyActionsForSettings() {
        cat := action.Has("category") ? action["category"] : "Other"
        if (cat != prevCategory) {
            if (prevCategory != "")
                y += 10
            if onCategory
                onCategory(cat, y)
            y += 22
            prevCategory := cat
        }
        if onAction
            onAction(action, y)
        y += 28
    }
    return y
}

; Absolute y of the "Reset all to defaults" button, given the tab content
; area's top (contentY). Used to size the Hotkeys tab before its rows exist.
_Settings_HotkeysContentBottom(contentY) {
    return _Settings_WalkHotkeyLayout(contentY + 28, 0, 0) + 10
}

_Settings_AddHotkeyCategoryHeader(g, contentX, cat, y) {
    g.SetFont("s9 Bold")
    g.Add("Text", "x" contentX " y" y " w430 c333333", cat)
    g.SetFont()
}

_Settings_AddHotkeyRow(g, contentX, rows, action, y) {
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
}

_Settings_BuildHotkeysTab(g, contentX, contentY) {
    g.SetFont()
    g.Add("Text", "x" contentX " y" contentY " w430",
        "Click a shortcut, then press the new keys. Esc cancels.")
    rows := []
    lastRowY := _Settings_WalkHotkeyLayout(
        contentY + 28,
        _Settings_AddHotkeyCategoryHeader.Bind(g, contentX),
        _Settings_AddHotkeyRow.Bind(g, contentX, rows)
    )
    resetAllBtn := g.Add("Button", "x" contentX " y" (lastRowY + 10) " w140", "Reset all to defaults")
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
    global gHotkeyActions
    for row in rows {
        chord := NormalizeHotkeyChord(row["pending"])
        if !IsHotkeyChordValid(chord) {
            TrayTip("Hotkeys", "Invalid shortcut for " row["action"]["label"] ".", "Iconx")
            return false
        }
        conflict := GetHotkeyConflictAction(chord, row["id"])
        if (conflict != "") {
            TrayTip("Hotkeys", FormatHotkeyDisplay(chord) " — already used by " conflict ".", "Iconx")
            return false
        }
    }
    for row in rows {
        gHotkeyActions[row["id"]]["chord"] := row["pending"]
    }
    SaveHotkeyOverrides()
    return true
}

; ── Minimap tab ──────────────────────────────────────────────────

_Settings_BuildMinimapTab(g, contentX, contentY) {
    global gMinimapScale, gMinimapOpacity, gMinimapAnchor, gMinimapOffsetX, gMinimapOffsetY
    global gMinimapKeepOpen, MINIMAP_ANCHORS, OVERLAY_W, OVERLAY_H

    g.SetFont()
    g.Add("Text", "x" contentX " y" contentY " Section w430",
        "Size and placement of the minimap overlay. Map calibration is unaffected by scale.")

    g.Add("Text", "xs y+16 w130", "Size:")
    scaleSlider := g.Add("Slider", "x+10 yp-3 w200 Range50-200 TickInterval25 Line5 Page25", gMinimapScale)
    scaleText := g.Add("Text", "x+10 yp+3 w110", "")
    _Settings_UpdateScaleText(scaleText, gMinimapScale)
    scaleSlider.OnEvent("Change", (ctrl, *) => _Settings_UpdateScaleText(scaleText, ctrl.Value))

    g.Add("Text", "xs y+16 w130", "Opacity:")
    opacitySlider := g.Add("Slider", "x+10 yp-3 w200 Range30-100 TickInterval10 Line5 Page10", gMinimapOpacity)
    opacityText := g.Add("Text", "x+10 yp+3 w110", gMinimapOpacity " %")
    opacitySlider.OnEvent("Change", (ctrl, *) => opacityText.Text := ctrl.Value " %")

    g.Add("Text", "xs y+16 w130", "Position:")
    anchorDdl := g.Add("DropDownList", "x+10 yp-3 w200", _Settings_AnchorLabels())
    anchorDdl.Value := _Settings_AnchorIndex(gMinimapAnchor)

    g.Add("Text", "xs y+14 w130", "Nudge X / Y (px):")
    offsetXEdit := g.Add("Edit", "x+10 yp-3 w70", String(gMinimapOffsetX))
    offsetYEdit := g.Add("Edit", "x+8 yp w70", String(gMinimapOffsetY))
    resetPosBtn := g.Add("Button", "x+8 yp-1 w54", "Reset")

    g.Add("Text", "xs y+14 w430 c666666",
        "Hold Ctrl and drag the overlay to set the nudge visually.`n"
        . "Double-click the overlay to re-centre it on the game window.")

    keepOpenCb := g.Add("CheckBox", "xs y+16 w400",
        "Keep the minimap open when the game loses focus")
    keepOpenCb.Value := gMinimapKeepOpen ? 1 : 0

    resetPosBtn.OnEvent("Click", (*) => (offsetXEdit.Value := "0", offsetYEdit.Value := "0",
        anchorDdl.Value := _Settings_AnchorIndex("Center")))

    return {
        scale: scaleSlider,
        opacity: opacitySlider,
        anchor: anchorDdl,
        offsetX: offsetXEdit,
        offsetY: offsetYEdit,
        keepOpen: keepOpenCb
    }
}

_Settings_UpdateScaleText(ctrl, scale) {
    global OVERLAY_W, OVERLAY_H
    ctrl.Text := scale " %  (" Round(OVERLAY_W * scale / 100) "×" Round(OVERLAY_H * scale / 100) ")"
}

_Settings_AnchorLabels() {
    return ["Centered on the game window", "Top-left", "Top-right", "Bottom-left", "Bottom-right"]
}

; MINIMAP_ANCHORS and the labels above are parallel lists.
_Settings_AnchorIndex(anchor) {
    global MINIMAP_ANCHORS
    for i, name in MINIMAP_ANCHORS {
        if (name = anchor)
            return i
    }
    return 1
}

; Validates and applies the Minimap tab. Returns false to keep the window open.
_Settings_SaveMinimap(c) {
    global gMinimapScale, gMinimapOpacity, gMinimapAnchor, gMinimapOffsetX, gMinimapOffsetY
    global gMinimapKeepOpen, MINIMAP_ANCHORS

    offX := Trim(c.offsetX.Value)
    offY := Trim(c.offsetY.Value)
    ; Negative values are legitimate here, so Number-only edits can't be used.
    if (!IsInteger(offX) || !IsInteger(offY)) {
        TrayTip("AHK Minimap", "Minimap nudge X/Y must be whole numbers (negative is allowed).", "Iconx")
        return false
    }

    previousScale := gMinimapScale
    gMinimapScale := c.scale.Value
    gMinimapOpacity := c.opacity.Value
    gMinimapAnchor := MINIMAP_ANCHORS[c.anchor.Value]
    gMinimapOffsetX := Integer(offX)
    gMinimapOffsetY := Integer(offY)
    gMinimapKeepOpen := c.keepOpen.Value ? true : false
    SaveMinimapConfig()

    ; A resize needs the Gui rebuilt; opacity/placement apply on the next tick.
    if (gMinimapScale != previousScale) {
        RebuildOverlayGui()
    } else {
        ApplyOverlayOpacity()
    }
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
