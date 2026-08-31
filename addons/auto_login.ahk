#Requires AutoHotkey v2.0

; Auto-login roster editor and the per-launch consent the DLL checks.
;
; The DLL (osmw_login.dll, injected by osmw_chat_launch.exe) does the in-process
; fill/submit/character-select. This addon never types into the client. Each
; launched process claims the next enabled row itself; Maps++ does not assign
; accounts. Passwords are Windows-user DPAPI blobs in login_roster.dat beside
; main.exe — never config.ini, never mapsplusplus.log, never a load message.

global _AUTOLOGIN_MAX_ROWS := 16
global _AUTOLOGIN_ID_BYTES := 40
global _AUTOLOGIN_ACC_BYTES := 32
global _AUTOLOGIN_CHAR_BYTES := 32
global _AUTOLOGIN_PW_CHARS := 63
global _AUTOLOGIN_MAGIC := "OSMWROS1"
global _AUTOLOGIN_DPAPI_DESC := "osmw_login"
global _AUTOLOGIN_UI_FORBIDDEN := 1
global _AUTOLOGIN_BLOB_MAX := 65536
; Must match LG_CONSENT_VAR in patches\login\src\dllmain.c.
global _AUTOLOGIN_CONSENT_VAR := "OSMW_AUTOLOGIN"
; Must match LG_ROW_VAR in patches\login\src\dllmain.c.
global _AUTOLOGIN_ROW_VAR := "OSMW_LOGIN_ROW"
; Row groups are a Maps++ launcher concept — the DLL has no use for them — so
; they live here rather than in login_roster.dat. Changing that file's format
; would make every already-deployed osmw_login.dll fail to read it and stop
; logging anyone in, which is far too high a price for a label.
global _AUTOLOGIN_GROUP_SECTION := "AutoLoginGroups"

global _AutoLogin_PanelMode := ""
global _AutoLogin_WebGui := 0
global _AutoLogin_NativeGui := 0
global _AutoLogin_NativeList := 0
global _AutoLogin_NativeAccount := 0
global _AutoLogin_NativePassword := 0
global _AutoLogin_NativeCharacter := 0
global _AutoLogin_NativeGroup := 0
global _AutoLogin_NativeEnabled := 0
global _AutoLogin_NativeStatus := 0
global _AutoLogin_NativeHint := 0
global _AutoLogin_Draft := []
global _AutoLogin_Selected := 0
global _AutoLogin_Refreshing := false

RegisterAddon(Map(
    "name",          "AutoLogin",
    "settingsLabel", "Auto-login",
    "OnTrayMenu",    _AutoLogin_OnTrayMenu
))

RegisterHotkeyAction(Map(
    "id", "autoLoginRoster",
    "label", "Open auto-login roster",
    "category", "Auto-login",
    "default", "",
    "addon", "AutoLogin",
    "handler", (*) => _AutoLogin_OpenEditor()
))

_AutoLogin_OnTrayMenu(trayGroups) {
    trayGroups["clients"].Add("Auto-login roster…`t" GetHotkeyDisplay("autoLoginRoster"),
        (*) => _AutoLogin_OpenEditor())
}

; Per-launch consent, carried to the DLL in the environment rather than in the
; ini.
;
; This used to write [Addons] AutoLogin. That key is persistent, so the value
; Maps++ wrote for its own launch was still there for a client the player
; started by hand afterwards — running osmw_chat_launch.exe on its own would log
; itself in, using whatever Maps++ last decided. The key could not simply be
; parked at 0 between launches either, because it doubles as this addon's own
; enable flag in Maps++'s [Addons] list.
;
; An environment variable answers the question the ini could not: not "is
; auto-login enabled" but "did the user ask for it for THIS process". Both
; launchers pass NULL for lpEnvironment in CreateProcess, so main.exe inherits
; it through the injection chain and no launcher change was needed. A client
; started from Explorer inherits Explorer's environment, has no such variable,
; and logs in normally.
;
; Called from BuildGameLaunchCommand via GetFuncByName so the launcher setting
; still reaches the DLL when the roster editor addon is disabled.
_AutoLogin_BeforeLaunch(gameDir) {
    global gLaunchAutoLogin, _AUTOLOGIN_CONSENT_VAR
    if (gameDir = "")
        return
    ; Set for a wanted launch, cleared otherwise, so a toggle turned off mid
    ; session cannot leave consent behind from an earlier launch.
    EnvSet(_AUTOLOGIN_CONSENT_VAR, gLaunchAutoLogin ? "1" : "")
}

; Which roster row the next client must take, set between the Run() calls of a
; batch rather than around it.
;
; Consent is one answer for the whole launch, so BeforeLaunch can set it once.
; The row is not: five clients that all inherited the same value would fight
; over one account. Run() snapshots the environment at CreateProcess, so setting
; it immediately before each Run() is what makes it per-client.
;
; An empty token means "no preference" — the client falls back to claiming the
; next free enabled row, exactly as it did before any of this existed.
_AutoLogin_BeforeLaunchClient(token) {
    global _AUTOLOGIN_ROW_VAR
    EnvSet(_AUTOLOGIN_ROW_VAR, String(token))
}

; Called after the launch batch's Run() calls, so the variables exist only for
; the moment it takes to spawn the clients that are meant to have them. The row
; variable is cleared here too: a plain next-free launch must never inherit a
; row id left behind by an earlier assigned batch.
_AutoLogin_AfterLaunch() {
    global _AUTOLOGIN_CONSENT_VAR, _AUTOLOGIN_ROW_VAR
    EnvSet(_AUTOLOGIN_CONSENT_VAR, "")
    EnvSet(_AUTOLOGIN_ROW_VAR, "")
}

; ── Paths ───────────────────────────────────────────────────────

_AutoLogin_RosterPath() {
    gameDir := ResolveGameDirectory()
    if (gameDir = "")
        return ""
    return gameDir "\login_roster.dat"
}

; ── Row groups ──────────────────────────────────────────────────

