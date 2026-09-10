/* Phase 2 -- font face and size.
 *
 * The client builds two fonts in its window-init routine and never rebuilds
 * them:
 *
 *   small  6x12  selected into DC [obj+0x4C]  -- draws ONLY the channel chip
 *                labels ("World", "System", ...) into a vertical atlas strip
 *   large  8x16  selected into DC [obj+0x44]  -- draws the message text
 *
 * The atlas is blitted per line as a fixed 38x18 cell, so the two fonts are
 * independent: changing the message font cannot break the chips, and vice
 * versa. That is why FontSize applies to the message font only. Chips get the
 * face but keep their size, because their cell is a fixed 38x18 and a larger
 * label would simply be clipped.
 *
 * Both fonts are created through `call [CreateFontA]`, which we rewrite into a
 * direct call to our own stdcall shim. The arguments are already on the stack,
 * so the shim just adjusts them and forwards.
 */
#include "osmw_chat.h"

#include <stdio.h>
#include <string.h>

/* Offset from each signature's start to its `call [CreateFontA]` instruction. */
#define FONT_CALL_DELTA 17

/* The chip atlas cell the client blits per line (see the BitBlt at the chip
 * draw site): anything wider or taller than this is clipped. */
#define CHIP_CELL_W 38
#define CHIP_CELL_H 18

typedef HFONT (WINAPI *createfont_fn)(int, int, int, int, int, DWORD, DWORD, DWORD,
                                      DWORD, DWORD, DWORD, DWORD, DWORD, LPCSTR);

/* Chat object fields the fonts live in. */
#define OBJ_TEXT_DC   0x44      /* the 760x600 surface the messages go on */
#define OBJ_CHIP_DC   0x4C      /* the 800x600 surface holding the chip atlas */
#define OBJ_TEXT_FONT 0x7A80

/* The chip atlas builder, so a chip font change can be re-rendered.
 * __thiscall(this) with no stack arguments, returns with a plain `ret`. */
typedef void (__fastcall *chipatlas_fn)(void *ecx, void *edx);

/* Exactly what the client asked CreateFontA for, captured the first time it
 * asks. Every rebuild starts from this rather than from the font currently in
 * place, so repeated changes cannot compound -- setting FontSize to 20 and
 * then back to 0 has to land on the client's own 16, not on whatever the last
 * override happened to leave behind. */
typedef struct {
    int   captured;
    int   h, w, esc, orient, weight;
    DWORD italic, underline, strike, charset, outp, clip, qual, pitch;
    char  face[LF_FACESIZE];
} font_baseline_t;

static mw_hook_t       g_small_hook;
static mw_hook_t       g_large_hook;
static int             g_installed;
static font_baseline_t g_baseline[2];       /* [0] = chip, [1] = text */
static char            g_applied[224];      /* the settings currently in effect */

/* Longest English chip labels; used to warn when a custom face overflows the
 * fixed chip cell. */
static const char *LONGEST_LABELS[] = { "System", "Market", "Local", "Guild", "Party" };

static HFONT real_create_font(int h, int w, int esc, int orient, int weight,
                              DWORD italic, DWORD underline, DWORD strike,
                              DWORD charset, DWORD outp, DWORD clip,
                              DWORD qual, DWORD pitch, LPCSTR face)
{
    static createfont_fn real;
    if (!real) {
        HMODULE gdi = GetModuleHandleA("gdi32.dll");
        real = (createfont_fn)GetProcAddress(gdi, "CreateFontA");
    }
    if (!real)
        return NULL;
    return real(h, w, esc, orient, weight, italic, underline, strike,
                charset, outp, clip, qual, pitch, face);
}

