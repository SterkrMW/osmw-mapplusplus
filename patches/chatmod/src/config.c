/* Chat-specific adapters over the core-owned config service. */
#include "osmw_chat.h"

#include <stdio.h>

#define MW_SECTION "Chat"

const char *mw_game_dir(void)
{
    const OSMW_CORE_API_V1 *api = mw_core_api();
    return api != NULL && api->game_dir != NULL ? api->game_dir() : "";
}

int mw_cfg_int_in(const char *section, const char *key, int fallback)
{
    const OSMW_CORE_API_V1 *api = mw_core_api();
    if (api == NULL || api->config_read_int == NULL)
        return fallback;
    return api->config_read_int(mw_core_owner(), section, key, fallback);
}

int mw_cfg_int(const char *key, int fallback)
{
    return mw_cfg_int_in(MW_SECTION, key, fallback);
}

int mw_cfg_copy(const char *key, const char *fallback, char *out, int size)
{
    const OSMW_CORE_API_V1 *api = mw_core_api();
    OSMW_RESULT result;

    if (out == NULL || size <= 0)
        return 0;
    out[0] = '\0';
    if (api == NULL || api->config_read_string == NULL)
        return 0;
    result = api->config_read_string(
        mw_core_owner(), MW_SECTION, key, fallback, out, (DWORD)size);
    return result == OSMW_OK;
}

const char *mw_cfg_str(const char *key, const char *fallback)
{
    static char buffers[4][256];
    static int next;
    char *output = buffers[next];
    next = (next + 1) & 3;
    (void)mw_cfg_copy(key, fallback, output, sizeof(buffers[0]));
    return output;
}

void mw_cfg_set_str(const char *key, const char *value)
{
    const OSMW_CORE_API_V1 *api = mw_core_api();
    OSMW_RESULT result;

    if (api == NULL || api->config_write_string == NULL)
        return;
    result = api->config_write_string(
        mw_core_owner(), MW_SECTION, key, value != NULL ? value : "");
    if (result != OSMW_OK)
        mw_log("config: failed to write %s (%ld)", key, (long)result);
}

void mw_cfg_set_int(const char *key, int value)
{
    char text[32];
    _snprintf(text, sizeof(text), "%d", value);
    text[sizeof(text) - 1] = '\0';
    mw_cfg_set_str(key, text);
}

void mw_cfg_clear(const char *key)
{
    /* ABI 1.0 has no delete operation. An empty explicit value makes all
     * integer geometry reads fall back to their call-site sentinel, which is
     * the behavior the old profile-key deletion provided for these keys. */
    mw_cfg_set_str(key, "");
}
