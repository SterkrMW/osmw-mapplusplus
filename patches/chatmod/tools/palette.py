"""Pick chat colours that survive the client's renderer.

The rich-text engine keeps colour in 16 bits and truncates: red and blue lose
their low 3 bits, green its low 2. So two #c codes closer than 8 in red/blue or
4 in green are the same colour on screen, however different they look in an
editor. Measured, not assumed -- see NOTES.txt.

    palette.py check FF0000 FB0000 00A0FF     what these really render as
    palette.py suggest 12                     12 well separated, legible colours
    palette.py show FF0000,00FF00,...         render a palette in a live client

`show` writes the list to config.ini as [Chat] ProbePalette and asks the mod to
draw it, so a candidate palette can be judged against the real renderer on the
real background rather than against a colour picker.
"""
from __future__ import annotations

import colorsys
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "common"))
from paths import game_root  # noqa: E402

# The mod's own config.ini, beside main.exe -- not the repo's.
CONFIG = game_root() / "config.ini"

# The chat pane's background, sampled from a capture. Colours need to stand off
# this to be readable.
BACKGROUND = (0x0B, 0x23, 0x7E)


def snap(rgb: int) -> int:
    """What the engine will actually render for a 24-bit colour."""
    r = (rgb >> 16) & 0xF8
    g = (rgb >> 8) & 0xFC
    b = rgb & 0xF8
    return (r << 16) | (g << 8) | b


def parse(text: str) -> int:
    return int(text.lstrip("#").lstrip("cC"), 16) & 0xFFFFFF


def luminance(rgb: tuple[int, int, int]) -> float:
    r, g, b = (c / 255.0 for c in rgb)
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def contrast(a: tuple[int, int, int], b: tuple[int, int, int]) -> float:
    la, lb = luminance(a), luminance(b)
    lo, hi = min(la, lb), max(la, lb)
    return (hi + 0.05) / (lo + 0.05)


def to_tuple(rgb: int) -> tuple[int, int, int]:
    return ((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF)


def distance(a: tuple[int, int, int], b: tuple[int, int, int]) -> float:
    """Rough perceptual distance; good enough for keeping swatches apart."""
    rm = (a[0] + b[0]) / 2.0
    dr, dg, db = a[0] - b[0], a[1] - b[1], a[2] - b[2]
    return ((2 + rm / 256) * dr * dr + 4 * dg * dg +
            (2 + (255 - rm) / 256) * db * db) ** 0.5


def cmd_check(args: list[str]) -> int:
    if not args:
        print("give one or more RRGGBB values")
        return 1

    rendered: dict[int, list[str]] = {}
    print("  %-10s %-10s %s" % ("asked for", "renders as", ""))
    for text in args:
        try:
            value = parse(text)
        except ValueError:
            print("  %-10s  not a hex colour" % text)
            continue
        got = snap(value)
        exact = "exact" if got == value else "shifted"
        print("  #%06X    #%06X    %s" % (value, got, exact))
        rendered.setdefault(got, []).append("#%06X" % value)

    clashes = {k: v for k, v in rendered.items() if len(v) > 1}
    if clashes:
        print("\n  these collapse to the same colour on screen:")
        for got, sources in clashes.items():
            print("    #%06X  <-  %s" % (got, ", ".join(sources)))
    else:
        print("\n  all distinct on screen")
    return 0


def cmd_suggest(args: list[str]) -> int:
    want = int(args[0]) if args else 10

    # Spread evenly around the hue wheel rather than maximising a distance
    # metric. Perceptual metrics weight green heavily, and green is the one
    # channel with a spare bit here, so optimising against one lands you with a
    # palette of greens. An even hue walk is what a palette actually wants.
    chosen: list[tuple[int, int, int]] = []
    for index in range(want):
        hue = index / float(want)
        pick = None
        # Drop value, then saturation, until it is legible and not a duplicate
        # of something already chosen once snapped to the grid.
        for value in (1.0, 0.92, 0.84, 0.76):
            for sat in (1.0, 0.85, 0.65, 0.45):
                r, g, b = colorsys.hsv_to_rgb(hue, sat, value)
                rgb = snap((int(r * 255) << 16) | (int(g * 255) << 8) | int(b * 255))
                colour = to_tuple(rgb)
                if contrast(colour, BACKGROUND) < 3.0:
                    continue
                if any(distance(colour, other) < 60 for other in chosen):
                    continue
                pick = colour
                break
            if pick:
                break
        if pick:
            chosen.append(pick)

    if not chosen:
        print("no legible candidates; is BACKGROUND right?")
        return 1

    print("%d colours on the 565 grid, legible on the chat background:\n" % len(chosen))
    for colour in chosen:
        rgb = (colour[0] << 16) | (colour[1] << 8) | colour[2]
        print("  #c%06X    contrast %.1f" % (rgb, contrast(colour, BACKGROUND)))
    print("\nProbePalette=%s"
          % ",".join("%06X" % ((c[0] << 16) | (c[1] << 8) | c[2]) for c in chosen))
    print("\nPaste that into [Chat] in config.ini, then:")
    print("  python patches/chatmod/tools/palette.py show")
    return 0


def set_config(key: str, value: str) -> None:
    """Write a key into the [Chat] section of config.ini."""
    lines = CONFIG.read_text(encoding="utf-8").splitlines() if CONFIG.exists() else []
    out, in_chat, done = [], False, False
    for line in lines:
        stripped = line.strip()
        if stripped.startswith("["):
            if in_chat and not done:
                out.append("%s=%s" % (key, value))
                done = True
            in_chat = stripped.lower() == "[chat]"
        elif in_chat and stripped.lower().startswith(key.lower() + "="):
            out.append("%s=%s" % (key, value))
            done = True
            continue
        out.append(line)
    if not done:
        if not in_chat:
            out.append("[Chat]")
        out.append("%s=%s" % (key, value))
    CONFIG.write_text("\n".join(out) + "\n", encoding="utf-8")


def cmd_show(args: list[str]) -> int:
    if args:
        colours = ",".join("%06X" % parse(t) for t in args[0].split(","))
        set_config("ProbePalette", colours)
        print("wrote ProbePalette=%s" % colours)
    print("asking the client to render it ...")
    return subprocess.call([sys.executable,
                            str(Path(__file__).with_name("inject_test.py")),
                            "--probe"])


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    command, args = sys.argv[1], sys.argv[2:]
    if command == "check":
        return cmd_check(args)
    if command == "suggest":
        return cmd_suggest(args)
    if command == "show":
        return cmd_show(args)
    print(__doc__)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
