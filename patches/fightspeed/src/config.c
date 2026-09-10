/* Thin fight-speed adapters over core's config service. */
#include "osmw_fightspeed.h"
#include <stdio.h>

#define FS_SECTION "Fight"

const char *fs_game_dir(void)
{
    const OSMW_CORE_API_V1 *api = fs_core_api();
    return api != NULL && api->game_dir != NULL ? api->game_dir() : "";
}
void fs_config_load(void) { }

int fs_cfg_int_in(const char *section, const char *key, int fallback)
{
    const OSMW_CORE_API_V1 *api = fs_core_api();
    if (api == NULL || api->config_read_int == NULL) return fallback;
    return api->config_read_int(fs_core_owner(), section, key, fallback);
}
int fs_cfg_int(const char *key, int fallback)
{ return fs_cfg_int_in(FS_SECTION, key, fallback); }

int fs_cfg_copy(const char *key, const char *fallback, char *out, int size)
{
    const OSMW_CORE_API_V1 *api = fs_core_api();
    OSMW_RESULT result;
    if (!out || size <= 0 || !api || !api->config_read_string) return 0;
    result = api->config_read_string(fs_core_owner(), FS_SECTION, key,
                                     fallback, out, (DWORD)size);
    return result == OSMW_OK;
}

void fs_cfg_set_int(const char *key, int value)
{
    const OSMW_CORE_API_V1 *api = fs_core_api();
    char text[32];
    OSMW_RESULT result;
    if (!api || !api->config_write_string) return;
    _snprintf(text, sizeof(text), "%d", value);
    text[sizeof(text) - 1] = '\0';
    result = api->config_write_string(fs_core_owner(), FS_SECTION, key, text);
    if (result != OSMW_OK)
        fs_log("config: failed to write %s=%s (%ld)", key, text, (long)result);
}
