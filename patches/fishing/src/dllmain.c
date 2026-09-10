/* Passive ABI 1 lifecycle for the graphical fishing panel.
 *
 * Core owns UiTick and HandlePacket. This module claims 0x7F000041
 * exclusively, observes MapData, and creates its node only after UiTick.
 */
#include "osmw_fishing.h"

#include <string.h>

#define FG_REQUIRED_CAPABILITIES \
    (OSMW_CAP_CONFIG | OSMW_CAP_SIGNATURES | OSMW_CAP_UI_TICK | \
     OSMW_CAP_GAME_PACKETS | OSMW_CAP_SEND_QUEUE)

static const OSMW_CORE_API_V1 *g_api;
static OSMW_OWNER g_owner = OSMW_INVALID_OWNER;
static OSMW_TOKEN g_snapshot_token = OSMW_INVALID_TOKEN;
static OSMW_TOKEN g_map_token = OSMW_INVALID_TOKEN;
static OSMW_TOKEN g_tick_token = OSMW_INVALID_TOKEN;
static int g_panel_installed;
static int g_active;

const OSMW_CORE_API_V1 *fg_core_api(void)
{
    return g_api;
}

OSMW_OWNER fg_core_owner(void)
{
    return g_owner;
}

static void clear_binding(void)
{
    g_active = 0;
    g_panel_installed = 0;
    g_tick_token = OSMW_INVALID_TOKEN;
    g_map_token = OSMW_INVALID_TOKEN;
    g_snapshot_token = OSMW_INVALID_TOKEN;
    g_owner = OSMW_INVALID_OWNER;
    g_api = NULL;
}

static void __cdecl fishing_tick_callback(
    OSMW_OWNER owner, void *context, LONG list_selector)
{
    (void)context;
    (void)list_selector;
    if (!g_active || owner != g_owner)
        return;
    fg_fishing_tick();
    fg_panel_tick();
}

static OSMW_PACKET_ACTION __cdecl snapshot_handler(
    OSMW_OWNER owner, void *context, const OSMW_GAME_PACKET_V1 *packet)
{
    (void)context;
    if (g_active && owner == g_owner && packet != NULL &&
            packet->struct_size >= sizeof(*packet) &&
            packet->type == FG_PACKET_SNAPSHOT) {
        fg_fishing_accept_snapshot(
            packet->field12, packet->extra, packet->extra_size);
    }
    return OSMW_PACKET_CONSUME;
}

static void __cdecl map_data_observer(
    OSMW_OWNER owner, void *context, const OSMW_GAME_PACKET_V1 *packet)
{
    (void)context;
    if (g_active && owner == g_owner && packet != NULL &&
            packet->struct_size >= sizeof(*packet) &&
            packet->type == FG_PACKET_MAP_DATA)
        fg_fishing_on_map_data();
}

static OSMW_RESULT release_token(OSMW_TOKEN *token)
{
    OSMW_RESULT result;

    if (*token == OSMW_INVALID_TOKEN)
        return OSMW_OK;
    result = g_api->unsubscribe(g_owner, *token);
    if (result == OSMW_OK)
        *token = OSMW_INVALID_TOKEN;
    return result;
}

static OSMW_RESULT release_tokens(void)
{
    OSMW_RESULT first = OSMW_OK;
    OSMW_RESULT result;

    result = release_token(&g_tick_token);
    if (first == OSMW_OK && result != OSMW_OK)
        first = result;
    result = release_token(&g_map_token);
    if (first == OSMW_OK && result != OSMW_OK)
        first = result;
    result = release_token(&g_snapshot_token);
    if (first == OSMW_OK && result != OSMW_OK)
        first = result;
    return first;
}

static OSMW_RESULT rollback_init(OSMW_RESULT failure)
{
    OSMW_RESULT cleanup = release_tokens();

    if (cleanup != OSMW_OK) {
        fg_log("rollback: token release failed (%ld)", (long)cleanup);
        return cleanup;
    }
    if (g_panel_installed) {
        fg_panel_remove();
        g_panel_installed = 0;
    }
    fg_fishing_reset();
    fg_resolve_reset();
    clear_binding();
    return failure;
}

