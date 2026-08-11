#Requires AutoHotkey v2.0

; ── Shared modal dialogs ─────────────────────────────────────────
; One themed replacement for MsgBox and InputBox, so a confirm or a rename
; prompt raised from a WebView2 panel does not open a Win32 dialog in the
; middle of it. Every user-facing dialog in the app goes through the three
; entry points below:
;
;   ShowMessage(text, title, opts)          → "OK"
;   ConfirmAction(text, title, opts)        → true / false
;   PromptText(text, title, default, opts)  → {Result: "OK"|"Cancel", Value: ""}
;
; PromptText deliberately returns InputBox's own shape so call sites that
; already test `.Result` and read `.Value` are unchanged.
;
; `opts` is an optional Map:
;   "severity"    "info" | "warn" | "danger" | "success" | "ask"
;   "detail"      monospace block under the message (diagnostic dumps)
;   "okLabel"     / "cancelLabel"   button text
;   "danger"      true → the confirming button is styled destructive
;   "inputLabel"  / "placeholder"   prompt field only
;   "owner"       hwnd to centre on and disable while the dialog is up
;
; ── Why these block ──────────────────────────────────────────────
; MsgBox and InputBox return a value to the line that called them, and roughly
; forty call sites are written that way. Rewriting each into a callback to gain
; nothing the user can see would be the expensive way to do this, so the
; WebView2 dialog blocks too: it parks the calling code in a Sleep loop until
; the page answers. AHK pumps messages during Sleep, which is how the answer
; arrives — and is exactly what MsgBox itself does, so timers and hotkeys keep
; running while a dialog is up, as they always have.
;
; Blocking is also why the owner window is disabled for the duration. Without
; it the panel behind the dialog would keep processing its own WebView
; messages, and a "confirm delete" could be answered by clicking something else
; entirely. `_Dlg_Run` restores it in a finally block — a dialog that threw and
; left the panel dead would be far worse than an ugly one.
;
; ── Falling back ─────────────────────────────────────────────────
; Native ("low memory") mode keeps MsgBox/InputBox: standing up Chromium for a
; confirm is precisely what that mode exists to avoid, and a Win32 dialog is
; not out of place among Win32 panels. The WebView path also falls back to it
; whenever it cannot be trusted to answer — page missing, page never reported
; ready, or a dialog already on screen. A dialog that silently never returns
; would hang the whole single-threaded app, so every wait here has a deadline
; and every deadline ends in a MsgBox rather than in a spin.
;
; ── The one place blocking is not allowed ────────────────────────
; A page's message arrives as an inbound cross-process COM call. While one is
; on the stack the STA will not deliver another, *even across Sleep* — which is
; where the "AHK pumps messages during Sleep" intuition above stops holding.
; Outbound calls (PostWebMessageAsJson) still work, so a dialog raised from
; inside a WebMessageReceived handler visibly appears and then does nothing: its
; own Yes/No is another WebMessage and can never arrive. The caller spins
; forever with the panel left EnableWindow(false) and no way out but a restart.
;
; Two things keep that from happening, and both are needed:
;
;   • Every WebMessageReceived registration wraps its router in WebMsgHandler,
;     so _Dlg_Run can see a COM callback on the stack and take the native path
;     instead of blocking. This is the net: it makes the hang impossible rather
;     than merely unlikely, and logs when it catches something.
;   • Handlers that mean to raise a dialog call DeferFromWebMessage first, which
;     re-enters from AHK's own message loop after the COM call has returned.
;     That is what keeps the themed dialog instead of a Win32 one, so it is the
;     fix and the guard above is the backstop.
;
; settings.ahk:624 hit this first and fixed it in one place; the guard here is
; what stops the next one being found by a user.

global DIALOG_W          := 420     ; message / prompt width, CSS px
global DIALOG_W_DETAIL   := 620     ; wider when a monospace block is present
global DIALOG_MIN_H      := 140
global DIALOG_MAX_H      := 620
global DIALOG_READY_MS   := 4000    ; page load budget before falling back
global DIALOG_ACK_MS     := 800     ; page silence that proves the reply channel is dead
global DIALOG_SIZE_MS    := 900     ; self-measure budget before showing anyway

