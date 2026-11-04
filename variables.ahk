#Requires AutoHotkey v2.0

; === Constants ===
; The one place the version is written. main.ahk carries a matching
; ;@Ahk2Exe-SetVersion literal so the compiled exe's file properties agree, and
; build.ps1 fails the build if the two ever drift apart.
global APP_VERSION := "1.4.0"
global PROCESS_EXE := "main.exe"
global GAME_WIN_FILTER := "ahk_exe " PROCESS_EXE
; The game's own window. GAME_WIN_FILTER also matches the client's separate chat
; window (class "WindowedMode", owned by this one), so anything that places
; itself on the game picture resolves through GameMainWindow() first — while
; chat had focus, the minimap used to anchor itself to the chat window.
global GAME_MAIN_WIN_CLASS := "##RAYSDDWNDCLASS20010705##"
; The game draws an 800×600 logical surface whatever size its window is.
global GAME_LOGICAL_W := 800
global GAME_LOGICAL_H := 600
; Published on the game window by osmw_display.dll (the client mods' 2×/3×,
; Resizable and Borderless modes) and removed in its Stock mode. Client-relative;
; the four edges are stored plus one because a zero property means "absent".
; Scale is the integer magnification; Mode is 1 windowed, 2 resizable,
; 3 borderless. See tools/client-dll/display-scaling.md in the server repo.
global DISPLAY_PROP_LEFT := "osMW.Display.ContentLeft.v1"
global DISPLAY_PROP_TOP := "osMW.Display.ContentTop.v1"
global DISPLAY_PROP_RIGHT := "osMW.Display.ContentRight.v1"
global DISPLAY_PROP_BOTTOM := "osMW.Display.ContentBottom.v1"
global DISPLAY_PROP_SCALE := "osMW.Display.ContentScale.v1"
global DISPLAY_PROP_MODE := "osMW.Display.Mode.v1"
global DISPLAY_MODE_BORDERLESS := 3
global DISPLAY_MODE_NAMES := Map(0, "stock", 1, "windowed", 2, "resizable", 3, "borderless")
; osmw_status.ini's Result codes, mirroring osmw_core_api.h in the server repo's
; tools/client-mods/core/sdk. Anything not listed is shown as its number.
global NATIVE_MODS_RESULT_NAMES := Map(
    0, "ok",
    1, "disabled",
    -1, "invalid argument",
    -2, "incompatible ABI",
    -3, "unavailable",
    -4, "not found (a signature missed — client update?)",
    -5, "signature matched more than once",
    -6, "address already claimed by another mod",
    -7, "wrong owner",
    -8, "busy",
    -9, "patch still active",
    -10, "internal error",
    -11, "buffer too small",
    -12, "queue full",
    -13, "duplicate id",
    -14, "stopping",
    -15, "blocked by Windows (Smart App Control)")
; Module states that mean the feature is not doing its job.
global NATIVE_MODS_BAD_STATES := ["missing", "legacy", "incompatible", "failed", "unsafe-to-unload"]
; A launch waits this long at most for osmw_display.dll to finish resizing a new
; client before centring it or applying a layout, and calls the size settled
; once it has held still for LAUNCH_SETTLE_STABLE_MS.
global LAUNCH_SETTLE_TIMEOUT_MS := 4000
global LAUNCH_SETTLE_STABLE_MS := 500
; RVAs from main.exe — do not use fixed absolute addresses (bases differ per process / ASLR).
global MAP_FILE_OFFSET := 0x340EC5
; Friendly zone name (e.g. "Woodlingor"), adjacent to the map filename string.
global MAP_NAME_OFFSET := 0x340EB8
; global POS_X_OFFSET := 0x3049E8
; global POS_Y_OFFSET := 0x3049EC
global POS_X_OFFSET := 0x30B2D0
global POS_Y_OFFSET := 0x30B2D4
global GAME_STATE_OFFSET := 0x34313C
global BATTLE_STATE_OFFSET := 0x301DE4

