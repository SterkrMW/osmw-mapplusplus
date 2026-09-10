/* Passive managed-module entry point and lifecycle.
 *
 * Core owns the shared UiTick detour and calls minimap after the original node
 * walk. Minimap claims only its two panel call sites and, when auto-walk is
 * enabled, the unique SendPlayerMove prologue before installing existing,
 * specialised patches.
 *
 * Spec: mw-tools/tools/client-dll/minimap-autowalk.md
 */
#include "osmw_minimap.h"

#include <stdio.h>

#define MM_REQUIRED_CAPABILITIES \
    (OSMW_CAP_CONFIG | OSMW_CAP_SIGNATURES | OSMW_CAP_REGION_CLAIMS | \
     OSMW_CAP_UI_TICK)

static const OSMW_CORE_API_V1 *g_api;
static OSMW_OWNER g_owner = OSMW_INVALID_OWNER;
static OSMW_TOKEN g_open_call_claim = OSMW_INVALID_TOKEN;
static OSMW_TOKEN g_destroy_call_claim = OSMW_INVALID_TOKEN;
static OSMW_TOKEN g_send_move_claim = OSMW_INVALID_TOKEN;
static OSMW_TOKEN g_draw_proc_claim = OSMW_INVALID_TOKEN;
static int        g_markers_drawing;
static OSMW_TOKEN g_tick_token = OSMW_INVALID_TOKEN;
static int g_panel_installed;
static int g_walk_configured;
static int g_active;

/* The old worker blocked indefinitely waiting for these transitions. They are
 * now observation state advanced in bounded post-tick work. */
static int g_world_seen;
static int g_minimap_seen;
static DWORD g_world_wait_log;
static DWORD g_minimap_wait_log;

const OSMW_CORE_API_V1 *mm_core_api(void)
{
    return g_api;
}

OSMW_OWNER mm_core_owner(void)
{
    return g_owner;
}

static void clear_binding(void)
{
    g_active = 0;
    g_panel_installed = 0;
    g_walk_configured = 0;
    g_open_call_claim = OSMW_INVALID_TOKEN;
    g_destroy_call_claim = OSMW_INVALID_TOKEN;
    g_send_move_claim = OSMW_INVALID_TOKEN;
    g_tick_token = OSMW_INVALID_TOKEN;
    g_world_seen = 0;
    g_minimap_seen = 0;
    g_world_wait_log = 0;
    g_minimap_wait_log = 0;
    g_owner = OSMW_INVALID_OWNER;
    g_api = NULL;
}

static OSMW_RESULT claim_region(
    void *address, DWORD length, OSMW_REGION_KIND kind,
    const char *label, OSMW_TOKEN *token)
{
    OSMW_REGION_CLAIM_V1 claim;

    ZeroMemory(&claim, sizeof(claim));
    claim.struct_size = sizeof(claim);
    claim.address = address;
    claim.length = length;
    claim.kind = kind;
    claim.label = label;
    return g_api->claim_region(g_owner, &claim, token);
}

/* Walk the array the way the panel will and retain the original cold-start
 * diagnostics without parking a thread inside the DLL. */
static void dump_map_characters(void)
{
    int count = mm_map_char_count();
    int local = mm_local_slot_index();
    int npcs = 0, players = 0, other = 0, shown = 0;
    int index;

    mm_log("mapchars: count %d, local slot %d", count, local);
    if (count < 0 || count > 4096) {
        mm_log("mapchars: count %d is not plausible", count);
        return;
    }

    for (index = 0; index < count; ++index) {
        unsigned char *slot = mm_map_char_slot(index);
        unsigned char type;
        const char *name;
        int px, py, gx = 0, gy = 0;

        if (slot == NULL)
            continue;
        type = slot[MM_SLOT_TYPE];
        if (type == MM_TYPE_NPC)
            ++npcs;
        else if (type == MM_TYPE_PLAYER)
            ++players;
        else
            ++other;

        if (type != MM_TYPE_NPC || shown >= 12)
            continue;
        name = (const char *)(slot + MM_SLOT_NAME);
        if (!name[0])
            continue;
        px = *(unsigned short *)(slot + MM_SLOT_X);
        py = *(unsigned short *)(slot + MM_SLOT_Y);
        mm_pixel_to_grid(px, py, &gx, &gy);
        mm_log("  npc[%3d] id %d \"%.14s\" px (%d,%d) grid (%d,%d)",
               index, *(int *)(slot + MM_SLOT_ID), name, px, py, gx, gy);
        ++shown;
    }

    mm_log("mapchars: %d NPC, %d player, %d other", npcs, players, other);
}

static void dump_scales(void)
{
    mm_log("scales: grid %d x %d px; marker (%d,%d); minimap handle %d",
           *(int *)mm_addr[MM_GRIDSCALEX], *(int *)mm_addr[MM_GRIDSCALEY],
           *(int *)mm_addr[MM_MARKERX], *(int *)mm_addr[MM_MARKERY],
           mm_minimap_handle());
}

