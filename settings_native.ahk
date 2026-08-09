#Requires AutoHotkey v2.0

; Native counterpart to the WebView2 settings page. It deliberately consumes
; the same addon field descriptors and the same save message shape, keeping
; settings behavior in one place while allowing a Chromium-free interface.

_Settings_BuildNative() {
    global gSettingsGui, gAddonHooks, gDisabledAddons, gSettingsHotkeyRows
    global gGamePath, gGameArgs, gLaunchOnStartup, gMultiClientCount, gMultiClientDelay
    global gPrimaryMonitorOverride, gSecondaryMonitorOverride
    global gPrimaryLaunchLayout, gSecondaryLaunchLayout
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

    tab := g.Add("Tab3", "x10 y10 w650 h500", tabNames)
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

    g.Add("Text", "x" contentX " y+22 w130", "Clients per launch:")
    countEdit := g.Add("Edit", "x158 yp-3 w90 Number", String(gMultiClientCount))
    g.Add("Text", "x" contentX " y+20 w130", "Launch delay (ms):")
    delayEdit := g.Add("Edit", "x158 yp-3 w90 Number", String(gMultiClientDelay))

    monitorChoices := _Settings_MonitorChoices()
    g.Add("Text", "x" contentX " y+20 w130", "Primary monitor:")
    primaryDdl := g.Add("DropDownList", "x158 yp-3 w300", monitorChoices)
    primaryDdl.Value := _Settings_MonitorIndexToChoice(gPrimaryMonitorOverride)
    launchLayoutOptions := _Settings_LaunchLayoutOptions()
    primaryLaunchLayoutDdl := 0
    secondaryLaunchLayoutDdl := 0
    if launchLayoutOptions.available {
        launchLayoutLabels := []
        for item in launchLayoutOptions.items
            launchLayoutLabels.Push(item.label)
        g.Add("Text", "x" contentX " y+20 w130", "Primary layout:")
        primaryLaunchLayoutDdl := g.Add("DropDownList", "x158 yp-3 w300", launchLayoutLabels)
        primaryLaunchLayoutDdl.Value := _SettingsNative_LaunchLayoutChoice(
            launchLayoutOptions, gPrimaryLaunchLayout)
    }
    g.Add("Text", "x" contentX " y+20 w130", "Secondary monitor:")
    secondaryDdl := g.Add("DropDownList", "x158 yp-3 w300", monitorChoices)
    secondaryDdl.Value := _Settings_MonitorIndexToChoice(gSecondaryMonitorOverride)
    if launchLayoutOptions.available {
        g.Add("Text", "x" contentX " y+20 w130", "Secondary layout:")
        secondaryLaunchLayoutDdl := g.Add("DropDownList", "x158 yp-3 w300", launchLayoutLabels)
        secondaryLaunchLayoutDdl.Value := _SettingsNative_LaunchLayoutChoice(
            launchLayoutOptions, gSecondaryLaunchLayout)
    }

    g.Add("Text", "x" contentX " y+28 w590 c666666",
        "Interface mode is changed from Tray → Interface and takes effect after an automatic reload.")

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
        cb := g.Add("CheckBox", "x" contentX " y" y " w430", name)
        cb.Value := (gDisabledAddons.Has(name) && gDisabledAddons[name]) ? 0 : 1
        addonChecks.Push(Map("name", name, "control", cb))
        y += 28
    }

    ; Shared buttons ---------------------------------------------
    tab.UseTab()
    resetDefaultsBtn := g.Add("Button", "x10 y524 w160", "Reset to Defaults")
    saveBtn := g.Add("Button", "x454 y524 w96 Default", "Save")
    cancelBtn := g.Add("Button", "x558 y524 w96", "Cancel")
    resetDefaultsBtn.OnEvent("Click", (*) => _Settings_HandleResetDefaults())
    saveBtn.OnEvent("Click", SaveNativeSettings)
    cancelBtn.OnEvent("Click", (*) => _Settings_Close())
    g.Show("w664 h566")

    BrowseGame(*) {
        global PROCESS_EXE
        selected := FileSelect(1, A_ScriptDir, "Locate " PROCESS_EXE " (game executable)",
            "Executables (*.exe)")
        if (selected != "")
            gamePathEdit.Value := selected
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
                "multiClientCount", Trim(countEdit.Value),
                "multiClientDelay", Trim(delayEdit.Value),
                "primaryMonitor", primaryDdl.Value - 1,
                "secondaryMonitor", secondaryDdl.Value - 1,
                "primaryLaunchLayout", launchLayoutOptions.available
                    ? launchLayoutOptions.items[primaryLaunchLayoutDdl.Value].value : gPrimaryLaunchLayout,
                "secondaryLaunchLayout", launchLayoutOptions.available
                    ? launchLayoutOptions.items[secondaryLaunchLayoutDdl.Value].value : gSecondaryLaunchLayout
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
