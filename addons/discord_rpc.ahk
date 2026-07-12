#Requires AutoHotkey v2.0

; Discord Rich Presence via local IPC (discord-ipc-0..9). No Social SDK auth.
; Requires the Discord desktop client. Application ID is the osMW Discord app.

global _DiscordRpc_APP_ID := "1525870758167969822"
global _DiscordRpc_Enabled := false
global _DiscordRpc_PipeHandle := 0
global _DiscordRpc_Connected := false
global _DiscordRpc_LastPresenceKey := ""
global _DiscordRpc_LastSendTick := 0
global _DiscordRpc_LastGamePid := 0
; Fixed for the whole play session. Re-sending the same start keeps Discord's
; elapsed timer continuous across map/battle updates.
global _DiscordRpc_SessionStart := 0
global _DiscordRpc_ClearStreak := 0

global _DiscordRpc_DEBOUNCE_MS := 4000
global _DiscordRpc_POLL_MS := 1500
global _DiscordRpc_RECONNECT_MS := 15000
global _DiscordRpc_CLEAR_STREAK_NEEDED := 3
global _DiscordRpc_LastReconnectAttempt := 0

RegisterAddon(Map(
    "name",          "DiscordRpc",
    "settingsLabel", "Discord",
    "OnInit",        _DiscordRpc_OnInit,
    "OnSettings",    _DiscordRpc_OnSettings,
    "OnMapChange",   _DiscordRpc_OnMapChange,
    "OnTrayMenu",    _DiscordRpc_OnTrayMenu
))

_DiscordRpc_OnInit() {
    _DiscordRpc_LoadConfig()
    OnExit(_DiscordRpc_OnExit)
    if _DiscordRpc_IsReady()
        _DiscordRpc_Start()
}

_DiscordRpc_OnTrayMenu(trayMenu) {
    trayMenu.Add("Clear Discord Presence", (*) => _DiscordRpc_ClearAndDisconnect())
}

_DiscordRpc_OnSettings(ctx) {
    global _DiscordRpc_Enabled

    g := ctx.gui
    g.Add("Text", "xs y+16 w430",
        "Shows what you're doing in-game on your Discord profile.`n"
        . "Off by default — enable below to opt in.`n"
        . "Requires the Discord desktop client (not web/mobile).")

    enabledCb := g.Add("CheckBox", "xs y+12 w340", "Enable Rich Presence")
    enabledCb.Value := _DiscordRpc_Enabled ? 1 : 0

    ctx.saveHandlers.Push(() => _DiscordRpc_ApplySettings(enabledCb.Value ? true : false))
}

_DiscordRpc_ApplySettings(enabled) {
    global _DiscordRpc_Enabled
    wasEnabled := _DiscordRpc_Enabled
    _DiscordRpc_Enabled := enabled
    _DiscordRpc_SaveConfig()

    if (!enabled) {
        _DiscordRpc_ClearAndDisconnect()
        SetTimer(_DiscordRpc_Tick, 0)
        return
    }

    if !wasEnabled {
        _DiscordRpc_ClearAndDisconnect()
        _DiscordRpc_Start()
    }
}

_DiscordRpc_OnMapChange(mapName, prev) {
    ; Content may have changed; never wipe the session start — that resets Discord's timer.
    _DiscordRpc_UpdatePresence(false)
}

_DiscordRpc_OnExit(*) {
    _DiscordRpc_ClearAndDisconnect()
}

; ── Config ───────────────────────────────────────────────────────

_DiscordRpc_LoadConfig() {
    global _DiscordRpc_Enabled, CONFIG_INI
    enabledVal := Trim(IniRead(CONFIG_INI, "Discord", "Enabled", "0"))
    _DiscordRpc_Enabled := (enabledVal = "1")
}

_DiscordRpc_SaveConfig() {
    global _DiscordRpc_Enabled, CONFIG_INI
    IniWrite(_DiscordRpc_Enabled ? "1" : "0", CONFIG_INI, "Discord", "Enabled")
}

_DiscordRpc_IsReady() {
    global _DiscordRpc_Enabled
    return _DiscordRpc_Enabled
}

_DiscordRpc_Start() {
    global _DiscordRpc_POLL_MS
    if !_DiscordRpc_IsReady()
        return
    SetTimer(_DiscordRpc_Tick, _DiscordRpc_POLL_MS)
    _DiscordRpc_UpdatePresence(false)
}

; ── Presence state ───────────────────────────────────────────────

