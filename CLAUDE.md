# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

**osMW Maps++** is an AutoHotkey v2 companion app for *MythWar Online* (osMW private server). It reads the running game client's process memory to overlay a custom minimap, track a live position marker, and provide multi-boxing tools (window layout, chat/inventory helpers, battle command broadcasting, a client roster, party markers, map POI editing). See [TUTORIAL.md](TUTORIAL.md) for the full end-user feature/hotkey/config reference — read it when a task touches user-facing behavior, since it documents the intended contract for each feature.

Windows-only. Requires AutoHotkey v2.0.

## Build / run

- Run uncompiled: open `main.ahk` with AutoHotkey v2 (or `AutoHotkey64.exe main.ahk`). There is no package manager or test framework — this is not a Node/Python project.
- Build release variants: `pwsh ./build.ps1` (all variants) or `pwsh ./build.ps1 -Variant full` (one variant: `full`, `lite`, or `battle`, per `variants\*.txt`). Requires `Ahk2Exe.exe` (probes `$env:AHK2EXE` then standard AutoHotkey install paths, or pass `-Ahk2ExePath`).
  - The build writes `_addons.ahk` from the variant's manifest (list of `addons\*.ahk` filenames), compiles `main.ahk` → `releases\<variant>\mapsplusplus.exe`, then copies `marker.png`, `maps\`, `ui\`, `avatars\`, `Lib\` alongside it, and restores your dev-mode `_addons.ahk` afterward.
  - `-Clean` wipes `releases\` first. Manifests list one addon filename per line (`#` comments allowed); an addon referenced by a manifest but missing from `addons\` fails the build.
- There are no automated tests. Verification is manual: run against the live game client, or use the in-app debug tools (Tray → Debug: **Debug State**, **Calibrate Signatures**, **Verify Signatures**) described in TUTORIAL.md.

## Architecture

### Load order and global state

`main.ahk` includes, in order: `variables.ahk` (all global state/constants) → `functions.ahk` (core logic) → `settings.ahk` (+ `settings_native.ahk`) → `hotkeys.ahk` → `_addons.ahk` (generated, conditionally included — see below). Nearly all state is declared as `global` in `variables.ahk` and mutated in place; there is no dependency-injection or module boundary, so when adding state, put constants/config-backed globals there and follow the existing naming (`g`-prefixed mutable globals, UPPER_CASE constants).

