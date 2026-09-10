"""Build and verify the managed 32-bit osmw_minimap.dll.

The target is an MSVC-built 2006 binary and we call into it with __thiscall,
so this uses cl.exe from the x86 toolchain, not the mingw64 that happens to be
on PATH.

Output goes to the repo's Lib/, which is what build.ps1 copies into a release
and what Maps++ deploys into a game folder that does not have it yet. That is
the shipped artifact, so it is the one the build must always produce.

It is then also copied into the game folder, because the test loop is to build
and immediately launch. That copy is best-effort: a client holding the DLL open
locks the file, and failing to deploy must not fail the build when Lib/ -- the
thing being shipped -- was written correctly.

    python patches/minimap/tools/build.py
    python patches/minimap/tools/build.py --debug
    python patches/minimap/tools/build.py --test
    python patches/minimap/tools/build.py --clean       remove the intermediates
    python patches/minimap/tools/build.py --no-deploy   Lib/ only
"""
from __future__ import annotations

import argparse
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "common"))
from paths import PROJ, LIB, add_game_root_arg, find_game_root  # noqa: E402
import modhash  # noqa: E402
import peutil  # noqa: E402

ROOT = PROJ / "patches" / "minimap"
SRC = ROOT / "src"
SDK = PROJ / "patches" / "core" / "sdk"
TESTS = ROOT / "tests"
OBJ = ROOT / "build"
OUT = LIB / "osmw_minimap.dll"
TEST_OUT = OBJ / "test_lifecycle.exe"
MARKER_TEST_OUT = OBJ / "test_markers.exe"

SOURCES = ["dllmain.c", "scan.c", "hook.c", "config.c", "mapchars.c",
           "grid.c", "pathfind.c", "walk.c", "panel.c", "log.c",
           "markers.c", "markers_io.c", "markers_map.c",
           "markers_edit.c"]
APPROVED_IMPORT_DLLS = {"kernel32.dll"}

# No feature-local launcher. Core owns bootstrap and managed unloading.


def deploy(built: list[Path], game: Path | None) -> None:
    """Copy freshly built binaries next to main.exe, reporting what happened.

    Never raises: Lib/ is the deliverable and is already written by this point.
    A locked file here means a client still has it loaded, which is worth
    saying plainly because it is the usual reason a change appears to do
    nothing.
    """
    if game is None:
        print("not deployed: no game folder configured (Lib/ is up to date)")
        return
    for path in built:
        target = game / path.name
        try:
            shutil.copy2(path, target)
            print("  deployed %s" % target)
        except OSError as exc:
            print("  NOT deployed: %s (%s)" % (target, exc))
            print("    a running client is probably holding it open -- close "
                  "every client, then re-run with --no-deploy skipped")

VSWHERE = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) \
    / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"


