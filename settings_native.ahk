#Requires AutoHotkey v2.0

; Native counterpart to the WebView2 settings page. It deliberately consumes
; the same addon field descriptors and the same save message shape, keeping
; settings behavior in one place while allowing a Chromium-free interface.

_Settings_BuildNative() {
    global gSettingsGui, gAddonHooks, gDisabledAddons, gSettingsHotkeyRows
    global gGamePath, gGameArgs, gLaunchOnStartup, gLaunchAutoLogin, gMultiClientCount, gMultiClientDelay
    global gPrimaryMonitorOverride, gSecondaryMonitorOverride
    global gPrimaryLaunchLayout, gSecondaryLaunchLayout
    global gPrimaryProfileId, gSecondaryProfileId, LAUNCH_LAYOUT_DEFAULT
    global gMinimapScale, gMinimapOpacity, gMinimapAnchor, gMinimapOffsetX, gMinimapOffsetY
    global gMinimapKeepOpen, MINIMAP_ANCHORS, gAccentScheme

    contributors := []
    for _, am in gAddonHooks {
        if !am.Has("OnSettingsWeb")
            continue
        label := am.Has("settingsLabel") ? am["settingsLabel"]
            : (am.Has("name") ? am["name"] : "Addon")
        contributors.Push({ map: am, label: label })
    }

    tabNames := ["Launcher", "Minimap", "Appearance", "Hotkeys"]
    for c in contributors
        tabNames.Push(c.label)
    tabNames.Push("Addons")

    g := Gui("+AlwaysOnTop -MinimizeBox", "osMW Maps++ — Settings (Native)")
    gSettingsGui := g
    g.SetFont("s9", "Segoe UI")
    g.OnEvent("Close", (*) => _Settings_Close())
    g.OnEvent("Escape", (*) => _Settings_Close())

    ; Taller than it was: the Launcher tab now carries the profile editor that
    ; replaced the two fixed display/layout pairs.
    tab := g.Add("Tab3", "x10 y10 w650 h548", tabNames)
    contentX := 28
    contentY := 58

    ; Launcher ----------------------------------------------------
    tab.UseTab(1)
    g.Add("Text", "x" contentX " y" contentY " w130", "Game path:")
    gamePathEdit := g.Add("Edit", "x158 yp-3 w390 ReadOnly", gGamePath)
    browseBtn := g.Add("Button", "x556 yp-1 w78", "Browse…")
    browseBtn.OnEvent("Click", BrowseGame)

    g.Add("Text", "x" contentX " y+20 w130", "Game arguments:")
    gameArgsEdit := g.Add("Edit", "x158 yp-3 w476", gGameArgs)

    autoStartCb := g.Add("CheckBox", "x" contentX " y+20 w520",
        "Start Maps++ automatically when Windows starts")
    autoStartCb.Value := IsRunOnStartupEnabled() ? 1 : 0
    launchOnStartupCb := g.Add("CheckBox", "x" contentX " y+10 w520",
        "Launch a game client when Maps++ starts")
    launchOnStartupCb.Value := gLaunchOnStartup ? 1 : 0
    launchAutoLoginCb := g.Add("CheckBox", "x" contentX " y+10 w520",
        "Auto-login launched clients (intro click-through + roster)")
    launchAutoLoginCb.Value := gLaunchAutoLogin ? 1 : 0

    ; Paired on one row: the profile editor below needs the vertical space, and
    ; these two are read together anyway.
    g.Add("Text", "x" contentX " y+22 w130", "Clients per launch:")
    countEdit := g.Add("Edit", "x158 yp-3 w70 Number", String(gMultiClientCount))
    g.Add("Text", "x244 yp+3 w120", "Launch delay (ms):")
    delayEdit := g.Add("Edit", "x370 yp-3 w70 Number", String(gMultiClientDelay))

    ; ── Launch profiles ──
    ; The Primary/Secondary display and layout pairs that used to sit here are
    ; now per-profile; the two dropdowns at the bottom say which profile each
    ; launch action runs. Same store and same validation as the WebView2 page.
    monitorChoices := _Settings_MonitorChoices()
    launchLayoutOptions := _Settings_LaunchLayoutOptions()
    launchLayoutLabels := []
    for item in launchLayoutOptions.items
        launchLayoutLabels.Push(item.label)
    accountChoices := _Settings_AccountChoices()
    accountLabels := []
    for choice in accountChoices
        accountLabels.Push(choice["label"])

    nativeProfiles := []
    for _, pid in LaunchProfileIds() {
        prof := LaunchProfileById(pid)
        nativeProfiles.Push(Map(
            "id", pid,
            "name", prof["name"],
            "accounts", prof["accounts"],
            "count", prof["count"],
            "monitor", prof["monitor"],
            "layout", prof["layout"]))
    }
    profileSel := nativeProfiles.Length ? 1 : 0
    profileSyncing := false

    ; Absolute Y from here down. The two columns have to line up with each other
    ; and the whole block has to end inside the tab, which a chain of y+ offsets
    ; through controls of three different heights does not reliably do.
    listY := 306
    fieldY := listY
    g.Add("Text", "x" contentX " y284 w200", "Launch profiles:")
    profileList := g.Add("ListBox", "x" contentX " y" listY " w240 r8")
    addProfileBtn := g.Add("Button", "x" contentX " y438 w76", "Add")
    dupProfileBtn := g.Add("Button", "x+6 yp w76", "Duplicate")
    delProfileBtn := g.Add("Button", "x+6 yp w76", "Remove")

    g.Add("Text", "x286 y" fieldY " w70", "Name:")
    profileNameEdit := g.Add("Edit", "x360 y" (fieldY - 3) " w274 Limit40")
    g.Add("Text", "x286 y" (fieldY + 29) " w70", "Accounts:")
    profileAccountsDdl := g.Add("DropDownList", "x360 y" (fieldY + 26) " w274", accountLabels)
    g.Add("Text", "x286 y" (fieldY + 58) " w70", "Clients:")
    profileCountEdit := g.Add("Edit", "x360 y" (fieldY + 55) " w70 Number")
    g.Add("Text", "x436 y" (fieldY + 58) " w198 c666666", "0 = Clients per launch")
    g.Add("Text", "x286 y" (fieldY + 87) " w70", "Display:")
    profileMonitorDdl := g.Add("DropDownList", "x360 y" (fieldY + 84) " w274", monitorChoices)
    profileLayoutDdl := 0
    if launchLayoutOptions.available {
        g.Add("Text", "x286 y" (fieldY + 116) " w70", "Layout:")
        profileLayoutDdl := g.Add("DropDownList", "x360 y" (fieldY + 113) " w274", launchLayoutLabels)
    }

    g.Add("Text", "x" contentX " y478 w130", "Primary runs:")
    primaryProfileDdl := g.Add("DropDownList", "x158 y475 w280")
    g.Add("Text", "x" contentX " y508 w130", "Secondary runs:")
    secondaryProfileDdl := g.Add("DropDownList", "x158 y505 w280")

    g.Add("Text", "x" contentX " y534 w606 c666666",
        "Any other profile is one click away under Client Launch in the tray menu."
        . " Interface mode is changed from Tray → Interface.")

    profileList.OnEvent("Change", SelectProfile)
    addProfileBtn.OnEvent("Click", AddProfile)
    dupProfileBtn.OnEvent("Click", DuplicateProfile)
    delProfileBtn.OnEvent("Click", RemoveProfile)
    for ctrl in [profileNameEdit, profileAccountsDdl, profileCountEdit, profileMonitorDdl]
        ctrl.OnEvent("Change", ApplyProfileFields)
    if IsObject(profileLayoutDdl)
        profileLayoutDdl.OnEvent("Change", ApplyProfileFields)

    RefreshProfileList()
    FillProfileFields()
    RefreshProfileBindings(gPrimaryProfileId, gSecondaryProfileId)

    ; Minimap -----------------------------------------------------
    tab.UseTab(2)
    g.Add("Text", "x" contentX " y" contentY " w150", "Scale (%):")
    scaleEdit := g.Add("Edit", "x178 yp-3 w80 Number", String(gMinimapScale))
    g.Add("UpDown", "Range50-200", gMinimapScale)
    g.Add("Text", "x" contentX " y+22 w150", "Opacity (%):")
    opacityEdit := g.Add("Edit", "x178 yp-3 w80 Number", String(gMinimapOpacity))
    g.Add("UpDown", "Range30-100", gMinimapOpacity)
    g.Add("Text", "x" contentX " y+22 w150", "Anchor:")
    anchorDdl := g.Add("DropDownList", "x178 yp-3 w220", MINIMAP_ANCHORS)
    anchorDdl.Choose(gMinimapAnchor)
    g.Add("Text", "x" contentX " y+22 w150", "Horizontal nudge:")
    offsetXEdit := g.Add("Edit", "x178 yp-3 w100", String(gMinimapOffsetX))
    g.Add("Text", "x" contentX " y+22 w150", "Vertical nudge:")
    offsetYEdit := g.Add("Edit", "x178 yp-3 w100", String(gMinimapOffsetY))
    keepOpenCb := g.Add("CheckBox", "x" contentX " y+24 w520",
        "Keep the minimap open when the game loses focus")
    keepOpenCb.Value := gMinimapKeepOpen ? 1 : 0

    ; Appearance --------------------------------------------------
    tab.UseTab(3)
    g.Add("Text", "x" contentX " y" contentY " w590",
        "Choose the highlight colour used across Maps++. Backgrounds and status colours stay unchanged.")
    g.Add("Text", "x" contentX " y+28 w150", "Accent colour:")
    accentSchemeValues := ["amber", "blue", "green"]
    accentSchemeDdl := g.Add("DropDownList", "x178 yp-3 w220", ["Amber (default)", "Blue", "Green"])
    accentSchemeDdl.Value := (gAccentScheme = "blue") ? 2 : ((gAccentScheme = "green") ? 3 : 1)

    ; Hotkeys -----------------------------------------------------
    tab.UseTab(4)
    g.Add("Text", "x" contentX " y" contentY " w590",
        "Select an action to change, unbind, or reset it. Escape cancels shortcut capture.")
    hotkeyLv := g.Add("ListView", "x" contentX " y82 w590 h340 -Multi Grid", ["Action", "Shortcut"])
    nativeHotkeyRows := []
    for action in GetHotkeyActionsForSettings() {
        hotkeyLv.Add(, action["label"], FormatHotkeySettingsDisplay(action["chord"]))
        nativeHotkeyRows.Push(Map(
            "id", action["id"],
            "pending", action["chord"],
            "default", action["default"],
            "action", action,
            "allowMouse", action.Has("allowMouse") && action["allowMouse"]
        ))
    }
    gSettingsHotkeyRows := nativeHotkeyRows
    hotkeyLv.ModifyCol(1, 420)
    hotkeyLv.ModifyCol(2, 145)
    captureBtn := g.Add("Button", "x" contentX " y434 w130", "Change selected")
    unbindBtn := g.Add("Button", "x+8 w130", "Unbind selected")
    resetBtn := g.Add("Button", "x+8 w130", "Reset selected")
    resetAllBtn := g.Add("Button", "x+8 w130", "Reset all")
    captureBtn.OnEvent("Click", BeginHotkeyCapture)
    unbindBtn.OnEvent("Click", UnbindSelectedHotkey)
    resetBtn.OnEvent("Click", ResetSelectedHotkey)
    resetAllBtn.OnEvent("Click", ResetAllHotkeys)

    ; Addon-contributed tabs -------------------------------------
    addonFieldGroups := Map()
    for i, c in contributors {
        tab.UseTab(4 + i)
        addonName := c.map.Has("name") ? c.map["name"] : c.label
        fields := []
        try fields := c.map["OnSettingsWeb"]()
        controls := []
        y := contentY
        for field in fields {
            fieldType := field.Has("type") ? field["type"] : "text"
            if (fieldType = "info") {
                infoText := field.Has("text") ? field["text"] : ""
                lineCount := StrSplit(infoText, "`n").Length
                infoH := Max(42, lineCount * 18)
                g.Add("Text", "x" contentX " y" y " w590 h" infoH " Wrap", infoText)
                y += infoH + 10
                continue
            }

            label := field.Has("label") ? field["label"] : ""
            value := field.Has("value") ? field["value"] : ""
            ctrl := 0
            rowIds := []
            if (fieldType = "checkbox") {
                ctrl := g.Add("CheckBox", "x" contentX " y" y " w590", label)
                ctrl.Value := value ? 1 : 0
                y += 32
            } else if (fieldType = "orderedlist") {
                g.Add("Text", "x" contentX " y" y " w590", label)
                y += 20
                ctrl := g.Add("ListView", "x" contentX " y" y " w500 h190 Checked -Multi NoSortHdr",
                    ["Action"])
                rowIds := _SettingsNative_FillOrderedList(ctrl, field)
                ; The row order is the value, so it needs to be editable. The
                ; buttons carry their own copy of rowIds — the save loop reads
                ; the same object back off the controls entry.
                upBtn := g.Add("Button", "x+8 y" y " w70", "Move up")
                downBtn := g.Add("Button", "x" (contentX + 508) " y" (y + 32) " w70", "Move down")
                upBtn.OnEvent("Click", _SettingsNative_MakeOrderedMover(ctrl, rowIds, -1))
                downBtn.OnEvent("Click", _SettingsNative_MakeOrderedMover(ctrl, rowIds, 1))
                y += 200
            } else {
                g.Add("Text", "x" contentX " y" y " w180", label)
                if (fieldType = "dropdown") {
                    options := field.Has("options") ? field["options"] : []
                    ctrl := g.Add("DropDownList", "x218 y" (y - 3) " w300", options)
                    ctrl.Value := Integer(value) + 1
                } else if (fieldType = "combo") {
                    options := field.Has("options") ? field["options"] : []
                    ctrl := g.Add("ComboBox", "x218 y" (y - 3) " w300", options)
                    try ctrl.Choose(String(value))
                    if (ctrl.Value = 0)
                        ctrl.Text := String(value)
                } else {
                    editOpts := (fieldType = "number") ? " Number" : ""
                    ctrl := g.Add("Edit", "x218 y" (y - 3) " w300" editOpts, String(value))
                }
                y += 36
            }
            controls.Push(Map("field", field, "control", ctrl, "rowIds", rowIds))
        }
        addonFieldGroups[addonName] := controls
    }

    ; Addon enable/disable tab -----------------------------------
    tab.UseTab(tabNames.Length)
    g.Add("Text", "x" contentX " y" contentY " w590",
        "Enable or disable addons. Reload Maps++ to fully apply changes.")
    addonChecks := []
    y := contentY + 34
    for _, am in gAddonHooks {
        name := am.Has("name") ? am["name"] : ""
        if (name = "")
            continue
        label := am.Has("settingsLabel") ? am["settingsLabel"] : name
        cb := g.Add("CheckBox", "x" contentX " y" y " w430", label)
        cb.Value := (gDisabledAddons.Has(name) && gDisabledAddons[name]) ? 0 : 1
        addonChecks.Push(Map("name", name, "control", cb))
        y += 28
    }

    ; Shared buttons ---------------------------------------------
    tab.UseTab()
    resetDefaultsBtn := g.Add("Button", "x10 y572 w160", "Reset to Defaults")
    saveBtn := g.Add("Button", "x454 y572 w96 Default", "Save")
    cancelBtn := g.Add("Button", "x558 y572 w96", "Cancel")
    resetDefaultsBtn.OnEvent("Click", (*) => _Settings_HandleResetDefaults())
    saveBtn.OnEvent("Click", SaveNativeSettings)
    cancelBtn.OnEvent("Click", (*) => _Settings_Close())
    g.Show("w664 h614")

    BrowseGame(*) {
        global PROCESS_EXE
        selected := FileSelect(1, A_ScriptDir, "Locate " PROCESS_EXE " (game executable)",
            "Executables (*.exe)")
        if (selected != "")
            gamePathEdit.Value := selected
    }

    ; ── Launch profile editor ──
    ; profileSyncing guards the Change handlers while the controls are being
    ; filled from the draft: without it, selecting a row would immediately write
    ; the previous row's field values back over the newly selected one.

    RefreshProfileList() {
        profileSyncing := true
        items := []
        for prof in nativeProfiles
            items.Push(prof["name"] != "" ? prof["name"] : "(unnamed)")
        profileList.Delete()
        if items.Length
            profileList.Add(items)
        if (profileSel >= 1 && profileSel <= nativeProfiles.Length)
            profileList.Choose(profileSel)
        profileSyncing := false
    }

    FillProfileFields() {
        profileSyncing := true
        has := (profileSel >= 1 && profileSel <= nativeProfiles.Length)
        prof := has ? nativeProfiles[profileSel] : 0
        profileNameEdit.Value := has ? prof["name"] : ""
        profileAccountsDdl.Value := has ? _SettingsNative_AccountChoice(accountChoices, prof["accounts"]) : 1
        profileCountEdit.Value := has ? String(prof["count"]) : "0"
        profileMonitorDdl.Value := has ? _Settings_MonitorIndexToChoice(prof["monitor"]) : 1
        if IsObject(profileLayoutDdl) {
            profileLayoutDdl.Value := has
                ? _SettingsNative_LaunchLayoutChoice(launchLayoutOptions, prof["layout"]) : 1
        }
        for ctrl in [profileNameEdit, profileAccountsDdl, profileCountEdit, profileMonitorDdl]
            ctrl.Enabled := has
        if IsObject(profileLayoutDdl)
            profileLayoutDdl.Enabled := has
        profileSyncing := false
    }

    SelectProfile(*) {
        if profileSyncing
            return
        chosen := profileList.Value
        if (chosen < 1 || chosen > nativeProfiles.Length)
            return
        profileSel := chosen
        FillProfileFields()
    }

    ApplyProfileFields(*) {
        if profileSyncing
            return
        if (profileSel < 1 || profileSel > nativeProfiles.Length)
            return
        prof := nativeProfiles[profileSel]
        prof["name"] := Trim(profileNameEdit.Value)
        idx := profileAccountsDdl.Value
        prof["accounts"] := (idx >= 1 && idx <= accountChoices.Length)
            ? accountChoices[idx]["value"] : ""
        raw := Trim(profileCountEdit.Value)
        prof["count"] := (IsInteger(raw) && Integer(raw) >= 0) ? Integer(raw) : 0
        prof["monitor"] := _Settings_ChoiceToMonitorIndex(profileMonitorDdl.Value)
        if (IsObject(profileLayoutDdl) && launchLayoutOptions.available) {
            li := profileLayoutDdl.Value
            if (li >= 1 && li <= launchLayoutOptions.items.Length)
                prof["layout"] := launchLayoutOptions.items[li].value
        }
        RefreshProfileList()
        RefreshProfileBindings(_SettingsNative_BoundId(primaryProfileDdl, nativeProfiles),
            _SettingsNative_BoundId(secondaryProfileDdl, nativeProfiles))
    }

    ; The two binding dropdowns are rebuilt from the draft whenever a name
    ; changes, so they can never offer a name the list no longer shows.
    RefreshProfileBindings(primaryId, secondaryId) {
        labels := []
        primaryIdx := 1
        secondaryIdx := 1
        for i, prof in nativeProfiles {
            labels.Push(prof["name"] != "" ? prof["name"] : "(unnamed)")
            if (prof["id"] = primaryId)
                primaryIdx := i
            if (prof["id"] = secondaryId)
                secondaryIdx := i
        }
        profileSyncing := true
        primaryProfileDdl.Delete()
        secondaryProfileDdl.Delete()
        if labels.Length {
            primaryProfileDdl.Add(labels)
            secondaryProfileDdl.Add(labels)
            primaryProfileDdl.Value := primaryIdx
            secondaryProfileDdl.Value := secondaryIdx
        }
        profileSyncing := false
    }

    AddProfile(*) {
        if (nativeProfiles.Length >= 32)
            return
        nativeProfiles.Push(Map(
            "id", _SettingsNative_NextProfileId(nativeProfiles),
            "name", "Profile " (nativeProfiles.Length + 1),
            "accounts", "",
            "count", 0,
            "monitor", 0,
            "layout", LAUNCH_LAYOUT_DEFAULT))
        profileSel := nativeProfiles.Length
        RefreshProfileList()
        FillProfileFields()
        RefreshProfileBindings(_SettingsNative_BoundId(primaryProfileDdl, nativeProfiles),
            _SettingsNative_BoundId(secondaryProfileDdl, nativeProfiles))
    }

    DuplicateProfile(*) {
        if (profileSel < 1 || profileSel > nativeProfiles.Length || nativeProfiles.Length >= 32)
            return
        src := nativeProfiles[profileSel]
        nativeProfiles.InsertAt(profileSel + 1, Map(
            "id", _SettingsNative_NextProfileId(nativeProfiles),
            "name", src["name"] " copy",
            "accounts", src["accounts"],
            "count", src["count"],
            "monitor", src["monitor"],
            "layout", src["layout"]))
        profileSel += 1
        RefreshProfileList()
        FillProfileFields()
        RefreshProfileBindings(_SettingsNative_BoundId(primaryProfileDdl, nativeProfiles),
            _SettingsNative_BoundId(secondaryProfileDdl, nativeProfiles))
    }

    RemoveProfile(*) {
        ; The last profile stays: with none at all the launch actions would fall
        ; back to a synthesised default the user cannot see or edit.
        if (profileSel < 1 || nativeProfiles.Length <= 1)
            return
        primaryId := _SettingsNative_BoundId(primaryProfileDdl, nativeProfiles)
        secondaryId := _SettingsNative_BoundId(secondaryProfileDdl, nativeProfiles)
        goneId := nativeProfiles[profileSel]["id"]
        nativeProfiles.RemoveAt(profileSel)
        if (profileSel > nativeProfiles.Length)
            profileSel := nativeProfiles.Length
        ; A binding pointing at the removed profile has to land somewhere real.
        if (primaryId = goneId)
            primaryId := nativeProfiles[1]["id"]
        if (secondaryId = goneId)
            secondaryId := nativeProfiles[nativeProfiles.Length]["id"]
        RefreshProfileList()
        FillProfileFields()
        RefreshProfileBindings(primaryId, secondaryId)
    }

    BeginHotkeyCapture(*) {
        rowNum := hotkeyLv.GetNext()
        if (rowNum < 1 || rowNum > nativeHotkeyRows.Length) {
            TrayTip("Select an action first.", "Hotkeys", "Icon!")
            return
        }
        row := nativeHotkeyRows[rowNum]
        row["button"] := captureBtn
        StartHotkeyCapture(row,
            (chord) => FinishHotkeyCapture(rowNum, chord),
            (*) => (captureBtn.Text := "Change selected"))
    }

    FinishHotkeyCapture(rowNum, chord) {
        nativeHotkeyRows[rowNum]["pending"] := chord
        hotkeyLv.Modify(rowNum, "Col2", FormatHotkeySettingsDisplay(chord))
        captureBtn.Text := "Change selected"
    }

    UnbindSelectedHotkey(*) {
        CancelHotkeyCapture()
        rowNum := hotkeyLv.GetNext()
        if (rowNum < 1 || rowNum > nativeHotkeyRows.Length)
            return
        nativeHotkeyRows[rowNum]["pending"] := ""
        hotkeyLv.Modify(rowNum, "Col2", FormatHotkeySettingsDisplay(""))
    }

    ResetSelectedHotkey(*) {
        CancelHotkeyCapture()
        rowNum := hotkeyLv.GetNext()
        if (rowNum < 1 || rowNum > nativeHotkeyRows.Length)
            return
        nativeHotkeyRows[rowNum]["pending"] := nativeHotkeyRows[rowNum]["default"]
        hotkeyLv.Modify(rowNum, "Col2", FormatHotkeySettingsDisplay(nativeHotkeyRows[rowNum]["default"]))
    }

    ResetAllHotkeys(*) {
        CancelHotkeyCapture()
        for rowNum, row in nativeHotkeyRows {
            row["pending"] := row["default"]
            hotkeyLv.Modify(rowNum, "Col2", FormatHotkeySettingsDisplay(row["default"]))
        }
    }

    SaveNativeSettings(*) {
        CancelHotkeyCapture()
        ; Commit whatever is sitting in the fields before reading the draft —
        ; a Change event has not necessarily fired for the last keystroke.
        ApplyProfileFields()
        profilePayload := []
        for prof in nativeProfiles {
            profilePayload.Push(Map(
                "id", prof["id"],
                "name", prof["name"],
                "accounts", prof["accounts"],
                "count", prof["count"],
                ; 0-based, matching what the page sends.
                "monitorChoice", _Settings_MonitorIndexToChoice(prof["monitor"]) - 1,
                "layout", prof["layout"]))
        }

        hotkeys := []
        for row in nativeHotkeyRows
            hotkeys.Push(Map("id", row["id"], "chord", row["pending"]))

        addonToggles := Map()
        for entry in addonChecks
            addonToggles[entry["name"]] := entry["control"].Value ? true : false

        addonSettings := Map()
        for addonName, entries in addonFieldGroups {
            values := Map()
            for entry in entries {
                field := entry["field"]
                if !field.Has("id") || field["id"] = ""
                    continue
                fieldType := field.Has("type") ? field["type"] : "text"
                ctrl := entry["control"]
                if (fieldType = "checkbox") {
                    value := ctrl.Value ? true : false
                } else if (fieldType = "orderedlist") {
                    ; Same comma-separated shape the web frontend sends, so the
                    ; addon has one save path to write.
                    value := _SettingsNative_ReadOrderedList(ctrl, entry["rowIds"])
                } else if (fieldType = "dropdown") {
                    value := ctrl.Value - 1
                } else if (fieldType = "combo") {
                    value := ctrl.Text
                } else {
                    raw := Trim(ctrl.Value)
                    value := (fieldType = "number" && IsInteger(raw)) ? Integer(raw) : raw
                }
                values[field["id"]] := value
            }
            addonSettings[addonName] := values
        }

        msg := Map(
            "launcher", Map(
                "gamePath", gamePathEdit.Value,
                "gameArgs", gameArgsEdit.Value,
                "autoStart", autoStartCb.Value ? true : false,
                "launchOnStartup", launchOnStartupCb.Value ? true : false,
                "launchAutoLogin", launchAutoLoginCb.Value ? true : false,
                "multiClientCount", Trim(countEdit.Value),
                "multiClientDelay", Trim(delayEdit.Value),
                ; Same shape the WebView2 page sends, so both frontends land in
                ; _Settings_ApplyProfiles and cannot validate differently.
                "profiles", profilePayload,
                "primaryProfile", _SettingsNative_BoundId(primaryProfileDdl, nativeProfiles),
                "secondaryProfile", _SettingsNative_BoundId(secondaryProfileDdl, nativeProfiles)
            ),
            "minimap", Map(
                "scale", Trim(scaleEdit.Value),
                "opacity", Trim(opacityEdit.Value),
                "anchor", anchorDdl.Text,
                "offsetX", Trim(offsetXEdit.Value),
                "offsetY", Trim(offsetYEdit.Value),
                "keepOpen", keepOpenCb.Value ? true : false
            ),
            "appearance", Map(
                "accentScheme", accentSchemeValues[accentSchemeDdl.Value]
            ),
            "hotkeys", hotkeys,
            "addons", addonToggles,
            "addonSettings", addonSettings
        )

        if _Settings_HandleSave(msg)
            TrayTip("Settings saved.", "Maps++", "Iconi")
    }
}

