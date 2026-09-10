"""Build, verify, test, stamp, and optionally deploy osmw_fishing.dll."""
from __future__ import annotations

import argparse
import importlib.util
import os
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "common"))
from paths import PROJ, LIB, add_game_root_arg, find_game_root  # noqa: E402
import modhash  # noqa: E402

ROOT = PROJ / "patches" / "fishing"
SRC = ROOT / "src"
SDK = PROJ / "patches" / "core" / "sdk"
TESTS = ROOT / "tests"
OBJ = ROOT / "build"
OUT = LIB / "osmw_fishing.dll"
MDA_OUT = LIB / "osmw_fishing.mda"
TEST_OUT = OBJ / "test_lifecycle.exe"
SOURCES = ["dllmain.c", "scan.c", "config.c", "fishing.c", "panel.c", "log.c"]


def bundled_mda() -> Path | None:
    """Prefer a freshly packed mw-tools bank, else the last Lib/ copy."""
    env = os.environ.get("OSMW_MW_TOOLS", "").strip()
    candidates = []
    if env:
        candidates.append(
            Path(env) / "tools" / "client-dll" / "fishing-mda" / "osmw_fishing.mda")
    # D:\mw-tools-gittest\mw-tools beside D:\oswm-betterminimaps\osmw-map-
    candidates.append(
        PROJ.parents[1] / "mw-tools-gittest" / "mw-tools" / "tools" /
        "client-dll" / "fishing-mda" / "osmw_fishing.mda")
    candidates.append(MDA_OUT)
    for path in candidates:
        if path.is_file():
            return path
    return None


def ship_mda(game: Path | None) -> None:
    """Lib/ holds the shippable bank; the client loads it from system\\."""
    source = bundled_mda()
    if source is None:
        print("  warning: osmw_fishing.mda not found; graphical art will not load")
        return
    if source.resolve() != MDA_OUT.resolve():
        shutil.copy2(source, MDA_OUT)
        print("  shipped %s (%d bytes)" % (MDA_OUT.name, MDA_OUT.stat().st_size))
    if game is None:
        return
    dest_dir = game / "system"
    dest_dir.mkdir(parents=True, exist_ok=True)
    dest = dest_dir / "osmw_fishing.mda"
    try:
        shutil.copy2(MDA_OUT, dest)
        print("  deployed %s" % dest)
    except OSError as exc:
        print("  NOT deployed: %s (%s)" % (dest, exc))
        print("    close the client if it has the MDA open, then retry")


def timer_helpers():
    path = PROJ / "patches" / "timers" / "tools" / "build.py"
    spec = importlib.util.spec_from_file_location("osmw_timer_build_helpers", path)
    if spec is None or spec.loader is None:
        raise SystemExit("cannot load native build helpers")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def check_not_locked() -> None:
    if not OUT.exists():
        return
    try:
        with open(OUT, "r+b"):
            pass
    except PermissionError:
        raise SystemExit("%s is locked -- close every client and retry" %
                         OUT.name)


def clean() -> int:
    if not OBJ.exists():
        print("nothing to clean")
        return 0
    removed = 0
    for path in OBJ.iterdir():
        if path.is_file():
            path.unlink()
            removed += 1
    print("removed %d intermediate(s)" % removed)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--debug", action="store_true")
    parser.add_argument("--test", action="store_true")
    parser.add_argument("--clean", action="store_true")
    parser.add_argument("--no-deploy", action="store_true")
    add_game_root_arg(parser)
    args = parser.parse_args()
    if args.clean:
        return clean()

    helper = timer_helpers()
    check_not_locked()
    LIB.mkdir(parents=True, exist_ok=True)
    OBJ.mkdir(parents=True, exist_ok=True)
    vcvars = helper.find_vcvarsall()
    print("toolchain: %s" % vcvars)
    cflags = [
        "/nologo", "/c", "/W4", "/GS-", "/MT",
        "/D_CRT_SECURE_NO_WARNINGS", "/DWIN32_LEAN_AND_MEAN",
        "/I", str(SRC), "/I", str(SDK), "/Fo:" + str(OBJ) + "\\",
    ]
    cflags += (["/Od", "/Zi", "/Fd:" + str(OBJ / "osmw_fishing.pdb")]
               if args.debug else ["/O2"])
    link = [
        "/nologo", "/DLL", "/MACHINE:X86",
        "/DEF:" + str(SRC / "osmw_fishing.def"),
        "/IMPLIB:" + str(OBJ / "osmw_fishing.lib"),
        "/OUT:" + str(OUT),
        "/PDB:" + str(OBJ / "osmw_fishing.pdb"),
        "kernel32.lib", "user32.lib",
    ]
    if args.debug:
        link.insert(2, "/DEBUG")
    objects = [str(OBJ / (Path(source).stem + ".obj"))
               for source in SOURCES]
    sources = [str(SRC / source) for source in SOURCES]
    commands = [
        "@echo off",
        'call "%s" x86 || exit /b 1' % vcvars,
        "cl " + " ".join(cflags) + " " +
        " ".join('"%s"' % source for source in sources) + " || exit /b 1",
        "link " + " ".join(link) + " " +
        " ".join('"%s"' % obj for obj in objects) + " || exit /b 1",
    ]
    if args.test:
        test_obj = OBJ / "test_lifecycle.obj"
        commands.extend([
            "cl " + " ".join(cflags) + ' "%s" || exit /b 1' %
            (TESTS / "test_lifecycle.c"),
            "link /nologo /MACHINE:X86 /OUT:%s /PDB:%s kernel32.lib \"%s\" || exit /b 1" %
            (TEST_OUT, OBJ / "test_lifecycle.pdb", test_obj),
        ])
    batch = OBJ / "_build.bat"
    batch.write_text("\r\n".join(commands) + "\r\n", encoding="utf-8")
    process = subprocess.run(["cmd", "/c", str(batch)],
                             capture_output=True, text=True)
    output = (process.stdout or "") + (process.stderr or "")
    for line in output.splitlines():
        stripped = line.strip()
        if stripped and not stripped.startswith("**") and \
                not stripped.startswith("[vcvarsall") and \
                not stripped.endswith(".c"):
            print("  " + stripped)
    if process.returncode:
        print("BUILD FAILED (exit %d)" % process.returncode)
        print(output)
        return process.returncode

    helper.verify_is_win32(OUT, want_dll=True)
    helper.verify_pe_contract(OUT)
    if args.test:
        helper.verify_is_win32(TEST_OUT, want_dll=False)
        test = subprocess.run([str(TEST_OUT), str(OUT)],
                              capture_output=True, text=True)
        print((test.stdout or "") + (test.stderr or ""), end="")
        if test.returncode:
            return test.returncode
    modhash.write_stamp("fishing")
    ship_mda(None if args.no_deploy else find_game_root(args.game_root))
    if not args.no_deploy:
        helper.deploy([OUT], find_game_root(args.game_root))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
