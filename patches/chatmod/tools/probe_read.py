"""Capture the chat pane and report the DISTINCT colours actually rendered.

Turns "those look the same" into hard numbers: for each probe row, walk the
pixels left to right and list every distinct non-background colour with the x
range it covers, so each swatch can be matched to what the engine really drew.

    python probe_read.py <pid>
"""
import ctypes, ctypes.wintypes as wt, struct, sys
from collections import Counter

PID = int(sys.argv[1])

u32 = ctypes.WinDLL("user32", use_last_error=True)
g32 = ctypes.WinDLL("gdi32", use_last_error=True)
u32.FindWindowExA.restype = wt.HWND
u32.FindWindowExA.argtypes = [wt.HWND, wt.HWND, ctypes.c_char_p, ctypes.c_char_p]
u32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
u32.GetClientRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
u32.GetDC.restype = wt.HDC
u32.GetDC.argtypes = [wt.HWND]
u32.ReleaseDC.argtypes = [wt.HWND, wt.HDC]
u32.PrintWindow.argtypes = [wt.HWND, wt.HDC, wt.UINT]
g32.CreateCompatibleDC.restype = wt.HDC
g32.CreateCompatibleDC.argtypes = [wt.HDC]
g32.CreateCompatibleBitmap.restype = wt.HBITMAP
g32.CreateCompatibleBitmap.argtypes = [wt.HDC, ctypes.c_int, ctypes.c_int]
g32.SelectObject.restype = wt.HGDIOBJ
g32.SelectObject.argtypes = [wt.HDC, wt.HGDIOBJ]
g32.DeleteObject.argtypes = [wt.HGDIOBJ]
g32.DeleteDC.argtypes = [wt.HDC]
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
        raise SystemExit("no chat window for pid %d" % PID)
    owner = wt.DWORD(0)
    u32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
    if owner.value == PID:
        break

rect = wt.RECT()
u32.GetClientRect(hwnd, ctypes.byref(rect))
W, H = rect.right, rect.bottom

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
g32.SelectObject(mem, old)
g32.DeleteObject(bmp)
g32.DeleteDC(mem)
u32.ReleaseDC(hwnd, src)
raw = bytes(buf)


def px(x, y):
    """Top-down (x, y) -> (r, g, b). GetDIBits gave us bottom-up BGRA."""
    o = ((H - 1 - y) * W + x) * 4
    return (raw[o + 2], raw[o + 1], raw[o])


# The pane background is a textured blue; sample it so we can ignore it.
bg = Counter(px(x, y) for x in range(2, W, 7) for y in range(H - 60, H - 40))
background = {c for c, n in bg.most_common(12)}


def row_colours(y0, y1, label):
    """Distinct strong colours across a band, in x order."""
    runs = []
    for x in range(W):
        best = None
        for y in range(y0, y1):
            c = px(x, y)
            if c in background:
                continue
            # Swatches are bright; the pane's blue texture and the glyph
            # anti-aliasing are not. Anything without a strong channel is noise.
            if max(c) < 0xC0:
                continue
            if best is None or sum(c) > sum(best):
                best = c
        if best is None:
            continue
        if runs and runs[-1][0] == best:
            runs[-1][2] = x
        else:
            runs.append([best, x, x])
    print("\n%s  (y %d..%d)" % (label, y0, y1))
    merged = [r for r in runs if r[2] - r[1] >= 1]
    seen = []
    for colour, x0, x1 in merged:
        if seen and seen[-1][0] == colour and x0 - seen[-1][2] < 6:
            seen[-1][2] = x1
            continue
        seen.append([colour, x0, x1])
    for colour, x0, x1 in seen:
        print("   x%3d-%3d  #%02X%02X%02X" % (x0, x1, colour[0], colour[1], colour[2]))
    distinct = sorted({c for c, _, _ in seen})
    print("   -> %d distinct colour(s): %s"
          % (len(distinct), ", ".join("#%02X%02X%02X" % c for c in distinct)))


print("client %dx%d" % (W, H))
if len(sys.argv) > 2:
    bands = [int(v) for v in sys.argv[2].split(",")]
    for i, y in enumerate(bands):
        row_colours(y, y + 16, "row @ y=%d" % y)
else:
    print("pass a comma separated list of row top coordinates as argv[2]")