/* Widest channel label as `f` renders it, in pixels. 0 if it cannot measure. */
static int widest_chip_label(HFONT f, const char **which)
{
    HDC     dc = CreateCompatibleDC(NULL);
    HGDIOBJ old;
    int     i, worst = 0;

    if (!dc)
        return 0;
    old = SelectObject(dc, f);
    for (i = 0; i < (int)(sizeof(LONGEST_LABELS) / sizeof(LONGEST_LABELS[0])); i++) {
        SIZE sz;
        if (GetTextExtentPoint32A(dc, LONGEST_LABELS[i],
                                  (int)strlen(LONGEST_LABELS[i]), &sz) &&
            sz.cx > worst) {
            worst = sz.cx;
            if (which)
                *which = LONGEST_LABELS[i];
        }
    }
    SelectObject(dc, old);
    DeleteDC(dc);
    return worst;
}

/* Report how wide the chip labels render in `f`, so an overflowing face shows
 * up in the log instead of as silently clipped text in game. */
static void check_chip_fit(HFONT f)
{
    HDC     dc = CreateCompatibleDC(NULL);
    HGDIOBJ old;
    int     i, worst = 0;
    const char *worst_label = "";

    if (!dc)
        return;
    old = SelectObject(dc, f);
    for (i = 0; i < (int)(sizeof(LONGEST_LABELS) / sizeof(LONGEST_LABELS[0])); i++) {
        SIZE sz;
        if (GetTextExtentPoint32A(dc, LONGEST_LABELS[i],
                                  (int)strlen(LONGEST_LABELS[i]), &sz)) {
            if (sz.cx > worst) {
                worst = sz.cx;
                worst_label = LONGEST_LABELS[i];
            }
        }
    }
    SelectObject(dc, old);
    DeleteDC(dc);

    if (worst > CHIP_CELL_W - 4) {
        mw_log("font: WARNING chip label \"%s\" measures %dpx, chip cell is %dpx "
               "-- labels will be clipped", worst_label, worst, CHIP_CELL_W);
    } else {
        mw_log("font: chip labels fit (widest \"%s\" = %dpx of %dpx)",
               worst_label, worst, CHIP_CELL_W);
    }
}

/* is_chip: the 6x12 font that renders the channel chip atlas. */
static void capture_baseline(int slot, int h, int w, int esc, int orient, int weight,
                             DWORD italic, DWORD underline, DWORD strike,
                             DWORD charset, DWORD outp, DWORD clip, DWORD qual,
                             DWORD pitch, LPCSTR face)
{
    font_baseline_t *b = &g_baseline[slot];

    if (b->captured)
        return;
    b->h = h; b->w = w; b->esc = esc; b->orient = orient; b->weight = weight;
    b->italic = italic; b->underline = underline; b->strike = strike;
    b->charset = charset; b->outp = outp; b->clip = clip; b->qual = qual;
    b->pitch = pitch;
    _snprintf(b->face, sizeof(b->face), "%s", face ? face : "");
    b->face[sizeof(b->face) - 1] = '\0';
    b->captured = 1;
}

