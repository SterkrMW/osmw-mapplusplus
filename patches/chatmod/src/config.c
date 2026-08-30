/* Settings live in the game root, section [Chat], alongside the Maps++ addon
 * settings. We follow the same fallback chain the rest of the project uses and
 * that defaults.ini documents:
 *
 *     config.ini (the user's own edits)  ->  defaults.ini (shipped)  ->  the
 *     literal passed in at the read site
 *
 * Values are read on demand rather than cached, so the Maps++ settings page can
 * rewrite the file and we pick the change up without a reload. */
#include "osmw_chat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MW_SECTION "Chat"

/* A sentinel no real setting will ever equal, used to detect "key absent". */
#define MW_ABSENT "\x01__absent__"

static char g_ini[MAX_PATH];
static char g_defaults[MAX_PATH];
static char g_dir[MAX_PATH];

const char *mw_game_dir(void)
{
    return g_dir;
}

void mw_config_load(void)
{
    char *slash;

    /* config.ini sits next to main.exe, not next to this DLL */
    if (!GetModuleFileNameA(NULL, g_dir, sizeof(g_dir))) {
        g_dir[0] = '\0';
        g_ini[0] = '\0';
        return;
    }
    slash = strrchr(g_dir, '\\');
    if (slash)
        *slash = '\0';

    _snprintf(g_ini, sizeof(g_ini), "%s\\config.ini", g_dir);
    g_ini[sizeof(g_ini) - 1] = '\0';
    _snprintf(g_defaults, sizeof(g_defaults), "%s\\defaults.ini", g_dir);
    g_defaults[sizeof(g_defaults) - 1] = '\0';

    mw_log("config: %s", g_ini);
    mw_log("config: %s%s", g_defaults,
           GetFileAttributesA(g_defaults) == INVALID_FILE_ATTRIBUTES ? " (absent)" : "");
}

/* Read `key` from one ini, or return 0 if the key is not present there. */
static int read_from(const char *ini, const char *section, const char *key,
                     char *out, int size)
{
    if (!ini[0])
        return 0;
    GetPrivateProfileStringA(section, key, MW_ABSENT, out, size, ini);
    return strcmp(out, MW_ABSENT) != 0;
}

/* Same chain as mw_cfg_int, but for a section other than [Chat] -- the addon
 * on/off gate lives in [Addons] alongside the other Maps++ addons. */
int mw_cfg_int_in(const char *section, const char *key, int fallback)
{
    char buf[128];

    if (read_from(g_ini, section, key, buf, sizeof(buf)) ||
        read_from(g_defaults, section, key, buf, sizeof(buf))) {
        char *end = buf;
        long  v = strtol(buf, &end, 0);
        if (end != buf)
            return (int)v;
    }
    return fallback;
}

const char *mw_cfg_str(const char *key, const char *fallback)
{
    /* Small rotating set of buffers so a caller can hold a few values at once. */
    static char bufs[4][128];
    static int  next = 0;
    char *out = bufs[next];
    next = (next + 1) & 3;

    if (read_from(g_ini, MW_SECTION, key, out, sizeof(bufs[0])))
        return out;
    if (read_from(g_defaults, MW_SECTION, key, out, sizeof(bufs[0])))
        return out;

    _snprintf(out, sizeof(bufs[0]), "%s", fallback ? fallback : "");
    out[sizeof(bufs[0]) - 1] = '\0';
    return out;
}

/* Writes go to config.ini only. defaults.ini is shipped state and is never
 * edited by the app -- that is the contract its own header states. */
void mw_cfg_set_int(const char *key, int value)
{
    char buf[32];

    if (!g_ini[0])
        return;
    _snprintf(buf, sizeof(buf), "%d", value);
    buf[sizeof(buf) - 1] = '\0';
    if (!WritePrivateProfileStringA(MW_SECTION, key, buf, g_ini))
        mw_log("config: failed to write %s=%s (%lu)", key, buf, GetLastError());
}

int mw_cfg_int(const char *key, int fallback)
{
    return mw_cfg_int_in(MW_SECTION, key, fallback);
}
