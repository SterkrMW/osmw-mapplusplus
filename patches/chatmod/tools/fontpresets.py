"""Find chat fonts and sizes that wrap correctly.

The client wraps chat by assuming a fixed character cell -- 8 pixels, the cWidth
its own text font is created with. It does not re-measure when the font
changes, so a font whose characters are wider than 8 overflows the right edge
and clips. Measured, not assumed; see NOTES.txt.

Two ways to stay inside that budget:

  natural   pick a height at which the face is already <= 8px per character.
            Glyphs keep their true proportions.
  pinned    set FontWidth=8 and raise FontSize freely. GDI condenses the
            glyphs to the 8px cell, so text gets taller without getting wider.
            This is how to get a genuinely larger chat font.

    fontpresets.py scan                 measure the installed faces
    fontpresets.py scan Consolas Tahoma just these
    fontpresets.py apply <name>         write a named preset to config.ini
    fontpresets.py list                 show the presets

Nothing here talks to the game; it measures through GDI exactly as the client
would. Verify a choice in-game afterwards with a long message.
"""
from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from palette import set_config           # noqa: E402

# The cell the client's own text font declares, and therefore the width its
# wrap arithmetic assumes.
CELL = 8

# The tallest font the client draws in full. Above this it cuts the bottom
# rows of every line.
#
# Test with UNDERSCORES, not descenders. An underscore sits lower than a y or g
# tail, so it is the strictest glyph: at 19 the tails still look about right
# while every underscore has already gone, which is exactly how this was first
# got wrong. 18 draws both correctly, 19 loses underscores, 20 loses tails too.
#
# It is not the descent. Consolas 18 and 20 both have a 4px descent and only 20
# clips, so it behaves like a fixed pixel budget rather than anything derived
# from the font.
MAX_HEIGHT = 18

g32 = ctypes.WinDLL("gdi32", use_last_error=True)
g32.CreateFontA.restype = wt.HANDLE
g32.CreateFontA.argtypes = ([ctypes.c_int] * 5 + [wt.DWORD] * 8 +
                            [ctypes.c_char_p])
g32.CreateCompatibleDC.restype = wt.HDC
g32.CreateCompatibleDC.argtypes = [wt.HDC]
g32.SelectObject.restype = wt.HGDIOBJ
g32.SelectObject.argtypes = [wt.HDC, wt.HGDIOBJ]
g32.DeleteObject.argtypes = [wt.HGDIOBJ]
g32.DeleteDC.argtypes = [wt.HDC]


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


g32.GetTextMetricsA.argtypes = [wt.HDC, ctypes.POINTER(TEXTMETRICA)]
g32.GetTextExtentPoint32A.argtypes = [wt.HDC, ctypes.c_char_p, ctypes.c_int,
                                      ctypes.POINTER(wt.SIZE)]

# GDI's tmAveCharWidth is not the average of real text -- it is roughly the
# width of 'x', which badly understates a proportional face. Measuring an
# actual sentence predicts wrapping; tmAveCharWidth does not. Tahoma 22 passes
# on tmAveCharWidth and still clips in game.
SAMPLE = (b"the quick brown fox jumps over the lazy dog "
          b"WHERE MANY WIZARDS QUICKLY VEX GNOMES "
          b"Selling major helm, armor cata, PM me 12mil each")


def measure(face: str, height: int, width: int = 0):
    """Character metrics for a font, exactly as the client would create it."""
    dc = g32.CreateCompatibleDC(None)
    font = g32.CreateFontA(height, width, 0, 0, 400, 0, 0, 0, 1, 0, 0, 0, 0,
                           face.encode("mbcs"))
    if not dc or not font:
        return None
    old = g32.SelectObject(dc, font)
    tm = TEXTMETRICA()
    g32.GetTextMetricsA(dc, ctypes.byref(tm))
    size = wt.SIZE()
    g32.GetTextExtentPoint32A(dc, SAMPLE, len(SAMPLE), ctypes.byref(size))
    g32.SelectObject(dc, old)
    g32.DeleteObject(font)
    g32.DeleteDC(dc)

    tm.sample_per_char = size.cx / float(len(SAMPLE))
    tm.monospace = (tm.tmPitchAndFamily & 1) == 0   # TMPF_FIXED_PITCH is clear
    return tm


