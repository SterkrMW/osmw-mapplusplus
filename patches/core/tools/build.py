"""Build and verify the managed-bootstrap 32-bit osmw_core.dll.

The core is the future bootstrap for every native module.  It currently owns
owns game-root config, tagged logging, pristine-image signatures, exclusive
runtime claims, a strict managed-module loader, and the tested shared-hook/
dispatch substrate.  It still installs no client hooks.

    python patches/core/tools/build.py
    python patches/core/tools/build.py --debug
    python patches/core/tools/build.py --test
    python patches/core/tools/build.py --clean
    python patches/core/tools/build.py --no-deploy
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "common"))
from paths import PROJ, LIB, add_game_root_arg, find_game_root  # noqa: E402
import modhash  # noqa: E402
import peutil  # noqa: E402

ROOT = PROJ / "patches" / "core"
SRC = ROOT / "src"
SDK = ROOT / "sdk"
TESTS = ROOT / "tests"
OBJ = ROOT / "build"
OUT = LIB / "osmw_core.dll"
API_METADATA = LIB / "osmw_core_api.ini"

MANAGED_MODULES = (
    "osmw_chat.dll",
    "osmw_login.dll",
    "osmw_minimap.dll",
    "osmw_fightspeed.dll",
    "osmw_weather.dll",
    "osmw_settings.dll",
    "osmw_timers.dll",
    "osmw_inventory.dll",
    "osmw_fishing.dll",
)

SOURCES = [
    "dllmain.c", "core.c", "config.c", "log.c", "image.c", "scan.c",
    "loader.c", "claims.c", "hook.c", "dispatch.c", "attest.c",
]
APPROVED_IMPORT_DLLS = {"kernel32.dll"}

VSWHERE = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) \
    / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"


def find_vcvarsall() -> Path:
    """Locate vcvarsall.bat, preferring whatever vswhere reports."""
    candidates: list[Path] = []
    if VSWHERE.exists():
        try:
            found = subprocess.run(
                [str(VSWHERE), "-latest", "-products", "*",
                 "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                 "-property", "installationPath"],
                capture_output=True, text=True, check=False).stdout.strip()
            if found:
                candidates.append(
                    Path(found) / "VC" / "Auxiliary" / "Build" / "vcvarsall.bat")
        except OSError:
            pass

    for program_files in (r"C:\Program Files (x86)\Microsoft Visual Studio",
                          r"C:\Program Files\Microsoft Visual Studio"):
        base = Path(program_files)
        if base.exists():
            candidates.extend(sorted(
                base.glob("*/*/VC/Auxiliary/Build/vcvarsall.bat")))

    for candidate in candidates:
        if candidate.exists():
            return candidate
    raise SystemExit(
        "vcvarsall.bat not found. Install the Visual Studio Build Tools with the\n"
        "'MSVC v14x - VS C++ x64/x86 build tools' component."
    )


def run_batch(path: Path) -> int:
    proc = subprocess.run(["cmd", "/c", str(path)],
                          capture_output=True, text=True)
    output = (proc.stdout or "") + (proc.stderr or "")
    for line in output.splitlines():
        stripped = line.strip()
        if (not stripped or stripped.startswith("**") or
                stripped.startswith("[vcvarsall") or
                stripped.endswith((".c", ".cpp"))):
            continue
        print("  " + stripped)
    return proc.returncode


def core_abi_version() -> tuple[int, int]:
    """Read the public header so package metadata cannot invent an ABI."""
    header = (SDK / "osmw_core_api.h").read_text(encoding="ascii")
    current = re.search(
        r"^#define\s+OSMW_CORE_ABI_CURRENT\s+(OSMW_CORE_ABI_[A-Z0-9_]+)\s*$",
        header,
        re.MULTILINE,
    )
    if not current:
        raise SystemExit("OSMW_CORE_ABI_CURRENT alias not found in osmw_core_api.h")
    definition = re.search(
        r"^#define\s+%s\s+OSMW_ABI_MAKE\(\s*(\d+)\s*,\s*(\d+)\s*\)\s*$"
        % re.escape(current.group(1)),
        header,
        re.MULTILINE,
    )
    if not definition:
        raise SystemExit("current core ABI definition is not an OSMW_ABI_MAKE pair")
    return int(definition.group(1)), int(definition.group(2))


def write_api_metadata() -> None:
    major, minor = core_abi_version()
    loader = (SRC / "loader.c").read_text(encoding="ascii")
    start = loader.find("#define OSMW_DEFAULT_MODULES")
    end = loader.find("\n\n", start)
    if start < 0 or end < 0:
        raise SystemExit("OSMW_DEFAULT_MODULES definition not found in loader.c")
    compiled_modules = tuple(
        "".join(re.findall(r'"([^"]*)"', loader[start:end])).split(",")
    )
    if compiled_modules != MANAGED_MODULES:
        raise SystemExit(
            "package module list does not match loader.c OSMW_DEFAULT_MODULES"
        )
    packed = (major << 16) | minor
    content = (
        "[CoreApi]\n"
        "Format=1\n"
        f"Abi={major}.{minor}\n"
        f"Packed=0x{packed:08X}\n"
        f"Modules={','.join(MANAGED_MODULES)}\n"
    )
    API_METADATA.write_text(content, encoding="ascii", newline="\n")
    print("  wrote SDK metadata: %s (ABI %d.%d)" %
          (API_METADATA, major, minor))


def deploy(game: Path | None) -> None:
    if game is None:
        print("not deployed: no game folder configured (Lib/ payload is up to date)")
        return
    for source in (OUT, API_METADATA):
        target = game / source.name
        try:
            shutil.copy2(source, target)
            print("  deployed %s" % target)
        except OSError as exc:
            print("  NOT deployed: %s (%s)" % (target, exc))
            print("    a running client may hold it open -- close every client and retry")


def check_not_locked() -> None:
    if not OUT.exists():
        return
    try:
        with open(OUT, "r+b"):
            pass
    except PermissionError:
        raise SystemExit(
            "%s is locked -- it is still loaded. Close every client and retry."
            % OUT.name
        )


def clean() -> int:
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


def verify_is_win32(path: Path) -> None:
    data = path.read_bytes()
    pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
    machine = struct.unpack_from("<H", data, pe_offset + 4)[0]
    characteristics = struct.unpack_from("<H", data, pe_offset + 22)[0]
    if machine != 0x014C:
        raise SystemExit("%s is machine 0x%04X, expected x86" %
                         (path.name, machine))
    if not characteristics & 0x2000:
        raise SystemExit("%s does not carry IMAGE_FILE_DLL" % path.name)
    print("  verified %s: x86 DLL, %d bytes" % (path.name, len(data)))


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
    names = []
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
    if exports != ["osmw_prepare_unload"]:
        raise SystemExit("%s exports %r; expected only osmw_prepare_unload" %
                         (path.name, exports))
    print("  verified imports: %s" % ", ".join(sorted(imported)))
    print("  verified sole export: osmw_prepare_unload")


def write_build_batch(vcvars: Path, debug: bool) -> Path:
    cflags = [
        "/nologo", "/c", "/W4", "/WX", "/GS-", "/MT",
        "/D_CRT_SECURE_NO_WARNINGS", "/DWIN32_LEAN_AND_MEAN",
        "/I", str(SRC), "/I", str(SDK),
        "/Fo:" + str(OBJ) + "\\",
    ]
    cflags += (["/Od", "/Zi", "/Fd:" + str(OBJ / "osmw_core.pdb")]
               if debug else ["/O2"])
    link = [
        "/nologo", "/DLL", "/MACHINE:X86",
        "/DEF:" + str(SRC / "osmw_core.def"),
        "/IMPLIB:" + str(OBJ / "osmw_core.lib"),
        "/OUT:" + str(OUT),
        "/PDB:" + str(OBJ / "osmw_core.pdb"),
        "kernel32.lib",
    ]
    if debug:
        link.insert(2, "/DEBUG")

    sources = [str(SRC / name) for name in SOURCES]
    objects = [str(OBJ / (Path(name).stem + ".obj")) for name in SOURCES]
    batch = OBJ / "_build.bat"
    batch.write_text("\r\n".join([
        "@echo off",
        'call "%s" x86 || exit /b 1' % vcvars,
        "cl " + " ".join(cflags) + " " +
        " ".join('"%s"' % source for source in sources) + " || exit /b 1",
        "link " + " ".join(link) + " " +
        " ".join('"%s"' % obj for obj in objects) + " || exit /b 1",
    ]) + "\r\n", encoding="utf-8")
    return batch


def write_test_batch(vcvars: Path) -> Path:
    common = ("/nologo /W4 /WX /MT /D_CRT_SECURE_NO_WARNINGS "
              "/DWIN32_LEAN_AND_MEAN /I \"%s\" /I \"%s\"" %
              (SRC, SDK))
    batch = OBJ / "_tests.bat"
    commands = [
        "@echo off",
        'call "%s" x86 || exit /b 1' % vcvars,
        ('cl %s /TC "%s" /Fo:"%s" /Fe:"%s" || exit /b 1' %
         (common, TESTS / "test_api_layout.c",
          OBJ / "test_api_layout_c.obj", OBJ / "test_api_layout_c.exe")),
        ('cl %s /TP "%s" /Fo:"%s" /Fe:"%s" || exit /b 1' %
         (common, TESTS / "test_api_layout.c",
          OBJ / "test_api_layout_cpp.obj", OBJ / "test_api_layout_cpp.exe")),
        ('cl %s /LD "%s" /Fo:"%s" /link /MACHINE:X86 /DEF:"%s" '
         '/OUT:"%s" /IMPLIB:"%s" || exit /b 1' %
         (common, TESTS / "test_module_fixture.c",
          OBJ / "test_module_fixture.obj",
          TESTS / "test_module_fixture.def",
          OBJ / "test_module_fixture.dll",
          OBJ / "test_module_fixture.lib")),
        ('cl %s "%s" /Fo:"%s" /Fe:"%s" /link /MACHINE:X86 || exit /b 1' %
         (common, TESTS / "test_host_fixture.c",
          OBJ / "test_host_fixture.obj", OBJ / "test_host_fixture.exe")),
        ('cl %s "%s" /Fo:"%s" /Fe:"%s" /link /MACHINE:X86 || exit /b 1' %
         (common, TESTS / "test_core_lifecycle.c",
          OBJ / "test_core_lifecycle.obj", OBJ / "test_core_lifecycle.exe")),
        ('cl %s /c "%s" /Fo:"%s" || exit /b 1' %
         (common, TESTS / "test_config.c", OBJ / "test_config.obj")),
        ('link /nologo /MACHINE:X86 /OUT:"%s" "%s" "%s" "%s" "%s" '
         '"%s" "%s" "%s" "%s" "%s" "%s" "%s" kernel32.lib || exit /b 1' %
         (OBJ / "test_config.exe", OBJ / "test_config.obj",
          OBJ / "core.obj", OBJ / "config.obj", OBJ / "log.obj",
          OBJ / "image.obj", OBJ / "scan.obj", OBJ / "loader.obj",
          OBJ / "claims.obj", OBJ / "hook.obj", OBJ / "dispatch.obj", OBJ / "attest.obj")),
        ('cl %s /c "%s" /Fo:"%s" || exit /b 1' %
         (common, TESTS / "test_log.c", OBJ / "test_log.obj")),
        ('cl %s /DOSMW_LOG_MAX_BYTES=4096 /c "%s" /Fo:"%s" || exit /b 1' %
         (common, SRC / "log.c", OBJ / "test_log_impl.obj")),
        ('link /nologo /MACHINE:X86 /OUT:"%s" "%s" "%s" "%s" "%s" '
         '"%s" "%s" "%s" "%s" "%s" "%s" "%s" kernel32.lib || exit /b 1' %
         (OBJ / "test_log.exe", OBJ / "test_log.obj",
          OBJ / "core.obj", OBJ / "config.obj", OBJ / "test_log_impl.obj",
          OBJ / "image.obj", OBJ / "scan.obj", OBJ / "loader.obj",
          OBJ / "claims.obj", OBJ / "hook.obj", OBJ / "dispatch.obj", OBJ / "attest.obj")),
        ('cl %s /c "%s" /Fo:"%s" || exit /b 1' %
         (common, TESTS / "test_image.c", OBJ / "test_image.obj")),
        ('link /nologo /MACHINE:X86 /OUT:"%s" "%s" "%s" '
         'kernel32.lib || exit /b 1' %
         (OBJ / "test_image.exe", OBJ / "test_image.obj", OBJ / "image.obj")),
        ('cl %s /c "%s" /Fo:"%s" || exit /b 1' %
         (common, TESTS / "test_scan.c", OBJ / "test_scan.obj")),
        ('link /nologo /MACHINE:X86 /OUT:"%s" "%s" "%s" "%s" "%s" '
         '"%s" "%s" "%s" "%s" "%s" "%s" "%s" kernel32.lib || exit /b 1' %
         (OBJ / "test_scan.exe", OBJ / "test_scan.obj", OBJ / "core.obj",
          OBJ / "config.obj", OBJ / "log.obj", OBJ / "image.obj",
          OBJ / "scan.obj", OBJ / "loader.obj", OBJ / "claims.obj",
          OBJ / "hook.obj", OBJ / "dispatch.obj", OBJ / "attest.obj")),
        ('cl %s /LD "%s" /Fo:"%s" /link /MACHINE:X86 /DEF:"%s" '
         '/OUT:"%s" /IMPLIB:"%s" || exit /b 1' %
         (common, TESTS / "fixtures" / "good" / "good.c",
          OBJ / "fixture_good.obj", TESTS / "fixtures" / "good" / "good.def",
          OBJ / "fixture_good.dll", OBJ / "fixture_good.lib")),
        ('cl %s /LD "%s" /Fo:"%s" /link /MACHINE:X86 /DEF:"%s" '
         '/OUT:"%s" /IMPLIB:"%s" || exit /b 1' %
         (common, TESTS / "fixtures" / "good" / "disabled.c",
          OBJ / "fixture_disabled.obj",
          TESTS / "fixtures" / "good" / "disabled.def",
          OBJ / "fixture_disabled.dll", OBJ / "fixture_disabled.lib")),
        ('cl %s /LD "%s" /Fo:"%s" /link /MACHINE:X86 /DEF:"%s" '
         '/OUT:"%s" /IMPLIB:"%s" || exit /b 1' %
         (common, TESTS / "fixtures" / "good" / "duplicate.c",
          OBJ / "fixture_duplicate.obj",
          TESTS / "fixtures" / "good" / "duplicate.def",
          OBJ / "fixture_duplicate.dll", OBJ / "fixture_duplicate.lib")),
        ('cl %s /LD "%s" /Fo:"%s" /link /MACHINE:X86 /DEF:"%s" '
         '/OUT:"%s" /IMPLIB:"%s" || exit /b 1' %
         (common, TESTS / "fixtures" / "incompatible" / "incompatible.c",
          OBJ / "fixture_incompatible.obj",
          TESTS / "fixtures" / "incompatible" / "incompatible.def",
          OBJ / "fixture_incompatible.dll", OBJ / "fixture_incompatible.lib")),
        ('cl %s /LD "%s" /Fo:"%s" /link /MACHINE:X86 /DEF:"%s" '
         '/OUT:"%s" /IMPLIB:"%s" || exit /b 1' %
         (common, TESTS / "fixtures" / "failed" / "failed.c",
          OBJ / "fixture_failed.obj",
          TESTS / "fixtures" / "failed" / "failed.def",
          OBJ / "fixture_failed.dll", OBJ / "fixture_failed.lib")),
        ('cl %s /LD "%s" /Fo:"%s" /link /MACHINE:X86 '
         '/OUT:"%s" || exit /b 1' %
         (common, TESTS / "fixtures" / "legacy" / "legacy.c",
          OBJ / "fixture_legacy.obj", OBJ / "fixture_legacy.dll")),
        ('cl %s /c "%s" /Fo:"%s" || exit /b 1' %
         (common, TESTS / "test_loader.c", OBJ / "test_loader.obj")),
        ('link /nologo /MACHINE:X86 /OUT:"%s" "%s" "%s" "%s" "%s" '
         '"%s" "%s" "%s" "%s" "%s" "%s" "%s" kernel32.lib || exit /b 1' %
         (OBJ / "test_loader.exe", OBJ / "test_loader.obj", OBJ / "core.obj",
          OBJ / "config.obj", OBJ / "log.obj", OBJ / "image.obj",
          OBJ / "scan.obj", OBJ / "loader.obj", OBJ / "claims.obj",
          OBJ / "hook.obj", OBJ / "dispatch.obj", OBJ / "attest.obj")),
        ('cl %s /c "%s" /Fo:"%s" || exit /b 1' %
         (common, TESTS / "test_missing_features.c",
          OBJ / "test_missing_features.obj")),
        ('link /nologo /MACHINE:X86 /OUT:"%s" "%s" "%s" "%s" "%s" '
         '"%s" "%s" "%s" "%s" "%s" "%s" "%s" kernel32.lib || exit /b 1' %
         (OBJ / "test_missing_features.exe",
          OBJ / "test_missing_features.obj", OBJ / "core.obj",
          OBJ / "config.obj", OBJ / "log.obj", OBJ / "image.obj",
          OBJ / "scan.obj", OBJ / "loader.obj", OBJ / "claims.obj",
          OBJ / "hook.obj", OBJ / "dispatch.obj", OBJ / "attest.obj")),
        ('cl %s /LD "%s" /Fo:"%s" /link /MACHINE:X86 /DEF:"%s" '
         '/OUT:"%s" /IMPLIB:"%s" || exit /b 1' %
         (common, TESTS / "fixtures" / "failed" / "unrestored.c",
          OBJ / "fixture_unrestored.obj",
          TESTS / "fixtures" / "failed" / "unrestored.def",
          OBJ / "fixture_unrestored.dll", OBJ / "fixture_unrestored.lib")),
        ('cl %s /c "%s" /Fo:"%s" || exit /b 1' %
         (common, TESTS / "test_claims.c", OBJ / "test_claims.obj")),
        ('link /nologo /MACHINE:X86 /OUT:"%s" "%s" "%s" "%s" "%s" '
         '"%s" "%s" "%s" "%s" "%s" "%s" "%s" kernel32.lib || exit /b 1' %
         (OBJ / "test_claims.exe", OBJ / "test_claims.obj", OBJ / "core.obj",
          OBJ / "config.obj", OBJ / "log.obj", OBJ / "image.obj",
          OBJ / "scan.obj", OBJ / "loader.obj", OBJ / "claims.obj",
          OBJ / "hook.obj", OBJ / "dispatch.obj", OBJ / "attest.obj")),
        ('cl %s /c "%s" /Fo:"%s" || exit /b 1' %
         (common, TESTS / "test_dispatch.c", OBJ / "test_dispatch.obj")),
        ('link /nologo /MACHINE:X86 /OUT:"%s" "%s" "%s" "%s" "%s" '
         '"%s" "%s" "%s" "%s" "%s" "%s" "%s" kernel32.lib || exit /b 1' %
         (OBJ / "test_broker.exe", OBJ / "test_dispatch.obj", OBJ / "core.obj",
          OBJ / "config.obj", OBJ / "log.obj", OBJ / "image.obj",
          OBJ / "scan.obj", OBJ / "loader.obj", OBJ / "claims.obj",
          OBJ / "hook.obj", OBJ / "dispatch.obj", OBJ / "attest.obj")),
        ('cl %s /c "%s" /Fo:"%s" || exit /b 1' %
         (common, TESTS / "test_packets.c", OBJ / "test_packets.obj")),
        ('cl %s /c "%s" /Fo:"%s" || exit /b 1' %
         (common, TESTS / "test_attest.c", OBJ / "test_attest.obj")),
        ('link /nologo /MACHINE:X86 /OUT:"%s" "%s" "%s" "%s" "%s" '
         '"%s" "%s" "%s" "%s" "%s" "%s" "%s" kernel32.lib || exit /b 1' %
         (OBJ / "test_attest.exe", OBJ / "test_attest.obj", OBJ / "core.obj",
          OBJ / "config.obj", OBJ / "log.obj", OBJ / "image.obj",
          OBJ / "scan.obj", OBJ / "loader.obj", OBJ / "claims.obj",
          OBJ / "hook.obj", OBJ / "dispatch.obj", OBJ / "attest.obj")),
        ('link /nologo /MACHINE:X86 /OUT:"%s" "%s" "%s" "%s" "%s" '
         '"%s" "%s" "%s" "%s" "%s" "%s" "%s" kernel32.lib || exit /b 1' %
         (OBJ / "test_packets.exe", OBJ / "test_packets.obj", OBJ / "core.obj",
          OBJ / "config.obj", OBJ / "log.obj", OBJ / "image.obj",
          OBJ / "scan.obj", OBJ / "loader.obj", OBJ / "claims.obj",
          OBJ / "hook.obj", OBJ / "dispatch.obj", OBJ / "attest.obj")),
    ]
    batch.write_text("\r\n".join(commands) + "\r\n", encoding="utf-8")
    return batch


def run_tests(vcvars: Path) -> int:
    print("building core ABI/lifecycle fixtures ...")
    result = run_batch(write_test_batch(vcvars))
    if result != 0:
        print("TEST BUILD FAILED (exit %d)" % result)
        return result

    runs = [
        [str(OBJ / "test_api_layout_c.exe")],
        [str(OBJ / "test_api_layout_cpp.exe")],
        [str(OBJ / "test_host_fixture.exe"),
         str(OBJ / "test_module_fixture.dll")],
        [str(OBJ / "test_core_lifecycle.exe"), str(OUT)],
        [str(OBJ / "test_config.exe")],
        [str(OBJ / "test_log.exe")],
        [str(OBJ / "test_image.exe")],
        [str(OBJ / "test_scan.exe")],
        [str(OBJ / "test_loader.exe"),
         str(OBJ / "fixture_good.dll"),
         str(OBJ / "fixture_disabled.dll"),
         str(OBJ / "fixture_failed.dll"),
         str(OBJ / "fixture_duplicate.dll"),
         str(OBJ / "fixture_incompatible.dll"),
         str(OBJ / "fixture_legacy.dll")],
        [str(OBJ / "test_missing_features.exe"),
         str(OBJ / "test_module_fixture.dll")],
        [str(OBJ / "test_claims.exe"),
         str(OBJ / "fixture_unrestored.dll"),
         str(OBJ / "fixture_good.dll")],
        [str(OBJ / "test_broker.exe")],
        [str(OBJ / "test_packets.exe")],
        [str(OBJ / "test_attest.exe")],
    ]
    for command in runs:
        proc = subprocess.run(command, capture_output=True, text=True)
        output = (proc.stdout or "") + (proc.stderr or "")
        for line in output.splitlines():
            if line.strip():
                print("  " + line.strip())
        if proc.returncode != 0:
            print("TEST FAILED: %s (exit %d)" %
                  (Path(command[0]).name, proc.returncode))
            return proc.returncode

    # Exercise the real DLL's GetModuleFileNameA(NULL) path, opt-in decision,
    # per-process filename and tagged startup line in an isolated directory.
    with tempfile.TemporaryDirectory(prefix="osmw-core-live-") as temporary:
        live = Path(temporary)
        live_host = live / "test_core_lifecycle.exe"
        live_dll = live / OUT.name
        shutil.copy2(OBJ / "test_core_lifecycle.exe", live_host)
        shutil.copy2(OUT, live_dll)
        (live / "config.ini").write_text(
            "[Debug]\nModLogging=1\n", encoding="ascii")
        proc = subprocess.run([str(live_host), str(live_dll)],
                              capture_output=True, text=True)
        if proc.returncode != 0:
            print("TEST FAILED: enabled real-DLL lifecycle (exit %d)" %
                  proc.returncode)
            return proc.returncode
        logs = list(live.glob("osmw_core.*.log"))
        if len(logs) != 1:
            print("TEST FAILED: expected one per-process core log, found %d" %
                  len(logs))
            return 1
        content = logs[0].read_text(encoding="ascii", errors="replace")
        if ("[core] pristine image ready" not in content or
                "[core] startup complete" not in content or
                "[core] shutdown complete" not in content):
            print("TEST FAILED: real DLL log is missing image/lifecycle lines")
            return 1
        print("  real DLL opt-in log fixture passed: %s" % logs[0].name)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--debug", action="store_true",
                        help="unoptimised build with symbols")
    parser.add_argument("--test", action="store_true",
                        help="run ABI, services, loader, claims and lifecycle fixtures")
    parser.add_argument("--clean", action="store_true",
                        help="delete build intermediates and exit")
    parser.add_argument("--no-deploy", action="store_true",
                        help="write Lib/ only; do not copy into the game folder")
    add_game_root_arg(parser)
    args = parser.parse_args()

    if args.clean:
        return clean()

    check_not_locked()
    LIB.mkdir(parents=True, exist_ok=True)
    OBJ.mkdir(parents=True, exist_ok=True)
    vcvars = find_vcvarsall()
    print("toolchain: %s" % vcvars)

    print("building %s ..." % OUT.name)
    result = run_batch(write_build_batch(vcvars, args.debug))
    if result != 0:
        print("BUILD FAILED (exit %d)" % result)
        return result

    verify_is_win32(OUT)
    verify_pe_contract(OUT)
    write_api_metadata()
    print("ok: %s" % OUT)

    if args.test:
        result = run_tests(vcvars)
        if result != 0:
            return result

    modhash.write_stamp("core")
    if not args.no_deploy:
        deploy(find_game_root(args.game_root))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