; config.ini [AutoLoginGroups] <rowId>=<name>. Keyed by the roster row's opaque
; id rather than by its account, so a group survives reordering, renaming an
; account, and two rows sharing one account — which is the normal case, since
; five characters on one account are five rows.
_AutoLogin_ReadGroups() {
    global CONFIG_INI, _AUTOLOGIN_GROUP_SECTION
    groups := Map()
    raw := ""
    try raw := IniRead(CONFIG_INI, _AUTOLOGIN_GROUP_SECTION)
    catch
        return groups
    for line in StrSplit(raw, "`n", " `t`r") {
        eq := InStr(line, "=")
        if !eq
            continue
        rowId := Trim(SubStr(line, 1, eq - 1))
        name := Trim(SubStr(line, eq + 1))
        if (rowId != "" && name != "")
            groups[rowId] := name
    }
    return groups
}

; Rewrites the section from the saved rows, so a removed row cannot leave its
; group behind to be inherited by a later row reusing the id.
_AutoLogin_SaveGroups(rows) {
    global CONFIG_INI, _AUTOLOGIN_GROUP_SECTION
    ; Group names are user text and this deletes before it writes — without
    ; this the ini becomes ANSI and mangles anything outside the codepage.
    EnsureIniUtf16(CONFIG_INI)
    try IniDelete(CONFIG_INI, _AUTOLOGIN_GROUP_SECTION)
    for row in rows {
        name := row.Has("group") ? Trim(String(row["group"])) : ""
        if (name = "")
            continue
        try IniWrite(name, CONFIG_INI, _AUTOLOGIN_GROUP_SECTION, row["id"])
    }
}

; Distinct group names, in roster row order — the order the user arranged, not
; alphabetical, so the menu matches the editor.
_AutoLogin_GroupNames(rows) {
    seen := Map()
    names := []
    for row in rows {
        name := row.Has("group") ? Trim(String(row["group"])) : ""
        if (name = "" || seen.Has(name))
            continue
        seen[name] := true
        names.Push(name)
    }
    return names
}

; ── Launch plans ────────────────────────────────────────────────

; Turns an accounts spec into one roster row id per client. Core calls this
; through GetFuncByName and treats the result as opaque tokens; the meaning of
; a token — a row id the DLL will claim — lives entirely on this side.
_AutoLogin_ResolvePlan(spec, count) {
    global LAUNCH_ACCOUNTS_GROUP, LAUNCH_ACCOUNTS_START, LAUNCH_ACCOUNTS_ONLY
    spec := Trim(String(spec))
    if (spec = "")
        return []
    loaded := _AutoLogin_ReadFile()
    if !loaded.ok
        return []
    rows := loaded.rows
    plan := []

    if (InStr(spec, LAUNCH_ACCOUNTS_GROUP) = 1) {
        want := SubStr(spec, StrLen(LAUNCH_ACCOUNTS_GROUP) + 1)
        for row in rows {
            if (row["group"] = want)
                plan.Push(row["id"])
        }
        return plan
    }

    if (InStr(spec, LAUNCH_ACCOUNTS_ONLY) = 1) {
        want := SubStr(spec, StrLen(LAUNCH_ACCOUNTS_ONLY) + 1)
        for row in rows {
            if (row["id"] = want)
                return [row["id"]]
        }
        return plan
    }

    if (InStr(spec, LAUNCH_ACCOUNTS_START) = 1) {
        want := SubStr(spec, StrLen(LAUNCH_ACCOUNTS_START) + 1)
        start := 0
        for i, row in rows {
            if (row["id"] = want) {
                start := i
                break
            }
        }
        if !start
            return plan
        wanted := (count >= 1) ? count : rows.Length
        Loop wanted {
            idx := start + A_Index - 1
            if (idx > rows.Length)
                break
            plan.Push(rows[idx]["id"])
        }
        return plan
    }

    ; An unrecognised spec means "next free" rather than an error: a group the
    ; user later deleted should not stop the launch, it should just stop
    ; assigning accounts.
    return plan
}

; What both Settings and the tray menu offer, from one place so they cannot
; drift. Rows are in roster file order throughout.
_AutoLogin_LaunchChoices() {
    global LAUNCH_ACCOUNTS_GROUP, LAUNCH_ACCOUNTS_START, LAUNCH_ACCOUNTS_ONLY
    out := Map("groups", [], "rows", [])
    loaded := _AutoLogin_ReadFile()
    if !loaded.ok
        return out
    rows := loaded.rows
    for name in _AutoLogin_GroupNames(rows) {
        n := 0
        for row in rows {
            if (row["group"] = name)
                n++
        }
        out["groups"].Push(Map("name", name, "count", n, "spec", LAUNCH_ACCOUNTS_GROUP name))
    }
    for i, row in rows {
        out["rows"].Push(Map(
            "id", row["id"],
            "index", i,
            "account", row["account"],
            "character", row["character"],
            "group", row["group"],
            "startSpec", LAUNCH_ACCOUNTS_START row["id"],
            "onlySpec", LAUNCH_ACCOUNTS_ONLY row["id"]))
    }
    return out
}

; ── Client Launch menu ──────────────────────────────────────────

; Menu.Add treats a repeated label as "modify that item", so two identical
; labels would silently collapse into one entry. Two rows on one account with
; the same character is a user mistake rather than a normal state, but it must
; not quietly lose a menu item.
_AutoLogin_UniqueLabel(used, label) {
    if !used.Has(label) {
        used[label] := 1
        return label
    }
    used[label] += 1
    return label " (" used[label] ")"
}

