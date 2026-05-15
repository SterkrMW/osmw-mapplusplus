#Requires AutoHotkey v2.0

; === Constants ===
global PROCESS_EXE := "main.exe"
global GAME_WIN_FILTER := "ahk_exe " PROCESS_EXE
; RVAs from main.exe — do not use fixed absolute addresses (bases differ per process / ASLR).
global MAP_FILE_OFFSET := 0x340EC5
; global POS_X_OFFSET := 0x3049E8
; global POS_Y_OFFSET := 0x3049EC
global POS_X_OFFSET := 0x30B2D0
global POS_Y_OFFSET := 0x30B2D4
global GAME_STATE_OFFSET := 0x34313C
global BATTLE_STATE_OFFSET := 0x301DE4
global MAP_FILE_LEN := 20
global MAP_DIR := A_ScriptDir "\maps"
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

; === Launcher config ===
global CONFIG_INI := A_ScriptDir "\config.ini"
global gGamePath := ""           ; Resolved path to the game executable.
global gGameArgs := ""           ; Optional command-line arguments for the game.
global gLaunchOnStartup := false ; Auto-launch one game instance on minimap startup.

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
global SIGNATURE_NAMES := ["MAP_FILE_OFFSET", "POS_X_OFFSET", "POS_Y_OFFSET", "GAME_STATE_OFFSET", "BATTLE_STATE_OFFSET"]
global gFallbackOffsets := Map("MAP_FILE_OFFSET", MAP_FILE_OFFSET, "POS_X_OFFSET", POS_X_OFFSET, "POS_Y_OFFSET", POS_Y_OFFSET, "GAME_STATE_OFFSET", GAME_STATE_OFFSET, "BATTLE_STATE_OFFSET", BATTLE_STATE_OFFSET)
global SIGNATURES_INI := A_ScriptDir "\signatures.ini"
global OFFSETS_CACHE_INI := A_ScriptDir "\offsets_cache.ini"
; name (string) → RVA (Integer). Populated lazily once per process attach.
global gResolvedOffsets := Map()
; PE TimeDateStamp (UInt) of the build that gResolvedOffsets was resolved against.
global gResolvedBuildStamp := 0

; === Addon system ===
global gAddonHooks := []
global ADDONS_DIR := A_ScriptDir "\addons"
global ADDONS_INCLUDE_FILE := A_ScriptDir "\_addons.ahk"