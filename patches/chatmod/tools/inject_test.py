"""Inject or eject osmw_chat.dll for testing, without going through Maps++.

    python patches/chatmod/tools/inject_test.py            # inject into main.exe
    python patches/chatmod/tools/inject_test.py --eject
    python patches/chatmod/tools/inject_test.py --status
    python patches/chatmod/tools/inject_test.py --pid 1234

The client is 32-bit and this script normally runs under 64-bit Python, so
LoadLibraryA cannot be resolved locally -- we find the target's own 32-bit
kernel32 and parse its export table over ReadProcessMemory.
"""
from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wt
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "common"))
from paths import game_root  # noqa: E402

# Everything here acts on the installed client: the deployed DLL is the one a
# launcher would load, and the logs land beside main.exe.
ROOT = game_root()
DLL = ROOT / "osmw_chat.dll"
TARGET_EXE = "main.exe"


def log_for(pid: int) -> Path:
    """The DLL writes one log per client so multiple clients do not collide."""
    return ROOT / ("osmw_chat.%d.log" % pid)

k32 = ctypes.WinDLL("kernel32", use_last_error=True)

# Declare every prototype we use. Without this, ctypes defaults to a 32-bit int
# return and silently truncates handles and remote pointers under 64-bit Python.
k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
k32.CreateToolhelp32Snapshot.argtypes = [wt.DWORD, wt.DWORD]
k32.CloseHandle.restype = wt.BOOL
k32.CloseHandle.argtypes = [wt.HANDLE]
k32.OpenProcess.restype = wt.HANDLE
k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.VirtualAllocEx.restype = ctypes.c_void_p
k32.VirtualAllocEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_size_t, wt.DWORD, wt.DWORD]
k32.VirtualFreeEx.restype = wt.BOOL
k32.VirtualFreeEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_size_t, wt.DWORD]
k32.WriteProcessMemory.restype = wt.BOOL
k32.WriteProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_char_p,
                                   ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k32.ReadProcessMemory.restype = wt.BOOL
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                  ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k32.CreateRemoteThread.restype = wt.HANDLE
k32.CreateRemoteThread.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_size_t,
                                   ctypes.c_void_p, ctypes.c_void_p, wt.DWORD,
                                   ctypes.POINTER(wt.DWORD)]
k32.WaitForSingleObject.restype = wt.DWORD
k32.WaitForSingleObject.argtypes = [wt.HANDLE, wt.DWORD]
k32.GetExitCodeThread.restype = wt.BOOL
k32.GetExitCodeThread.argtypes = [wt.HANDLE, ctypes.POINTER(wt.DWORD)]

INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
PROCESS_ALL_ACCESS = 0x1F0FFF
MEM_COMMIT_RESERVE = 0x3000
MEM_RELEASE = 0x8000
PAGE_READWRITE = 0x04
TH32CS_SNAPPROCESS = 0x02
TH32CS_SNAPMODULE = 0x08
TH32CS_SNAPMODULE32 = 0x10
MAX_PATH = 260


class PROCESSENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ProcessID", wt.DWORD),
                ("th32DefaultHeapID", ctypes.POINTER(ctypes.c_ulong)),
                ("th32ModuleID", wt.DWORD), ("cntThreads", wt.DWORD),
                ("th32ParentProcessID", wt.DWORD), ("pcPriClassBase", ctypes.c_long),
                ("dwFlags", wt.DWORD), ("szExeFile", ctypes.c_char * MAX_PATH)]


class MODULEENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("th32ModuleID", wt.DWORD), ("th32ProcessID", wt.DWORD),
                ("GlblcntUsage", wt.DWORD), ("ProccntUsage", wt.DWORD),
                ("modBaseAddr", ctypes.POINTER(ctypes.c_byte)), ("modBaseSize", wt.DWORD),
                ("hModule", wt.HMODULE), ("szModule", ctypes.c_char * 256),
                ("szExePath", ctypes.c_char * MAX_PATH)]


def _snapshot(flags: int, pid: int):
    snap = k32.CreateToolhelp32Snapshot(flags, pid)
    if not snap or snap == INVALID_HANDLE_VALUE:
        raise OSError("CreateToolhelp32Snapshot failed: %d" % ctypes.get_last_error())
    return snap


