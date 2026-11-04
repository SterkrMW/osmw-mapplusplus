#Requires AutoHotkey v2.0

; The one place Maps++ touches the native client mods.
;
; The mods (osmw_core.dll and its feature DLLs) are a separate product, in the
; osMW server repo, shipped and updated independently of Maps++. They own two
; files beside main.exe:
;
;     osmw.ini            what the player has changed
;     osmw_defaults.ini   what the mods ship with; never written at runtime
;
; and every read is osmw.ini -> osmw_defaults.ini -> a literal compiled into
; the DLL. That is exactly the chain ConfigRead() uses for Maps++'s own
; config.ini/defaults.ini pair, against a different pair of files.
;
; THE RULES, and the reasons, because both halves have been broken before:
;
;   1. Maps++ writes osmw.ini only when the user asks for it -- a settings
;      field they edited, a menu item they clicked. Never on launch, never to
;      "sync" its own state. The old _ChatMod_SyncGate wrote [Addons] ChatMod
;      before every launch, which silently undid the in-game F12 panel.
;   2. One key per write, never a section rewrite and never IniDelete: a
;      running client may be writing its own keys into the same file.
;   3. Never write osmw_defaults.ini. It ships with the mods and is replaced
;      wholesale when they update.
;   4. Maps++ never deploys, updates or verifies the DLLs. It does not need
;      them to be installed at all: NativeModsInstalled() is false on a stock
;      client, and every caller here degrades to saying so.
;
; Maps++'s own config.ini and defaults.ini are a different pair of files, even
; when Maps++ is installed in the game folder and they sit side by side. They
; used to be the SAME files, which is what made [Addons], [Minimap] and [Debug]
; collide between the two products.

; The mods' settings live beside main.exe, which may not be beside Maps++.
; "" when no game path is configured yet -- every caller must handle it rather
; than writing to "\osmw.ini".
NativeModsIniPath() {
    gameDir := ResolveGameDirectory()
    return (gameDir = "") ? "" : gameDir "\osmw.ini"
}

NativeModsDefaultsPath() {
    gameDir := ResolveGameDirectory()
    return (gameDir = "") ? "" : gameDir "\osmw_defaults.ini"
}

; Whether this game folder actually has the mods. The core DLL is the one file
; that is always present when any of them are, because everything else is
; loaded by it.
NativeModsInstalled() {
    gameDir := ResolveGameDirectory()
    return (gameDir != "") && FileExist(gameDir "\osmw_core.dll")
}

; osmw.ini -> osmw_defaults.ini -> the caller's fallback. Reading the shipped
; defaults rather than guessing means a fresh install shows what the DLL will
; actually do.
NativeModsRead(section, key, fallback := "") {
    defaultsPath := NativeModsDefaultsPath()
    shipped := (defaultsPath = "")
        ? fallback
        : IniRead(defaultsPath, section, key, fallback)
    iniPath := NativeModsIniPath()
    if (iniPath = "")
        return shipped
    return IniRead(iniPath, section, key, shipped)
}

; One key, leaving the rest of the file alone. Returns false when there is
; nowhere to write, so a caller can say so instead of failing mute.
;
; Refuses outright on a game folder without the mods (rule 4): writing there
; would only create an osmw.ini that nothing reads, and that a later install of
; the mods would then inherit as if the player had set it.
NativeModsWrite(section, key, value) {
    iniPath := NativeModsIniPath()
    if (iniPath = "") {
        LogWarn("NativeMods", "No game path configured — nothing written")
        return false
    }
    if !NativeModsInstalled() {
        LogWarn("NativeMods", "Client mods not installed beside " iniPath
            . " — " section "/" key " not written")
        return false
    }
    try {
        EnsureIniUtf16(iniPath)
        IniWrite(value, iniPath, section, key)
    } catch as err {
        LogWarn("NativeMods", "Could not write " iniPath ": " err.Message)
        return false
    }
    return true
}

