#Requires AutoHotkey v2.0

global _BattleSend_PROCESS_ACCESS := 0x0008 | 0x0010 | 0x0020 | 0x0400

RegisterAddonOffset("BattleAction", 0x30B5F8)
RegisterAddonOffset("PetBattleAction", 0x3029C4)

RegisterAddon(Map(
    "name",       "BattleSend",
    "OnTrayMenu", _BattleSend_OnTrayMenu
))

; The name says "send" for historical reasons only — nothing is sent. The battle
; action is written straight into each fighting client's memory below, so the
; label describes the effect rather than a keystroke that never happens.
RegisterHotkeyAction(Map(
    "id", "battleSend",
    "label", "Trigger battle action on fighting clients",
    "category", "Battle",
    "default", "+!q",
    "addon", "BattleSend",
    "handler", _BattleSend_SendToFighting,
    "hotIfWinActive", true
))

_BattleSend_OnTrayMenu(trayGroups) {
    trayGroups["quickActions"].Add("Battle Action on Fighting`t" GetHotkeyDisplay("battleSend"), (*) => _BattleSend_SendToFighting())
}

; One pass per *process*: a client can contribute more than one top-level hwnd,
; and opening/writing it twice is wasted work at best.
_BattleSend_SendToFighting() {
    buf := Buffer(4, 0)
    NumPut("Int", 9, buf, 0)
    seen := Map()
    for hwnd in GetTopLevelGameWindows() {
        pid := 0
        ; The window can close between the enumeration and this call.
        try pid := WinGetPID("ahk_id " hwnd)
        if (!pid || seen.Has(pid))
            continue
        seen[pid] := true
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
