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

**`pwsh ./build.ps1` is the only reliable syntax gate.** Two traps make the obvious alternatives lie:

- `AutoHotkey64.exe /validate main.ahk` returns **exit 2 for valid and invalid scripts alike**, and `/ErrorStdOut` prints nothing with it. Its exit code carries no signal — do not gate on it.
- `AutoHotkey64.exe` and `Ahk2Exe.exe` are **GUI-subsystem** binaries, so PowerShell's `&` operator does not wait for them and leaves `$LASTEXITCODE` stale from an earlier command. `build.ps1` already uses `Start-Process -Wait -PassThru` for exactly this reason; anything invoking them directly must do the same, or run them from Bash where `$?` is real.

Ahk2Exe *does* discriminate (valid → exit 0 and an exe; invalid → exit 17, no exe), which is why the build is the gate. Note it is stricter than the interpreter: a script that runs fine under `AutoHotkey64.exe` can still fail to compile — shadowing a built-in function name with a local variable (`mod := ...` against the built-in `Mod()`) is one that does. When a build fails with no message, bisect by stubbing function bodies rather than truncating the file, since truncation removes functions that `RegisterAddon` references and produces a different error.

## Architecture

### Load order and global state

`main.ahk` includes, in order: `variables.ahk` (all global state/constants) → `functions.ahk` (core logic) → `settings.ahk` (+ `settings_native.ahk`) → `radial.ahk` (the shared radial-menu engine) → `hotkeys.ahk` → `_addons.ahk` (generated, conditionally included — see below). Nearly all state is declared as `global` in `variables.ahk` and mutated in place; there is no dependency-injection or module boundary, so when adding state, put constants/config-backed globals there and follow the existing naming (`g`-prefixed mutable globals, UPPER_CASE constants).

`_addons.ahk` is a **generated** file (gitignored-style scratch, not hand-edited) listing `#Include addons\<file>.ahk` lines. In dev mode, `GenerateAddonIncludes()` regenerates it from whatever is physically in `addons\` and `Reload()`s if it changed. In a compiled release, it's frozen to whatever `build.ps1` wrote for that variant and never regenerated (`!A_IsCompiled` guards the regen call in `main.ahk`).

### Reading the game process

Everything the app knows about the game comes from `ReadProcessMemory` calls against `main.exe`, at RVAs (offsets from the module base — never fixed absolute addresses, since ASLR/relocation differs per process). The offsets are defined in two layers:

1. **Hardcoded fallback constants** in `variables.ahk` (`MAP_FILE_OFFSET`, `POS_X_OFFSET`, `GAME_STATE_OFFSET`, etc.) and per-addon via `RegisterAddonOffset(name, fallbackRva)`.
2. **Signature-based resolution** (`functions.ahk`, ~line 2280+): byte-pattern signatures captured once (Tray → Debug → **Calibrate Signatures**) let the app re-derive the correct RVA after a game client update shifts addresses, without shipping a new build. `GetResolvedOffset(name)` returns the live-resolved value if available, else the fallback constant. `DERIVED_OFFSETS` expresses offsets that have no independent signature but sit at a fixed byte delta from one that does (e.g. `MAP_NAME_OFFSET` from `MAP_FILE_OFFSET`).

When editing an offset or adding a new memory read, update the fallback constant and consider whether it needs signature support (`RegisterAddonOffset` for addon-owned offsets, or add to `SIGNATURE_NAMES`/`gFallbackOffsets` for core ones) — a raw offset with no fallback/signature story will break silently on the next game patch.

`gClientSnapshots` (in `variables.ahk`) is a single shared poll (`UpdateClientSnapshots()`, every `CLIENT_SNAPSHOT_INTERVAL` ms) of every running `main.exe`: map, position, game/battle state, character class. Per-client addons (roster, party markers, discord RPC) consume this via the `OnSnapshot` hook instead of each opening the process independently — follow this pattern rather than adding another independent poll loop.

### Minimap overlay lifecycle

The overlay (`gGui`, built in `main.ahk`'s `ShowOrToggleOverlay`) is a borderless always-on-top `+E0x08000000` (`WS_EX_NOACTIVATE`) window so it never steals game focus. `UpdateMapState()` runs on a 200ms timer, reads the current map name from memory, resolves it to an image in `maps\`, and auto-closes the overlay when the minimap becomes disallowed (`IsMinimapAllowed()`: battle, loading screen, unsupported/unreadable zone). Calibration (world position → overlay pixel) is per-map data in `maps\calibration.ini`, always in **base** (unscaled 400×300) map space — the user's `Scale` setting is applied only at draw time (`MinimapScaleFactor()`), so calibrating at any zoom level produces the same stored numbers.

### Calibration panel

`ShowCalibrationPanel()` (`main.ahk`, beside the calibration handlers) is a frontend over the existing two-point flow — `CaptureCalibrationPoint` and `ApplyCalibrationFromPoints` are unchanged. Capturing stays on `Ctrl+Alt+1`/`2` rather than becoming buttons **because the capture reads the cursor's position over the minimap**: a button would move the mouse off the point being captured. The panel renders live state pushed on a `CALIB_PUSH_MS` timer and sends back only `apply`/`reset`. `_Calib_State()` builds everything both frontends show, so the WebView2 page and the native fallback cannot drift.

### Radial menus

`radial.ahk` is a core, addon-agnostic engine for the ring-of-buttons-at-the-cursor menus, rendered by the single WebView2 page in `ui\radial\`. Two rings ship on it: the client switcher (`addons\client_roster.ahk`) and Quick Actions (`addons\quick_actions.ahk`). An addon calls `RadialRegister(Map("name", ..., "getItems", fn, "onSelect", fn, ...))` in its `OnInit` and drives it with `RadialToggle/Open/Close/Refresh(name)`; it supplies items and decides what a click means, and owns nothing else.

The engine's two unusual mechanisms are both documented at length in the file header and must not be "simplified" away: the ring window is colour-key transparent, which takes three things together (controller `DefaultBackgroundColor := 0`, the host Static painted via `WM_CTLCOLORSTATIC`, and `WinSetTransColor`) and is silently opaque if any is missing; and because a colour-keyed window is click-through everywhere, **no mouse event ever reaches the page** — instead the page posts a hit map of its own measured layout and AHK does the pointing (hover on a timer, clicks via dynamically registered `Hotkey("LButton")`, not `#HotIf`).