_DiscordRpc_Tick() {
    global _DiscordRpc_Connected, _DiscordRpc_LastReconnectAttempt, _DiscordRpc_RECONNECT_MS
    if !_DiscordRpc_IsReady() {
        SetTimer(_DiscordRpc_Tick, 0)
        return
    }
    if !_DiscordRpc_Connected {
        now := A_TickCount
        if (now - _DiscordRpc_LastReconnectAttempt < _DiscordRpc_RECONNECT_MS)
            return
        _DiscordRpc_LastReconnectAttempt := now
        _DiscordRpc_Connect()
    }
    _DiscordRpc_UpdatePresence(false)
}

_DiscordRpc_UpdatePresence(force := false) {
    global _DiscordRpc_LastPresenceKey, _DiscordRpc_LastSendTick, _DiscordRpc_LastGamePid
    global _DiscordRpc_DEBOUNCE_MS, _DiscordRpc_SessionStart, _DiscordRpc_ClearStreak
    global _DiscordRpc_CLEAR_STREAK_NEEDED

    if !_DiscordRpc_IsReady()
        return

    snap := _DiscordRpc_BuildSnapshot()
    if (snap.clear) {
        _DiscordRpc_ClearStreak += 1
        ; Ignore brief empty reads (loading screens) so we don't clear/re-set and jump the timer.
        if (_DiscordRpc_LastPresenceKey != "" && _DiscordRpc_ClearStreak >= _DiscordRpc_CLEAR_STREAK_NEEDED) {
            _DiscordRpc_ClearActivity()
            _DiscordRpc_LastPresenceKey := ""
            _DiscordRpc_SessionStart := 0
            _DiscordRpc_ClearStreak := 0
        }
        return
    }
    _DiscordRpc_ClearStreak := 0

    ; Key is display content only — pid swaps must not re-push activity.
    key := snap.details "|" snap.state
    now := A_TickCount
    if (key = _DiscordRpc_LastPresenceKey)
        return
    if (!force && (now - _DiscordRpc_LastSendTick < _DiscordRpc_DEBOUNCE_MS) && _DiscordRpc_LastPresenceKey != "")
        return

    if !_DiscordRpc_EnsureConnected()
        return

    if !_DiscordRpc_SessionStart
        _DiscordRpc_SessionStart := _DiscordRpc_UnixNow()

    ; Tie activity to Maps++ so Discord doesn't treat game focus/pid changes as a new session.
    discordPid := DllCall("GetCurrentProcessId")
    if _DiscordRpc_SendSetActivity(discordPid, snap.details, snap.state, _DiscordRpc_SessionStart) {
        _DiscordRpc_LastPresenceKey := key
        _DiscordRpc_LastSendTick := now
        _DiscordRpc_LastGamePid := snap.pid
    } else {
        ; Keep LastPresenceKey / SessionStart so a retry does not look like a new session.
        _DiscordRpc_Disconnect()
    }
}

_DiscordRpc_BuildSnapshot() {
    global GAME_WIN_FILTER, PROCESS_EXE, gTrackedGameHwnd

    ; Prefer the focused client when available; otherwise keep tracking the last
    ; focused client / any running main.exe so alt-tabbing does not clear presence.
    hwnd := 0
    if WinActive(GAME_WIN_FILTER) {
        hwnd := WinActive(GAME_WIN_FILTER)
        gTrackedGameHwnd := hwnd
    } else if (gTrackedGameHwnd && WinExist("ahk_id " gTrackedGameHwnd)) {
        try {
            if (StrLower(WinGetProcessName("ahk_id " gTrackedGameHwnd)) = StrLower(PROCESS_EXE))
                hwnd := gTrackedGameHwnd
        }
    }
    if !hwnd {
        list := WinGetList(GAME_WIN_FILTER)
        if (list.Length >= 1)
            hwnd := list[1]
    }
    if !hwnd {
        return { clear: true }
    }

    pid := WinGetPID("ahk_id " hwnd)
    mapInfo := _DiscordRpc_ReadMapInfoForPid(pid)
    if (mapInfo.mapId = "") {
        return { clear: true }
    }
    details := (mapInfo.displayName != "") ? mapInfo.displayName : mapInfo.mapId

    inBattle := _DiscordRpc_ReadInBattleForPid(pid)
    charName := _DiscordRpc_ReadCharacterName(hwnd)

    state := inBattle ? "In battle" : (charName != "" ? charName : "Exploring")

    return {
        clear: false,
        pid: pid,
        details: details,
        state: state
    }
}