global gDlgGui     := 0        ; built once, then parked off-screen
global gDlgReady   := false    ; page has loaded and spoken
global gDlgBusy    := false    ; a dialog is on screen right now
global gDlgToken   := 0        ; increments per dialog; stale replies are ignored
global gDlgAnswer  := 0        ; Map once the page (or the window) answered
global gDlgShown   := false    ; moved on screen for the current dialog
global gDlgHeard   := false    ; page has said anything at all about this dialog
global gDlgSpec    := 0        ; the dialog being shown, for geometry on reveal
global gDlgOwner   := 0        ; window disabled for it, and centred on

global gWebMsgDepth := 0       ; > 0 while a WebMessageReceived handler is on the stack

; ── Entry points ─────────────────────────────────────────────────

ShowMessage(text, title := "Maps++", opts := 0) {
    spec := _Dlg_Spec("say", text, title, opts)
    answer := _Dlg_Run(spec)
    if IsObject(answer)
        return "OK"
    ; Native path. The detail block has nowhere else to go in a MsgBox, so it
    ; goes under the message rather than being dropped.
    body := spec["text"]
    if (spec["detail"] != "")
        body .= "`n`n" spec["detail"]
    MsgBox(body, spec["title"], _Dlg_NativeIcon(spec["severity"]))
    return "OK"
}

ConfirmAction(text, title := "Maps++", opts := 0) {
    spec := _Dlg_Spec("ask", text, title, opts)
    answer := _Dlg_Run(spec)
    if IsObject(answer)
        return answer["result"] = "ok"
    body := spec["text"]
    if (spec["detail"] != "")
        body .= "`n`n" spec["detail"]
    ; Default2 = No, so the destructive answer is never one stray Enter away.
    return MsgBox(body, spec["title"],
        "YesNo Default2 " _Dlg_NativeIcon(spec["severity"])) = "Yes"
}

PromptText(text, title := "Maps++", initial := "", opts := 0) {
    spec := _Dlg_Spec("prompt", text, title, opts)
    spec["value"] := String(initial)
    answer := _Dlg_Run(spec)
    if IsObject(answer) {
        return { Result: (answer["result"] = "ok" ? "OK" : "Cancel"),
                 Value: answer["value"] }
    }
    return InputBox(spec["text"], spec["title"], "w360 h150", spec["value"])
}

; ── Spec ─────────────────────────────────────────────────────────

_Dlg_Spec(kind, text, title, opts) {
    o := IsObject(opts) ? opts : Map()
    return Map(
        "kind",        kind,
        "text",        String(text),
        "title",       String(title),
        "severity",    _Dlg_Opt(o, "severity", (kind = "ask") ? "ask" : "info"),
        "detail",      _Dlg_Opt(o, "detail", ""),
        "okLabel",     _Dlg_Opt(o, "okLabel", (kind = "ask") ? "Yes" : "OK"),
        "cancelLabel", _Dlg_Opt(o, "cancelLabel", (kind = "ask") ? "No" : "Cancel"),
        "danger",      (o.Has("danger") && o["danger"]) ? true : false,
        "inputLabel",  _Dlg_Opt(o, "inputLabel", ""),
        "placeholder", _Dlg_Opt(o, "placeholder", ""),
        "value",       "",
        "owner",       o.Has("owner") ? o["owner"] : 0)
}

_Dlg_Opt(o, key, fallback) {
    return (o.Has(key) && o[key] != "") ? String(o[key]) : fallback
}

_Dlg_NativeIcon(severity) {
    switch severity {
        case "warn":    return "Icon!"
        case "danger":  return "Iconx"
        case "ask":     return "Icon?"
        default:        return "Iconi"
    }
}

; ── WebView message re-entrancy ──────────────────────────────────
; See the header. These two live here rather than in functions.ahk because the
; problem they exist for is modality: dialogs.ahk owns every blocking wait in
; the app, so it owns knowing when blocking is safe.

