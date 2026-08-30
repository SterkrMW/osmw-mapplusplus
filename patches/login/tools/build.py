"""Build osmw_login.dll (32-bit) with MSVC into Lib/, and deploy it for testing.

Same output rules as the chat mod's build.py: Lib/ is the shipped artifact and
is always written, the copy next to main.exe is a best-effort convenience for
the test loop. See that file's header for why.

    python patches/login/tools/build.py
    python patches/login/tools/build.py --debug
    python patches/login/tools/build.py --test
    python patches/login/tools/build.py --no-deploy
"""
from __future__ import annotations

import argparse
import os
import struct
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "common"))
from paths import PROJ, LIB, add_game_root_arg, find_game_root  # noqa: E402
import modhash  # noqa: E402

SRC = PROJ / "patches" / "login" / "src"
OBJ = PROJ / "patches" / "login" / "build"
TESTS = PROJ / "patches" / "login" / "tests"
OUT = LIB / "osmw_login.dll"
LAUNCHER_OUT = LIB / "osmw_login_launch.exe"
TEST_OUT = OBJ / "test_roster.exe"

SOURCES = ["dllmain.c", "scan.c", "hook.c", "config.c", "log.c",
           "roster.c", "claim.c", "autologin.c"]
LAUNCHER_SOURCES = ["launcher.c", "roster.c", "claim.c", "roster_ui.c"]
TEST_SOURCES = ["../tests/test_roster.c", "roster.c", "claim.c"]

# The two mods share one toolchain probe and one PE sanity check; the chat mod's
# build.py owns them because it was written first.
sys.path.insert(0, str(PROJ / "patches" / "chatmod" / "tools"))
from build import deploy, find_vcvarsall, verify_is_win32  # noqa: E402


def run_bat(lines: list[str]) -> int:
    OBJ.mkdir(parents=True, exist_ok=True)
    bat = OBJ / "_build.bat"
    bat.write_text("\r\n".join(lines) + "\r\n", encoding="utf-8")
    proc = subprocess.run(["cmd", "/c", str(bat)], capture_output=True, text=True)
    output = (proc.stdout or "") + (proc.stderr or "")
    for line in output.splitlines():
        s = line.strip()
        if not s or s.startswith("**") or s.startswith("[vcvarsall") or s.endswith(".c"):
            continue
        print("  " + s)
    if proc.returncode != 0:
        print("BUILD FAILED (exit %d)" % proc.returncode)
        if proc.stdout:
            print(proc.stdout)
        if proc.stderr:
            print(proc.stderr)
    return proc.returncode


def compile_flags(debug: bool, extra_inc: str = "") -> list[str]:
    cflags = [
        "/nologo", "/c", "/W4", "/GS-", "/MT",
        "/D_CRT_SECURE_NO_WARNINGS", "/DWIN32_LEAN_AND_MEAN",
        "/I", str(SRC),
        "/Fo:" + str(OBJ) + "\\",
    ]
    if extra_inc:
        cflags += ["/I", extra_inc]
    cflags += ["/Od", "/Zi", "/Fd:" + str(OBJ / "osmw_login.pdb")] if debug else ["/O2"]
    return cflags


def build_test() -> int:
    vcvars = find_vcvarsall()
    print("toolchain: %s" % vcvars)
    cflags = compile_flags(True)
    srcs = [str(TESTS / "test_roster.c"), str(SRC / "roster.c"), str(SRC / "claim.c")]
    objs = [str(OBJ / "test_roster.obj"), str(OBJ / "roster.obj"), str(OBJ / "claim.obj")]
    link = [
        "/nologo", "/MACHINE:X86", "/SUBSYSTEM:CONSOLE",
        "/OUT:" + str(TEST_OUT),
        "kernel32.lib", "advapi32.lib", "crypt32.lib",
    ]
    print("building %s ..." % TEST_OUT.name)
    rc = run_bat([
        "@echo off",
        'call "%s" x86 || exit /b 1' % vcvars,
        "cl " + " ".join(cflags) + " " + " ".join('"%s"' % s for s in srcs) + " || exit /b 1",
        "link " + " ".join(link) + " " + " ".join('"%s"' % o for o in objs) + " || exit /b 1",
    ])
    if rc != 0:
        return rc
    print("running %s ..." % TEST_OUT.name)
    proc = subprocess.run([str(TEST_OUT)], capture_output=True, text=True)
    print(proc.stdout or "")
    if proc.stderr:
        print(proc.stderr)
    return proc.returncode


