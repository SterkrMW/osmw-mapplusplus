"""Where the sources live, and where the game lives.

These used to be the same place. The mod sources sat inside the game folder, so
one ROOT (three parents up from a tool) meant both "the project" and "the
installed game", and the tools could not tell the two apart -- nor did they need
to. Now the sources live in the Maps++ repo and the game is somewhere else
entirely, so every path a tool touches belongs to exactly one of two roots:

  PROJ  the repo. Sources, generated sigs.h, object files, and Lib/ -- the
        shipped binaries build.ps1 copies into a release. Always known: it is
        derived from this file's own location.

  GAME  the installed client. main.exe, the signature database, config.ini, the
        per-pid logs, and the deployed copies of the DLLs. NOT always known, and
        deliberately not guessed: a tool that needs it says so and fails with an
        instruction rather than reading some other installation by accident.

Resolution order for GAME, first hit wins:

  1. --game-root on the command line   (explicit beats everything)
  2. $OSMW_GAME_ROOT                   (per-shell, good for a second install)
  3. [Launcher] GamePath in the repo's config.ini, minus the filename
     (the usual case: you already told Maps++ where the game is)

A directory only counts if main.exe is actually in it, so a stale config.ini
entry is reported as missing rather than silently accepted.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

# <repo>/patches/common/paths.py -> <repo>
PROJ = Path(__file__).resolve().parents[2]

LIB = PROJ / "Lib"

_ENV_VAR = "OSMW_GAME_ROOT"


def _from_config_ini() -> Path | None:
    """The GamePath Maps++ already stores, minus the executable name.

    Read by hand rather than with configparser: these inis are written by
    AutoHotkey and are UTF-16 as often as not, and one may carry duplicate
    section names that configparser rejects outright.
    """
    ini = PROJ / "config.ini"
    if not ini.is_file():
        return None
    for encoding in ("utf-16", "utf-8-sig", "mbcs"):
        try:
            text = ini.read_text(encoding=encoding)
        except (UnicodeError, LookupError, OSError):
            continue
        in_launcher = False
        for raw in text.splitlines():
            line = raw.strip()
            if line.startswith("[") and line.endswith("]"):
                in_launcher = line.lower() == "[launcher]"
                continue
            if not in_launcher or "=" not in line:
                continue
            key, _, value = line.partition("=")
            if key.strip().lower() == "gamepath" and value.strip():
                return Path(value.strip()).parent
        return None
    return None


def find_game_root(explicit: str | Path | None = None) -> Path | None:
    """The installed game folder, or None when it cannot be established."""
    candidates = []
    if explicit:
        candidates.append(Path(explicit))
    env = os.environ.get(_ENV_VAR, "").strip()
    if env:
        candidates.append(Path(env))
    from_ini = _from_config_ini()
    if from_ini is not None:
        candidates.append(from_ini)

    for candidate in candidates:
        try:
            if (candidate / "main.exe").is_file():
                return candidate.resolve()
        except OSError:
            continue
    return None


def game_root(explicit: str | Path | None = None) -> Path:
    """find_game_root, but exit with instructions instead of returning None.

    For the tools that cannot do anything useful without the client: reading
    signatures out of main.exe, injecting, capturing the chat window.
    """
    found = find_game_root(explicit)
    if found is not None:
        return found
    sys.exit(
        "Cannot find the game folder (no main.exe).\n"
        "Point this at your install in any of these ways:\n"
        "  --game-root D:\\path\\to\\MythWar\n"
        "  set %s=D:\\path\\to\\MythWar\n"
        "  set [Launcher] GamePath in %s\n"
        % (_ENV_VAR, PROJ / "config.ini")
    )


def add_game_root_arg(parser) -> None:
    """Standard --game-root flag, so every tool spells it the same way."""
    parser.add_argument(
        "--game-root",
        metavar="DIR",
        default=None,
        help="the MythWar install to work against (default: $%s, "
             "else GamePath from the repo's config.ini)" % _ENV_VAR,
    )
