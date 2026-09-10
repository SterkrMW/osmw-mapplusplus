/* Thin settings-specific adapters over the core config service.
 *
 * Core preserves the existing config.ini -> defaults.ini -> literal fallback
 * chain and owns writes to config.ini.  Keeping these st_cfg_* names leaves
 * the panel/model code independent of the cross-DLL ABI details.
 */
#include "osmw_settings.h"

#include <stdio.h>

#define ST_SECTION "Settings"

int st_cfg_int_in(const char *section, const char *key, int fallback)
{
    const OSMW_CORE_API_V1 *api = st_core_api();
    if (api == NULL || api->config_read_int == NULL)
        return fallback;
    return api->config_read_int(st_core_owner(), section, key, fallback);
}

int st_cfg_int(const char *key, int fallback)
{
    return st_cfg_int_in(ST_SECTION, key, fallback);
}

int st_cfg_copy(const char *key, const char *fallback, char *out, int size)
{
    const OSMW_CORE_API_V1 *api = st_core_api();
    OSMW_RESULT result;

    if (out == NULL || size <= 0)
        return 0;
    out[0] = '\0';
    if (api == NULL || api->config_read_string == NULL)
        return 0;
    result = api->config_read_string(
        st_core_owner(), ST_SECTION, key, fallback, out, (DWORD)size);
    return result == OSMW_OK;
}

const char *st_cfg_str(const char *key, const char *fallback)
{
    static char buffers[4][256];
    static int next;
    char *output = buffers[next];
    next = (next + 1) & 3;
    (void)st_cfg_copy(key, fallback, output, sizeof(buffers[0]));
    return output;
}

void st_cfg_set_int_in(const char *section, const char *key, int value)
{
    const OSMW_CORE_API_V1 *api = st_core_api();
    char text[32];
    OSMW_RESULT result;

    if (api == NULL || api->config_write_string == NULL)
        return;
    _snprintf(text, sizeof(text), "%d", value);
    text[sizeof(text) - 1] = '\0';
    result = api->config_write_string(st_core_owner(), section, key, text);
    if (result != OSMW_OK)
        st_log("config: failed to write [%s] %s=%s (%ld)",
               section, key, text, (long)result);
}

void st_cfg_set_int(const char *key, int value)
{
    st_cfg_set_int_in(ST_SECTION, key, value);
}

void st_cfg_set_str(const char *key, const char *value)
{
    const OSMW_CORE_API_V1 *api = st_core_api();
    OSMW_RESULT result;

    if (api == NULL || api->config_write_string == NULL)
        return;
    result = api->config_write_string(
        st_core_owner(), ST_SECTION, key, value != NULL ? value : "");
    if (result != OSMW_OK)
        st_log("config: failed to write %s (%ld)", key, (long)result);
}