; Wraps a WebMessageReceived router so the dialog layer can tell that a COM
; callback is on the stack. Every registration in the app goes through this —
; a closure is a valid handler, as radial.ahk's per-ring handlers already show.
;
; The two parameters are declared rather than taken variadically on purpose:
; the COM sink sizes its callback from the handler's MinParams (WebView2.ahk's
; `cb.MinParams || 2`), and a variadic closure reports 0 there. Every router in
; the app takes (wv, args), so this matches them exactly.
WebMsgHandler(fn) {
    return (wv, args) => _WebMsgDispatch(fn, wv, args)
}

_WebMsgDispatch(fn, wv, args) {
    global gWebMsgDepth
    gWebMsgDepth += 1
    try
        fn(wv, args)
    finally
        gWebMsgDepth -= 1
}

InWebMessageCallback() {
    global gWebMsgDepth
    return gWebMsgDepth > 0
}

; Leaves the current WebView message handler before doing anything that can
; raise a dialog. A negative period fires the callback once from AHK's own
; message loop, after the COM call has returned — which is where blocking is
; safe. Callers must re-check that their window still exists when they land:
; the panel can be gone by then. Outside a message handler this is a harmless
; one-tick delay, so a shared entry point may use it unconditionally.
DeferFromWebMessage(fn, params*) {
    SetTimer(fn.Bind(params*), -1)
}

; ── Runner ───────────────────────────────────────────────────────

; Returns the answer Map, or 0 to mean "use the native dialog instead".
_Dlg_Run(spec) {
    global gDlgGui, gDlgBusy, gDlgToken, gDlgAnswer, gDlgShown, gDlgHeard
    global gDlgSpec, gDlgOwner

    if !_Dlg_CanUseWebView()
        return 0
    ; A dialog raised while one is already up (from a timer, say) cannot reuse
    ; the single parked window, and queueing it behind the first would look
    ; like a freeze. It gets a MsgBox — rare, and honest about being nested.
    if gDlgBusy
        return 0
    ; Blocking here could never end — see the header. A MsgBox is worse-looking
    ; and entirely functional. Logged, because a handler reaching this has a
    ; missing DeferFromWebMessage and the log is where that becomes findable.
    if InWebMessageCallback() {
        LogWarn("Dialog", "raised inside a WebView message handler — using native")
        return 0
    }

    ownerHwnd := 0
    ownerDisabled := false
    ; Claimed before the page is stood up, not after: _Dlg_EnsureGui sleeps for
    ; up to DIALOG_READY_MS, and a timer firing in that window would otherwise
    ; re-enter, build a second WebViewGui and overwrite gDlgGui with it.
    gDlgBusy := true

    try {
        if !_Dlg_EnsureGui()
            return 0

        ; Resolved once and remembered: the reveal needs the same window to
        ; centre on that was disabled here, and re-resolving it there would
        ; consult a foreground window that this dialog has meanwhile changed.
        ownerHwnd := _Dlg_ResolveOwner(spec)

        gDlgAnswer := 0
        gDlgShown := false
        gDlgHeard := false
        gDlgSpec := spec
        gDlgOwner := ownerHwnd
        gDlgToken += 1

        if (ownerHwnd && _Dlg_SetEnabled(ownerHwnd, false))
            ownerDisabled := true

        ; A page that was never told to show anything will never answer.
        if !_Dlg_Post(spec)
            return 0

        ; Only the machine's half of the wait is bounded. The page answers
        ; "dlg-size" long before the user can reach a button, so silence past
        ; ACK_MS means the reply channel is dead and no answer is coming — bail
        ; to the native dialog rather than spin with the owner disabled. It sits
        ; below SIZE_MS deliberately, so a dead channel bails before the reveal
        ; backstop puts a dialog on screen that is about to be replaced.
        ; Once the page has spoken the wait is open-ended again: a human may
        ; take as long as they like to answer.
        ackDeadline := A_TickCount + DIALOG_ACK_MS
        sizeDeadline := A_TickCount + DIALOG_SIZE_MS
        while !IsObject(gDlgAnswer) {
            if (!IsObject(gDlgGui) || !gDlgGui.Hwnd) {
                ; Window died under us; treat as a cancel rather than spin.
                gDlgAnswer := Map("result", "cancel", "value", "")
                break
            }
            if (!gDlgHeard && A_TickCount > ackDeadline) {
                LogWarn("Dialog", "page did not acknowledge within "
                    . DIALOG_ACK_MS "ms — using native")
                return 0
            }
            ; Covers a page that renders but never reports its height — show it
            ; at a default rather than leave the user staring at a disabled
            ; panel and nothing else.
            if (!gDlgShown && A_TickCount > sizeDeadline)
                _Dlg_Reveal(0)
            Sleep(10)
        }
        return gDlgAnswer
    } finally {
        _Dlg_Park()
        gDlgBusy := false
        gDlgSpec := 0
        gDlgOwner := 0
        if ownerDisabled {
            _Dlg_SetEnabled(ownerHwnd, true)
            ; Re-enabling does not restore focus, and the panel the user was
            ; working in would otherwise sink behind the game.
            try WinActivate("ahk_id " ownerHwnd)
        }
    }
}

