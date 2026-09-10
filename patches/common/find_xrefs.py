"""Find instructions that reference an absolute address, for signature anchoring.

A global only gets a signature by way of some instruction that names it, so the
first job is finding a site whose surrounding bytes are distinctive. This scans
.text for the little-endian dword, then recovers the instruction boundary by
trying each start offset in a short window before the hit and keeping the decode
that actually covers it.

    python patches/common/find_xrefs.py --game-root <dir> --va 0x743B30
    python patches/common/find_xrefs.py --game-root <dir> --va 0x743B30 --context 3
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

try:
    import capstone
    import pefile
except ImportError:  # pragma: no cover
    sys.exit("needs pefile and capstone: pip install pefile capstone")


def text_span_va(pe):
    base = pe.OPTIONAL_HEADER.ImageBase
    for s in pe.sections:
        if s.Name.rstrip(b"\x00") == b".text":
            size = s.Misc_VirtualSize or s.SizeOfRawData
            return base + s.VirtualAddress, base + s.VirtualAddress + size
    raise SystemExit("no .text section")


def owning_instruction(md, data, base, hit_va):
    """The instruction whose encoding covers hit_va, or None.

    x86 is not self-synchronising, so this brute-forces the start: the correct
    boundary is the earliest one whose decode spans the dword, and a wrong guess
    almost always decodes to something that stops short.
    """
    for back in range(1, 9):
        start = hit_va - back
        window = bytes(data[start - base : start - base + 16])
        try:
            insn = next(md.disasm(window, start), None)
        except capstone.CsError:
            continue
        if insn and insn.address + insn.size >= hit_va + 4 and insn.size >= back + 4:
            return insn
    return None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--game-root", required=True, type=Path)
    ap.add_argument("--va", required=True, type=lambda s: int(s, 0))
    ap.add_argument("--limit", type=int, default=12)
    ap.add_argument("--context", type=int, default=0,
                    help="also show N instructions after each site")
    args = ap.parse_args()

    pe = pefile.PE(str(args.game_root / "main.exe"), fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    data = pe.get_memory_mapped_image()
    lo, hi = text_span_va(pe)

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)

    needle = args.va.to_bytes(4, "little")
    sites, pos = [], lo - base
    end = hi - base
    while True:
        i = data.find(needle, pos, end)
        if i < 0:
            break
        sites.append(i + base)
        pos = i + 1

    print(f"{args.va:#010x}: {len(sites)} reference(s) in .text")
    for hit_va in sites[: args.limit]:
        insn = owning_instruction(md, data, base, hit_va)
        if not insn:
            print(f"  {hit_va:#010x}  (no clean instruction boundary)")
            continue
        off = hit_va - insn.address
        print(f"  {insn.address:#010x}  +{off}  {insn.bytes.hex():<20} "
              f"{insn.mnemonic} {insn.op_str}")
        if args.context:
            window = bytes(data[insn.address - base : insn.address - base + 64])
            for k, nxt in enumerate(md.disasm(window, insn.address)):
                if k == 0:
                    continue
                if k > args.context:
                    break
                print(f"  {' ':>10}       {nxt.bytes.hex():<20} {nxt.mnemonic} {nxt.op_str}")
    if len(sites) > args.limit:
        print(f"  ... {len(sites) - args.limit} more")
    return 0


if __name__ == "__main__":
    sys.exit(main())
