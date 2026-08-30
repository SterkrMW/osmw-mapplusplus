"""Resolve Login_* signatures against main.exe. Same contract as chatmod's scanner."""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "common"))
from paths import PROJ, add_game_root_arg, game_root  # noqa: E402
sys.path.insert(0, str(PROJ / "patches" / "chatmod" / "tools"))
import scan_offsets as chat  # noqa: E402

chat.PREFIX = "Login_"
chat.HEADER_TEMPLATE = chat.HEADER_TEMPLATE.replace(
    "patches/chatmod/tools/scan_offsets.py",
    "patches/login/tools/scan_offsets.py",
).replace("ChatMod_*", "Login_*").replace("mw_sig_t", "lg_sig_t").replace(
    "MW_SIGS", "LG_SIGS").replace("MW_SIG_COUNT", "LG_SIG_COUNT").replace(
    "MW_ADDR_COUNT", "LG_ADDR_COUNT").replace("    MW_%s,", "    LG_%s,")


def generate_header(pe, sigs, resolved) -> str:
    blobs, rows, enums = [], [], []
    for name, body in sigs.items():
        if not name.startswith(chat.PREFIX) or "sig" not in body:
            continue
        raw, mask = chat.parse_pattern(body["sig"])
        short = name[len(chat.PREFIX):]
        var = "sig_" + short.lower()
        blobs.append("static const unsigned char %s[] = {%s};"
                     % (var, ", ".join("0x%02X" % b for b in raw)))
        rows.append('    {"%s", %s, "%s", %d, %d, 0x%08XU},'
                    % (short, var, "".join("x" if m else "?" for m in mask),
                       len(raw), int(body.get("opOffset", -1)), resolved.get(name, 0)))
        enums.append("    LG_%s," % short.upper())
    return chat.HEADER_TEMPLATE.format(
        ts=pe.timestamp,
        blobs="\n".join(blobs),
        rows="\n".join(rows) + "\n",
        enums="\n".join(enums) + "\n",
    )


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true")
    add_game_root_arg(ap)
    args = ap.parse_args()

    from peutil import load_pe  # noqa: E402

    game = game_root(getattr(args, "game_root", None))
    pe = load_pe(game / "main.exe")
    sigs = chat.read_signatures(game / "signatures.ini")
    ours = {k: v for k, v in sigs.items() if k.startswith(chat.PREFIX)}
    if not ours:
        print("no Login_* sections in signatures.ini")
        return 1

    resolved, problems = chat.scan(pe, sigs)
    print("main.exe build 0x%08X  (%d Login_ signatures)  rva / va"
          % (pe.timestamp, len(ours)))
    for name in ours:
        if name in resolved:
            print("  OK   %-28s 0x%08X / 0x%08X"
                  % (name, resolved[name], resolved[name] + pe.image_base))
        else:
            print("  FAIL %-28s" % name)
    for p in problems:
        print("  !! %s" % p)
    if problems:
        return 1

    if args.write:
        chat.update_offsets_cache(game / "offsets_cache.ini",
                                  "%08X" % pe.timestamp, resolved)
        header = PROJ / "patches" / "login" / "src" / "sigs.h"
        header.write_text(generate_header(pe, ours, resolved), encoding="utf-8")
        print("\nwrote offsets_cache.ini and %s" % header.relative_to(PROJ))
    else:
        print("\n(dry run -- pass --write to update offsets_cache.ini and sigs.h)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