_DiscordRpc_OpenGameProcess(pid) {
    handle := DllCall("OpenProcess", "UInt", 0x0010 | 0x0400, "Int", 0, "UInt", pid, "Ptr")
    if !handle
        return { ok: false }
    modBase := GetModuleBaseAddress(handle, PROCESS_EXE)
    if !modBase {
        DllCall("CloseHandle", "Ptr", handle)
        return { ok: false }
    }
    EnsureResolvedOffsetsForBuild(handle, modBase)
    return { ok: true, handle: handle, modBase: modBase }
}

_DiscordRpc_ReadMapInfoForPid(pid) {
    proc := _DiscordRpc_OpenGameProcess(pid)
    if !proc.ok
        return { mapId: "", displayName: "" }

    displayName := ""
    nameBuf := Buffer(MAP_NAME_LEN, 0)
    okName := DllCall("ReadProcessMemory",
        "Ptr", proc.handle,
        "Ptr", proc.modBase + GetResolvedOffset("MAP_NAME_OFFSET"),
        "Ptr", nameBuf.Ptr,
        "UPtr", MAP_NAME_LEN,
        "UPtr*", 0,
        "Int")
    if okName {
        displayName := Trim(StrGet(nameBuf, MAP_NAME_LEN, "CP0"), " `t`r`n`0")
        ; Reject garbage / filename spill if the display field is empty or looks like a map file.
        if RegExMatch(displayName, "i)^MAP\d+\.map$")
            displayName := ""
    }

    mapId := ""
    fileBuf := Buffer(MAP_FILE_LEN, 0)
    okFile := DllCall("ReadProcessMemory",
        "Ptr", proc.handle,
        "Ptr", proc.modBase + GetResolvedOffset("MAP_FILE_OFFSET"),
        "Ptr", fileBuf.Ptr,
        "UPtr", MAP_FILE_LEN,
        "UPtr*", 0,
        "Int")
    DllCall("CloseHandle", "Ptr", proc.handle)
    if okFile {
        mapFile := Trim(StrGet(fileBuf, MAP_FILE_LEN, "CP0"), " `t`r`n`0")
        if (mapFile != "")
            mapId := RegExReplace(mapFile, "i)\.map$", "")
    }

    return { mapId: mapId, displayName: displayName }
}

_DiscordRpc_ReadInBattleForPid(pid) {
    proc := _DiscordRpc_OpenGameProcess(pid)
    if !proc.ok
        return false
    valBuf := Buffer(4, 0)
    ok := DllCall("ReadProcessMemory",
        "Ptr", proc.handle,
        "Ptr", proc.modBase + GetResolvedOffset("BATTLE_STATE_OFFSET"),
        "Ptr", valBuf.Ptr,
        "UPtr", 4,
        "UPtr*", 0,
        "Int")
    DllCall("CloseHandle", "Ptr", proc.handle)
    if !ok
        return false
    return NumGet(valBuf, 0, "Int") != 0
}

_DiscordRpc_ReadCharacterName(hwnd) {
    try title := WinGetTitle("ahk_id " hwnd)
    catch
        return ""
    if RegExMatch(title, "Behemoth:\s+(.+?)\s+ID\b", &m)
        return m[1]
    return ""
}

; ── Discord IPC ──────────────────────────────────────────────────

_DiscordRpc_EnsureConnected() {
    global _DiscordRpc_Connected
    if _DiscordRpc_Connected
        return true
    return _DiscordRpc_Connect()
}

_DiscordRpc_Connect() {
    global _DiscordRpc_APP_ID, _DiscordRpc_PipeHandle, _DiscordRpc_Connected
    _DiscordRpc_Disconnect()

    Loop 10 {
        pipePath := "\\.\pipe\discord-ipc-" (A_Index - 1)
        handle := DllCall("CreateFileW",
            "Str", pipePath,
            "UInt", 0xC0000000,
            "UInt", 0,
            "Ptr", 0,
            "UInt", 3,
            "UInt", 0,
            "UInt", 0,
            "Ptr")
        if (handle = -1 || handle = 0xFFFFFFFF)
            continue

        _DiscordRpc_PipeHandle := handle
        payload := '{"v":1,"client_id":"' _DiscordRpc_APP_ID '"}'
        if !_DiscordRpc_SendFrame(0, payload) {
            _DiscordRpc_Disconnect()
            continue
        }
        ; Consume handshake response (ignore content).
        _DiscordRpc_ReadFrame()
        _DiscordRpc_Connected := true
        return true
    }
    return false
}