def check_not_locked() -> None:
    for path in (OUT, LAUNCHER_OUT):
        if not path.exists():
            continue
        try:
            with open(path, "r+b"):
                pass
        except PermissionError:
            raise SystemExit(
                "%s is locked -- it is still loaded or running." % path.name
            )


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--debug", action="store_true")
    ap.add_argument("--test", action="store_true", help="build and run roster/claim tests")
    ap.add_argument("--no-deploy", action="store_true",
                    help="write Lib/ only; do not copy into the game folder")
    add_game_root_arg(ap)
    args = ap.parse_args()

    if args.test:
        return build_test()

    check_not_locked()
    LIB.mkdir(parents=True, exist_ok=True)
    vcvars = find_vcvarsall()
    print("toolchain: %s" % vcvars)
    cflags = compile_flags(args.debug)

    link = [
        "/nologo", "/DLL", "/MACHINE:X86",
        "/DEF:" + str(SRC / "osmw_login.def"),
        "/IMPLIB:" + str(OBJ / "osmw_login.lib"),
        "/OUT:" + str(OUT),
        "/PDB:" + str(OBJ / "osmw_login.pdb"),
        "kernel32.lib", "user32.lib", "advapi32.lib", "crypt32.lib",
    ]
    if args.debug:
        link.insert(2, "/DEBUG")

    objs = [str(OBJ / (Path(s).stem + ".obj")) for s in SOURCES]
    srcs = [str(SRC / s) for s in SOURCES]
    lobjs = [str(OBJ / (Path(s).stem + ".obj")) for s in LAUNCHER_SOURCES]
    # launcher objects share roster.obj / claim.obj names with the DLL compile
    lsrcs = [str(SRC / s) for s in LAUNCHER_SOURCES]

    launcher_link = [
        "/nologo", "/MACHINE:X86", "/SUBSYSTEM:WINDOWS",
        "/OUT:" + str(LAUNCHER_OUT),
        "/PDB:" + str(OBJ / "osmw_login_launch.pdb"),
        "kernel32.lib", "user32.lib", "advapi32.lib", "crypt32.lib", "gdi32.lib",
    ]

    print("building %s and %s ..." % (OUT.name, LAUNCHER_OUT.name))
    rc = run_bat([
        "@echo off",
        'call "%s" x86 || exit /b 1' % vcvars,
        "cl " + " ".join(cflags) + " " + " ".join('"%s"' % s for s in srcs) + " || exit /b 1",
        "link " + " ".join(link) + " " + " ".join('"%s"' % o for o in objs) + " || exit /b 1",
        "cl " + " ".join(cflags) + " " + " ".join('"%s"' % s for s in lsrcs) + " || exit /b 1",
        "link " + " ".join(launcher_link) + " " + " ".join('"%s"' % o for o in lobjs)
        + " || exit /b 1",
    ])
    if rc != 0:
        return rc
    verify_is_win32(OUT, want_dll=True)
    verify_is_win32(LAUNCHER_OUT, want_dll=False)
    print("ok: %s" % OUT)
    print("ok: %s" % LAUNCHER_OUT)

    # Record what these were built from, so build.ps1 can tell later
    # whether Lib/ still matches the sources. See patches/common/modhash.py.
    modhash.write_stamp("login")

    if not args.no_deploy:
        deploy([OUT, LAUNCHER_OUT], find_game_root(args.game_root))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
