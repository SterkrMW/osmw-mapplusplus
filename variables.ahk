#Requires AutoHotkey v2.0

; === Constants ===
; The one place the version is written. main.ahk carries a matching
; ;@Ahk2Exe-SetVersion literal so the compiled exe's file properties agree, and
; build.ps1 fails the build if the two ever drift apart.
global APP_VERSION := "0.9.0-beta.1"
global PROCESS_EXE := "main.exe"
global GAME_WIN_FILTER := "ahk_exe " PROCESS_EXE
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
; Character class of the logged-in character, 0-7. Indexes avatars\c<N>.png.
; Only meaningful once the client is past the login screens (GAME_STATE_READY).
global CHAR_CLASS_OFFSET := 0x2AA62C
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
; Each record is BATTLE_ACTOR_STRIDE apart, and carries TWO identical-looking
; blocks of {MaxHP, CurHP, MaxMP, CurMP} — one at +0x00 and a copy at +0x24.
; Which of the two is the live pool is not yet established; Tray > Debug >
; Verify Battle Stats dumps both side by side to settle it against a real fight.
; BATTLE_STATS_BLOCK selects the one everything else reads.
global BATTLE_ACTOR_BASE_OFFSET := 0x2A3D90
global BATTLE_ACTOR_STRIDE := 0x1D0      ; 464 bytes between locations
global BATTLE_PARTY_SLOTS := 10          ; locations 0-9
global BATTLE_TOTAL_SLOTS := 20          ; both sides
global BATTLE_ACTOR_BLOCK_A := 0x00
global BATTLE_ACTOR_BLOCK_B := 0x24
global BATTLE_ACTOR_RECORD_BYTES := 0x34 ; through the end of the second block
; Field order inside either block.
global BATTLE_F_MAXHP := 0x00
global BATTLE_F_HP    := 0x04
global BATTLE_F_MAXMP := 0x08
global BATTLE_F_MP    := 0x0C
; 0 = the block at +0x00, 1 = the copy at +0x24. Provisional until the
; diagnostic says which one moves when damage is taken.
global BATTLE_STATS_BLOCK := 0
; Guards a plainly wrong read (a moved offset after a patch) from being rendered
; as a real health bar.
global BATTLE_HP_SANE_MAX := 1000000
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
global MINIMAP_ANCHORS := ["Center", "TopLeft", "TopRight", "BottomLeft", "BottomRight"]
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
global gGamePath := ""           ; Resolved path to the game executable.
global gGameArgs := ""           ; Optional command-line arguments for the game.
global gLaunchOnStartup := false ; Auto-launch one game instance on minimap startup.
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
; Interface mode ([UI] in config.ini). WebView2 preserves the polished tray,
; settings and radial client picker; native keeps the process Chromium-free.
global gInterfaceMode := "webview"
; Accent scheme ([UI] AccentScheme). Dark surfaces and semantic status colours
; remain fixed; this only selects the shared highlight palette.
global gAccentScheme := "amber"
; Settings window handle, for the single-instance guard.
global gSettingsGui := 0
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
global SIGNATURE_NAMES := ["MAP_FILE_OFFSET", "MAP_NAME_OFFSET", "POS_X_OFFSET", "POS_Y_OFFSET", "GAME_STATE_OFFSET", "BATTLE_STATE_OFFSET", "CHAR_CLASS_OFFSET"]
global gFallbackOffsets := Map("MAP_FILE_OFFSET", MAP_FILE_OFFSET, "MAP_NAME_OFFSET", MAP_NAME_OFFSET, "POS_X_OFFSET", POS_X_OFFSET, "POS_Y_OFFSET", POS_Y_OFFSET, "GAME_STATE_OFFSET", GAME_STATE_OFFSET, "BATTLE_STATE_OFFSET", BATTLE_STATE_OFFSET, "CHAR_CLASS_OFFSET", CHAR_CLASS_OFFSET)
; Offsets that cannot be captured as byte signatures — they are data strings
; with no abs32 code reference for the scanner to latch onto — but that sit at
; a fixed delta from an offset that can. Once the source resolves, these follow.
; delta is taken from the constants above, so editing those keeps it correct.
; validate(handle, addr) → "ok" | "unknown" | "bad" (see ValidateMapNameRva).
global DERIVED_OFFSETS := Map(
    "MAP_NAME_OFFSET", { from: "MAP_FILE_OFFSET", delta: MAP_NAME_OFFSET - MAP_FILE_OFFSET, validate: ValidateMapNameRva }
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
