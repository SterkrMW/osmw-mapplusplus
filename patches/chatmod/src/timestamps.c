/* Inline arrival times on chat messages.
 *
 * The client stores no time on a message -- a record is sender id, channel,
 * sender name, text, and nothing else -- so the time has to be observed as the
 * message arrives and remembered on the side.
 *
 * WHERE THE TIME IS KEPT, and why it is not simply stamped at copy time.
 *
 * The mirror is rebuilt from the client's ring on every filter change, so a
 * time taken when a record is copied would be the time you last clicked a tab,
 * on every visible line at once. Instead the arrival time is recorded against
 * the REAL ring's slot index, which is stable for as long as the message lives,
 * and looked up again on each rebuild. Switching tabs then leaves the times
 * exactly as they were.
 *
 * Messages already in the ring when the mod is injected have no observed time
 * and are left unstamped rather than given a made-up one. On the normal launch
 * path this never arises: injection happens during the client's startup init,
 * long before any chat exists. It only shows up when injecting into a client
 * that is already running, which is a development path.
 *
 * The stamp is prefixed to the message TEXT, which is where the mod already
 * decorates (see mention.c). That puts it after the sender name the client
 * draws from a separate field, so a line reads
 *
 *     [World] Rajesh: 12:34 selling helm
 *
 * rather than leading with the time. Prefixing the name field instead is not
 * possible: it is 21 bytes and most names do not leave room.
 */
#include "osmw_chat.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ChatMod_LineCompose pins the run-up to the composing sprintf; the call is the
 * last instruction the signature covers, 25 bytes in. Held here rather than in
 * the signature because signatures.ini resolves addresses, not call sites --
 * the same arrangement font.c uses for the CreateFontA sites. */
#define COMPOSE_CALL_DELTA 25

#define MSG_STRIDE   0xC0
#define MSG_TEXT     0x1D
#define MSG_TEXT_MAX (MSG_STRIDE - MSG_TEXT)    /* 163, including the NUL */
#define MSG_SLOTS    1024

#define UNKNOWN 0xFFFF

/* Minutes since midnight, and seconds, per REAL ring slot. Two small arrays
 * rather than preformatted text so the format can change without the already
 * recorded times having to be thrown away. */
static unsigned short g_minute[MSG_SLOTS];
static unsigned char  g_second[MSG_SLOTS];

static int  g_enabled;
static char g_format[32];

/* Counted so a rebuild can report how many records it could actually stamp.
 * "No time" is the expected state for anything that predates injection, so
 * without this the difference between that and a broken key is invisible.
 *
 * Counted as the mirror takes records in, NOT where the stamp is written: the
 * composer runs per line per frame, so counting there would report hundreds a
 * second and mean nothing. */
static int g_stamped;
static int g_no_time;

void mw_time_note(int real_slot)
{
    if (real_slot < 0 || real_slot >= MSG_SLOTS || g_minute[real_slot] == UNKNOWN)
        g_no_time++;
    else
        g_stamped++;
}

void mw_time_stats(int *stamped, int *no_time)
{
    *stamped = g_stamped;
    *no_time = g_no_time;
    g_stamped = 0;
    g_no_time = 0;
}

void mw_time_reset(void)
{
    int i;
    for (i = 0; i < MSG_SLOTS; i++)
        g_minute[i] = UNKNOWN;
}

static void refresh(void)
{
    g_enabled = mw_cfg_int("Timestamps", 0) != 0;
    /* #c808080 rather than one of the letter codes: #K renders identical to
     * plain white, measured against the real renderer, so it would have cost
     * four characters of the line budget for nothing. */
    _snprintf(g_format, sizeof(g_format), "%s",
              mw_cfg_str("TimestampFormat", "#c808080[%H:%M]#N"));
    g_format[sizeof(g_format) - 1] = '\0';
}

/* Re-read the settings, and rebuild the visible ring if they changed.
 *
 * The rebuild is what makes the toggle apply to history rather than only to
 * messages that arrive afterwards: it re-copies every kept record from the
 * client's untouched ring and re-decorates it, so stamps appear on, or vanish
 * from, everything on screen at once. */
