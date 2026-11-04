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
    "hotIfFn", _InventoryToggle_HotIfClick
))

; The client patch inventory_hotkey.py makes Alt+E open the bag natively. It
; rewrites one jcc in the Alt+E handler, at this RVA, from "leave unless in a
; fight" to "open the inventory unless in a fight". Code bytes, so a client
; update moves them; anything that is neither form reads as "unknown" and the
; Maps++ remap stays on.
global _InventoryToggle_ALTE_JCC_RVA := 0x47DA5
global _InventoryToggle_ALTE_STOCK := "0F85B4030000"
global _InventoryToggle_ALTE_PATCHED := "0F8547FFFFFF"
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

; When the click hotkey fires. It stands down — letting the keystroke through to
; the game — wherever the game now does better itself, but only while the chord
; IS Alt+E, the one key it can collide with:
;   - in a fight, where Alt+E is the game's own Skill command and the remap used
;     to swallow it;
;   - on a client carrying inventory_hotkey.py, which opens the bag on Alt+E
;     natively. A click on top would open it and shut it again, and the patch's
;     notes ask for exactly this: remove the remap once it is applied.
; Rebound to anything else, it never collided with the game and works as before.
; The tray and Quick Actions entries call the handler directly and are unaffected.
_InventoryToggle_HotIfClick(*) {
    if !WinActive(GAME_WIN_FILTER)
        return false
    if (CanonicalHotkeyChord(GetHotkeyChord("inventoryOpenClick")) != "!e")
        return true
    if _InventoryToggle_InBattle()
        return false
    return _InventoryToggle_NativeAltE() != "patched"
}

_InventoryToggle_InBattle() {
    cached := GetCachedProcessHandleAndBase()
    if !cached.ok
        return false
    buf := ReadClientBuffer(cached, GetResolvedOffset("BATTLE_STATE_OFFSET"), 4)
    return buf ? (NumGet(buf, 0, "Int") != 0) : false
}

; "patched" | "stock" | "unknown" for the focused client. Fixed for a process's
; lifetime, so it is read once per pid; the hotkey's HotIf asks on every press.
_InventoryToggle_NativeAltE() {
    global _InventoryToggle_ALTE_JCC_RVA, _InventoryToggle_ALTE_STOCK, _InventoryToggle_ALTE_PATCHED
    static cache := Map()
    cached := GetCachedProcessHandleAndBase()
    if !cached.ok
        return "unknown"
    key := cached.pid ":" cached.modBase
    if cache.Has(key)
        return cache[key]
    verdict := "unknown"
    if (buf := ReadClientBuffer(cached, _InventoryToggle_ALTE_JCC_RVA, 6)) {
        hex := ""
        loop 6
            hex .= Format("{:02X}", NumGet(buf, A_Index - 1, "UChar"))
        verdict := (hex = _InventoryToggle_ALTE_PATCHED) ? "patched"
            : (hex = _InventoryToggle_ALTE_STOCK) ? "stock" : "unknown"
    }
    if (cache.Count >= 32)
        cache := Map()
    cache[key] := verdict
    if (verdict = "patched")
        LogInfo("InventoryToggle", "Client " cached.pid " opens the inventory on Alt+E itself"
            . " — the Maps++ remap stands down for it")
    return verdict
}

; The bag button sits at (425, 569) on the game's 800×600 surface. Converted to
; real client pixels on the game's own window, so it still lands on the button
; when the client mods scale the game 2×/3× or letterbox it.
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
    target := LogicalToClient(activeHwnd, 425, 569)
    BlockInput("Mouse")
    try {
        ControlClick("x" target.x " y" target.y, "ahk_id " target.hwnd, , "Left", 1, "NA")
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
