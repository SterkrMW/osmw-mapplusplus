# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

**osMW Maps++** is an AutoHotkey v2 companion app for *MythWar Online* (osMW private server). It reads the running game client's process memory to overlay a custom minimap, track a live position marker, and provide multi-boxing tools (window layout, chat/inventory helpers, battle command broadcasting, a client roster, party markers, map POI editing). See [TUTORIAL.md](TUTORIAL.md) for the full end-user feature/hotkey/config reference — read it when a task touches user-facing behavior, since it documents the intended contract for each feature.

Windows-only. Requires AutoHotkey v2.0.

## Build / run

- Run uncompiled: open `main.ahk` with AutoHotkey v2 (or `AutoHotkey64.exe main.ahk`). There is no package manager or test framework — this is not a Node/Python project.
- Build release variants: `pwsh ./build.ps1` (all variants) or `pwsh ./build.ps1 -Variant full` (one variant: `full`, `lite`, or `battle`, per `variants\*.txt`). Requires `Ahk2Exe.exe` (probes `$env:AHK2EXE` then standard AutoHotkey install paths, or pass `-Ahk2ExePath`).
  - The build writes `_addons.ahk` from the variant's manifest (list of `addons\*.ahk` filenames), compiles `main.ahk` → `releases\<variant>\mapsplusplus.exe`, then copies `marker.png`, `defaults.ini`, `maps\`, `ui\`, `avatars\`, `Lib\` alongside it, and restores your dev-mode `_addons.ahk` afterward. It ships nothing of the native client mods — no DLLs, no launchers, no ABI payload (see "The native client mods" below); `Lib\` holds only the WebView2 libraries.
  - `-Clean` wipes `releases\` first. Manifests list one addon filename per line (`#` comments allowed); an addon referenced by a manifest but missing from `addons\` fails the build.
- There are no automated tests. Verification is manual: run against the live game client, or use the in-app debug tools (Tray → Debug: **Debug State**, **Calibrate Signatures**, **Verify Signatures**) described in TUTORIAL.md.

**`pwsh ./build.ps1` is the only reliable syntax gate.** Two traps make the obvious alternatives lie:

- `AutoHotkey64.exe /validate main.ahk` returns **exit 2 for valid and invalid scripts alike**, and `/ErrorStdOut` prints nothing with it. Its exit code carries no signal — do not gate on it.
- `AutoHotkey64.exe` and `Ahk2Exe.exe` are **GUI-subsystem** binaries, so PowerShell's `&` operator does not wait for them and leaves `$LASTEXITCODE` stale from an earlier command. `build.ps1` waits on the process object for exactly this reason; anything invoking them directly must do the same, or run them from Bash where `$?` is real.

Ahk2Exe *does* discriminate (valid → exit 0 and an exe; invalid → exit 17, no exe), which is why the build is the gate.

**But a syntax error does not take the exit-17 path — it hangs.** Ahk2Exe opens a modal `Ahk2Exe Error` window naming the file, the line and the offending text, and then waits for someone to click OK. Nothing is written to stdout, so under a plain `Start-Process -Wait` the build blocks forever with an empty log, and the one thing that would tell you what is wrong is sitting in a window off to the side. A backgrounded or CI run never sees it at all. This cost an eight-minute stall before it was understood, and it is *not* the shadowing failure described below — that one exits.

`build.ps1`'s `Invoke-Ahk2Exe` now polls for exit instead of blocking, watches for that window, reads it via UI Automation and rethrows the text as an ordinary build failure (240s backstop). So a syntax error looks like:

```
BUILD FAILED
  Ahk2Exe reported an error:

Error: The script contains syntax errors.

Namely:
...\addons\chat_mod.ahk (526) : ==> Missing "}"
     Specifically: BrokenFn( {
```

If you ever invoke Ahk2Exe yourself rather than through `build.ps1`, wrap it the same way or you will reproduce the hang. When a build does stall, check for an `Ahk2Exe Error` window before assuming anything else.

**Its most common failure here is a variable shadowing a built-in.** `mod`, `local`, `run` and `max` have all caused it — natural-looking names colliding with `Mod()`, the `local` keyword, `Run()` and `Max()`. The build fails with no message while the script still runs fine under `AutoHotkey64.exe`, so the error gives no hint.

`build.ps1` now runs `tools\check-shadowing.ps1` first and refuses to compile with a file and line number instead. Run it directly (`pwsh ./tools/check-shadowing.ps1`) when a build fails mysteriously. If it passes and the build still fails, bisect by stubbing function *bodies* — truncating the file removes functions that `RegisterAddon` references and produces a different error that sends you the wrong way.

**Keep `build.ps1` and `tools\check-shadowing.ps1` pure ASCII.** They have no BOM, and Windows PowerShell 5.1 decodes a BOM-less file as the system codepage, not UTF-8 — so an em dash (`—`, `E2 80 94`) arrives as `â€"`, whose trailing `0x94` is a *smart closing quote* that 5.1 accepts as a string delimiter. Inside a string literal that ends the string early and the script dies at parse time with `The string is missing the terminator`, pointing at a line that looks fine; in a comment it is harmless, which is why the two can coexist for a while. `pwsh` (7+) always assumes UTF-8 and never sees it, so this reproduces only for whoever runs the build under 5.1 — which is what "Run with PowerShell" and a bare `powershell` prompt still use. The prose style everywhere else in this repo makes em dashes the default keystroke, so use `-` in these two files specifically.