; 1-based index of the accounts choice matching a stored spec. A spec whose
; group or roster row has since been deleted falls back to "next free" rather
; than leaving the dropdown showing something unrelated.
_SettingsNative_AccountChoice(choices, value) {
    for i, choice in choices {
        if (choice["value"] = value)
            return i
    }
    return 1
}

; The profile id a binding dropdown currently points at. The dropdown holds
; positions, not ids, so it is always read back through the draft array.
_SettingsNative_BoundId(ddl, profiles) {
    idx := ddl.Value
    return (idx >= 1 && idx <= profiles.Length) ? profiles[idx]["id"] : ""
}

; Lowest unused positive id, so a removed profile's config.ini section cannot be
; inherited by the next one added.
_SettingsNative_NextProfileId(profiles) {
    n := 1
    loop {
        taken := false
        for prof in profiles {
            if (prof["id"] = String(n)) {
                taken := true
                break
            }
        }
        if !taken
            return String(n)
        n++
    }
}

_SettingsNative_LaunchLayoutChoice(options, value) {
    global LAUNCH_LAYOUT_DEFAULT
    for i, item in options.items {
        if (item.value = value)
            return i
    }
    for i, item in options.items {
        if (item.value = LAUNCH_LAYOUT_DEFAULT)
            return i
    }
    return 1
}

