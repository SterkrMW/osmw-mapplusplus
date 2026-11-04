#Requires AutoHotkey v2.0

; Native counterpart to the WebView2 settings page. It deliberately consumes
; the same addon field descriptors and the same save message shape, keeping
; settings behavior in one place while allowing a Chromium-free interface.

_Settings_BuildNative() {
    global gSettingsGui, gAddonHooks, gDisabledAddons, gSettingsHotkeyRows
    global gGamePath, gGameArgs, gLaunchOnStartup, gLaunchAutoLogin, gMultiClientCount, gMultiClientDelay
    global gPrimaryProfileId, gSecondaryProfileId, LAUNCH_LAYOUT_DEFAULT
    global gMinimapScale, gMinimapOpacity, gMinimapAnchor, gMinimapOffsetX, gMinimapOffsetY
    global gMinimapKeepOpen, MINIMAP_ANCHORS, gAccentScheme, gShowHoverCoords
    global gMinimapSource, MINIMAP_SOURCES, MINIMAP_SOURCE_LABELS, gMinimapFollowGameScale
    global gSettingsNativeFieldSetter, gSettingsRenderedAddonValues
    global gInterfaceMode, gDeveloperTools

    gSettingsRenderedAddonValues := Map()
    ; The same grouped tab model as the WebView2 page, so the two cannot list
    ; different tabs or put them in a different order.
    tabLayout := _Settings_GetTabLayout()
    contributors := Map()
    for _, am in gAddonHooks {
        ; Same rule as the WebView2 page, so a disabled addon has no page here
        ; either and its Save cannot reach the files it edits.
        if _Settings_AddonHasTab(am)
            contributors[_Settings_AddonTabLabel(am)] := am
    }

    g := Gui("+AlwaysOnTop -MinimizeBox", "osMW Maps++ — Settings (Native)")
    gSettingsGui := g
    g.SetFont("s9", "Segoe UI")
    g.OnEvent("Close", (*) => _Settings_Close())
    g.OnEvent("Escape", (*) => _Settings_Close())

    ; ── Pages ──
    ; A tree of the tabs down the left rather than a Tab3 strip: sixteen tabs
    ; wrapped a strip onto three rows, and its pages' content sat at a fixed y
    ; under it. Each page's controls are recorded as they are added and shown
    ; or hidden together. The page bodies below keep the coordinates they had
    ; inside the old tab; PAdd moves every absolute x right of the tree.
    NAV_W := 176
    pages := Map()
    pageOrder := []
    curPage := ""
    contentX := 28
    contentY := 58

    ; Launching ---------------------------------------------------
    UsePage("Game & startup")
    PAdd("Text", "x" contentX " y" contentY " w130", "Game (main.exe):")
    gamePathEdit := PAdd("Edit", "x158 yp-3 w390 ReadOnly", gGamePath)
    browseBtn := PAdd("Button", "x556 yp-1 w78", "Browse…")
    browseBtn.OnEvent("Click", BrowseGame)
    gameStatus := _Settings_GameStatus(gGamePath)
    gameStatusText := PAdd("Text", "x158 y+6 w476 h32 c666666", gameStatus.text)

    PAdd("Text", "x" contentX " y+14 w130", "Extra arguments:")
    gameArgsEdit := PAdd("Edit", "x158 yp-3 w476", gGameArgs)

    autoStartCb := PAdd("CheckBox", "x" contentX " y+24 w520",
        "Start Maps++ when Windows starts")
    autoStartCb.Value := IsRunOnStartupEnabled() ? 1 : 0
    launchOnStartupCb := PAdd("CheckBox", "x" contentX " y+10 w520",
        "Launch the game when Maps++ starts")
    launchOnStartupCb.Value := gLaunchOnStartup ? 1 : 0

    UsePage("Launching")
    ; ── Launch profiles ──
    ; Same store and same validation as the WebView2 page. A profile's count is
    ; always explicit here: the global "Clients per launch" it could defer to
    ; is no longer shown, so a stored 0 is shown and saved as what it meant.
    monitorChoices := _Settings_MonitorChoices()
    launchLayoutOptions := _Settings_LaunchLayoutOptions()
    launchLayoutLabels := []
    for item in launchLayoutOptions.items
        launchLayoutLabels.Push(item.label)
    accountChoices := _Settings_AccountChoices()
    accountLabels := []
    for choice in accountChoices
        accountLabels.Push(choice["label"])
    defaultCount := Max(1, gMultiClientCount)

    nativeProfiles := []
    for _, pid in LaunchProfileIds() {
        prof := LaunchProfileById(pid)
        nativeProfiles.Push(Map(
            "id", pid,
            "name", prof["name"],
            "accounts", prof["accounts"],
            "count", (prof["count"] >= 1) ? prof["count"] : defaultCount,
            "monitor", prof["monitor"],
            "layout", prof["layout"]))
    }
    profileSel := nativeProfiles.Length ? 1 : 0
    profileSyncing := false

    primaryChord := GetHotkeyDisplay("launchPrimary")
    secondaryChord := GetHotkeyDisplay("launchSecondary")
    PAdd("Text", "x" contentX " y" contentY " w180",
        "Launch primary" (primaryChord != "" ? " (" primaryChord ")" : "") " runs:")
    primaryProfileDdl := PAdd("DropDownList", "x218 yp-3 w300")
    PAdd("Text", "x" contentX " y+12 w180",
        "Launch secondary" (secondaryChord != "" ? " (" secondaryChord ")" : "") " runs:")
    secondaryProfileDdl := PAdd("DropDownList", "x218 yp-3 w300")
    PAdd("Text", "x" contentX " y+6 w606 c666666",
        "None takes Launch secondary out of the tray. Other profiles are under Launch other in the tray.")

    ; Absolute Y from here down. The two columns have to line up with each other
    ; and the whole block has to end inside the page, which a chain of y+
    ; offsets through controls of three different heights does not reliably do.
    listY := 166
    fieldY := listY
    PAdd("Text", "x" contentX " y" (listY - 22) " w200", "Profiles:")
    profileList := PAdd("ListBox", "x" contentX " y" listY " w240 r8")
    addProfileBtn := PAdd("Button", "x" contentX " y" (listY + 132) " w76", "Add")
    dupProfileBtn := PAdd("Button", "x+6 yp w76", "Duplicate")
    delProfileBtn := PAdd("Button", "x+6 yp w76", "Remove")

    PAdd("Text", "x286 y" fieldY " w70", "Name:")
    profileNameEdit := PAdd("Edit", "x360 y" (fieldY - 3) " w274 Limit40")
    PAdd("Text", "x286 y" (fieldY + 29) " w70", "Accounts:")
    profileAccountsDdl := PAdd("DropDownList", "x360 y" (fieldY + 26) " w274", accountLabels)
    PAdd("Text", "x286 y" (fieldY + 58) " w70", "Clients:")
    profileCountEdit := PAdd("Edit", "x360 y" (fieldY + 55) " w70 Number")
    profileCountHint := PAdd("Text", "x436 y" (fieldY + 58) " w198 c666666", "")
    PAdd("Text", "x286 y" (fieldY + 87) " w70", "Display:")
    profileMonitorDdl := PAdd("DropDownList", "x360 y" (fieldY + 84) " w274", monitorChoices)
    profileLayoutDdl := 0
    if launchLayoutOptions.available {
        PAdd("Text", "x286 y" (fieldY + 116) " w70", "Layout:")
        profileLayoutDdl := PAdd("DropDownList", "x360 y" (fieldY + 113) " w274", launchLayoutLabels)
    }

    launchAutoLoginCb := PAdd("CheckBox", "x" contentX " y" (listY + 178) " w440",
        "Log launched clients in automatically (needs the osMW client mods)")
    launchAutoLoginCb.Value := gLaunchAutoLogin ? 1 : 0
    if _Settings_AccountsEditorAvailable()
        PAdd("Button", "x486 yp-4 w148", "Edit accounts…").OnEvent("Click", (*) => _Settings_OpenAccounts())
    PAdd("Text", "x" contentX " y+16 w180", "Delay between clients (ms):")
    delayEdit := PAdd("Edit", "x218 yp-3 w70 Number", String(gMultiClientDelay))

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
    UsePage("Minimap")
    PAdd("Text", "x" contentX " y" contentY " w150", "Tab key:")
    sourceDdl := PAdd("DropDownList", "x178 yp-3 w440", MINIMAP_SOURCE_LABELS)
    sourceDdl.Value := 1
    for i, src in MINIMAP_SOURCES {
        if (src = gMinimapSource)
            sourceDdl.Value := i
    }
    openChord := GetHotkeyDisplay("openMapsppMinimap")
    PAdd("Text", "x178 y+4 w440 cGray",
        "“Open Maps++ minimap” opens this overlay even where the game has its own"
        . (openChord != "" ? " (" openChord ")." : " — bind it under Hotkeys."))
    PAdd("Text", "x" contentX " y+18 w150", "Scale (%):")
    scaleEdit := PAdd("Edit", "x178 yp-3 w80 Number", String(gMinimapScale))
    PAdd("UpDown", "Range50-200", gMinimapScale)
    PAdd("Text", "x" contentX " y+22 w150", "Opacity (%):")
    opacityEdit := PAdd("Edit", "x178 yp-3 w80 Number", String(gMinimapOpacity))
    PAdd("UpDown", "Range30-100", gMinimapOpacity)
    followScaleCb := PAdd("CheckBox", "x" contentX " y+12 w520",
        "Grow with the game's display scale (the client mods' 2×/3×)")
    followScaleCb.Value := gMinimapFollowGameScale ? 1 : 0
    PAdd("Text", "x" contentX " y+22 w150", "Anchor:")
    anchorDdl := PAdd("DropDownList", "x178 yp-3 w220", MINIMAP_ANCHORS)
    anchorDdl.Choose(gMinimapAnchor)
    PAdd("Text", "x" contentX " y+22 w150", "Horizontal nudge:")
    offsetXEdit := PAdd("Edit", "x178 yp-3 w100", String(gMinimapOffsetX))
    PAdd("Text", "x" contentX " y+22 w150", "Vertical nudge:")
    offsetYEdit := PAdd("Edit", "x178 yp-3 w100", String(gMinimapOffsetY))
    keepOpenCb := PAdd("CheckBox", "x" contentX " y+24 w520",
        "Keep the minimap open when the game loses focus")
    keepOpenCb.Value := gMinimapKeepOpen ? 1 : 0
    hoverCoordsCb := PAdd("CheckBox", "x" contentX " y+10 w520",
        "Show coordinates under the cursor")
    hoverCoordsCb.Value := gShowHoverCoords ? 1 : 0

    ; Appearance --------------------------------------------------
    UsePage("Appearance")
    PAdd("Text", "x" contentX " y" contentY " w150", "Interface:")
    interfaceValues := ["webview", "native"]
    runtimeOk := WebView2RuntimeAvailable()
    interfaceDdl := PAdd("DropDownList", "x178 yp-3 w300", [
        "Enhanced" (runtimeOk ? "" : " (needs the WebView2 Runtime)"),
        "Low memory (this window)"])
    interfaceDdl.Value := (gInterfaceMode = "native") ? 2 : 1
    PAdd("Text", "x178 y+4 w440 c666666", "Maps++ restarts when you save a change here.")
    PAdd("Text", "x" contentX " y+22 w150", "Accent colour:")
    accentSchemeValues := ["amber", "blue", "green"]
    accentSchemeDdl := PAdd("DropDownList", "x178 yp-3 w220", ["Amber (default)", "Blue", "Green"])
    accentSchemeDdl.Value := (gAccentScheme = "blue") ? 2 : ((gAccentScheme = "green") ? 3 : 1)
    PAdd("Text", "x178 y+4 w440 c666666", "The highlight colour in Maps++'s windows and on the minimap.")
    developerToolsCb := PAdd("CheckBox", "x" contentX " y+22 w600",
        "Show developer tools in the tray menu (signature tools, debug state, reload)")
    developerToolsCb.Value := gDeveloperTools ? 1 : 0
    PAdd("Text", "x" contentX " y+4 w600 c666666",
        "Shift+right-click on the tray icon shows them once without this.")

    ; Hotkeys -----------------------------------------------------
    UsePage("Hotkeys")
    PAdd("Text", "x" contentX " y" contentY " w590",
        "Select an action to change, unbind, or reset it. Escape cancels shortcut capture.")
    hotkeyLv := PAdd("ListView", "x" contentX " y82 w590 h400 -Multi Grid", ["Action", "Shortcut"])
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
    captureBtn := PAdd("Button", "x" contentX " y494 w130", "Change selected")
    unbindBtn := PAdd("Button", "x+8 w130", "Unbind selected")
    resetBtn := PAdd("Button", "x+8 w130", "Reset selected")
    resetAllBtn := PAdd("Button", "x+8 w130", "Reset all")
    captureBtn.OnEvent("Click", BeginHotkeyCapture)
    unbindBtn.OnEvent("Click", UnbindSelectedHotkey)
    resetBtn.OnEvent("Click", ResetSelectedHotkey)
    resetAllBtn.OnEvent("Click", ResetAllHotkeys)

    ; Addon-contributed pages -------------------------------------
    addonFieldGroups := Map()
    for pageName, am in contributors {
        UsePage(pageName)
        addonName := _Settings_AddonKey(am)
        ; Records what each field shows, so Save hands the addon only changes.
        fields := _Settings_AddonFieldsFor(am)
        controls := []
        y := contentY
        for field in fields {
            fieldType := field.Has("type") ? field["type"] : "text"
            if (fieldType = "info") {
                infoText := field.Has("text") ? field["text"] : ""
                lineCount := StrSplit(infoText, "`n").Length
                infoH := Max(42, lineCount * 18)
                PAdd("Text", "x" contentX " y" y " w590 h" infoH " Wrap", infoText)
                y += infoH + 10
                continue
            }

            label := field.Has("label") ? field["label"] : ""
            value := field.Has("value") ? field["value"] : ""
            ctrl := 0
            rowIds := []
            if (fieldType = "button") {
                ; Carries no value and never joins `controls`, so the save loop
                ; below cannot see it. The addon acts the moment this is
                ; clicked; it does not wait for Save.
                actionId := field.Has("id") ? field["id"] : ""
                btn := PAdd("Button", "x" contentX " y" y " w200", label)
                btn.OnEvent("Click",
                    _SettingsNative_MakeActionHandler(addonName, actionId))
                y += 30
                if field.Has("hint") {
                    PAdd("Text", "x" contentX " y" y " w590", field["hint"])
                    y += 20
                }
                y += 6
                continue
            }
            if (fieldType = "checkbox") {
                ; Narrower than the row so a revert button fits on the same
                ; line; without that every default-bearing checkbox would cost
                ; a second row on a page that does not scroll.
                ctrl := PAdd("CheckBox", "x" contentX " y" y " w480", label)
                ctrl.Value := value ? 1 : 0
                AddRevert(field, ctrl, fieldType, y)
                y += 32
            } else if (fieldType = "orderedlist") {
                PAdd("Text", "x" contentX " y" y " w590", label)
                y += 20
                ctrl := PAdd("ListView", "x" contentX " y" y " w500 h190 Checked -Multi NoSortHdr",
                    ["Action"])
                rowIds := _SettingsNative_FillOrderedList(ctrl, field)
                ; The row order is the value, so it needs to be editable. The
                ; buttons carry their own copy of rowIds — the save loop reads
                ; the same object back off the controls entry.
                upBtn := PAdd("Button", "x+8 y" y " w70", "Move up")
                downBtn := PAdd("Button", "x" (contentX + 508) " y" (y + 32) " w70", "Move down")
                upBtn.OnEvent("Click", _SettingsNative_MakeOrderedMover(ctrl, rowIds, -1))
                downBtn.OnEvent("Click", _SettingsNative_MakeOrderedMover(ctrl, rowIds, 1))
                y += 200
            } else {
                PAdd("Text", "x" contentX " y" y " w180", label)
                if (fieldType = "dropdown") {
                    options := field.Has("options") ? field["options"] : []
                    ctrl := PAdd("DropDownList", "x218 y" (y - 3) " w300", options)
                    ctrl.Value := Integer(value) + 1
                } else if (fieldType = "combo") {
                    options := field.Has("options") ? field["options"] : []
                    ctrl := PAdd("ComboBox", "x218 y" (y - 3) " w300", options)
                    try ctrl.Choose(String(value))
                    if (ctrl.Value = 0)
                        ctrl.Text := String(value)
                } else {
                    editOpts := (fieldType = "number") ? " Number" : ""
                    ctrl := PAdd("Edit", "x218 y" (y - 3) " w300" editOpts, String(value))
                }
                AddRevert(field, ctrl, fieldType, y)
                y += 36
            }
            controls.Push(Map("field", field, "control", ctrl, "rowIds", rowIds))
        }
        addonFieldGroups[addonName] := controls
    }
    ; How _Settings_PushAddonFieldValues reaches this panel. The WebView2 page
    ; is driven by a posted message; native has no message loop of its own, so
    ; it exposes a setter over the controls just built instead. Cleared in
    ; _Settings_Close.
    gSettingsNativeFieldSetter := _SettingsNative_MakeFieldSetter(addonFieldGroups)

    ; Features (addon enable/disable) -----------------------------
    UsePage("Features")
    PAdd("Text", "x" contentX " y" contentY " w600",
        "Turn off what you don't use: its tray items, shortcuts and settings page go with it."
        . " Maps++ offers to restart when you save a change here.")
    featureLv := PAdd("ListView", "x" contentX " y+10 w606 h420 Checked -Multi NoSortHdr Grid",
        ["Feature", "Group", "What it does"])
    groupNames := Map("map", "Map", "multibox", "Multi-boxing", "tools", "Tools")
    addonChecks := []
    for entry in _Settings_FeatureList() {
        grpName := groupNames.Has(entry["group"]) ? groupNames[entry["group"]] : "Tools"
        featureLv.Add(entry["enabled"] ? "Check" : "", entry["label"], grpName, entry["description"])
        addonChecks.Push(entry["name"])
    }
    featureLv.ModifyCol(1, 130)
    featureLv.ModifyCol(2, 90)
    featureLv.ModifyCol(3, 360)

    ; Navigation ---------------------------------------------------
    nav := g.Add("TreeView", "x10 y10 w" (NAV_W - 16) " h552 -Buttons -Lines")
    navPages := Map()
    firstItem := 0
    for grp in tabLayout {
        parent := 0
        if (grp["group"] != "")
            parent := nav.Add(grp["group"], 0, "Bold")
        for t in grp["tabs"] {
            if !pages.Has(t["name"])
                continue
            item := nav.Add(t["name"], parent)
            navPages[item] := t["name"]
            if !firstItem
                firstItem := item
        }
        ; Expanded once it has children; a heading added with Expand and no
        ; children yet is not reliably drawn open.
        if parent
            nav.Modify(parent, "Expand")
    }
    nav.OnEvent("ItemSelect", NavSelect)

    ; Shared buttons ---------------------------------------------
    resetDefaultsBtn := g.Add("Button", "x10 y572 w160", "Reset to Defaults")
    saveBtn := g.Add("Button", "x" (NAV_W + 454) " y572 w96 Default", "Save")
    cancelBtn := g.Add("Button", "x" (NAV_W + 558) " y572 w96", "Cancel")
    resetDefaultsBtn.OnEvent("Click", (*) => _Settings_HandleResetDefaults())
    saveBtn.OnEvent("Click", SaveNativeSettings)
    cancelBtn.OnEvent("Click", (*) => _Settings_Close())
    if firstItem
        nav.Modify(firstItem, "Select")
    ShowPage(pageOrder.Length ? pageOrder[1] : "")
    g.Show("w" (NAV_W + 664) " h614")

    ; ── Page plumbing ──

    UsePage(newPage) {
        curPage := newPage
        if !pages.Has(newPage) {
            pages[newPage] := []
            pageOrder.Push(newPage)
        }
        ; The page's own heading, where the tab strip used to name it.
        g.SetFont("s11 Bold")
        pages[newPage].Push(g.Add("Text", "x" (NAV_W + 28) " y18 w600", newPage))
        g.SetFont("s9 Norm")
    }

    ; g.Add for the page being built: shifts absolute x past the tree and
    ; records the control so ShowPage can hide it with the rest of its page.
    ; Its locals are named so no outer variable shares them: a nested function
    ; assigning a name the builder also uses would write the builder's copy.
    PAdd(ctrlType, opts := "", params*) {
        pageCtrl := g.Add(ctrlType, _SettingsNative_ShiftX(opts, NAV_W), params*)
        pages[curPage].Push(pageCtrl)
        return pageCtrl
    }

    AddRevert(revertField, revertCtrl, revertType, revertY) {
        if (!revertField.Has("default") || !IsObject(revertCtrl))
            return
        revertBtnY := (revertType = "checkbox") ? revertY - 2 : revertY - 3
        revertBtn := PAdd("Button", "x526 y" revertBtnY " w110 h24", "Default")
        revertBtn.OnEvent("Click",
            _SettingsNative_MakeRevertHandler(revertCtrl, revertType, revertField["default"]))
    }

    ShowPage(shownPage) {
        for pageKey, pageCtrls in pages {
            pageVisible := (pageKey = shownPage)
            for pageMember in pageCtrls
                pageMember.Visible := pageVisible
        }
    }

    NavSelect(*) {
        navItem := nav.GetSelection()
        if navPages.Has(navItem) {
            ShowPage(navPages[navItem])
            return
        }
        ; A group heading: go to its first page.
        navChild := nav.GetChild(navItem)
        if navChild
            nav.Modify(navChild, "Select")
    }

    BrowseGame(*) {
        global PROCESS_EXE
        selected := FileSelect(1, A_ScriptDir, "Locate " PROCESS_EXE " (game executable)",
            "Executables (*.exe)")
        if (selected != "") {
            gamePathEdit.Value := selected
            gameStatusText.Text := _Settings_GameStatus(selected).text
        }
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
        profileCountEdit.Value := has ? String(prof["count"]) : ""
        profileMonitorDdl.Value := has ? _Settings_MonitorIndexToChoice(prof["monitor"]) : 1
        if IsObject(profileLayoutDdl) {
            profileLayoutDdl.Value := has
                ? _SettingsNative_LaunchLayoutChoice(launchLayoutOptions, prof["layout"]) : 1
        }
        for ctrl in [profileNameEdit, profileAccountsDdl, profileCountEdit, profileMonitorDdl]
            ctrl.Enabled := has
        if IsObject(profileLayoutDdl)
            profileLayoutDdl.Enabled := has
        SyncCountField(true)
        profileSyncing := false
    }

    ; A group launches exactly its rows and "only" exactly one account, so for
    ; those the count is not the user's to set.
    FixedCount() {
        idx := profileAccountsDdl.Value
        if (idx >= 1 && idx <= accountChoices.Length && accountChoices[idx].Has("fixedCount"))
            return accountChoices[idx]["fixedCount"]
        return 0
    }

    ; Locks the Clients box for a group or single account, and puts the
    ; profile's own count back in it — but that only on selecting a profile
    ; (writeCount), never from ApplyProfileFields.
    ;
    ; Writing an Edit raises its Change event, and that event runs as a thread
    ; of its own *after* this returns, past the profileSyncing guard. Called
    ; from ApplyProfileFields — which that very event runs — an unconditional
    ; write started the next round, forever: Settings never appeared and the
    ; script stopped doing anything else. So: no write while the user is
    ; typing, and none that would not change the text, which leaves the event
    ; a write does raise with nothing to do.
    SyncCountField(writeCount := false) {
        has := (profileSel >= 1 && profileSel <= nativeProfiles.Length)
        fixed := has ? FixedCount() : 0
        if fixed {
            SetCountText(String(fixed))
            profileCountEdit.Enabled := false
            profileCountHint.Text := (fixed = 1) ? "set by the account" : "set by the group"
        } else {
            profileCountEdit.Enabled := has
            if (has && writeCount)
                SetCountText(String(nativeProfiles[profileSel]["count"]))
            profileCountHint.Text := ""
        }
    }

    SetCountText(countText) {
        if (profileCountEdit.Value != countText)
            profileCountEdit.Value := countText
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
        if (!FixedCount() && IsInteger(raw) && Integer(raw) >= 1)
            prof["count"] := Min(Integer(raw), 32)
        prof["monitor"] := _Settings_ChoiceToMonitorIndex(profileMonitorDdl.Value)
        if (IsObject(profileLayoutDdl) && launchLayoutOptions.available) {
            li := profileLayoutDdl.Value
            if (li >= 1 && li <= launchLayoutOptions.items.Length)
                prof["layout"] := launchLayoutOptions.items[li].value
        }
        SyncCountField()
        RefreshProfileList()
        RefreshProfileBindings(_SettingsNative_BoundId(primaryProfileDdl, nativeProfiles),
            _SettingsNative_SecondaryId(secondaryProfileDdl, nativeProfiles))
    }

    ; The two binding dropdowns are rebuilt from the draft whenever a name
    ; changes, so they can never offer a name the list no longer shows.
    ; Secondary's list leads with None (run nothing).
    RefreshProfileBindings(primaryId, secondaryId) {
        labels := []
        primaryIdx := 1
        secondaryIdx := 1
        for i, prof in nativeProfiles {
            labels.Push(prof["name"] != "" ? prof["name"] : "(unnamed)")
            if (prof["id"] = primaryId)
                primaryIdx := i
            if (prof["id"] = secondaryId)
                secondaryIdx := i + 1
        }
        profileSyncing := true
        primaryProfileDdl.Delete()
        secondaryProfileDdl.Delete()
        secondaryProfileDdl.Add(["None"])
        if labels.Length {
            primaryProfileDdl.Add(labels)
            secondaryProfileDdl.Add(labels)
            primaryProfileDdl.Value := primaryIdx
        }
        secondaryProfileDdl.Value := (secondaryId = "") ? 1 : secondaryIdx
        profileSyncing := false
    }

    AddProfile(*) {
        if (nativeProfiles.Length >= 32)
            return
        nativeProfiles.Push(Map(
            "id", _SettingsNative_NextProfileId(nativeProfiles),
            "name", "Profile " (nativeProfiles.Length + 1),
            "accounts", "",
            "count", defaultCount,
            "monitor", 0,
            "layout", LAUNCH_LAYOUT_DEFAULT))
        profileSel := nativeProfiles.Length
        RefreshProfileList()
        FillProfileFields()
        RefreshProfileBindings(_SettingsNative_BoundId(primaryProfileDdl, nativeProfiles),
            _SettingsNative_SecondaryId(secondaryProfileDdl, nativeProfiles))
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
            _SettingsNative_SecondaryId(secondaryProfileDdl, nativeProfiles))
    }

    RemoveProfile(*) {
        ; The last profile stays: with none at all the launch actions would fall
        ; back to a synthesised default the user cannot see or edit.
        if (profileSel < 1 || nativeProfiles.Length <= 1)
            return
        primaryId := _SettingsNative_BoundId(primaryProfileDdl, nativeProfiles)
        secondaryId := _SettingsNative_SecondaryId(secondaryProfileDdl, nativeProfiles)
        goneId := nativeProfiles[profileSel]["id"]
        nativeProfiles.RemoveAt(profileSel)
        if (profileSel > nativeProfiles.Length)
            profileSel := nativeProfiles.Length
        ; A binding pointing at the removed profile has to land somewhere real
        ; — or, for secondary, on None, which is real too.
        if (primaryId = goneId)
            primaryId := nativeProfiles[1]["id"]
        if (secondaryId = goneId)
            secondaryId := (nativeProfiles.Length > 1) ? nativeProfiles[nativeProfiles.Length]["id"] : ""
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
        checkedRows := Map()
        row := 0
        loop {
            row := featureLv.GetNext(row, "C")
            if !row
                break
            checkedRows[row] := true
        }
        for i, addonName in addonChecks
            addonToggles[addonName] := checkedRows.Has(i)

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
                "multiClientDelay", Trim(delayEdit.Value),
                ; Same shape the WebView2 page sends, so both frontends land in
                ; _Settings_ApplyProfiles and cannot validate differently.
                "profiles", profilePayload,
                "primaryProfile", _SettingsNative_BoundId(primaryProfileDdl, nativeProfiles),
                "secondaryProfile", _SettingsNative_SecondaryId(secondaryProfileDdl, nativeProfiles)
            ),
            "minimap", Map(
                "source", MINIMAP_SOURCES[sourceDdl.Value],
                "showHoverCoords", hoverCoordsCb.Value ? true : false,
                "followGameScale", followScaleCb.Value ? true : false,
                "scale", Trim(scaleEdit.Value),
                "opacity", Trim(opacityEdit.Value),
                "anchor", anchorDdl.Text,
                "offsetX", Trim(offsetXEdit.Value),
                "offsetY", Trim(offsetYEdit.Value),
                "keepOpen", keepOpenCb.Value ? true : false
            ),
            "appearance", Map(
                "accentScheme", accentSchemeValues[accentSchemeDdl.Value],
                "interfaceMode", interfaceValues[interfaceDdl.Value],
                "developerTools", developerToolsCb.Value ? true : false
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

; Secondary's dropdown leads with None, so its positions are off by one and
; position 1 means "run nothing".
_SettingsNative_SecondaryId(ddl, profiles) {
    idx := ddl.Value - 1
    return (idx >= 1 && idx <= profiles.Length) ? profiles[idx]["id"] : ""
}

; Every absolute "x<n>" in a control's options moved right by dx, leaving
; relative ones (x+8, xp, xm, xs) alone. How the native pages keep the
; coordinates they had inside the old tab strip while sitting beside the tree.
_SettingsNative_ShiftX(opts, dx) {
    out := ""
    pos := 1
    while (found := RegExMatch(opts, "(?<![\w+-])x(\d+)", &m, pos)) {
        out .= SubStr(opts, pos, found - pos) "x" (Integer(m[1]) + dx)
        pos := found + m.Len
    }
    return out SubStr(opts, pos)
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

; ── button field ─────────────────────────────────────────────
; The click is already outside any WebView message callback here, so it calls
; the shared runner directly rather than deferring. Both frontends therefore
; reach the addon through one function.

_SettingsNative_MakeActionHandler(addonName, actionId) {
    return (*) => _Settings_RunAddonAction(addonName, actionId)
}

; ── Host-driven field updates ────────────────────────────────
; An addon action can hand back new values for fields the panel is showing.
; The web frontend receives them as a posted message; here they are applied
; straight to the controls, which is the same thing one step shorter.

_SettingsNative_MakeFieldSetter(addonFieldGroups) {
    return (addonName, fieldId, value) =>
        _SettingsNative_SetFieldValue(addonFieldGroups, addonName, fieldId, value)
}

_SettingsNative_SetFieldValue(addonFieldGroups, addonName, fieldId, value) {
    if !addonFieldGroups.Has(addonName)
        return
    for entry in addonFieldGroups[addonName] {
        field := entry["field"]
        if (!field.Has("id") || field["id"] != fieldId)
            continue
        fieldType := field.Has("type") ? field["type"] : "text"
        if (fieldType = "button" || fieldType = "info" || fieldType = "orderedlist")
            return
        ; The revert path already knows how to write every control type.
        _SettingsNative_ApplyRevert(entry["control"], fieldType, value)
        return
    }
}

; ── Per-field revert ─────────────────────────────────────────
; The web frontend hides its arrow until the value differs from the addon's
; stated default. Native has no cheap way to watch every control for changes,
; so the button is always shown and is simply a no-op when the value already
; matches — the same affordance, without a timer polling the tab.
;
; Rows leave a gutter for this on purpose: the label is w180 at contentX and
; the control is w300 at x218, so x526 w110 lands inside the 650-wide tab.
; Checkbox rows are narrowed to w480 in the builder for the same reason.

_SettingsNative_MakeRevertHandler(ctrl, fieldType, defaultValue) {
    return (*) => _SettingsNative_ApplyRevert(ctrl, fieldType, defaultValue)
}

_SettingsNative_ApplyRevert(ctrl, fieldType, defaultValue) {
    if (fieldType = "checkbox") {
        ctrl.Value := defaultValue ? 1 : 0
        return
    }
    if (fieldType = "dropdown") {
        ctrl.Value := Integer(defaultValue) + 1
        return
    }
    if (fieldType = "combo") {
        text := String(defaultValue)
        try ctrl.Choose(text)
        if (ctrl.Value = 0)
            ctrl.Text := text
        return
    }
    ctrl.Value := String(defaultValue)
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