; The Primary/Secondary half of Client Launch: groups, then batches starting at
; a chosen account, then single accounts. Returns "" when there is nothing to
; offer, so the caller can leave the whole item out rather than show an empty
; submenu.
_AutoLogin_BuildAccountMenu(monitorWhich) {
    global gMultiClientCount
    loaded := _AutoLogin_ReadFile()
    if (!loaded.ok || !loaded.rows.Length)
        return ""
    rows := loaded.rows
    m := Menu()
    used := Map()

    groups := _AutoLogin_GroupNames(rows)
    for name in groups {
        n := 0
        for row in rows {
            if (row["group"] = name)
                n++
        }
        m.Add(_AutoLogin_UniqueLabel(used, "Group: " name " (" n ")"),
            _AutoLogin_LaunchSpec.Bind(monitorWhich, "group:" name))
    }
    if groups.Length
        m.Add()

    ; How far a "start at" batch reaches, so the label can say so rather than
    ; leaving the user to count rows in the editor.
    profile := ResolveLaunchProfile(monitorWhich)
    span := (profile["count"] >= 1) ? profile["count"] : gMultiClientCount
    for i, row in rows {
        last := i + span - 1
        if (last > rows.Length)
            last := rows.Length
        m.Add(_AutoLogin_UniqueLabel(used,
            "Start at: " row["account"] " (" i (last > i ? "-" last : "") ")"),
            _AutoLogin_LaunchSpec.Bind(monitorWhich, "start:" row["id"]))
    }
    m.Add()

    for row in rows {
        m.Add(_AutoLogin_UniqueLabel(used, "Only: " row["account"] " (" row["character"] ")"),
            _AutoLogin_LaunchSpec.Bind(monitorWhich, "only:" row["id"]))
    }
    return m
}

; Reached from the tray menu, which arrives via PostMessage rather than a
; WebView2 message, so raising a dialog here needs no DeferFromWebMessage.
_AutoLogin_LaunchSpec(monitorWhich, spec, *) {
    global gMultiClientCount
    profile := ResolveLaunchProfile(monitorWhich)
    span := (profile["count"] >= 1) ? profile["count"] : gMultiClientCount
    plan := _AutoLogin_ResolvePlan(spec, span)
    if !plan.Length {
        ShowMessage("Nothing in the roster matches that choice any more."
            . " Open Clients & Windows → Auto-login roster… and check the accounts and groups.",
            "Client Launch", Map("severity", "warn"))
        return
    }
    LaunchConfiguredClients(monitorWhich, 0, plan)
}

; ── DPAPI / file ────────────────────────────────────────────────

_AutoLogin_PtrOff() {
    return (A_PtrSize = 8) ? 8 : 4
}

_AutoLogin_ClipField(text, maxChars) {
    s := String(text)
    if (StrLen(s) <= maxChars)
        return s
    return SubStr(s, 1, maxChars)
}

_AutoLogin_EncodeFixed(text, byteCount) {
    buf := Buffer(byteCount, 0)
    need := StrPut(String(text), "CP0")
    tmp := Buffer(need)
    StrPut(String(text), tmp, "CP0")
    copyLen := need - 1
    if (copyLen > byteCount - 1)
        copyLen := byteCount - 1
    if (copyLen > 0)
        DllCall("RtlMoveMemory", "Ptr", buf.Ptr, "Ptr", tmp.Ptr, "UPtr", copyLen)
    return buf
}

_AutoLogin_ReadFixed(f, byteCount) {
    buf := Buffer(byteCount, 0)
    got := f.RawRead(buf)
    if (got < byteCount)
        return {ok: false, text: ""}
    return {ok: true, text: StrGet(buf, byteCount, "CP0")}
}

_AutoLogin_ProtectPassword(plain) {
    global _AUTOLOGIN_DPAPI_DESC, _AUTOLOGIN_UI_FORBIDDEN
    pw := String(plain)
    if (pw = "")
        return {ok: false, blob: 0, reason: "Password cannot be empty."}
    need := StrPut(pw, "CP0")
    if (need < 2)
        return {ok: false, blob: 0, reason: "Password cannot be empty."}
    byteCount := need - 1
    inBuf := Buffer(need)
    StrPut(pw, inBuf, "CP0")
    ptrOff := _AutoLogin_PtrOff()
    blobIn := Buffer(ptrOff + A_PtrSize, 0)
    blobOut := Buffer(ptrOff + A_PtrSize, 0)
    NumPut("UInt", byteCount, blobIn, 0)
    NumPut("Ptr", inBuf.Ptr, blobIn, ptrOff)
    ok := DllCall("crypt32\CryptProtectData",
        "Ptr", blobIn,
        "WStr", _AUTOLOGIN_DPAPI_DESC,
        "Ptr", 0, "Ptr", 0, "Ptr", 0,
        "UInt", _AUTOLOGIN_UI_FORBIDDEN,
        "Ptr", blobOut,
        "Int")
    if !ok
        return {ok: false, blob: 0, reason: "Could not protect the password."}
    outLen := NumGet(blobOut, 0, "UInt")
    outPtr := NumGet(blobOut, ptrOff, "Ptr")
    result := Buffer(outLen, 0)
    if (outLen && outPtr)
        DllCall("RtlMoveMemory", "Ptr", result.Ptr, "Ptr", outPtr, "UPtr", outLen)
    if outPtr
        DllCall("kernel32\LocalFree", "Ptr", outPtr)
    return {ok: true, blob: result, reason: ""}
}

