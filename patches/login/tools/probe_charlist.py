"""Dump the character-select list from a live main.exe.

Reads [0x7430D8] (count) and [0x7430D4] (pointer to 0x1C records) out of a
running client so we can see nickname vs ID vs portrait bytes on a real
account. Safe to run on the title screen — it just prints count=0.

    python patches/login/tools/probe_charlist.py
    python patches/login/tools/probe_charlist.py --pid 1234
"""
from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wt
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "common"))
from paths import game_root  # noqa: E402
from peutil import load_pe  # noqa: E402

PROCESS_VM_READ = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400
TH32CS_SNAPPROCESS = 0x00000002

COUNT_RVA = 0x003430D8
LIST_RVA = 0x003430D4
STRIDE = 0x1C


class PROCESSENTRY32(ctypes.Structure):
    _fields_ = [
        ("dwSize", wt.DWORD),
        ("cntUsage", wt.DWORD),
        ("th32ProcessID", wt.DWORD),
        ("th32DefaultHeapID", ctypes.c_size_t),
        ("th32ModuleID", wt.DWORD),
        ("cntThreads", wt.DWORD),
        ("th32ParentProcessID", wt.DWORD),
        ("pcPriClassBase", ctypes.c_long),
        ("dwFlags", wt.DWORD),
        ("szExeFile", ctypes.c_char * wt.MAX_PATH),
    ]


k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
k32.Process32First.argtypes = [wt.HANDLE, ctypes.POINTER(PROCESSENTRY32)]
k32.Process32Next.argtypes = [wt.HANDLE, ctypes.POINTER(PROCESSENTRY32)]
k32.OpenProcess.restype = wt.HANDLE
k32.ReadProcessMemory.argtypes = [
    wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
    ctypes.POINTER(ctypes.c_size_t),
]
k32.ReadProcessMemory.restype = wt.BOOL


def pids_named(exe: bytes) -> list[int]:
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if snap == wt.HANDLE(-1).value:
        raise OSError("CreateToolhelp32Snapshot failed")
    pe = PROCESSENTRY32()
    pe.dwSize = ctypes.sizeof(pe)
    out: list[int] = []
    ok = k32.Process32First(snap, ctypes.byref(pe))
    while ok:
        if pe.szExeFile.lower() == exe.lower():
            out.append(int(pe.th32ProcessID))
        ok = k32.Process32Next(snap, ctypes.byref(pe))
    k32.CloseHandle(snap)
    return out


def rpm(handle, addr: int, size: int) -> bytes:
    buf = (ctypes.c_ubyte * size)()
    n = ctypes.c_size_t(0)
    if not k32.ReadProcessMemory(handle, ctypes.c_void_p(addr), buf, size, ctypes.byref(n)):
        raise OSError("ReadProcessMemory failed at 0x%X (%d)" % (addr, ctypes.get_last_error()))
    return bytes(buf[: n.value])


def ascii_run(blob: bytes) -> str:
    chars = []
    for b in blob:
        chars.append(chr(b) if 32 <= b < 127 else ".")
    return "".join(chars)


def dump_pid(pid: int, image_base: int) -> int:
    h = k32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)
    if not h:
        print("  pid %d: OpenProcess failed (%d)" % (pid, ctypes.get_last_error()))
        return 1
    try:
        count_va = image_base + COUNT_RVA
        list_va = image_base + LIST_RVA
        count = struct.unpack_from("<i", rpm(h, count_va, 4))[0]
        ptr = struct.unpack_from("<I", rpm(h, list_va, 4))[0]
        print("pid %d" % pid)
        print("  count  [%08X] = %d" % (count_va, count))
        print("  list   [%08X] = 0x%08X" % (list_va, ptr))
        if count <= 0 or ptr == 0:
            print("  (empty — not on the character-select screen)")
            return 0
        if count > 8:
            print("  refusing to dump count=%d (expected 1..5)" % count)
            return 1
        blob = rpm(h, ptr, count * STRIDE)
        for i in range(count):
            rec = blob[i * STRIDE : (i + 1) * STRIDE]
            id0, unk4, id8 = struct.unpack_from("<III", rec, 0)
            b13, b14 = rec[0x13], rec[0x14]
            print("  slot %d  +00=%u  +04=%u  +08=%u (display ID)  +13=%d +14=%d" % (
                i + 1, id0, unk4, id8, b13, b14))
            print("          hex %s" % rec.hex())
            print("          asc %s" % ascii_run(rec))
        return 0
    finally:
        k32.CloseHandle(h)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--pid", type=int, default=0)
    args = ap.parse_args()
    pe = load_pe(game_root() / "main.exe")
    pids = [args.pid] if args.pid else pids_named(b"main.exe")
    if not pids:
        print("no main.exe process. Start a client and run this on the character-select screen.")
        print("static layout is in patches/login/NOTES.txt")
        return 0
    rc = 0
    for pid in pids:
        rc |= dump_pid(pid, pe.image_base)
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