; ── The game's own minimap ──
; The client now ships minimaps for maps it never had (Oranus, via the
; minimap_file4 patch and system\smallmap04.mda), so Maps++ must know whether the
; map in front of it has one before it takes Tab away from the game. All from
; the client RE notes (docs/client-packet-re.md, "the 52-minimap cap") and the
; OpenMinimap gate disassembled at VA 0x442370; RVA = VA - 0x400000.
;
; The map's minimap image index, from MapData offset 40 (written at 0x417DC2).
; Signed: < 0 means the map has none, and 0 is a real image.
global MINIMAP_INDEX_OFFSET := 0x340EE8
; MapData offset 72. Zeroed alongside a negative index; must be non-zero.
global MINIMAP_GATE_OFFSET := 0x340F44
; Byte. The server's EnableMinimap flag (packet 0x00130007). The game refuses to
; open a minimap from files 02/03/04 while it is clear.
global MINIMAP_ENABLE_FLAG_OFFSET := 0x3BAE24
; Byte. 1 once art from files 02/03/04 has been loaded. Reported, never relied
; on: it describes the last minimap the game LOADED, which can be the previous
; map's — the tier is derived from the index instead (MINIMAP_FILE2_FIRST_INDEX).
global MINIMAP_TIER_OFFSET := 0x3BAE25
; The game's minimap window handle: >= 0 open, -1 closed, and 0 before the
; client has initialised it. Diagnostics only.
global MINIMAP_WINDOW_OFFSET := 0x3028E0
; First index served by smallmap02.mda — every index from here up is a "tier"
; minimap that also needs the EnableMinimap flag.
global MINIMAP_FILE2_FIRST_INDEX := 20
; Indices 52 and up exist only on a client carrying minimap_file4. Its hook
; replaces `83 F8 34` (cmp eax, 0x34) at this RVA with an E9 jmp; on a client
; without it the imm8 there is the exclusive upper bound, and anything at or
; above it loads no art at all. Code bytes, so they move on a client update —
; anything that is neither form reads as "unknown" and the index is trusted.
global MINIMAP_FILE4_HOOK_RVA := 0x605C8
global MINIMAP_FILE4_FIRST_INDEX := 52
global MINIMAP_FILE4_MDA := "system\smallmap04.mda"
; Larger than any real index by an order of magnitude. A value past it means the
; offset has moved, not that the map has a minimap.
global MINIMAP_INDEX_SANE_MAX := 1024
; Character class of the logged-in character, 0-7. Indexes avatars\c<N>.png.
; Only meaningful once the client is past the login screens (GAME_STATE_READY).
global CHAR_CLASS_OFFSET := 0x2AA62C
; This character's own vitals — the block starting 0x1C past the class id.
;
; THE per-character health source. Not the battle actor array, which describes
; the fight from the party leader's seat and so reports the leader's health to
; every client in the party.
;
; Established with two clients dumping this region side by side while BOTH were
; in battle: every field below differed per client and the max values matched
; each client's own on-screen figures exactly (2121697 and 46775). A shared or
; leader-relative field cannot do that.
;
; Same field order as a battle record: MaxHP, CurHP, MaxMP, CurMP.
global CHAR_VITALS_OFFSET := 0x2AA648
global CHAR_V_MAXHP := 0x00
global CHAR_V_HP    := 0x04
global CHAR_V_MAXMP := 0x08
global CHAR_V_MP    := 0x0C
global CHAR_VITALS_BYTES := 0x10
global MAP_FILE_LEN := 20
global MAP_NAME_LEN := 14

