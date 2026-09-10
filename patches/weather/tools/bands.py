"""How coarse is the band gradient, and how fine does it need to be?

Mirrors fog_build_lut's integer arithmetic exactly. What matters is not the
band count but the biggest jump in the FINAL 5/6-bit channel value between two
vertically adjacent pixels: anything above 1 is a contour the eye can find.
"""
TOP, BOT = 135, 55
HEIGHT = 600


def conv565(rgb):
    r, g, b = (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


FOG = conv565(0xE0E4EC)
# green is the worst case: 6 bits, so the finest steps and the most contours
G_MASK, G_SHIFT = 0x07E0, 5
G_TOP = G_MASK >> G_SHIFT
G_FOG = (FOG & G_MASK) >> G_SHIFT


def band_alpha(alpha, band, bands):
    pct = TOP + ((BOT - TOP) * band) // (bands - 1)
    a = (alpha * pct) // 100
    return max(0, min(90, a))


def worst_step(alpha, bands):
    """Largest change in output channel value between adjacent screen rows."""
    worst = 0
    for src in range(G_TOP + 1):
        prev = None
        for y in range(HEIGHT):
            band = min((y * bands) // HEIGHT, bands - 1)
            a = band_alpha(alpha, band, bands)
            out = src + ((G_FOG - src) * a) // 100
            out = max(0, min(G_TOP, out))
            if prev is not None and abs(out - prev) > worst:
                worst = abs(out - prev)
            prev = out
    return worst


print("worst adjacent-row jump in the 6-bit green channel")
print("bands   a=25   a=40   a=55   a=90    KB of tables")
for bands in (8, 16, 32, 64, 128, 256):
    steps = [worst_step(a, bands) for a in (25, 40, 55, 90)]
    kb = bands * 3 * 64 * 2 / 1024.0
    print("%5d   %4d   %4d   %4d   %4d    %8.0f" % (bands, *steps, kb))
print()
print("1 = indistinguishable from the client's own 16-bit quantisation")
print("2+ = a visible contour line, which is what a moving scene picks out")
