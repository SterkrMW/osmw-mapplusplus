#Requires AutoHotkey v2.0

; Character Vendor pricing.
;
; Setting up a player shop means typing a sale price for all 24 inventory slots
; one at a time, into a client input box that renders no thousands separators.
; 1000000 and 100000 look nearly identical there, so mispricing by a factor of
; ten is easy and expensive. This addon reads the vendor price table out of the
; client, shows it as the 6x4 inventory grid with comma-formatted and
; magnitude-coloured prices, and writes the whole block back in one go.
;
; It writes prices ONLY. It never opens the shop — the player clicks Open
; in-game themselves so they can check every price first. There is deliberately
; no ControlClick and no Send anywhere in this file.
;
; ── Memory layout (32-bit main.exe, RVAs from module base) ───────
;
; Two unrelated structures, 1.4 MB apart in the image:
;
;   ShopPriceBase  0x343768  24 x Int32, stride 4  — the vendor price table
;   (count)        0x3437C8  Int32                 — exactly base + 24*4
;   ShopItemIdBase 0x2E2028  Int32, stride 20      — file id is field 0 of a
;                                                    20-byte inventory record
;
; The two strides are different and must never be transposed, hence the
; separate constants and the _ShopPrices_PriceRva/_ShopPrices_ItemIdRva pair.
;
; Prices and the count are contiguous, so the vendor block is one 100-byte read
; and one 100-byte write, and the count is derived and written in the same call
; as the array — no code path can leave the two disagreeing. Item ids are
; strided, so they are one 480-byte read indexed at (i-1)*20; the other 16 bytes
; of each record are unidentified and are never written.
;
; The load-bearing assumption is that the two arrays are POSITIONALLY ALIGNED:
; price index N prices inventory slot N. Nothing structural guarantees that, so
; Tray > Character Vendor > Verify Slot Mapping dumps both side by side.

global _ShopPrices_SLOT_COUNT     := 24
global _ShopPrices_GRID_COLS      := 6
global _ShopPrices_MAX_PRICE      := 99999999
global _ShopPrices_PRICE_STRIDE   := 4     ; packed vendor price array
global _ShopPrices_ITEM_STRIDE    := 20    ; 0x14 — inventory record; id is field 0
; PROCESS_VM_OPERATION | VM_READ | VM_WRITE | QUERY_INFORMATION. The handles
; cached in gClientHandles are read-only, so writes must open their own.
global _ShopPrices_PROCESS_ACCESS := 0x0008 | 0x0010 | 0x0020 | 0x0400

; Config-backed. ItemIdOffset lets a game patch be worked around from
; config.ini without a rebuild; 0 disables every id-dependent feature rather
; than reading garbage from a stale address.
global _ShopPrices_ItemIdOverride := 0
global _ShopPrices_ConfirmHigh    := true
global _ShopPrices_WarnAbove      := 10000000
global _ShopPrices_PresetClears   := false
; 0 = the file id is 0-based, 1 = 1-based. Cannot be auto-detected: item icons
; are named "<n>.<n+1>.png", so both readings resolve to a real file for every
; id except the two boundaries. Settled by one live observation.
global _ShopPrices_IconBase       := 0

RegisterAddonOffset("ShopPriceBase",  0x343768)
RegisterAddonOffset("ShopItemIdBase", 0x2E2028)

RegisterAddon(Map(
    "name",              "ShopPrices",
    "settingsLabel",     "Character Vendor",
    "OnInit",            _ShopPrices_OnInit,
    "OnTrayMenu",        _ShopPrices_OnTrayMenu,
    "OnSnapshot",        _ShopPrices_OnSnapshot,
    "OnSettingsWeb",     _ShopPrices_OnSettingsWeb,
    "OnSettingsWebSave", _ShopPrices_OnSettingsWebSave
))

RegisterHotkeyAction(Map(
    "id", "shopPricesOpen",
    "label", "Open Character Vendor pricing",
    "category", "Character Vendor",
    "default", "^!b",
    "addon", "ShopPrices",
    "handler", (*) => _ShopPrices_Open(),
    "hotIfWinActive", true
))

_ShopPrices_OnInit() {
    _ShopPrices_LoadConfig()
}

_ShopPrices_OnTrayMenu(trayMenu) {
    shopMenu := Menu()
    shopMenu.Add("Pricing Panel…`t" GetHotkeyDisplay("shopPricesOpen"), (*) => _ShopPrices_Open())
    shopMenu.Add()
    shopMenu.Add("Verify Slot Mapping…", (*) => _ShopPrices_ShowSlotDump())
    trayMenu.Add("Character Vendor", shopMenu)
}

; The client dropdown and the live write guard both read gClientSnapshots, and
; UpdateClientSnapshots() skips its whole poll unless some enabled addon
; registers this hook — so registering it is what keeps the list populated.
; Rides the shared client poll rather than adding a second loop of its own.
; While a panel is open this re-reads the pinned client so prices edited in the
; game's shop window appear in the grid, and refreshes the guard so Apply greys
; out within one tick of entering battle or opening the vendor.
_ShopPrices_OnSnapshot(snapshots) {
    global _ShopPrices_PanelMode, _ShopPrices_TargetPid

    if (_ShopPrices_PanelMode = "" || !_ShopPrices_TargetPid)
        return

    changed := false
    dropped := []
    state := _ShopPrices_ReadSlots(_ShopPrices_TargetPid)
    if state.ok {
        res := _ShopPrices_MergeExternal(state)
        changed := res.changed
        dropped := res.dropped
    }

    if (_ShopPrices_PanelMode = "native") {
        if changed
            _ShopPrices_NativeRefresh()      ; updates the status line too
        else
            _ShopPrices_NativeUpdateStatus()
        _ShopPrices_NativeRefreshClients()
        if dropped.Length
            TrayTip(_ShopPrices_DroppedText(dropped), "Character Vendor", "Icon!")
    } else if (_ShopPrices_PanelMode = "web") {
        if changed
            _ShopPrices_SendGrid()
        _ShopPrices_SendGuard()
        if dropped.Length
            _ShopPrices_WebToast("warn", _ShopPrices_DroppedText(dropped))
    }
}

_ShopPrices_DroppedText(dropped) {
    slots := ""
    for s in dropped
        slots .= (slots = "" ? "" : ", ") s
    one := (dropped.Length = 1)
    return (one ? "Slot " : "Slots ") slots
        . (one ? " holds" : " hold") " a different item now, so the unapplied "
        . (one ? "price was" : "prices were") " dropped."
}

; ── Settings tab ─────────────────────────────────────────────────
;
; Declarative, so the WebView2 and native settings windows render the same
; fields from one description.

_ShopPrices_OnSettingsWeb() {
    global _ShopPrices_ConfirmHigh, _ShopPrices_WarnAbove, _ShopPrices_PresetClears
    global _ShopPrices_IconBase, _ShopPrices_MAX_PRICE

    return [
        Map("type", "info", "text",
            "Prices your player shop from a grid of your inventory, with "
            . "thousands separators and colour by magnitude so a misplaced zero "
            . "is obvious.`n`n"
            . "It only writes prices — you still click Vendor in-game yourself, "
            . "so you can check every price first."),
        Map("type", "checkbox", "id", "confirmHigh",
            "label", "Ask before applying a large price",
            "value", _ShopPrices_ConfirmHigh ? true : false),
        Map("type", "number", "id", "warnAbove",
            "label", "Ask when a price reaches:",
            "value", _ShopPrices_WarnAbove, "min", 0, "max", _ShopPrices_MAX_PRICE),
        Map("type", "checkbox", "id", "presetClears",
            "label", "Presets also clear prices on slots they don't mention",
            "value", _ShopPrices_PresetClears ? true : false),
        Map("type", "dropdown", "id", "iconBaseIdx",
            "label", "Item icon numbering:",
            "options", ["Item id is 0-based (id 0 = 000.1.png)",
                        "Item id is 1-based (id 1 = 000.1.png)"],
            "value", _ShopPrices_IconBase),
        Map("type", "info", "text",
            "Icon files are named <n>.<n+1>.png, so both numberings resolve to a "
            . "real file and the right one cannot be detected automatically — "
            . "compare one known item against your inventory and pick the match."),
        Map("type", "combo", "id", "itemIdOffset",
            "label", "Item id array offset (hex) — blank for the built-in:",
            "options", ["0x2E2028"],
            "value", _ShopPrices_ItemIdOffsetText())
    ]
}

_ShopPrices_OnSettingsWebSave(values) {
    global _ShopPrices_ConfirmHigh, _ShopPrices_WarnAbove, _ShopPrices_PresetClears
    global _ShopPrices_IconBase, _ShopPrices_MAX_PRICE, _ShopPrices_ItemIdOverride
    global gFallbackOffsets, CONFIG_INI

    _ShopPrices_ConfirmHigh := values.Has("confirmHigh") && values["confirmHigh"] ? true : false
    _ShopPrices_PresetClears := values.Has("presetClears") && values["presetClears"] ? true : false

    if (values.Has("warnAbove") && IsNumber(values["warnAbove"])) {
        warn := Integer(Round(values["warnAbove"]))
        if (warn >= 0 && warn <= _ShopPrices_MAX_PRICE)
            _ShopPrices_WarnAbove := warn
    }
    if (values.Has("iconBaseIdx") && IsNumber(values["iconBaseIdx"])) {
        idx := Integer(Round(values["iconBaseIdx"]))
        if (idx = 0 || idx = 1)
            _ShopPrices_IconBase := idx
    }

    ; Blank restores the built-in offset; anything unparseable is refused rather
    ; than silently disabling item ids.
    if values.Has("itemIdOffset") {
        raw := Trim(String(values["itemIdOffset"]))
        if (raw = "") {
            _ShopPrices_ItemIdOverride := 0
            gFallbackOffsets["ShopItemIdBase"] := 0x2E2028
            try IniDelete(CONFIG_INI, "ShopPrices", "ItemIdOffset")
        } else {
            val := _ShopPrices_ParseOffset(raw)
            if (val < 0) {
                TrayTip("'" raw "' isn't a hex offset — the built-in one was kept.",
                    "Character Vendor", "Icon!")
            } else {
                _ShopPrices_ItemIdOverride := val
                gFallbackOffsets["ShopItemIdBase"] := val
                IniWrite(Format("0x{:X}", val), CONFIG_INI, "ShopPrices", "ItemIdOffset")
            }
        }
    }

    _ShopPrices_SaveConfig()
}

_ShopPrices_ItemIdOffsetText() {
    global _ShopPrices_ItemIdOverride
    return _ShopPrices_ItemIdOverride ? Format("0x{:X}", _ShopPrices_ItemIdOverride) : ""
}

; ── Config ───────────────────────────────────────────────────────

_ShopPrices_LoadConfig() {
    global CONFIG_INI, gFallbackOffsets
    global _ShopPrices_ItemIdOverride, _ShopPrices_ConfirmHigh, _ShopPrices_WarnAbove
    global _ShopPrices_PresetClears, _ShopPrices_IconBase

    _ShopPrices_ConfirmHigh  := (Trim(IniRead(CONFIG_INI, "ShopPrices", "ConfirmHigh", "1")) != "0")
    _ShopPrices_PresetClears := (Trim(IniRead(CONFIG_INI, "ShopPrices", "PresetClears", "0")) != "0")

    warn := Trim(IniRead(CONFIG_INI, "ShopPrices", "WarnAbove", ""))
    if (IsInteger(warn) && Integer(warn) >= 0)
        _ShopPrices_WarnAbove := Integer(warn)

    iconBase := Trim(IniRead(CONFIG_INI, "ShopPrices", "IconBase", ""))
    if (iconBase = "0" || iconBase = "1")
        _ShopPrices_IconBase := Integer(iconBase)

    ; Accepts 0x2E2028 or plain decimal. An unparseable value is ignored rather
    ; than silently disabling icons, but an explicit 0 does disable them.
    raw := Trim(IniRead(CONFIG_INI, "ShopPrices", "ItemIdOffset", ""))
    if (raw != "") {
        val := _ShopPrices_ParseOffset(raw)
        if (val >= 0) {
            _ShopPrices_ItemIdOverride := val
            gFallbackOffsets["ShopItemIdBase"] := val
        }
    }
}

