"""Generate a signatures.ini pattern for a VA and prove it is unique.

scan.c requires exactly one match per signature across .text, so a pattern is
only useful once it has been counted. This disassembles forward from a target,
wildcards the bytes that are allowed to move between builds, checks the match
count, and prints the ini section.

What gets wildcarded, and why:

  * rel8 / rel32 branch displacements - they shift whenever anything before the
    target moves.
  * imm32 and disp32 operands that land inside the image (0x400000-0x900000) -
    these are absolute addresses of globals and functions, the things a rebuild
    relocates.

Everything else - opcodes, register encodings, small immediates like the
`sub esp, 0x30` that separates the minimap draw proc from the input proc - is
fixed and is what gives the pattern its identity.

For a global rather than a function, pass --extract <offset-in-pattern>. That
dword is wildcarded and recorded as opOffset, so the scanner reads the live
address out of the instruction instead of assuming the old one. This mirrors
how ChatMod_Object and friends are shaped.

Usage:
    python patches/common/make_signature.py --game-root <dir> \
        --name Minimap_DrawProc --va 0x43ECC0 --len 12
    python patches/common/make_signature.py --game-root <dir> \
        --name Minimap_MapCharBase --va 0x46DD10 --len 14 --extract 2
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

try:
    import capstone
    import pefile
except ImportError:  # pragma: no cover
    sys.exit("needs pefile and capstone: pip install pefile capstone")

IMAGE_LO = 0x400000
IMAGE_HI = 0x900000


def text_span(pe):
    for s in pe.sections:
        if s.Name.rstrip(b"\x00") == b".text":
            size = s.Misc_VirtualSize or s.SizeOfRawData
            return s.PointerToRawData, s.PointerToRawData + min(size, s.SizeOfRawData)
    raise SystemExit("no .text section")


def volatile_ranges(insn, start):
    """Byte ranges within the instruction that may move between builds.

    Offsets are relative to `start` (the pattern's first byte).
    """
    out = []
    off = insn.address - start

    # Branch displacements: the operand is the tail of the instruction.
    if capstone.CS_GRP_JUMP in insn.groups or capstone.CS_GRP_CALL in insn.groups:
        size = insn.size
        # rel8 forms are 2 bytes (or 3 for 0x0F-prefixed), rel32 are 5 or 6.
        disp = 4 if size >= 5 else 1
        out.append((off + size - disp, disp))
        return out

    # Absolute addresses embedded as imm32 or disp32.
    #
    # Taken from capstone's encoding info rather than by scanning every 4-byte
    # window, because that scan produces false positives that silently gut a
    # pattern: in `and eax, 0x8000` (25 00 80 00 00) the window starting at the
    # OPCODE reads as 0x00800025, which is inside the image range, so the whole
    # instruction got wildcarded -- and that immediate was the only thing
    # telling three otherwise identical cell-flag queries apart.
    enc = getattr(insn, "encoding", None)
    spans = []
    if enc is not None:
        if getattr(enc, "disp_size", 0):
            spans.append((enc.disp_offset, enc.disp_size))
        if getattr(enc, "imm_size", 0):
            spans.append((enc.imm_offset, enc.imm_size))

    if not spans:
        # Older capstone bindings expose no encoding offsets. Fall back to the
        # window scan, but only where an operand can actually start: past the
        # opcode, and 4-byte aligned to the instruction's end.
        raw = insn.bytes
        for i in range(1, len(raw) - 3):
            if (len(raw) - i) % 4 == 0:
                spans.append((i, 4))

    for pos, size in spans:
        if size != 4:
            continue
        value = int.from_bytes(insn.bytes[pos : pos + 4], "little")
        if IMAGE_LO <= value < IMAGE_HI:
            out.append((off + pos, 4))
    return out


def build(pe, va, length, extract):
    base = pe.OPTIONAL_HEADER.ImageBase
    data = pe.get_memory_mapped_image()
    raw = data[va - base : va - base + length]

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True

    # Disassemble past the end of the pattern. capstone refuses to decode an
    # instruction the buffer truncates, so feeding it exactly `length` bytes
    # silently leaves a trailing partial instruction unmasked - which is how a
    # half of a rel32 displacement ends up as fixed bytes in the pattern.
    window = bytes(data[va - base : va - base + length + 16])

    mask = ["x"] * length
    listing = []
    for insn in md.disasm(window, va):
        if insn.address - va >= length:
            break
        listing.append("  %#010x  %-20s %s %s"
                       % (insn.address, insn.bytes.hex(), insn.mnemonic, insn.op_str))
        for off, n in volatile_ranges(insn, va):
            for k in range(off, min(off + n, length)):
                if k >= 0:
                    mask[k] = "?"

    if extract is not None:
        for k in range(extract, min(extract + 4, length)):
            mask[k] = "?"

    sig = " ".join("??" if mask[i] == "?" else "%02X" % raw[i] for i in range(length))
    return sig, "".join(mask), raw, listing


def count_matches(pe, raw, mask):
    data = pe.__data__[:]
    lo, hi = text_span(pe)
    fixed = [i for i, c in enumerate(mask) if c == "x"]
    if not fixed:
        raise SystemExit("pattern is entirely wildcards")
    anchor = fixed[0]
    needle = bytes([raw[anchor]])
    hits, pos = [], lo + anchor
    end = hi - len(raw) + anchor
    while True:
        i = data.find(needle, pos, end)
        if i < 0:
            return hits
        start = i - anchor
        if start >= lo and all(data[start + k] == raw[k] for k in fixed):
            hits.append(start)
        pos = i + 1


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--game-root", required=True, type=Path)
    ap.add_argument("--name", required=True, help="ini section name, without the prefix")
    ap.add_argument("--prefix", default="Minimap_")
    ap.add_argument("--va", required=True, type=lambda s: int(s, 0))
    ap.add_argument("--len", dest="length", type=int, default=12)
    ap.add_argument("--extract", type=int, default=None,
                    help="offset in the pattern of a dword to read as the value (opOffset)")
    ap.add_argument("--grow", action="store_true",
                    help="on a multi-match, extend the pattern until it is unique")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    pe = pefile.PE(str(args.game_root / "main.exe"), fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    lo, _ = text_span(pe)

    length = args.length
    while True:
        sig, mask, raw, listing = build(pe, args.va, length, args.extract)
        hits = count_matches(pe, raw, mask)
        if len(hits) == 1 or not args.grow or length >= 64:
            break
        length += 2

    if not args.quiet:
        print(f"=== {args.prefix}{args.name}  {args.va:#x}  ({length} bytes) ===")
        print("\n".join(listing))
        print()

    rva_of = lambda off: off - lo + 0x1000
    status = "OK" if len(hits) == 1 else "NOT UNIQUE"
    print(f"  {status}: {len(hits)} match(es)"
          + (f" at {base + rva_of(hits[0]):#010x}" if len(hits) == 1 else ""))
    if len(hits) != 1:
        for h in hits[:6]:
            print(f"    {base + rva_of(h):#010x}")
        return 1

    print()
    print(f"[{args.prefix}{args.name}]")
    print(f"sig={sig}")
    if args.extract is not None:
        print(f"opOffset={args.extract}")
        value = int.from_bytes(raw[args.extract : args.extract + 4], "little")
        print(f"; resolves to {value:#010x}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
