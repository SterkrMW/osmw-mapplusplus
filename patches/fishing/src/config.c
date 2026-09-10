#include "osmw_fishing.h"

#include <stdio.h>

#define FG_SECTION "Fishing"

int fg_cfg_int_in(const char *section, const char *key, int fallback)
{
    const OSMW_CORE_API_V1 *api = fg_core_api();
    if (api == NULL || api->config_read_int == NULL)
        return fallback;
    return api->config_read_int(fg_core_owner(), section, key, fallback);
}

int fg_cfg_int(const char *key, int fallback)
{
    return fg_cfg_int_in(FG_SECTION, key, fallback);
}
