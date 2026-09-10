"""Rebuild the ChatMod_* / Login_* sections of signatures.ini from the
generated sigs.h headers.

signatures.ini is the documented source of truth for scan_offsets.py, but it
lives in the game folder and is not in version control, so it can go missing -
and did: it was found at 0 bytes, which made both scanners fail with
"no ChatMod_* sections in signatures.ini" and took the post-patch verification
step down with them.

Nothing is actually lost when that happens. scan_offsets.py generates
src/sigs.h from the ini, and every field it needs survives in that header:
pattern bytes, mask, length, opOffset and the optional flag. This walks it
back.

Two details that matter for a faithful rebuild:

  * opOffset is OMITTED when it is -1, never written as "-1". scan() keys off
    `"opOffset" in body`, so a literal -1 would make it read an address four
    bytes before the match instead of using the match itself.
  * Section order is preserved from the header's row order. generate_header
    iterates the ini in insertion order, so the order decides both the row
    order in sigs.h and the enum values in MW_ADDR_COUNT / LG_ADDR_COUNT.

Usage:
    python patches/common/rebuild_signatures.py --game-root <dir>          # preview
    python patches/common/rebuild_signatures.py --game-root <dir> --write
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

PROJ = Path(__file__).resolve().parents[2]

# (header, ini section prefix, whether rows carry a trailing `optional` field)
SOURCES = [
    (PROJ / "patches" / "chatmod" / "src" / "sigs.h", "ChatMod_", True),
    (PROJ / "patches" / "login" / "src" / "sigs.h", "Login_", False),
]

BLOB = re.compile(r"static const unsigned char (\w+)\[\] = \{([^}]*)\};")
ROW = re.compile(
    r'\{"(\w+)",\s*(\w+),\s*"([x?]+)",\s*(\d+),\s*(-?\d+),\s*0x([0-9A-Fa-f]+)U(?:,\s*(\d))?\s*\}'
)


def read_header(path: Path):
    """[(name, sig_string, op_offset, optional)] in declaration order."""
    text = path.read_text()
    blobs = {
        m.group(1): [int(b, 16) for b in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(2))]
        for m in BLOB.finditer(text)
    }
    body = re.split(r"(?:MW|LG)_SIGS\[\] = \{", text, maxsplit=1)[1].split("\n};", 1)[0]

    out = []
    for m in ROW.finditer(body):
        name, var, mask, length, op_offset, _rva, optional = m.groups()
        pattern = blobs[var]
        if not len(pattern) == len(mask) == int(length):
            raise SystemExit(
                f"{path.name}: {name} disagrees on length "
                f"(pattern {len(pattern)}, mask {len(mask)}, declared {length})"
            )
        # Wildcard bytes are stored as 0 in the array and must go back as "??".
        sig = " ".join("??" if c == "?" else "%02X" % b for b, c in zip(pattern, mask))
        out.append((name, sig, int(op_offset), int(optional or 0)))
    return out


def read_existing(path: Path) -> str:
    """Whatever is already in the ini, minus the sections we are rebuilding.

    Maps++ owns its own sections in this file and must survive untouched.
    """
    if not path.is_file() or path.stat().st_size == 0:
        return ""
    for encoding in ("utf-16", "utf-8-sig", "mbcs"):
        try:
            text = path.read_text(encoding=encoding)
            break
        except (UnicodeError, LookupError, OSError):
            continue
    else:
        return ""

    kept, dropping = [], False
    for raw in text.splitlines():
        line = raw.strip()
        if line.startswith("[") and line.endswith("]"):
            dropping = any(line[1:-1].startswith(p) for _, p, _ in SOURCES)
        if not dropping:
            kept.append(raw)
    return "\n".join(kept).strip()


def build(existing: str) -> str:
    chunks = [existing] if existing else []
    for header, prefix, has_optional in SOURCES:
        rows = read_header(header)
        chunks.append(
            f"; {prefix}* rebuilt from {header.relative_to(PROJ).as_posix()}\n"
            f"; by patches/common/rebuild_signatures.py - do not hand-edit the order,\n"
            f"; it decides the row order and enum values in the generated header."
        )
        for name, sig, op_offset, optional in rows:
            section = [f"[{prefix}{name}]", f"sig={sig}"]
            if op_offset >= 0:
                section.append(f"opOffset={op_offset}")
            if has_optional and optional:
                section.append("optional=1")
            chunks.append("\n".join(section))
    return "\n\n".join(chunks) + "\n"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--game-root", required=True, type=Path,
                    help="the installed client folder (must contain main.exe)")
    ap.add_argument("--write", action="store_true",
                    help="write signatures.ini; without this, preview only")
    args = ap.parse_args()

    game = args.game_root
    if not (game / "main.exe").is_file():
        print(f"no main.exe in {game}", file=sys.stderr)
        return 2

    ini = game / "signatures.ini"
    existing = read_existing(ini)
    text = build(existing)

    counts = {prefix: len(read_header(h)) for h, prefix, _ in SOURCES}
    kept = sum(1 for line in existing.splitlines() if line.strip().startswith("["))
    print(f"{ini}")
    print(f"  kept   {kept} existing section(s) (Maps++ and anything else)")
    for prefix, n in counts.items():
        print(f"  rebuilt {n:>2} {prefix}* section(s)")

    if not args.write:
        print("\npreview only; pass --write to apply")
        print("-" * 60)
        print("\n".join(text.splitlines()[:24]))
        return 0

    # UTF-16 LE with BOM, matching what read_signatures expects and what the
    # AutoHotkey side writes.
    ini.write_text(text, encoding="utf-16")
    print(f"\nwrote {ini} ({ini.stat().st_size} bytes)")
    print("Now round-trip it: run both scan_offsets.py and confirm every")
    print("signature resolves exactly once, then --write and diff sigs.h.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