`_addons.ahk` is a **generated** file (gitignored-style scratch, not hand-edited) listing `#Include addons\<file>.ahk` lines. In dev mode, `GenerateAddonIncludes()` regenerates it from whatever is physically in `addons\` and `Reload()`s if it changed. In a compiled release, it's frozen to whatever `build.ps1` wrote for that variant and never regenerated (`!A_IsCompiled` guards the regen call in `main.ahk`).

### Reading the game process

Everything the app knows about the game comes from `ReadProcessMemory` calls against `main.exe`, at RVAs (offsets from the module base — never fixed absolute addresses, since ASLR/relocation differs per process). The offsets are defined in two layers:

1. **Hardcoded fallback constants** in `variables.ahk` (`MAP_FILE_OFFSET`, `POS_X_OFFSET`, `GAME_STATE_OFFSET`, etc.) and per-addon via `RegisterAddonOffset(name, fallbackRva)`.
2. **Signature-based resolution** (`functions.ahk`, ~line 2280+): byte-pattern signatures captured once (Tray → Debug → **Calibrate Signatures**) let the app re-derive the correct RVA after a game client update shifts addresses, without shipping a new build. `GetResolvedOffset(name)` returns the live-resolved value if available, else the fallback constant. `DERIVED_OFFSETS` expresses offsets that have no independent signature but sit at a fixed byte delta from one that does (e.g. `MAP_NAME_OFFSET` from `MAP_FILE_OFFSET`).

When editing an offset or adding a new memory read, update the fallback constant and consider whether it needs signature support (`RegisterAddonOffset` for addon-owned offsets, or add to `SIGNATURE_NAMES`/`gFallbackOffsets` for core ones) — a raw offset with no fallback/signature story will break silently on the next game patch.

`gClientSnapshots` (in `variables.ahk`) is a single shared poll (`UpdateClientSnapshots()`, every `CLIENT_SNAPSHOT_INTERVAL` ms) of every running `main.exe`: map, position, game/battle state, character class. Per-client addons (roster, party markers, discord RPC) consume this via the `OnSnapshot` hook instead of each opening the process independently — follow this pattern rather than adding another independent poll loop.

### Minimap overlay lifecycle

The overlay (`gGui`, built in `main.ahk`'s `ShowOrToggleOverlay`) is a borderless always-on-top `+E0x08000000` (`WS_EX_NOACTIVATE`) window so it never steals game focus. `UpdateMapState()` runs on a 200ms timer, reads the current map name from memory, resolves it to an image in `maps\`, and auto-closes the overlay when the minimap becomes disallowed (`IsMinimapAllowed()`: battle, loading screen, unsupported/unreadable zone). Calibration (world position → overlay pixel) is per-map data in `maps\calibration.ini`, always in **base** (unscaled 400×300) map space — the user's `Scale` setting is applied only at draw time (`MinimapScaleFactor()`), so calibrating at any zoom level produces the same stored numbers.

### Addon system

Addons are self-contained `.ahk` files in `addons\`, each calling `RegisterAddon(Map("name", ..., "OnXxx", handlerFn, ...))` at include-time (global scope) to register hook callbacks, and typically `RegisterHotkeyAction(...)` for their own hotkeys. `FireAddonHook(hookName, params*)` (functions.ahk) dispatches to every enabled addon that registered that hook name, catching and reporting exceptions per-addon so one broken addon doesn't take down the dispatch loop. Hooks fired by core code include `OnInit`, `OnTrayMenu`, `OnMapChange`, `OnOverlayShow`/`OnOverlayHide`, `OnSnapshot`. `OnInit` is fired once from `main.ahk` right after `LoadAddonEnabledStates()` — that position matters, because it is after `LoadLauncherConfig()` sets `gInterfaceMode` (so an addon's `OnInit` can build the right frontend) and after the enabled/disabled state is known (so a disabled addon's `OnInit` is skipped). Load your addon's config there, not at include time, and never block in it — `SetTimer(fn, -N)` anything that opens a window or waits. Addons are enabled/disabled per-name via `config.ini [Addons]`, toggled from Settings → Addons or `SetAddonEnabled()`; a disabled addon's hooks are skipped and its hotkeys drop out on the next `ApplyAllHotkeys()`.

When adding a new addon: create `addons\<name>.ahk`, call `RegisterAddon` with a unique `name`, add it to the relevant `variants\*.txt` manifest(s) it should ship in, and if it reads game memory, register its offset via `RegisterAddonOffset`.

### Hotkey system

All rebindable hotkeys go through a central registry in `hotkeys.ahk`: `RegisterHotkeyAction(Map("id", ..., "default", chord, "handler", fn, ...))`. `ApplyAllHotkeys()` tears down and rebinds everything (needed after Settings save or addon enable/disable toggling which actions are live) using AHK v2's dynamic `Hotkey()` API rather than static `#HotIf`/hotkey directives. Context (when the hotkey is active) is expressed via `hotIfWinActive` (bound to `GAME_WIN_FILTER`) or `hotIfFn` (a `HotIf()` predicate callback) on the action spec — see `HotIfToggleMinimap` for an example combining overlay state and map eligibility. User overrides live in `config.ini [Hotkeys]`, one entry per **rebound** action (unbound = default). `gHotkeyReserved` in `hotkeys.ahk` is a hand-maintained list of fixed, non-rebindable chords — it has no compile-time link to the actual fixed bindings, so a new fixed hotkey must be added to both places.

### Two interface modes (Native vs WebView2)

`gInterfaceMode` (`config.ini [UI]`) picks between two parallel frontends for every GUI surface (tray menu, settings, client roster):