_ShopPrices_SaveConfig() {
    global CONFIG_INI
    global _ShopPrices_ConfirmHigh, _ShopPrices_WarnAbove, _ShopPrices_PresetClears
    global _ShopPrices_IconBase

    IniWrite(_ShopPrices_ConfirmHigh ? "1" : "0", CONFIG_INI, "ShopPrices", "ConfirmHigh")
    IniWrite(_ShopPrices_PresetClears ? "1" : "0", CONFIG_INI, "ShopPrices", "PresetClears")
    IniWrite(_ShopPrices_WarnAbove, CONFIG_INI, "ShopPrices", "WarnAbove")
    IniWrite(_ShopPrices_IconBase, CONFIG_INI, "ShopPrices", "IconBase")
}

; Returns the parsed RVA, or -1 when the text is not a usable offset.
_ShopPrices_ParseOffset(raw) {
    s := Trim(raw)
    if (s = "")
        return -1
    if (SubStr(s, 1, 2) = "0x" || SubStr(s, 1, 2) = "0X")
        s := SubStr(s, 3)
    if !RegExMatch(s, "^[0-9A-Fa-f]+$")
        return -1
    try val := Integer("0x" s)
    catch
        return -1
    return (val >= 0) ? val : -1
}

; ── Offsets ──────────────────────────────────────────────────────

_ShopPrices_PriceRva(slot) {
    global _ShopPrices_PRICE_STRIDE
    return GetResolvedOffset("ShopPriceBase") + (slot - 1) * _ShopPrices_PRICE_STRIDE
}

_ShopPrices_ItemIdRva(slot) {
    global _ShopPrices_ITEM_STRIDE
    return GetResolvedOffset("ShopItemIdBase") + (slot - 1) * _ShopPrices_ITEM_STRIDE
}

_ShopPrices_CountRva() {
    global _ShopPrices_SLOT_COUNT, _ShopPrices_PRICE_STRIDE
    return GetResolvedOffset("ShopPriceBase") + _ShopPrices_SLOT_COUNT * _ShopPrices_PRICE_STRIDE
}

; Non-zero (typically 256) while the vendor window is open in the client.
; Sits immediately *before* the price array, mirroring the count immediately
; after it, so like the count it is derived from the same base rather than
; registered separately — a fixed delta from a resolved base stays correct.
_ShopPrices_VendorOpenRva() {
    return GetResolvedOffset("ShopPriceBase") - 4
}

; False when the item-id array has been disabled (ItemIdOffset=0 after a patch).
; Everything id-dependent — thumbnails, empty-slot detection, the stale
; inventory guard — is gated on this; prices still work without it.
_ShopPrices_IdsKnown() {
    return GetResolvedOffset("ShopItemIdBase") > 0
}

; ── Pure helpers (shared by both frontends) ──────────────────────

; 1000000 -> "1,000,000". The whole point of the feature, so it is the one
; formatter every surface goes through.
_ShopPrices_FormatPrice(n) {
    if !IsInteger(n)
        return "0"
    n := Integer(n)
    neg := (n < 0)
    s := String(Abs(n))
    len := StrLen(s)
    out := ""
    Loop len {
        out := SubStr(s, len - A_Index + 1, 1) . out
        if (Mod(A_Index, 3) = 0 && A_Index < len)
            out := "," . out
    }
    return (neg ? "-" : "") . out
}

; A short second reading of the same number, shown beside the full one so a
; misplaced zero has to survive two independent renderings to go unnoticed.
_ShopPrices_FormatShort(n) {
    if (!IsInteger(n) || Integer(n) = 0)
        return "—"
    n := Integer(n)
    if (Abs(n) >= 1000000)
        return Format("{:.1f}m", n / 1000000)
    if (Abs(n) >= 1000)
        return Format("{:.0f}k", n / 1000)
    return String(n)
}

; Order-of-magnitude band, 0..6. Colour is bound to digit count because the
; failure being prevented is a misplaced zero: adding or dropping one always
; changes the band, which is pre-attentive in a way a number is not.
; Computed here rather than in JS so the native frontend and the web panel can
; never drift apart. 6 means over the client's cap.
_ShopPrices_MagnitudeBand(n) {
    global _ShopPrices_MAX_PRICE
    if (!IsInteger(n))
        return 0
    n := Integer(n)
    if (n <= 0)
        return 0
    if (n > _ShopPrices_MAX_PRICE)
        return 6
    if (n < 1000)
        return 1
    if (n < 100000)
        return 2
    if (n < 1000000)
        return 3
    if (n < 10000000)
        return 4
    return 5
}

; Accepts "1,234,567", "1.2m", "850k", and blank/dash for "not listed".
; Returns {ok, value, reason}; a rejected value never reaches the draft.
_ShopPrices_NormalizePrice(text) {
    global _ShopPrices_MAX_PRICE

    raw := Trim(text)
    s := StrReplace(StrReplace(StrReplace(raw, ",", ""), " ", ""), "_", "")
    if (s = "" || s = "—" || s = "-")
        return { ok: true, value: 0, reason: "" }

    mult := 1
    last := StrLower(SubStr(s, -1))
    if (last = "m") {
        mult := 1000000
        s := SubStr(s, 1, StrLen(s) - 1)
    } else if (last = "k") {
        mult := 1000
        s := SubStr(s, 1, StrLen(s) - 1)
    }

    if (s = "" || !IsNumber(s))
        return { ok: false, value: 0, reason: "'" raw "' isn't a number." }

    val := Number(s) * mult
    if (val < 0)
        return { ok: false, value: 0, reason: "A price can't be negative." }
    val := Integer(Floor(val + 0.5))   ; 1.25m is legitimate; 1.25 rounds
    if (val > _ShopPrices_MAX_PRICE)
        return { ok: false, value: 0,
                 reason: "The most the client allows is "
                     . _ShopPrices_FormatPrice(_ShopPrices_MAX_PRICE) "." }
    return { ok: true, value: val, reason: "" }
}

; The value written to the count field. An item lists only when it has a
; non-zero price AND this number is right, so it is always derived from the
; same array that is about to be written, never tracked separately.
_ShopPrices_CountListed(prices) {
    n := 0
    for p in prices {
        if (IsInteger(p) && Integer(p) > 0)
            n++
    }
    return n
}

; The empty-slot sentinel is assumed to be 0 and must be confirmed against a
; known-empty slot via Verify Slot Mapping before it is relied on.
_ShopPrices_IsEmptySlot(itemId) {
    return !IsInteger(itemId) || Integer(itemId) = 0 || Integer(itemId) = -1
}

; ── Read layer ───────────────────────────────────────────────────

; Two ReadProcessMemory calls for the whole grid, never 48: one 104-byte read
; covers the vendor-open flag, the packed prices and the count — all three are
; adjacent (flag at base-4, prices at base, count at base+96) — and one
; 480-byte read covers the strided inventory records.
_ShopPrices_ReadSlots(pid) {
    global _ShopPrices_SLOT_COUNT, _ShopPrices_PRICE_STRIDE, _ShopPrices_ITEM_STRIDE
    global _ShopPrices_MAX_PRICE

    proc := GetClientProcess(pid)
    if !proc.ok
        return { ok: false, reason: "That client can't be opened — it may have closed." }

    span := 4 + _ShopPrices_SLOT_COUNT * _ShopPrices_PRICE_STRIDE + 4
    buf := ReadClientBuffer(proc, _ShopPrices_VendorOpenRva(), span)
    if !buf {
        ; Drop the handle so the next attempt reopens rather than reusing a
        ; dead one — same self-heal as UpdateClientSnapshots.
        ReleaseClientProcess(pid)
        return { ok: false, reason: "Could not read the shop price table." }
    }

    ; Everything below is offset by the 4-byte flag the read starts on.
    vendorOpen := NumGet(buf, 0, "Int") != 0
    prices := [], suspect := []
    Loop _ShopPrices_SLOT_COUNT {
        v := NumGet(buf, 4 + (A_Index - 1) * _ShopPrices_PRICE_STRIDE, "Int")
        prices.Push(v)
        ; Surfaced rather than silently corrected: a value outside the client's
        ; own range means the offset is wrong, and rewriting it would hide that.
        suspect.Push((v < 0 || v > _ShopPrices_MAX_PRICE) ? true : false)
    }
    count := NumGet(buf, 4 + _ShopPrices_SLOT_COUNT * _ShopPrices_PRICE_STRIDE, "Int")

    itemIds := []
    idsKnown := false
    if _ShopPrices_IdsKnown() {
        idBuf := ReadClientBuffer(proc, GetResolvedOffset("ShopItemIdBase"),
            _ShopPrices_SLOT_COUNT * _ShopPrices_ITEM_STRIDE)
        if idBuf {
            Loop _ShopPrices_SLOT_COUNT
                itemIds.Push(NumGet(idBuf, (A_Index - 1) * _ShopPrices_ITEM_STRIDE, "Int"))
            idsKnown := true
        }
    }
    if !idsKnown {
        Loop _ShopPrices_SLOT_COUNT
            itemIds.Push(0)
    }

    return { ok: true, reason: "", prices: prices, count: count,
             itemIds: itemIds, idsKnown: idsKnown, suspect: suspect,
             vendorOpen: vendorOpen }
}

; Just the flag, for the once-a-second guard refresh while the panel is open —
; cheaper than re-reading the whole block to answer one question.
;
; Unreadable counts as closed rather than open: it must not be possible for a
; failed read to lock the panel, and a write that really can't go through will
; still fail loudly at the read-back check.
_ShopPrices_ReadVendorOpen(pid) {
    proc := GetClientProcess(pid)
    if !proc.ok
        return false
    buf := ReadClientBuffer(proc, _ShopPrices_VendorOpenRva(), 4)
    if !buf
        return false
    return NumGet(buf, 0, "Int") != 0
}

; ── Diagnostic ───────────────────────────────────────────────────

; Dumps both arrays side by side against the live client. This is how the
; assumptions the whole feature rests on get checked: that the price array is
; 24 long (the count must sit at +96 and not move when slot 24 is priced), that
; the item-id stride really is 20 across all 24 records, that the two arrays are
; positionally aligned, and what an empty slot's file id actually reads as.
_ShopPrices_ShowSlotDump() {
    global _ShopPrices_SLOT_COUNT, _ShopPrices_ITEM_STRIDE, _ShopPrices_IconBase
    global GAME_WIN_FILTER

    hwnd := WinActive(GAME_WIN_FILTER)
    if !hwnd {
        for snap in GetClientSnapshots() {
            hwnd := snap.hwnd
            break
        }
    }
    if !hwnd {
        TrayTip("No game client is running.", "Character Vendor", "Icon!")
        return
    }

    pid := WinGetPID("ahk_id " hwnd)
    charName := CharacterNameFromWindow(hwnd)
    if (charName = "")
        charName := "PID " pid

    state := _ShopPrices_ReadSlots(pid)
    if !state.ok {
        _ShopPrices_Say(state.reason, "Iconx")
        return
    }

    priceBase := GetResolvedOffset("ShopPriceBase")
    idBase    := GetResolvedOffset("ShopItemIdBase")

    out := charName "`n`n"
        . "ShopPriceBase  " Format("0x{:06X}", priceBase) "  stride 4`n"
        . "vendor open at " Format("0x{:06X}", _ShopPrices_VendorOpenRva())
        . "   reads " (state.vendorOpen ? "OPEN" : "closed") "`n"
        . "count      at  " Format("0x{:06X}", _ShopPrices_CountRva())
        . "   reads " state.count "`n"
        . "ShopItemIdBase " Format("0x{:06X}", idBase) "  stride "
        . _ShopPrices_ITEM_STRIDE (state.idsKnown ? "" : "  (unavailable)") "`n`n"
        . "Slot  ItemId  Icon file        Price`n"
        . "----  ------  ---------------  -------------`n"

    Loop _ShopPrices_SLOT_COUNT {
        id := state.itemIds[A_Index]
        icon := (!state.idsKnown || _ShopPrices_IsEmptySlot(id))
            ? "(empty)"
            : _ShopPrices_IconFile(id)
        out .= Format("{:4}  {:6}  {:-15}  {}{}`n",
            A_Index, (state.idsKnown ? id : "?"), icon,
            _ShopPrices_FormatPrice(state.prices[A_Index]),
            state.suspect[A_Index] ? "  <-- out of range" : "")
    }

    listed := _ShopPrices_CountListed(state.prices)
    out .= "`nNon-zero prices: " listed
        . "   count field: " state.count
        . (listed = state.count ? "   (consistent)" : "   <-- MISMATCH")
        . "`n`nCheck against the game: item ids must line up with the inventory"
        . "`nslots in the same order, and the prices must line up with the ids."

    _ShopPrices_Say(out, "Iconi", "Character Vendor — Verify Slot Mapping")
}

