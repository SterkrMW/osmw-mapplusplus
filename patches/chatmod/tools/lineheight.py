"""Compare the line heights the client stores with the font's real metrics."""
import ctypes, ctypes.wintypes as wt, struct, sys

PID = int(sys.argv[1])
OBJ = 0x007DEDD0

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
g32 = ctypes.WinDLL("gdi32", use_last_error=True)
k32.OpenProcess.restype = wt.HANDLE
k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                  ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
proc = k32.OpenProcess(0x1F0FFF, False, PID)
if not proc:
    raise SystemExit("OpenProcess failed %d" % ctypes.get_last_error())


def rd(addr, size):
    buf = (ctypes.c_char * size)()
    got = ctypes.c_size_t(0)
    k32.ReadProcessMemory(proc, ctypes.c_void_p(addr), ctypes.byref(buf),
                          size, ctypes.byref(got))
    return bytes(buf[:got.value])


class TEXTMETRICA(ctypes.Structure):
    _fields_ = [("tmHeight", ctypes.c_long), ("tmAscent", ctypes.c_long),
                ("tmDescent", ctypes.c_long), ("tmInternalLeading", ctypes.c_long),
                ("tmExternalLeading", ctypes.c_long),
                ("tmAveCharWidth", ctypes.c_long), ("tmMaxCharWidth", ctypes.c_long),
                ("tmWeight", ctypes.c_long), ("tmOverhang", ctypes.c_long),
                ("tmDigitizedAspectX", ctypes.c_long),
                ("tmDigitizedAspectY", ctypes.c_long),
                ("tmFirstChar", ctypes.c_byte), ("tmLastChar", ctypes.c_byte),
                ("tmDefaultChar", ctypes.c_byte), ("tmBreakChar", ctypes.c_byte),
                ("tmItalic", ctypes.c_byte), ("tmUnderlined", ctypes.c_byte),
                ("tmStruckOut", ctypes.c_byte), ("tmPitchAndFamily", ctypes.c_byte),
                ("tmCharSet", ctypes.c_byte)]


g32.CreateFontA.restype = wt.HANDLE
g32.CreateFontA.argtypes = [ctypes.c_int] * 5 + [wt.DWORD] * 8 + [ctypes.c_char_p]
g32.CreateCompatibleDC.restype = wt.HDC
g32.CreateCompatibleDC.argtypes = [wt.HDC]
g32.SelectObject.restype = wt.HGDIOBJ
g32.SelectObject.argtypes = [wt.HDC, wt.HGDIOBJ]
g32.GetTextMetricsA.argtypes = [wt.HDC, ctypes.POINTER(TEXTMETRICA)]

face = sys.argv[2].encode("mbcs") if len(sys.argv) > 2 else b"Consolas"
height = int(sys.argv[3]) if len(sys.argv) > 3 else 22
width = int(sys.argv[4]) if len(sys.argv) > 4 else 8

dc = g32.CreateCompatibleDC(None)
font = g32.CreateFontA(height, width, 0, 0, 400, 0, 0, 0, 1, 0, 0, 0, 0, face)
g32.SelectObject(dc, font)
tm = TEXTMETRICA()
g32.GetTextMetricsA(dc, ctypes.byref(tm))
print("font %s h=%d w=%d" % (face.decode(), height, width))
print("  tmHeight %d  = ascent %d + descent %d, external leading %d"
      % (tm.tmHeight, tm.tmAscent, tm.tmDescent, tm.tmExternalLeading))
print("  a single line needs %d px to show descenders\n" % tm.tmHeight)

count = rd(OBJ + 0x7A63, 1)[0] & 0x7F
print("stored line heights (chatobj+0x408 per record), %d lines:" % count)
seen = {}
for i in range(count):
    rec = OBJ + 0x80 + i * 0x410
    h = struct.unpack("<i", rd(rec + 0x408, 4))[0]
    text = rd(rec, 40).split(b"\x00")[0].decode("mbcs", "replace")
    seen[h] = seen.get(h, 0) + 1
    if i < 6:
        print("  line[%2d] height %3d   %r" % (i, h, text[:34]))
print("\n  heights seen: %s"
      % ", ".join("%dpx x%d" % (h, n) for h, n in sorted(seen.items())))
single = min(seen) if seen else 0
print("  smallest (a one-line message) = %d vs tmHeight %d -> %s"
      % (single, tm.tmHeight,
         "SHORT BY %d" % (tm.tmHeight - single) if single < tm.tmHeight else "ok"))