def find_pids(name: str) -> list[int]:
    snap = _snapshot(TH32CS_SNAPPROCESS, 0)
    try:
        entry = PROCESSENTRY32()
        entry.dwSize = ctypes.sizeof(entry)
        pids = []
        ok = k32.Process32First(snap, ctypes.byref(entry))
        while ok:
            if entry.szExeFile.decode("mbcs", "replace").lower() == name.lower():
                pids.append(entry.th32ProcessID)
            ok = k32.Process32Next(snap, ctypes.byref(entry))
        return pids
    finally:
        k32.CloseHandle(snap)


def modules(pid: int) -> list[tuple[str, int, str]]:
    snap = _snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)
    try:
        entry = MODULEENTRY32()
        entry.dwSize = ctypes.sizeof(entry)
        out = []
        ok = k32.Module32First(snap, ctypes.byref(entry))
        while ok:
            out.append((entry.szModule.decode("mbcs", "replace"),
                        ctypes.cast(entry.modBaseAddr, ctypes.c_void_p).value or 0,
                        entry.szExePath.decode("mbcs", "replace")))
            ok = k32.Module32Next(snap, ctypes.byref(entry))
        return out
    finally:
        k32.CloseHandle(snap)


def read_mem(proc: int, addr: int, size: int) -> bytes:
    buf = (ctypes.c_char * size)()
    got = ctypes.c_size_t(0)
    if not k32.ReadProcessMemory(proc, ctypes.c_void_p(addr), ctypes.byref(buf),
                                 size, ctypes.byref(got)):
        raise OSError("ReadProcessMemory at 0x%X failed: %d" % (addr, ctypes.get_last_error()))
    return bytes(buf[:got.value])


def remote_export(proc: int, mod_base: int, want: bytes) -> int:
    """Resolve an export in the *target's* copy of a module."""
    hdr = read_mem(proc, mod_base, 0x400)
    pe = struct.unpack_from("<I", hdr, 0x3C)[0]
    magic = struct.unpack_from("<H", hdr, pe + 24)[0]
    dir_off = pe + 24 + (96 if magic == 0x10B else 112)
    exp_rva, exp_size = struct.unpack_from("<II", hdr, dir_off)
    if not exp_rva:
        raise OSError("module at 0x%X has no export directory" % mod_base)

    # IMAGE_EXPORT_DIRECTORY: +24 NumberOfNames, +28 AddressOfFunctions,
    # +32 AddressOfNames, +36 AddressOfNameOrdinals
    exp = read_mem(proc, mod_base + exp_rva, max(exp_size, 40))
    n_names, funcs_rva, names_rva, ords_rva = struct.unpack_from("<IIII", exp, 24)

    names = struct.unpack_from("<%dI" % n_names, read_mem(proc, mod_base + names_rva, n_names * 4), 0)
    ordinals = struct.unpack_from("<%dH" % n_names, read_mem(proc, mod_base + ords_rva, n_names * 2), 0)

    for i, nrva in enumerate(names):
        raw = read_mem(proc, mod_base + nrva, len(want) + 1)
        if raw[:len(want)] == want and raw[len(want):len(want) + 1] == b"\0":
            frva = struct.unpack_from("<I", read_mem(proc, mod_base + funcs_rva + ordinals[i] * 4, 4))[0]
            return mod_base + frva
    raise OSError("export %s not found" % want.decode())


def call_remote(proc: int, fn: int, arg: int, what: str) -> int:
    tid = wt.DWORD(0)
    thread = k32.CreateRemoteThread(proc, None, 0, ctypes.c_void_p(fn),
                                    ctypes.c_void_p(arg), 0, ctypes.byref(tid))
    if not thread:
        raise OSError("CreateRemoteThread(%s) failed: %d" % (what, ctypes.get_last_error()))
    try:
        if k32.WaitForSingleObject(thread, 15000) != 0:
            raise OSError("%s timed out in the target" % what)
        code = wt.DWORD(0)
        k32.GetExitCodeThread(thread, ctypes.byref(code))
        return code.value
    finally:
        k32.CloseHandle(thread)


def open_target(pid: int) -> int:
    proc = k32.OpenProcess(PROCESS_ALL_ACCESS, False, pid)
    if not proc:
        raise SystemExit("OpenProcess(%d) failed: %d (run as the same user)"
                         % (pid, ctypes.get_last_error()))
    return proc


def loaded_module(pid: int) -> tuple[str, int, str] | None:
    for m in modules(pid):
        if m[0].lower() == DLL.name.lower():
            return m
    return None


