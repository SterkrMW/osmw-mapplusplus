/* Thin login-specific adapters over core's config and game-root services. */
#include "osmw_login.h"

const char *lg_game_dir(void)
{
    const OSMW_CORE_API_V1 *api = lg_core_api();
    return api != NULL && api->game_dir != NULL ? api->game_dir() : "";
}

void lg_config_load(void)
{
    /* Kept for source compatibility with the launcher-era implementation. */
}

int lg_cfg_int_in(const char *section, const char *key, int fallback)
{
    const OSMW_CORE_API_V1 *api = lg_core_api();
    if (api == NULL || api->config_read_int == NULL)
        return fallback;
    return api->config_read_int(lg_core_owner(), section, key, fallback);
}