_Dlg_CanUseWebView() {
    return IsWebViewInterface()
        && FileExist(A_ScriptDir "\Lib\WebViewToo.ahk")
        && FileExist(A_ScriptDir "\ui\dialog\index.html")
}

; The window the dialog belongs to: whatever the caller named, else the
; foreground window if it is one of ours. A game window is never an owner —
; disabling the client the user is playing would be a bug, not modality.
;
; Two of this app's own windows must be rejected even though they pass that
; test. The dialog window itself is foreground for as long as a dialog is up,
; so the *second* dialog of a session would otherwise adopt the first as its
; owner, centre on it where it now sits parked off-screen, and disable it —
; the window it is about to show in. Any other parked window (the tray menu,
; a radial ring) is wrong for the same reason, so the off-screen test covers
; the whole family rather than naming them.
_Dlg_ResolveOwner(spec) {
    global gDlgGui
    if (spec.Has("owner") && spec["owner"])
        return _Dlg_UsableOwner(spec["owner"])
    hwnd := 0
    try hwnd := WinExist("A")
    if !hwnd
        return 0
    if (IsObject(gDlgGui) && gDlgGui.Hwnd && hwnd = gDlgGui.Hwnd)
        return 0
    pid := 0
    try pid := WinGetPID("ahk_id " hwnd)
    if (pid != DllCall("GetCurrentProcessId", "UInt"))
        return 0
    return _Dlg_UsableOwner(hwnd)
}

; Parked windows live at -30000; minimized ones at -32000. Neither is somewhere
; a dialog should be centred, and neither needs disabling to be unreachable.
_Dlg_UsableOwner(hwnd) {
    if (!hwnd || _Dlg_IsMinimized(hwnd))
        return 0
    try {
        WinGetPos(&ox, &oy, , , "ahk_id " hwnd)
        if (ox <= -20000 || oy <= -20000)
            return 0
    }
    return hwnd
}

_Dlg_IsMinimized(hwnd) {
    if !hwnd
        return false
    try return WinGetMinMax("ahk_id " hwnd) = -1
    return false
}

_Dlg_SetEnabled(hwnd, enabled) {
    try {
        DllCall("EnableWindow", "Ptr", hwnd, "Int", enabled ? 1 : 0)
        return true
    }
    return false
}

; ── Window ───────────────────────────────────────────────────────