class STARTUPINFOA(ctypes.Structure):
    _fields_ = [("cb", wt.DWORD), ("lpReserved", ctypes.c_char_p),
                ("lpDesktop", ctypes.c_char_p), ("lpTitle", ctypes.c_char_p),
                ("dwX", wt.DWORD), ("dwY", wt.DWORD), ("dwXSize", wt.DWORD),
                ("dwYSize", wt.DWORD), ("dwXCountChars", wt.DWORD),
                ("dwYCountChars", wt.DWORD), ("dwFillAttribute", wt.DWORD),
                ("dwFlags", wt.DWORD), ("wShowWindow", wt.WORD),
                ("cbReserved2", wt.WORD), ("lpReserved2", ctypes.POINTER(ctypes.c_byte)),
                ("hStdInput", wt.HANDLE), ("hStdOutput", wt.HANDLE),
                ("hStdError", wt.HANDLE)]


class PROCESS_INFORMATION(ctypes.Structure):
    _fields_ = [("hProcess", wt.HANDLE), ("hThread", wt.HANDLE),
                ("dwProcessId", wt.DWORD), ("dwThreadId", wt.DWORD)]


CREATE_SUSPENDED = 0x00000004


def do_launch() -> int:
    """Start the client suspended, inject, then let it run.

    The chat window (and its two fonts) is built during startup init, so a hook
    that lands after the client is already up has nothing left to intercept.
    This is the same sequence Maps++ will use to load the mod for real.
    """
    exe = ROOT / "main.exe"
    if not exe.exists():
        raise SystemExit("%s not found" % exe)
    if not DLL.exists():
        raise SystemExit("%s not built -- run build.py first" % DLL)

    k32.CreateProcessA.restype = wt.BOOL
    k32.ResumeThread.restype = wt.DWORD
    k32.ResumeThread.argtypes = [wt.HANDLE]

    si = STARTUPINFOA()
    si.cb = ctypes.sizeof(si)
    pi = PROCESS_INFORMATION()

    if not k32.CreateProcessA(str(exe).encode("mbcs"), None, None, None, False,
                              CREATE_SUSPENDED, None, str(ROOT).encode("mbcs"),
                              ctypes.byref(si), ctypes.byref(pi)):
        raise SystemExit("CreateProcess failed: %d" % ctypes.get_last_error())

    pid = pi.dwProcessId
    print("launched %s suspended, pid %d" % (exe.name, pid))

    try:
        # A suspended process has no module list yet -- the loader only builds
        # it once the main thread runs -- so we cannot resolve LoadLibraryA
        # before resuming. Resume, then inject the moment kernel32 appears.
        # That is still far ahead of the client reaching its chat-window init,
        # which happens only after it has loaded its art and set up DirectDraw.
        k32.ResumeThread(pi.hThread)

        deadline = time.time() + 10.0
        k32_base = 0
        while time.time() < deadline:
            try:
                k32_base = next((b for n, b, _ in modules(pid)
                                 if n.lower() == "kernel32.dll"), 0)
            except OSError:
                k32_base = 0
            if k32_base:
                break
            time.sleep(0.001)
        if not k32_base:
            raise RuntimeError("kernel32.dll never appeared in pid %d" % pid)
        print("  loader up after %.0fms, kernel32 0x%08X"
              % ((10.0 - (deadline - time.time())) * 1000, k32_base))

        load_library = remote_export(pi.hProcess, k32_base, b"LoadLibraryA")

        path = str(DLL).encode("mbcs") + b"\0"
        addr = k32.VirtualAllocEx(pi.hProcess, None, len(path),
                                  MEM_COMMIT_RESERVE, PAGE_READWRITE)
        if not addr:
            raise RuntimeError("VirtualAllocEx failed: %d" % ctypes.get_last_error())
        try:
            written = ctypes.c_size_t(0)
            if not k32.WriteProcessMemory(pi.hProcess, ctypes.c_void_p(addr), path,
                                          len(path), ctypes.byref(written)):
                raise RuntimeError("WriteProcessMemory failed: %d" % ctypes.get_last_error())
            call_remote(pi.hProcess, load_library, addr, "LoadLibraryA")
        finally:
            k32.VirtualFreeEx(pi.hProcess, ctypes.c_void_p(addr), 0, MEM_RELEASE)

        if not loaded_module(pid):
            raise RuntimeError("LoadLibraryA ran but the module is not present")
    except BaseException:
        k32.TerminateProcess(pi.hProcess, 1)
        k32.CloseHandle(pi.hThread)
        k32.CloseHandle(pi.hProcess)
        print("  launch failed; the new client was terminated")
        raise

    k32.CloseHandle(pi.hThread)
    k32.CloseHandle(pi.hProcess)
    print("  injected, pid %d is starting" % pid)
    print("  log: %s" % log_for(pid).name)
    return 0