; === Battle actors ===
; One flat array of combatant records, in the client's own "fight view" order:
;
;       enemy        party
;       16 17         9 8
;       12 13         5 4
;       10 11         1 0        <- location 0 is the party leader
;       14 15         3 2
;       18 19         7 6
;
; Locations 0-9 are your party, 10-19 the opposing side. Even numbers are
; characters and the odd number after each one is that character's pet, so
; location 1 is location 0's pet.
;
; Each record is BATTLE_ACTOR_STRIDE apart, and carries two blocks of
; {MaxHP, CurHP, MaxMP, CurMP} — one at +0x00 and a copy at +0x24.
;
; Measured in a live fight: the two are a genuine mirror. Every location agreed
; in both values, and taking damage moved them by identical amounts (-982/-982,
; -195/-195). So the choice does not matter; block A is read because it is first.
;
; ── Telling an occupied location from a leftover ─────────────────
;
; A location that is no longer in the fight KEEPS its last values, so HP and MP
; cannot answer "is anyone here". Measured: locations 2 and 3 reported plausible
; HP (29997/30971) for a party member who was not in that battle at all.
;
; BATTLE_F_ACTOR_ID is what answers it. It carries a per-combatant id where
; someone is standing (56, 27, 29 in that measurement) and -1 everywhere else —
; for leftovers and for never-used locations alike. That is the presence test.
;
; BATTLE_F_ACTIVE discriminated equally well in the same sample (1 against 0)
; but is a plain boolean, so it may well mean "alive" rather than "present" —
; which would hide a dead ally. It is read but is not what presence is decided
; on.
;
; Finding this is what made zeroing the array from outside unnecessary: nothing
; is written into the client.
;
; ── +0x1B8 holds text, but it is NOT this combatant's name ───────
;
; Worth stating so it is not re-investigated. The record's tail contains
; readable names ("Devious Devil", "Queef", "Dream Flyer", "Thief"), which looks
; exactly like the identity field a roster would need. It is not:
;
;   loc 10 was occupied (the enemy, id 29) and had NO name
;   loc  9 was empty (id -1, all stats zero) and HAD one
;   loc  0 held the local character's stats (max HP 46775, consistent across
;          every dump) while naming the enemy it was fighting
;
; So the text does not track occupancy and does not describe the record it sits
; in — a target or last-interaction name would fit the evidence better. Pairing
; a location with a character needs something else.
;
; Which entry belongs to which character is answered instead by matching the
; maxima against CHAR_VITALS_OFFSET — see FindOwnBattleLocation. Location 0 is
; the party LEADER, not whoever is looking, so nothing may read it as "mine".
global BATTLE_ACTOR_BASE_OFFSET := 0x2A3D90
global BATTLE_ACTOR_STRIDE := 0x1D0      ; 464 bytes between locations
global BATTLE_PARTY_SLOTS := 10          ; locations 0-9
global BATTLE_TOTAL_SLOTS := 20          ; both sides
global BATTLE_ACTOR_BLOCK_A := 0x00
global BATTLE_ACTOR_BLOCK_B := 0x24
global BATTLE_ACTOR_RECORD_BYTES := 0x4C ; through the end of BATTLE_F_ACTOR_ID
; Field order inside either block.
global BATTLE_F_MAXHP := 0x00
global BATTLE_F_HP    := 0x04
global BATTLE_F_MAXMP := 0x08
global BATTLE_F_MP    := 0x0C
; Record-level fields, outside both blocks.
global BATTLE_F_ACTIVE   := 0x3C   ; 1 where someone stands, 0 otherwise
; Per-combatant id, -1 when nobody is there. Good for presence, useless for
; identity ACROSS clients: two clients watching one fight do not agree on it,
; so it must never be part of anything compared between them (see
; BattleFingerprint, which had to stop using it).
global BATTLE_F_ACTOR_ID := 0x48
global BATTLE_ACTOR_ID_NONE := -1
; 0 = the block at +0x00, 1 = the copy at +0x24. Either works — both were
; measured moving by identical amounts when damage landed.
global BATTLE_STATS_BLOCK := 0
; Guards a plainly wrong read (a moved offset after a patch) from being rendered
; as a real health bar.
;
; Raised from 1,000,000, which was set from a small sample and would have
; rejected a real character: Queef's max HP is 2,121,697. The ceiling only has
; to be below what a misread pointer looks like (hundreds of millions, or
; negative), not close to a plausible maximum.
global BATTLE_HP_SANE_MAX := 50000000
; Raw memory position → the coordinates the game shows the player.
;
;   displayed = (raw + offset) / divisor
;
; The offsets are not cosmetic: without them every coordinate this app shows is
; low by 25 on X and 37 on Y. They were established by teleporting to known
; coordinates and reading the raw values back — five points along X and four
; along Y on Stillreach, all exact, then confirmed unchanged on Newgrove, which
; is what makes them global constants rather than per-map calibration.
;
; (A fifth Y sample, HUD y=25, wants raw -100 and reads 0 instead: the position
; clamps at the edge of the walkable area, so it sits outside this relation.)
global GAME_COORD_DIV_X := 16
global GAME_COORD_DIV_Y := 8
global GAME_COORD_OFFSET_X := 400   ; raw units, i.e. 25 displayed units
global GAME_COORD_OFFSET_Y := 300   ; raw units, i.e. 37.5 displayed units
global MAP_DIR := A_ScriptDir "\maps"
; Base map-image space. Every calibration in maps\calibration.ini maps world
; coordinates into *these* pixels, so they must not change with the user's
; display scale — scaling is applied when drawing (see MinimapDisplayW/H).
global OVERLAY_W := 400
global OVERLAY_H := 300
; 1px accent (Gui background) + 1px black ring, then map — total inset per side = 2px.
global MINIMAP_BORDER_GOLD_PX := 1
global MINIMAP_BORDER_BLACK_PX := 1
global MINIMAP_MAP_INSET := MINIMAP_BORDER_GOLD_PX + MINIMAP_BORDER_BLACK_PX
; Kept under the original name because the drawing code treats it as a mutable
; current-accent value rather than a fixed gold constant.
global MINIMAP_COLOR_GOLD := "9c7c10"
; Default source size only when calibration has no sourceW/H and image size cannot be read.
global SOURCE_MAP_W := 400
global SOURCE_MAP_H := 300
global MARKER_SIZE := 9
global MARKER_PNG := A_ScriptDir "\marker.png"