; Item icons ship as ui\items\<n>.<n+1>.png, so the filename encodes the id
; twice and a base of 0 or 1 both resolve to a real file for every id except
; the boundaries. That rules out detecting the base by load failure — it is a
; setting, confirmed once by eye against a known item.
_ShopPrices_IconFile(itemId) {
    global _ShopPrices_IconBase
    n := Integer(itemId) - _ShopPrices_IconBase
    if (n < 0)
        return ""
    return Format("{:03}.{}.png", n, n + 1)
}

; ── Target client ────────────────────────────────────────────────
;
; Pinned when the panel opens and changed only by explicit user action. A panel
; that silently followed the focused window would let a distracted multi-boxer
; price one character's inventory into another's shop.

global _ShopPrices_TargetPid  := 0
global _ShopPrices_TargetHwnd := 0

_ShopPrices_PinTarget(pid, hwnd := 0) {
    global _ShopPrices_TargetPid, _ShopPrices_TargetHwnd
    _ShopPrices_TargetPid := pid
    _ShopPrices_TargetHwnd := hwnd
}

_ShopPrices_TargetSnapshot() {
    global _ShopPrices_TargetPid
    if !_ShopPrices_TargetPid
        return 0
    for snap in GetClientSnapshots() {
        if (snap.pid = _ShopPrices_TargetPid)
            return snap
    }
    return 0
}

_ShopPrices_TargetLabel() {
    global _ShopPrices_TargetPid, _ShopPrices_TargetHwnd
    if !_ShopPrices_TargetPid
        return ""
    snap := _ShopPrices_TargetSnapshot()
    if (IsObject(snap) && snap.charName != "")
        return snap.charName
    if _ShopPrices_TargetHwnd {
        name := CharacterNameFromWindow(_ShopPrices_TargetHwnd)
        if (name != "")
            return name
    }
    return "PID " _ShopPrices_TargetPid
}

; ── Draft store ──────────────────────────────────────────────────
;
; Editing mutates the draft only; nothing reaches game memory until Apply. A
; live write per keystroke would push half-typed magnitudes (1, 12, 123…) into
; the count-bearing table, which is the exact mistake this panel exists to
; prevent. Both frontends go through these functions, so "set price on all
; selected" is literally the same code path in the grid and the ListView.

global _ShopPrices_Draft     := []   ; what the user has authored
global _ShopPrices_Live      := []   ; prices as of the last read
global _ShopPrices_LiveIds   := []   ; item ids as of the last read
global _ShopPrices_LiveCount := 0
global _ShopPrices_IdsAvail  := false
global _ShopPrices_Suspect   := []
global _ShopPrices_Rev       := 0    ; bumped on every draft change

; Reads the pinned client and rebases both draft and live onto it, discarding
; any unapplied edits. Returns the read result.
_ShopPrices_LoadFromClient() {
    global _ShopPrices_TargetPid
    if !_ShopPrices_TargetPid
        return { ok: false, reason: "No game client is selected." }
    state := _ShopPrices_ReadSlots(_ShopPrices_TargetPid)
    if state.ok
        _ShopPrices_RebaseLive(state, true)
    return state
}

; Adopts a read as the new "live" baseline. resetDraft also discards edits;
; after a successful apply the draft already equals what was written.
_ShopPrices_RebaseLive(state, resetDraft) {
    global _ShopPrices_Draft, _ShopPrices_Live, _ShopPrices_LiveIds
    global _ShopPrices_LiveCount, _ShopPrices_IdsAvail, _ShopPrices_Suspect, _ShopPrices_Rev

    _ShopPrices_Live      := state.prices.Clone()
    _ShopPrices_LiveIds   := state.itemIds.Clone()
    _ShopPrices_LiveCount := state.count
    _ShopPrices_IdsAvail  := state.idsKnown
    _ShopPrices_Suspect   := state.suspect.Clone()
    if resetDraft
        _ShopPrices_Draft := state.prices.Clone()
    _ShopPrices_Rev++
}

; Folds a fresh read into the panel without discarding unapplied edits, so a
; price the player types into the game's own shop window shows up here. The
; intended workflow is to set a base price across similar items from the grid
; and then fine-tune individual ones in game.
;
; Per slot:
;   untouched (draft still equals what we last read) — follow the client
;   edited                                            — keep the edit, but
;     rebase onto the new client value so the struck-through "was" figure
;     stays truthful and the slot stays marked dirty
;
; The exception is an edited slot whose *item* changed. That price was authored
; for something that is no longer there, so it is dropped rather than left
; pointing at whatever moved in. Without this, continuously refreshing
; LiveIds would quietly defeat the stale-inventory guard in ApplyPrices.
;
; Returns {changed, dropped} — dropped is the list of slots whose pending edit
; was discarded.
_ShopPrices_MergeExternal(state) {
    global _ShopPrices_Draft, _ShopPrices_Live, _ShopPrices_LiveIds
    global _ShopPrices_LiveCount, _ShopPrices_IdsAvail, _ShopPrices_Suspect
    global _ShopPrices_SLOT_COUNT, _ShopPrices_Rev

    ; Nothing coherent to merge against yet — adopt wholesale.
    if (_ShopPrices_Draft.Length != _ShopPrices_SLOT_COUNT
        || _ShopPrices_Live.Length != _ShopPrices_SLOT_COUNT
        || _ShopPrices_LiveIds.Length != _ShopPrices_SLOT_COUNT) {
        _ShopPrices_RebaseLive(state, true)
        return { changed: true, dropped: [] }
    }

    changed := false
    dropped := []
    canCompareIds := (_ShopPrices_IdsAvail && state.idsKnown)

    Loop _ShopPrices_SLOT_COUNT {
        newPrice := state.prices[A_Index]
        oldLive  := _ShopPrices_Live[A_Index]
        wasDirty := (_ShopPrices_Draft[A_Index] != oldLive)
        idChanged := canCompareIds && (_ShopPrices_LiveIds[A_Index] != state.itemIds[A_Index])

        if (newPrice != oldLive || idChanged)
            changed := true

        if (wasDirty && idChanged) {
            _ShopPrices_Draft[A_Index] := newPrice
            dropped.Push(A_Index)
            continue
        }
        if (!wasDirty && _ShopPrices_Draft[A_Index] != newPrice) {
            _ShopPrices_Draft[A_Index] := newPrice
            changed := true
        }
    }

    if (_ShopPrices_LiveCount != state.count || _ShopPrices_IdsAvail != state.idsKnown)
        changed := true

    _ShopPrices_Live      := state.prices.Clone()
    _ShopPrices_LiveIds   := state.itemIds.Clone()
    _ShopPrices_LiveCount := state.count
    _ShopPrices_IdsAvail  := state.idsKnown
    _ShopPrices_Suspect   := state.suspect.Clone()
    if changed
        _ShopPrices_Rev++
    return { changed: changed, dropped: dropped }
}

_ShopPrices_DraftGet() {
    global _ShopPrices_Draft
    return _ShopPrices_Draft
}

_ShopPrices_DraftSet(slot, value) {
    global _ShopPrices_Draft, _ShopPrices_SLOT_COUNT, _ShopPrices_Rev
    if (!IsInteger(slot) || slot < 1 || slot > _ShopPrices_SLOT_COUNT)
        return false
    if (_ShopPrices_Draft.Length < slot)
        return false
    _ShopPrices_Draft[slot] := Integer(value)
    _ShopPrices_Rev++
    return true
}

_ShopPrices_DraftSetMany(slots, value) {
    n := 0
    for slot in slots {
        if _ShopPrices_DraftSet(slot, value)
            n++
    }
    return n
}

_ShopPrices_DraftReset() {
    global _ShopPrices_Draft, _ShopPrices_Live, _ShopPrices_Rev
    _ShopPrices_Draft := _ShopPrices_Live.Clone()
    _ShopPrices_Rev++
}

_ShopPrices_DraftIsDirty() {
    global _ShopPrices_Draft, _ShopPrices_Live
    if (_ShopPrices_Draft.Length != _ShopPrices_Live.Length)
        return true
    for i, v in _ShopPrices_Draft {
        if (v != _ShopPrices_Live[i])
            return true
    }
    return false
}

_ShopPrices_DirtyCount() {
    global _ShopPrices_Draft, _ShopPrices_Live
    n := 0
    if (_ShopPrices_Draft.Length != _ShopPrices_Live.Length)
        return _ShopPrices_Draft.Length
    for i, v in _ShopPrices_Draft {
        if (v != _ShopPrices_Live[i])
            n++
    }
    return n
}

; ── Write guards ─────────────────────────────────────────────────

; Cheap, snapshot-only checks — safe to call on every poll to drive the panel's
; guard badge. The stale-inventory check needs a fresh read and lives in
; _ShopPrices_ApplyPrices instead. Returns "" or the reason to show the user.
_ShopPrices_CheckWritable() {
    global _ShopPrices_TargetPid, GAME_STATE_READY

    if !_ShopPrices_TargetPid
        return "No game client is selected."

    snap := _ShopPrices_TargetSnapshot()
    if !IsObject(snap) {
        ; The shared poll runs once a second and may not have covered this
        ; client yet, so a missing snapshot alone does not mean it has gone.
        if !ProcessExist(_ShopPrices_TargetPid)
            return "The client you were editing has closed."
        return ""
    }
    ; gameState -1 means the read failed, which is not itself a reason to block.
    if (snap.gameState >= 0 && snap.gameState < GAME_STATE_READY)
        return "That client isn't logged in yet."
    if snap.inBattle
        return "That client is in battle — prices can't be set right now."
    ; The client owns the price table while the vendor window is up. Writing
    ; underneath it would leave what is on screen disagreeing with what is
    ; stored, which is the one state the listing rule punishes.
    if _ShopPrices_ReadVendorOpen(_ShopPrices_TargetPid)
        return "Your vendor is open — close it before changing prices."
    return ""
}

; ── Write layer ──────────────────────────────────────────────────

; The handles cached in gClientHandles are opened read-only, so writing needs
; its own. Never cached: a stale read/write handle on a dead pid is the failure
; ReleaseClientProcess exists to avoid, and applies are rare.
_ShopPrices_OpenWrite(pid) {
    global _ShopPrices_PROCESS_ACCESS, PROCESS_EXE

    handle := DllCall("OpenProcess", "UInt", _ShopPrices_PROCESS_ACCESS,
        "Int", 0, "UInt", pid, "Ptr")
    if !handle {
        return { ok: false, reason: "Could not open the game client for writing (error "
            . A_LastError "). If the game runs as administrator, Maps++ must too." }
    }
    modBase := GetModuleBaseAddress(handle, PROCESS_EXE)
    if !modBase {
        DllCall("CloseHandle", "Ptr", handle)
        return { ok: false, reason: "Could not locate main.exe inside that client." }
    }
    return { ok: true, handle: handle, modBase: modBase, reason: "" }
}