; Load never decrypts. blobs maps id → DPAPI Buffer for keep-on-save.
_AutoLogin_ReadFile() {
    global _AUTOLOGIN_MAGIC, _AUTOLOGIN_MAX_ROWS, _AUTOLOGIN_ID_BYTES
    global _AUTOLOGIN_ACC_BYTES, _AUTOLOGIN_CHAR_BYTES, _AUTOLOGIN_BLOB_MAX
    path := _AutoLogin_RosterPath()
    rows := []
    blobs := Map()
    groups := _AutoLogin_ReadGroups()
    if (path = "" || !FileExist(path))
        return {ok: true, rows: rows, blobs: blobs, reason: ""}

    f := ""
    try f := FileOpen(path, "r")
    catch as err
        return {ok: false, rows: rows, blobs: blobs, reason: "Could not open the roster file. " err.Message}
    if !IsObject(f)
        return {ok: false, rows: rows, blobs: blobs, reason: "Could not open the roster file."}

    magicBuf := Buffer(8, 0)
    gotMagic := f.RawRead(magicBuf)
    if (gotMagic < 8 || StrGet(magicBuf, 8, "CP0") != _AUTOLOGIN_MAGIC) {
        f.Close()
        return {ok: false, rows: rows, blobs: blobs, reason: "Roster file is not a recognised login_roster.dat."}
    }
    rowCount := f.ReadUInt()
    if !IsInteger(rowCount) || rowCount < 0 || rowCount > _AUTOLOGIN_MAX_ROWS {
        f.Close()
        return {ok: false, rows: rows, blobs: blobs, reason: "Roster file is corrupt."}
    }

    Loop rowCount {
        idRes := _AutoLogin_ReadFixed(f, _AUTOLOGIN_ID_BYTES)
        accRes := _AutoLogin_ReadFixed(f, _AUTOLOGIN_ACC_BYTES)
        charRes := _AutoLogin_ReadFixed(f, _AUTOLOGIN_CHAR_BYTES)
        if !(idRes.ok && accRes.ok && charRes.ok) {
            f.Close()
            return {ok: false, rows: rows, blobs: blobs, reason: "Roster file is truncated."}
        }
        enabled := f.ReadUInt()
        blobLen := f.ReadUInt()
        if !IsInteger(enabled) || !IsInteger(blobLen) || blobLen < 0 || blobLen > _AUTOLOGIN_BLOB_MAX {
            f.Close()
            return {ok: false, rows: rows, blobs: blobs, reason: "Roster file is corrupt."}
        }
        blob := Buffer(blobLen, 0)
        if (blobLen) {
            gotBlob := f.RawRead(blob)
            if (gotBlob < blobLen) {
                f.Close()
                return {ok: false, rows: rows, blobs: blobs, reason: "Roster file is truncated."}
            }
        }
        rowId := idRes.text
        if (rowId = "")
            continue
        blobs[rowId] := blob
        rows.Push(Map(
            "id", rowId,
            "account", accRes.text,
            "character", (charRes.text = "") ? "#1" : charRes.text,
            ; Groups live in config.ini, not in this file — see the header of
            ; _AutoLogin_ReadGroups for why the .dat format stays put.
            "group", groups.Has(rowId) ? groups[rowId] : "",
            "enabled", enabled ? true : false,
            "hasPassword", blobLen > 0
        ))
    }
    f.Close()
    return {ok: true, rows: rows, blobs: blobs, reason: ""}
}

_AutoLogin_NewId() {
    static seq := 0
    seq++
    return Format("{:x}-{:x}-{:x}", A_TickCount, DllCall("GetCurrentProcessId"), seq)
}

; The plaintext a not-yet-saved row is carrying, for a duplicate of a duplicate.
; Follows passwordFrom transitively with a visited guard, since the payload is
; user-supplied and a cycle would otherwise spin forever.
_AutoLogin_PendingPassword(incoming, wantId) {
    seen := Map()
    while (wantId != "" && !seen.Has(wantId)) {
        seen[wantId] := true
        next := ""
        for row in incoming {
            if !IsObject(row)
                continue
            rowId := row.Has("id") ? Trim(String(row["id"])) : ""
            if (rowId != wantId)
                continue
            if (row.Has("password") && String(row["password"]) != "")
                return String(row["password"])
            next := row.Has("passwordFrom") ? Trim(String(row["passwordFrom"])) : ""
            break
        }
        wantId := next
    }
    return ""
}