void mw_time_poll(void)
{
    static int ready;
    int        was = g_enabled;
    char       previous[sizeof(g_format)];

    memcpy(previous, g_format, sizeof(previous));
    refresh();

    if (!ready) {
        ready = 1;                      /* first read is not a change */
        return;
    }
    if (g_enabled == was && strcmp(g_format, previous) == 0)
        return;

    mw_log("timestamps: %s, format \"%s\"", g_enabled ? "on" : "off", g_format);
    mw_mirror_rebuild(mw_tabs_mask());
}

int mw_time_enabled(void)
{
    return g_enabled;
}

/* Note when a slot in the client's ring was filled. Called from the mirror as
 * it consumes new arrivals, whether or not the message survives the filter and
 * whether or not timestamps are currently switched on -- so turning them on
 * shows correct times for everything already on screen, instead of starting
 * from blank and filling in over the next few minutes. */
void mw_time_observe(int real_slot)
{
    SYSTEMTIME st;

    if (real_slot < 0 || real_slot >= MSG_SLOTS)
        return;
    GetLocalTime(&st);
    g_minute[real_slot] = (unsigned short)(st.wHour * 60 + st.wMinute);
    g_second[real_slot] = (unsigned char)st.wSecond;
}

/* Expand the format, substituting %H %M %S. Everything else is copied through
 * verbatim, including the client's own #-colour markup.
 *
 * Deliberately not a printf: the format string comes from config.ini, and
 * handing user text to a %-formatter with a mismatched argument list is a way
 * to crash the client from an ini file. */
static int expand(char *out, int size, int minute, int second)
{
    const char *f = g_format;
    int n = 0;

    while (*f && n < size - 1) {
        if (f[0] == '%' && f[1]) {
            int value = -1;
            switch (f[1]) {
            case 'H': value = minute / 60; break;
            case 'M': value = minute % 60; break;
            case 'S': value = second;      break;
            case '%': break;
            default:  break;
            }
            if (value >= 0) {
                if (n + 2 >= size - 1)
                    break;
                out[n++] = (char)('0' + (value / 10) % 10);
                out[n++] = (char)('0' + value % 10);
                f += 2;
                continue;
            }
            if (f[1] == '%') {
                out[n++] = '%';
                f += 2;
                continue;
            }
            /* An unknown %x is passed through as written, so a typo in the ini
             * is visible in the chat rather than silently swallowed. */
        }
        out[n++] = *f++;
    }

    /* Always separate the stamp from the message, and do it here rather than
     * asking for a trailing space in the ini: GetPrivateProfileString strips
     * trailing whitespace, so one written there never survives the read. */
    if (n && out[n - 1] != ' ' && n < size - 1)
        out[n++] = ' ';

    out[n] = '\0';
    return n;
}

/* ---- the line composer hook --------------------------------------------
 *
 * Every visible line is composed by ONE sprintf, and every channel reaches it:
 *
 *     "#C00FFFF%s: #N%s"   name, text    Local Party Guild Market World FD GM
 *     "%s%s"               prefix, text  System, Info, channel 99, and PM
 *                                        (PM's prefix is "You whisper to ...")
 *
 * Composing is the right place to put the stamp, for three reasons that the
 * two obvious shortcuts each get wrong:
 *
 *   - Prefixing the record's NAME field would break click-to-PM. The hit test
 *     reads that field to build the whisper, so the target would become a
 *     player called "12:43 Admin".
 *   - Prefixing at DRAW time would overflow the right edge, because the line
 *     was already wrapped to a width measured without the stamp.
 *   - Here, the line is wrapped afterwards, so wrapping stays correct; the
 *     record is untouched, so the hit test is unaffected; and the destination
 *     is the ~1KB line buffer rather than the record's 163 bytes, so long
 *     messages get a stamp too.
 *
 * In BOTH formats the last argument is the message text, pointing into our
 * mirrored ring, which is how the record -- and so its observed time -- is
 * recovered without needing the client's registers.
 */
static mw_hook_t g_compose_hook;
static int (__cdecl *g_real_sprintf)(char *, const char *, ...);
static int g_compose_hooked;