_ShopPrices_CloseWrite(w) {
    if (IsObject(w) && w.ok)
        try DllCall("CloseHandle", "Ptr", w.handle)
}

; One 100-byte write covering the price array and the count together, so the
; count is consistent with the array by construction — there is no window in
; which seven prices are set but the count still says five.
;
; Unlike the other writers in this codebase, the return value is checked. A
; partial write here leaves prices disagreeing with the count, which is exactly
; the state the game's listing rule punishes.
;
; Note the asymmetry with _ShopPrices_ReadSlots: the read starts 4 bytes
; earlier and spans 104 to pick up the vendor-open flag, whereas the write
; starts at the price base and spans 100. The flag belongs to the client and is
; never written.
_ShopPrices_WriteSlots(w, prices) {
    global _ShopPrices_SLOT_COUNT, _ShopPrices_PRICE_STRIDE

    span := _ShopPrices_SLOT_COUNT * _ShopPrices_PRICE_STRIDE + 4
    buf := Buffer(span, 0)
    Loop _ShopPrices_SLOT_COUNT
        NumPut("Int", prices[A_Index], buf, (A_Index - 1) * _ShopPrices_PRICE_STRIDE)
    NumPut("Int", _ShopPrices_CountListed(prices), buf,
        _ShopPrices_SLOT_COUNT * _ShopPrices_PRICE_STRIDE)

    written := 0
    ok := DllCall("WriteProcessMemory",
        "Ptr", w.handle,
        "Ptr", w.modBase + GetResolvedOffset("ShopPriceBase"),
        "Ptr", buf.Ptr,
        "UPtr", span,
        "UPtr*", &written,
        "Int")
    if !ok
        return { ok: false, written: 0, reason: "The client rejected the write (error " A_LastError ")." }
    if (written != span)
        return { ok: false, written: written,
            reason: "Only " written " of " span " bytes were written." }
    return { ok: true, written: written, reason: "" }
}

; Guard, pre-read, write, read back, verify. Returns
; {ok, reason, listed, written, mismatches}.
_ShopPrices_ApplyPrices() {
    static busy := false
    global _ShopPrices_TargetPid, _ShopPrices_Draft, _ShopPrices_LiveIds
    global _ShopPrices_SLOT_COUNT, _ShopPrices_IdsAvail

    if busy
        return { ok: false, reason: "An apply is already running.", listed: 0, mismatches: [] }
    busy := true
    w := 0
    try {
        reason := _ShopPrices_CheckWritable()
        if (reason != "")
            return { ok: false, reason: reason, listed: 0, mismatches: [] }

        pid := _ShopPrices_TargetPid
        prices := _ShopPrices_Draft.Clone()
        if (prices.Length != _ShopPrices_SLOT_COUNT)
            return { ok: false, reason: "The grid hasn't been read yet.", listed: 0, mismatches: [] }

        ; The draft was authored against a snapshot. If the inventory has moved
        ; since, slot N no longer holds the item the user priced, so refuse
        ; rather than pricing whatever landed there. This narrows the
        ; read-modify-write window; it cannot close it.
        before := _ShopPrices_ReadSlots(pid)
        if !before.ok
            return { ok: false, reason: before.reason, listed: 0, mismatches: [] }
        ; Re-checked off the pre-read rather than trusting the guard from a
        ; second ago, which narrows the window in which the vendor could have
        ; been opened between the two.
        if before.vendorOpen {
            return { ok: false, listed: 0, mismatches: [],
                reason: "Your vendor is open — close it and try again." }
        }
        if (before.idsKnown && _ShopPrices_IdsAvail) {
            Loop _ShopPrices_SLOT_COUNT {
                if (before.itemIds[A_Index] != _ShopPrices_LiveIds[A_Index]) {
                    return { ok: false, listed: 0, mismatches: [],
                        reason: "Your inventory changed since this grid was read (slot "
                            . A_Index " now holds something else). Refresh and check your prices." }
                }
            }
        }

        w := _ShopPrices_OpenWrite(pid)
        if !w.ok
            return { ok: false, reason: w.reason, listed: 0, mismatches: [] }

        res := _ShopPrices_WriteSlots(w, prices)
        if !res.ok
            return { ok: false, reason: res.reason, listed: 0, mismatches: [] }

        check := _ShopPrices_ReadSlots(pid)
        if !check.ok {
            return { ok: false, listed: 0, mismatches: [],
                reason: "The prices were written but could not be read back to confirm. "
                    . "Check them in-game before you open Vendor." }
        }

        expectCount := _ShopPrices_CountListed(prices)
        mismatches := []
        Loop _ShopPrices_SLOT_COUNT {
            if (check.prices[A_Index] != prices[A_Index]) {
                mismatches.Push("Slot " A_Index " reads "
                    . _ShopPrices_FormatPrice(check.prices[A_Index]) ", expected "
                    . _ShopPrices_FormatPrice(prices[A_Index]))
            }
        }
        if (check.count != expectCount)
            mismatches.Push("The items-for-sale count reads " check.count ", expected " expectCount)

        if mismatches.Length {
            detail := ""
            shown := 0
            for m in mismatches {
                if (shown >= 5)
                    break
                detail .= (detail = "" ? "" : "`n") m
                shown++
            }
            if (mismatches.Length > shown)
                detail .= "`n… and " (mismatches.Length - shown) " more"
            return { ok: false, listed: 0, mismatches: mismatches,
                reason: "The client did not accept every price, so nothing is guaranteed "
                    . "to be listed — check in-game before opening Vendor.`n`n" detail }
        }

        _ShopPrices_RebaseLive(check, true)
        return { ok: true, reason: "", listed: expectCount,
                 written: res.written, mismatches: [] }
    } finally {
        _ShopPrices_CloseWrite(w)
        busy := false
    }
}

; ── Preset store ─────────────────────────────────────────────────
;
; shop_presets.ini sits next to config.ini — never in maps\, which build.ps1
; copies into releases\ and would clobber. Same two-level shape as layouts.ini:
; an [Index] mapping id to display name, and one [Preset.<id>] per preset, so
; renaming never touches price data.
;
;   [Index]
;   1=Weekend Prices
;
;   [Preset.1]
;   Saved=20260803143012
;   Char=Sterkr
;   Slot1=1000000|37
;
; Slot<N>=price|itemIdAtSaveTime. Presets are keyed by SLOT INDEX; the recorded
; item id is advisory only, used to warn that a slot now holds something else.
; Slots the preset says nothing about are absent, so a preset is additive by
; default and never silently unlists anything.

global _ShopPresets_Ini   := ""
global _ShopPresets_Cache := Map()
global _ShopPresets_Index := 0

_ShopPresets_Path() {
    global _ShopPresets_Ini
    if (_ShopPresets_Ini = "")
        _ShopPresets_Ini := ResolveWritableIniPath("shop_presets.ini")
    return _ShopPresets_Ini
}

_ShopPresets_InvalidateCache() {
    global _ShopPresets_Cache, _ShopPresets_Index
    _ShopPresets_Cache := Map()
    _ShopPresets_Index := 0
}

; [{id, name}] ordered by id. Cached until a write.
_ShopPresets_LoadIndex() {
    global _ShopPresets_Index
    if IsObject(_ShopPresets_Index)
        return _ShopPresets_Index

    list := []
    path := _ShopPresets_Path()
    if FileExist(path) {
        try section := IniRead(path, "Index")
        catch
            section := ""
        loop parse, section, "`n", "`r" {
            line := Trim(A_LoopField)
            eq := InStr(line, "=")
            if (line = "" || !eq)
                continue
            id := Trim(SubStr(line, 1, eq - 1))
            name := Trim(SubStr(line, eq + 1))
            if (!IsInteger(id) || Integer(id) < 1 || name = "")
                continue
            list.Push({id: Integer(id), name: name})
        }
    }
    ; Numeric order, so the list matches creation order. A clean install has no
    ; file yet, and Loop -1 throws, so guard the insertion sort.
    if (list.Length > 1) {
        Loop list.Length - 1 {
            i := A_Index + 1
            item := list[i]
            j := i - 1
            while (j >= 1 && list[j].id > item.id) {
                list[j + 1] := list[j]
                j -= 1
            }
            list[j + 1] := item
        }
    }
    _ShopPresets_Index := list
    return list
}

_ShopPresets_NameForId(id) {
    for entry in _ShopPresets_LoadIndex() {
        if (entry.id = id)
            return entry.name
    }
    return ""
}

; A preset object, or 0 when the id is unknown. A malformed slot line is
; skipped rather than failing the whole preset.
_ShopPresets_Load(id) {
    global _ShopPresets_Cache, _ShopPrices_SLOT_COUNT, _ShopPrices_MAX_PRICE
    if (!IsInteger(id) || id < 1)
        return 0
    id := Integer(id)
    if _ShopPresets_Cache.Has(id)
        return _ShopPresets_Cache[id]

    name := _ShopPresets_NameForId(id)
    if (name = "")
        return 0

    preset := {id: id, name: name, savedBy: "", saved: "", entries: Map()}
    try section := IniRead(_ShopPresets_Path(), "Preset." id)
    catch
        section := ""

    loop parse, section, "`n", "`r" {
        line := Trim(A_LoopField)
        eq := InStr(line, "=")
        if (line = "" || !eq)
            continue
        key := Trim(SubStr(line, 1, eq - 1))
        value := Trim(SubStr(line, eq + 1))

        if (key = "Char") {
            preset.savedBy := value
            continue
        }
        if (key = "Saved") {
            preset.saved := value
            continue
        }
        if (SubStr(key, 1, 4) != "Slot")
            continue

        slot := SubStr(key, 5)
        if (!IsInteger(slot) || Integer(slot) < 1 || Integer(slot) > _ShopPrices_SLOT_COUNT)
            continue
        parts := StrSplit(value, "|")
        if (parts.Length < 1 || !IsInteger(Trim(parts[1])))
            continue
        price := Integer(Trim(parts[1]))
        if (price < 0 || price > _ShopPrices_MAX_PRICE)
            continue
        itemId := 0
        if (parts.Length >= 2 && IsInteger(Trim(parts[2])))
            itemId := Integer(Trim(parts[2]))
        preset.entries[Integer(slot)] := {price: price, itemId: itemId}
    }

    _ShopPresets_Cache[id] := preset
    return preset
}

_ShopPresets_LoadByName(name) {
    for entry in _ShopPresets_LoadIndex() {
        if (StrLower(entry.name) = StrLower(Trim(name)))
            return _ShopPresets_Load(entry.id)
    }
    return 0
}

_ShopPresets_NextId() {
    maxId := 0
    for entry in _ShopPresets_LoadIndex() {
        if (entry.id > maxId)
            maxId := entry.id
    }
    return maxId + 1
}

; `|` is the field delimiter and `[`/`]` would break the section header, so both
; are removed from user text here rather than being escaped on every write.
_ShopPresets_SanitizeName(name) {
    s := Trim(name)
    s := StrReplace(StrReplace(StrReplace(s, "`r", " "), "`n", " "), "`t", " ")
    s := StrReplace(s, "|", " ")
    s := StrReplace(StrReplace(s, "[", "("), "]", ")")
    s := StrReplace(s, "=", " ")
    while InStr(s, "  ")
        s := StrReplace(s, "  ", " ")
    s := Trim(s)
    if (StrLen(s) > 48)
        s := Trim(SubStr(s, 1, 48))
    return s
}

