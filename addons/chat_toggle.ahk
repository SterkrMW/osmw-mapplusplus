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

_ChatToggle_OnTrayMenu(trayGroups) {
    chatMenu := Menu()
    chatMenu.Add("Toggle All`t" GetHotkeyDisplay("chatToggleAll"), (*) => _ChatToggle_ToggleAllExceptActive())
    chatMenu.Add("Toggle Active`t" GetHotkeyDisplay("chatToggleActive"), (*) => _ChatToggle_ToggleActiveChat())
    chatMenu.Add("Toggle Size`t" GetHotkeyDisplay("chatToggleSize"), (*) => _ChatToggle_ToggleSize())
    trayGroups["quickActions"].Add("Chat", chatMenu)
}

; One click per *process*. GetTopLevelGameWindows filters child and owned
; windows, but a client can still contribute more than one top-level hwnd —
; clicking twice toggles chat back off and looks like the hotkey did nothing.
; Same fix view_mode already carries for its own write path.
_ChatToggle_ToggleAllExceptActive() {
    activeHwnd := WinActive(GAME_WIN_FILTER)
    activePid := 0
    if activeHwnd
        try activePid := WinGetPID("ahk_id " activeHwnd)
    seen := Map()
    for hwnd in GetTopLevelGameWindows() {
        if (hwnd = activeHwnd)
            continue
        pid := 0
        try pid := WinGetPID("ahk_id " hwnd)
        if (!pid || pid = activePid || seen.Has(pid))
            continue
        seen[pid] := true
        try ControlClick("x30 y550", "ahk_id " hwnd, , "Left", 1, "NA")
    }
}

_ChatToggle_ToggleActiveChat() {
    activeHwnd := WinActive(GAME_WIN_FILTER)
    if !activeHwnd
        return
    try ControlClick("x30 y550", "ahk_id " activeHwnd, , "Left", 1, "NA")
}

_ChatToggle_ToggleSize() {
    rva := GetResolvedOffset("ChatMiniSize")
    currentVal := _ChatToggle_ReadFirst(rva)
    ; -1 means nothing could be read. Writing anyway would push every client to
    ; the "big" size on the strength of a failed read.
    if (currentVal < 0) {
        TrayTip("Could not read the chat size from any client.", "Chat", "Iconx")
        return
    }
    newVal := (currentVal = 0) ? 9 : 0
    _ChatToggle_WriteAll(rva, newVal)
}

; The current value from the first client that answers, or -1 when none does.
_ChatToggle_ReadFirst(rva) {
    for hwnd in GetTopLevelGameWindows() {
        pid := 0
        try pid := WinGetPID("ahk_id " hwnd)
        if !pid
            continue
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
        ; The return value matters: an unchecked failure leaves the zeroed
        ; buffer, which reads as a legitimate "chat is small".
        ok := DllCall("ReadProcessMemory",
            "Ptr", handle,
            "Ptr", modBase + rva,
            "Ptr", buf.Ptr,
            "UPtr", 4, "UPtr*", 0, "Int")
        DllCall("CloseHandle", "Ptr", handle)
        if ok
            return NumGet(buf, 0, "Int")
    }
    return -1
}

_ChatToggle_WriteAll(rva, value) {
    seen := Map()
    buf := Buffer(4, 0)
    NumPut("Int", value, buf, 0)
    for hwnd in GetTopLevelGameWindows() {
        pid := 0
        try pid := WinGetPID("ahk_id " hwnd)
        if (!pid || seen.Has(pid))
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
