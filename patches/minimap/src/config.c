/* Thin minimap-specific adapters over core-owned paths and config.
 *
 * Core preserves config.ini -> defaults.ini -> literal fallback. Minimap reads
 * settings on demand so edits made by the in-game settings panel remain live.
 */
#include "osmw_minimap.h"

#define MM_SECTION "Minimap"

const char *mm_game_dir(void)
{
    const OSMW_CORE_API_V1 *api = mm_core_api();
    if (api == NULL || api->game_dir == NULL)
        return "";
    return api->game_dir();
}

int mm_cfg_int_in(const char *section, const char *key, int fallback)
{
    const OSMW_CORE_API_V1 *api = mm_core_api();
    if (api == NULL || api->config_read_int == NULL)
        return fallback;
    return api->config_read_int(mm_core_owner(), section, key, fallback);
}

int mm_cfg_int(const char *key, int fallback)
{
    return mm_cfg_int_in(MM_SECTION, key, fallback);
}

/* Word-valued settings (currently only MarkerLabels) read through the same
 * on-demand path as the ints, so the settings panel can change them live. The
 * fallback is copied on any failure, so a caller never reads an uninitialised
 * buffer. */
void mm_cfg_string(const char *key, const char *fallback,
                   char *out, unsigned size)
{
    const OSMW_CORE_API_V1 *api = mm_core_api();

    if (out == NULL || size == 0)
        return;
    out[0] = '\0';
    if (api != NULL && api->config_read_string != NULL &&
        api->config_read_string(mm_core_owner(), MM_SECTION, key,
                                fallback, out, size) == OSMW_OK)
        return;

    if (fallback != NULL) {
        unsigned i = 0;
        while (i + 1 < size && fallback[i] != '\0') {
            out[i] = fallback[i];
            i++;
        }
        out[i] = '\0';
    }
}