; Built once and kept for the session, parked off-screen between dialogs, for
; the same reason as the tray menu and the radial rings: standing up a WebView2
; costs hundreds of milliseconds, and a confirm that takes half a second to
; appear reads as a hang. Unlike those two it is not prewarmed — a session may
; never raise a dialog at all.
_Dlg_EnsureGui() {
    global gDlgGui, gDlgReady, DIALOG_W, DIALOG_MIN_H, DIALOG_READY_MS

    if (IsObject(gDlgGui) && gDlgGui.Hwnd)
        return true

    gDlgReady := false
    dllDir := (A_PtrSize = 8) ? "64bit" : "32bit"
    dllPath := A_ScriptDir "\Lib\" dllDir "\WebView2Loader.dll"
    wvSettings := { DllPath: dllPath, DefaultWidth: DIALOG_W, DefaultHeight: DIALOG_MIN_H }

    try {
        g := WebViewGui("-Caption +AlwaysOnTop +ToolWindow -Resize", "Maps++", , wvSettings)
    } catch as err {
        LogWarn("Dialog", "WebView2 dialog unavailable — using native: " err.Message)
        return false
    }
    gDlgGui := g

    ; The X button and Alt+F4 are a cancel, never a silent dismissal that
    ; leaves the caller waiting.
    g.OnEvent("Close", (*) => _Dlg_Answer("cancel", ""))
    g.WebMessageReceived(WebMsgHandler(_Dlg_OnWebMessage))
    g.DOMContentLoaded((*) => _Dlg_MarkReady())
    g.Navigate(UiPageUrl("ui/dialog/index.html"))
    g.Show("x-30000 y-30000 w" DIALOG_W " h" DIALOG_MIN_H " NoActivate")

    deadline := A_TickCount + DIALOG_READY_MS
    while (!gDlgReady && A_TickCount < deadline) {
        if (!IsObject(gDlgGui) || !gDlgGui.Hwnd)
            break
        Sleep(15)
    }
    if !gDlgReady {
        ; A page that never loaded will never answer either. Drop the window so
        ; the next dialog gets a clean attempt instead of inheriting this one.
        LogWarn("Dialog", "Dialog page did not load within " DIALOG_READY_MS "ms — using native")
        try gDlgGui.Destroy()
        gDlgGui := 0
        return false
    }
    return true
}

_Dlg_MarkReady() {
    global gDlgReady
    gDlgReady := true
}

; Returns false when the page could not be told to show anything, so the caller
; can fall back at the cause rather than waiting out the acknowledgement
; deadline for an answer that was never asked for.
_Dlg_Post(spec) {
    global gDlgGui, gDlgToken
    if !IsObject(gDlgGui)
        return false
    json := '{"type":"dlg-show"'
        . ',"token":' gDlgToken
        . ',"kind":' _JSON_Str(spec["kind"])
        . ',"severity":' _JSON_Str(spec["severity"])
        . ',"title":' _JSON_Str(spec["title"])
        . ',"text":' _JSON_Str(spec["text"])
        . ',"detail":' _JSON_Str(spec["detail"])
        . ',"okLabel":' _JSON_Str(spec["okLabel"])
        . ',"cancelLabel":' _JSON_Str(spec["cancelLabel"])
        . ',"danger":' (spec["danger"] ? "true" : "false")
        . ',"inputLabel":' _JSON_Str(spec["inputLabel"])
        . ',"placeholder":' _JSON_Str(spec["placeholder"])
        . ',"value":' _JSON_Str(spec["value"])
        . '}'
    try {
        gDlgGui.PostWebMessageAsJson(json)
        return true
    } catch as err {
        LogWarn("Dialog", "could not post to the dialog page: " err.Message)
        return false
    }
}

_Dlg_OnWebMessage(wv, args) {
    global gDlgToken, gDlgHeard
    msgStr := ""
    try msgStr := args.TryGetWebMessageAsString()
    if (msgStr = "") {
        try msgStr := args.WebMessageAsJson
    }
    if (msgStr = "")
        return

    msg := _JSON_Parse(msgStr)
    if !IsObject(msg) || !msg.Has("type")
        return

    ; The window outlives every dialog shown in it, so a reply that arrives
    ; late — after its dialog was answered another way — must not resolve
    ; whichever dialog happens to be up now.
    if (msg.Has("token") && msg["token"] != gDlgToken)
        return

    ; Anything at all from the page for this dialog proves the reply channel is
    ; open, which is what the acknowledgement deadline in _Dlg_Run waits on.
    gDlgHeard := true

    switch msg["type"] {
        case "dlg-size":
            _Dlg_Reveal(msg.Has("h") ? Integer(msg["h"]) : 0)
        case "dlg-result":
            _Dlg_Answer(msg.Has("result") ? msg["result"] : "cancel",
                        msg.Has("value") ? msg["value"] : "")
        case "dlg-copy":
            if msg.Has("text")
                A_Clipboard := msg["text"]
    }
}