Consequences to respect when adding a ring: only one may be on screen at a time (the mouse grab is process-global — `RadialOpen` enforces this), per-ring state like the hit map lives on the ring object rather than in a global (every page posts into the same handler, so a background push would otherwise clobber the live ring), and rings are built once and parked off-screen while *shown*, never hidden. Item `icon` names must exist in the subsetted Material Symbols font — see the header of `ui\common\style.css`.

### Versioning, logging and diagnostics

`APP_VERSION` in `variables.ahk` is the single source of truth. `main.ahk` carries a matching `;@Ahk2Exe-SetVersion` literal so the compiled exe's file properties agree, and `build.ps1`'s `Get-AppVersion` **fails the build** if the two drift — Ahk2Exe cannot read the constant, so the check is what keeps them honest. Bump both together.

`Log(level, source, message)` (+ `LogInfo`/`LogWarn`/`LogError`) in `functions.ahk` writes one line per entry to `mapsplusplus.log`, resolved through `ResolveWritableIniPath()` so it follows the same next-to-exe-or-`%AppData%` rule as the inis. It rotates to a single `.log.1` past `LOG_MAX_BYTES` and silently gives up for the session if it ever fails to write — logging must never become the thing that breaks the app. Anything that currently only raises a TrayTip should log too; a notification the user missed is not a record.

`FireAddonHook` no longer lets a broken addon shout forever: failures are counted per `(addon, hook)`, every one is logged, the user is notified at most once per `ADDON_FAIL_TIP_MS`, and after `ADDON_FAIL_QUARANTINE` consecutive failures on the same hook the addon is put into `gDisabledAddons` for the session. That quarantine is deliberately **not** persisted to `config.ini`, so a restart gives it another chance. A hook that succeeds clears its own streak.

`BuildDiagnosticsReport()` is what beta bug reports should carry — it identifies the build, dumps every `SIGNATURE_NAMES` entry with its `OffsetSourceLabel()` verdict, and includes the addon states and log tail. When adding a subsystem worth triaging, add it there rather than to `ShowDebugState()`.

`VERSION_CHECK_URL` points at the manifest on osmw.net (shape and hosting rules in `web/README.md`; `web/` is not shipped and `build.ps1` does not copy it). This is the app's **only** network request and its only untrusted input — it sends nothing about the user, and `_ParseVersionManifest` validates every field before anything reaches a notification. `VERSION_DOWNLOAD_URL` is compiled in and deliberately **not** read from the manifest: a URL that arrives over the network and gets handed to `Run()` is a remote-code-execution vector. Preserve both properties; TUTORIAL.md promises them to users.