def find_vcvarsall() -> Path:
    """Locate vcvarsall.bat, preferring whatever vswhere reports."""
    candidates: list[Path] = []
    if VSWHERE.exists():
        try:
            out = subprocess.run(
                [str(VSWHERE), "-latest", "-products", "*",
                 "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                 "-property", "installationPath"],
                capture_output=True, text=True, check=False).stdout.strip()
            if out:
                candidates.append(Path(out) / "VC" / "Auxiliary" / "Build" / "vcvarsall.bat")
        except OSError:
            pass

    for pf in (r"C:\Program Files (x86)\Microsoft Visual Studio",
               r"C:\Program Files\Microsoft Visual Studio"):
        base = Path(pf)
        if base.exists():
            candidates.extend(sorted(base.glob("*/*/VC/Auxiliary/Build/vcvarsall.bat")))

    for c in candidates:
        if c.exists():
            return c
    raise SystemExit(
        "vcvarsall.bat not found. Install the Visual Studio Build Tools with the\n"
        "'MSVC v14x - VS C++ x64/x86 build tools' component."
    )


def verify_is_win32(path: Path, want_dll: bool) -> None:
    data = path.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    machine = struct.unpack_from("<H", data, pe + 4)[0]
    chars = struct.unpack_from("<H", data, pe + 22)[0]
    if machine != 0x014C:
        raise SystemExit("%s is machine 0x%04X, expected 0x014C (x86)"
                         % (path.name, machine))
    is_dll = bool(chars & 0x2000)
    if is_dll != want_dll:
        raise SystemExit("%s: IMAGE_FILE_DLL is %s, expected %s"
                         % (path.name, is_dll, want_dll))
    print("  verified %s: x86, %s, %d bytes"
          % (path.name, "DLL" if is_dll else "EXE", len(data)))


def export_names(path: Path) -> list[str]:
    image = peutil.load_pe(path)
    pe_offset = struct.unpack_from("<I", image.data, 0x3C)[0]
    export_rva = struct.unpack_from(
        "<I", image.data, pe_offset + 24 + 96)[0]
    if export_rva == 0:
        return []
    directory = peutil.rva_to_off(image, export_rva)
    name_count = struct.unpack_from("<I", image.data, directory + 24)[0]
    names_rva = struct.unpack_from("<I", image.data, directory + 32)[0]
    names_offset = peutil.rva_to_off(image, names_rva)
    names: list[str] = []
    for index in range(name_count):
        name_rva = struct.unpack_from(
            "<I", image.data, names_offset + index * 4)[0]
        names.append(peutil.read_cstr(
            image.data, peutil.rva_to_off(image, name_rva)))
    return sorted(names)


def verify_pe_contract(path: Path) -> None:
    image = peutil.load_pe(path)
    imported = set(image.imports)
    unexpected = imported - APPROVED_IMPORT_DLLS
    if unexpected:
        raise SystemExit("%s imports unapproved DLL(s): %s" %
                         (path.name, ", ".join(sorted(unexpected))))
    exports = export_names(path)
    if exports != ["osmw_mod_query"]:
        raise SystemExit("%s exports %r; expected only osmw_mod_query" %
                         (path.name, exports))
    print("  verified imports: %s" % ", ".join(sorted(imported)))
    print("  verified sole export: osmw_mod_query")


def check_not_locked() -> None:
    """A loaded DLL cannot be overwritten; say so plainly instead of leaking
    LNK1104 out of the linker."""
    for path in (OUT,):
        if not path.exists():
            continue
        try:
            with open(path, "r+b"):
                pass
        except PermissionError:
            raise SystemExit(
                "%s is locked -- it is still loaded or running.\n"
                "Eject it first:\n"
                "    eject osmw_core or close every client" % path.name
            )


def clean() -> int:
    """Drop everything in build/. All of it is regenerated by a plain build --
    the .obj files, the import library, the .exp, and _build.bat itself -- so
    this never costs more than one rebuild."""
    if not OBJ.exists():
        print("nothing to clean")
        return 0
    removed = 0
    for path in sorted(OBJ.iterdir()):
        if path.is_file():
            path.unlink()
            removed += 1
    print("removed %d intermediate(s) from %s" % (removed, OBJ))
    print("the shipped %s is untouched" % OUT.name)
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--debug", action="store_true", help="unoptimised build with symbols")
    ap.add_argument("--test", action="store_true",
                    help="run managed lifecycle/claim/tick fixtures")
    ap.add_argument("--clean", action="store_true",
                    help="delete build intermediates and exit")
    ap.add_argument("--no-deploy", action="store_true",
                    help="write Lib/ only; do not copy into the game folder")
    add_game_root_arg(ap)
    args = ap.parse_args()

    if args.clean:
        return clean()

    check_not_locked()
    LIB.mkdir(parents=True, exist_ok=True)
    OBJ.mkdir(parents=True, exist_ok=True)
    vcvars = find_vcvarsall()
    print("toolchain: %s" % vcvars)

    cflags = [
        "/nologo", "/c", "/W4", "/GS-", "/MT",
        "/D_CRT_SECURE_NO_WARNINGS", "/DWIN32_LEAN_AND_MEAN",
        "/I", str(SRC), "/I", str(SDK),
        "/Fo:" + str(OBJ) + "\\",
    ]
    cflags += ["/Od", "/Zi", "/Fd:" + str(OBJ / "osmw_minimap.pdb")] if args.debug else ["/O2"]

    link = [
        "/nologo", "/DLL", "/MACHINE:X86",
        "/DEF:" + str(SRC / "osmw_minimap.def"),
        # keep the import library and exports file out of the game root
        "/IMPLIB:" + str(OBJ / "osmw_minimap.lib"),
        "/OUT:" + str(OUT),
        "/PDB:" + str(OBJ / "osmw_minimap.pdb"),
        "kernel32.lib", "user32.lib", "gdi32.lib",
    ]
    if args.debug:
        link.insert(2, "/DEBUG")

    objs = [str(OBJ / (Path(s).stem + ".obj")) for s in SOURCES]
    srcs = [str(SRC / s) for s in SOURCES]
    # Write a batch file rather than passing this to `cmd /c` inline: the paths
    # contain spaces and cmd's own quote handling mangles a one-liner.
    commands = [
        "@echo off",
        'call "%s" x86 || exit /b 1' % vcvars,
        "cl " + " ".join(cflags) + " " + " ".join('"%s"' % s for s in srcs) + " || exit /b 1",
        "link " + " ".join(link) + " " + " ".join('"%s"' % o for o in objs) + " || exit /b 1",
    ]
    if args.test:
        test_obj = OBJ / "test_lifecycle.obj"
        test_link = [
            "/nologo", "/MACHINE:X86", "/OUT:" + str(TEST_OUT),
            "/PDB:" + str(OBJ / "test_lifecycle.pdb"), "kernel32.lib",
        ]
        # The marker fixture compiles markers.c / markers_io.c directly rather
        # than loading the DLL: the store is not on the module's export surface,
        # and its file half wants a scratch directory, not a game folder.
        marker_obj = OBJ / "test_markers.obj"
        marker_link = [
            "/nologo", "/MACHINE:X86", "/OUT:" + str(MARKER_TEST_OUT),
            "/PDB:" + str(OBJ / "test_markers.pdb"), "kernel32.lib",
        ]
        commands.extend([
            "cl " + " ".join(cflags) + ' "%s" || exit /b 1' %
            (TESTS / "test_lifecycle.c"),
            "link " + " ".join(test_link) + ' "%s" || exit /b 1' % test_obj,
            "cl " + " ".join(cflags) + ' "%s" || exit /b 1' %
            (TESTS / "test_markers.c"),
            "link " + " ".join(marker_link) + ' "%s" || exit /b 1' % marker_obj,
        ])
    bat = OBJ / "_build.bat"
    bat.write_text("\r\n".join(commands) + "\r\n", encoding="utf-8")

    print("building %s ..." % OUT.name)
    proc = subprocess.run(["cmd", "/c", str(bat)], capture_output=True, text=True)
    output = (proc.stdout or "") + (proc.stderr or "")
    for line in output.splitlines():
        s = line.strip()
        if not s or s.startswith("**") or s.startswith("[vcvarsall") or s.endswith(".c"):
            continue
        print("  " + s)

    if proc.returncode != 0:
        print("BUILD FAILED (exit %d)" % proc.returncode)
        return proc.returncode

    verify_is_win32(OUT, want_dll=True)
    verify_pe_contract(OUT)
    print("ok: %s" % OUT)

    if args.test:
        verify_is_win32(TEST_OUT, want_dll=False)
        test = subprocess.run([str(TEST_OUT), str(OUT)],
                              capture_output=True, text=True)
        for line in ((test.stdout or "") + (test.stderr or "")).splitlines():
            if line.strip():
                print("  " + line.strip())
        if test.returncode != 0:
            print("TEST FAILED: %s (exit %d)" %
                  (TEST_OUT.name, test.returncode))
            return test.returncode

        verify_is_win32(MARKER_TEST_OUT, want_dll=False)
        markers = subprocess.run([str(MARKER_TEST_OUT)],
                                 capture_output=True, text=True)
        for line in ((markers.stdout or "") + (markers.stderr or "")).splitlines():
            if line.strip():
                print("  " + line.strip())
        if markers.returncode != 0:
            print("TEST FAILED: %s (exit %d)" %
                  (MARKER_TEST_OUT.name, markers.returncode))
            return markers.returncode

    # Record what these were built from, so build.ps1 can tell later
    # whether Lib/ still matches the sources. See patches/common/modhash.py.
    modhash.write_stamp("minimap")

    if not args.no_deploy:
        deploy([OUT], find_game_root(args.game_root))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