DEFAULT_FACES = ["Courier New", "Consolas", "Lucida Console", "Cascadia Mono",
                 "DejaVu Sans Mono", "Tahoma", "Verdana", "Segoe UI", "Arial"]


def cmd_scan(faces: list[str]) -> int:
    faces = faces or DEFAULT_FACES

    base = measure("Courier New", 16, 8)
    print("client baseline: Courier New h=16, cell %dpx  (real text %.1fpx/char)\n"
          % (CELL, base.sample_per_char))

    print("  %-18s %-6s %-24s %s"
          % ("face", "pitch", "largest natural height", "measured px/char there"))
    for face in faces:
        probe = measure(face, 16)
        if not probe or probe.tmAveCharWidth <= 0:
            print("  %-18s not installed" % face)
            continue

        # Largest height at which REAL text still fits the 8px-per-character
        # budget the client's wrap arithmetic assumes, and which also stays
        # inside the descender budget.
        natural, at = 0, 0.0
        for height in range(10, MAX_HEIGHT + 1):
            tm = measure(face, height)
            if tm and tm.sample_per_char <= CELL:
                natural, at = height, tm.sample_per_char
            else:
                break

        pitch = "fixed" if probe.monospace else "prop"
        if natural:
            print("  %-18s %-6s %-24s %.2f"
                  % (face, pitch, "%d px" % natural, at))
        else:
            print("  %-18s %-6s %-24s -" % (face, pitch, "none at any size"))

    print("\nA 'natural' height keeps the face's true proportions and wraps")
    print("correctly. Above it, set FontWidth=8: GDI condenses every glyph")
    print("into the 8px cell, so wrapping stays correct at any height.")
    print("\nBut do not go above %dpx whatever the width. The client cuts the"
          % MAX_HEIGHT)
    print("bottom rows of every line past that. Judge it on UNDERSCORES, which")
    print("sit lower than any descender: at 19 the y and g tails still look")
    print("about right while every underscore has already gone.")
    print("\nPrefer a fixed-pitch face. With a proportional one the budget is an")
    print("average, so a line of wide capitals can still overflow even at a")
    print("height this reports as safe -- Tahoma 22 does exactly that.")
    return 0


# Two limits bound every entry, and both were found against the real renderer:
#   wrapping    real text must fit the 8px cell, or lines run off the right
#               edge. Pinning FontWidth=8 satisfies this at any height.
#   descenders  the font must be no taller than MAX_HEIGHT, or the bottom rows
#               of every line are cut and y, g, q lose their tails.
# All fixed-pitch: a proportional face can overflow on wide capitals even
# below its reported natural height.
PRESETS = {
    "stock": ("",              0,  -1, "the client's own Courier New 16"),
    "crisp": ("Consolas",      16, -1, "stock size, cleaner glyphs"),
    "large": ("Consolas",      18, -1, "the largest the client draws in full; "
                                       "verified in game"),
    "wide":  ("Cascadia Mono", 18, -1, "same size, wider-looking face"),
}


def cmd_list() -> int:
    print("  %-10s %-18s %-6s %-7s %s"
          % ("preset", "face", "size", "width", "notes"))
    for name, (face, size, width, note) in PRESETS.items():
        print("  %-10s %-18s %-6s %-7s %s"
              % (name, face or "(client default)", size or "-",
                 "auto" if width < 0 else width, note))
    print("\n  fontpresets.py apply large")
    return 0


def cmd_apply(args: list[str]) -> int:
    if not args or args[0] not in PRESETS:
        print("unknown preset; try: %s" % ", ".join(PRESETS))
        return 1
    face, size, width, note = PRESETS[args[0]]
    set_config("FontFace", face)
    set_config("FontSize", str(size))
    set_config("FontWidth", str(width))
    print("applied '%s': %s" % (args[0], note))
    print("  FontFace=%s FontSize=%d FontWidth=%d" % (face, size, width))
    print("Takes effect within a couple of seconds; no restart needed.")
    return 0


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    command, args = sys.argv[1], sys.argv[2:]
    if command == "scan":
        return cmd_scan(args)
    if command == "list":
        return cmd_list()
    if command == "apply":
        return cmd_apply(args)
    print(__doc__)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