static void observe_startup_state(void)
{
    DWORD now = GetTickCount();

    if (!mm_client_in_world()) {
        if (g_world_seen) {
            mm_log("world: map left; waiting for the next map");
            g_world_seen = 0;
            g_minimap_seen = 0;
            g_world_wait_log = now;
        } else if (now - g_world_wait_log >= 30000u) {
            mm_log("world: still waiting for a map");
            g_world_wait_log = now;
        }
        return;
    }

    if (!g_world_seen) {
        g_world_seen = 1;
        g_minimap_seen = 0;
        g_minimap_wait_log = now;
        mm_log("world: map loaded");
        dump_scales();
        dump_map_characters();
    }

    if (mm_minimap_handle() >= 0) {
        if (!g_minimap_seen) {
            int left = 0, top = 0, right = 0, bottom = 0;
            g_minimap_seen = 1;
            mm_log("minimap: opened, handle %d", mm_minimap_handle());
            if (mm_panel_rect(&left, &top, &right, &bottom))
                mm_log("panel: slot (%d,%d)-(%d,%d), %dx%d",
                       left, top, right, bottom,
                       right - left, bottom - top);
        }
    } else if (!g_minimap_seen && now - g_minimap_wait_log >= 60000u) {
        mm_log("minimap: not opened yet (press Tab)");
        g_minimap_wait_log = now;
    }
}

static void __cdecl minimap_tick(
    OSMW_OWNER owner, void *context, LONG list_selector)
{
    (void)context;
    (void)list_selector;
    if (!g_active || owner != g_owner)
        return;

    /* Core has already completed the original client node walk. */
    mm_grid_poll();
    mm_panel_idle();
    mm_walk_tick();
    observe_startup_state();

    /* Markers: both are cheap and self-rate-limiting -- the store stats the
     * file at most every two seconds, and the verify line is emitted once per
     * map. Neither draws anything yet. */
    mm_store_poll();
    mm_markers_verify();
    mm_panel_edit_tick();
}

static OSMW_RESULT cleanup_resources(
    const OSMW_CORE_API_V1 *api, OSMW_OWNER owner)
{
    OSMW_RESULT first_failure = OSMW_OK;
    OSMW_RESULT result;

    g_active = 0;
    if (g_tick_token != OSMW_INVALID_TOKEN) {
        result = api->unsubscribe(owner, g_tick_token);
        if (result != OSMW_OK) {
            mm_log("cleanup: UiTick unsubscribe failed (%ld)", (long)result);
            return result;
        }
        g_tick_token = OSMW_INVALID_TOKEN;
    }

    /* Before the hooks come down: the store owns a heap block and a mutex,
     * neither of which core can reclaim for us. */
    mm_store_shutdown();

    if (g_markers_drawing) {
        mm_markers_remove_draw();
        g_markers_drawing = 0;
    }
    if (g_draw_proc_claim != OSMW_INVALID_TOKEN) {
        result = api->release_region(owner, g_draw_proc_claim);
        if (result == OSMW_OK)
            g_draw_proc_claim = OSMW_INVALID_TOKEN;
        else if (first_failure == OSMW_OK)
            first_failure = result;
    }

    if (g_walk_configured) {
        mm_walk_remove();
        g_walk_configured = 0;
    }
    if (g_send_move_claim != OSMW_INVALID_TOKEN) {
        result = api->release_region(owner, g_send_move_claim);
        if (result == OSMW_OK)
            g_send_move_claim = OSMW_INVALID_TOKEN;
        else if (first_failure == OSMW_OK)
            first_failure = result;
    }

    if (g_panel_installed) {
        mm_panel_remove();
        g_panel_installed = 0;
    }
    if (g_destroy_call_claim != OSMW_INVALID_TOKEN) {
        result = api->release_region(owner, g_destroy_call_claim);
        if (result == OSMW_OK)
            g_destroy_call_claim = OSMW_INVALID_TOKEN;
        else if (first_failure == OSMW_OK)
            first_failure = result;
    }
    if (g_open_call_claim != OSMW_INVALID_TOKEN) {
        result = api->release_region(owner, g_open_call_claim);
        if (result == OSMW_OK)
            g_open_call_claim = OSMW_INVALID_TOKEN;
        else if (first_failure == OSMW_OK)
            first_failure = result;
    }

    mm_path_release();
    mm_grid_reset();
    if (first_failure == OSMW_OK) {
        mm_resolve_reset();
        clear_binding();
    }
    return first_failure;
}

static OSMW_RESULT rollback_init(OSMW_RESULT failure)
{
    OSMW_RESULT cleanup = cleanup_resources(g_api, g_owner);
    return cleanup == OSMW_OK ? failure : cleanup;
}