static HFONT build_font(int is_chip, int h, int w, int esc, int orient, int weight,
                        DWORD italic, DWORD underline, DWORD strike, DWORD charset,
                        DWORD outp, DWORD clip, DWORD qual, DWORD pitch, LPCSTR face)
{
    const char *want_face;
    int   want_size, want_weight, want_width, want_qual;
    int   customised = 0;
    HFONT f;

    /* Chips may override the shared face; both fall back to "unchanged". */
    want_face = is_chip ? mw_cfg_str("ChipFontFace", mw_cfg_str("FontFace", ""))
                        : mw_cfg_str("FontFace", "");

    /* FontSize is deliberately NOT applied to the chip font: its cell is a
     * fixed 38x18 blit, so growing it only clips. ChipFontSize is there for
     * anyone who wants to try anyway. */
    want_size = is_chip ? mw_cfg_int("ChipFontSize", 0) : mw_cfg_int("FontSize", 0);

    want_weight = mw_cfg_int("FontWeight", 0);
    want_qual   = mw_cfg_int("FontQuality", -1);

    /* FontWidth belongs to the message font alone. It exists to hold the
     * message text inside the 8px cell the client's line wrapping assumes --
     * a concern the chips do not share, since they are drawn once into a
     * fixed 38px atlas cell. Applying it here forced the chip cell from its
     * baseline 6 up to 8 and clipped every label to "Marke", "Syste". */
    want_width = is_chip ? mw_cfg_int("ChipFontWidth", -1)
                         : mw_cfg_int("FontWidth", -1);

    capture_baseline(is_chip ? 0 : 1, h, w, esc, orient, weight, italic,
                     underline, strike, charset, outp, clip, qual, pitch, face);

    if (want_face[0]) {
        face = want_face;
        /* The client hardcodes FIXED_PITCH|FF_MODERN, which fights any
         * proportional face. Let GDI match the name it was given. */
        pitch = DEFAULT_PITCH | FF_DONTCARE;
        /* A forced cell width only makes sense for the original monospace
         * font; let GDI derive it unless the user asked otherwise. */
        if (want_width < 0)
            w = 0;
        customised = 1;
    }
    if (want_size > 0) {
        h = want_size;
        if (want_width < 0)
            w = 0;      /* keep the aspect natural when the height changes */
        customised = 1;
    }
    if (want_width >= 0) {
        w = want_width;
        customised = 1;
    }
    if (want_weight > 0) {
        weight = want_weight;
        customised = 1;
    }
    if (want_qual >= 0) {
        qual = (DWORD)want_qual;
        customised = 1;
    }

    f = real_create_font(h, w, esc, orient, weight, italic, underline, strike,
                         charset, outp, clip, qual, pitch, face);

    mw_log("font: %s -> \"%s\" h=%d w=%d weight=%d charset=%d pitch=0x%02X qual=%d%s%s",
           is_chip ? "chip " : "text ", face ? face : "(null)", h, w, weight,
           (int)charset, (unsigned)pitch, (int)qual,
           customised ? "  [customised]" : "  [client default]",
           f ? "" : "  CREATE FAILED");

    if (!f) {
        /* Never hand the client a NULL font -- fall back to exactly what it
         * asked for so the window still renders. */
        mw_log("font: falling back to the client's own font parameters");
        return real_create_font(h, w, esc, orient, weight, italic, underline,
                                strike, charset, outp, clip, qual,
                                FIXED_PITCH | FF_MODERN, face);
    }

    /* The chip cell is a fixed 38px and the labels are the client's, so a face
     * or width that overflows it silently clips every chip to "Marke",
     * "Syste". Narrow the cell until the widest label fits rather than let
     * that happen -- a slightly condensed label beats a truncated one. */
    if (is_chip) {
        const char *widest = "";
        int measured = widest_chip_label(f, &widest);
        int budget = CHIP_CELL_W - 4;

        if (measured > budget && measured > 0) {
            int narrowed = (w > 0 ? w : measured / 6) * budget / measured;
            if (narrowed < 3)
                narrowed = 3;
            {
                HFONT retry = real_create_font(h, narrowed, esc, orient, weight,
                                               italic, underline, strike,
                                               charset, outp, clip, qual,
                                               pitch, face);
                if (retry) {
                    mw_log("font: chip labels needed %dpx of %dpx, narrowing the "
                           "cell to %d", measured, budget, narrowed);
                    DeleteObject(f);
                    f = retry;
                }
            }
        }
        check_chip_fit(f);
    }
    return f;
}

static HFONT WINAPI chip_font_shim(int h, int w, int esc, int orient, int weight,
                                   DWORD italic, DWORD underline, DWORD strike,
                                   DWORD charset, DWORD outp, DWORD clip,
                                   DWORD qual, DWORD pitch, LPCSTR face)
{
    return build_font(1, h, w, esc, orient, weight, italic, underline, strike,
                      charset, outp, clip, qual, pitch, face);
}

