"""Tell whether Lib/'s binaries were built from the sources now in the tree.

The point of moving the mod sources into this repo was to stop the shipped DLLs
and the code that produced them drifting apart. Something has to actually check
that, or "single source of truth" is just a filing decision.

Timestamps cannot do it. Git does not preserve mtimes, so a fresh clone stamps
every file with the checkout time in no useful order, and any newer-than test
would fire at random on a machine that had changed nothing. So each build writes
a hash of the sources that produced it, next to the binary, and the check
recomputes that hash and compares.

    python patches/common/modhash.py --check    exit 1 if anything drifted
    python patches/common/modhash.py --write     record the current sources
    python patches/common/modhash.py             print the state of each mod

The hash covers every file the compiler reads -- .c, .h, .def -- by name and
content, sorted, so it is stable across machines and checkouts and changes only
when the code does. It deliberately does NOT cover the tools: editing build.py's
help text should not invalidate a binary.
"""

from __future__ import annotations

import argparse
import hashlib
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from paths import LIB, PROJ  # noqa: E402

# mod directory -> the binaries it produces. The first is the one that carries
# the recorded hash; all of them are rebuilt together by that mod's build.py.
MODS = {
    "core": ["osmw_core.dll", "osmw_core_api.ini"],
    "chatmod": ["osmw_chat.dll", "osmw_chat_launch.exe"],
    "login": ["osmw_login.dll", "osmw_login_launch.exe"],
    # One binary and no launcher: the client loads this mod itself, through the
    # mod_autoload cave patch in mw-tools, so there is nothing to inject it.
    "minimap": ["osmw_minimap.dll"],
    "fightspeed": ["osmw_fightspeed.dll"],
    "weather": ["osmw_weather.dll"],
    "settings": ["osmw_settings.dll"],
    "timers": ["osmw_timers.dll"],
    "inventory": ["osmw_inventory.dll"],
    "fishing": ["osmw_fishing.dll"],
}

SOURCE_SUFFIXES = {".c", ".h", ".def"}


def source_hash(mod: str) -> str:
    src = PROJ / "patches" / mod / "src"
    digest = hashlib.sha256()
    roots = [(src, Path())]
    if mod == "core":
        # Core's public SDK is a compiler input even though it intentionally
        # lives outside src/.  Prefix both roots so equal filenames cannot
        # collide in the digest.  Other mod hashes retain their old paths.
        roots = [(src, Path("src")),
                 (PROJ / "patches" / "core" / "sdk", Path("sdk"))]
    elif mod in ("chatmod", "fightspeed", "fishing", "inventory", "login", "minimap",
                 "settings", "timers", "weather"):
        # Managed features compile directly against the frozen core ABI, so an
        # SDK change also makes their shipped DLLs stale.
        roots.append(
            (PROJ / "patches" / "core" / "sdk", Path("sdk")))
    for root, prefix in roots:
        for path in sorted(root.rglob("*")):
            if (not path.is_file() or
                    path.suffix.lower() not in SOURCE_SUFFIXES):
                continue
            relative = prefix / path.relative_to(root)
            digest.update(relative.as_posix().encode("utf-8"))
            digest.update(b"\0")
            digest.update(path.read_bytes())
            digest.update(b"\0")
    if mod == "core":
        # The generated sigs.h is a compiler input, but its local INI catalog
        # is the source of truth and must not drift unnoticed after a build.
        catalog = PROJ / "patches" / "core" / "signatures.ini"
        digest.update(b"signatures.ini\0")
        digest.update(catalog.read_bytes())
        digest.update(b"\0")
    elif mod in ("fishing", "inventory", "timers"):
        # These UI modules own local signature catalogs used to generate their
        # compiled signature tables.
        catalog = PROJ / "patches" / mod / "signatures.ini"
        digest.update(b"signatures.ini\0")
        digest.update(catalog.read_bytes())
        digest.update(b"\0")
    return digest.hexdigest()


def stamp_path(mod: str) -> Path:
    return LIB / (MODS[mod][0] + ".srchash")


def write_stamp(mod: str) -> None:
    stamp_path(mod).write_text(source_hash(mod) + "\n", encoding="ascii")


def read_stamp(mod: str) -> str | None:
    path = stamp_path(mod)
    if not path.is_file():
        return None
    try:
        return path.read_text(encoding="ascii").strip()
    except OSError:
        return None


def state(mod: str) -> tuple[str, str]:
    """(verdict, detail) for one mod. Verdict is ok / stale / missing."""
    for name in MODS[mod]:
        if not (LIB / name).is_file():
            return "missing", "Lib/%s does not exist" % name
    recorded = read_stamp(mod)
    if recorded is None:
        return "stale", "no recorded source hash (built before this check existed)"
    if recorded != source_hash(mod):
        return "stale", "sources have changed since Lib/%s was built" % MODS[mod][0]
    if mod == "core":
        return "ok", "Lib/ matches core source, SDK and signature inputs"
    if mod in ("chatmod", "fightspeed", "login", "minimap", "settings",
               "weather"):
        return "ok", "Lib/ matches %s source and core SDK inputs" % mod
    if mod in ("inventory", "timers"):
        return "ok", "Lib/ matches %s source, SDK and signature inputs" % mod
    if mod == "fishing":
        return "ok", "Lib/ matches fishing source, SDK and signature inputs"
    return "ok", "Lib/ matches patches/%s/src" % mod


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="exit 1 if any mod's binaries are stale or missing")
    ap.add_argument("--write", action="store_true",
                    help="record the current sources as built")
    ap.add_argument("--mod", choices=sorted(MODS), default=None,
                    help="act on one mod only (default: all)")
    args = ap.parse_args()

    mods = [args.mod] if args.mod else sorted(MODS)

    if args.write:
        for mod in mods:
            write_stamp(mod)
            print("recorded %s" % stamp_path(mod).relative_to(PROJ))
        return 0

    drifted = []
    for mod in mods:
        verdict, detail = state(mod)
        print("%-8s %-8s %s" % (mod, verdict, detail))
        if verdict != "ok":
            drifted.append(mod)

    if args.check and drifted:
        print("\nRebuild with:")
        for mod in drifted:
            print("    python patches/%s/tools/build.py" % mod)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
