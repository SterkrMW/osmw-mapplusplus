/* Passive managed-module entry point and lifecycle.
 *
 * The core owns UiTick.  Settings subscribes at priority zero, so its panel
 * work still runs after the client's original node walk without writing or
 * chaining the UiTick prologue itself.
 *
 * Spec: mw-tools/tools/client-dll/settings-panel.md
 */
#include "osmw_settings.h"

#include <string.h>

#define ST_REQUIRED_CAPABILITIES \
    (OSMW_CAP_CONFIG | OSMW_CAP_SIGNATURES | OSMW_CAP_UI_TICK | \
     OSMW_CAP_MODULE_STATUS)

static const OSMW_CORE_API_V1 *g_api;
static OSMW_OWNER g_owner = OSMW_INVALID_OWNER;
static OSMW_TOKEN g_tick_token = OSMW_INVALID_TOKEN;
static int g_panel_installed;
static int g_active;

const OSMW_CORE_API_V1 *st_core_api(void)
{
    return g_api;
}

OSMW_OWNER st_core_owner(void)
{
    return g_owner;
}

static void clear_binding(void)
{
    g_active = 0;
    g_panel_installed = 0;
    g_tick_token = OSMW_INVALID_TOKEN;
    g_owner = OSMW_INVALID_OWNER;
    g_api = NULL;
}

static void __cdecl settings_tick(
    OSMW_OWNER owner, void *context, LONG list_selector)
{
    (void)context;
    (void)list_selector;
    if (g_active && owner == g_owner)
        st_panel_tick();
}

static OSMW_RESULT __cdecl settings_init(
    const OSMW_CORE_API_V1 *api, OSMW_OWNER owner)
{
    OSMW_RESULT result;

    if (api == NULL || owner == OSMW_INVALID_OWNER)
        return OSMW_E_INVALID_ARGUMENT;
    if (api->struct_size < OSMW_CORE_API_V1_0_SIZE ||
            OSMW_ABI_MAJOR(api->abi_version) !=
                OSMW_ABI_MAJOR(OSMW_CORE_ABI_V1_0))
        return OSMW_E_INCOMPATIBLE_ABI;
    if ((api->capabilities & ST_REQUIRED_CAPABILITIES) !=
            ST_REQUIRED_CAPABILITIES ||
            api->log == NULL || api->config_read_int == NULL ||
            api->config_read_string == NULL ||
            api->config_write_string == NULL ||
            api->resolve_signature == NULL ||
            api->subscribe_post_ui_tick == NULL ||
            api->unsubscribe == NULL || api->module_status == NULL)
        return OSMW_E_UNAVAILABLE;
    if (g_api != NULL)
        return OSMW_E_BUSY;

    g_api = api;
    g_owner = owner;
    st_log("osmw_settings managed init");

    if (!st_cfg_int_in("Addons", "Settings", 1)) {
        st_log("[Addons] Settings=0 -- module disabled, client untouched");
        return OSMW_MOD_DISABLED;
    }

    result = st_resolve_all();
    if (result != OSMW_OK) {
        st_log("resolve: required signature failed (%ld); module standing down",
               (long)result);
        st_resolve_reset();
        clear_binding();
        return result;
    }

    if (!st_panel_install()) {
        st_log("panel: install failed; module standing down");
        st_resolve_reset();
        clear_binding();
        return OSMW_E_INTERNAL;
    }
    g_panel_installed = 1;

    result = api->subscribe_post_ui_tick(
        owner, OSMW_PRIORITY_DEFAULT, settings_tick, NULL, &g_tick_token);
    if (result != OSMW_OK) {
        st_log("UiTick: subscription failed (%ld); module standing down",
               (long)result);
        st_panel_remove();
        st_resolve_reset();
        clear_binding();
        return result;
    }

    g_active = 1;
    st_log("startup complete; priority-0 post-UiTick subscription active");
    return OSMW_OK;
}

static OSMW_RESULT __cdecl settings_shutdown(
    const OSMW_CORE_API_V1 *api, OSMW_OWNER owner)
{
    OSMW_RESULT result;

    if (g_api == NULL)
        return OSMW_OK;
    if (api != g_api)
        return OSMW_E_INVALID_ARGUMENT;
    if (owner != g_owner)
        return OSMW_E_WRONG_OWNER;

    if (g_tick_token != OSMW_INVALID_TOKEN) {
        result = api->unsubscribe(owner, g_tick_token);
        if (result != OSMW_OK) {
            st_log("shutdown: UiTick unsubscribe failed (%ld)", (long)result);
            return result;
        }
        g_tick_token = OSMW_INVALID_TOKEN;
    }
    g_active = 0;

    if (g_panel_installed) {
        st_panel_remove();
        g_panel_installed = 0;
    }
    st_log("shutdown complete; no live tokens or claims");
    st_resolve_reset();
    clear_binding();
    return OSMW_OK;
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
    output->module_id = "settings";
    output->display_name = "osMW Settings";
    output->init = settings_init;
    output->shutdown = settings_shutdown;
    return OSMW_OK;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(instance);
    return TRUE;
}