_AutoLogin_SaveIncoming(incoming) {
    global _AUTOLOGIN_MAX_ROWS, _AUTOLOGIN_PW_CHARS
    global _AUTOLOGIN_ID_BYTES, _AUTOLOGIN_ACC_BYTES, _AUTOLOGIN_CHAR_BYTES, _AUTOLOGIN_MAGIC
    if !IsObject(incoming) || Type(incoming) != "Array"
        return {ok: false, reason: "No roster rows were received."}
    if (incoming.Length > _AUTOLOGIN_MAX_ROWS)
        return {ok: false, reason: "At most " _AUTOLOGIN_MAX_ROWS " rows can be stored."}

    path := _AutoLogin_RosterPath()
    if (path = "")
        return {ok: false, reason: "Set the game path in Settings → Launcher first. The roster is stored next to main.exe."}

    existing := _AutoLogin_ReadFile()
    if !existing.ok {
        needsKeep := false
        for row in incoming {
            pwText := ""
            if IsObject(row) && row.Has("password")
                pwText := String(row["password"])
            if (pwText = "")
                needsKeep := true
        }
        if needsKeep
            return {ok: false, reason: existing.reason}
        existing := {ok: true, rows: [], blobs: Map(), reason: ""}
    }

    outRows := []
    for row in incoming {
        if !IsObject(row)
            return {ok: false, reason: "A roster row was unreadable."}
        account := _AutoLogin_ClipField(row.Has("account") ? Trim(String(row["account"])) : "", 31)
        if (account = "")
            return {ok: false, reason: "Every row needs an account."}
        character := _AutoLogin_ClipField(row.Has("character") ? Trim(String(row["character"])) : "", 31)
        if (character = "")
            character := "#1"
        rowId := _AutoLogin_ClipField(row.Has("id") ? Trim(String(row["id"])) : "", 39)
        if (rowId = "")
            rowId := _AutoLogin_NewId()
        enabled := true
        if row.Has("enabled")
            enabled := !!(row["enabled"])
        group := _AutoLogin_ClipField(row.Has("group") ? Trim(String(row["group"])) : "", 31)
        pwText := ""
        if row.Has("password")
            pwText := String(row["password"])
        ; Duplicating a row carries this instead of a password: five characters
        ; on one account are five rows, and typing the same password five times
        ; is five chances to typo something that can never be read back.
        pwFrom := row.Has("passwordFrom") ? Trim(String(row["passwordFrom"])) : ""
        blob := 0
        if (pwText != "") {
            if (StrLen(pwText) > _AUTOLOGIN_PW_CHARS)
                return {ok: false, reason: "Passwords cannot be longer than " _AUTOLOGIN_PW_CHARS " characters."}
            protected := _AutoLogin_ProtectPassword(pwText)
            if !protected.ok
                return {ok: false, reason: protected.reason}
            blob := protected.blob
        } else if existing.blobs.Has(rowId) {
            blob := existing.blobs[rowId]
        } else if (pwFrom != "" && existing.blobs.Has(pwFrom)) {
            blob := existing.blobs[pwFrom]
        } else if (pwFrom != "" && (borrowed := _AutoLogin_PendingPassword(incoming, pwFrom)) != "") {
            ; The source row is itself new in this same save — duplicating a row
            ; that has not been written yet has to work too.
            if (StrLen(borrowed) > _AUTOLOGIN_PW_CHARS)
                return {ok: false, reason: "Passwords cannot be longer than " _AUTOLOGIN_PW_CHARS " characters."}
            protected := _AutoLogin_ProtectPassword(borrowed)
            if !protected.ok
                return {ok: false, reason: protected.reason}
            blob := protected.blob
        } else {
            return {ok: false, reason: "New rows need a password."}
        }
        outRows.Push(Map(
            "id", rowId,
            "account", account,
            "character", character,
            "group", group,
            "enabled", enabled,
            "blob", blob
        ))
    }

    f := ""
    try f := FileOpen(path, "w")
    catch as err
        return {ok: false, reason: "Could not write the roster file. " err.Message}
    if !IsObject(f)
        return {ok: false, reason: "Could not write the roster file."}

    try {
        ; Magic is exactly 8 ASCII bytes with no reserved NUL.
        magicRaw := Buffer(8, 0)
        need := StrPut(_AUTOLOGIN_MAGIC, "CP0")
        tmp := Buffer(need)
        StrPut(_AUTOLOGIN_MAGIC, tmp, "CP0")
        DllCall("RtlMoveMemory", "Ptr", magicRaw.Ptr, "Ptr", tmp.Ptr, "UPtr", 8)
        f.RawWrite(magicRaw)
        f.WriteUInt(outRows.Length)
        for saved in outRows {
            f.RawWrite(_AutoLogin_EncodeFixed(saved["id"], _AUTOLOGIN_ID_BYTES))
            f.RawWrite(_AutoLogin_EncodeFixed(saved["account"], _AUTOLOGIN_ACC_BYTES))
            f.RawWrite(_AutoLogin_EncodeFixed(saved["character"], _AUTOLOGIN_CHAR_BYTES))
            f.WriteUInt(saved["enabled"] ? 1 : 0)
            blob := saved["blob"]
            blobLen := IsObject(blob) ? blob.Size : 0
            f.WriteUInt(blobLen)
            if (blobLen)
                f.RawWrite(blob)
        }
    } catch as err {
        f.Close()
        return {ok: false, reason: "Could not write the roster file. " err.Message}
    }
    f.Close()
    ; Only after the roster itself is on disk, so a failed write cannot leave
    ; groups pointing at rows that were never saved.
    _AutoLogin_SaveGroups(outRows)
    LogInfo("AutoLogin", "Saved " outRows.Length " roster row(s)")
    ; Client Launch lists these accounts and groups, so it has to be rebuilt.
    ; Deferred, because this can be reached from a WebView2 message handler.
    SetTimer(RebuildTrayMenu, -1)
    return {ok: true, reason: ""}
}

_AutoLogin_RowsToJson(rows) {
    json := ""
    for row in rows {
        json .= (json = "" ? "" : ",")
            . '{"id":' _JSON_Str(row["id"])
            . ',"account":' _JSON_Str(row["account"])
            . ',"character":' _JSON_Str(row["character"])
            . ',"group":' _JSON_Str(row.Has("group") ? row["group"] : "")
            . ',"enabled":' (row["enabled"] ? "true" : "false")
            . ',"hasPassword":' (row["hasPassword"] ? "true" : "false") '}'
    }
    return json
}

; ── Editor ──────────────────────────────────────────────────────

_AutoLogin_CanUseWebView() {
    return FileExist(A_ScriptDir "\Lib\WebViewToo.ahk")
        && FileExist(A_ScriptDir "\ui\login_roster\index.html")
}

_AutoLogin_OpenEditor() {
    global _AutoLogin_PanelMode, _AutoLogin_WebGui, _AutoLogin_NativeGui
    if (ResolveGameDirectory() = "") {
        ShowMessage(
            "Set the game path in Settings → Launcher first. The roster file is stored next to main.exe.",
            "Auto-login")
        return
    }
    if (_AutoLogin_PanelMode = "web" && IsObject(_AutoLogin_WebGui) && _AutoLogin_WebGui.Hwnd) {
        try WinActivate("ahk_id " _AutoLogin_WebGui.Hwnd)
        return
    }
    if (_AutoLogin_PanelMode = "native" && IsObject(_AutoLogin_NativeGui) && _AutoLogin_NativeGui.Hwnd) {
        try WinActivate("ahk_id " _AutoLogin_NativeGui.Hwnd)
        return
    }
    if (IsNativeInterface() || !_AutoLogin_CanUseWebView())
        _AutoLogin_ShowNative()
    else
        _AutoLogin_ShowWeb()
}

_AutoLogin_DialogOpts(severity := "warn") {
    global _AutoLogin_PanelMode, _AutoLogin_WebGui, _AutoLogin_NativeGui
    o := Map("severity", severity)
    g := 0
    if (_AutoLogin_PanelMode = "web")
        g := _AutoLogin_WebGui
    else if (_AutoLogin_PanelMode = "native")
        g := _AutoLogin_NativeGui
    if (IsObject(g) && g.Hwnd) {
        try g.Opt("+OwnDialogs")
        o["owner"] := g.Hwnd
    }
    return o
}

; ── WebView2 ────────────────────────────────────────────────────