; "" when the name is usable, otherwise the reason.
_ShopPresets_ValidateName(name, exceptId := 0) {
    s := _ShopPresets_SanitizeName(name)
    if (s = "")
        return "Give the preset a name."
    for entry in _ShopPresets_LoadIndex() {
        if (entry.id != exceptId && StrLower(entry.name) = StrLower(s))
            return "A preset called '" s "' already exists."
    }
    return ""
}

_ShopPresets_UniqueName(base) {
    s := _ShopPresets_SanitizeName(base)
    if (s = "")
        s := "Shop prices"
    if (_ShopPresets_ValidateName(s) = "")
        return s
    n := 2
    while (n < 100) {
        candidate := s " (" n ")"
        if (_ShopPresets_ValidateName(candidate) = "")
            return candidate
        n++
    }
    return s " (" A_Now ")"
}

; Builds a preset from the current draft. Only priced slots are stored — an
; absent slot means "this preset says nothing about that one".
_ShopPrices_PresetFromDraft(name) {
    global _ShopPrices_Draft, _ShopPrices_LiveIds, _ShopPrices_SLOT_COUNT, _ShopPrices_IdsAvail

    preset := {id: 0, name: _ShopPresets_SanitizeName(name),
               savedBy: _ShopPrices_TargetLabel(), saved: A_Now, entries: Map()}
    Loop _ShopPrices_SLOT_COUNT {
        price := (_ShopPrices_Draft.Length >= A_Index) ? _ShopPrices_Draft[A_Index] : 0
        if (!IsInteger(price) || price <= 0)
            continue
        itemId := (_ShopPrices_IdsAvail && _ShopPrices_LiveIds.Length >= A_Index)
            ? _ShopPrices_LiveIds[A_Index] : 0
        preset.entries[A_Index] := {price: price, itemId: itemId}
    }
    return preset
}

; Returns the id, or 0 on failure. The section is deleted before rewriting so
; slots dropped since the last save actually disappear.
_ShopPresets_Save(preset) {
    path := _ShopPresets_Path()
    id := (preset.HasProp("id") && IsInteger(preset.id) && preset.id >= 1) ? Integer(preset.id) : 0
    if (id = 0)
        id := _ShopPresets_NextId()
    try {
        IniWrite(preset.name, path, "Index", String(id))
        try IniDelete(path, "Preset." id)
        IniWrite(preset.saved, path, "Preset." id, "Saved")
        IniWrite(StrReplace(preset.savedBy, "|", " "), path, "Preset." id, "Char")
        for slot, entry in preset.entries
            IniWrite(entry.price "|" entry.itemId, path, "Preset." id, "Slot" slot)
    } catch as err {
        TrayTip("Could not write shop_presets.ini: " err.Message, "Character Vendor", "Iconx")
        return 0
    }
    preset.id := id
    _ShopPresets_InvalidateCache()
    return id
}

_ShopPresets_Delete(id) {
    if (!IsInteger(id) || id < 1)
        return false
    path := _ShopPresets_Path()
    try {
        IniDelete(path, "Preset." id)
        IniDelete(path, "Index", String(id))
    } catch as err {
        TrayTip("Could not update shop_presets.ini: " err.Message, "Character Vendor", "Iconx")
        return false
    }
    _ShopPresets_InvalidateCache()
    return true
}

; "" on success, otherwise the reason. Touches only [Index].
_ShopPresets_Rename(id, name) {
    reason := _ShopPresets_ValidateName(name, id)
    if (reason != "")
        return reason
    if (_ShopPresets_NameForId(id) = "")
        return "That preset no longer exists."
    try IniWrite(_ShopPresets_SanitizeName(name), _ShopPresets_Path(), "Index", String(id))
    catch as err
        return "Could not write shop_presets.ini: " err.Message
    _ShopPresets_InvalidateCache()
    return ""
}

_ShopPresets_Duplicate(id) {
    preset := _ShopPresets_Load(id)
    if !IsObject(preset)
        return 0
    copy := {id: 0, name: _ShopPresets_UniqueName(preset.name),
             savedBy: preset.savedBy, saved: A_Now, entries: preset.entries.Clone()}
    return _ShopPresets_Save(copy)
}

; ── Preset diff ──────────────────────────────────────────────────

; Compares a preset against the current draft and the live inventory. Presets
; are slot-keyed, so an inventory reorder silently repoints every price — this
; is what makes that visible instead of writing blind.
;
; Rows: {slot, action, from, to, itemId, presetItemId, warn}
;   change/set  the slot takes a new price
;   clear       priced now, preset silent (only when PresetClears is on)
;   skip        the slot is empty; pricing it would inflate the count field
_ShopPrices_DiffPreset(id) {
    global _ShopPrices_SLOT_COUNT, _ShopPrices_Draft, _ShopPrices_LiveIds
    global _ShopPrices_IdsAvail, _ShopPrices_PresetClears

    preset := _ShopPresets_Load(id)
    if !IsObject(preset)
        return { ok: false, reason: "That preset no longer exists.", rows: [] }

    rows := [], warnCount := 0, noopCount := 0
    Loop _ShopPrices_SLOT_COUNT {
        slot := A_Index
        from := (_ShopPrices_Draft.Length >= slot) ? _ShopPrices_Draft[slot] : 0
        itemId := (_ShopPrices_LiveIds.Length >= slot) ? _ShopPrices_LiveIds[slot] : 0

        if !preset.entries.Has(slot) {
            ; Additive by default: a preset that says nothing about a slot
            ; leaves it alone unless the user has opted into clearing.
            if (_ShopPrices_PresetClears && from > 0) {
                rows.Push({slot: slot, action: "clear", from: from, to: 0,
                           itemId: itemId, presetItemId: 0, warn: ""})
            }
            continue
        }

        entry := preset.entries[slot]
        to := entry.price

        if (_ShopPrices_IdsAvail && _ShopPrices_IsEmptySlot(itemId)) {
            rows.Push({slot: slot, action: "skip", from: from, to: to,
                       itemId: itemId, presetItemId: entry.itemId, warn: "empty-slot"})
            warnCount++
            continue
        }
        if (from = to) {
            noopCount++
            continue
        }

        warn := ""
        if (_ShopPrices_IdsAvail && entry.itemId != 0 && entry.itemId != itemId) {
            warn := "different-item"
            warnCount++
        }
        rows.Push({slot: slot, action: (from = 0 ? "set" : "change"), from: from, to: to,
                   itemId: itemId, presetItemId: entry.itemId, warn: warn})
    }

    return { ok: true, reason: "", rows: rows, warnCount: warnCount,
             noopCount: noopCount, name: preset.name, id: preset.id }
}

; Applies a diff to the DRAFT only — never to memory. The user still has to
; press Apply, so there are two deliberate steps between a preset and the game.
_ShopPrices_ApplyPresetToDraft(id) {
    diff := _ShopPrices_DiffPreset(id)
    if !diff.ok
        return diff
    changed := 0
    for row in diff.rows {
        if (row.action = "skip")
            continue
        if _ShopPrices_DraftSet(row.slot, row.to)
            changed++
    }
    return { ok: true, reason: "", changed: changed, rows: diff.rows,
             warnCount: diff.warnCount, name: diff.name }
}

; ── Dialogs ──────────────────────────────────────────────────────
;
; MsgBox and InputBox are unowned top-level windows, so against the panel's
; +AlwaysOnTop they open *behind* it and look like a freeze. +OwnDialogs makes
; every dialog raised later in the same thread owned by — and modal to — the
; panel, which is the documented fix.
;
; It only lasts for the current thread, so it has to be set again before each
; dialog. That is why every dialog in this addon goes through these three
; helpers instead of calling MsgBox/InputBox directly: one place to get right,
; and a new dialog cannot quietly reintroduce the bug.

_ShopPrices_OwnDialogs() {
    global _ShopPrices_PanelMode, _ShopPrices_WebGui, _ShopPrices_NativeGui

    g := 0
    if (_ShopPrices_PanelMode = "web")
        g := _ShopPrices_WebGui
    else if (_ShopPrices_PanelMode = "native")
        g := _ShopPrices_NativeGui
    ; No panel open (a tray-triggered dialog) means there is nothing to own it,
    ; which is correct rather than a failure.
    if (IsObject(g) && g.Hwnd)
        try g.Opt("+OwnDialogs")
}

_ShopPrices_Say(text, icon := "Iconi", title := "Character Vendor") {
    _ShopPrices_OwnDialogs()
    return MsgBox(text, title, icon)
}

; Default2 = No, so the destructive answer is never one stray Enter away.
_ShopPrices_Ask(text, icon := "Icon!", title := "Character Vendor") {
    _ShopPrices_OwnDialogs()
    return MsgBox(text, title, "YesNo Default2 " icon) = "Yes"
}

_ShopPrices_Prompt(text, default) {
    _ShopPrices_OwnDialogs()
    return InputBox(text, "Character Vendor", "w320 h130", default)
}

; ── Shared frontend helpers ──────────────────────────────────────

; Asked before applying, in AHK rather than in either frontend, so the
; safeguard cannot exist in the web panel and be missing from the native one.
_ShopPrices_ConfirmHighPrice(prices) {
    global _ShopPrices_ConfirmHigh, _ShopPrices_WarnAbove
    if !_ShopPrices_ConfirmHigh
        return true
    top := 0
    for p in prices {
        if (IsInteger(p) && p > top)
            top := p
    }
    if (top < _ShopPrices_WarnAbove)
        return true
    return _ShopPrices_Ask("The highest price you are about to set is "
        . _ShopPrices_FormatPrice(top) ".`n`nApply these prices?")
}

; A diff rendered as text, used by the native preview dialog. The web overlay
; renders the same rows as a table.
_ShopPrices_DiffText(diff) {
    if !diff.rows.Length
        return "This preset would not change anything in the grid."

    out := "Slot  Now              Preset`n"
         . "----  ---------------  ---------------`n"
    for row in diff.rows {
        note := ""
        if (row.action = "skip")
            note := "   skipped — slot is empty"
        else if (row.action = "clear")
            note := "   clears this slot"
        else if (row.warn = "different-item")
            note := "   different item than when saved"
        out .= Format("{:4}  {:-15}  {:-15}{}`n", row.slot,
            _ShopPrices_FormatPrice(row.from),
            _ShopPrices_FormatPrice(row.to), note)
    }
    out .= "`n" diff.rows.Length " change"
        . (diff.rows.Length = 1 ? "" : "s")
    if diff.warnCount
        out .= ", " diff.warnCount " warning" (diff.warnCount = 1 ? "" : "s")
    if diff.noopCount
        out .= " (" diff.noopCount " already match)"
    out .= ".`n`nThis only fills in the grid — you still have to press Apply."
    return out
}

; Which client the panel binds to when it opens: the focused one, else the
; first the poll knows about, else the first game window.
_ShopPrices_ResolveOpenTarget() {
    global GAME_WIN_FILTER

    hwnd := WinActive(GAME_WIN_FILTER)
    if hwnd {
        try return { ok: true, pid: WinGetPID("ahk_id " hwnd), hwnd: hwnd, reason: "" }
    }
    ; Poll now rather than binding to a stale list.
    UpdateClientSnapshots()
    for snap in GetClientSnapshots()
        return { ok: true, pid: snap.pid, hwnd: snap.hwnd, reason: "" }
    for w in GetTopLevelGameWindows() {
        try return { ok: true, pid: WinGetPID("ahk_id " w), hwnd: w, reason: "" }
    }
    return { ok: false, pid: 0, hwnd: 0, reason: "No game client is running." }
}

; ── Native frontend ──────────────────────────────────────────────
;
; The low-memory counterpart to the web grid. It renders differently but calls
; exactly the same read/validate/format/write/preset functions, so behaviour
; cannot drift between the two.