static int slot_from_text_pointer(const char *text)
{
    const unsigned char *base = mw_mirror_base();
    size_t off;

    if (!base || !text)
        return -1;
    if ((const unsigned char *)text < base)
        return -1;
    off = (size_t)((const unsigned char *)text - base);
    if (off >= (size_t)MSG_SLOTS * MSG_STRIDE)
        return -1;
    /* It must be exactly a record's text field, not merely inside the ring. */
    if (off % MSG_STRIDE != MSG_TEXT)
        return -1;
    return (int)(off / MSG_STRIDE);
}

static int __cdecl compose_detour(char *dest, const char *fmt, ...)
{
    va_list ap;
    const char *a1, *a2;
    int slot, real_slot, n = 0;

    va_start(ap, fmt);
    a1 = va_arg(ap, const char *);
    a2 = va_arg(ap, const char *);
    va_end(ap);

    if (g_enabled && dest) {
        slot = slot_from_text_pointer(a2);
        real_slot = (slot >= 0) ? mw_mirror_real_slot(slot) : -1;
        if (real_slot >= 0 && real_slot < MSG_SLOTS &&
            g_minute[real_slot] != UNKNOWN) {
            char stamp[48];
            int  len = expand(stamp, sizeof(stamp), g_minute[real_slot],
                              g_second[real_slot]);
            if (len > 0) {
                memcpy(dest, stamp, (size_t)len);
                n = len;
            }
        }
    }

    /* Chain to the real sprintf, which appends the client's own line after
     * whatever stamp we just wrote. Both formats take exactly two strings. */
    return n + g_real_sprintf(dest + n, fmt, a1, a2);
}

int mw_time_install(void)
{
    void *site = mw_addr[MW_LINECOMPOSE];
    void *original = NULL;

    if (g_compose_hooked)
        return 1;
    if (!site) {
        mw_log("timestamps: line composer unresolved; "
               "falling back to stamping the message text");
        return 0;
    }

    /* The signature pins the run-up to the call; the call itself is the last
     * instruction it covers. */
    site = (unsigned char *)site + COMPOSE_CALL_DELTA;

    /* Function-to-void* goes through UINT_PTR, as font.c does at its own call
     * sites: same width on Win32, but the direct conversion is not standard C. */
    if (!mw_patch_direct_call(&g_compose_hook, site,
                              (void *)(UINT_PTR)compose_detour, &original,
                              "chat line-composer call")) {
        mw_log("timestamps: could not patch the line composer; "
               "falling back to stamping the message text");
        return 0;
    }

    /* A void* round trip: ISO C keeps function and object pointers distinct,
     * and MSVC warns at /W4 about assigning one to the other directly. The
     * conversion is well defined on Win32, where both are a flat 32-bit
     * address, and going through memcpy states that deliberately. */
    memcpy(&g_real_sprintf, &original, sizeof(g_real_sprintf));

    g_compose_hooked = 1;
    mw_log("timestamps: line composer hooked at %p", site);
    return 1;
}

OSMW_RESULT mw_time_remove(void)
{
    OSMW_RESULT result = mw_hook_remove(&g_compose_hook);
    if (result != OSMW_OK)
        return result;
    g_compose_hooked = 0;
    return OSMW_OK;
}

/* Fallback only. Prefixes the stamp onto a MIRRORED record's text, which puts
 * it after the sender name -- correct but ragged, and limited to the record's
 * 163 bytes. Used only when the composer hook could not be installed. */
void mw_time_decorate(unsigned char *rec, int real_slot)
{
    char   stamp[48];
    char  *text = (char *)(rec + MSG_TEXT);
    size_t slen, tlen;

    if (g_compose_hooked)
        return;
    if (!g_enabled || real_slot < 0 || real_slot >= MSG_SLOTS)
        return;
    if (g_minute[real_slot] == UNKNOWN)
        return;                         /* arrived before we were watching */

    slen = (size_t)expand(stamp, sizeof(stamp), g_minute[real_slot],
                          g_second[real_slot]);
    if (!slen)
        return;

    tlen = strlen(text);
    if (slen + tlen + 1 > MSG_TEXT_MAX)
        return;                         /* no room; leave it unstamped */

    memmove(text + slen, text, tlen + 1);
    memcpy(text, stamp, slen);
}
