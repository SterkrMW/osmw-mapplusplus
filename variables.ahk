#Requires AutoHotkey v2.0

; === Constants ===
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
; Raw memory position → the coordinates the game shows the player.
global GAME_COORD_DIV_X := 16
global GAME_COORD_DIV_Y := 8
global MAP_DIR := A_ScriptDir "\maps"
; Base map-image space. Every calibration in maps\calibration.ini maps world
; coordinates into *these* pixels, so they must not change with the user's
; display scale — scaling is applied when drawing (see MinimapDisplayW/H).
global OVERLAY_W := 400
global OVERLAY_H := 300
; 1px gold (Gui background) + 1px black ring, then map — total inset per side = 2px.
global MINIMAP_BORDER_GOLD_PX := 1
global MINIMAP_BORDER_BLACK_PX := 1
global MINIMAP_MAP_INSET := MINIMAP_BORDER_GOLD_PX + MINIMAP_BORDER_BLACK_PX
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
; Interface mode ([UI] in config.ini). WebView2 preserves the polished tray,
; settings and radial client picker; native keeps the process Chromium-free.
global gInterfaceMode := "webview"
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