global _ShopPrices_PanelMode      := ""   ; "" | "native" | "web"
global _ShopPrices_NativeGui      := 0
global _ShopPrices_NativeList     := 0
global _ShopPrices_NativeClientDdl := 0
global _ShopPrices_NativePriceEdit := 0
global _ShopPrices_NativePresetDdl := 0
global _ShopPrices_NativeStatus   := 0
global _ShopPrices_NativeClientPids := []
global _ShopPrices_NativePresetIds  := []

_ShopPrices_CanUseWebView() {
    ; _Settings_CanUseWebView() hardcodes the settings page, so this panel needs
    ; its own gate against its own files.
    return FileExist(A_ScriptDir "\Lib\WebViewToo.ahk")
        && FileExist(A_ScriptDir "\ui\shop_prices\index.html")
}

_ShopPrices_ShowPanelNative() {
    global _ShopPrices_NativeGui, _ShopPrices_PanelMode

    _ShopPrices_NativeEnsureGui()
    _ShopPrices_PanelMode := "native"
    _ShopPrices_NativeRefreshClients()
    _ShopPrices_NativeRefreshPresets()
    _ShopPrices_NativeRefresh()
    _ShopPrices_NativeGui.Show("AutoSize Center")
}

_ShopPrices_NativeEnsureGui() {
    global _ShopPrices_NativeGui, _ShopPrices_NativeList, _ShopPrices_NativeClientDdl
    global _ShopPrices_NativePriceEdit, _ShopPrices_NativePresetDdl, _ShopPrices_NativeStatus
    global _ShopPrices_SLOT_COUNT

    if (IsObject(_ShopPrices_NativeGui) && _ShopPrices_NativeGui.Hwnd)
        return

    g := Gui("+AlwaysOnTop -MaximizeBox", "Maps++ — Character Vendor")
    g.MarginX := 10
    g.MarginY := 10
    g.SetFont("s9", "Segoe UI")

    g.Add("Text", "x10 y13 w44", "Client:")
    _ShopPrices_NativeClientDdl := g.Add("DropDownList", "x58 y10 w220 vShopClient")
    _ShopPrices_NativeClientDdl.OnEvent("Change", (*) => _ShopPrices_NativeOnClientChange())
    _ShopPrices_NativeStatus := g.Add("Text", "x288 y13 w300", "")

    ; Multi-select is the default; -Multi would defeat the bulk pricing this
    ; panel exists for. Rows are always the 24 slots in order, so a row index
    ; is the slot number and no parallel identity array is needed.
    _ShopPrices_NativeList := g.Add("ListView", "x10 y36 w578 r14 Grid NoSortHdr",
        ["Slot", "Item", "Price", "≈", "Listed"])
    _ShopPrices_NativeList.OnEvent("DoubleClick", _ShopPrices_NativeOnRowDouble)

    g.Add("Text", "x10 y+12 w40", "Price:")
    _ShopPrices_NativePriceEdit := g.Add("Edit", "x58 yp-3 w140")
    g.Add("Button", "x+8 yp-1 w120", "Set on selected")
        .OnEvent("Click", (*) => _ShopPrices_NativeSetOnSelected())
    g.Add("Button", "x+6 yp w110", "Clear selected")
        .OnEvent("Click", (*) => _ShopPrices_NativeClearSelected())
    g.Add("Button", "x+6 yp w80", "Select all")
        .OnEvent("Click", (*) => _ShopPrices_NativeSelectAll())

    g.Add("Text", "x10 y+12 w40", "Preset:")
    _ShopPrices_NativePresetDdl := g.Add("DropDownList", "x58 yp-3 w200 vShopPreset")
    g.Add("Button", "x+8 yp-1 w110", "Save current…")
        .OnEvent("Click", (*) => _ShopPrices_NativeSavePreset())
    g.Add("Button", "x+6 yp w130", "Preview && apply…")
        .OnEvent("Click", (*) => _ShopPrices_NativeApplyPreset())
    g.Add("Button", "x+6 yp w70", "Delete")
        .OnEvent("Click", (*) => _ShopPrices_NativeDeletePreset())

    g.Add("Button", "x10 y+14 w130 Default", "Apply to game")
        .OnEvent("Click", (*) => _ShopPrices_NativeApply())
    g.Add("Button", "x+8 yp w90", "Revert")
        .OnEvent("Click", (*) => _ShopPrices_NativeRevert())
    g.Add("Button", "x+8 yp w90", "Refresh")
        .OnEvent("Click", (*) => _ShopPrices_NativeReload())
    g.Add("Text", "x+12 yp+4 w240 vShopHint",
        "Prices are written to your client — click Vendor in-game yourself.")

    g.OnEvent("Close", (*) => _ShopPrices_NativeClose())
    g.OnEvent("Escape", (*) => _ShopPrices_NativeClose())
    _ShopPrices_NativeGui := g
}

; A Close/Escape handler that returns non-zero cancels the close, so answering
; "No" to the discard prompt has to return 1 — otherwise the window would hide
; anyway and the confirmation would be decorative.
_ShopPrices_NativeClose() {
    global _ShopPrices_NativeGui, _ShopPrices_PanelMode

    if _ShopPrices_DraftIsDirty() {
        if !_ShopPrices_Ask("You have " _ShopPrices_DirtyCount()
            . " unapplied price change(s).`n`nClose and discard them?")
            return 1
    }
    _ShopPrices_PanelMode := ""
    if IsObject(_ShopPrices_NativeGui)
        try _ShopPrices_NativeGui.Hide()
    return 1   ; already hidden above; skip the default action
}

_ShopPrices_NativeRefreshClients() {
    global _ShopPrices_NativeClientDdl, _ShopPrices_NativeClientPids, _ShopPrices_TargetPid

    if !IsObject(_ShopPrices_NativeClientDdl)
        return
    labels := [], pids := [], chosen := 0
    for snap in GetClientSnapshots() {
        pids.Push(snap.pid)
        labels.Push((snap.charName != "" ? snap.charName : "PID " snap.pid)
            . (snap.isActive ? "  (active)" : ""))
        if (snap.pid = _ShopPrices_TargetPid)
            chosen := pids.Length
    }
    ; The poll may not have covered the pinned client yet — never drop it from
    ; its own picker.
    if (!chosen && _ShopPrices_TargetPid) {
        pids.Push(_ShopPrices_TargetPid)
        labels.Push(_ShopPrices_TargetLabel())
        chosen := pids.Length
    }
    _ShopPrices_NativeClientPids := pids
    _ShopPrices_NativeClientDdl.Delete()
    if labels.Length
        _ShopPrices_NativeClientDdl.Add(labels)
    if chosen
        _ShopPrices_NativeClientDdl.Choose(chosen)
}

_ShopPrices_NativeRefreshPresets() {
    global _ShopPrices_NativePresetDdl, _ShopPrices_NativePresetIds

    if !IsObject(_ShopPrices_NativePresetDdl)
        return
    labels := [], ids := []
    for entry in _ShopPresets_LoadIndex() {
        ids.Push(entry.id)
        labels.Push(entry.name)
    }
    _ShopPrices_NativePresetIds := ids
    _ShopPrices_NativePresetDdl.Delete()
    if labels.Length {
        _ShopPrices_NativePresetDdl.Add(labels)
        _ShopPrices_NativePresetDdl.Choose(1)
    }
}

_ShopPrices_NativeSelectedPresetId() {
    global _ShopPrices_NativePresetDdl, _ShopPrices_NativePresetIds
    if !IsObject(_ShopPrices_NativePresetDdl)
        return 0
    idx := _ShopPrices_NativePresetDdl.Value
    if (!idx || idx > _ShopPrices_NativePresetIds.Length)
        return 0
    return _ShopPrices_NativePresetIds[idx]
}

_ShopPrices_NativeRefresh() {
    global _ShopPrices_NativeList, _ShopPrices_SLOT_COUNT
    global _ShopPrices_Draft, _ShopPrices_Live, _ShopPrices_LiveIds, _ShopPrices_IdsAvail
    global _ShopPrices_Suspect

    if !IsObject(_ShopPrices_NativeList)
        return

    _ShopPrices_NativeList.Opt("-Redraw")
    _ShopPrices_NativeList.Delete()
    Loop _ShopPrices_SLOT_COUNT {
        price := (_ShopPrices_Draft.Length >= A_Index) ? _ShopPrices_Draft[A_Index] : 0
        live  := (_ShopPrices_Live.Length  >= A_Index) ? _ShopPrices_Live[A_Index]  : 0
        id    := (_ShopPrices_LiveIds.Length >= A_Index) ? _ShopPrices_LiveIds[A_Index] : 0

        if !_ShopPrices_IdsAvail
            itemText := "?"
        else if _ShopPrices_IsEmptySlot(id)
            itemText := "(empty)"
        else
            itemText := String(id)

        priceText := _ShopPrices_FormatPrice(price)
        if (price != live)
            priceText .= "  *"          ; unapplied
        ; Only while the read value still stands — see _ShopPrices_SendGrid.
        if (price = live && _ShopPrices_Suspect.Length >= A_Index
            && _ShopPrices_Suspect[A_Index])
            priceText .= "  (out of range)"

        _ShopPrices_NativeList.Add(, A_Index, itemText, priceText,
            _ShopPrices_FormatShort(price), price > 0 ? "Listed" : "—")
    }
    loop 5
        _ShopPrices_NativeList.ModifyCol(A_Index, "AutoHdr")
    _ShopPrices_NativeList.Opt("+Redraw")
    _ShopPrices_NativeUpdateStatus()
}

_ShopPrices_NativeUpdateStatus() {
    global _ShopPrices_NativeStatus, _ShopPrices_Draft, _ShopPrices_LiveCount

    if !IsObject(_ShopPrices_NativeStatus)
        return
    listed := _ShopPrices_CountListed(_ShopPrices_Draft)
    dirty := _ShopPrices_DirtyCount()
    text := listed " will be listed (" _ShopPrices_LiveCount " now)"
    if dirty
        text .= " · " dirty " changed"
    reason := _ShopPrices_CheckWritable()
    if (reason != "")
        text .= " · " reason
    _ShopPrices_NativeStatus.Value := text
}

; Row index is the slot number — the list is always the 24 slots in order.
_ShopPrices_NativeSelectedSlots() {
    global _ShopPrices_NativeList
    slots := []
    if !IsObject(_ShopPrices_NativeList)
        return slots
    row := 0
    loop {
        row := _ShopPrices_NativeList.GetNext(row)
        if !row
            break
        slots.Push(row)
    }
    return slots
}

_ShopPrices_NativeSelectAll() {
    global _ShopPrices_NativeList
    if IsObject(_ShopPrices_NativeList)
        _ShopPrices_NativeList.Modify(0, "Select")
}

_ShopPrices_NativeOnRowDouble(lv, row) {
    global _ShopPrices_NativePriceEdit, _ShopPrices_Draft
    if (row < 1 || row > _ShopPrices_Draft.Length)
        return
    if IsObject(_ShopPrices_NativePriceEdit)
        _ShopPrices_NativePriceEdit.Value := _ShopPrices_FormatPrice(_ShopPrices_Draft[row])
}

_ShopPrices_NativeSetOnSelected() {
    global _ShopPrices_NativePriceEdit

    slots := _ShopPrices_NativeSelectedSlots()
    if !slots.Length {
        _ShopPrices_Say("Select one or more slots first.", "Icon!")
        return
    }
    parsed := _ShopPrices_NormalizePrice(_ShopPrices_NativePriceEdit.Value)
    if !parsed.ok {
        _ShopPrices_Say(parsed.reason, "Icon!")
        return
    }
    _ShopPrices_DraftSetMany(slots, parsed.value)
    ; Echo the canonical form back so the user sees what was actually taken.
    _ShopPrices_NativePriceEdit.Value := _ShopPrices_FormatPrice(parsed.value)
    _ShopPrices_NativeRefresh()
}