static OSMW_RESULT __cdecl fishing_init(
    const OSMW_CORE_API_V1 *api, OSMW_OWNER owner)
{
    OSMW_RESULT result;

    if (api == NULL || owner == OSMW_INVALID_OWNER)
        return OSMW_E_INVALID_ARGUMENT;
    if (api->struct_size < OSMW_CORE_API_V1_0_SIZE ||
            OSMW_ABI_MAJOR(api->abi_version) !=
                OSMW_ABI_MAJOR(OSMW_CORE_ABI_V1_0))
        return OSMW_E_INCOMPATIBLE_ABI;
    if ((api->capabilities & FG_REQUIRED_CAPABILITIES) !=
            FG_REQUIRED_CAPABILITIES || api->log == NULL ||
            api->config_read_int == NULL ||
            api->resolve_signature == NULL ||
            api->subscribe_post_ui_tick == NULL ||
            api->unsubscribe == NULL || api->claim_game_packet == NULL ||
            api->observe_game_packet_after == NULL ||
            api->queue_game_packet == NULL)
        return OSMW_E_UNAVAILABLE;
    if (g_api != NULL)
        return OSMW_E_BUSY;

    g_api = api;
    g_owner = owner;
    fg_log("osmw_fishing managed init");
    if (!fg_cfg_int_in("Addons", "Fishing", 1)) {
        fg_log("[Addons] Fishing=0 -- module disabled, client untouched");
        return OSMW_MOD_DISABLED;
    }

    result = fg_resolve_all();
    if (result != OSMW_OK) {
        fg_log("resolve: required signature failed (%ld); module standing down",
               (long)result);
        return rollback_init(result);
    }
    if (!fg_fishing_install() || !fg_panel_install()) {
        fg_log("install: fishing or panel setup failed; module standing down");
        return rollback_init(OSMW_E_INTERNAL);
    }
    g_panel_installed = 1;

    result = api->claim_game_packet(
        owner, FG_PACKET_SNAPSHOT, snapshot_handler, NULL,
        &g_snapshot_token);
    if (result != OSMW_OK)
        return rollback_init(result);
    result = api->observe_game_packet_after(
        owner, FG_PACKET_MAP_DATA, OSMW_PRIORITY_DEFAULT,
        map_data_observer, NULL, &g_map_token);
    if (result != OSMW_OK)
        return rollback_init(result);
    result = api->subscribe_post_ui_tick(
        owner, OSMW_PRIORITY_DEFAULT, fishing_tick_callback, NULL,
        &g_tick_token);
    if (result != OSMW_OK)
        return rollback_init(result);

    g_active = 1;
    fg_log("startup complete; packet services and post-UiTick callback active");
    return OSMW_OK;
}

static OSMW_RESULT __cdecl fishing_shutdown(
    const OSMW_CORE_API_V1 *api, OSMW_OWNER owner)
{
    OSMW_RESULT result;

    if (g_api == NULL)
        return OSMW_OK;
    if (api != g_api)
        return OSMW_E_INVALID_ARGUMENT;
    if (owner != g_owner)
        return OSMW_E_WRONG_OWNER;

    g_active = 0;
    result = release_tokens();
    if (result != OSMW_OK) {
        fg_log("shutdown: token release failed (%ld)", (long)result);
        return result;
    }
    if (g_panel_installed) {
        fg_panel_remove();
        g_panel_installed = 0;
    }
    fg_fishing_reset();
    fg_log("shutdown complete; no live tokens or claims");
    fg_resolve_reset();
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
    output->module_id = "fishing";
    output->display_name = "osMW Fishing Panel";
    output->init = fishing_init;
    output->shutdown = fishing_shutdown;
    return OSMW_OK;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(instance);
    return TRUE;
}