; ── orderedlist field ────────────────────────────────────────
; The web frontend's pick-and-order list, as a checked ListView plus two move
; buttons. Both frontends produce the same comma-separated id string.
;
; rowIds is the row order as ids, kept alongside the control and mutated in
; place by the movers — the ListView itself only stores display text, so the
; ids have to be reordered in step with it.

_SettingsNative_FillOrderedList(lv, field) {
    items := field.Has("items") ? field["items"] : []
    value := field.Has("value") ? String(field["value"]) : ""

    labels := Map()
    for it in items {
        if it.Has("id")
            labels[it["id"]] := it.Has("label") ? it["label"] : it["id"]
    }

    ; Chosen first, in their stored order; everything else keeps catalog order.
    rowIds := []
    seen := Map()
    for part in StrSplit(value, ",") {
        id := Trim(part)
        if (id != "" && labels.Has(id) && !seen.Has(id)) {
            seen[id] := true
            rowIds.Push(id)
            lv.Add("Check", labels[id])
        }
    }
    for it in items {
        id := it.Has("id") ? it["id"] : ""
        if (id = "" || seen.Has(id))
            continue
        seen[id] := true
        rowIds.Push(id)
        lv.Add(, labels[id])
    }
    lv.ModifyCol(1, 470)
    return rowIds
}

