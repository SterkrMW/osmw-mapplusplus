/* Reading the keyboard, and turning it into focus-local edges.
 *
 * Copied from osmw_fightspeed's hotkey (speed.c), with the tm_ prefix. Core now
 * supplies the process-global config, log, scan, lifecycle and shared-hook
 * services; this file retains only the timer panel's key/edge behavior.
 */
#include "osmw_timers.h"

#include <string.h>

/* Scancodes, not virtual keys: the client's key state is scancode-indexed and
 * the arrays are 0x59 entries, a range that ends exactly at F12.
 *
 * F12 is therefore the LAST key this table can ever hold. Widening it past the
 * function keys is not a bigger constant -- it needs the extended array too,
 * see below. */
static const struct { const char *name; int sc; } TM_KEYS[] = {
    {"F1", 0x3B}, {"F2", 0x3C}, {"F3", 0x3D}, {"F4",  0x3E},
    {"F5", 0x3F}, {"F6", 0x40}, {"F7", 0x41}, {"F8",  0x42},
    {"F9", 0x43}, {"F10", 0x44}, {"F11", 0x57}, {"F12", 0x58},
};
#define TM_KEY_COUNT ((int)(sizeof(TM_KEYS) / sizeof(TM_KEYS[0])))
#define TM_KEY_ARRAY_LEN 0x59

static const unsigned char *g_keys_plain;   /* 0x7E6BA0, emergency fallback */
static unsigned char g_was_down[TM_KEY_ARRAY_LEN];

int tm_key_scancode_for(const char *name)
{
    int i;

    if (!name || !name[0])
        return 0;                  /* deliberately disabled */
    for (i = 0; i < TM_KEY_COUNT; i++) {
        if (_stricmp(name, TM_KEYS[i].name) == 0)
            return TM_KEYS[i].sc;
    }
    return -1;                     /* not a key this mod knows */
}

void tm_key_init(void)
{
    g_keys_plain = (const unsigned char *)tm_addr[TM_KEYARRAY];
    memset(g_was_down, 0, sizeof(g_was_down));
    if (!g_keys_plain)
        tm_log("keys: the plain-scancode array did not resolve -- "
               "the panel cannot be opened by hotkey");
}

/* Prefer the physical key state, but only while a window owned by this client
 * process is foreground.
 *
 * The first live timer-panel trial showed why the client array is insufficient
 * here: after the DLL-owned UI node opened, its F9 entry did not reliably
 * observe the break event, so g_was_down never re-armed and a second press
 * could not close the panel. GetAsyncKeyState observes the real release. It is
 * system-wide, so using it by itself would toggle every multi-boxed client;
 * the foreground HWND's owning PID is therefore a mandatory gate. A chat or
 * child window from the same process still counts, while every other client
 * process reads up.
 */
static int process_is_foreground(void)
{
    HWND foreground = GetForegroundWindow();
    DWORD process_id = 0;

    if (!foreground)
        return 0;
    GetWindowThreadProcessId(foreground, &process_id);
    return process_id == GetCurrentProcessId();
}

static int key_down(int sc)
{
    UINT virtual_key;

    if (sc <= 0 || sc >= TM_KEY_ARRAY_LEN)
        return 0;
    if (!process_is_foreground())
        return 0;

    virtual_key = MapVirtualKeyA((UINT)sc, MAPVK_VSC_TO_VK);
    if (virtual_key)
        return (GetAsyncKeyState((int)virtual_key) & 0x8000) != 0;

    /* F1-F12 always map, but retain the signature-resolved client array as a
     * fail-closed fallback if an unusual keyboard layout returns no VK. */
    return g_keys_plain && g_keys_plain[sc];
}

int tm_key_pressed(int sc)
{
    int down, was;

    if (sc <= 0 || sc >= TM_KEY_ARRAY_LEN)
        return 0;

    down = key_down(sc);
    was  = g_was_down[sc];
    g_was_down[sc] = (unsigned char)down;

    /* The edge, not the level. The array is held-state, so returning `down`
     * would toggle the panel every frame the key is held -- about ten times a
     * second, which reads as the panel refusing to open. */
    return down && !was;
}
