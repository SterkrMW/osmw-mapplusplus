/* Passive managed-module entry point and lifecycle.
 *
 * Core owns loading, configuration, logging, signature resolution and runtime
 * region arbitration. Login keeps its specialised two-prologue trampoline
 * hooks and its per-launch environment consent/roster-row selection.
 */
#include "osmw_login.h"

#define LG_REQUIRED_CAPABILITIES \
    (OSMW_CAP_CONFIG | OSMW_CAP_SIGNATURES | OSMW_CAP_REGION_CLAIMS)

/* Per-launch consent, set by whoever started this process. Persistent config
 * cannot express whether this particular launch asked for auto-login. */
#define LG_CONSENT_VAR "OSMW_AUTOLOGIN"
#define LG_ROW_VAR     "OSMW_LOGIN_ROW"

static const OSMW_CORE_API_V1 *g_api;
static OSMW_OWNER g_owner = OSMW_INVALID_OWNER;

const OSMW_CORE_API_V1 *lg_core_api(void)
{
    return g_api;
}

OSMW_OWNER lg_core_owner(void)
{
    return g_owner;
}

static void clear_binding(void)
{
    g_owner = OSMW_INVALID_OWNER;
    g_api = NULL;
}

static int lg_launch_consent(void)
{
    char buf[16];
    DWORD length = GetEnvironmentVariableA(LG_CONSENT_VAR, buf, sizeof(buf));

    if (length == 0 || length >= sizeof(buf))
        return 0;
    return buf[0] != '0' && buf[0] != '\0';
}

int lg_launch_row(char *output, int capacity)
{
    DWORD length;

    if (output == NULL || capacity <= 0)
        return 0;
    output[0] = '\0';
    length = GetEnvironmentVariableA(LG_ROW_VAR, output, (DWORD)capacity);
    if (length == 0 || length >= (DWORD)capacity) {
        output[0] = '\0';
        return 0;
    }
    return output[0] != '\0';
}

static OSMW_RESULT cleanup_resources(
    const OSMW_CORE_API_V1 *api, OSMW_OWNER owner)
{
    OSMW_RESULT result;

    (void)api;
    (void)owner;
    result = lg_autologin_stop();
    if (result != OSMW_OK)
        return result;
    lg_resolve_reset();
    clear_binding();
    return OSMW_OK;
}

static OSMW_RESULT rollback_init(OSMW_RESULT failure)
{
    OSMW_RESULT cleanup = cleanup_resources(g_api, g_owner);
    return cleanup == OSMW_OK ? failure : cleanup;
}

static OSMW_RESULT __cdecl login_init(
    const OSMW_CORE_API_V1 *api, OSMW_OWNER owner)
{
    OSMW_RESULT result;

    if (api == NULL || owner == OSMW_INVALID_OWNER)
        return OSMW_E_INVALID_ARGUMENT;
    if (api->struct_size < OSMW_CORE_API_V1_0_SIZE ||
            OSMW_ABI_MAJOR(api->abi_version) !=
                OSMW_ABI_MAJOR(OSMW_CORE_ABI_V1_0))
        return OSMW_E_INCOMPATIBLE_ABI;
    if ((api->capabilities & LG_REQUIRED_CAPABILITIES) !=
            LG_REQUIRED_CAPABILITIES ||
            api->game_dir == NULL || api->log == NULL ||
            api->config_read_int == NULL ||
            api->resolve_signature == NULL ||
            api->claim_region == NULL || api->release_region == NULL)
        return OSMW_E_UNAVAILABLE;
    if (g_api != NULL)
        return OSMW_E_BUSY;

    g_api = api;
    g_owner = owner;
    lg_log("osmw_login managed init");

    if (!lg_cfg_int_in("Addons", "AutoLogin", 1)) {
        lg_log("[Addons] AutoLogin=0 -- module disabled, client untouched");
        return OSMW_MOD_DISABLED;
    }
    if (!lg_launch_consent()) {
        lg_log("%s not set -- this launch did not request auto-login", LG_CONSENT_VAR);
        return OSMW_MOD_DISABLED;
    }

    result = lg_resolve_all();
    if (result != OSMW_OK) {
        lg_log("resolve: required signature failed (%ld); module standing down",
               (long)result);
        return rollback_init(result);
    }

    if (!lg_autologin_start()) {
        lg_log("autologin: start failed; rolling back");
        return rollback_init(OSMW_E_INTERNAL);
    }

    lg_log("startup complete; 10 private signatures and up to two claimed prologues");
    return OSMW_OK;
}

static OSMW_RESULT __cdecl login_shutdown(
    const OSMW_CORE_API_V1 *api, OSMW_OWNER owner)
{
    OSMW_RESULT result;

    if (g_api == NULL)
        return OSMW_OK;
    if (api != g_api)
        return OSMW_E_INVALID_ARGUMENT;
    if (owner != g_owner)
        return OSMW_E_WRONG_OWNER;

    result = cleanup_resources(api, owner);
    if (result != OSMW_OK)
        lg_log("shutdown: resource cleanup failed (%ld)", (long)result);
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
    output->module_id = "login";
    output->display_name = "osMW Auto Login";
    output->init = login_init;
    output->shutdown = login_shutdown;
    return OSMW_OK;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(instance);
    return TRUE;
}