_ShopPrices_NativeClearSelected() {
    slots := _ShopPrices_NativeSelectedSlots()
    if !slots.Length {
        _ShopPrices_Say("Select one or more slots first.", "Icon!")
        return
    }
    _ShopPrices_DraftSetMany(slots, 0)
    _ShopPrices_NativeRefresh()
}

_ShopPrices_NativeRevert() {
    if !_ShopPrices_DraftIsDirty() {
        _ShopPrices_Say("There is nothing to revert.")
        return
    }
    if !_ShopPrices_Ask("Discard " _ShopPrices_DirtyCount() " unapplied change(s)?")
        return
    _ShopPrices_DraftReset()
    _ShopPrices_NativeRefresh()
}

_ShopPrices_NativeReload() {
    if _ShopPrices_DraftIsDirty() {
        if !_ShopPrices_Ask("Re-reading the client will discard " _ShopPrices_DirtyCount()
            . " unapplied change(s).`n`nContinue?")
            return
    }
    state := _ShopPrices_LoadFromClient()
    if !state.ok {
        _ShopPrices_Say(state.reason, "Iconx")
        return
    }
    _ShopPrices_NativeRefresh()
}

_ShopPrices_NativeOnClientChange() {
    global _ShopPrices_NativeClientDdl, _ShopPrices_NativeClientPids, _ShopPrices_TargetPid

    idx := _ShopPrices_NativeClientDdl.Value
    if (!idx || idx > _ShopPrices_NativeClientPids.Length)
        return
    pid := _ShopPrices_NativeClientPids[idx]
    if (pid = _ShopPrices_TargetPid)
        return
    if _ShopPrices_DraftIsDirty() {
        if !_ShopPrices_Ask("Switching client will discard " _ShopPrices_DirtyCount()
            . " unapplied change(s).`n`nSwitch anyway?") {
            _ShopPrices_NativeRefreshClients()   ; put the picker back
            return
        }
    }
    hwnd := 0
    for snap in GetClientSnapshots() {
        if (snap.pid = pid) {
            hwnd := snap.hwnd
            break
        }
    }
    _ShopPrices_PinTarget(pid, hwnd)
    state := _ShopPrices_LoadFromClient()
    if !state.ok
        _ShopPrices_Say(state.reason, "Iconx")
    _ShopPrices_NativeRefresh()
}

_ShopPrices_NativeApply() {
    global _ShopPrices_Draft

    if !_ShopPrices_DraftIsDirty() {
        _ShopPrices_Say("Nothing has changed since the grid was read.")
        return
    }
    reason := _ShopPrices_CheckWritable()
    if (reason != "") {
        _ShopPrices_Say(reason, "Icon!")
        return
    }
    if !_ShopPrices_ConfirmHighPrice(_ShopPrices_Draft)
        return

    res := _ShopPrices_ApplyPrices()
    _ShopPrices_NativeRefresh()
    if !res.ok {
        _ShopPrices_Say(res.reason, "Iconx")
        return
    }
    _ShopPrices_Say(res.listed " item(s) priced and the shop count updated.`n`n"
        . "Make sure you check your item prices before you open Vendor!")
}

_ShopPrices_NativeSavePreset() {
    name := _ShopPresets_UniqueName(_ShopPrices_TargetLabel() " prices")
    ; Names are collected here rather than in a frontend so both panels get the
    ; same sanitising and duplicate checks.
    ib := _ShopPrices_Prompt("Name this preset.", name)
    if (ib.Result != "OK")
        return
    reason := _ShopPresets_ValidateName(ib.Value)
    if (reason != "") {
        _ShopPrices_Say(reason, "Icon!")
        return
    }
    preset := _ShopPrices_PresetFromDraft(ib.Value)
    if !preset.entries.Count {
        _ShopPrices_Say("No slot has a price, so there is nothing to save.", "Icon!")
        return
    }
    if !_ShopPresets_Save(preset)
        return
    _ShopPrices_NativeRefreshPresets()
    TrayTip("Saved '" preset.name "' (" preset.entries.Count " priced slots).",
        "Character Vendor", "Iconi")
}

_ShopPrices_NativeApplyPreset() {
    id := _ShopPrices_NativeSelectedPresetId()
    if !id {
        _ShopPrices_Say("There are no saved presets yet.", "Icon!")
        return
    }
    diff := _ShopPrices_DiffPreset(id)
    if !diff.ok {
        _ShopPrices_Say(diff.reason, "Iconx")
        return
    }
    if !diff.rows.Length {
        _ShopPrices_Say("'" diff.name "' matches the grid already — nothing to do.")
        return
    }
    ; Slot-keyed presets repoint silently when the inventory is reordered, so
    ; the diff is always shown before anything is loaded.
    if !_ShopPrices_Ask(_ShopPrices_DiffText(diff), "Icon?",
        "Character Vendor — " diff.name)
        return
    res := _ShopPrices_ApplyPresetToDraft(id)
    _ShopPrices_NativeRefresh()
    if res.ok
        TrayTip("Loaded " res.changed " price(s) into the grid. Press Apply when you're happy.",
            "Character Vendor", "Iconi")
}

_ShopPrices_NativeDeletePreset() {
    id := _ShopPrices_NativeSelectedPresetId()
    if !id {
        _ShopPrices_Say("There are no saved presets yet.", "Icon!")
        return
    }
    name := _ShopPresets_NameForId(id)
    if !_ShopPrices_Ask("Delete the preset '" name "'?")
        return
    if _ShopPresets_Delete(id)
        _ShopPrices_NativeRefreshPresets()
}

; ── WebView2 frontend ────────────────────────────────────────────
;
; AHK owns the store: every edit round-trips through here, so the page never
; holds a price the addon does not also have. Destroy-on-close rather than the
; park/reveal the tray menu uses — this panel is opened deliberately and rarely,
; and a parked window holding a stale draft against a pinned pid is a footgun.

global _ShopPrices_WebGui := 0

_ShopPrices_ShowPanelWeb() {
    global _ShopPrices_WebGui, _ShopPrices_PanelMode

    if (IsObject(_ShopPrices_WebGui) && _ShopPrices_WebGui.Hwnd) {
        try WinActivate("ahk_id " _ShopPrices_WebGui.Hwnd)
        return
    }

    dllDir := (A_PtrSize = 8) ? "64bit" : "32bit"
    dllPath := A_ScriptDir "\Lib\" dllDir "\WebView2Loader.dll"
    wvSettings := { DllPath: dllPath, DefaultWidth: 980, DefaultHeight: 680 }

    g := WebViewGui("-Caption +AlwaysOnTop +Resize",
        "Maps++ — Character Vendor", , wvSettings)
    g.OnEvent("Close", (*) => _ShopPrices_WebRequestClose())
    g.WebMessageReceived(_ShopPrices_OnWebMsg)
    g.DOMContentLoaded((*) => SetTimer(_ShopPrices_SendState, -50))
    g.Navigate("ui/shop_prices/index.html")

    _ShopPrices_WebGui := g
    _ShopPrices_PanelMode := "web"
    g.Show("w980 h680 Center")
}

_ShopPrices_WebClose() {
    global _ShopPrices_WebGui, _ShopPrices_PanelMode
    if IsObject(_ShopPrices_WebGui)
        try _ShopPrices_WebGui.Destroy()
    _ShopPrices_WebGui := 0
    _ShopPrices_PanelMode := ""
}

; The confirm lives here rather than in the page so the native panel asks the
; same question in the same words. Returns non-zero either way: 1 on cancel to
; veto the close, and 1 after destroying so the default action does not run
; against a window that no longer exists.
_ShopPrices_WebRequestClose() {
    if _ShopPrices_DraftIsDirty() {
        if !_ShopPrices_Ask("You have " _ShopPrices_DirtyCount()
            . " unapplied price change(s).`n`nClose and discard them?")
            return 1
    }
    _ShopPrices_WebClose()
    return 1
}

_ShopPrices_WebPost(json) {
    global _ShopPrices_WebGui
    if !IsObject(_ShopPrices_WebGui)
        return
    try _ShopPrices_WebGui.PostWebMessageAsJson(json)
}

_ShopPrices_WebToast(level, text) {
    _ShopPrices_WebPost('{"type":"toast","level":' _JSON_Str(level)
        . ',"text":' _JSON_Str(text) '}')
}

_ShopPrices_SendState() {
    global _ShopPrices_SLOT_COUNT, _ShopPrices_GRID_COLS, _ShopPrices_MAX_PRICE
    global _ShopPrices_IdsAvail, _ShopPrices_IconBase, _ShopPrices_TargetPid
    global _ShopPrices_TargetHwnd

    reason := _ShopPrices_CheckWritable()
    clientsJson := ""
    for snap in GetClientSnapshots() {
        clientsJson .= (clientsJson = "" ? "" : ",")
            . '{"pid":' snap.pid
            . ',"charName":' _JSON_Str(snap.charName != "" ? snap.charName : "PID " snap.pid)
            . ',"isActive":' (snap.isActive ? "true" : "false")
            . '}'
    }
    ; The poll may not have covered the pinned client yet — never drop it from
    ; its own picker.
    if (clientsJson = "" && _ShopPrices_TargetPid) {
        clientsJson := '{"pid":' _ShopPrices_TargetPid
            . ',"charName":' _JSON_Str(_ShopPrices_TargetLabel())
            . ',"isActive":true}'
    }

    _ShopPrices_WebPost('{"type":"shop-state"'
        . ',"target":{"pid":' _ShopPrices_TargetPid
        . ',"hwnd":' _ShopPrices_TargetHwnd
        . ',"charName":' _JSON_Str(_ShopPrices_TargetLabel())
        . ',"canWrite":' (reason = "" ? "true" : "false")
        . ',"reason":' _JSON_Str(reason) '}'
        . ',"clients":[' clientsJson ']'
        . ',"slotCount":' _ShopPrices_SLOT_COUNT
        . ',"cols":' _ShopPrices_GRID_COLS
        . ',"maxPrice":' _ShopPrices_MAX_PRICE
        . ',"idsKnown":' (_ShopPrices_IdsAvail ? "true" : "false")
        . ',"iconBase":' _ShopPrices_IconBase
        . '}')

    _ShopPrices_SendGrid()
    _ShopPrices_SendPresets()
}

_ShopPrices_SendGrid() {
    global _ShopPrices_SLOT_COUNT, _ShopPrices_Draft, _ShopPrices_Live
    global _ShopPrices_LiveIds, _ShopPrices_LiveCount, _ShopPrices_IdsAvail
    global _ShopPrices_Suspect, _ShopPrices_Rev

    slotsJson := ""
    Loop _ShopPrices_SLOT_COUNT {
        price := (_ShopPrices_Draft.Length   >= A_Index) ? _ShopPrices_Draft[A_Index]   : 0
        live  := (_ShopPrices_Live.Length    >= A_Index) ? _ShopPrices_Live[A_Index]    : 0
        id    := (_ShopPrices_LiveIds.Length >= A_Index) ? _ShopPrices_LiveIds[A_Index] : 0
        susp  := (_ShopPrices_Suspect.Length >= A_Index) ? _ShopPrices_Suspect[A_Index] : false

        slotsJson .= (slotsJson = "" ? "" : ",")
            . '{"i":' A_Index
            . ',"itemId":' id
            . ',"price":' price
            . ',"live":' live
            . ',"band":' _ShopPrices_MagnitudeBand(price)
            . ',"empty":' ((_ShopPrices_IdsAvail && _ShopPrices_IsEmptySlot(id)) ? "true" : "false")
            ; "suspect" describes what was read, so it stops applying the moment
            ; the user authors a value over it — otherwise a bad read would
            ; leave Apply disabled with no way to clear it.
            . ',"suspect":' ((susp && price = live) ? "true" : "false")
            . '}'
    }

    _ShopPrices_WebPost('{"type":"grid-state"'
        . ',"rev":' _ShopPrices_Rev
        . ',"listed":' _ShopPrices_CountListed(_ShopPrices_Draft)
        . ',"liveListed":' _ShopPrices_LiveCount
        . ',"dirty":' (_ShopPrices_DraftIsDirty() ? "true" : "false")
        . ',"slots":[' slotsJson ']}')
}