; === Minimap appearance ([Minimap] in config.ini) ===
global gMinimapScale := 100        ; 50–200 % of the 400×300 base size
global gMinimapOpacity := 100      ; 30–100 % (100 = fully opaque)
; Corner of the game's client rect the overlay sits in, before the offsets:
; Center | TopLeft | TopRight | BottomLeft | BottomRight.
global gMinimapAnchor := "Center"
global gMinimapOffsetX := 0        ; pixel nudge from the anchor; drag writes back here
global gMinimapOffsetY := 0
global gMinimapKeepOpen := false   ; pin: don't auto-close when the game loses focus
; Multiply the overlay by the focused client's display scale (osmw_display.dll's
; 2×/3×), so it stays the same size relative to the game picture. Calibration is
; unaffected: it lives in 400×300 base space and every drawing path goes through
; MinimapScaleFactor(). [Minimap] FollowGameScale.
global gMinimapFollowGameScale := true
; The focused client's integer display scale, 1 without the display module.
global gGameContentScale := 1
; Overlay size cap, as a multiple of the 400×300 base: 200 % at 3× would
; otherwise be a 2400×1800 window.
global MINIMAP_SCALE_FACTOR_MAX := 4.0
global MINIMAP_ANCHORS := ["Center", "TopLeft", "TopRight", "BottomLeft", "BottomRight"]
; Who Tab belongs to ([Minimap] Source):
;   game    the game's own minimap wherever the client has one for this map;
;           Maps++ only where it does not (the default — the client now has
;           minimaps for every map Maps++ ships an image for)
;   mapspp  Maps++ wherever it has an image, as before the client gained them
;   off     never Maps++; Tab always reaches the game
; The "Open Maps++ minimap" action ignores this and opens the overlay anyway.
global MINIMAP_SOURCES := ["game", "mapspp", "off"]
global MINIMAP_SOURCE_LABELS := ["The game's minimap where it has one, Maps++ elsewhere",
    "Maps++ wherever it has a map", "The game's minimap only (Maps++ never takes Tab)"]
global gMinimapSource := "game"
; Last ReadNativeMinimapState() for the focused client, for the debug views.
global gNativeMinimap := 0
; Whether the one-time "Tab now opens the game's minimap here" notice has been
; shown ([Minimap] SourceNoticeShown in config.ini).
global gMinimapSourceNoticeShown := false
; Marker labels (party markers, POIs): white on an opaque black box. The dot
; carries the colour — coloured text on map art was the unreadable part.
global MARKER_LABEL_TEXT_COLOR := "FFFFFF"
global MARKER_LABEL_PAD_X := 3
; When labels are drawn. "autohide" is the default: labels are what you want at
; a glance while playing, so they show normally and get out of the way only
; when you put the mouse on the minimap to look at the art underneath.
; ("hover" is the old name for the inverted behaviour and migrates to this.)
global MARKER_LABEL_MODES := ["autohide", "always", "never"]
global MARKER_LABEL_MODE_LABELS := ["Hide while the mouse is over the minimap", "Always", "Never"]
; True while the cursor is over the overlay window; maintained by the marker
; timer and published to addons through the OnOverlayHover hook.
global gOverlayHover := false
; In-game coordinates under the cursor, shown while hovering the minimap.
global gShowHoverCoords := true
global gCoordReadout := 0
; A temporary "meet me here" mark, distinct from a POI: one at a time, never
; saved, and dropped when you leave the zone. {rawX, rawY, mapName} or 0.
global gWaypoint := 0
global gWaypointDot := 0
global gWaypointLabel := 0
global WAYPOINT_COLOR := "FF3B30"
; True while the user is dragging the overlay — suppresses the follow-the-window
; reposition in UpdateMarkerPosition so the drag doesn't fight the timer.
global gOverlayDragging := false
; Tick of the last click on the overlay, for double-click detection.
global gOverlayLastClickTick := 0