Because that manifest is parsed by `_JSON_Parse` (`settings.ahk`), the hand-rolled parser now takes input from outside the app. Its object/array loops carry explicit end-of-string bail-outs — without them a truncated body (`{"version":`) spins forever and hangs the whole single-threaded app. Keep those guards if you touch it, and treat any new caller that feeds it non-local data as a reason to re-check them.

### Addon system

Addons are self-contained `.ahk` files in `addons\`, each calling `RegisterAddon(Map("name", ..., "OnXxx", handlerFn, ...))` at include-time (global scope) to register hook callbacks, and typically `RegisterHotkeyAction(...)` for their own hotkeys. `FireAddonHook(hookName, params*)` (functions.ahk) dispatches to every enabled addon that registered that hook name, catching and reporting exceptions per-addon so one broken addon doesn't take down the dispatch loop. Hooks fired by core code include `OnInit`, `OnTrayMenu`, `OnMapChange`, `OnOverlayShow`/`OnOverlayHide`, `OnSnapshot`. `OnInit` is fired once from `main.ahk` right after `LoadAddonEnabledStates()` — that position matters, because it is after `LoadLauncherConfig()` sets `gInterfaceMode` (so an addon's `OnInit` can build the right frontend) and after the enabled/disabled state is known (so a disabled addon's `OnInit` is skipped). Load your addon's config there, not at include time, and never block in it — `SetTimer(fn, -N)` anything that opens a window or waits. Addons are enabled/disabled per-name via `config.ini [Addons]`, toggled from Settings → Addons or `SetAddonEnabled()`; a disabled addon's hooks are skipped and its hotkeys drop out on the next `ApplyAllHotkeys()`.

When adding a new addon: create `addons\<name>.ahk`, call `RegisterAddon` with a unique `name`, add it to the relevant `variants\*.txt` manifest(s) it should ship in, and if it reads game memory, register its offset via `RegisterAddonOffset`.

### Hotkey system

All rebindable hotkeys go through a central registry in `hotkeys.ahk`: `RegisterHotkeyAction(Map("id", ..., "default", chord, "handler", fn, ...))`. An empty default or saved chord means the action is deliberately unbound; `ApplyAllHotkeys()` skips it. The function tears down and rebinds everything else (needed after Settings save or addon enable/disable toggling which actions are live) using AHK v2's dynamic `Hotkey()` API rather than static `#HotIf`/hotkey directives. Context (when the hotkey is active) is expressed via `hotIfWinActive` (bound to `GAME_WIN_FILTER`) or `hotIfFn` (a `HotIf()` predicate callback) on the action spec — see `HotIfToggleMinimap` for an example combining overlay state and map eligibility. User choices live in `config.ini [Hotkeys]`; a blank value preserves an explicit unbound choice. `gHotkeyReserved` in `hotkeys.ahk` is a hand-maintained list of fixed, non-rebindable chords — it has no compile-time link to the actual fixed bindings, so a new fixed hotkey must be added to both places.

### Two interface modes (Native vs WebView2)

`gInterfaceMode` (`config.ini [UI]`) picks between two parallel frontends for every GUI surface (tray menu, settings, client roster):

