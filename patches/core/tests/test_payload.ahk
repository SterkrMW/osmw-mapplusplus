#Requires AutoHotkey v2.0

PayloadTestAssert(condition, message) {
    if !condition
        throw Error(message)
}

PayloadTestWrite(path, content) {
    if FileExist(path)
        FileDelete(path)
    FileAppend(content, path, "UTF-8-RAW")
}

PayloadTestRead(path) {
    return FileRead(path, "UTF-8")
}

RunPayloadTransactionTest() {
    global APP_VERSION, LOG_MAX_BYTES, gLogPath, gLogFailed

    APP_VERSION := "payload-fixture"
    LOG_MAX_BYTES := 1048576
    gLogPath := ""
    gLogFailed := false
    resultPath := A_Args.Length ? A_Args[1]
        : A_ScriptDir "\..\build\test_payload.result"
    if FileExist(resultPath)
        FileDelete(resultPath)

    root := A_Temp "\osmw-payload-" DllCall("GetCurrentProcessId", "UInt")
        . "-" A_TickCount
    source := root "\source"
    lib := source "\Lib"
    game := root "\game"
    try {
        DirCreate(lib)
        DirCreate(game)
        gLogPath := root "\test.log"

        modules := ManagedPayloadModuleCsv()
        for name in ManagedPayloadFiles() {
            path := (name = "osmw_chat_launch.exe"
                || name = "osmw_login_launch.exe")
                ? source "\" name : lib "\" name
            if (name = "osmw_core_api.ini") {
                PayloadTestWrite(path,
                    "[CoreApi]`nFormat=1`nAbi=1.0`nPacked=0x00010000`n"
                    . "Modules=" modules "`n")
            } else {
                PayloadTestWrite(path, name "|v1")
            }
        }

        PayloadTestAssert(EnsureManagedPayload(game, source),
            "initial payload deploy failed")
        ; Metadata has real INI content, so check it separately from fixtures.
        for name in ManagedPayloadFiles() {
            if (name = "osmw_core_api.ini")
                continue
            PayloadTestAssert(PayloadTestRead(game "\" name) = name "|v1",
                "initial content mismatch for " name)
        }
        PayloadTestAssert(
            IniRead(game "\osmw_payload.ini", "Payload", "CoreAbi", "") = "1.0",
            "ABI stamp missing")
        PayloadTestAssert(
            IniRead(game "\osmw_payload.ini", "Payload", "Modules", "") = modules,
            "module stamp mismatch")

        ; Preflight happens even with a fresh existing stamp. An incomplete
        ; source package must change neither the game payload nor its stamp.
        missingSource := lib "\osmw_weather.dll"
        FileDelete(missingSource)
        stampBefore := FileRead(game "\osmw_payload.ini")
        PayloadTestAssert(!EnsureManagedPayload(game, source),
            "incomplete source package was accepted")
        PayloadTestAssert(FileRead(game "\osmw_payload.ini") = stampBefore,
            "incomplete package changed the stamp")
        PayloadTestAssert(
            PayloadTestRead(game "\osmw_weather.dll") = "osmw_weather.dll|v1",
            "incomplete package changed the deployed feature")
        PayloadTestWrite(missingSource, "osmw_weather.dll|v2")

        ; Prepare v2 and force refresh by removing the stamp. Hold a middle
        ; destination without sharing so earlier commits have to roll back.
        for name in ManagedPayloadFiles() {
            path := (name = "osmw_chat_launch.exe"
                || name = "osmw_login_launch.exe")
                ? source "\" name : lib "\" name
            if (name != "osmw_core_api.ini")
                PayloadTestWrite(path, name "|v2")
        }
        FileDelete(game "\osmw_payload.ini")
        lockedPath := game "\osmw_login.dll"
        locked := DllCall("CreateFile", "Str", lockedPath,
            "UInt", 0x80000000, "UInt", 0, "Ptr", 0, "UInt", 3,
            "UInt", 0x80, "Ptr", 0, "Ptr")
        PayloadTestAssert(locked != -1,
            "could not acquire exclusive fixture handle")
        try {
            PayloadTestAssert(!EnsureManagedPayload(game, source),
                "locked destination did not block the transaction")
        } finally {
            DllCall("CloseHandle", "Ptr", locked)
        }

        for name in ManagedPayloadFiles() {
            if (name = "osmw_core_api.ini")
                continue
            PayloadTestAssert(PayloadTestRead(game "\" name) = name "|v1",
                "rollback did not restore " name)
        }
        PayloadTestAssert(!FileExist(game "\osmw_payload.ini"),
            "failed transaction left a current stamp")

        PayloadTestAssert(EnsureManagedPayload(game, source),
            "retry after unlock failed")
        for name in ManagedPayloadFiles() {
            if (name = "osmw_core_api.ini")
                continue
            PayloadTestAssert(PayloadTestRead(game "\" name) = name "|v2",
                "retry did not deploy " name)
        }
        PayloadTestAssert(
            IniRead(game "\osmw_payload.ini", "Payload", "CoreAbi", "") = "1.0",
            "retry did not stamp ABI")

        leftovers := 0
        Loop Files game "\*.osmw-*", "F"
            leftovers++
        PayloadTestAssert(leftovers = 0,
            "transaction scratch files were left behind")
        PayloadTestWrite(resultPath,
            "PASS: managed payload transaction fixtures passed`n")
        exitCode := 0
    } catch as err {
        PayloadTestWrite(resultPath,
            "FAIL: " err.Message "`n" err.Stack "`n")
        exitCode := 1
    }

    try DirDelete(root, true)
    return exitCode
}

ExitApp(RunPayloadTransactionTest())

; Parse the same complete application graph as production so these calls are
; the actual release functions. ExitApp above runs before application startup.
#Include ..\..\..\main.ahk