_DiscordRpc_Disconnect() {
    global _DiscordRpc_PipeHandle, _DiscordRpc_Connected
    if _DiscordRpc_PipeHandle {
        try DllCall("CloseHandle", "Ptr", _DiscordRpc_PipeHandle)
    }
    _DiscordRpc_PipeHandle := 0
    _DiscordRpc_Connected := false
}

_DiscordRpc_ClearAndDisconnect() {
    global _DiscordRpc_LastPresenceKey, _DiscordRpc_SessionStart, _DiscordRpc_ClearStreak
    _DiscordRpc_ClearActivity()
    _DiscordRpc_Disconnect()
    _DiscordRpc_LastPresenceKey := ""
    _DiscordRpc_SessionStart := 0
    _DiscordRpc_ClearStreak := 0
}

_DiscordRpc_ClearActivity() {
    global _DiscordRpc_Connected
    if !_DiscordRpc_Connected
        return
    _DiscordRpc_SendSetActivity(DllCall("GetCurrentProcessId"), "", "", 0, true)
}

_DiscordRpc_SendSetActivity(pid, details, state, startTs := 0, clear := false) {
    nonce := _DiscordRpc_NewNonce()
    if clear {
        json := '{"cmd":"SET_ACTIVITY","args":{"pid":' pid ',"activity":null},"nonce":"' nonce '"}'
    } else {
        activity := '{"details":"' _DiscordRpc_EscapeJson(details) '","state":"' _DiscordRpc_EscapeJson(state) '"'
        if startTs
            activity .= ',"timestamps":{"start":' startTs '}'
        activity .= '}'
        json := '{"cmd":"SET_ACTIVITY","args":{"pid":' pid ',"activity":' activity '},"nonce":"' nonce '"}'
    }
    if !_DiscordRpc_SendFrame(1, json)
        return false
    resp := _DiscordRpc_ReadFrame()
    return resp != ""
}

_DiscordRpc_SendFrame(opcode, jsonPayload) {
    global _DiscordRpc_PipeHandle
    if !_DiscordRpc_PipeHandle
        return false

    payloadLen := StrPut(jsonPayload, "UTF-8") - 1
    payload := Buffer(payloadLen, 0)
    StrPut(jsonPayload, payload, "UTF-8")

    header := Buffer(8, 0)
    NumPut("UInt", opcode, header, 0)
    NumPut("UInt", payloadLen, header, 4)

    written := 0
    if !DllCall("WriteFile", "Ptr", _DiscordRpc_PipeHandle, "Ptr", header.Ptr, "UInt", 8, "UInt*", &written, "Ptr", 0)
        || written != 8
        return false
    if (payloadLen > 0) {
        if !DllCall("WriteFile", "Ptr", _DiscordRpc_PipeHandle, "Ptr", payload.Ptr, "UInt", payloadLen, "UInt*", &written, "Ptr", 0)
            || written != payloadLen
            return false
    }
    return true
}

_DiscordRpc_ReadFrame() {
    global _DiscordRpc_PipeHandle
    if !_DiscordRpc_PipeHandle
        return ""

    header := Buffer(8, 0)
    read := 0
    if !DllCall("ReadFile", "Ptr", _DiscordRpc_PipeHandle, "Ptr", header.Ptr, "UInt", 8, "UInt*", &read, "Ptr", 0)
        || read != 8
        return ""

    payloadLen := NumGet(header, 4, "UInt")
    if (payloadLen < 1 || payloadLen > 65536)
        return ""

    payload := Buffer(payloadLen, 0)
    if !DllCall("ReadFile", "Ptr", _DiscordRpc_PipeHandle, "Ptr", payload.Ptr, "UInt", payloadLen, "UInt*", &read, "Ptr", 0)
        || read != payloadLen
        return ""

    return StrGet(payload.Ptr, payloadLen, "UTF-8")
}

_DiscordRpc_EscapeJson(str) {
    str := StrReplace(str, "\", "\\")
    str := StrReplace(str, '"', '\"')
    str := StrReplace(str, "`n", "\n")
    str := StrReplace(str, "`r", "\r")
    str := StrReplace(str, "`t", "\t")
    return str
}

_DiscordRpc_NewNonce() {
    return Format("{:08X}{:08X}", Random(0, 0xFFFFFFFF), Random(0, 0xFFFFFFFF))
}

_DiscordRpc_UnixNow() {
    return DateDiff(A_NowUTC, "19700101000000", "Seconds")
}