- **WebView2 ("enhanced")**: renders HTML/CSS/JS panels under `ui\<panel>\` inside a `WebViewGui` (from `Lib\WebViewToo.ahk`, using `Lib\{32bit,64bit}\WebView2Loader.dll`). AHK ↔ JS communication is one hand-rolled JSON protocol per panel: AHK calls `gui.PostWebMessageAsJson('{"type":"...",...}')` to push state, and JS posts back via `window.chrome.webview.postMessage`, received AHK-side through `WebMessageReceived` → a per-panel `_OnWebMessage` handler that switches on `msg["type"]`. There's no shared client-side framework; each `ui\<panel>\main.js` is vanilla JS. Windows are built once and parked off-screen (`x-30000 y-30000`) rather than destroyed/recreated between opens, because standing up a WebView2 + loading a page costs hundreds of ms — see the tray menu's `_EnsureWebTrayGui`/`_RevealWebTrayMenu`/`_CloseWebTrayMenu` in `main.ahk` for the canonical park/reveal/dismiss pattern to copy for new WebView2 panels.
- **Native ("low memory")**: plain Win32/AHK `Gui` controls (`settings_native.ahk`, and list-based fallbacks like the client roster's list view), no Chromium subprocess. If `Lib\WebViewToo.ahk` or the relevant `ui\...\index.html` is missing, WebView2 surfaces fall back to native automatically (see `_Settings_CanUseWebView()`).

Both frontends for a given panel (e.g. `settings.ahk`'s `_Settings_BuildWebView` vs `_Settings_BuildNative`) must feed the same underlying state/save functions — when changing a setting's behavior, update the shared read/write logic, not just one frontend.

Shared web UI styling lives in `ui\common\style.css` (one dark theme via CSS custom properties, used by every panel — see `.impeccable.md` for the design system: dense table/form-heavy utility UI, no light theme, practical rather than WCAG-graded accessibility). Don't hardcode one-off colors per panel; extend the shared custom properties instead.

### Config and data files

- `config.ini` — user settings (`[Launcher]`, `[Minimap]`, `[Hotkeys]`, `[MapPois]`, `[Addons]`, `[UI]`, `[WindowLayout]`); written next to the exe or falls back to `%AppData%` via `ResolveWritableIniPath()` if that directory isn't writable (e.g. under Program Files, or Controlled Folder Access). Full key reference is in [TUTORIAL.md](TUTORIAL.md#configuration-configini).
- `maps\calibration.ini` — per-map world→pixel calibration (base 400×300 space).
- `maps\pois.ini` — user/curated map points of interest, keyed by map id, storing **raw** game positions. Raw → the coordinates the game displays is `(raw + offset) / divisor` — `GAME_COORD_OFFSET_X/Y` (400/300) and `GAME_COORD_DIV_X/Y` (16/8) in `variables.ahk`, via `GameCoordX/Y()`. The offsets are not optional: without them every displayed coordinate is low by 25 on X and 37 on Y. They were measured by teleporting to known coordinates and reading raw back, and confirmed identical on a second map, which is why they are global rather than per-map calibration.
- `layouts.ini` — user-authored window layouts (owned by `addons\window_layout.ahk`), also via `ResolveWritableIniPath()`. An `[Index]` section maps a numeric id to the display name, and one `[Layout.<id>]` section per layout holds a monitor fingerprint, the authoring client window size, and `Slot<N>=fx|fy|charName|flags` records. Slot positions are **fractions of the target monitor's work area**, not pixels, so a layout survives a resolution change; `WinSize` is used only to draw boxes to scale in the editor, since applying a layout never resizes a window. Deliberately *not* in `maps\`, which `build.ps1` copies into `releases\`.
- `better_hotkeys.ini` — explicit per-character skill profiles owned by `addons\better_hotkeys.ahk`. Dynamic profile chords are transient hidden entries in the central registry, rebuilt through `OnBeforeApplyHotkeys`; they must not be persisted into `config.ini [Hotkeys]` or exposed in the general Settings hotkey list.
- `signatures.ini` / `offsets_cache.ini` — persisted byte-signatures and per-build resolved RVAs from the signature calibration system above.
- `mapsplusplus.log` (+ `.log.1`) — the rolling log, and `mapsplusplus.log.diagnostics.txt` written by **Copy Diagnostics**. Same `ResolveWritableIniPath()` location rule as the inis. Both are output only; nothing reads them back except `LogTail()`.

**Any save path that writes user text to an ini must call `EnsureIniUtf16(path)` before its first `IniWrite`/`IniDelete`** (`functions.ahk`, near `ResolveWritableIniPath`). Windows fixes an ini's encoding from what is already on disk, and AHK only creates a UTF-16 one when `IniWrite` is what creates the file — a store whose first call is `IniDelete` (deleting a section it is about to rewrite, as `_Pois_Save` and `_BH_SaveProfiles` do) instead gets an ANSI file, and from then on every write through it silently replaces characters outside the system codepage with `?`. Character names and POI labels are exactly the affected values.
- `maps\*.jpg` — one custom minimap image per supported zone, named `MAP<id>.jpg` matching the in-game map filename.

### Variant system

Three shipped builds (`variants\full.txt`, `lite.txt`, `battle.txt`) are just addon-file lists consumed by `build.ps1`: Full has everything, Lite drops combat/inventory/chat/roster addons, Battle drops inventory/window-layout. Core (`main.ahk`/`functions.ahk`/`settings*.ahk`/`hotkeys.ahk`) is identical across variants — only the addon set differs.