_SettingsNative_MakeOrderedMover(lv, rowIds, delta) {
    return (*) => _SettingsNative_MoveOrderedRow(lv, rowIds, delta)
}

_SettingsNative_MoveOrderedRow(lv, rowIds, delta) {
    row := lv.GetNext(0, "F")            ; focused row, checked or not
    if (row = 0)
        return
    target := row + delta
    if (target < 1 || target > rowIds.Length)
        return

    ; Swap the two rows' text, checked state and id together, then follow the
    ; selection so a repeated click keeps moving the same entry.
    rowText := lv.GetText(row, 1)
    targetText := lv.GetText(target, 1)
    rowChecked := _SettingsNative_IsRowChecked(lv, row)
    targetChecked := _SettingsNative_IsRowChecked(lv, target)

    lv.Modify(row, targetChecked ? "Check" : "-Check", targetText)
    lv.Modify(target, (rowChecked ? "Check" : "-Check") " Select Focus", rowText)

    tmp := rowIds[row]
    rowIds[row] := rowIds[target]
    rowIds[target] := tmp
}

_SettingsNative_IsRowChecked(lv, row) {
    checked := 0
    loop {
        checked := lv.GetNext(checked, "C")
        if (checked = 0)
            return false
        if (checked = row)
            return true
    }
}

_SettingsNative_ReadOrderedList(lv, rowIds) {
    checked := Map()
    row := 0
    loop {
        row := lv.GetNext(row, "C")
        if (row = 0)
            break
        checked[row] := true
    }
    out := ""
    for i, id in rowIds {
        if checked.Has(i)
            out .= (out = "" ? "" : ",") id
    }
    return out
}
