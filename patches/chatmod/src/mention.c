/* Highlight messages that mention you.
 *
 * A message record is 0xC0 bytes:
 *
 *   +0x00  sender id
 *   +0x04  channel
 *   +0x08  sender name, 21 bytes
 *   +0x1D  message text, 163 bytes
 *
 * Your own character name is not stored anywhere obvious, but the client puts
 * it in the main window's caption:
 *
 *   MythWar osMW Patch ( ENGLISH version 1.0.48 - 3762 ) [ Behemoth: Borgmin ID 6911 ]
 *
 * so it is read from there, between the last colon and " ID ". [Chat] MyName
 * overrides that if the caption format ever changes.
 *
 * Marking happens on the private mirror, never on the client's own ring, so a
 * decorated message stays decorated only for the window that shows it.
 */
#include "osmw_chat.h"

#include <stdio.h>
#include <string.h>

#define MSG_CHANNEL 0x04
#define MSG_NAME    0x08
#define MSG_TEXT    0x1D
#define MSG_STRIDE  0xC0
#define MSG_TEXT_MAX (MSG_STRIDE - MSG_TEXT)    /* 163, including the NUL */

#define CH_PM 2

static char g_name[64];
static char g_prefix[24];
static int  g_enabled = 1;

const char *mw_mention_name(void)
{
    return g_name;
}

/* Pull the character name out of the main window caption. */
static int name_from_caption(char *out, int size)
{
    HWND main_wnd = mw_addr[MW_MAINHWND] ? *(HWND *)mw_addr[MW_MAINHWND] : NULL;
    char title[256];
    const char *id, *colon, *p;
    int len;

    if (!main_wnd || !IsWindow(main_wnd))
        return 0;
    if (!GetWindowTextA(main_wnd, title, sizeof(title) - 1))
        return 0;

    id = strstr(title, " ID ");
    if (!id)
        return 0;

    colon = NULL;
    for (p = title; p < id; p++) {
        if (*p == ':')
            colon = p;
    }
    if (!colon)
        return 0;

    colon++;
    while (*colon == ' ')
        colon++;
    len = (int)(id - colon);
    while (len > 0 && colon[len - 1] == ' ')
        len--;
    if (len <= 0 || len >= size)
        return 0;

    memcpy(out, colon, len);
    out[len] = '\0';
    return 1;
}

/* Re-read settings and, if we do not have a name yet, try the caption again.
 * The caption only carries the character name once you are in the world, so
 * this keeps trying rather than giving up at startup. */
void mw_mention_refresh(void)
{
    const char *configured = mw_cfg_str("MyName", "");
    char found[64];

    g_enabled = mw_cfg_int("HighlightMentions", 1) != 0;
    _snprintf(g_prefix, sizeof(g_prefix), "%s",
              mw_cfg_str("MentionPrefix", "#R>#N "));
    g_prefix[sizeof(g_prefix) - 1] = '\0';

    if (configured[0]) {
        if (strcmp(g_name, configured) != 0) {
            _snprintf(g_name, sizeof(g_name), "%s", configured);
            g_name[sizeof(g_name) - 1] = '\0';
            mw_log("mention: watching for \"%s\" (from MyName)", g_name);
        }
        return;
    }

    if (name_from_caption(found, sizeof(found)) &&
        strcmp(found, g_name) != 0) {
        _snprintf(g_name, sizeof(g_name), "%s", found);
        g_name[sizeof(g_name) - 1] = '\0';
        mw_log("mention: watching for \"%s\" (from the window caption)", g_name);
    }
}

static int contains_ci(const char *haystack, const char *needle)
{
    size_t n = strlen(needle);
    if (!n)
        return 0;
    for (; *haystack; haystack++) {
        if (_strnicmp(haystack, needle, n) == 0)
            return 1;
    }
    return 0;
}

int mw_mention_matches(const unsigned char *rec)
{
    unsigned channel;

    if (!g_enabled || !g_name[0] || !rec)
        return 0;

    /* Anything sent directly to you counts, whether or not it says your name. */
    channel = *(const unsigned *)(rec + MSG_CHANNEL);
    if (channel == CH_PM)
        return 1;

    /* Only the message body: matching the sender field would light up on
     * every line you sent yourself. */
    return contains_ci((const char *)(rec + MSG_TEXT), g_name);
}

/* Prefix the marker onto a mirrored record's text, in place and bounded. */
void mw_mention_decorate(unsigned char *rec)
{
    char  *text = (char *)(rec + MSG_TEXT);
    size_t plen = strlen(g_prefix);
    size_t tlen;

    if (!g_enabled || !plen || !mw_mention_matches(rec))
        return;

    /* Already marked? The mirror is rebuilt from the untouched ring, so this
     * only guards against a rebuild racing an append. */
    if (strncmp(text, g_prefix, plen) == 0)
        return;

    tlen = strlen(text);
    if (plen + tlen + 1 > MSG_TEXT_MAX)
        return;                         /* no room; leave it undecorated */

    memmove(text + plen, text, tlen + 1);
    memcpy(text, g_prefix, plen);
}
