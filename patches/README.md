# Native mods

Ten 32-bit DLL projects: the managed `osmw_core.dll` bootstrap and nine
host-verified managed features. The core-only release tooling is host-verified;
production autoload and live-client acceptance remain pending. They are C, not
AutoHotkey, and run inside the game's own process.

The production client still carries the legacy multi-DLL `mod_autoload.py`
list and was not changed during Task 16. The release candidate migrates that
list to only `osmw_core.dll`, packages generated ABI metadata plus all nine
features, and blocks launch if its transactional payload update cannot commit.
Both compatibility launchers now load core only; the login-named launcher keeps
its `--roster` mode.

| Directory | Produces | What it does |
|---|---|---|
| `core/` | `osmw_core.dll` | Central bootstrap and versioned module API; services, managed loading, runtime claims, shared UiTick/packet brokers, copied send queue, and all-or-none retry-safe shutdown are host-verified, but live-client validation remains and it is not yet in the production autoload list |
| `chatmod/` | `osmw_chat.dll`, `osmw_chat_launch.exe` | Managed ABI 1 chat QoL and item links: passive lifecycle, early claimed font hooks, nonblocking HWND state, 29 exact ranges, one WndProc dispatcher and core-queued sends are host-verified; live first-launch font/client matrix remains |
| `login/` | `osmw_login.dll`, `osmw_login_launch.exe` | Managed ABI 1 auto-login: passive lifecycle, core resolver/config/log, per-launch consent and named-row selection, and two exact prologue claims are host-verified; managed live-client matrix remains |
| `minimap/` | `osmw_minimap.dll` | Managed ABI 1 NPC list/auto-walk: passive lifecycle, core resolver/config/log, priority-0 post-UiTick work, two claimed panel call sites and optional claimed SendPlayerMove detour are host-verified; managed live-client matrix remains |
| `fightspeed/` | `osmw_fightspeed.dll` | Managed ABI 1 Normal/Fast/Off: exact VFX, FrameSync, FrameWait-caller and IAT claims retain the specialised freeze/EIP-safe transaction and are host-verified; managed live-client matrix remains |
| `weather/` | `osmw_weather.dll` | Managed ABI 1 extra weather: every SetWeather/draw/icon caller written is exactly claimed and partial installs roll back; host-verified, managed live-client matrix remains |
| `settings/` | `osmw_settings.dll` | Managed ABI 1 settings feature: passive lifecycle, core resolver/config/log/status services, and priority-0 post-UiTick panel callback are host-verified; managed live-client matrix remains |
| `timers/` | `osmw_timers.dll` | Managed ABI 1 timer feature: passive lifecycle, core resolver/config/log/send services, exclusive snapshot handling, post-stock MapData observation and priority-0 post-UiTick panel work are host-verified; managed live-client matrix remains |
| `inventory/` | `osmw_inventory.dll` | Managed ABI 1 inventory tools: stock companion control, client-computed permutation, one-time server-authorised apply protocol, and lifecycle fixtures |
| `fishing/` | `osmw_fishing.dll` | Managed ABI 1 fishing panel: capability hello, snapshot claim, post-UiTick modal, optional MDA bank, stock-dialog fallback |
| `common/` | — | `paths.py` (the two roots), `modhash.py` (drift check), `peutil.py` (PE reader) |

Each mod's `NOTES.txt` is its real documentation — architecture, every hook
point with its address, the failed approaches and why they failed, the
measurements behind each limit, and a dated trial log. Read it before changing
anything; it was written to be read cold.

## Why they live here

They used to live inside the game folder, unversioned, while the compiled
binaries were committed to `Lib/`. The two could drift with nothing to notice,
and did: a fix went into the C source in the game tree while `Lib/` kept
shipping the build from before it, so a fresh install silently got the old
behaviour. Sources and shipped artifacts now sit in one repo with one history,
and `build.ps1` refuses to cut a release where they disagree.

## Two roots

Because the sources moved out of the game folder, every path a tool touches
belongs to exactly one of two places, and `common/paths.py` is the only thing
that decides which:

- **`PROJ`** — this repo. Sources, generated `sigs.h`, object files, and `Lib/`.
  Always known.
- **`GAME`** — the installed client. `main.exe`, `signatures.ini`,
  `offsets_cache.ini`, `config.ini`, per-pid logs, and the deployed DLLs. Found
  from `--game-root`, else `$OSMW_GAME_ROOT`, else `[Launcher] GamePath` in this
  repo's `config.ini`. Never guessed: a directory only counts if `main.exe` is
  in it, so a stale config entry reports as missing rather than quietly
  targeting some other install.

## Building