; === Launcher config ===
global CONFIG_INI := ResolveWritableIniPath("config.ini")
; Shipped, read-only defaults — never routed through ResolveWritableIniPath
; (that function finds a writable location for user state; this file is
; neither user state nor ever written). Always sits next to the script/exe.
global DEFAULTS_INI := A_ScriptDir "\defaults.ini"
global gGamePath := ""           ; Resolved path to the game executable.
global gGameArgs := ""           ; Optional command-line arguments for the game.
global gLaunchOnStartup := false ; Auto-launch one game instance on minimap startup.
global gLaunchAutoLogin := true  ; Launch shortcuts skip intros and log in from the roster.
; Windows "run on login" registry entry (per-user, no admin). The registry is the
; source of truth for this setting — not config.ini — so it can't drift from the
; Task Manager Startup tab.
global STARTUP_RUN_KEY := "HKCU\Software\Microsoft\Windows\CurrentVersion\Run"
global STARTUP_RUN_NAME := "osMW Maps++"
global gMultiClientCount := 5    ; How many clients the multi-client launch task starts.
global gMultiClientDelay := 0    ; Delay (ms) between each client launch in that task.
; Per-display layout applied after the Primary/Secondary launch actions finish.
; Empty means center the new clients only; the sentinel follows Window Layout's
; current default, while any other value pins that named preset/custom layout.
global LAUNCH_LAYOUT_DEFAULT := "__default__"
global gPrimaryLaunchLayout := LAUNCH_LAYOUT_DEFAULT
global gSecondaryLaunchLayout := LAUNCH_LAYOUT_DEFAULT
; Named launch profiles. Each bundles the four things a launch needs — which
; accounts, how many clients, which display, which layout — so the Primary and
; Secondary actions stay one click and one hotkey while still being able to mean
; "my main team" and "my alts". Map of id (a positive integer, as a string) to a
; Map of name/accounts/count/monitor/layout; see LoadLaunchProfiles.
global gLaunchProfiles := Map()
; Which profile each launch action runs. "" falls back to a synthesised default
; that reproduces the pre-profile behaviour exactly.
global gPrimaryProfileId := ""
global gSecondaryProfileId := ""
; Accounts spec prefixes, shared by the store, the menus and Settings.
global LAUNCH_ACCOUNTS_NEXTFREE := ""      ; whatever the roster hands out
global LAUNCH_ACCOUNTS_GROUP := "group:"   ; every row tagged with this group
global LAUNCH_ACCOUNTS_START := "start:"   ; count rows from this row onward
global LAUNCH_ACCOUNTS_ONLY := "only:"     ; exactly this one row
; Interface mode ([UI] in config.ini). WebView2 preserves the polished tray,
; settings and radial client picker; native keeps the process Chromium-free.
global gInterfaceMode := "webview"
; Whether the WebView2 Runtime can actually be used, which gInterfaceMode alone
; cannot say: "" (not probed yet), "ok", "missing" (the probe found none) or
; "failed" (a window could not be built anyway). Never persisted, so installing
; the runtime brings enhanced mode back on the next start. Info is the version,
; or the HRESULT and reason, for the log and diagnostics.
global gWebViewRuntimeState := ""
global gWebViewRuntimeInfo := ""
global gWebViewFallbackNotified := false
; Accent scheme ([UI] AccentScheme). Dark surfaces and semantic status colours
; remain fixed; this only selects the shared highlight palette.
global gAccentScheme := "amber"
; Settings window handle, for the single-instance guard.
global gSettingsGui := 0
; Set by _Settings_BuildNative to a closure over its addon field controls, so
; _Settings_PushAddonFieldValues can drive the native panel the same way it
; posts to the WebView2 one. 0 whenever the native panel is not the live
; frontend, which is how that function tells the two apart.
global gSettingsNativeFieldSetter := 0
; addonName → Map(fieldId → {type, value}) as the open Settings window rendered
; it. Save compares against this so an addon is only handed what the user
; changed — see _Settings_ChangedAddonValues. Cleared on open and close.
global gSettingsRenderedAddonValues := Map()
; Monitor overrides for the "primary"/"secondary" launch targets. 0 = auto:
; primary follows the OS primary display, secondary picks the first non-primary
; display. Set to a 1-based monitor index to pin either target — this is how
; users with more than two displays choose which monitors to use.
global gPrimaryMonitorOverride := 0
global gSecondaryMonitorOverride := 0

