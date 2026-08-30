"""Minimal PE32 helpers for MythWar main.exe analysis/patching."""
from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path


@dataclass
class Section:
    name: str
    vaddr: int
    vsize: int
    raw: int
    rsize: int


@dataclass
class PEImage:
    path: Path
    data: bytearray
    image_base: int
    timestamp: int
    sections: list[Section]
    imports: dict[str, dict[str, int]]  # dll -> func -> IAT VA


def load_pe(path: Path) -> PEImage:
    data = bytearray(Path(path).read_bytes())
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    timestamp = struct.unpack_from("<I", data, pe + 8)[0]
    opt = struct.unpack_from("<H", data, pe + 20)[0]
    magic = struct.unpack_from("<H", data, pe + 24)[0]
    if magic != 0x10B:
        raise ValueError("not PE32")
    image_base = struct.unpack_from("<I", data, pe + 24 + 28)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    sec0 = pe + 24 + opt
    sections: list[Section] = []
    for i in range(nsec):
        o = sec0 + i * 40
        name = data[o : o + 8].split(b"\0", 1)[0].decode("ascii", "replace")
        vsize, vaddr, rsize, raw = struct.unpack_from("<IIII", data, o + 8)
        sections.append(Section(name, vaddr, vsize, raw, rsize))
    peimg = PEImage(Path(path), data, image_base, timestamp, sections, {})
    peimg.imports = _parse_imports(peimg, pe)
    return peimg


def _parse_imports(pe: PEImage, pe_off: int) -> dict[str, dict[str, int]]:
    import_rva = struct.unpack_from("<I", pe.data, pe_off + 24 + 96 + 8)[0]
    off = rva_to_off(pe, import_rva)
    result: dict[str, dict[str, int]] = {}
    while True:
        ilt, _, _, name_rva, iat = struct.unpack_from("<IIIII", pe.data, off)
        if ilt == 0 and name_rva == 0:
            break
        dll = read_cstr(pe.data, rva_to_off(pe, name_rva)).lower()
        thunk_rva = ilt or iat
        t = rva_to_off(pe, thunk_rva)
        iat_va = pe.image_base + iat
        funcs: dict[str, int] = {}
        idx = 0
        while True:
            entry = struct.unpack_from("<I", pe.data, t)[0]
            if entry == 0:
                break
            if entry & 0x80000000:
                name = f"ord#{entry & 0xFFFF}"
            else:
                name = read_cstr(pe.data, rva_to_off(pe, entry) + 2)
            funcs[name] = iat_va + idx * 4
            t += 4
            idx += 1
        result[dll] = funcs
        off += 20
    return result


def rva_to_off(pe: PEImage, rva: int) -> int:
    for s in pe.sections:
        span = max(s.vsize, s.rsize)
        if s.vaddr <= rva < s.vaddr + span:
            return rva - s.vaddr + s.raw
    raise ValueError(f"RVA 0x{rva:X} not in any section")


def va_to_off(pe: PEImage, va: int) -> int:
    return rva_to_off(pe, va - pe.image_base)


def off_to_va(pe: PEImage, off: int) -> int:
    for s in pe.sections:
        if s.raw <= off < s.raw + s.rsize:
            return pe.image_base + s.vaddr + (off - s.raw)
        # BSS / virtual tail of .data
        if s.raw <= off < s.raw + max(s.vsize, s.rsize) and off < s.raw + s.rsize:
            return pe.image_base + s.vaddr + (off - s.raw)
    # file offset == RVA for this unpacked MSVC image in mapped sections
    for s in pe.sections:
        if s.vaddr <= off < s.vaddr + max(s.vsize, s.rsize) and s.raw == s.vaddr:
            return pe.image_base + off
    raise ValueError(f"offset 0x{off:X} not in any section")


def read_cstr(data: bytes, off: int) -> str:
    end = data.find(b"\0", off)
    if end < 0:
        end = len(data)
    return data[off:end].decode("ascii", "replace")


def find_ascii(data: bytes, s: bytes) -> list[int]:
    hits = []
    start = 0
    while True:
        i = data.find(s, start)
        if i < 0:
            return hits
        hits.append(i)
        start = i + 1


def find_imm32(data: bytes, value: int, start: int = 0, end: int | None = None) -> list[int]:
    pat = struct.pack("<I", value)
    hits = []
    limit = len(data) if end is None else end
    i = start
    while True:
        j = data.find(pat, i, limit)
        if j < 0:
            return hits
        hits.append(j)
        i = j + 1


PUSH_W = bytes((0x68, 0x20, 0x03, 0x00, 0x00))  # push 800
PUSH_H = bytes((0x68, 0x58, 0x02, 0x00, 0x00))  # push 600
PUSH_W_NEW = bytes((0x68, 0x00, 0x04, 0x00, 0x00))  # push 1024
PUSH_H_NEW = bytes((0x68, 0x00, 0x03, 0x00, 0x00))  # push 768

OLD_W, OLD_H = 800, 600
NEW_W, NEW_H = 1024, 768
BUILD_TIMESTAMP = 0x45650696
EXPECTED_SHA256 = "285C3BE50304E3B70A817E5089477F3E18774C16357C8BB394CA6B308F8E034E"