static HFONT WINAPI text_font_shim(int h, int w, int esc, int orient, int weight,
                                   DWORD italic, DWORD underline, DWORD strike,
                                   DWORD charset, DWORD outp, DWORD clip,
                                   DWORD qual, DWORD pitch, LPCSTR face)
{
    return build_font(0, h, w, esc, orient, weight, italic, underline, strike,
                      charset, outp, clip, qual, pitch, face);
}

/* Take the baseline from whatever is currently selected in the client's device
 * contexts.
 *
 * The CreateFontA shims only see anything if the mod was loaded before the
 * client built its window, which means at launch. Injecting into a running
 * client leaves no baseline -- and the fonts sitting in those DCs are then the
 * client's own, because we were not there to change them. So font settings
 * work on a client we attached to late, not only on one we launched. If the
 * shims did run, they win: `captured` is already set.
 *
 * One caveat, and it only bites during development: eject the mod from a
 * client that WAS launched with it and re-inject, and the fonts in those DCs
 * are the previous instance's overrides, not the client's originals. The
 * baseline then anchors to those, so clearing FontSize returns to whatever was
 * last applied rather than to the client's own. Relaunch to get a true
 * baseline. A player who launches once through osmw_chat_launch.exe never hits
 * this. */
static void derive_baseline(unsigned char *obj)
{
    HDC dcs[2];
    int slot;

    dcs[0] = *(HDC *)(obj + OBJ_CHIP_DC);
    dcs[1] = *(HDC *)(obj + OBJ_TEXT_DC);

    for (slot = 0; slot < 2; slot++) {
        LOGFONTA lf;
        HFONT    current;

        if (g_baseline[slot].captured || !dcs[slot])
            continue;
        current = (HFONT)GetCurrentObject(dcs[slot], OBJ_FONT);
        if (!current || GetObjectA(current, sizeof(lf), &lf) != sizeof(lf))
            continue;

        capture_baseline(slot, lf.lfHeight, lf.lfWidth, lf.lfEscapement,
                         lf.lfOrientation, lf.lfWeight, lf.lfItalic,
                         lf.lfUnderline, lf.lfStrikeOut, lf.lfCharSet,
                         lf.lfOutPrecision, lf.lfClipPrecision, lf.lfQuality,
                         lf.lfPitchAndFamily, lf.lfFaceName);
        mw_log("font: baseline for the %s font taken from the live DC "
               "(\"%s\" h=%ld w=%ld)", slot ? "text" : "chip",
               lf.lfFaceName, lf.lfHeight, lf.lfWidth);
    }
}

/* Build one slot's font from its captured baseline plus the current settings. */
static HFONT font_from_baseline(int slot)
{
    const font_baseline_t *b = &g_baseline[slot];

    if (!b->captured)
        return NULL;
    return build_font(slot == 0, b->h, b->w, b->esc, b->orient, b->weight,
                      b->italic, b->underline, b->strike, b->charset,
                      b->outp, b->clip, b->qual, b->pitch, b->face);
}

/* A short signature of every setting that affects the fonts. */
static void settings_key(char *out, int size)
{
    _snprintf(out, size, "%s|%d|%d|%d|%d|%s|%d",
              mw_cfg_str("FontFace", ""), mw_cfg_int("FontSize", 0),
              mw_cfg_int("FontWeight", 0), mw_cfg_int("FontWidth", -1),
              mw_cfg_int("FontQuality", -1),
              mw_cfg_str("ChipFontFace", ""), mw_cfg_int("ChipFontSize", 0));
    out[size - 1] = '\0';
}

/* Rebuild both fonts and put them into the client's device contexts.
 *
 * MUST run on the game thread. It is called from the settings poll, which
 * lives in the window procedure, so it lands between frames rather than in the
 * middle of one.
 */
