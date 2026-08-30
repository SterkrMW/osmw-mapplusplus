"""Capture the chat pane and save a magnified crop, so glyph detail is legible.

    cropzoom.py <pid> <out.png> [x y w h] [zoom]
"""
import ctypes, ctypes.wintypes as wt, struct, sys, zlib

PID = int(sys.argv[1])
OUT = sys.argv[2]
CX, CY, CW, CH = (int(v) for v in sys.argv[3:7]) if len(sys.argv) > 6 else (40, 470, 260, 130)
ZOOM = int(sys.argv[7]) if len(sys.argv) > 7 else 4

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

CW = min(CW, W - CX)
CH = min(CH, H - CY)


def px(x, y):
    o = ((H - 1 - y) * W + x) * 4
    return (raw[o + 2], raw[o + 1], raw[o])


rows = []
for y in range(CY, CY + CH):
    line = bytearray()
    for x in range(CX, CX + CW):
        rgb = bytes(px(x, y))
        line += rgb * ZOOM
    for _ in range(ZOOM):
        rows.append(b"\x00" + bytes(line))

out_w, out_h = CW * ZOOM, CH * ZOOM


def chunk(tag, data):
    return (struct.pack(">I", len(data)) + tag + data +
            struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


png = b"\x89PNG\r\n\x1a\n"
png += chunk(b"IHDR", struct.pack(">IIBBBBB", out_w, out_h, 8, 2, 0, 0, 0))
png += chunk(b"IDAT", zlib.compress(b"".join(rows), 6))
png += chunk(b"IEND", b"")
open(OUT, "wb").write(png)
print("wrote %s  (%dx%d crop at %d,%d, zoom %dx)" % (OUT, CW, CH, CX, CY, ZOOM))
