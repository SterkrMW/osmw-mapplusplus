"""Resolve Timer_* signatures against main.exe. Same contract as chatmod's scanner.

UI patterns are the already-proven settings-panel signatures. Shared UiTick,
HandlePacket and SendPacket dependencies are intentionally absent: the managed
module receives those services from core. The signature source is local to this
mod so a game install does not need development-only Timer_* sections.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "common"))
from paths import PROJ, add_game_root_arg, game_root  # noqa: E402
sys.path.insert(0, str(PROJ / "patches" / "chatmod" / "tools"))
import scan_offsets as chat  # noqa: E402

chat.PREFIX = "Timer_"
chat.HEADER_TEMPLATE = chat.HEADER_TEMPLATE.replace(
	"patches/chatmod/tools/scan_offsets.py",
	"patches/timers/tools/scan_offsets.py",
).replace("ChatMod_*", "Timer_*").replace("mw_sig_t", "tm_sig_t").replace(
	"MW_SIGS", "TM_SIGS"
).replace("MW_SIG_COUNT", "TM_SIG_COUNT").replace(
	"MW_ADDR_COUNT", "TM_ADDR_COUNT"
).replace("    MW_%s,", "    TM_%s,")

ROOT = PROJ / "patches" / "timers"
SRC = ROOT / "src"


def read_local_signatures(path: Path) -> dict[str, dict[str, str]]:
    """Parse the timer-local UTF-8 signature catalogue."""
    out: dict[str, dict[str, str]] = {}
    section: str | None = None
    for raw_line in path.read_text(encoding="utf-8-sig").splitlines():
        line = raw_line.strip()
        if not line or line.startswith(";"):
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1]
            out[section] = {}
        elif section and "=" in line:
            key, value = line.split("=", 1)
            out[section][key.strip()] = value.strip()
    return out


def generate_header(pe, sigs, resolved) -> str:
    """Chatmod's generated-header shape, including the `optional` field."""
    blobs, rows, enums = [], [], []
    for name, body in sigs.items():
        if not name.startswith(chat.PREFIX) or "sig" not in body:
            continue
        raw, mask = chat.parse_pattern(body["sig"])
        short = name[len(chat.PREFIX):]
        var = "sig_" + short.lower()
        blobs.append("static const unsigned char %s[] = {%s};"
                     % (var, ", ".join("0x%02X" % b for b in raw)))
        optional = 0 if body.get("optional", "0") in ("0", "", "false") else 1
        rows.append('    {"%s", %s, "%s", %d, %d, 0x%08XU, %d},'
                    % (short, var, "".join("x" if m else "?" for m in mask),
                       len(raw), int(body.get("opOffset", -1)),
                       resolved.get(name, 0), optional))
        enums.append("    TM_%s," % short.upper())
    return chat.HEADER_TEMPLATE.format(
        ts=pe.timestamp,
        blobs="\n".join(blobs),
        rows="\n".join(rows) + "\n",
        enums="\n".join(enums) + "\n",
    )


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true",
                    help="update offsets_cache.ini and regenerate src/sigs.h")
    add_game_root_arg(ap)
    args = ap.parse_args()

    from peutil import load_pe  # noqa: E402

    game = game_root(getattr(args, "game_root", None))
    pe = load_pe(game / "main.exe")
    sigs = read_local_signatures(ROOT / "signatures.ini")
    ours = {k: v for k, v in sigs.items() if k.startswith(chat.PREFIX)}
    if not ours:
        print("no Timer_* sections in patches/timers/signatures.ini")
        return 1

    resolved, problems = chat.scan(pe, sigs)
    print("main.exe build 0x%08X  (%d Timer_ signatures)  rva / va"
          % (pe.timestamp, len(ours)))
    for name in ours:
        if name in resolved:
            print("  OK   %-28s 0x%08X / 0x%08X"
                  % (name, resolved[name], resolved[name] + pe.image_base))
        else:
            print("  FAIL %-28s" % name)
    for p in problems:
        print("  !! %s" % p)

    ours_failed = [n for n in ours if n not in resolved]
    if ours_failed:
        print("\n%d Timer_ signature(s) did not resolve; not writing." % len(ours_failed))
        return 1

    if not args.write:
        print("\n(dry run -- pass --write to update offsets_cache.ini and sigs.h)")
        return 0

    chat.update_offsets_cache(game / "offsets_cache.ini",
                              "%08X" % pe.timestamp,
                              {k: v for k, v in resolved.items() if k.startswith(chat.PREFIX)})
    SRC.mkdir(parents=True, exist_ok=True)
    (SRC / "sigs.h").write_text(generate_header(pe, sigs, resolved))
    print("\nwrote %s" % (SRC / "sigs.h"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
