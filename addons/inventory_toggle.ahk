#Requires AutoHotkey v2.0

RegisterAddon(Map(
    "name",       "InventoryToggle",
    "OnTrayMenu", _InventoryToggle_OnTrayMenu
))

#HotIf WinActive(GAME_WIN_FILTER)
!+e::  _InventoryToggle_OpenInventory()
!e:: _InventoryToggle_OpenInventoryAlt()
#HotIf

_InventoryToggle_OnTrayMenu(trayMenu) {
    trayMenu.Add("Open Inventory (Click)`tAlt+E",       (*) => _InventoryToggle_OpenInventory())
    trayMenu.Add("Open Inventory (Send Alt+I)`tAlt+Shift+E", (*) => _InventoryToggle_OpenInventoryAlt())
}

_InventoryToggle_OpenInventory() {
    activeHwnd := WinActive(GAME_WIN_FILTER)
    if !activeHwnd
        return
    KeyWait("Alt")
    BlockInput("Mouse")
    ControlClick("x425 y569", "ahk_id " activeHwnd, , "Left", 1, "NA")
    BlockInput("Default")
}

_InventoryToggle_OpenInventoryAlt() {
    activeHwnd := WinActive(GAME_WIN_FILTER)
    if !activeHwnd
        return
    SetKeyDelay 0, 100
    SendEvent("{Blind}{i}")
}
