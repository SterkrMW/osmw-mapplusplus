"""Measure the rendered chip cells in the chat pane's left gutter."""
import ctypes, ctypes.wintypes as wt, sys
from collections import Counter

PID = int(sys.argv[1])
u32 = ctypes.WinDLL("user32", use_last_error=True)
g32 = ctypes.WinDLL("gdi32", use_last_error=True)
u32.FindWindowExA.restype = wt.HWND
u32.FindWindowExA.argtypes = [wt.HWND, wt.HWND, ctypes.c_char_p, ctypes.c_char_p]
u32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
u32.GetClientRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
u32.GetDC.restype = wt.HDC; u32.GetDC.argtypes = [wt.HWND]
u32.ReleaseDC.argtypes = [wt.HWND, wt.HDC]
u32.PrintWindow.argtypes = [wt.HWND, wt.HDC, wt.UINT]
g32.CreateCompatibleDC.restype = wt.HDC; g32.CreateCompatibleDC.argtypes = [wt.HDC]
g32.CreateCompatibleBitmap.restype = wt.HBITMAP
g32.CreateCompatibleBitmap.argtypes = [wt.HDC, ctypes.c_int, ctypes.c_int]
g32.SelectObject.restype = wt.HGDIOBJ
g32.SelectObject.argtypes = [wt.HDC, wt.HGDIOBJ]
g32.DeleteObject.argtypes = [wt.HGDIOBJ]; g32.DeleteDC.argtypes = [wt.HDC]
g32.GetDIBits.argtypes = [wt.HDC, wt.HBITMAP, wt.UINT, wt.UINT,
                          ctypes.c_void_p, ctypes.c_void_p, wt.UINT]


class BMIH(ctypes.Structure):
    _fields_ = [("biSize", wt.DWORD), ("biWidth", ctypes.c_long),
                ("biHeight", ctypes.c_long), ("biPlanes", wt.WORD),
                ("biBitCount", wt.WORD), ("biCompression", wt.DWORD),
                ("biSizeImage", wt.DWORD), ("biXPelsPerMeter", ctypes.c_long),
                ("biYPelsPerMeter", ctypes.c_long), ("biClrUsed", wt.DWORD),
                ("biClrImportant", wt.DWORD)]


hwnd = None
while True:
    hwnd = u32.FindWindowExA(None, hwnd, b"WindowedMode", None)
    if not hwnd:
        raise SystemExit("no chat window")
    owner = wt.DWORD(0)
    u32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
    if owner.value == PID:
        break

r = wt.RECT()
u32.GetClientRect(hwnd, ctypes.byref(r))
W, H = r.right, r.bottom
src = u32.GetDC(hwnd)
mem = g32.CreateCompatibleDC(src)
bmp = g32.CreateCompatibleBitmap(src, W, H)
old = g32.SelectObject(mem, bmp)
u32.PrintWindow(hwnd, mem, 1)
bi = BMIH()
bi.biSize = ctypes.sizeof(bi)
bi.biWidth, bi.biHeight, bi.biPlanes, bi.biBitCount, bi.biCompression = W, H, 1, 32, 0
buf = (ctypes.c_char * (W * H * 4))()
g32.GetDIBits(mem, bmp, 0, H, buf, ctypes.byref(bi), 0)
g32.SelectObject(mem, old); g32.DeleteObject(bmp); g32.DeleteDC(mem)
u32.ReleaseDC(hwnd, src)
raw = bytes(buf)


def px(x, y):
    o = ((H - 1 - y) * W + x) * 4
    return (raw[o + 2], raw[o + 1], raw[o])


# The pane background, sampled from a column well right of the chips.
bg = Counter(px(x, y) for x in range(600, 700, 3) for y in range(30, H - 10, 3))
background = {c for c, _ in bg.most_common(10)}

# A chip fills most of the gutter width; a row belongs to a chip if several
# gutter pixels are non-background.
rows = []
for y in range(25, H):
    hits = sum(1 for x in range(2, 36) if px(x, y) not in background)
    rows.append(hits >= 20)

runs, start = [], None
for y, on in enumerate(rows, start=25):
    if on and start is None:
        start = y
    elif not on and start is not None:
        runs.append((start, y - 1, y - start))
        start = None

heights = [h for _, _, h in runs if h >= 6]
print("chip cells found: %d" % len(heights))
if heights:
    common = Counter(heights).most_common(4)
    print("heights seen: %s" % ", ".join("%dpx x%d" % (h, n) for h, n in common))
    tops = [s for s, _, h in runs if h >= 6]
    gaps = [b - a for a, b in zip(tops, tops[1:])]
    if gaps:
        print("spacing between chips: %s" % ", ".join(str(g) for g in sorted(set(gaps))[:6]))