`build.ps1` has a script-scope `trap` for the other half of that problem: every failure path is a `throw` under `$ErrorActionPreference = 'Stop'`, which unwinds out of the script and takes the window with it when the script was double-clicked. The trap prints `BUILD FAILED` with the message and origin line, then pauses for a keypress — but only when `[Console]::IsInputRedirected` is false, so a captured or piped run exits instead of hanging forever.

## Architecture

### Load order and global state

`main.ahk` includes, in order: `variables.ahk` (all global state/constants) → `functions.ahk` (core logic) → `native_mods.ahk` (the only door to the client mods' files) → `settings.ahk` (+ `settings_native.ahk`) → `dialogs.ahk` → `radial.ahk` (the shared radial-menu engine) → `hotkeys.ahk` → `_addons.ahk` (generated, conditionally included — see below). Nearly all state is declared as `global` in `variables.ahk` and mutated in place; there is no dependency-injection or module boundary, so when adding state, put constants/config-backed globals there and follow the existing naming (`g`-prefixed mutable globals, UPPER_CASE constants).

`_addons.ahk` is a **generated** file (gitignored-style scratch, not hand-edited) listing `#Include addons\<file>.ahk` lines. In dev mode, `GenerateAddonIncludes()` regenerates it from whatever is physically in `addons\` and `Reload()`s if it changed. In a compiled release, it's frozen to whatever `build.ps1` wrote for that variant and never regenerated (`!A_IsCompiled` guards the regen call in `main.ahk`).

### Reading the game process

Everything the app knows about the game comes from `ReadProcessMemory` calls against `main.exe`, at RVAs (offsets from the module base — never fixed absolute addresses, since ASLR/relocation differs per process). The offsets are defined in two layers:

1. **Hardcoded fallback constants** in `variables.ahk` (`MAP_FILE_OFFSET`, `POS_X_OFFSET`, `GAME_STATE_OFFSET`, etc.) and per-addon via `RegisterAddonOffset(name, fallbackRva)`.
2. **Signature-based resolution** (`functions.ahk`, ~line 2280+): byte-pattern signatures captured once (Tray → Debug → **Calibrate Signatures**) let the app re-derive the correct RVA after a game client update shifts addresses, without shipping a new build. `GetResolvedOffset(name)` returns the live-resolved value if available, else the fallback constant. `DERIVED_OFFSETS` expresses offsets that have no independent signature but sit at a fixed byte delta from one that does (e.g. `MAP_NAME_OFFSET` from `MAP_FILE_OFFSET`).

When editing an offset or adding a new memory read, update the fallback constant and consider whether it needs signature support (`RegisterAddonOffset` for addon-owned offsets, or add to `SIGNATURE_NAMES`/`gFallbackOffsets` for core ones) — a raw offset with no fallback/signature story will break silently on the next game patch.

The battle actor array at `BATTLE_ACTOR_BASE_OFFSET` describes the fight from the **party leader's** seat, identically in every client's memory — location 0 is the leader, not "this client's character". A solo test cannot distinguish the two and once led to the leader's health being shown on every roster row. Per-character health therefore needs **both** sources joined, because each is wrong in a way the other is not: `CHAR_VITALS_OFFSET` (0x1C past `CHAR_CLASS_OFFSET`, holding MaxHP/CurHP/MaxMP/CurMP) is reliably per-character but its *current* values freeze when a fight starts, while the battle array is live but leader-relative. `FindOwnBattleLocation` joins them on the **maxima**, which stay correct in the character block mid-fight — the location whose max HP and max MP both match this character is this character, which also identifies its pet (the next location). An ambiguous match returns nothing rather than guessing. The same array's uselessness for identity makes it ideal for grouping: every client in one fight holds the same array, so `BattleFingerprint` (location and maxima only) gives clients a `battleKey` that matches only within a single battle. It must key on nothing client-local: current HP moves between polls, and the actor id — the obvious candidate — is evidently not the same number in two clients watching one fight, so including it grouped a five-strong party as five parties of one, which is how the HUD shows one fight rather than every fight — grouping that applies to its peek and battle modes alike, since those differ in *when* the strip appears, not in what it lists. Both facts were established by reading the same region from two clients at once and comparing columns; a single-client dump cannot distinguish shared data from per-character data, which is how each wrong answer along the way survived as long as it did.

`gClientSnapshots` (in `variables.ahk`) is a single shared poll (`UpdateClientSnapshots()`, every `CLIENT_SNAPSHOT_INTERVAL` ms) of every running `main.exe`: map, position, game/battle state, character class. Per-client addons (roster, party markers, discord RPC) consume this via the `OnSnapshot` hook instead of each opening the process independently — follow this pattern rather than adding another independent poll loop.

### Minimap overlay lifecycle

The overlay (`gGui`, built in `main.ahk`'s `ShowOrToggleOverlay`) is a borderless always-on-top `+E0x08000000` (`WS_EX_NOACTIVATE`) window so it never steals game focus. `UpdateMapState()` runs on a 200ms timer, reads the current map name from memory, resolves it to an image in `maps\`, and auto-closes the overlay when the minimap becomes disallowed (`IsMinimapAllowed()`: battle, loading screen, unsupported/unreadable zone). Calibration (world position → overlay pixel) is per-map data in `maps\calibration.ini`, always in **base** (unscaled 400×300) map space — the user's `Scale` setting is applied only at draw time (`MinimapScaleFactor()`), so calibrating at any zoom level produces the same stored numbers.

**Tab belongs to the game wherever the game has a minimap.** The client's `minimap_file4` patch (server repo, `tools/client-patch/`) gave every map Maps++ ships an image for a native minimap too (server `minimapFile` 52–79), so a `$Tab` that captured whenever an image existed swallowed the game's own minimap everywhere. `[Minimap] Source` (`game` default | `mapspp` | `off`) decides; `TabOpensMapsppMinimap()` is the one predicate, used by `HotIfToggleMinimap` and `HandleTab`, and it reads `ReadNativeMinimapState()` **at the keypress**, never from the 200ms cache, so it is right the instant after a teleport. That function mirrors the client's own OpenMinimap gate (VA `0x442370`): index ≥ 0, gate ≠ 0, and for any index ≥ 20 the server's EnableMinimap flag. The tier is **derived from the index**, not read from `MINIMAP_TIER_OFFSET`, because that byte describes the last minimap the game *loaded*, which can be the previous map's. Indices ≥ 52 additionally need the patch (code bytes at RVA `0x605C8`: `E9` patched, `83 F8 nn` not) and `system\smallmap04.mda`; bytes that are neither read as "unknown" and the index is trusted. Every failure mode returns "no game minimap", which is the pre-patch behaviour. `openMapsppMinimap` (unbound) / `ToggleMapsppMinimap()` opens the overlay regardless — it is the only way to POIs, party markers and waypoints on a patched client — and a one-time notice (`[Minimap] SourceNoticeShown`, config.ini only) says so the first time Tab goes to the game on a covered map.

**The game picture is not always the client area.** The client mods' `osmw_display.dll` scales the 800×600 surface 2×/3× (its shipped Windowed/Auto default already does on a 1440p monitor) or letterboxes it in a Resizable/Borderless window, and publishes where the picture is as window properties on the game window (`DISPLAY_PROP_*`; edges stored +1). Three helpers in `functions.ahk` are the only things that should know: `GameMainWindow(hwnd)` maps any `main.exe` window to the real game window — `GAME_WIN_FILTER` also matches the client's chat window, which is what the overlay used to anchor to whenever chat had focus; `GameContentRect(hwnd)` is the picture in screen pixels plus scale and mode, falling back to the whole client area when the properties are absent or inconsistent; `LogicalToClient(hwnd, lx, ly)` turns a fixed 800×600 click (inventory button, chat button) into real client pixels. Anything placed *on* the game uses the content rect; `GameClientRect()` remains for things placed relative to the window. `MinimapScaleFactor()` multiplies by `gGameContentScale` when `[Minimap] FollowGameScale` is on (refreshed in `UpdateMapState`, which drops the overlay Gui on a change, since a picture control cannot resize in place). Launches wait for the size to settle (`WaitForGameWindowsSettled`, only when `NativeDisplayModuleExpected()`) before re-centring or applying a layout; Window Layout skips Borderless and maximized clients, because the display module owns size and Maps++ only ever owns position.

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

The party-health HUD (`addons\party_health.ahk`) is the third overlay shape alongside the minimap and the radial rings. Its two window styles are load-bearing: `WS_EX_TRANSPARENT` (click-through, so a HUD over the game never swallows a click meant for the client) and `WS_EX_NOACTIVATE` plus `SW_SHOWNOACTIVATE` (so appearing mid-fight cannot steal focus). Placement is **derived, never stored** — a strip centred on the bottom edge of `GameContentRect()` (the picture, not the black bars), width-capped to that client — because a saved coordinate is wrong as soon as the user alt-tabs to a differently-placed window; that is also why it has no drag. Size is derived the same way: every layout constant is a 1× size multiplied by the display module's integer scale (`_PartyHealth_Scale`), and the scale is part of the layout signature so a change rebuilds the strip. Following the active window runs on its own short timer rather than the 1 Hz snapshot poll (too slow for an alt-tab), which does not break the one-shared-poll rule because it reads no memory. It defaults to a **peek**: the hotkey shows it for a few seconds rather than leaving it up, since a permanent strip just competes with the gauges the game already draws. A peek is pinned to the client it was opened on and ends when focus leaves it — following instead would slide the strip onto the new window and regroup around it mid-glance. It also forces a `UpdateClientSnapshots()` before drawing, because a keypress usually follows a window switch and the 1 Hz poll would otherwise show the new client the fight the old one was in. It rebuilds only when its layout *signature* (clients, pet row, cell width, scale) changes, never on a value update.

It is built from native controls rather than a WebView2 panel because it is visible over the game rather than in a panel, and standing up Chromium for a few bars is the wrong trade. That means it cannot read `ui\common\style.css`, so its palette is those **same tokens converted to hex once**, each commented with the token it came from. Converting a token is the only sanctioned way to add a colour there — inventing one is how a shared palette quietly stops being shared, which is exactly what happened on the first version of this file.

### Addon system

Addons are self-contained `.ahk` files in `addons\`, each calling `RegisterAddon(Map("name", ..., "OnXxx", handlerFn, ...))` at include-time (global scope) to register hook callbacks, and typically `RegisterHotkeyAction(...)` for their own hotkeys. `FireAddonHook(hookName, params*)` (functions.ahk) dispatches to every enabled addon that registered that hook name, catching and reporting exceptions per-addon so one broken addon doesn't take down the dispatch loop. Hooks fired by core code include `OnInit`, `OnTrayMenu`, `OnMapChange`, `OnOverlayShow`/`OnOverlayHide`, `OnSnapshot`. `OnInit` is fired once from `main.ahk` right after `LoadAddonEnabledStates()` — that position matters, because it is after `LoadLauncherConfig()` sets `gInterfaceMode` (so an addon's `OnInit` can build the right frontend) and after the enabled/disabled state is known (so a disabled addon's `OnInit` is skipped). Load your addon's config there, not at include time, and never block in it — `SetTimer(fn, -N)` anything that opens a window or waits. Addons are enabled/disabled per-name via `config.ini [Addons]`, toggled from Settings → Addons or `SetAddonEnabled()`; a disabled addon's hooks are skipped and its hotkeys drop out on the next `ApplyAllHotkeys()`.

`OnTrayMenu` receives the four task-oriented groups the tray menu is built from — `quickActions`, `clients`, `map` and `debug` — each an already-created `Menu()`. The core entries of each group are added *before* the hook fires, so an addon's items append below them rather than above.

When adding a new addon: create `addons\<name>.ahk`, call `RegisterAddon` with a unique `name`, add it to the relevant `variants\*.txt` manifest(s) it should ship in, and if it reads game memory, register its offset via `RegisterAddonOffset`.

An addon deliberately left out of every manifest is dev-only: `GenerateAddonIncludes()` picks it up from `addons\` on a source run, and `build.ps1` never ships it. `addons\debug_gallery.ahk` (Tray → Debug → **GUI Gallery**, for screenshots) is the one that exists, and its header documents the two things a tool like it has to get right — reaching panels by *name string*, since a direct call to a function a build does not contain is a load-time error, and rendering the three parked surfaces (tray menu, radial rings, dialog) without going through the entry points that dismiss or block them.

### Hotkey system

All rebindable hotkeys go through a central registry in `hotkeys.ahk`: `RegisterHotkeyAction(Map("id", ..., "default", chord, "handler", fn, ...))`. An empty default or saved chord means the action is deliberately unbound; `ApplyAllHotkeys()` skips it. The function tears down and rebinds everything else (needed after Settings save or addon enable/disable toggling which actions are live) using AHK v2's dynamic `Hotkey()` API rather than static `#HotIf`/hotkey directives. Context (when the hotkey is active) is expressed via `hotIfWinActive` (bound to `GAME_WIN_FILTER`) or `hotIfFn` (a `HotIf()` predicate callback) on the action spec — see `HotIfToggleMinimap` for an example combining overlay state and map eligibility. User choices live in `config.ini [Hotkeys]`; a blank value preserves an explicit unbound choice. `gHotkeyReserved` in `hotkeys.ahk` is a hand-maintained list of fixed, non-rebindable chords — it has no compile-time link to the actual fixed bindings (the static `^!r::`… block under "Fixed hotkeys" in `main.ahk`), so a new fixed hotkey must be added to both places. That block was once deleted by an unrelated rewrite and nothing noticed for months: the calibration panel kept telling users to press Ctrl+Alt+1/2, which did nothing. If you touch `main.ahk`'s top half, check it is still there.

### Two interface modes (Native vs WebView2)

`gInterfaceMode` (`config.ini [UI]`) picks between two parallel frontends for every GUI surface (tray menu, settings, client roster):

- **WebView2 ("enhanced")**: renders HTML/CSS/JS panels under `ui\<panel>\` inside a `WebViewGui` (from `Lib\WebViewToo.ahk`, using `Lib\{32bit,64bit}\WebView2Loader.dll`). AHK ↔ JS communication is one hand-rolled JSON protocol per panel: AHK calls `gui.PostWebMessageAsJson('{"type":"...",...}')` to push state, and JS posts back via `window.chrome.webview.postMessage`, received AHK-side through `WebMessageReceived` → a per-panel `_OnWebMessage` handler that switches on `msg["type"]`. There's no shared client-side framework; each `ui\<panel>\main.js` is vanilla JS. Windows are built once and parked off-screen (`x-30000 y-30000`) rather than destroyed/recreated between opens, because standing up a WebView2 + loading a page costs hundreds of ms — see the tray menu's `_EnsureWebTrayGui`/`_RevealWebTrayMenu`/`_CloseWebTrayMenu` in `main.ahk` for the canonical park/reveal/dismiss pattern to copy for new WebView2 panels.
- **Native ("low memory")**: plain Win32/AHK `Gui` controls (`settings_native.ahk`, and list-based fallbacks like the client roster's list view), no Chromium subprocess. If `Lib\WebViewToo.ahk` or the relevant `ui\...\index.html` is missing, WebView2 surfaces fall back to native automatically (see `_Settings_CanUseWebView()`).

Our own files are not enough: without the **WebView2 Runtime** every `WebViewGui(` throws `0x80070002` out of `CreateCoreWebView2EnvironmentWithOptions`. Users got that as an uncaught error on every tray right-click. That is why `IsWebViewInterface()` means *effective* mode. It is `gInterfaceMode = "webview"` **and** `WebView2RuntimeAvailable()`, a once-per-session probe of the loader's `GetAvailableCoreWebView2BrowserVersionString`. It passes the same folder that creation will (`_WV2_RuntimeFolder()` mirrors the scan in `Lib\WebView2.ahk`, which carries a local fix that skips version folders missing `msedgewebview2.exe`; keep the two in step).

`gInterfaceMode` itself keeps the user's choice and is never downgraded, so installing the runtime restores enhanced mode with no further step. A new panel must branch on `IsNativeInterface()` before building, as every existing one does; a files-only `_X_CanUseWebView()` gate does not cover this. For a runtime that passes the probe but fails anyway, the tray, radial, Settings and dialog builders catch the failure and call `MarkWebView2Unavailable(where, err)`, which switches the rest of the session to native and tells the user once.

Both frontends for a given panel (e.g. `settings.ahk`'s `_Settings_BuildWebView` vs `_Settings_BuildNative`) must feed the same underlying state/save functions — when changing a setting's behavior, update the shared read/write logic, not just one frontend.

Shared web UI styling lives in `ui\common\style.css` (one dark theme via CSS custom properties, used by every panel — see `.impeccable.md` for the design system: dense table/form-heavy utility UI, no light theme, practical rather than WCAG-graded accessibility). Don't hardcode one-off colors per panel; extend the shared custom properties instead.

### Dialogs

`dialogs.ahk` is the app's `MsgBox`/`InputBox` replacement — `ShowMessage`, `ConfirmAction`, `PromptText`, rendering `ui\dialog\` in enhanced mode and falling back to the Win32 originals in native mode. **Nothing outside it should call `MsgBox` or `InputBox` directly**, or a Win32 dialog appears in the middle of a WebView2 panel, which is the inconsistency it exists to remove. `FileSelect` stays native everywhere: the OS file picker is the right dialog for picking a file.

The unusual property is that these **block**, parking the caller in a `Sleep` loop until the page answers, because the ~forty call sites are written as `if !ConfirmAction(…)` and rewriting them into callbacks would buy nothing visible. AHK pumps messages during `Sleep`, so the answer arrives and timers keep running — exactly as under `MsgBox`. Two consequences are load-bearing: the owner window is **disabled** for the duration (otherwise the panel behind the dialog keeps handling its own WebView messages and a confirm could be answered by clicking something else), and **every wait has a deadline that ends in a `MsgBox`** rather than a spin — a dialog that never returns would hang the whole single-threaded app. Nested calls, a missing page and a page that never loads all take that fallback. Only the *machine's* half of the wait is bounded: `DIALOG_ACK_MS` covers the page acknowledging the dialog at all, and once it has spoken the wait for the human is open-ended, as it must be.

**A blocking dialog must never be raised from inside a `WebMessageReceived` handler.** A page's message is an inbound cross-process COM call, and while one is on the stack the STA will not deliver another *even across `Sleep`* — so the dialog's own Yes/No can never arrive. The dialog appears (outbound `PostWebMessageAsJson` still works) and then nothing happens, forever, with the panel left `EnableWindow(false)`; the user's only escape is restarting the app. This shipped as a hang in the Character Vendor's Apply, the calibration panel's Apply, and two panels' close prompts, and it is the reason for two mechanisms in `dialogs.ahk`:

- `DeferFromWebMessage(fn, params*)` is **the fix**. A handler that means to raise a dialog calls it instead of calling the work directly; `SetTimer(…, -1)` re-enters from AHK's own message loop after the COM call has returned. Deferred functions land a tick late, so each one must re-check that its window still exists before touching `.Hwnd`. `settings.ahk`'s `_Settings_ResetDefaultsBegin` is the original of this pattern.
- `WebMsgHandler(fn)` is **the backstop**. Every `WebMessageReceived` registration in the app wraps its router in it, which keeps `gWebMsgDepth` accurate; `_Dlg_Run` then takes the native `MsgBox` path rather than blocking, and logs that it did. So a handler that forgets to defer looks ugly and works, instead of freezing the app — and leaves a line in `mapsplusplus.log` naming the problem. Register through it whenever you add a WebView2 panel.

Deferring is not needed for work reached via `PostMessage`/hotkey/timer — the tray menu's `_MakeHmenuCallback` and radial's `onSelect` already leave the callback that way, which is why dialogs work from those.

**Both "leave the callback" mechanisms rely on `_WebMsgDispatch` making the handler's thread `Critical`.** Without it, a handler that ran past AHK's 15 ms uninterruptible window could have the `WM_COMMAND` or `SetTimer(-1)` it had just queued dispatched at its next line, still inside the page's COM call. Inside the handler, the backstop caught that and showed a MsgBox. In the few lines after the handler, where `gWebMsgDepth` is already 0, it got no protection. There, the session's first dialog built its WebView2 inside the COM call, and the creation callbacks could never arrive. The app hung for good, with nothing logged, and the tray page stayed alive but unheard. Map POIs → Export to Game Markers from the tray did exactly that. It was reproduced on demand by giving a second tray handler a 50 ms pause, and the fix was verified against that test. Critical lasts until the callback thread ends, so it covers both windows. Hotkeys, menu items and timers that arrive meanwhile are buffered, not dropped, and messages are still pumped. Keep it, and keep every `WebMessageReceived` registration going through `WebMsgHandler`. A handler must therefore never wait for a timer or hotkey to fire, because neither can run until the handler returns.

Like the tray menu and the radial rings, the window is built once and parked off-screen; unlike them it is not prewarmed, since a session may raise no dialog at all. Because one window serves every dialog, replies carry a **token** and stale ones are dropped, and `_Dlg_ResolveOwner` rejects the dialog's own window along with anything else parked off-screen — without that the second dialog of a session adopts the first as its owner and disables the window it is about to appear in.

The page reports the height it needs and AHK sizes the window to it. It must measure under `body.measuring` (see `ui\dialog\style.css`): the window is still parked at a placeholder height at that moment, so an unmodified measurement returns the squashed layout. For the same family of reasons `[hidden]` is forced with `!important` there — the shared primitives it hides (`.field`, `.btn`, `.dlg-detail-wrap`) all set an explicit `display`, which beats the UA sheet.

Column-aligned output (signature dumps, debug state, slot tables) belongs in the `detail` option, not in the message: it renders in a monospace, scrollable block with a Copy button, and the message above it says what the dump means.

### Config and data files

- `config.ini` — user settings (`[Launcher]`, `[Minimap]`, `[Hotkeys]`, `[MapPois]`, `[Addons]`, `[UI]`, `[WindowLayout]`); written next to the exe or falls back to `%AppData%` via `ResolveWritableIniPath()` if that directory isn't writable (e.g. under Program Files, or Controlled Folder Access). Full key reference is in [TUTORIAL.md](TUTORIAL.md#configuration-configini).
- `maps\calibration.ini` — per-map world→pixel calibration (base 400×300 space).
- `maps\pois.ini` — user/curated map points of interest, keyed by map id, storing **raw** game positions. Raw → the coordinates the game displays is `(raw + offset) / divisor` — `GAME_COORD_OFFSET_X/Y` (400/300) and `GAME_COORD_DIV_X/Y` (16/8) in `variables.ahk`, via `GameCoordX/Y()`. The offsets are not optional: without them every displayed coordinate is low by 25 on X and 37 on Y. They were measured by teleporting to known coordinates and reading raw back, and confirmed identical on a second map, which is why they are global rather than per-map calibration.
- `layouts.ini` — user-authored window layouts (owned by `addons\window_layout.ahk`), also via `ResolveWritableIniPath()`. An `[Index]` section maps a numeric id to the display name, and one `[Layout.<id>]` section per layout holds a monitor fingerprint, the authoring client window size, and `Slot<N>=fx|fy|charName|flags` records. Slot positions are **fractions of the target monitor's work area**, not pixels, so a layout survives a resolution change; `WinSize` is used only to draw boxes to scale in the editor, since applying a layout never resizes a window. Deliberately *not* in `maps\`, which `build.ps1` copies into `releases\`.
- `better_hotkeys.ini` — explicit per-character skill profiles owned by `addons\better_hotkeys.ahk`. Dynamic profile chords are transient hidden entries in the central registry, rebuilt through `OnBeforeApplyHotkeys`; they must not be persisted into `config.ini [Hotkeys]` or exposed in the general Settings hotkey list.
- `signatures.ini` / `offsets_cache.ini` — persisted byte-signatures and per-build resolved RVAs from the signature calibration system above.
- `map_markers_export.ini` — which marker ids in a game folder's `map-markers.tsv` Maps++ wrote (owned by `addons\map_pois.ahk`), one `[Folder.<fnv64 of the lowercased game dir>]` section per folder with `GameDir=` and `Ids=`. It is what lets a re-export remove markers for deleted POIs and lets "Remove Exported Game Markers" touch nothing else. Losing it costs only those removals: ids are derived from map + position + label, so a re-export still updates in place.
- `mapsplusplus.log` (+ `.log.1`) — the rolling log, and `mapsplusplus.log.diagnostics.txt` written by **Copy Diagnostics**. Same `ResolveWritableIniPath()` location rule as the inis. Both are output only; nothing reads them back except `LogTail()`.

**Any save path that writes user text to an ini must call `EnsureIniUtf16(path)` before its first `IniWrite`/`IniDelete`** (`functions.ahk`, near `ResolveWritableIniPath`). Windows fixes an ini's encoding from what is already on disk, and AHK only creates a UTF-16 one when `IniWrite` is what creates the file — a store whose first call is `IniDelete` (deleting a section it is about to rewrite, as `_Pois_Save` and `_BH_SaveProfiles` do) instead gets an ANSI file, and from then on every write through it silently replaces characters outside the system codepage with `?`. Character names and POI labels are exactly the affected values.
- `maps\*.jpg` — one custom minimap image per supported zone, named `MAP<id>.jpg` matching the in-game map filename.

### The native client mods (a separate product)

The 32-bit C mods that run inside the client -- `osmw_core.dll` and its
features, including the chat window and auto-login -- live in the osMW server
repo under `tools/client-mods/`, are built there, and reach a game folder by
their own route. **Maps++ does not ship, deploy, update or version them**, and
works normally on a client that does not have them. `patches-MOVED.md` has the
history; `Lib\` holds only the WebView2 libraries now.

**`native_mods.ahk` is the whole of the contract.** The mods own `osmw.ini` and
`osmw_defaults.ini` beside `main.exe`, with the same chain Maps++ uses for its
own pair: `osmw.ini` -> `osmw_defaults.ini` -> the literal compiled into the
DLL. `NativeModsRead`/`NativeModsWrite`/`NativeModsInstalled` are the only ways
to touch them, and the rules there are load-bearing:

- **Writes happen only when the user asks.** `_ChatMod_SyncGate` used to write
  `[Addons] ChatMod` before every launch, which silently undid the in-game F12
  settings panel. Nothing writes on launch now; the gate is a checkbox on the
  Chat Mod tab instead.
- **One key per write**, never a section rewrite and never `IniDelete`: a
  running client writes its own keys (chat geometry, fight speed) into the same
  file.
- **Never write `osmw_defaults.ini`.** It ships with the mods and is replaced
  wholesale when they update.
- Maps++'s `config.ini`/`defaults.ini` are a *different pair of files*, even
  when Maps++ is installed in the game folder and they sit side by side. They
  used to be the same files, which is what made `[Addons]`, `[Minimap]` and
  `[Debug]` collide between the two products. The mods carry a pre-split
  `config.ini` across once themselves, then leave it alone.
- `NativeModsInstalled()` is false on a stock client. Every caller must degrade
  to saying so rather than writing into a folder with no mods in it, and
  `NativeModsWrite` itself now refuses there.
- **A Settings tab that edits the mods' files declares `"nativeMods", true`**
  in its `RegisterAddon` map (Chat Mod does). Both Settings frontends send
  every field on every Save, so before this, saving the minimap opacity
  rewrote all of `[Chat]` with the values the window had opened with and
  undid any F12 edit made meanwhile. Save now snapshots what each addon tab
  rendered (`_Settings_AddonFieldsFor`) and calls an addon only if one of
  its fields changed; a `nativeMods` addon receives *only* the changed keys,
  and Reset to Defaults skips it entirely. Ordinary addons still get their
  full map, because most of them read an absent key as "off". A disabled
  addon gets no tab, no Save and no Reset (`_Settings_AddonHasTab`).

**Reading what the mods report.** `NativeModsReadStatus()` parses
`osmw_status.ini`, the core's atomic startup report (core state, release
integrity, one `[Module.N]` per feature with state, result code and version)
-- the same payload as the in-game F12 -> Status page. It is one file for every
client in the folder with no PID, so anything showing it says it describes the
last client to change state. Tray -> Debug -> **Client Mods Status...** and
`BuildDiagnosticsReport` share `NativeModsStatusTable`, so they cannot drift.
Result `-15` is "blocked by Windows" (Smart App Control refusing an unsigned
DLL); a `disabled` login module is normal in any client not launched with
auto-login consent and is reported as idle, not off.

**The one refusal the mods cannot report is their own.** For a refused
*feature* the core shows a once-only in-game box (`[Core] BlockedNotice`), so
Maps++ deliberately says nothing extra. A refused `osmw_core.dll`, though,
leaves a stock client, no fresh status file and no in-game notice.
`NativeModsCoreLoadState(proc, pid)` detects it per running client, from three
facts that are all needed: mod_autoload's `E9 rel32 90 90` at
`NATIVE_MODS_AUTOLOAD_RVA` (the CRT entry), `osmw_core.dll` beside *that
process's* `main.exe`, and no such module in its module list. A failed module
walk returns `unknown` rather than passing for a refusal.
`_CheckClientModsLoaded` (from `UpdateMapState`, once per focused PID) raises
`ShowCoreRefusedNotice` once and records it in Maps++'s own `config.ini`
`[ClientMods] CoreRefusedNoticeShown`, never in `osmw.ini`. A client seen with
the core loaded clears it. `SmartAppControlState()` decides the wording: SAC is
only blamed when it is actually on.

**The game's marker file.** Map POIs → Export to Game Markers writes POIs into
the minimap mod's `map-markers.tsv` (spec: server repo
`tools/client-dll/map-markers.md` section 4). `NativeModsMarkersReplace` is the
only writer, and it follows the mod's own rule exactly: take the folder's
`Global\osmw_map_markers_<dir>` mutex (name derived as `mm_store_mutex_name`
does), reload, drop only the caller's owned ids, carry every other line over
byte for byte (Latin-1 in and out), write a temp file, `MoveFileEx`
replace-with-write-through. A header other than `OSMW-MARKERS<TAB>1` means a
newer writer and the file is not touched. The mod's limits (64 per map
counting the player's own, 2048 total) are applied before writing, since the
mod would drop the excess silently. Marker units are POI raw + 400 / + 300
(`GAME_COORD_OFFSET_X/Y`); that was confirmed against the mod's own character
slot position on a live client, not inferred. The output was round-tripped
through the mod's real `mm_markers_parse`/`mm_markers_serialise` (built MSVC
x86, as `tests/test_markers.c` does) and came back byte-identical. The layer
ships off (`[Minimap] MarkerLayer=0`), so the export works but says the markers
will not show until the player turns it on. Maps++ never turns it on itself.

**Keys the mods take in game** (`NativeModsReservedKeys`: `[Settings] OpenKey`
F12, `[Fight] CycleKey` F11, Timers' fixed F9, `[Leaderboard] OpenKey`, Chat
Mod's system-wide `ClearHotkey`/`TabHotkey`, each only while its `[Addons]`
gate is on) are **warned about, never refused** -- at capture, at Save (only
for chords that changed), and in diagnostics. The F-key features poll key
state, so they match on the trigger key with any modifiers; Chat Mod's are real
hotkeys and match the exact chord. Chord comparison everywhere goes through
`CanonicalHotkeyChord` (modifiers in `^!+#` order, key lowercased): the capture
writes `^+c` while defaults were typed `+^c`, and a plain string compare let
two actions share a chord unnoticed.

**The mods' own logging is opt-in.** Each DLL writes one `osmw_*.<pid>.log` per
client per launch into the game folder, which is five files a launch for a
player who is not debugging anything -- so `[Debug] ModLogging` in `osmw.ini`
(default `0`, toggled from Tray -> Debug -> **Client Mod Logging**, read
AHK-side by `ModLoggingEnabled`/`SetModLogging` in `native_mods.ahk`) gates it,
and with it clear the log calls open no file at all. Each DLL reads the key
**once, as the client starts**, so a change applies to clients launched
afterwards, never to ones already running. `BuildDiagnosticsReport` names the
state and whether the mods are installed at all, so a report with no
`osmw_*.log` reads as "logging was off" rather than "the mod never loaded".

### Launch profiles

A profile bundles the four things a launch needs — accounts, client count, display, layout — so `Launch (Primary)` / `Launch (Secondary)` and their hotkeys stay a single click while still meaning "my main team" and "my alts". Stored the way `layouts.ini` does it: `[LaunchProfileIndex] <id>=<name>` plus one `[LaunchProfile.<id>]` section, with `[Launcher] PrimaryProfile`/`SecondaryProfile` naming ids so a rename cannot orphan a binding.

`gPrimaryMonitorOverride`, `gSecondaryMonitorOverride`, `gPrimaryLaunchLayout` and `gSecondaryLaunchLayout` **survive as mirrors** of the bound profiles (`SyncLaunchGlobalsFromProfiles`), which is what let `ResolveMonitorForHotkey`, `ResolveLaunchLayoutName` and the startup single-client `LaunchGameInstance` stay untouched; `ResolveMonitorIndex`/`ResolveLayoutPreference` are the explicit-argument forms a profile that is *not* bound uses. `LoadLaunchProfiles` seeds two profiles from those same four keys when none exist, so the first launch after an upgrade is identical to the last one before it — and `ResetAllSettingsToDefaults` must rebuild the profiles too, since resetting only the mirrors would have `SaveLauncherConfig` sync the old values straight back.

Core stays ignorant of accounts: `LaunchWithProfile` takes a `plan` of **opaque tokens**, one per client, and `ResolveAccountPlan` gets them from the addon through `GetFuncByName("_AutoLogin_ResolvePlan")`. A disabled or absent AutoLogin yields no plan and the launch falls back to the roster's own next-free claiming. The tray's `Client Launch` submenu is built the same way (`_AutoLogin_BuildAccountMenu`), so it simply disappears in a build or configuration without the addon.

### Variant system

Three shipped builds (`variants\full.txt`, `lite.txt`, `battle.txt`) are just addon-file lists consumed by `build.ps1`: Full has everything, Lite drops combat/inventory/chat/roster addons, Battle drops inventory/window-layout. Core (`main.ahk`/`functions.ahk`/`settings*.ahk`/`hotkeys.ahk`) is identical across variants — only the addon set differs.