- **WebView2 ("enhanced")**: renders HTML/CSS/JS panels under `ui\<panel>\` inside a `WebViewGui` (from `Lib\WebViewToo.ahk`, using `Lib\{32bit,64bit}\WebView2Loader.dll`). AHK ↔ JS communication is one hand-rolled JSON protocol per panel: AHK calls `gui.PostWebMessageAsJson('{"type":"...",...}')` to push state, and JS posts back via `window.chrome.webview.postMessage`, received AHK-side through `WebMessageReceived` → a per-panel `_OnWebMessage` handler that switches on `msg["type"]`. There's no shared client-side framework; each `ui\<panel>\main.js` is vanilla JS. Windows are built once and parked off-screen (`x-30000 y-30000`) rather than destroyed/recreated between opens, because standing up a WebView2 + loading a page costs hundreds of ms — see the tray menu's `_EnsureWebTrayGui`/`_RevealWebTrayMenu`/`_CloseWebTrayMenu` in `main.ahk` for the canonical park/reveal/dismiss pattern to copy for new WebView2 panels.
- **Native ("low memory")**: plain Win32/AHK `Gui` controls (`settings_native.ahk`, and list-based fallbacks like the client roster's list view), no Chromium subprocess. If `Lib\WebViewToo.ahk` or the relevant `ui\...\index.html` is missing, WebView2 surfaces fall back to native automatically (see `_Settings_CanUseWebView()`).

Both frontends for a given panel (e.g. `settings.ahk`'s `_Settings_BuildWebView` vs `_Settings_BuildNative`) must feed the same underlying state/save functions — when changing a setting's behavior, update the shared read/write logic, not just one frontend.

Shared web UI styling lives in `ui\common\style.css` (one dark theme via CSS custom properties, used by every panel — see `.impeccable.md` for the design system: dense table/form-heavy utility UI, no light theme, practical rather than WCAG-graded accessibility). Don't hardcode one-off colors per panel; extend the shared custom properties instead.

### Config and data files

- `config.ini` — user settings (`[Launcher]`, `[Minimap]`, `[Hotkeys]`, `[MapPois]`, `[Addons]`, `[UI]`, `[WindowLayout]`); written next to the exe or falls back to `%AppData%` via `ResolveWritableIniPath()` if that directory isn't writable (e.g. under Program Files, or Controlled Folder Access). Full key reference is in [TUTORIAL.md](TUTORIAL.md#configuration-configini).
- `maps\calibration.ini` — per-map world→pixel calibration (base 400×300 space).
- `maps\pois.ini` — user/curated map points of interest, keyed by map id, storing **raw** game positions (raw X÷16, Y÷8 = the in-game displayed coordinates).
- `layouts.ini` — user-authored window layouts (owned by `addons\window_layout.ahk`), also via `ResolveWritableIniPath()`. An `[Index]` section maps a numeric id to the display name, and one `[Layout.<id>]` section per layout holds a monitor fingerprint, the authoring client window size, and `Slot<N>=fx|fy|charName|flags` records. Slot positions are **fractions of the target monitor's work area**, not pixels, so a layout survives a resolution change; `WinSize` is used only to draw boxes to scale in the editor, since applying a layout never resizes a window. Deliberately *not* in `maps\`, which `build.ps1` copies into `releases\`.
- `signatures.ini` / `offsets_cache.ini` — persisted byte-signatures and per-build resolved RVAs from the signature calibration system above.
- `maps\*.jpg` — one custom minimap image per supported zone, named `MAP<id>.jpg` matching the in-game map filename.

### Variant system

Three shipped builds (`variants\full.txt`, `lite.txt`, `battle.txt`) are just addon-file lists consumed by `build.ps1`: Full has everything, Lite drops combat/inventory/chat/roster addons, Battle drops inventory/window-layout. Core (`main.ahk`/`functions.ahk`/`settings*.ahk`/`hotkeys.ahk`) is identical across variants — only the addon set differs.