u32 = ctypes.WinDLL("user32", use_last_error=True)
u32.FindWindowExA.restype = wt.HWND
u32.FindWindowExA.argtypes = [wt.HWND, wt.HWND, ctypes.c_char_p, ctypes.c_char_p]
u32.GetWindowThreadProcessId.restype = wt.DWORD
u32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
u32.RegisterWindowMessageA.restype = wt.UINT
u32.RegisterWindowMessageA.argtypes = [ctypes.c_char_p]
u32.PostMessageA.restype = wt.BOOL
u32.PostMessageA.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
u32.GetWindowTextA.restype = ctypes.c_int
u32.GetWindowTextA.argtypes = [wt.HWND, ctypes.c_char_p, ctypes.c_int]

CHAT_CLASS = b"WindowedMode"


def find_chat_window(pid: int):
    """The chat window is the only one using this class -- the main game window
    registers ##RAYSDDWNDCLASS20010705##."""
    hwnd = None
    while True:
        hwnd = u32.FindWindowExA(None, hwnd, CHAT_CLASS, None)
        if not hwnd:
            return None
        owner = wt.DWORD(0)
        u32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid:
            return hwnd


def do_clear(pid: int) -> int:
    """Drive the mod's clear the same way an external tool would."""
    hwnd = find_chat_window(pid)
    if not hwnd:
        print("  no chat window found for pid %d" % pid)
        return 1
    msg = u32.RegisterWindowMessageA(b"osmw_chat_clear")
    if not msg:
        print("  RegisterWindowMessage failed: %d" % ctypes.get_last_error())
        return 1
    if not u32.PostMessageA(hwnd, msg, 0, 0):
        print("  PostMessage failed: %d" % ctypes.get_last_error())
        return 1
    print("  posted osmw_chat_clear (0x%04X) to chat window 0x%X" % (msg, hwnd))
    return 0


def do_post(pid: int, message: bytes, wparam: int, label: str) -> int:
    """Post one of the mod's registered window messages to the chat window."""
    hwnd = find_chat_window(pid)
    if not hwnd:
        print("  no chat window found for pid %d" % pid)
        return 1
    msg = u32.RegisterWindowMessageA(message)
    if not msg or not u32.PostMessageA(hwnd, msg, wparam, 0):
        print("  post failed: %d" % ctypes.get_last_error())
        return 1
    print("  posted %s (0x%04X) -- %s" % (message.decode(), msg, label))
    return 0


def do_tab(pid: int, which: str) -> int:
    """Switch the channel filter the way an external tool would.

    Tabs are configurable via [Chat] Tabs, so a bare number selects the Nth
    tab; the names are shorthand for the shipped default set."""
    names = {"cycle": 0, "all": 1, "chat": 2, "system": 3}
    if which.isdigit():
        names = {which: int(which)}
    elif which.lower() not in names:
        print("  --tab takes a tab number, or one of: %s" % ", ".join(names))
        return 1
    hwnd = find_chat_window(pid)
    if not hwnd:
        print("  no chat window found for pid %d" % pid)
        return 1
    msg = u32.RegisterWindowMessageA(b"osmw_chat_tab")
    if not msg or not u32.PostMessageA(hwnd, msg, names[which.lower()], 0):
        print("  post failed: %d" % ctypes.get_last_error())
        return 1
    time.sleep(0.4)
    buf = ctypes.create_string_buffer(256)
    u32.GetWindowTextA(hwnd, buf, 256)
    print("  posted osmw_chat_tab wParam=%d; caption is now %r"
          % (names[which.lower()], buf.value.decode("mbcs", "replace")))
    return 0


def do_status(pid: int) -> int:
    m = loaded_module(pid)
    if m:
        print("  loaded: %s at 0x%08X  (%s)" % (m[0], m[1], m[2]))
    else:
        print("  not loaded")
    log = log_for(pid)
    if log.exists():
        print("  log: %s (%d bytes, mtime %s)"
              % (log.name, log.stat().st_size,
                 time.strftime("%H:%M:%S", time.localtime(log.stat().st_mtime))))
    else:
        print("  log: %s (not written yet)" % log.name)
    return 0


