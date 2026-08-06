#Requires AutoHotkey v2.0

RegisterAddon(Map(
    "name",       "InventoryToggle",
    "OnTrayMenu", _InventoryToggle_OnTrayMenu
))

RegisterHotkeyAction(Map(
    "id", "inventoryOpenClick",
    "label", "Open inventory (click)",
    "category", "Inventory",
    "default", "!e",
    "addon", "InventoryToggle",
    "handler", _InventoryToggle_OpenInventory,
    "hotIfWinActive", true
))
RegisterHotkeyAction(Map(
    "id", "inventoryOpenSend",
    "label", "Open inventory (send Alt+I)",
    "category", "Inventory",
    "default", "!+e",
    "addon", "InventoryToggle",
    "handler", _InventoryToggle_OpenInventoryAlt,
    "hotIfWinActive", true
))

_InventoryToggle_OnTrayMenu(trayGroups) {
    invMenu := Menu()
    invMenu.Add("Open (Click)`t" GetHotkeyDisplay("inventoryOpenClick"), (*) => _InventoryToggle_OpenInventory())
    invMenu.Add("Open (Send Alt+I)`t" GetHotkeyDisplay("inventoryOpenSend"), (*) => _InventoryToggle_OpenInventoryAlt())
    trayGroups["quickActions"].Add("Inventory", invMenu)
}

; The click coordinates are client-area pixels for the fixed-size osMW client.
;
; BlockInput must be released on every path. ControlClick throws when the target
; window dies mid-call, and without the finally that leaves the user's mouse
; blocked with no way back short of killing the script.
_InventoryToggle_OpenInventory() {
    activeHwnd := WinActive(GAME_WIN_FILTER)
    if !activeHwnd
        return
    KeyWait("Alt")
    BlockInput("Mouse")
    try {
        ControlClick("x425 y569", "ahk_id " activeHwnd, , "Left", 1, "NA")
    } catch {
        ; The client went away between the focus check and the click.
    } finally {
        BlockInput("Default")
    }
}

; Sends Alt+I, which is what the action says it does.
;
; This used to be SendEvent("{Blind}{i}"): {Blind} passes the modifiers that are
; physically down straight through, and the chord that triggers this is Alt+Shift+E,
; so the client actually received Alt+SHIFT+I. Our own modifiers are released
; first and the combination is then stated outright.
_InventoryToggle_OpenInventoryAlt() {
    activeHwnd := WinActive(GAME_WIN_FILTER)
    if !activeHwnd
        return
    _InventoryToggle_ReleaseModifiers()
    SetKeyDelay 0, 100
    SendEvent("!i")
}

; A held modifier from our own chord must not ride along on the Send. Bounded so
; a stuck key can never park a hotkey thread.
_InventoryToggle_ReleaseModifiers() {
    for key in ["Alt", "Shift", "Control"]
        KeyWait(key, "T0.3")
}