_AutoLogin_ShowWeb() {
    global _AutoLogin_WebGui, _AutoLogin_PanelMode
    dllDir := (A_PtrSize = 8) ? "64bit" : "32bit"
    settings := {DllPath: A_ScriptDir "\Lib\" dllDir "\WebView2Loader.dll",
        DefaultWidth: 560, DefaultHeight: 780}
    g := WebViewGui("-Caption +AlwaysOnTop +Resize", "Maps++ — Auto-login", , settings)
    g.OnEvent("Close", (*) => _AutoLogin_WebRequestClose())
    g.WebMessageReceived(WebMsgHandler(_AutoLogin_OnWebMessage))
    g.DOMContentLoaded((*) => SetTimer(_AutoLogin_SendWebState, -50))
    g.Navigate(UiPageUrl("ui/login_roster/index.html"))
    _AutoLogin_WebGui := g
    _AutoLogin_PanelMode := "web"
    g.Show("w560 h780 Center")
}

_AutoLogin_WebPost(json) {
    global _AutoLogin_WebGui
    if IsObject(_AutoLogin_WebGui)
        try _AutoLogin_WebGui.PostWebMessageAsJson(json)
}

_AutoLogin_SendWebState() {
    loaded := _AutoLogin_ReadFile()
    if !loaded.ok {
        _AutoLogin_WebPost('{"type":"login-roster","rows":[]}')
        _AutoLogin_WebPost('{"type":"save-result","ok":false,"message":' _JSON_Str(loaded.reason) '}')
        return
    }
    _AutoLogin_WebPost('{"type":"login-roster","rows":[' _AutoLogin_RowsToJson(loaded.rows) ']}')
}

_AutoLogin_OnWebMessage(wv, args) {
    msgStr := ""
    try msgStr := args.TryGetWebMessageAsString()
    if (msgStr = "")
        try msgStr := args.WebMessageAsJson
    if (msgStr = "")
        return
    msg := _JSON_Parse(msgStr)
    if !IsObject(msg) || !msg.Has("type")
        return
    switch msg["type"] {
        case "login-roster-request":
            _AutoLogin_SendWebState()
        case "login-roster-save":
            _AutoLogin_WebSave(msg)
        case "close":
            DeferFromWebMessage(_AutoLogin_WebRequestClose)
    }
}

_AutoLogin_WebSave(msg) {
    incoming := msg.Has("rows") ? msg["rows"] : 0
    result := _AutoLogin_SaveIncoming(incoming)
    if !result.ok {
        _AutoLogin_WebPost('{"type":"save-result","ok":false,"message":' _JSON_Str(result.reason) '}')
        return
    }
    _AutoLogin_WebPost('{"type":"save-result","ok":true,"message":"Saved. Next launch claims these rows."}')
    _AutoLogin_SendWebState()
}

_AutoLogin_WebRequestClose() {
    global _AutoLogin_WebGui, _AutoLogin_PanelMode
    if !(IsObject(_AutoLogin_WebGui) && _AutoLogin_WebGui.Hwnd)
        return 1
    if IsObject(_AutoLogin_WebGui)
        try _AutoLogin_WebGui.Destroy()
    _AutoLogin_WebGui := 0
    _AutoLogin_PanelMode := ""
    return 1
}

; ── Native ──────────────────────────────────────────────────────

_AutoLogin_ShowNative() {
    global _AutoLogin_PanelMode, _AutoLogin_Draft, _AutoLogin_Selected, _AutoLogin_NativeGui
    loaded := _AutoLogin_ReadFile()
    if !loaded.ok {
        ShowMessage(loaded.reason, "Auto-login", _AutoLogin_DialogOpts("warn"))
        _AutoLogin_Draft := []
    } else {
        _AutoLogin_Draft := loaded.rows
    }
    _AutoLogin_Selected := _AutoLogin_Draft.Length ? 1 : 0
    _AutoLogin_NativeEnsureGui()
    _AutoLogin_PanelMode := "native"
    _AutoLogin_NativeRefreshList()
    _AutoLogin_NativeFillFields()
    _AutoLogin_NativeGui.Show("w520 h620 Center")
}

_AutoLogin_NativeEnsureGui() {
    global _AutoLogin_NativeGui, _AutoLogin_NativeList
    global _AutoLogin_NativeAccount, _AutoLogin_NativePassword
    global _AutoLogin_NativeCharacter, _AutoLogin_NativeGroup, _AutoLogin_NativeEnabled
    global _AutoLogin_NativeStatus, _AutoLogin_NativeHint
    if (IsObject(_AutoLogin_NativeGui) && _AutoLogin_NativeGui.Hwnd)
        return

    g := Gui("+AlwaysOnTop -MaximizeBox", "Maps++ — Auto-login")
    g.MarginX := 12
    g.MarginY := 12
    g.SetFont("s9", "Segoe UI")
    g.OnEvent("Close", (*) => _AutoLogin_NativeClose())
    g.OnEvent("Escape", (*) => _AutoLogin_NativeClose())

    g.Add("Text", "x12 y12 w496",
        "Each launched client claims the next enabled row, unless a launch profile or Client Launch names the accounts. Passwords stay on this Windows user. Turn auto-login off under Settings → Launcher for the stock login.")
    _AutoLogin_NativeList := g.Add("ListView", "x12 y+10 w496 r10 Grid -Multi NoSortHdr",
        ["Account", "Character", "Group", "On"])
    _AutoLogin_NativeList.OnEvent("ItemSelect", _AutoLogin_NativeOnSelect)
    g.Add("Button", "x12 y+8 w72", "Up").OnEvent("Click", (*) => _AutoLogin_NativeMove(-1))
    g.Add("Button", "x+8 yp w72", "Down").OnEvent("Click", (*) => _AutoLogin_NativeMove(1))
    g.Add("Button", "x+8 yp w88", "Duplicate").OnEvent("Click", (*) => _AutoLogin_NativeDuplicate())
    g.Add("Button", "x+8 yp w72", "Remove").OnEvent("Click", (*) => _AutoLogin_NativeRemove())

    g.Add("Text", "x12 y+16 w80", "Account")
    _AutoLogin_NativeAccount := g.Add("Edit", "x100 yp-3 w408 Limit31")
    g.Add("Text", "x12 y+10 w80", "Password")
    _AutoLogin_NativePassword := g.Add("Edit", "x100 yp-3 w408 Password Limit63")
    _AutoLogin_NativeHint := g.Add("Text", "x100 y+4 w408 c666666",
        "Write-only. Blank keeps the stored password.")
    g.Add("Text", "x12 y+10 w80", "Character")
    _AutoLogin_NativeCharacter := g.Add("Edit", "x100 yp-3 w408 Limit31")
    g.Add("Text", "x12 y+10 w80", "Group")
    _AutoLogin_NativeGroup := g.Add("Edit", "x100 yp-3 w408 Limit31")
    g.Add("Text", "x100 y+4 w408 c666666",
        "Optional. Launch a whole group from Client Launch or a launch profile.")
    _AutoLogin_NativeEnabled := g.Add("CheckBox", "x100 y+10 w408 Checked",
        "Enabled — claim this row on launch")

    g.Add("Button", "x12 y+14 w120", "Add").OnEvent("Click", (*) => _AutoLogin_NativeAdd())
    g.Add("Button", "x+8 yp w160", "Apply to selected")
        .OnEvent("Click", (*) => _AutoLogin_NativeApply())

    _AutoLogin_NativeStatus := g.Add("Text", "x12 y+16 w496", "Changes are written when you save.")
    g.Add("Button", "x252 y+12 w120", "Close").OnEvent("Click", (*) => _AutoLogin_NativeClose())
    g.Add("Button", "x+8 yp w120 Default", "Save").OnEvent("Click", (*) => _AutoLogin_NativeSave())

    _AutoLogin_NativeGui := g
}

