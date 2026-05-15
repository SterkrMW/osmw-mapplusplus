#Requires AutoHotkey v2.0

; ── Example Addon ─────────────────────────────────────────────────
;
; How to use:
;   Copy or rename this file (or any .ahk file) into the addons\ folder.
;   On the next app start (or Ctrl+Alt+R reload), the addon is loaded
;   automatically. One extra reload will occur if the addon set changed.
;
; IMPORTANT: Do NOT use #SingleInstance or Persistent — this file is
;   #Include'd into main.ahk and shares its process.
;
; Available hooks and their signatures:
;   OnInit()                         called once after the app finishes startup
;   OnMapChange(newMap, prevMap)     called when the resolved map name changes
;                                    newMap is "" when leaving a mapped area
;   OnOverlayShow(mapName)           called when the minimap overlay becomes visible
;   OnOverlayHide()                  called when the minimap overlay is hidden
;   OnTrayMenu(trayMenu)             called at startup so you can add tray items
;
; Prefix ALL your functions/variables with your addon name to avoid
; collisions with the host app and other addons.
; ──────────────────────────────────────────────────────────────────

; Register addon-owned memory offsets into the shared rescan system (optional).
; Once registered, Ctrl+Alt+S (Calibrate Signatures) and the per-build offset
; cache cover your offsets too. Use GetResolvedOffset("MyOffset") everywhere
; instead of a hardcoded constant so the live-resolved value is used.
; RegisterAddonOffset("MyOffset", 0x12345678)

RegisterAddon(Map(
    "name",          "ExampleAddon",
    "OnInit",        _ExampleAddon_OnInit,
    "OnMapChange",   _ExampleAddon_OnMapChange,
    "OnOverlayShow", _ExampleAddon_OnOverlayShow,
    "OnOverlayHide", _ExampleAddon_OnOverlayHide,
    "OnTrayMenu",    _ExampleAddon_OnTrayMenu
))

_ExampleAddon_OnInit() {
    ; Called once after the host app finishes startup.
    ; Uncomment the line below to confirm the addon loaded:
    ; TrayTip("ExampleAddon", "Loaded!", "Iconi")
}

_ExampleAddon_OnMapChange(newMap, prevMap) {
    ; newMap  — current resolved map name (e.g. "MAP301.jpg")
    ; prevMap — previous map name, "" on first detection or when leaving all maps
    ; TrayTip("ExampleAddon", "Map: " newMap, "Iconi")
}

_ExampleAddon_OnOverlayShow(mapName) {
    ; mapName — name of the map now displayed in the overlay
}

_ExampleAddon_OnOverlayHide() {
}

_ExampleAddon_OnTrayMenu(trayMenu) {
    ; Add items to the system tray right-click menu.
    ; trayMenu.Add("My Action", (*) => _ExampleAddon_MyAction())
}