def do_inject(pid: int) -> int:
    if not DLL.exists():
        raise SystemExit("%s not built -- run patches/chatmod/tools/build.py first" % DLL)
    if loaded_module(pid):
        print("  already loaded; eject first")
        return 1

    proc = open_target(pid)
    try:
        k32_base = next((b for n, b, _ in modules(pid) if n.lower() == "kernel32.dll"), 0)
        if not k32_base:
            raise SystemExit("kernel32.dll not found in target %d" % pid)
        load_library = remote_export(proc, k32_base, b"LoadLibraryA")
        print("  target kernel32 0x%08X  LoadLibraryA 0x%08X" % (k32_base, load_library))

        path = str(DLL).encode("mbcs") + b"\0"
        addr = k32.VirtualAllocEx(proc, None, len(path), MEM_COMMIT_RESERVE, PAGE_READWRITE)
        if not addr:
            raise SystemExit("VirtualAllocEx failed: %d" % ctypes.get_last_error())
        try:
            written = ctypes.c_size_t(0)
            if not k32.WriteProcessMemory(proc, ctypes.c_void_p(addr), path,
                                          len(path), ctypes.byref(written)):
                raise SystemExit("WriteProcessMemory failed: %d" % ctypes.get_last_error())
            call_remote(proc, load_library, addr, "LoadLibraryA")
        finally:
            k32.VirtualFreeEx(proc, ctypes.c_void_p(addr), 0, MEM_RELEASE)
    finally:
        k32.CloseHandle(proc)

    # LoadLibraryA's exit code is a truncated HMODULE on Win64 hosts, so confirm
    # via the module list rather than trusting it.
    time.sleep(0.4)
    m = loaded_module(pid)
    if not m:
        print("  INJECT FAILED: module not present after LoadLibraryA")
        return 1
    print("  injected: %s at 0x%08X" % (m[0], m[1]))
    return 0


def do_eject(pid: int) -> int:
    m = loaded_module(pid)
    if not m:
        print("  not loaded, nothing to eject")
        return 0
    proc = open_target(pid)
    try:
        # Ask the DLL to quiesce first. FreeLibrary unmaps the code
        # immediately, so any thread still running inside it faults -- which
        # kills the client, not just the mod.
        try:
            prepare = remote_export(proc, m[1], b"osmw_prepare_unload")
            call_remote(proc, prepare, 0, "osmw_prepare_unload")
            print("  quiesced")
        except OSError as exc:
            print("  WARNING: no osmw_prepare_unload export (%s)" % exc)
            print("  ejecting anyway -- this can fault an older build")

        k32_base = next((b for n, b, _ in modules(pid) if n.lower() == "kernel32.dll"), 0)
        free_library = remote_export(proc, k32_base, b"FreeLibrary")
        call_remote(proc, free_library, m[1], "FreeLibrary")
    finally:
        k32.CloseHandle(proc)

    time.sleep(0.4)
    if loaded_module(pid):
        print("  EJECT FAILED: still loaded")
        return 1
    print("  ejected")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--pid", type=int, help="target PID (default: the single running main.exe)")
    group = ap.add_mutually_exclusive_group()
    group.add_argument("--eject", action="store_true")
    group.add_argument("--status", action="store_true")
    group.add_argument("--launch", action="store_true",
                       help="start a new client suspended and inject before startup init")
    group.add_argument("--clear", action="store_true",
                       help="post osmw_chat_clear to the client's chat window")
    group.add_argument("--tab", metavar="WHICH",
                       help="switch the channel filter: cycle|all|chat|system")
    group.add_argument("--probe", action="store_true",
                       help="render colour ramps into the pane (diagnostic; "
                            "injected into the window's private ring only)")
    args = ap.parse_args()

    if args.launch:
        return do_launch()

    if args.pid:
        pid = args.pid
    else:
        pids = find_pids(TARGET_EXE)
        if not pids:
            raise SystemExit("no %s running -- start the client first" % TARGET_EXE)
        if len(pids) > 1:
            raise SystemExit("%d %s processes running (%s); pass --pid"
                             % (len(pids), TARGET_EXE, ", ".join(map(str, pids))))
        pid = pids[0]

    print("target: %s pid %d" % (TARGET_EXE, pid))
    if args.status:
        return do_status(pid)
    if args.clear:
        return do_clear(pid)
    if args.tab:
        return do_tab(pid, args.tab)
    if args.probe:
        return do_post(pid, b"osmw_chat_probe", 0, "colour probe")
    if args.eject:
        return do_eject(pid)
    return do_inject(pid)


if __name__ == "__main__":
    raise SystemExit(main())