_AutoLogin_NativeRefreshList() {
    global _AutoLogin_NativeList, _AutoLogin_Draft, _AutoLogin_Selected, _AutoLogin_Refreshing
    if !IsObject(_AutoLogin_NativeList)
        return
    _AutoLogin_Refreshing := true
    _AutoLogin_NativeList.Delete()
    for row in _AutoLogin_Draft {
        _AutoLogin_NativeList.Add(, row["account"], row["character"],
            row.Has("group") ? row["group"] : "",
            row["enabled"] ? "yes" : "no")
    }
    if (_AutoLogin_Selected >= 1 && _AutoLogin_Selected <= _AutoLogin_Draft.Length)
        _AutoLogin_NativeList.Modify(_AutoLogin_Selected, "Select Focus Vis")
    _AutoLogin_Refreshing := false
}

_AutoLogin_NativeFillFields() {
    global _AutoLogin_Draft, _AutoLogin_Selected
    global _AutoLogin_NativeAccount, _AutoLogin_NativePassword
    global _AutoLogin_NativeCharacter, _AutoLogin_NativeGroup
    global _AutoLogin_NativeEnabled, _AutoLogin_NativeHint
    if (_AutoLogin_Selected < 1 || _AutoLogin_Selected > _AutoLogin_Draft.Length) {
        _AutoLogin_NativeAccount.Value := ""
        _AutoLogin_NativePassword.Value := ""
        _AutoLogin_NativeCharacter.Value := ""
        _AutoLogin_NativeGroup.Value := ""
        _AutoLogin_NativeEnabled.Value := 1
        _AutoLogin_NativeHint.Text := "Write-only. Blank keeps the stored password."
        return
    }
    row := _AutoLogin_Draft[_AutoLogin_Selected]
    _AutoLogin_NativeAccount.Value := row["account"]
    _AutoLogin_NativePassword.Value := ""
    _AutoLogin_NativeCharacter.Value := row["character"]
    _AutoLogin_NativeGroup.Value := row.Has("group") ? row["group"] : ""
    _AutoLogin_NativeEnabled.Value := row["enabled"] ? 1 : 0
    _AutoLogin_NativeHint.Text := (row["hasPassword"] || row.Has("passwordFrom"))
        ? "Stored. Leave blank to keep it, or type a new password."
        : "No password stored yet — type one before save."
}

_AutoLogin_NativeOnSelect(*) {
    global _AutoLogin_Refreshing, _AutoLogin_NativeList, _AutoLogin_Selected
    if _AutoLogin_Refreshing
        return
    rowNum := _AutoLogin_NativeList.GetNext()
    _AutoLogin_Selected := rowNum
    _AutoLogin_NativeFillFields()
}

_AutoLogin_NativeReadFields() {
    global _AutoLogin_NativeAccount, _AutoLogin_NativePassword
    global _AutoLogin_NativeCharacter, _AutoLogin_NativeGroup, _AutoLogin_NativeEnabled
    return Map(
        "account", Trim(_AutoLogin_NativeAccount.Value),
        "password", _AutoLogin_NativePassword.Value,
        "character", Trim(_AutoLogin_NativeCharacter.Value),
        "group", Trim(_AutoLogin_NativeGroup.Value),
        "enabled", _AutoLogin_NativeEnabled.Value ? true : false
    )
}

_AutoLogin_NativeSetStatus(text) {
    global _AutoLogin_NativeStatus
    if IsObject(_AutoLogin_NativeStatus)
        _AutoLogin_NativeStatus.Text := text
}

_AutoLogin_NativeApply() {
    global _AutoLogin_Draft, _AutoLogin_Selected, _AutoLogin_NativePassword
    if (_AutoLogin_Selected < 1 || _AutoLogin_Selected > _AutoLogin_Draft.Length) {
        _AutoLogin_NativeSetStatus("Select a row first, or click Add.")
        return false
    }
    fields := _AutoLogin_NativeReadFields()
    if (fields["account"] = "") {
        _AutoLogin_NativeSetStatus("Account cannot be empty.")
        return false
    }
    row := _AutoLogin_Draft[_AutoLogin_Selected]
    row["account"] := fields["account"]
    row["character"] := (fields["character"] = "") ? "#1" : fields["character"]
    row["group"] := fields["group"]
    row["enabled"] := fields["enabled"]
    if (fields["password"] != "") {
        row["password"] := fields["password"]
        row["hasPassword"] := true
        _AutoLogin_NativePassword.Value := ""
    }
    _AutoLogin_NativeRefreshList()
    _AutoLogin_NativeSetStatus("Row updated. Save to write the file.")
    return true
}

