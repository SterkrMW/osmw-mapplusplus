/* Reading the client's keyboard, and turning it into edges.
 *
 * Copied from osmw_fightspeed's hotkey (speed.c), with the st_ prefix. The
 * process-global services around it are now supplied by core; this file keeps
 * only the settings-specific scancode interpretation and edge state.
 */
#include "osmw_settings.h"

#include <string.h>

/* Scancodes, not virtual keys: the client's key state is scancode-indexed and
 * the arrays are 0x59 entries, a range that ends exactly at F12.
 *
 * F12 is therefore the LAST key this table can ever hold. Widening it past the
 * function keys is not a bigger constant -- it needs the extended array too,
 * see below. */
static const struct { const char *name; int sc; } ST_KEYS[] = {
    {"F1", 0x3B}, {"F2", 0x3C}, {"F3", 0x3D}, {"F4",  0x3E},
    {"F5", 0x3F}, {"F6", 0x40}, {"F7", 0x41}, {"F8",  0x42},
    {"F9", 0x43}, {"F10", 0x44}, {"F11", 0x57}, {"F12", 0x58},
};
#define ST_KEY_COUNT ((int)(sizeof(ST_KEYS) / sizeof(ST_KEYS[0])))
#define ST_KEY_ARRAY_LEN 0x59

static const unsigned char *g_keys_plain;   /* 0x7E6BA0 */
static unsigned char g_was_down[ST_KEY_ARRAY_LEN];

int st_key_scancode_for(const char *name)
{
    int i;

    if (!name || !name[0])
        return 0;                  /* deliberately disabled */
    for (i = 0; i < ST_KEY_COUNT; i++) {
        if (_stricmp(name, ST_KEYS[i].name) == 0)
            return ST_KEYS[i].sc;
    }
    return -1;                     /* not a key this mod knows */
}

void st_key_init(void)
{
    g_keys_plain = (const unsigned char *)st_addr[ST_KEYARRAY];
    memset(g_was_down, 0, sizeof(g_was_down));
    if (!g_keys_plain)
        st_log("keys: the plain-scancode array did not resolve -- "
               "the panel cannot be opened by hotkey");
}

/* ONE array, not two.
 *
 * osmw_fightspeed's first build ORed 0x7E6BA0 and 0x7E6C00 on the assumption
 * that one was "held" and the other "pressed this frame". That was wrong: the
 * driver at 0x494F30 is a prefix state machine, and the two arrays are
 * non-extended vs EXTENDED (0xE0/0xE1-prefixed) scancodes. F1-F12 are plain,
 * so they only ever appear in 0x7E6BA0, and ORing the extended array in could
 * only ever contribute a false positive from an unrelated key that happens to
 * share the index. Both halves are proper make/break.
 *
 * Reading the client's array rather than GetAsyncKeyState is not a micro-
 * optimisation: it is what makes this focus-correct. Players run up to five
 * clients at once, GetAsyncKeyState is global, and one press would open the
 * panel in every one of them.
 */
static int key_down(int sc)
{
    if (sc <= 0 || sc >= ST_KEY_ARRAY_LEN)
        return 0;
    return g_keys_plain && g_keys_plain[sc];
}

int st_key_pressed(int sc)
{
    int down, was;

    if (sc <= 0 || sc >= ST_KEY_ARRAY_LEN)
        return 0;

    down = key_down(sc);
    was  = g_was_down[sc];
    g_was_down[sc] = (unsigned char)down;

    /* The edge, not the level. The array is held-state, so returning `down`
     * would toggle the panel every frame the key is held -- about ten times a
     * second, which reads as the panel refusing to open. */
    return down && !was;
}
