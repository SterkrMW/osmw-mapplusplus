#Requires AutoHotkey v2.0

global _BattleSend_PROCESS_ACCESS := 0x0008 | 0x0010 | 0x0020 | 0x0400

RegisterAddonOffset("BattleAction", 0x30B5F8)
RegisterAddonOffset("PetBattleAction", 0x3029C4)

RegisterAddon(Map(
    "name",       "BattleSend",
    "OnTrayMenu", _BattleSend_OnTrayMenu
))

+!q:: _BattleSend_SendToFighting()

_BattleSend_OnTrayMenu(trayMenu) {
    trayMenu.Add("Send Alt+Q to Fighting`tShift+Alt+Q", (*) => _BattleSend_SendToFighting())
}

_BattleSend_SendToFighting() {
    buf := Buffer(4, 0)
    NumPut("Int", 9, buf, 0)
    for hwnd in WinGetList(GAME_WIN_FILTER) {
        pid := WinGetPID("ahk_id " hwnd)
        handle := DllCall("OpenProcess",
            "UInt", _BattleSend_PROCESS_ACCESS,
            "Int", 0, "UInt", pid, "Ptr")
        if !handle
            continue
        modBase := GetModuleBaseAddress(handle, PROCESS_EXE)
        if !modBase {
            DllCall("CloseHandle", "Ptr", handle)
            continue
        }
        stateBuf := Buffer(4, 0)
        ok := DllCall("ReadProcessMemory",
            "Ptr", handle,
            "Ptr", modBase + GetResolvedOffset("BATTLE_STATE_OFFSET"),
            "Ptr", stateBuf.Ptr,
            "UPtr", 4, "UPtr*", 0, "Int")
        if !ok || NumGet(stateBuf, 0, "Int") = 0 {
            DllCall("CloseHandle", "Ptr", handle)
            continue
        }
        actionBuf := Buffer(4, 0)
        for _, offsetName in ["BattleAction", "PetBattleAction"] {
            ok := DllCall("ReadProcessMemory",
                "Ptr", handle,
                "Ptr", modBase + GetResolvedOffset(offsetName),
                "Ptr", actionBuf.Ptr,
                "UPtr", 4, "UPtr*", 0, "Int")
            if !ok || NumGet(actionBuf, 0, "Int") != 255
                continue
            DllCall("WriteProcessMemory",
                "Ptr", handle,
                "Ptr", modBase + GetResolvedOffset(offsetName),
                "Ptr", buf.Ptr,
                "UPtr", 4, "UPtr*", 0, "Int")
        }
        DllCall("CloseHandle", "Ptr", handle)
    }
}
