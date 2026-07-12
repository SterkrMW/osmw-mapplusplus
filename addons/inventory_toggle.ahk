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

_InventoryToggle_OnTrayMenu(trayMenu) {
    invMenu := Menu()
    invMenu.Add("Open (Click)`t" GetHotkeyDisplay("inventoryOpenClick"), (*) => _InventoryToggle_OpenInventory())
    invMenu.Add("Open (Send Alt+I)`t" GetHotkeyDisplay("inventoryOpenSend"), (*) => _InventoryToggle_OpenInventoryAlt())
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
