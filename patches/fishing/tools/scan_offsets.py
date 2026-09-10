"""Resolve Fishing_* signatures against the configured main.exe."""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "common"))
from paths import PROJ, add_game_root_arg, game_root  # noqa: E402
sys.path.insert(0, str(PROJ / "patches" / "chatmod" / "tools"))
import scan_offsets as chat  # noqa: E402

PREFIX = "Fishing_"
ROOT = PROJ / "patches" / "fishing"
HEADER_TEMPLATE = chat.HEADER_TEMPLATE.replace(
    "patches/chatmod/tools/scan_offsets.py",
    "patches/fishing/tools/scan_offsets.py",
).replace("ChatMod_*", "Fishing_*").replace(
    "OSMW_SIGS_H", "OSFG_SIGS_H"
).replace("mw_sig_t", "fg_sig_t").replace(
    "MW_SIGS", "FG_SIGS"
).replace("MW_SIG_COUNT", "FG_SIG_COUNT").replace(
    "MW_ADDR_COUNT", "FG_ADDR_COUNT"
).replace("MW_%s,", "FG_%s,").replace("mw_addr[]", "fg_addr[]")


def read_signatures(path: Path) -> dict[str, dict[str, str]]:
    result: dict[str, dict[str, str]] = {}
    section: str | None = None
    for raw in path.read_text(encoding="utf-8-sig").splitlines():
        line = raw.strip()
        if not line or line.startswith(";"):
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1]
            result[section] = {}
        elif section and "=" in line:
            key, value = line.split("=", 1)
            result[section][key.strip()] = value.strip()
    return result


def generate_header(pe, signatures, resolved) -> str:
    blobs: list[str] = []
    rows: list[str] = []
    enums: list[str] = []
    for name, body in signatures.items():
        if not name.startswith(PREFIX) or "sig" not in body:
            continue
        raw, mask = chat.parse_pattern(body["sig"])
        short = name[len(PREFIX):]
        variable = "sig_" + short.lower()
        blobs.append("static const unsigned char %s[] = {%s};" %
                     (variable, ", ".join("0x%02X" % byte for byte in raw)))
        optional = 0 if body.get("optional", "0") in (
            "0", "", "false") else 1
        rows.append('    {"%s", %s, "%s", %d, %d, 0x%08XU, %d},' %
                    (short, variable,
                     "".join("x" if required else "?" for required in mask),
                     len(raw), int(body.get("opOffset", -1)),
                     resolved[name], optional))
        enums.append("    FG_%s," % short.upper())
    return HEADER_TEMPLATE.format(
        ts=pe.timestamp,
        blobs="\n".join(blobs),
        rows="\n".join(rows) + "\n",
        enums="\n".join(enums) + "\n",
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--write", action="store_true",
                        help="update offsets_cache.ini and regenerate src/sigs.h")
    add_game_root_arg(parser)
    args = parser.parse_args()
    from peutil import load_pe  # noqa: E402

    game = game_root(args.game_root)
    pe = load_pe(game / "main.exe")
    signatures = read_signatures(ROOT / "signatures.ini")
    ours = {key: value for key, value in signatures.items()
            if key.startswith(PREFIX)}
    chat.PREFIX = PREFIX
    resolved, problems = chat.scan(pe, signatures)
    print("main.exe build 0x%08X  (%d fishing signatures)  rva / va" %
          (pe.timestamp, len(ours)))
    for name in ours:
        if name in resolved:
            print("  OK   %-32s 0x%08X / 0x%08X" %
                  (name, resolved[name], resolved[name] + pe.image_base))
        else:
            print("  FAIL %-32s" % name)
    for problem in problems:
        print("  !! %s" % problem)
    failed = [name for name in ours if name not in resolved]
    if failed:
        print("\n%d fishing signature(s) did not resolve." % len(failed))
        return 1
    print("\nall fishing signatures resolved uniquely")
    if args.write:
        chat.update_offsets_cache(
            game / "offsets_cache.ini", "%08X" % pe.timestamp,
            {key: value for key, value in resolved.items()
             if key.startswith(PREFIX)},
        )
        output = ROOT / "src" / "sigs.h"
        output.write_text(generate_header(pe, signatures, resolved),
                          encoding="utf-8")
        print("wrote %s" % output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
