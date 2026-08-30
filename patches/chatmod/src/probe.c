/* Colour probe -- a diagnostic, not a feature.
 *
 * The client's rich-text engine keeps the active colour in a 16-bit field
 * (`mov word ptr [esi+0xB4], ax` in the markup parser at 0x00477B05), so a
 * 24-bit #cRRGGBB is quantised the moment it is parsed. This renders known
 * colour ramps into the chat pane so the quantisation steps can be read
 * straight off a screenshot: where two adjacent swatches look identical, that
 * is where the precision runs out.
 *
 * The lines are injected into the window's private ring only. Nothing is sent
 * to the server, the client's own message ring is untouched, and everything
 * disappears on the next tab switch.
 *
 * Trigger it by posting the registered message "osmw_chat_probe" to the chat
 * window; patches/chatmod/tools/inject_test.py --probe does that.
 */
#include "osmw_chat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CH_INFO 7

/* Each swatch is "#cRRGGBB" followed by the same six digits as its label, so
 * every label is drawn in the colour it names. */
static void ramp(const char *title, const unsigned *colours, int count)
{
    char line[160];
    int  i, used;

    used = _snprintf(line, sizeof(line), "%s", title);
    if (used < 0)
        return;

    for (i = 0; i < count; i++) {
        int n = _snprintf(line + used, sizeof(line) - used - 1,
                          "#c%06X%06X ", colours[i], colours[i]);
        if (n < 0 || used + n >= (int)sizeof(line) - 1)
            break;
        used += n;
    }
    line[sizeof(line) - 1] = '\0';
    mw_mirror_inject(CH_INFO, "probe", line);
}

/* Render a caller-supplied palette instead of the ramps.
 *
 * [Chat] ProbePalette is a comma separated list of RRGGBB values; each is
 * drawn as its own hex label in its own colour, several to a line. This is how
 * a candidate server palette gets checked against the real renderer rather
 * than against a colour picker. Returns 0 if no palette is configured. */
static int probe_configured_palette(void)
{
    const char *spec = mw_cfg_str("ProbePalette", "");
    char  work[128];
    char  line[160];
    char *item, *next;
    int   used = 0, on_line = 0, any = 0;

    if (!spec[0])
        return 0;

    _snprintf(work, sizeof(work), "%s", spec);
    work[sizeof(work) - 1] = '\0';

    used = _snprintf(line, sizeof(line), "%s", "#cF8FCF8palette ");
    for (item = work; item && *item; item = next) {
        unsigned value;
        int n;

        next = strchr(item, ',');
        if (next)
            *next++ = '\0';
        while (*item == ' ')
            item++;
        if (!*item)
            continue;
        value = (unsigned)strtoul(item, NULL, 16) & 0xFFFFFFu;

        n = _snprintf(line + used, sizeof(line) - used - 1,
                      "#c%06X%06X ", value, value);
        if (n < 0)
            break;
        used += n;
        any = 1;

        if (++on_line >= 6) {
            mw_mirror_inject(CH_INFO, "probe", line);
            used = _snprintf(line, sizeof(line), "%s", "#cF8FCF8        ");
            on_line = 0;
        }
    }
    if (on_line)
        mw_mirror_inject(CH_INFO, "probe", line);

    if (any)
        mw_log("probe: rendered ProbePalette=%s", spec);
    return any;
}

/* Render arbitrary text through the client's own markup renderer.
 *
 * [Chat] ProbeText is injected verbatim, so a question like "does the chat
 * renderer swallow #L or print it?" is answered by looking rather than by
 * reading the dispatcher table. Semicolons separate lines, since commas and
 * most punctuation are worth being able to test. */
static int probe_configured_text(void)
{
    const char *spec = mw_cfg_str("ProbeText", "");
    char  work[256];
    char *item, *next;
    int   any = 0;

    if (!spec[0])
        return 0;

    _snprintf(work, sizeof(work), "%s", spec);
    work[sizeof(work) - 1] = '\0';

    for (item = work; item && *item; item = next) {
        next = strchr(item, ';');
        if (next)
            *next++ = '\0';
        if (!*item)
            continue;
        mw_mirror_inject(CH_INFO, "probe", item);
        any = 1;
    }
    if (any)
        mw_log("probe: rendered ProbeText=%s", spec);
    return any;
}

void mw_probe_colours(void)
{
    if (probe_configured_text())
        return;
    if (probe_configured_palette())
        return;

    /* Steps of 4 and 8 in one channel: at 5 bits per channel the 4-steps
     * collapse in pairs, at 6 bits they stay distinct. */
    static const unsigned reds[]   = { 0xFF0000, 0xFB0000, 0xF70000, 0xF30000,
                                       0xEF0000, 0xE70000, 0xDF0000 };
    static const unsigned greens[] = { 0x00FF00, 0x00FB00, 0x00F700, 0x00F300,
                                       0x00EF00, 0x00E700, 0x00DF00 };
    static const unsigned blues[]  = { 0x0000FF, 0x0000FB, 0x0000F7, 0x0000F3,
                                       0x0000EF, 0x0000E7, 0x0000DF };
    static const unsigned greys[]  = { 0xFFFFFF, 0xF7F7F7, 0xEFEFEF, 0xC0C0C0,
                                       0x808080, 0x404040, 0x202020 };
    /* If the field were a small palette index rather than packed RGB, even
     * these would collapse. */
    static const unsigned wide[]   = { 0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00,
                                       0xFF00FF, 0x00FFFF, 0xFF8000, 0x8000FF };

    mw_log("probe: injecting colour ramps into the window's private ring only");

    mw_mirror_inject(CH_INFO, "probe", "#cFFFFFF-- colour probe: identical-looking neighbours show the quantisation step --");
    ramp("#cFFFFFFR ", reds, (int)(sizeof(reds) / sizeof(reds[0])));
    ramp("#cFFFFFFG ", greens, (int)(sizeof(greens) / sizeof(greens[0])));
    ramp("#cFFFFFFB ", blues, (int)(sizeof(blues) / sizeof(blues[0])));
    ramp("#cFFFFFFK ", greys, (int)(sizeof(greys) / sizeof(greys[0])));
    ramp("#cFFFFFFW ", wide, (int)(sizeof(wide) / sizeof(wide[0])));

    /* The client's own named codes, for comparison against the hex path. */
    mw_mirror_inject(CH_INFO, "probe",
                     "named: #rred #ggreen #yyellow #bblue #wwhite #nnormal");
}