; === Caches ===
; Calibration: maps\calibration.ini only (one [Section] per map name).
global gCalibrationCache := Map()
global gImageDimsCache := Map()

; === Mutable state ===
global gOverlayVisible := false
global gCurrentMapName := ""
global gCurrentMapPath := ""
global gGui := 0
global gPic := 0
; Position marker image (same Gui as the map) — marker.png next to the script.
global gMarkerDot := 0
global gCanOverride := false
global gResolvedMapName := ""
global gResolvedMapPath := ""
global gLastReadStatus := "init"
global gLastPosStatus := "init"
global gLastRawX := 0
global gLastRawY := 0
global gCalibrationPoint1 := 0
global gCalibrationPoint2 := 0
; Last focused main.exe window — used so memory reads match the right instance when several are open.
global gTrackedGameHwnd := 0
; Cached process handle / module base — avoids reopening on every tick.
global gCachedPID := 0
global gCachedProcessHandle := 0
global gCachedModuleBase := 0

; === NPC Generator ===
global NPC_OUTPUT_FILE := A_ScriptDir "\npc_generated.txt"
global NPC_ID_START := 0x80020000
global gNpcNextId := NPC_ID_START

; === Signature-based RVA discovery ===
; Hardcoded RVAs that get discovered and resolved at runtime. The values here
; serve two purposes: bootstrap input for signature capture (Ctrl+Alt+S), and
; runtime fallback when no cache or signature is available.
global SIGNATURE_NAMES := ["MAP_FILE_OFFSET", "MAP_NAME_OFFSET", "POS_X_OFFSET", "POS_Y_OFFSET", "GAME_STATE_OFFSET", "BATTLE_STATE_OFFSET", "CHAR_CLASS_OFFSET", "CHAR_VITALS_OFFSET", "BATTLE_ACTOR_BASE",
    "MINIMAP_INDEX_OFFSET", "MINIMAP_GATE_OFFSET", "MINIMAP_ENABLE_FLAG_OFFSET", "MINIMAP_TIER_OFFSET", "MINIMAP_WINDOW_OFFSET"]
global gFallbackOffsets := Map("MAP_FILE_OFFSET", MAP_FILE_OFFSET, "MAP_NAME_OFFSET", MAP_NAME_OFFSET, "POS_X_OFFSET", POS_X_OFFSET, "POS_Y_OFFSET", POS_Y_OFFSET, "GAME_STATE_OFFSET", GAME_STATE_OFFSET, "BATTLE_STATE_OFFSET", BATTLE_STATE_OFFSET, "CHAR_CLASS_OFFSET", CHAR_CLASS_OFFSET, "CHAR_VITALS_OFFSET", CHAR_VITALS_OFFSET, "BATTLE_ACTOR_BASE", BATTLE_ACTOR_BASE_OFFSET,
    ; The game's own minimap (see MINIMAP_INDEX_OFFSET). Each is read by game
    ; code at a fixed address, so Calibrate Signatures can capture all five.
    "MINIMAP_INDEX_OFFSET", MINIMAP_INDEX_OFFSET, "MINIMAP_GATE_OFFSET", MINIMAP_GATE_OFFSET,
    "MINIMAP_ENABLE_FLAG_OFFSET", MINIMAP_ENABLE_FLAG_OFFSET, "MINIMAP_TIER_OFFSET", MINIMAP_TIER_OFFSET,
    "MINIMAP_WINDOW_OFFSET", MINIMAP_WINDOW_OFFSET)
