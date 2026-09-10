/* Passive managed-module entry point and lifecycle. */
#include "osmw_fightspeed.h"

#define FS_REQUIRED_CAPABILITIES \
    (OSMW_CAP_CONFIG | OSMW_CAP_SIGNATURES | OSMW_CAP_REGION_CLAIMS)

static const OSMW_CORE_API_V1 *g_api;
static OSMW_OWNER g_owner = OSMW_INVALID_OWNER;

const OSMW_CORE_API_V1 *fs_core_api(void) { return g_api; }
OSMW_OWNER fs_core_owner(void) { return g_owner; }

static void clear_binding(void)
{
    g_owner = OSMW_INVALID_OWNER;
    g_api = NULL;
}

static OSMW_RESULT cleanup_resources(void)
{
    OSMW_RESULT result = fs_speed_remove();
    if (result != OSMW_OK)
        return result;
    fs_resolve_reset();
    clear_binding();
    return OSMW_OK;
}

static OSMW_RESULT rollback_init(OSMW_RESULT failure)
{
    OSMW_RESULT cleanup = cleanup_resources();
    return cleanup == OSMW_OK ? failure : cleanup;
}

static OSMW_RESULT __cdecl fightspeed_init(
    const OSMW_CORE_API_V1 *api, OSMW_OWNER owner)
{
    OSMW_RESULT result;
    if (api == NULL || owner == OSMW_INVALID_OWNER)
        return OSMW_E_INVALID_ARGUMENT;
    if (api->struct_size < OSMW_CORE_API_V1_0_SIZE ||
            OSMW_ABI_MAJOR(api->abi_version) !=
                OSMW_ABI_MAJOR(OSMW_CORE_ABI_V1_0))
        return OSMW_E_INCOMPATIBLE_ABI;
    if ((api->capabilities & FS_REQUIRED_CAPABILITIES) !=
            FS_REQUIRED_CAPABILITIES || api->log == NULL ||
            api->config_read_int == NULL || api->config_read_string == NULL ||
            api->config_write_string == NULL ||
            api->resolve_signature == NULL || api->claim_region == NULL ||
            api->release_region == NULL)
        return OSMW_E_UNAVAILABLE;
    if (g_api != NULL)
        return OSMW_E_BUSY;
    g_api = api;
    g_owner = owner;
    fs_log("osmw_fightspeed managed init");
    if (!fs_cfg_int_in("Addons", "FightSpeed", 1)) {
        fs_log("[Addons] FightSpeed=0 -- module disabled, client untouched");
        return OSMW_MOD_DISABLED;
    }
    result = fs_resolve_all();
    if (result != OSMW_OK)
        return rollback_init(result);
    if (!fs_speed_install()) {
        fs_log("install failed; rolling back all fight-speed claims");
        return rollback_init(OSMW_E_INTERNAL);
    }
    fs_log("startup complete: mode %s", fs_mode_name(fs_mode_get()));
    return OSMW_OK;
}

static OSMW_RESULT __cdecl fightspeed_shutdown(
    const OSMW_CORE_API_V1 *api, OSMW_OWNER owner)
{
    OSMW_RESULT result;
    if (g_api == NULL)
        return OSMW_OK;
    if (api != g_api)
        return OSMW_E_INVALID_ARGUMENT;
    if (owner != g_owner)
        return OSMW_E_WRONG_OWNER;
    result = cleanup_resources();
    if (result != OSMW_OK)
        fs_log("shutdown: resource cleanup failed (%ld)", (long)result);
    return result;
}

OSMW_RESULT __cdecl osmw_mod_query(
    DWORD core_abi_version, DWORD output_size,
    OSMW_MOD_DESCRIPTOR_V1 *output)
{
    if (OSMW_ABI_MAJOR(core_abi_version) !=
            OSMW_ABI_MAJOR(OSMW_CORE_ABI_V1_0))
        return OSMW_E_INCOMPATIBLE_ABI;
    if (output == NULL)
        return OSMW_E_INVALID_ARGUMENT;
    if (output_size < OSMW_MOD_DESCRIPTOR_V1_MIN_SIZE)
        return OSMW_E_BUFFER_TOO_SMALL;
    ZeroMemory(output, sizeof(*output));
    output->struct_size = sizeof(*output);
    output->required_core_abi = OSMW_CORE_ABI_V1_0;
    output->module_version = OSMW_MODULE_VERSION(1, 0);
    output->module_id = "fightspeed";
    output->display_name = "osMW Fight Speed";
    output->init = fightspeed_init;
    output->shutdown = fightspeed_shutdown;
    return OSMW_OK;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(instance);
    return TRUE;
}
