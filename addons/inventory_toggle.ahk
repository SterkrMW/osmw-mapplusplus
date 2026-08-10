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
    key := _InventoryToggle_TriggerKey("inventoryOpenClick")
    _InventoryToggle_AwaitRelease(key)
    BlockInput("Mouse")
    try {
        ControlClick("x425 y569", "ahk_id " activeHwnd, , "Left", 1, "NA")
    } catch {
        ; The client went away between the focus check and the click.
    } finally {
        BlockInput("Default")
    }
    ; Holding the key past the timeout above lets auto-repeat re-enter this
    ; handler, which would toggle the panel open and shut. Absorb the repeats.
    _InventoryToggle_AwaitRelease(key)
}

; Sends Alt+I, which is what the action says it does.
;
; This used to be SendEvent("{Blind}{i}"): {Blind} passes the modifiers that are
; physically down straight through, and the chord that triggers this is
; Alt+Shift+E, so the client actually received Alt+SHIFT+I.
;
; Stating the combination outright is the whole fix. The KeyWait loop that also
; came with it — waiting out Alt, Shift and Control, up to 0.9s of dead time —
; was belt and braces: Send in any mode but {Blind} already forces the modifier
; state to exactly what the keystroke asks for, lifting a physically-held Shift
; for the duration and putting it back afterwards. Opting out of that is what
; {Blind} did wrong, so with it gone there is nothing left to wait for.
_InventoryToggle_OpenInventoryAlt() {
    activeHwnd := WinActive(GAME_WIN_FILTER)
    if !activeHwnd
        return
    key := _InventoryToggle_TriggerKey("inventoryOpenSend")
    _InventoryToggle_AwaitRelease(key)
    SetKeyDelay 0, 100
    SendEvent("!i")
    _InventoryToggle_AwaitRelease(key)
}

; Both actions fire on the press of their chord but act on the *release* of its
; key, and deliberately do not care what the modifiers are doing.
;
; The click used to KeyWait("Alt"), which stalled it for as long as Alt stayed
; down. Players hold Alt across a whole sequence (Alt+E to open the bag, then
; Alt+G to give), so that read as an uncomfortable delay on every press and an
; indefinite one mid-sequence. Alt being held is now simply irrelevant: the
; click is posted straight to the control and carries no modifier state.
;
; The chord is rebindable, so the key to wait on comes from the registry.
_InventoryToggle_TriggerKey(actionId) {
    return HotkeyTriggerKey(GetHotkeyChord(actionId))
}

; Bounded, so a key the hook believes is stuck can never park the hotkey thread,
; and tolerant of a key name KeyWait does not accept (a mouse or wheel binding).
; Both are why the tray and radial entries, which arrive with nothing held, pass
; through this at no cost.
_InventoryToggle_AwaitRelease(key) {
    if (key = "")
        return
    try KeyWait(key, "T1.0")
}
