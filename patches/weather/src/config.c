/* Thin weather-specific adapters over core's config service. */
#include "osmw_weather.h"

#include <stdio.h>

#define WX_SECTION "Fight"

const char *wx_game_dir(void)
{
    const OSMW_CORE_API_V1 *api = wx_core_api();
    return api != NULL && api->game_dir != NULL ? api->game_dir() : "";
}

void wx_config_load(void) { }

int wx_cfg_int_in(const char *section, const char *key, int fallback)
{
    const OSMW_CORE_API_V1 *api = wx_core_api();
    if (api == NULL || api->config_read_int == NULL)
        return fallback;
    return api->config_read_int(wx_core_owner(), section, key, fallback);
}

int wx_cfg_int(const char *key, int fallback)
{
    return wx_cfg_int_in(WX_SECTION, key, fallback);
}

int wx_cfg_copy(const char *key, const char *fallback, char *out, int size)
{
    const OSMW_CORE_API_V1 *api = wx_core_api();
    OSMW_RESULT result;
    if (out == NULL || size <= 0 || api == NULL ||
            api->config_read_string == NULL)
        return 0;
    result = api->config_read_string(wx_core_owner(), WX_SECTION, key,
                                     fallback, out, (DWORD)size);
    return result == OSMW_OK;
}

void wx_cfg_set_int(const char *key, int value)
{
    const OSMW_CORE_API_V1 *api = wx_core_api();
    char text[32];
    OSMW_RESULT result;
    if (api == NULL || api->config_write_string == NULL)
        return;
    _snprintf(text, sizeof(text), "%d", value);
    text[sizeof(text) - 1] = '\0';
    result = api->config_write_string(wx_core_owner(), WX_SECTION, key, text);
    if (result != OSMW_OK)
        wx_log("config: failed to write %s=%s (%ld)", key, text, (long)result);
}