; Offsets that cannot be captured as byte signatures — they are data strings
; with no abs32 code reference for the scanner to latch onto — but that sit at
; a fixed delta from an offset that can. Once the source resolves, these follow.
; delta is taken from the constants above, so editing those keeps it correct.
; validate(handle, addr) → "ok" | "unknown" | "bad" (see ValidateMapNameRva).
global DERIVED_OFFSETS := Map(
    "MAP_NAME_OFFSET", { from: "MAP_FILE_OFFSET", delta: MAP_NAME_OFFSET - MAP_FILE_OFFSET, validate: ValidateMapNameRva },
    "CHAR_VITALS_OFFSET", { from: "CHAR_CLASS_OFFSET", delta: CHAR_VITALS_OFFSET - CHAR_CLASS_OFFSET, validate: ValidateCharVitalsRva }
)
; INIs live next to the script when writable; otherwise %AppData% so users who
; install under Program Files (or whose folder is locked by AV/Controlled Folder
; Access) don't hit "Access denied" on writes.
global SIGNATURES_INI := ResolveWritableIniPath("signatures.ini")
global OFFSETS_CACHE_INI := ResolveWritableIniPath("offsets_cache.ini")
; name (string) → RVA (Integer). Populated lazily once per process attach.
global gResolvedOffsets := Map()
; PE TimeDateStamp (UInt) of the build that gResolvedOffsets was resolved against.
global gResolvedBuildStamp := 0

; === Client snapshots ===
; One shared poll of every running client (map, position, state, battle) that
; per-client addons consume through the OnSnapshot hook, instead of each one
; opening the game process on its own tick.
global gClientHandles := Map()      ; pid → {ok, handle, modBase}
global gClientSnapshots := []       ; result of the last poll
global CLIENT_SNAPSHOT_INTERVAL := 1000
global GAME_STATE_READY := 5        ; below this a client is still loading/at login
global GAME_STATE_WORLD := 10       ; in the overworld (minimap-eligible)

; === Addon system ===
global gAddonHooks := []
global gDisabledAddons := Map()   ; addon name → true when disabled
global ADDONS_DIR := A_ScriptDir "\addons"
global ADDONS_INCLUDE_FILE := A_ScriptDir "\_addons.ahk"

; === Diagnostics ===
; Path resolved lazily — Log() is reachable from anywhere, including before the
; launcher config has loaded, so it must not do disk work at include time.
global gLogPath := ""
global LOG_MAX_BYTES := 1048576   ; rotate to a single .1 file past this
global gLogFailed := false        ; a log that can't be written stops trying
; The client mods' own logging, which is a separate thing from the above: the
; core and each feature DLL write tagged osmw_*.<pid>.log files into the game
; folder, one set per client per launch. Off unless someone turns it on — see
; ModLoggingEnabled() in native_mods.ahk: the key is [Debug] in the mods' own
; osmw.ini, not CONFIG_INI.
global MOD_LOG_SECTION := "Debug"
global MOD_LOG_KEY := "ModLogging"
; Per (addon, hook) failure tracking for FireAddonHook. An addon that throws on
; a 1 Hz hook must not spam the user, and must not throw forever unnoticed.
global gAddonFailCounts := Map()  ; "Addon|Hook" → consecutive failures
global gAddonFailLastTip := Map() ; "Addon|Hook" → tick of the last TrayTip
global ADDON_FAIL_TIP_MS := 30000        ; at most one notification per key
global ADDON_FAIL_QUARANTINE := 10       ; consecutive failures before disabling
; Session-only quarantine, deliberately not persisted to config.ini — a restart
; gives a disabled-by-error addon another chance.
global gQuarantinedAddons := Map()

; === Version check ===
; Passive only: fetches a small manifest and says a newer build exists. Never
; downloads, never installs, never sends anything about the user. An empty URL
; makes the whole feature inert and hides its Settings toggle.
; The manifest's shape and hosting rules are documented in web\README.md.
global VERSION_CHECK_URL := "https://osmw.net/mapsplusplus/version.json"
; Where "Get the update" sends people. Deliberately compiled in rather than read
; from the manifest: a URL that arrives over the network and gets handed to
; Run() is a remote code execution vector, and server-side flexibility here is
; not worth that.
global VERSION_DOWNLOAD_URL := "https://osmw.net/mapsplusplus/"
global VERSION_CHECK_TIMEOUT_MS := 5000
global VERSION_NOTES_MAX := 100
global gVersionCheckEnabled := true
; Set once a newer version is seen, so the tray can offer it and About can
; mention it. "" means up to date or not yet checked.
global gUpdateVersion := ""
global gUpdateNotes := ""
