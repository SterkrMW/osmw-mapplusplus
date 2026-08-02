#Requires AutoHotkey v2.0

global _ChatToggle_PROCESS_ACCESS := 0x0008 | 0x0010 | 0x0020 | 0x0400

RegisterAddonOffset("ChatMiniSize", 0x30BFC8)

RegisterAddon(Map(
    "name",       "ChatToggle",
    "OnTrayMenu", _ChatToggle_OnTrayMenu
))

RegisterHotkeyAction(Map(
    "id", "chatToggleAll",
    "label", "Toggle chat on all other clients",
    "category", "Chat",
    "default", "+^c",
    "addon", "ChatToggle",
    "handler", _ChatToggle_ToggleAllExceptActive,
    "hotIfWinActive", true
))
RegisterHotkeyAction(Map(
    "id", "chatToggleActive",
    "label", "Toggle chat on active client",
    "category", "Chat",
    "default", "^c",
    "addon", "ChatToggle",
    "handler", _ChatToggle_ToggleActiveChat,
    "hotIfWinActive", true
))
RegisterHotkeyAction(Map(
    "id", "chatToggleSize",
    "label", "Toggle mini chat size on all clients",
    "category", "Chat",
    "default", "!^c",
    "addon", "ChatToggle",
    "handler", _ChatToggle_ToggleSize,
    "hotIfWinActive", true
))

_ChatToggle_OnTrayMenu(trayMenu) {
    chatMenu := Menu()
    chatMenu.Add("Toggle All`t" GetHotkeyDisplay("chatToggleAll"), (*) => _ChatToggle_ToggleAllExceptActive())
    chatMenu.Add("Toggle Active`t" GetHotkeyDisplay("chatToggleActive"), (*) => _ChatToggle_ToggleActiveChat())
    chatMenu.Add("Toggle Size`t" GetHotkeyDisplay("chatToggleSize"), (*) => _ChatToggle_ToggleSize())
    trayMenu.Add("Chat", chatMenu)
}

_ChatToggle_ToggleAllExceptActive() {
    activeHwnd := WinActive(GAME_WIN_FILTER)
    for hwnd in WinGetList(GAME_WIN_FILTER) {
        if (hwnd = activeHwnd)
            continue
        ControlClick("x30 y550", "ahk_id " hwnd, , "Left", 1, "NA")
    }
}

_ChatToggle_ToggleActiveChat() {
    activeHwnd := WinActive(GAME_WIN_FILTER)
    if !activeHwnd
        return
    ControlClick("x30 y550", "ahk_id " activeHwnd, , "Left", 1, "NA")
}

_ChatToggle_ToggleSize() {
    rva := GetResolvedOffset("ChatMiniSize")
    currentVal := _ChatToggle_ReadFirst(rva)
    newVal := (currentVal = 0) ? 9 : 0
    _ChatToggle_WriteAll(rva, newVal)
}

_ChatToggle_ReadFirst(rva) {
    for hwnd in WinGetList(GAME_WIN_FILTER) {
        pid := WinGetPID("ahk_id " hwnd)
        handle := DllCall("OpenProcess",
            "UInt", _ChatToggle_PROCESS_ACCESS,
            "Int", 0, "UInt", pid, "Ptr")
        if !handle
            continue
        modBase := GetModuleBaseAddress(handle, PROCESS_EXE)
        if !modBase {
            DllCall("CloseHandle", "Ptr", handle)
            continue
        }
        buf := Buffer(4, 0)
        DllCall("ReadProcessMemory",
            "Ptr", handle,
            "Ptr", modBase + rva,
            "Ptr", buf.Ptr,
            "UPtr", 4, "UPtr*", 0, "Int")
        DllCall("CloseHandle", "Ptr", handle)
        return NumGet(buf, 0, "Int")
    }
    return 0
}

_ChatToggle_WriteAll(rva, value) {
    seen := Map()
    buf := Buffer(4, 0)
    NumPut("Int", value, buf, 0)
    for hwnd in WinGetList(GAME_WIN_FILTER) {
        pid := WinGetPID("ahk_id " hwnd)
        if seen.Has(pid)
            continue
        seen[pid] := true
        handle := DllCall("OpenProcess",
            "UInt", _ChatToggle_PROCESS_ACCESS,
            "Int", 0, "UInt", pid, "Ptr")
        if !handle
            continue
        modBase := GetModuleBaseAddress(handle, PROCESS_EXE)
        if modBase {
            DllCall("WriteProcessMemory",
                "Ptr", handle,
                "Ptr", modBase + rva,
                "Ptr", buf.Ptr,
                "UPtr", 4, "UPtr*", 0, "Int")
        }
        DllCall("CloseHandle", "Ptr", handle)
    }
}