_Dlg_Answer(result, value) {
    global gDlgAnswer, gDlgBusy
    if !gDlgBusy
        return
    if IsObject(gDlgAnswer)
        return
    gDlgAnswer := Map("result", (result = "ok" ? "ok" : "cancel"), "value", String(value))
}

; Moves the parked window into view at the height the page measured. `height`
; of 0 means the page never reported and this is the backstop.
_Dlg_Reveal(height) {
    global gDlgGui, gDlgShown, gDlgSpec, gDlgOwner
    global DIALOG_W, DIALOG_W_DETAIL, DIALOG_MIN_H, DIALOG_MAX_H

    if (gDlgShown || !IsObject(gDlgGui) || !gDlgGui.Hwnd || !IsObject(gDlgSpec))
        return
    gDlgShown := true

    w := (gDlgSpec["detail"] != "") ? DIALOG_W_DETAIL : DIALOG_W
    h := height ? height : 200
    if (h < DIALOG_MIN_H)
        h := DIALOG_MIN_H
    if (h > DIALOG_MAX_H)
        h := DIALOG_MAX_H

    pos := _Dlg_Geometry(w, h, gDlgOwner)
    try {
        WinMove(pos.x, pos.y, w, h, "ahk_id " gDlgGui.Hwnd)
        ; Parked NoActivate, so it has to be raised explicitly — a dialog the
        ; keyboard cannot reach is worse than no dialog.
        WinActivate("ahk_id " gDlgGui.Hwnd)
    }
}

; Centred on the owning panel, else on the game client, else on the display the
; cursor is on — and always clamped inside that display's work area.
;
; Both of the first two candidates are skipped when their window is minimized.
; A minimized window's rect is (-32000, -32000), which is not a "slightly off"
; centre the clamp can rescue: it pins every dialog to the top-left corner of
; the work area, which is what a minimized game client did here.
_Dlg_Geometry(w, h, ownerHwnd) {
    cx := 0, cy := 0, centred := false
    if (ownerHwnd && !_Dlg_IsMinimized(ownerHwnd)) {
        try {
            WinGetPos(&ox, &oy, &ow, &oh, "ahk_id " ownerHwnd)
            cx := ox + ow // 2
            cy := oy + oh // 2
            centred := true
        }
    }
    if !centred {
        area := GameClientRect()
        if (area.ok && !_Dlg_IsMinimized(area.hwnd)) {
            cx := area.x + area.w // 2
            cy := area.y + area.h // 2
            centred := true
        }
    }
    if !centred {
        ; The cursor picks the display, not the position — a modal parked under
        ; the pointer in a screen corner reads as a stray tooltip.
        CoordMode("Mouse", "Screen")
        MouseGetPos(&mx, &my)
        MonitorGetWorkArea(GetMonitorIndexAtPoint(mx, my), &ml, &mt, &mr, &mb)
        cx := (ml + mr) // 2
        cy := (mt + mb) // 2
    }

    MonitorGetWorkArea(GetMonitorIndexAtPoint(cx, cy), &wl, &wt, &wr, &wb)
    x := cx - w // 2
    y := cy - h // 2
    if (x + w > wr)
        x := wr - w
    if (x < wl)
        x := wl
    if (y + h > wb)
        y := wb - h
    if (y < wt)
        y := wt
    return { x: x, y: y }
}

; Dismisses by parking off-screen. The window and its loaded page survive,
; which is what makes the second dialog of a session instant.
_Dlg_Park() {
    global gDlgGui, gDlgShown
    if (IsObject(gDlgGui) && gDlgGui.Hwnd) {
        try WinMove(-30000, -30000, , , "ahk_id " gDlgGui.Hwnd)
    }
    gDlgShown := false
}