; ── The display module ───────────────────────────────────────────
;
; osmw_display.dll resizes the game window shortly after it appears — on a
; 1440p monitor its shipped default (Windowed, Scale=Auto) already makes every
; client 1600×1200 — and publishes where the picture ended up as window
; properties (see GameContentRect in functions.ahk). A launch that centres a
; client the instant its window exists centres the 800×600 one, and is then
; off-centre once the module grows it. This says whether that is coming, so a
; launch knows whether to wait for the size to settle. Read-only, like
; everything in this file that is not a user's explicit edit.
NativeDisplayModuleExpected() {
    if !NativeModsInstalled()
        return false
    if !FileExist(ResolveGameDirectory() "\osmw_display.dll")
        return false
    if (Trim(NativeModsRead("Addons", "Display", "1")) = "0")
        return false
    return StrLower(Trim(NativeModsRead("Display", "Mode", "Windowed"))) != "stock"
}

; ── Whether a running client loaded the mods at all ──────────────
;
; osmw_core.dll reports everything about the mods except its own refusal. The
; client loads it from mod_autoload's stub at the CRT entry, and a core that
; fails to load is deliberately ignored: the game carries on stock, behind
; Windows' own "Bad Image" box when Smart App Control refused the file. Nothing
; else follows. No status report is written (the one on disk is whatever the
; last good client left), and the in-game notice the core raises for refused
; FEATURES cannot appear, because the core is what raises it. Maps++ is the one
; thing left that can tell the player — which is also why it does not repeat
; that in-game notice for features: the core shows it once already.
;
; "refused" needs all three: the autoload patch in this process's code, the
; core DLL beside this process's own main.exe, and no osmw_core.dll among its
; modules. The core is loaded before CRT startup reaches WinMain, so once a
; client has a window the answer is final.
;
; Returns "loaded" | "refused" | "stock" (nothing in this client tried to load
; the mods) | "unknown" (could not tell; ask again later).
NativeModsCoreLoadState(proc, pid) {
    static cache := Map()
    key := pid ":" proc.modBase
    if cache.Has(key)
        return cache[key]
    result := _NativeModsProbeCore(proc, pid)
    if (result = "unknown")
        return result
    if (result = "refused")
        LogWarn("ClientMods", "osmw_core.dll is beside main.exe and the client is patched "
            . "to load it, but PID " pid " has no osmw_core.dll loaded — Windows refused "
            . "it, so that client runs without the mods. Smart App Control: "
            . (SmartAppControlState() = "" ? "unknown" : SmartAppControlState()))
    ; PIDs are recycled; see _NativeMinimapCapacity.
    if (cache.Count >= 32)
        cache := Map()
    cache[key] := result
    return result
}

