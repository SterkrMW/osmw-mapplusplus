"""Capture a client's chat window to a PNG so the result can actually be seen.

    python capture.py <pid> <out.png>
"""
import ctypes, ctypes.wintypes as wt, struct, sys, zlib

PID = int(sys.argv[1])
OUT = sys.argv[2]

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


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", wt.DWORD), ("biWidth", ctypes.c_long),
                ("biHeight", ctypes.c_long), ("biPlanes", wt.WORD),
                ("biBitCount", wt.WORD), ("biCompression", wt.DWORD),
                ("biSizeImage", wt.DWORD), ("biXPelsPerMeter", ctypes.c_long),
                ("biYPelsPerMeter", ctypes.c_long), ("biClrUsed", wt.DWORD),
                ("biClrImportant", wt.DWORD)]


def find_chat(pid):
    hwnd = None
    while True:
        hwnd = u32.FindWindowExA(None, hwnd, b"WindowedMode", None)
        if not hwnd:
            return None
        owner = wt.DWORD(0)
        u32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid:
            return hwnd


def write_png(path, w, h, bgra_bottom_up):
    """GetDIBits gives BGRA bottom-up; PNG wants RGB top-down."""
    rows = []
    stride = w * 4
    for y in range(h - 1, -1, -1):
        row = bgra_bottom_up[y * stride:(y + 1) * stride]
        out = bytearray(b"\x00")            # filter type 0
        for x in range(0, stride, 4):
            out += bytes((row[x + 2], row[x + 1], row[x]))
        rows.append(bytes(out))
    raw = b"".join(rows)

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6))
    png += chunk(b"IEND", b"")
    open(path, "wb").write(png)


hwnd = find_chat(PID)
if not hwnd:
    raise SystemExit("no chat window for pid %d" % PID)

rect = wt.RECT()
u32.GetClientRect(hwnd, ctypes.byref(rect))
w, h = rect.right - rect.left, rect.bottom - rect.top
print("chat window 0x%X, client %dx%d" % (hwnd, w, h))

src = u32.GetDC(hwnd)
mem = g32.CreateCompatibleDC(src)
bmp = g32.CreateCompatibleBitmap(src, w, h)
old = g32.SelectObject(mem, bmp)

if not u32.PrintWindow(hwnd, mem, 1):        # PW_CLIENTONLY
    print("PrintWindow failed: %d" % ctypes.get_last_error())

bi = BITMAPINFOHEADER()
bi.biSize = ctypes.sizeof(bi)
bi.biWidth = w
bi.biHeight = h
bi.biPlanes = 1
bi.biBitCount = 32
bi.biCompression = 0

buf = (ctypes.c_char * (w * h * 4))()
if not g32.GetDIBits(mem, bmp, 0, h, buf, ctypes.byref(bi), 0):
    raise SystemExit("GetDIBits failed: %d" % ctypes.get_last_error())

g32.SelectObject(mem, old)
g32.DeleteObject(bmp)
g32.DeleteDC(mem)
u32.ReleaseDC(hwnd, src)

write_png(OUT, w, h, bytes(buf))
print("wrote %s" % OUT)
