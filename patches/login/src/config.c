/* Settings live in the game root. Same fallback chain as the rest of the
 * project: config.ini -> defaults.ini -> the literal passed at the read site.
 */
#include "osmw_login.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LG_ABSENT "\x01__absent__"

static char g_ini[MAX_PATH];
static char g_defaults[MAX_PATH];
static char g_dir[MAX_PATH];

const char *lg_game_dir(void)
{
    return g_dir;
}

void lg_config_load(void)
{
    char *slash;

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

    lg_log("config: %s", g_ini);
    lg_log("config: %s%s", g_defaults,
           GetFileAttributesA(g_defaults) == INVALID_FILE_ATTRIBUTES ? " (absent)" : "");
}

static int read_from(const char *ini, const char *section, const char *key,
                     char *out, int size)
{
    if (!ini[0])
        return 0;
    GetPrivateProfileStringA(section, key, LG_ABSENT, out, size, ini);
    return strcmp(out, LG_ABSENT) != 0;
}

int lg_cfg_int_in(const char *section, const char *key, int fallback)
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