_NativeModsProbeCore(proc, pid) {
    global NATIVE_MODS_AUTOLOAD_RVA, NATIVE_MODS_CORE_DLL, PROCESS_EXE
    if GetModuleBaseAddress(proc.handle, NATIVE_MODS_CORE_DLL)
        return "loaded"
    ; Not found can also mean the module list could not be read at all, which
    ; must not pass for a refusal. The exe itself is always in a readable list.
    if !GetModuleBaseAddress(proc.handle, PROCESS_EXE)
        return "unknown"
    code := ReadClientBuffer(proc, NATIVE_MODS_AUTOLOAD_RVA, 7)
    if !code
        return "unknown"
    if !(NumGet(code, 0, "UChar") = 0xE9 && NumGet(code, 5, "UChar") = 0x90
        && NumGet(code, 6, "UChar") = 0x90)
        return "stock"
    exePath := ""
    try exePath := ProcessGetPath(pid)
    if (exePath = "")
        return "unknown"
    SplitPath(exePath, , &exeDir)
    ; A patched client with no core beside it is a client without the mods
    ; installed, which runs stock by design.
    return FileExist(exeDir "\" NATIVE_MODS_CORE_DLL) ? "refused" : "stock"
}

; Smart App Control's state: "on", "evaluation", "off", or "" when the value
; cannot be read (older Windows, or no such policy). Only "on" blocks; in
; evaluation it watches without refusing anything.
SmartAppControlState() {
    v := ""
    try v := RegRead("HKLM\SYSTEM\CurrentControlSet\Control\CI\Policy",
        "VerifiedAndReputablePolicyState")
    if (v = 1)
        return "on"
    if (v = 2)
        return "evaluation"
    if (v = 0)
        return "off"
    return ""
}

; ── The mods' startup report ─────────────────────────────────────
;
; osmw_core.dll writes osmw_status.ini beside main.exe at each lifecycle point
; (running, safe mode, stopped...): the core's own state, a release-integrity
; check, and one section per feature module with its state, result code and
; version. It is the same payload the in-game F12 → Status page shows, and the
; one place that says a module was "Blocked by Windows".
;
; Read-only, like everything here that is not a user's explicit edit. Two
; things about the file itself, both from core/src/status.c:
;   - It is replaced atomically (temp file + MoveFileEx), so one FileRead sees
;     either the old report or the new one, never half of each.
;   - It is ONE file for every client in this folder, with no PID: the last
;     client to change state wrote it. Say so wherever it is shown.

NativeModsStatusPath() {
    gameDir := ResolveGameDirectory()
    return (gameDir = "") ? "" : gameDir "\osmw_status.ini"
}

; {ok, reason, path, written, format, core, integrity, modules}. reason is ""
; when ok, else "no-game-path" | "not-installed" | "no-status" | "unreadable" |
; "unknown-format". core and integrity are Maps of the two sections; modules is
; an Array of one Map per [Module.N], in N order.
NativeModsReadStatus() {
    st := { ok: false, reason: "", path: NativeModsStatusPath(), written: "", format: "",
        core: Map(), integrity: Map(), modules: [] }
    if (st.path = "") {
        st.reason := "no-game-path"
        return st
    }
    if !NativeModsInstalled() {
        st.reason := "not-installed"
        return st
    }
    if !FileExist(st.path) {
        st.reason := "no-status"
        return st
    }
    text := ""
    ; A client replacing the file at this instant holds it for a moment.
    loop 3 {
        try {
            ; 16 modules come to about 3 KB. Anything near this is not the report.
            if (FileGetSize(st.path) <= 65536)
                text := FileRead(st.path, "CP0")
            break
        } catch {
            Sleep 50
        }
    }
    if (text = "") {
        st.reason := "unreadable"
        return st
    }
    try st.written := FileGetTime(st.path, "M")

    sections := Map()
    sections.CaseSense := "Off"
    current := ""
    for line in StrSplit(text, "`n", "`r") {
        line := Trim(line)
        if (line = "" || SubStr(line, 1, 1) = ";")
            continue
        if RegExMatch(line, "^\[(.+)\]$", &hdr) {
            current := hdr[1]
            if !sections.Has(current) {
                sec := Map()
                sec.CaseSense := "Off"
                sections[current] := sec
            }
            continue
        }
        eq := InStr(line, "=")
        if (current = "" || !eq)
            continue
        sections[current][Trim(SubStr(line, 1, eq - 1))] := Trim(SubStr(line, eq + 1))
    }
    if sections.Has("Core")
        st.core := sections["Core"]
    if sections.Has("Integrity")
        st.integrity := sections["Integrity"]
    st.format := st.core.Has("Format") ? st.core["Format"] : ""
    if (st.format != "1") {
        st.reason := "unknown-format"
        return st
    }
    count := (st.core.Has("ModuleCount") && IsInteger(st.core["ModuleCount"]))
        ? Integer(st.core["ModuleCount"]) : 0
    loop Min(count, 64) {
        name := "Module." (A_Index - 1)
        if sections.Has(name)
            st.modules.Push(sections[name])
    }
    st.ok := true
    return st
}

NativeModsResultText(code) {
    global NATIVE_MODS_RESULT_NAMES
    if IsInteger(code) && NATIVE_MODS_RESULT_NAMES.Has(Integer(code))
        return NATIVE_MODS_RESULT_NAMES[Integer(code)]
    return "result " code
}

_NativeModsField(m, key) {
    return m.Has(key) ? m[key] : ""
}

; {text, severity} for ShowMessage: what the report says, in a sentence or
; three, worst news first.
NativeModsStatusSummary(st) {
    global NATIVE_MODS_BAD_STATES
    switch st.reason {
        case "no-game-path":
            return { severity: "warn", text: "No game path is set, so Maps++ cannot find "
                . "the client mods. Set one under Settings → Launcher." }
        case "not-installed":
            return { severity: "info", text: "The osMW client mods are not installed in "
                . ResolveGameDirectory() ". The game runs as stock; there is nothing to report." }
        case "no-status":
            return { severity: "info", text: "The client mods are installed, but no client "
                . "has written a status report yet. Start a client, then look again." }
        case "unreadable":
            return { severity: "warn", text: "Could not read " st.path "." }
        case "unknown-format":
            return { severity: "warn", text: "osmw_status.ini is in a format this Maps++ "
                . "does not know (Format=" st.format "). The client mods are newer than "
                . "Maps++ — the in-game F12 → Status page shows the same report." }
    }

    blocked := [], broken := [], disabled := []
    active := 0
    loginIdle := false
    for m in st.modules {
        label := _NativeModsField(m, "Name") != "" ? _NativeModsField(m, "Name") : _NativeModsField(m, "File")
        state := _NativeModsField(m, "State")
        if (_NativeModsField(m, "Result") = "-15") {
            blocked.Push(label)
            continue
        }
        if (state = "active") {
            active++
            continue
        }
        ; Auto-login stands down unless the client was launched asking for it
        ; (OSMW_AUTOLOGIN, which Maps++ sets per launch). That is its normal
        ; state in any client started by hand, not a feature switched off.
        if (state = "disabled" && _NativeModsField(m, "Id") = "login") {
            loginIdle := true
            continue
        }
        if (state = "disabled") {
            disabled.Push(label)
            continue
        }
        for bad in NATIVE_MODS_BAD_STATES {
            if (state = bad) {
                broken.Push(label)
                break
            }
        }
    }
    startup := _NativeModsField(st.core, "Startup")
    integrity := _NativeModsField(st.integrity, "State")

    text := active " of " st.modules.Length " client mod features active"
        . (startup != "" && startup != "running" ? " — the core reports " startup : "") "."
    severity := "success"
    if (_NativeModsField(st.core, "SafeMode") = "1" || startup = "safe-mode") {
        text .= "`n`nThe client was started in SAFE MODE, so no features load. Clear "
            . "[Core] SafeMode in osmw.ini (or OSMW_SAFE_MODE) to bring them back."
        severity := "warn"
    }
    if blocked.Length {
        text .= "`n`nBlocked by Windows: " _NativeModsJoin(blocked) ". Smart App Control "
            . "refuses unsigned DLLs — they are not damaged. The in-game notice explains "
            . "how to allow them."
        severity := "danger"
    }
    if broken.Length {
        text .= "`n`nNot working: " _NativeModsJoin(broken) ". Usually a client update the "
            . "mods have not caught up with; the table below says why."
        if (severity != "danger")
            severity := "warn"
    }
    if disabled.Length
        text .= "`n`nSwitched off: " _NativeModsJoin(disabled) "."
    if loginIdle
        text .= "`n`nAuto-login was idle in that client — it runs only in clients launched "
            . "with auto-login (Settings → Launcher), so this is normal for one started by hand."
    if (integrity = "mismatch" || integrity = "manifest-invalid") {
        text .= "`n`nRelease check: " integrity " — some mod files differ from the release "
            . "they came with."
        if (severity = "success")
            severity := "warn"
    }
    text .= "`n`nShared by every client in this folder; the last one to start or stop wrote it"
        . (st.written != "" ? " (" FormatTime(st.written, "yyyy-MM-dd HH:mm:ss") ")" : "") "."
    return { severity: severity, text: text }
}

_NativeModsJoin(arr) {
    out := ""
    for v in arr
        out .= (out = "" ? "" : ", ") v
    return out
}

; The whole report as a monospace block — the dialog's detail and the
; diagnostics report share it, so the two cannot drift.
NativeModsStatusTable(st) {
    if !st.ok
        return st.reason = "" ? "" : "  (" st.reason ")"
    f := _NativeModsField
    out := "Core      : " f(st.core, "Startup") "  ABI " f(st.core, "Abi")
        . (f(st.core, "SafeMode") = "1" ? "  SAFE MODE" : "") "`n"
        . "Integrity : " f(st.integrity, "State")
        . (f(st.integrity, "ReleaseReady") = "1" ? "  (release-ready)" : "")
        . "  verified " f(st.integrity, "Verified") "/" f(st.integrity, "Expected")
        . (f(st.integrity, "Missing") != "" && f(st.integrity, "Missing") != "0" ? "  missing " f(st.integrity, "Missing") : "")
        . (f(st.integrity, "Modified") != "" && f(st.integrity, "Modified") != "0" ? "  modified " f(st.integrity, "Modified") : "")
        . "`n"
        . "Written   : " (st.written != "" ? FormatTime(st.written, "yyyy-MM-dd HH:mm:ss") : "?") "`n`n"
        . Format("{:-22} {:-18} {:-6} {}", "Module", "State", "Ver", "Result") "`n"
    for m in st.modules {
        label := f(m, "File") != "" ? f(m, "File") : f(m, "Id")
        out .= Format("{:-22} {:-18} {:-6} {}", label, f(m, "State"), f(m, "Version"),
            NativeModsResultText(f(m, "Result"))) "`n"
    }
    return out
}

; ── The game's own map markers (map-markers.tsv) ─────────────────
;
; The minimap mod's persistent marker layer keeps its markers in one file beside
; main.exe, shared by every client in the folder. Its write rule, which Maps++
; follows exactly: take the folder's named mutex, reload the file, change only
; the records being changed (by their stable id), write a temporary file, then
; atomically replace. Writing back anything read before the lock is how one
; client erases another's markers.
;
; Maps++ replaces only records whose ids the caller names as its own. Every
; other line, the player's markers and anything this does not parse alike, is
; carried over byte for byte. A file whose header is not the one version known
; here is never written: it belongs to a newer writer, and the mod refuses to
; overwrite it too. Like every write in this file, it happens only when the user
; asks for one.

NativeModsMarkersPath() {
    global NATIVE_MODS_MARKERS_FILE
    gameDir := ResolveGameDirectory()
    return (gameDir = "") ? "" : gameDir "\" NATIVE_MODS_MARKERS_FILE
}

; Whether the game currently draws markers at all. Both gates are the mods'
; own: the minimap feature, and its marker layer, which ships off.
NativeModsMarkerLayerOn() {
    return Trim(NativeModsRead("Addons", "MinimapList", "1")) != "0"
        && Trim(NativeModsRead("Minimap", "MarkerLayer", "0")) != "0"
}

; A label as the file stores it: at most NATIVE_MODS_MARKER_LABEL_MAX bytes in
; the client's code page, cut on a character boundary so a double-byte
; character is never split, then percent-encoded the way the mod's
; mm_label_encode does ("%", control characters and every byte outside
; printable ASCII). A character the code page lacks becomes "?", as it would in
; anything else the client draws.
NativeModsMarkerLabel(text) {
    global NATIVE_MODS_MARKER_LABEL_MAX
    s := ""
    for ch in StrSplit(text) {
        if (StrPut(s ch, "CP0") - 1 > NATIVE_MODS_MARKER_LABEL_MAX)
            break
        s .= ch
    }
    n := StrPut(s, "CP0") - 1
    buf := Buffer(n + 1, 0)
    StrPut(s, buf, "CP0")
    out := ""
    loop n {
        b := NumGet(buf, A_Index - 1, "UChar")
        out .= (b = 0x25 || b < 0x20 || b > 0x7E) ? Format("%{:02X}", b) : Chr(b)
    }
    return out
}

; The mod's mutex name for a game folder (markers_io.c, mm_store_mutex_name):
; the folder as GetModuleFileNameA gives it, trailing separator dropped, with
; \ / : turned to _ and ASCII letters, only, lowercased.
_NativeModsMarkersMutexName(gameDir) {
    dir := gameDir
    while (StrLen(dir) > 1 && (SubStr(dir, -1) = "\" || SubStr(dir, -1) = "/"))
        dir := SubStr(dir, 1, -1)
    out := ""
    loop parse dir {
        o := Ord(A_LoopField)
        if (A_LoopField = "\" || A_LoopField = "/" || A_LoopField = ":")
            out .= "_"
        else if (o >= 65 && o <= 90)
            out .= Chr(o + 32)
        else
            out .= A_LoopField
    }
    return "Global\osmw_map_markers_" out
}

; {state, h}: "ok" (held), "none" (no mutex to be had, and no client running
; to contend for the file), "locked" (a client held it past the wait) or
; "lock-unavailable" (clients are running and the lock cannot be opened).
_NativeModsMarkersLock(gameDir) {
    global NATIVE_MODS_MARKERS_LOCK_MS
    static SYNCHRONIZE_AND_MODIFY := 0x00100001
    name := _NativeModsMarkersMutexName(gameDir)
    h := DllCall("CreateMutexW", "Ptr", 0, "Int", 0, "WStr", name, "Ptr")
    if !h
        h := DllCall("OpenMutexW", "UInt", SYNCHRONIZE_AND_MODIFY, "Int", 0, "WStr", name, "Ptr")
    if !h
        return { state: GetTopLevelGameWindows().Length ? "lock-unavailable" : "none", h: 0 }
    ; WAIT_ABANDONED (0x80): a client died holding it. The mod takes it anyway,
    ; and the file it left is whole, because the replace is atomic.
    r := DllCall("WaitForSingleObject", "Ptr", h, "UInt", NATIVE_MODS_MARKERS_LOCK_MS, "UInt")
    if (r = 0 || r = 0x80)
        return { state: "ok", h: h }
    DllCall("CloseHandle", "Ptr", h)
    return { state: "locked", h: 0 }
}

_NativeModsMarkersUnlock(lock) {
    if lock.h {
        DllCall("ReleaseMutex", "Ptr", lock.h)
        DllCall("CloseHandle", "Ptr", lock.h)
    }
}

; Replaces the markers in `ownedIds` with `records`, under the mod's lock.
; Each record is {id, mapFile, x, y, kind, label}, label as plain text; a
; record's own id also counts as owned, so an export repeated after its list of
; ids was lost updates its markers instead of doubling them.
;
; Returns {ok, reason, written, removedIds, full}. written are the ids now in
; the file, removedIds the owned ids that were taken out (an id in both was
; updated), full how many records the mod's limits had no room for. reason, when
; not ok: "no-game-path" | "not-installed" | "locked" | "lock-unavailable" |
; "unknown-format" | "read-failed" | "write-failed".
NativeModsMarkersReplace(ownedIds, records) {
    global NATIVE_MODS_MARKERS_HEADER, NATIVE_MODS_MARKERS_PER_MAP, NATIVE_MODS_MARKERS_MAX
    res := { ok: false, reason: "", written: [], removedIds: [], full: 0 }
    gameDir := ResolveGameDirectory()
    if (gameDir = "") {
        res.reason := "no-game-path"
        return res
    }
    if !NativeModsInstalled() {
        res.reason := "not-installed"
        return res
    }
    path := NativeModsMarkersPath()
    lock := _NativeModsMarkersLock(gameDir)
    if (lock.state != "ok" && lock.state != "none") {
        res.reason := lock.state
        return res
    }
    try {
        ; Latin-1 maps every byte to one character and back, so lines this
        ; does not own are written out exactly as they were read.
        text := ""
        if FileExist(path) {
            try {
                text := FileRead(path, "CP28591")
            } catch {
                res.reason := "read-failed"
                return res
            }
        }
        lines := []
        if (text != "") {
            all := StrSplit(text, "`n", "`r")
            if (all[1] != NATIVE_MODS_MARKERS_HEADER) {
                res.reason := "unknown-format"
                return res
            }
            loop all.Length - 1 {
                if (all[A_Index + 1] != "")
                    lines.Push(all[A_Index + 1])
            }
        }

        owned := Map()
        for id in ownedIds
            owned[StrLower(id)] := true
        for r in records
            owned[r.id] := true

        out := NATIVE_MODS_MARKERS_HEADER "`r`n"
        perMap := Map()
        total := 0
        for line in lines {
            f := StrSplit(line, "`t")
            if owned.Has(StrLower(f[1])) {
                res.removedIds.Push(StrLower(f[1]))
                continue
            }
            out .= line "`r`n"
            total++
            ; A map file is at most 65535, so five digits; anything else is a
            ; record the mod rejects, and counts toward no map.
            if (f.Length >= 2 && RegExMatch(f[2], "^\d{1,5}$")) {
                mf := Integer(f[2])
                perMap[mf] := (perMap.Has(mf) ? perMap[mf] : 0) + 1
            }
        }

        seen := Map()
        for r in records {
            if seen.Has(r.id)
                continue
            seen[r.id] := true
            onMap := perMap.Has(r.mapFile) ? perMap[r.mapFile] : 0
            if (onMap >= NATIVE_MODS_MARKERS_PER_MAP || total >= NATIVE_MODS_MARKERS_MAX) {
                res.full++
                continue
            }
            ; Flags 1 = visible, the only bit the format defines.
            out .= r.id "`t" r.mapFile "`t" r.x "`t" r.y "`t" r.kind "`t1`t"
                . NativeModsMarkerLabel(r.label) "`r`n"
            perMap[r.mapFile] := onMap + 1
            total++
            res.written.Push(r.id)
        }

        tmp := path ".mapspp.tmp"
        try {
            f := FileOpen(tmp, "w", "CP28591")
            f.Write(out)
            DllCall("FlushFileBuffers", "Ptr", f.Handle)
            f.Close()
        } catch as err {
            try FileDelete(tmp)
            LogWarn("ClientMods", "Could not write " tmp ": " err.Message)
            res.reason := "write-failed"
            return res
        }
        ; MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH: a client polling
        ; the file sees the old one or the new one, never half of either.
        if !DllCall("MoveFileExW", "WStr", tmp, "WStr", path, "UInt", 0x9) {
            code := A_LastError
            try FileDelete(tmp)
            LogWarn("ClientMods", "Could not replace " path " (error " code ")")
            res.reason := "write-failed"
            return res
        }
        res.ok := true
        LogInfo("ClientMods", "map-markers.tsv: wrote " res.written.Length ", took out "
            . res.removedIds.Length ", " res.full " over the limits"
            . (lock.state = "none" ? " (no lock: no client running)" : ""))
        return res
    } finally {
        _NativeModsMarkersUnlock(lock)
    }
}

; ── Keys the mods use in game ────────────────────────────────────
;
; A Maps++ hotkey on one of these does not stop the mod seeing the key, so one
; press does two things. That is a warning, never a refusal: these are the
; mods' settings, they can change under Maps++ at any time, and a player may
; want both. Read only when asked (hotkey capture, Save, diagnostics).
;
; Each entry is {owner, key, anyMods}. The F-key features poll the key's state
; directly, so they fire whatever modifiers are held: those match on the
; trigger key alone (anyMods). Chat Mod's two are real system-wide hotkeys,
; registered with exact modifiers, so those match the whole chord.
NativeModsReservedKeys() {
    keys := []
    if !NativeModsInstalled()
        return keys
    if _NativeModsGateOn("Settings")
        _NativeModsPushKey(keys, "the client's settings panel key",
            NativeModsRead("Settings", "OpenKey", "F12"))
    if _NativeModsGateOn("FightSpeed")
        _NativeModsPushKey(keys, "the client's fight speed key",
            NativeModsRead("Fight", "CycleKey", "F11"))
    ; Fixed in the DLL; there is no setting for it.
    if _NativeModsGateOn("Timers")
        _NativeModsPushKey(keys, "the client's timer panel key", "F9")
    if _NativeModsGateOn("Leaderboard")
        _NativeModsPushKey(keys, "the client's leaderboard key",
            NativeModsRead("Leaderboard", "OpenKey", ""))
    if _NativeModsGateOn("ChatMod") {
        for name, what in Map("ClearHotkey", "clear-chat", "TabHotkey", "chat tab") {
            c := NativeModsChordFromText(NativeModsRead("Chat", name, ""))
            if (c != "")
                keys.Push({ owner: "Chat Mod's " what " hotkey", key: c, anyMods: false })
        }
    }
    return keys
}

_NativeModsGateOn(name) {
    return Trim(NativeModsRead("Addons", name, "1")) != "0"
}

_NativeModsPushKey(keys, owner, keyName) {
    keyName := StrLower(Trim(keyName))
    if (keyName != "")
        keys.Push({ owner: owner, key: keyName, anyMods: true })
}

; Chat Mod spells its hotkeys "CTRL+SHIFT+C"; Maps++ spells the same chord
; "^+c". "" for anything that is not a modifier list ending in one key.
NativeModsChordFromText(text) {
    text := Trim(text)
    if (text = "")
        return ""
    parts := StrSplit(text, "+", " `t")
    keyName := parts.Pop()
    if (keyName = "")
        return ""
    mods := ""
    for part in parts {
        switch StrUpper(part) {
            case "CTRL", "CONTROL": mods .= "^"
            case "ALT": mods .= "!"
            case "SHIFT": mods .= "+"
            case "WIN": mods .= "#"
            default: return ""
        }
    }
    return CanonicalHotkeyChord(mods keyName)
}

; The mod feature that also uses `chord` in game, or "". Pass a list from
; NativeModsReservedKeys() when checking many chords at once, so the ini is
; read once rather than once per chord.
NativeModsKeyConflict(chord, reserved := 0) {
    canonical := CanonicalHotkeyChord(chord)
    if (canonical = "")
        return ""
    trigger := StrLower(HotkeyTriggerKey(chord))
    if !IsObject(reserved)
        reserved := NativeModsReservedKeys()
    for r in reserved {
        if (r.anyMods ? (trigger = r.key) : (canonical = r.key))
            return r.owner
    }
    return ""
}

; ── The mods' own logging ────────────────────────────────────────
;
; Each DLL writes one log per client per launch into the game folder. That is
; how a mod problem gets diagnosed, and clutter the rest of the time -- five
; clients a launch, every launch, for a player with nothing to investigate --
; so they read this key as a client starts and open no file at all when it is
; clear. Off is the default on both sides.

ModLoggingEnabled() {
    global MOD_LOG_SECTION, MOD_LOG_KEY
    return NativeModsRead(MOD_LOG_SECTION, MOD_LOG_KEY, "0") = "1"
}

SetModLogging(enabled) {
    global MOD_LOG_SECTION, MOD_LOG_KEY
    if !NativeModsWrite(MOD_LOG_SECTION, MOD_LOG_KEY, enabled ? "1" : "0")
        return false
    LogInfo("NativeMods", "Client mod logging turned " (enabled ? "on" : "off")
        . " — applies to clients launched from now on")
    return true
}
