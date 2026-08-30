# Native mods

Two 32-bit DLLs injected into the MythWar client, plus the launchers that load
them. They are C, not AutoHotkey, and they are the only part of this repo that
runs inside the game's own process.

| Directory | Produces | What it does |
|---|---|---|
| `chatmod/` | `osmw_chat.dll`, `osmw_chat_launch.exe` | Rewrites the client's chat window: font, free placement, channel tabs, mention highlighting, timestamps, wheel scrolling, cooldown text |
| `login/` | `osmw_login.dll`, `osmw_login_launch.exe` | Fills the login form and picks a character, one roster row per launched client |
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
python patches/chatmod/tools/build.py
python patches/login/tools/build.py
```

Both write to `Lib/` — the shipped artifact — then copy into the game folder so
you can launch straight away. That second copy is best-effort: a running client
holds the DLL open, and the build says so rather than failing, because `Lib/`
was still written correctly. `--no-deploy` skips it; `--clean` drops the
intermediates.

**A DLL cannot be rebuilt while a client has it loaded.** Close every client
first, or `python patches/chatmod/tools/inject_test.py --eject`.

`build.ps1` runs `common/modhash.py --check` before compiling Maps++ and
rebuilds either mod whose sources no longer match what produced `Lib/`. It
compares recorded source hashes, not timestamps, because git does not preserve
mtimes and a fresh clone would otherwise trip a newer-than test at random. On a
machine without Python or MSVC this is a warning, not an error — the AHK side
still builds and ships `Lib/` as it stands. `-SkipMods` opts out entirely.

## After a client update

Every hook is resolved by byte-pattern signature, so a client update disables a
mod rather than corrupting the process. Re-resolve with:

```
python patches/chatmod/tools/scan_offsets.py           # report only
python patches/chatmod/tools/scan_offsets.py --write   # update sigs.h + cache
python patches/login/tools/scan_offsets.py --write
```

`--write` updates the game folder's `offsets_cache.ini` and regenerates
`src/sigs.h` in this repo, so the next build carries the new addresses.

## Ground rules

- **Never patch `main.exe` on disk.** SHA-256 `285C3BE5…`, verified after every
  change.
- **Never hook without a signature check.** A client update must disable a mod,
  not crash it.
