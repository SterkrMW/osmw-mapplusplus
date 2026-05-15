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
    invMenu := Menu()
    invMenu.Add("Open (Click)`tAlt+E",            (*) => _InventoryToggle_OpenInventory())
    invMenu.Add("Open (Send Alt+I)`tAlt+Shift+E", (*) => _InventoryToggle_OpenInventoryAlt())
    trayMenu.Add("Inventory", invMenu)
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