Needs Python 3 and the **MSVC x86 build tools** (`vcvarsall.bat`; the client is
a 2006 MSVC binary called into with `__thiscall`, so mingw will not do).

```
python patches/core/tools/build.py --test --no-deploy
python patches/chatmod/tools/build.py --test --no-deploy
python patches/login/tools/build.py --test --no-deploy
python patches/minimap/tools/build.py
python patches/fightspeed/tools/build.py --test --no-deploy
python patches/weather/tools/build.py --test --no-deploy
python patches/settings/tools/build.py
python patches/timers/tools/build.py
python patches/inventory/tools/build.py --test --no-deploy
python patches/fishing/tools/build.py --test --no-deploy
```

Each writes to `Lib/` — the shipped artifact — then copies into the game folder so
you can launch straight away. That second copy is best-effort: a running client
holds the DLL open, and the build says so rather than failing, because `Lib/`
was still written correctly. `--no-deploy` skips it; `--clean` drops the
intermediates.

**A DLL cannot be rebuilt while a client has it loaded.** Close every client
first, or `python patches/chatmod/tools/inject_test.py --eject`.

`build.ps1` runs `common/modhash.py --check` before compiling Maps++ and
rebuilds the native mods if their sources no longer match what produced `Lib/`. It
compares recorded source hashes, not timestamps, because git does not preserve
mtimes and a fresh clone would otherwise trip a newer-than test at random. On a
machine without Python or MSVC this is a warning, not an error — the AHK side
still builds and ships `Lib/` as it stands. `-SkipMods` opts out entirely.

## After a client update

Every private entry point and core-owned shared hook is resolved by byte-pattern
signature, so a client update disables the affected feature rather than
corrupting the process. Re-resolve with:

```
python patches/chatmod/tools/scan_offsets.py           # report only
python patches/chatmod/tools/scan_offsets.py --write   # update sigs.h + cache
python patches/login/tools/scan_offsets.py --write
python patches/timers/tools/scan_offsets.py --write
python patches/fishing/tools/scan_offsets.py --write
```

There is one scanner per mod and they all take `--game-root`. As of
2026-09-10 the feature DLLs carry 157 module-owned signatures (40 chatmod,
10 login, 30 minimap, 10 fightspeed, 8 weather, 14 settings, 13 timers,
17 inventory, 15 fishing). Core
owns the three shared UiTick, HandlePacket and SendPacket requests. Settings'
and minimap's old UiTick rows and timers' old three shared rows remain excluded
from their managed generated headers. The timer scanner keeps its signature
definitions beside its source, including its two optional sprite requests; the
older scanners read their sections from the GAME folder's `signatures.ini`,
whose contents may differ between installs.

`--write` updates the game folder's `offsets_cache.ini` and regenerates
`src/sigs.h` in this repo, so the next build carries the new addresses.

## Ground rules

- **Never patch `main.exe` on disk**, with one sanctioned exception (below).
  SHA-256 `285C3BE5…`, verified after every change.
- **Never hook without a signature check.** A client update must disable a mod,
  not crash it.

### The one exception: mod autoload

`mw-tools`' `tools/client-patch/mod_autoload.py` patches the 7-byte CRT entry
prologue at `0x4cd360` into a `jmp` to a code cave that `LoadLibraryA`s these
DLLs by name. It exists so a player who does not launch through Maps++ still
gets the mods: without it, the injector - and therefore Maps++ - is mandatory.

That changes the shipped exe's hash. The rule above still holds for everything
in *this* repo, which never writes to `main.exe`; a patched client is an input
we accept, not something the mods produce.

Two consequences worth keeping in mind:

- **The signature scan is the compatibility contract, not the hash.** `scan.c`
  needs exactly one match per signature across all of `.text`, cave included,
  so any `tools/client-patch/` patch could in principle destroy a signature or
  create a second match. Re-run `scan_offsets.py` against the patched exe after
  any new patch. All 157 module-owned requests and core's three shared requests
  currently resolve exactly once; the timer sprite-array operand is expected to
  produce a different target after the capacity patch.
- **The core is loaded at the executable entry point.** Its worker processes
  chat first, and chat installs both font hooks synchronously before registering
  deferred window work. Host ordering is verified; the worker-versus-client
  first-font timing remains part of the live cutover matrix. Core and all nine
  feature modules otherwise keep `DllMain` passive.

Both launchers detect a patched `main.exe` via `client_self_loads()` and skip
injection. For an unpatched client, either launcher injects only core.
`inject_test.py` likewise loads/ejects core and refuses an
unsafe `FreeLibrary`; it also treats a zero `FreeLibrary` result as a failed,
still-quiesced eject.