static OSMW_RESULT __cdecl minimap_init(
    const OSMW_CORE_API_V1 *api, OSMW_OWNER owner)
{
    OSMW_RESULT result;
    int auto_walk;

    if (api == NULL || owner == OSMW_INVALID_OWNER)
        return OSMW_E_INVALID_ARGUMENT;
    if (api->struct_size < OSMW_CORE_API_V1_0_SIZE ||
            OSMW_ABI_MAJOR(api->abi_version) !=
                OSMW_ABI_MAJOR(OSMW_CORE_ABI_V1_0))
        return OSMW_E_INCOMPATIBLE_ABI;
    if ((api->capabilities & MM_REQUIRED_CAPABILITIES) !=
            MM_REQUIRED_CAPABILITIES ||
            api->game_dir == NULL || api->log == NULL ||
            api->config_read_int == NULL ||
            api->resolve_signature == NULL ||
            api->claim_region == NULL || api->release_region == NULL ||
            api->subscribe_post_ui_tick == NULL || api->unsubscribe == NULL)
        return OSMW_E_UNAVAILABLE;
    if (g_api != NULL)
        return OSMW_E_BUSY;

    g_api = api;
    g_owner = owner;
    g_world_wait_log = GetTickCount();
    mm_log("osmw_minimap managed init");

    if (!mm_cfg_int_in("Addons", "MinimapList", 1)) {
        mm_log("[Addons] MinimapList=0 -- module disabled, client untouched");
        return OSMW_MOD_DISABLED;
    }

    result = mm_resolve_all();
    if (result != OSMW_OK)
        return rollback_init(result);

    result = claim_region(
        mm_addr[MM_UICREATECALL], MM_DIRECT_CALL_PATCH_LENGTH,
        OSMW_REGION_CALL_SITE, "minimap open UiCreateNode call",
        &g_open_call_claim);
    if (result != OSMW_OK)
        return rollback_init(result);
    result = claim_region(
        mm_addr[MM_UICREATECALL] + MM_PANEL_DESTROY_CALL_OFFSET,
        MM_DIRECT_CALL_PATCH_LENGTH, OSMW_REGION_CALL_SITE,
        "minimap close UiDestroyNode call", &g_destroy_call_claim);
    if (result != OSMW_OK)
        return rollback_init(result);

    auto_walk = mm_cfg_int("AutoWalk", 1) != 0;
    if (auto_walk) {
        result = claim_region(
            mm_addr[MM_SENDPLAYERMOVE],
            MM_SENDPLAYERMOVE_PROLOGUE_LENGTH,
            OSMW_REGION_CODE, "SendPlayerMove prologue",
            &g_send_move_claim);
        if (result != OSMW_OK)
            return rollback_init(result);
    }

    if (!mm_panel_install())
        return rollback_init(OSMW_E_INTERNAL);
    g_panel_installed = 1;

    if (!mm_walk_install(auto_walk))
        return rollback_init(OSMW_E_INTERNAL);
    g_walk_configured = 1;

    /* The marker store is optional infrastructure: it owns no claim and no
     * hook, so a failure here leaves the list and auto-walk exactly as they
     * were rather than standing the module down. */
    if (!mm_store_init())
        mm_log("markers: store unavailable; markers disabled this session");

    /* The marker layer draws from the map's own proc. Optional in the same
     * sense: if the claim or the hook is refused, the panel and auto-walk are
     * untouched and only the glyphs are missing. */
    if (claim_region(mm_addr[MM_DRAWPROC], MM_MARKER_DRAWPROC_PROLOGUE,
                     OSMW_REGION_CODE, "minimap draw proc prologue",
                     &g_draw_proc_claim) == OSMW_OK) {
        g_markers_drawing = mm_markers_install_draw(mm_panel_text_boxed());
        if (!g_markers_drawing) {
            api->release_region(owner, g_draw_proc_claim);
            g_draw_proc_claim = OSMW_INVALID_TOKEN;
        }
    } else {
        mm_log("markers: draw proc already claimed; glyphs disabled");
    }

    result = api->subscribe_post_ui_tick(
        owner, OSMW_PRIORITY_DEFAULT, minimap_tick, NULL, &g_tick_token);
    if (result != OSMW_OK)
        return rollback_init(result);

    g_active = 1;
    mm_log("startup complete; 30 private signatures, %d unique claims, "
           "priority-0 post-UiTick subscription", auto_walk ? 3 : 2);
    return OSMW_OK;
}

static OSMW_RESULT __cdecl minimap_shutdown(
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
    if (result != OSMW_OK) {
        mm_log("shutdown: resource cleanup failed (%ld)", (long)result);
        return result;
    }
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
    output->module_id = "minimap";
    output->display_name = "osMW Minimap NPC List";
    output->init = minimap_init;
    output->shutdown = minimap_shutdown;
    return OSMW_OK;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(instance);
    return TRUE;
}