; One slot only, so typing in a field never triggers a whole-grid re-render
; that would steal the caret mid-edit.
_ShopPrices_SendSlot(slot) {
    global _ShopPrices_Draft, _ShopPrices_Rev

    price := (_ShopPrices_Draft.Length >= slot) ? _ShopPrices_Draft[slot] : 0
    _ShopPrices_WebPost('{"type":"slot-updated"'
        . ',"rev":' _ShopPrices_Rev
        . ',"slot":' slot
        . ',"price":' price
        . ',"band":' _ShopPrices_MagnitudeBand(price)
        . ',"listed":' _ShopPrices_CountListed(_ShopPrices_Draft)
        . ',"dirty":' (_ShopPrices_DraftIsDirty() ? "true" : "false")
        . '}')
}

_ShopPrices_SendPresets() {
    presetsJson := ""
    for entry in _ShopPresets_LoadIndex() {
        preset := _ShopPresets_Load(entry.id)
        if !IsObject(preset)
            continue
        presetsJson .= (presetsJson = "" ? "" : ",")
            . '{"id":' entry.id
            . ',"name":' _JSON_Str(preset.name)
            . ',"slots":' preset.entries.Count
            . ',"savedBy":' _JSON_Str(preset.savedBy)
            . ',"saved":' _JSON_Str(preset.saved)
            . '}'
    }
    _ShopPrices_WebPost('{"type":"presets-state","presets":[' presetsJson ']}')
}

_ShopPrices_SendDiff(id) {
    diff := _ShopPrices_DiffPreset(id)
    if !diff.ok {
        _ShopPrices_WebToast("error", diff.reason)
        return
    }
    rowsJson := ""
    for row in diff.rows {
        rowsJson .= (rowsJson = "" ? "" : ",")
            . '{"slot":' row.slot
            . ',"action":' _JSON_Str(row.action)
            . ',"from":' row.from
            . ',"to":' row.to
            . ',"itemId":' row.itemId
            . ',"presetItemId":' row.presetItemId
            . ',"warn":' _JSON_Str(row.warn)
            . '}'
    }
    _ShopPrices_WebPost('{"type":"preset-diff"'
        . ',"id":' diff.id
        . ',"name":' _JSON_Str(diff.name)
        . ',"warnCount":' diff.warnCount
        . ',"rows":[' rowsJson ']}')
}

_ShopPrices_SendGuard() {
    reason := _ShopPrices_CheckWritable()
    _ShopPrices_WebPost('{"type":"guard-state"'
        . ',"canWrite":' (reason = "" ? "true" : "false")
        . ',"reason":' _JSON_Str(reason) '}')
}

_ShopPrices_MsgInt(msg, key) {
    if (!msg.Has(key) || !IsNumber(msg[key]))
        return 0
    return Integer(Round(msg[key]))
}

_ShopPrices_MsgIntArray(msg, key) {
    out := []
    if (!msg.Has(key) || !IsObject(msg[key]))
        return out
    for v in msg[key] {
        if IsNumber(v)
            out.Push(Integer(Round(v)))
    }
    return out
}

_ShopPrices_MsgText(msg, key) {
    if !msg.Has(key)
        return ""
    return String(msg[key])
}

_ShopPrices_OnWebMsg(wv, args) {
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

    switch msg["type"] {
        case "init-request":
            _ShopPrices_SendState()

        case "refresh":
            _ShopPrices_WebRefresh(msg)

        case "set-price":
            _ShopPrices_WebSetPrice(msg)

        case "bulk-set":
            _ShopPrices_WebBulkSet(msg)

        case "clear-slots":
            _ShopPrices_DraftSetMany(_ShopPrices_MsgIntArray(msg, "slots"), 0)
            _ShopPrices_SendGrid()

        case "revert":
            _ShopPrices_DraftReset()
            _ShopPrices_SendGrid()
            _ShopPrices_WebToast("info", "Reverted to the prices in the client.")

        case "apply":
            _ShopPrices_WebApply()

        case "select-client":
            _ShopPrices_WebSelectClient(msg)

        case "preset-save-request":
            _ShopPrices_WebSavePreset()

        case "preset-preview":
            _ShopPrices_SendDiff(_ShopPrices_MsgInt(msg, "id"))

        case "preset-apply":
            _ShopPrices_WebApplyPreset(_ShopPrices_MsgInt(msg, "id"))

        case "preset-delete":
            _ShopPrices_WebDeletePreset(_ShopPrices_MsgInt(msg, "id"))

        case "preset-rename":
            _ShopPrices_WebRenamePreset(_ShopPrices_MsgInt(msg, "id"))

        case "close":
            _ShopPrices_WebRequestClose()
    }
}

_ShopPrices_WebRefresh(msg) {
    if (_ShopPrices_DraftIsDirty() && !_ShopPrices_MsgInt(msg, "force")) {
        if !_ShopPrices_Ask("Re-reading the client will discard " _ShopPrices_DirtyCount()
            . " unapplied change(s).`n`nContinue?")
            return
    }
    state := _ShopPrices_LoadFromClient()
    if !state.ok {
        _ShopPrices_WebToast("error", state.reason)
        return
    }
    _ShopPrices_SendState()
}

_ShopPrices_WebSetPrice(msg) {
    slot := _ShopPrices_MsgInt(msg, "slot")
    parsed := _ShopPrices_NormalizePrice(_ShopPrices_MsgText(msg, "text"))
    if !parsed.ok {
        _ShopPrices_WebToast("error", parsed.reason)
        _ShopPrices_SendSlot(slot)          ; echo the unchanged value back
        return
    }
    _ShopPrices_DraftSet(slot, parsed.value)
    _ShopPrices_SendSlot(slot)
}

_ShopPrices_WebBulkSet(msg) {
    slots := _ShopPrices_MsgIntArray(msg, "slots")
    parsed := _ShopPrices_NormalizePrice(_ShopPrices_MsgText(msg, "text"))
    if !parsed.ok {
        _ShopPrices_WebToast("error", parsed.reason)
        return
    }
    n := _ShopPrices_DraftSetMany(slots, parsed.value)
    _ShopPrices_SendGrid()
    _ShopPrices_WebToast("info", "Set " _ShopPrices_FormatPrice(parsed.value)
        . " on " n " slot" (n = 1 ? "" : "s") ".")
}

_ShopPrices_WebApply() {
    global _ShopPrices_Draft

    if !_ShopPrices_DraftIsDirty() {
        _ShopPrices_WebToast("warn", "Nothing has changed since the grid was read.")
        return
    }
    if !_ShopPrices_ConfirmHighPrice(_ShopPrices_Draft) {
        _ShopPrices_WebToast("info", "Nothing was written.")
        return
    }

    res := _ShopPrices_ApplyPrices()
    _ShopPrices_SendGrid()
    if res.ok {
        _ShopPrices_WebPost('{"type":"save-result","ok":true'
            . ',"listed":' res.listed
            . ',"written":' res.written
            . ',"message":' _JSON_Str(res.listed
                . " item(s) priced. Make sure you check your item prices before you open Vendor!") '}')
    } else {
        _ShopPrices_WebPost('{"type":"save-result","ok":false,"listed":0,"written":0'
            . ',"message":' _JSON_Str(res.reason) '}')
    }
}

_ShopPrices_WebSelectClient(msg) {
    global _ShopPrices_TargetPid

    pid := _ShopPrices_MsgInt(msg, "pid")
    if (!pid || pid = _ShopPrices_TargetPid)
        return
    if _ShopPrices_DraftIsDirty() {
        if !_ShopPrices_Ask("Switching client will discard " _ShopPrices_DirtyCount()
            . " unapplied change(s).`n`nSwitch anyway?") {
            _ShopPrices_SendState()          ; put the picker back
            return
        }
    }
    hwnd := 0
    for snap in GetClientSnapshots() {
        if (snap.pid = pid) {
            hwnd := snap.hwnd
            break
        }
    }
    _ShopPrices_PinTarget(pid, hwnd)
    state := _ShopPrices_LoadFromClient()
    if !state.ok
        _ShopPrices_WebToast("error", state.reason)
    _ShopPrices_SendState()
}

; Names are collected with an InputBox rather than in the page: _JSON_Parse
; does not decode \uXXXX escapes, so a name with any non-ASCII character would
; corrupt on the way back from JS.
_ShopPrices_WebSavePreset() {
    name := _ShopPresets_UniqueName(_ShopPrices_TargetLabel() " prices")
    ib := _ShopPrices_Prompt("Name this preset.", name)
    if (ib.Result != "OK")
        return
    reason := _ShopPresets_ValidateName(ib.Value)
    if (reason != "") {
        _ShopPrices_WebToast("error", reason)
        return
    }
    preset := _ShopPrices_PresetFromDraft(ib.Value)
    if !preset.entries.Count {
        _ShopPrices_WebToast("warn", "No slot has a price, so there is nothing to save.")
        return
    }
    if !_ShopPresets_Save(preset)
        return
    _ShopPrices_SendPresets()
    _ShopPrices_WebToast("info", "Saved '" preset.name "' ("
        . preset.entries.Count " priced slots).")
}

_ShopPrices_WebApplyPreset(id) {
    res := _ShopPrices_ApplyPresetToDraft(id)
    if !res.ok {
        _ShopPrices_WebToast("error", res.reason)
        return
    }
    _ShopPrices_SendGrid()
    _ShopPrices_WebToast("info", "Loaded " res.changed
        . " price(s) into the grid. Press Apply when you're happy.")
}

_ShopPrices_WebDeletePreset(id) {
    name := _ShopPresets_NameForId(id)
    if (name = "")
        return
    if !_ShopPrices_Ask("Delete the preset '" name "'?")
        return
    if _ShopPresets_Delete(id)
        _ShopPrices_SendPresets()
}

_ShopPrices_WebRenamePreset(id) {
    current := _ShopPresets_NameForId(id)
    if (current = "")
        return
    ib := _ShopPrices_Prompt("Rename this preset.", current)
    if (ib.Result != "OK")
        return
    reason := _ShopPresets_Rename(id, ib.Value)
    if (reason != "") {
        _ShopPrices_WebToast("error", reason)
        return
    }
    _ShopPrices_SendPresets()
}

; ── Panel entry point ────────────────────────────────────────────

_ShopPrices_Open() {
    global _ShopPrices_PanelMode, _ShopPrices_NativeGui, _ShopPrices_WebGui

    ; Already open — activate rather than re-pinning and losing the draft.
    if (_ShopPrices_PanelMode = "native" && IsObject(_ShopPrices_NativeGui)
        && _ShopPrices_NativeGui.Hwnd) {
        try WinActivate("ahk_id " _ShopPrices_NativeGui.Hwnd)
        return
    }
    if (_ShopPrices_PanelMode = "web" && IsObject(_ShopPrices_WebGui)
        && _ShopPrices_WebGui.Hwnd) {
        try WinActivate("ahk_id " _ShopPrices_WebGui.Hwnd)
        return
    }

    target := _ShopPrices_ResolveOpenTarget()
    if !target.ok {
        TrayTip(target.reason, "Character Vendor", "Icon!")
        return
    }
    _ShopPrices_PinTarget(target.pid, target.hwnd)

    state := _ShopPrices_LoadFromClient()
    if !state.ok {
        TrayTip(state.reason, "Character Vendor", "Iconx")
        return
    }

    if (IsNativeInterface() || !_ShopPrices_CanUseWebView()) {
        _ShopPrices_ShowPanelNative()
        return
    }
    _ShopPrices_ShowPanelWeb()
}