_AutoLogin_NativeAdd() {
    global _AutoLogin_Draft, _AutoLogin_Selected, _AutoLogin_NativePassword
    global _AUTOLOGIN_MAX_ROWS
    if (_AutoLogin_Draft.Length >= _AUTOLOGIN_MAX_ROWS) {
        _AutoLogin_NativeSetStatus("At most " _AUTOLOGIN_MAX_ROWS " rows can be stored.")
        return
    }
    fields := _AutoLogin_NativeReadFields()
    if (fields["account"] = "" || fields["password"] = "") {
        _AutoLogin_NativeSetStatus("New rows need an account and a password.")
        return
    }
    _AutoLogin_Draft.Push(Map(
        "id", _AutoLogin_NewId(),
        "account", fields["account"],
        "character", (fields["character"] = "") ? "#1" : fields["character"],
        "group", fields["group"],
        "enabled", fields["enabled"],
        "hasPassword", true,
        "password", fields["password"]
    ))
    _AutoLogin_NativePassword.Value := ""
    _AutoLogin_Selected := _AutoLogin_Draft.Length
    _AutoLogin_NativeRefreshList()
    _AutoLogin_NativeFillFields()
    _AutoLogin_NativeSetStatus("Row added. Save to write the file.")
}

; The usual reason for a second row is the same account's next character, so the
; slot advances by one; anything that is not a #N slot has no "next" and is
; copied verbatim. The password is never read back — the new row names the row
; to copy the blob from and the save path does the copying.
_AutoLogin_NativeDuplicate() {
    global _AutoLogin_Draft, _AutoLogin_Selected, _AUTOLOGIN_MAX_ROWS
    if (_AutoLogin_Selected < 1 || _AutoLogin_Selected > _AutoLogin_Draft.Length) {
        _AutoLogin_NativeSetStatus("Select a row to duplicate first.")
        return
    }
    if (_AutoLogin_Draft.Length >= _AUTOLOGIN_MAX_ROWS) {
        _AutoLogin_NativeSetStatus("At most " _AUTOLOGIN_MAX_ROWS " rows can be stored.")
        return
    }
    src := _AutoLogin_Draft[_AutoLogin_Selected]
    copy := Map(
        "id", _AutoLogin_NewId(),
        "account", src["account"],
        "character", _AutoLogin_NextSlot(src["character"]),
        "group", src.Has("group") ? src["group"] : "",
        "enabled", src["enabled"],
        "hasPassword", true
    )
    if (src.Has("password") && src["password"] != "")
        copy["password"] := src["password"]
    else
        copy["passwordFrom"] := src.Has("passwordFrom") ? src["passwordFrom"] : src["id"]
    _AutoLogin_Draft.InsertAt(_AutoLogin_Selected + 1, copy)
    _AutoLogin_Selected += 1
    _AutoLogin_NativeRefreshList()
    _AutoLogin_NativeFillFields()
    _AutoLogin_NativeSetStatus("Duplicated — set the character, then Apply and Save.")
}

_AutoLogin_NextSlot(character) {
    c := Trim(String(character))
    if !RegExMatch(c, "^#(\d+)$", &m)
        return (c = "") ? "#1" : c
    n := Integer(m[1]) + 1
    return (n <= 5) ? "#" n : c
}

_AutoLogin_NativeRemove() {
    global _AutoLogin_Draft, _AutoLogin_Selected
    if (_AutoLogin_Selected < 1 || _AutoLogin_Selected > _AutoLogin_Draft.Length)
        return
    _AutoLogin_Draft.RemoveAt(_AutoLogin_Selected)
    if (_AutoLogin_Draft.Length = 0)
        _AutoLogin_Selected := 0
    else if (_AutoLogin_Selected > _AutoLogin_Draft.Length)
        _AutoLogin_Selected := _AutoLogin_Draft.Length
    _AutoLogin_NativeRefreshList()
    _AutoLogin_NativeFillFields()
    _AutoLogin_NativeSetStatus("Row removed. Save to write the file.")
}

_AutoLogin_NativeMove(delta) {
    global _AutoLogin_Draft, _AutoLogin_Selected
    dest := _AutoLogin_Selected + delta
    if (_AutoLogin_Selected < 1 || dest < 1 || dest > _AutoLogin_Draft.Length)
        return
    tmp := _AutoLogin_Draft[_AutoLogin_Selected]
    _AutoLogin_Draft[_AutoLogin_Selected] := _AutoLogin_Draft[dest]
    _AutoLogin_Draft[dest] := tmp
    _AutoLogin_Selected := dest
    _AutoLogin_NativeRefreshList()
}

_AutoLogin_NativeSave() {
    global _AutoLogin_Draft, _AutoLogin_Selected
    if (_AutoLogin_Selected >= 1) {
        if !_AutoLogin_NativeApply()
            return
    }
    payload := []
    for row in _AutoLogin_Draft {
        item := Map(
            "id", row["id"],
            "account", row["account"],
            "character", row["character"],
            "group", row.Has("group") ? row["group"] : "",
            "enabled", row["enabled"]
        )
        if (row.Has("password") && row["password"] != "")
            item["password"] := row["password"]
        else if (row.Has("passwordFrom") && row["passwordFrom"] != "")
            item["passwordFrom"] := row["passwordFrom"]
        payload.Push(item)
    }
    result := _AutoLogin_SaveIncoming(payload)
    if !result.ok {
        ShowMessage(result.reason, "Auto-login", _AutoLogin_DialogOpts("warn"))
        _AutoLogin_NativeSetStatus(result.reason)
        return
    }
    loaded := _AutoLogin_ReadFile()
    if loaded.ok
        _AutoLogin_Draft := loaded.rows
    _AutoLogin_NativeRefreshList()
    _AutoLogin_NativeFillFields()
    _AutoLogin_NativeSetStatus("Saved. Next launch claims these rows.")
}

_AutoLogin_NativeClose() {
    global _AutoLogin_NativeGui, _AutoLogin_PanelMode
    if IsObject(_AutoLogin_NativeGui)
        try _AutoLogin_NativeGui.Destroy()
    _AutoLogin_NativeGui := 0
    _AutoLogin_PanelMode := ""
}
