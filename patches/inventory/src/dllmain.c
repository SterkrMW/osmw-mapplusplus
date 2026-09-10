/* Passive ABI 1 lifecycle for client-computed, server-authorised inventory
 * sorting. Core owns all shared detours; this module owns only callbacks and a
 * stock UI companion node, all of which are revoked before unload.
 */
#include "osmw_inventory.h"

#include <string.h>

#define IV_REQUIRED_CAPABILITIES \
    (OSMW_CAP_CONFIG | OSMW_CAP_SIGNATURES | OSMW_CAP_UI_TICK | \
     OSMW_CAP_GAME_PACKETS | OSMW_CAP_SEND_QUEUE)

static const OSMW_CORE_API_V1 *g_api;
static OSMW_OWNER g_owner = OSMW_INVALID_OWNER;
static OSMW_TOKEN g_snapshot_token = OSMW_INVALID_TOKEN;
static OSMW_TOKEN g_inventory_token = OSMW_INVALID_TOKEN;
static OSMW_TOKEN g_tick_token = OSMW_INVALID_TOKEN;
static int g_panel_installed;
static int g_active;

const OSMW_CORE_API_V1 *iv_core_api(void)
{
    return g_api;
}

OSMW_OWNER iv_core_owner(void)
{
    return g_owner;
}

static void clear_binding(void)
{
    g_active = 0;
    g_panel_installed = 0;
    g_tick_token = OSMW_INVALID_TOKEN;
    g_inventory_token = OSMW_INVALID_TOKEN;
    g_snapshot_token = OSMW_INVALID_TOKEN;
    g_owner = OSMW_INVALID_OWNER;
    g_api = NULL;
}

static void __cdecl inventory_tick_callback(
    OSMW_OWNER owner, void *context, LONG list_selector)
{
    (void)context;
    (void)list_selector;
    if (!g_active || owner != g_owner)
        return;
    iv_panel_tick();
    iv_sort_tick();
    /* Last, and after the manager has drawn every stock window: this is what
     * keeps the buttons on top of an inventory window the player just raised. */
    iv_panel_overlay();
}

static OSMW_PACKET_ACTION __cdecl snapshot_handler(
    OSMW_OWNER owner, void *context, const OSMW_GAME_PACKET_V1 *packet)
{
    (void)context;
    if (g_active && owner == g_owner && packet &&
            packet->struct_size >= sizeof(*packet) &&
            packet->type == IV_PACKET_SORT_SNAPSHOT) {
        iv_sort_accept_snapshot(packet->field12, packet->extra,
                                packet->extra_size);
    }
    return OSMW_PACKET_CONSUME;
}

static void __cdecl inventory_data_observer(
    OSMW_OWNER owner, void *context, const OSMW_GAME_PACKET_V1 *packet)
{
    (void)context;
    if (g_active && owner == g_owner && packet &&
            packet->struct_size >= sizeof(*packet) &&
            packet->type == IV_PACKET_INVENTORY_DATA)
        iv_sort_on_inventory_data();
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
    result = release_token(&g_inventory_token);
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
        iv_log("rollback: token release failed (%ld)", (long)cleanup);
        return cleanup;
    }
    if (g_panel_installed) {
        iv_panel_remove();
        g_panel_installed = 0;
    }
    iv_sort_reset();
    iv_resolve_reset();
    clear_binding();
    return failure;
}

static OSMW_RESULT __cdecl inventory_init(
    const OSMW_CORE_API_V1 *api, OSMW_OWNER owner)
{
    OSMW_RESULT result;

    if (!api || owner == OSMW_INVALID_OWNER)
        return OSMW_E_INVALID_ARGUMENT;
    if (api->struct_size < OSMW_CORE_API_V1_0_SIZE ||
            OSMW_ABI_MAJOR(api->abi_version) !=
                OSMW_ABI_MAJOR(OSMW_CORE_ABI_V1_0))
        return OSMW_E_INCOMPATIBLE_ABI;
    if ((api->capabilities & IV_REQUIRED_CAPABILITIES) !=
            IV_REQUIRED_CAPABILITIES || !api->log ||
            !api->config_read_int || !api->resolve_signature ||
            !api->subscribe_post_ui_tick || !api->unsubscribe ||
            !api->claim_game_packet || !api->observe_game_packet_after ||
            !api->queue_game_packet)
        return OSMW_E_UNAVAILABLE;
    if (g_api)
        return OSMW_E_BUSY;

    g_api = api;
    g_owner = owner;
    iv_log("osmw_inventory managed init");
    if (!api->config_read_int(owner, "Addons", "Inventory", 1)) {
        iv_log("[Addons] Inventory=0 -- module disabled, client untouched");
        return OSMW_MOD_DISABLED;
    }

    result = iv_resolve_all();
    if (result != OSMW_OK) {
        iv_log("resolve: required signature failed (%ld); standing down",
               (long)result);
        return rollback_init(result);
    }
    if (!iv_sort_install() || !iv_panel_install())
        return rollback_init(OSMW_E_INTERNAL);
    g_panel_installed = 1;

    result = api->claim_game_packet(owner, IV_PACKET_SORT_SNAPSHOT,
                                    snapshot_handler, NULL,
                                    &g_snapshot_token);
    if (result != OSMW_OK)
        return rollback_init(result);
    result = api->observe_game_packet_after(
        owner, IV_PACKET_INVENTORY_DATA, OSMW_PRIORITY_DEFAULT,
        inventory_data_observer, NULL, &g_inventory_token);
    if (result != OSMW_OK)
        return rollback_init(result);
    result = api->subscribe_post_ui_tick(
        owner, OSMW_PRIORITY_DEFAULT, inventory_tick_callback, NULL,
        &g_tick_token);
    if (result != OSMW_OK)
        return rollback_init(result);

    g_active = 1;
    iv_log("startup complete; sort control and packet services active");
    return OSMW_OK;
}

static OSMW_RESULT __cdecl inventory_shutdown(
    const OSMW_CORE_API_V1 *api, OSMW_OWNER owner)
{
    OSMW_RESULT result;

    if (!g_api)
        return OSMW_OK;
    if (api != g_api)
        return OSMW_E_INVALID_ARGUMENT;
    if (owner != g_owner)
        return OSMW_E_WRONG_OWNER;

    g_active = 0;
    result = release_tokens();
    if (result != OSMW_OK) {
        iv_log("shutdown: token release failed (%ld)", (long)result);
        return result;
    }
    if (g_panel_installed) {
        iv_panel_remove();
        g_panel_installed = 0;
    }
    iv_sort_reset();
    iv_log("shutdown complete; no live tokens or UI nodes");
    iv_resolve_reset();
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
    if (!output)
        return OSMW_E_INVALID_ARGUMENT;
    if (output_size < OSMW_MOD_DESCRIPTOR_V1_MIN_SIZE)
        return OSMW_E_BUFFER_TOO_SMALL;

    ZeroMemory(output, sizeof(*output));
    output->struct_size = sizeof(*output);
    output->required_core_abi = OSMW_CORE_ABI_V1_0;
    output->module_version = OSMW_MODULE_VERSION(1, 0);
    output->module_id = "inventory";
    output->display_name = "osMW Inventory Tools";
    output->init = inventory_init;
    output->shutdown = inventory_shutdown;
    return OSMW_OK;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(instance);
    return TRUE;
}