static void reapply(int chip_changed)
{
    unsigned char *obj = mw_addr[MW_OBJECT];
    HDC   text_dc, chip_dc;
    HFONT text_font, chip_font;
    HGDIOBJ replaced;

    if (!obj)
        return;
    text_dc = *(HDC *)(obj + OBJ_TEXT_DC);
    chip_dc = *(HDC *)(obj + OBJ_CHIP_DC);
    if (!text_dc || !chip_dc)
        return;                     /* window not built yet */

    text_font = font_from_baseline(1);
    chip_font = font_from_baseline(0);

    if (text_font) {
        replaced = SelectObject(text_dc, text_font);
        *(HFONT *)(obj + OBJ_TEXT_FONT) = text_font;
        if (replaced)
            DeleteObject(replaced);
    }
    if (chip_font) {
        replaced = SelectObject(chip_dc, chip_font);
        if (replaced)
            DeleteObject(replaced);
    }

    /* The channel chips are drawn with the chip font into an atlas built once
     * at window creation, so a chip font change needs that atlas redrawn. The
     * client's own builder is re-runnable -- it creates and destroys its own
     * brushes -- and it also refreshes each channel's atlas offset. */
    if (chip_changed && mw_addr[MW_CHIPATLAS]) {
        ((chipatlas_fn)mw_addr[MW_CHIPATLAS])(obj, NULL);
        mw_log("font: channel chip atlas rebuilt");
    }

    /* The cached line records hold wrap points and heights measured with the
     * font that has just been replaced. Left alone, the pane keeps the old
     * wrap width and clips at the right edge. */
    mw_mirror_invalidate_view();

    if (mw_addr[MW_DIRTYFLAG])
        *(DWORD *)mw_addr[MW_DIRTYFLAG] = 1;
}

/* Called from the settings poll. Applies a font change without a restart. */
void mw_font_poll(void)
{
    char key[sizeof(g_applied)];
    unsigned char *obj = mw_addr[MW_OBJECT];

    if (!g_installed || !obj)
        return;

    derive_baseline(obj);
    if (!g_baseline[1].captured)
        return;                     /* window not built yet; nothing to redo */

    settings_key(key, sizeof(key));
    if (!strcmp(key, g_applied))
        return;

    /* First call just records what the client already built. */
    if (g_applied[0]) {
        mw_log("font: settings changed, re-applying without a restart");
        reapply(1);     /* always redraw the atlas: this runs once per edit */
    }

    _snprintf(g_applied, sizeof(g_applied), "%s", key);
    g_applied[sizeof(g_applied) - 1] = '\0';
}

int mw_font_install(void)
{
    unsigned char *small_call, *large_call;

    if (g_installed)
        return 1;
    if (!mw_addr[MW_FONTSMALL] || !mw_addr[MW_FONTLARGE]) {
        mw_log("font: signatures unresolved, not installing");
        return 0;
    }

    small_call = mw_addr[MW_FONTSMALL] + FONT_CALL_DELTA;
    large_call = mw_addr[MW_FONTLARGE] + FONT_CALL_DELTA;

    /* Function-to-void* goes through UINT_PTR: they are the same width on
     * Win32, but the direct conversion is not standard C. */
    if (!mw_patch_call(&g_small_hook, small_call,
                       (void *)(UINT_PTR)chip_font_shim,
                       "chat chip CreateFontA call"))
        return 0;
    if (!mw_patch_call(&g_large_hook, large_call,
                       (void *)(UINT_PTR)text_font_shim,
                       "chat text CreateFontA call")) {
        mw_hook_remove(&g_small_hook);
        return 0;
    }

    g_installed = 1;
    mw_log("font: installed (chip @ %p, text @ %p)", small_call, large_call);
    return 1;
}

OSMW_RESULT mw_font_remove(void)
{
    OSMW_RESULT result;

    result = mw_hook_remove(&g_large_hook);
    if (result != OSMW_OK)
        return result;
    result = mw_hook_remove(&g_small_hook);
    if (result != OSMW_OK)
        return result;
    g_installed = 0;
    return OSMW_OK;
}
