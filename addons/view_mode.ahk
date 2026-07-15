#Requires AutoHotkey v2.0

global _ViewMode_PROCESS_ACCESS := 0x0008 | 0x0010 | 0x0020 | 0x0400

RegisterAddonOffset("ViewMode", 0x301DD8)

RegisterAddon(Map(
    "name",       "ViewMode",
    "OnTrayMenu", _ViewMode_OnTrayMenu
))

RegisterHotkeyAction(Map(
    "id", "viewModeCycle",
    "label", "Cycle view mode (all clients)",
    "category", "View Mode",
    "default", "!2",
    "addon", "ViewMode",
    "handler", _ViewMode_CycleAll,
    "hotIfWinActive", true
))
RegisterHotkeyAction(Map(
    "id", "viewModeToggle",
    "label", "Toggle view mode 0/1 (all clients)",
    "category", "View Mode",
    "default", "!1",
    "addon", "ViewMode",
    "handler", _ViewMode_ToggleAll,
    "hotIfWinActive", true
))

_ViewMode_OnTrayMenu(trayMenu) {
    vmMenu := Menu()
    vmMenu.Add("Cycle (All)`t" GetHotkeyDisplay("viewModeCycle"), (*) => _ViewMode_CycleAll())
    vmMenu.Add("Toggle 0/1 (All)`t" GetHotkeyDisplay("viewModeToggle"), (*) => _ViewMode_ToggleAll())
    trayMenu.Add("View Mode", vmMenu)
}

_ViewMode_CycleAll() {
    _ViewMode_ApplyToAll("cycle")
}

_ViewMode_ToggleAll() {
    _ViewMode_ApplyToAll("toggle")
}

; One write per process. WinGetList can return several hwnds for the same
; main.exe; processing each one used to apply +1 twice (0→1→2).
_ViewMode_ApplyToAll(mode) {
    static busy := false
    if busy
        return
    busy := true
    try {
        offset := GetResolvedOffset("ViewMode")
        seenPid := Map()
        for hwnd in GetTopLevelGameWindows() {
            pid := WinGetPID("ahk_id " hwnd)
            if seenPid.Has(pid)
                continue
            seenPid[pid] := true

            handle := DllCall("OpenProcess",
                "UInt", _ViewMode_PROCESS_ACCESS,
                "Int", 0, "UInt", pid, "Ptr")
            if !handle
                continue
            modBase := GetModuleBaseAddress(handle, PROCESS_EXE)
            if !modBase {
                DllCall("CloseHandle", "Ptr", handle)
                continue
            }

            addr := modBase + offset
            valBuf := Buffer(1, 0)
            ok := DllCall("ReadProcessMemory",
                "Ptr", handle,
                "Ptr", addr,
                "Ptr", valBuf.Ptr,
                "UPtr", 1, "UPtr*", 0, "Int")
            if !ok {
                DllCall("CloseHandle", "Ptr", handle)
                continue
            }

            current := NumGet(valBuf, 0, "UChar") & 3
            if (mode = "cycle")
                newVal := (current + 1) & 3
            else
                newVal := (current = 0) ? 1 : 0

            writeBuf := Buffer(1, 0)
            NumPut("UChar", newVal, writeBuf, 0)
            DllCall("WriteProcessMemory",
                "Ptr", handle,
                "Ptr", addr,
                "Ptr", writeBuf.Ptr,
                "UPtr", 1, "UPtr*", 0, "Int")
            DllCall("CloseHandle", "Ptr", handle)
        }
    } finally {
        busy := false
    }
}
