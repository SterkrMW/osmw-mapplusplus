/* Real-DLL host fixture for the managed minimap lifecycle.
 *
 * The host supplies a fake ABI 1 core plus executable x86-shaped patch targets.
 * This verifies ownership and rollback against the release DLL without loading
 * or modifying the game client.
 */
#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "osmw_core_api.h"

/* Signature count from src/sigs.h. The fixture loads the built DLL and so
 * cannot include that header; bump this when a signature is added, which is
 * the point -- an unnoticed change in what the module resolves should fail. */
#define MM_EXPECTED_SIGS 45

#define TEST_OWNER ((OSMW_OWNER)77u)
#define MAX_CLAIMS 8
#define MAX_NODES 8
#define SLOT_STRIDE 0xCC
#define SLOT_ID 0x00
#define SLOT_NAME 0x04
#define SLOT_X 0x3A
#define SLOT_Y 0x3C
#define SLOT_TYPE 0x58

typedef int (__cdecl *ui_create_fn)(
    int, int, int, int, void *, void *, int, int);
typedef void (__cdecl *void_callback)(void);
typedef void (__cdecl *send_move_fn)(
    int, unsigned short, unsigned short, unsigned char);

typedef struct CLAIM_STATE {
    OSMW_TOKEN token;
    void *address;
    DWORD length;
    OSMW_REGION_KIND kind;
    BYTE snapshot[24];
    int active;
} CLAIM_STATE;

typedef struct NODE_STATE {
    int handle;
    int active;
    int left;
    int top;
    int right;
    int bottom;
    void_callback draw;
    void_callback input;
} NODE_STATE;

static int g_failures;
static int g_addon_enabled = 1;
static int g_auto_walk = 1;
static const char *g_resolve_miss;
static int g_resolve_count;
static int g_saw_uitick_request;
static int g_claim_calls;
static int g_claim_fail_on_call;
static CLAIM_STATE g_claims[MAX_CLAIMS];
static CLAIM_STATE g_claim_history[MAX_CLAIMS];
static int g_claim_history_count;
static OSMW_TOKEN g_next_claim_token = 1;
static OSMW_TOKEN g_force_release_token;
static int g_force_release_used;
static DWORD g_cleanup_order[16];
static int g_cleanup_count;
static OSMW_UI_TICK_CALLBACK_V1 g_tick_callback;
static OSMW_UI_TICK_CALLBACK_V1 g_saved_tick_callback;
static void *g_tick_context;
static OSMW_TOKEN g_tick_token;
static int g_subscribe_fail;
static int g_log_count;
static char g_logs[32768];
static size_t g_logs_used;

static BYTE g_open_call_site[32];
static BYTE g_open_call_original[32];
static BYTE *g_send_target;
static BYTE g_send_original[32];
static int g_send_count;

static unsigned char g_open_flag;
static unsigned char g_enable_tier;
static unsigned char g_enable_flag;
static int g_minimap_handle;
static int g_map_char_count;
static int g_local_slot;
static unsigned char g_map_chars[2 * SLOT_STRIDE];
static unsigned char *g_map_chars_pointer;
static int g_marker_x;
static int g_marker_y;
static int g_scale_x;
static int g_scale_y;
static int g_mouse_x;
static int g_mouse_y;
static unsigned char g_event_code;
static unsigned short g_grid[100];
static unsigned short *g_grid_pointer;
static int g_grid_width;
static int g_grid_height;
static void *g_draw_context;
static void *g_info_frames[9];
static int g_dummy_data;
static NODE_STATE g_nodes[MAX_NODES];
static int g_node_count;
static int g_next_handle;
static int g_create_count;
static int g_destroy_count;
static void_callback g_panel_draw;
static void_callback g_panel_input;

static void check(int condition, const char *message)
{
    if (condition)
        printf("PASS: %s\n", message);
    else {
        printf("FAIL: %s\n", message);
        ++g_failures;
    }
}

static void append_log(const char *message)
{
    size_t length = strlen(message);
    if (length + 2 > sizeof(g_logs) - g_logs_used)
        return;
    memcpy(g_logs + g_logs_used, message, length);
    g_logs_used += length;
    g_logs[g_logs_used++] = '\n';
    g_logs[g_logs_used] = '\0';
}

static int log_contains(const char *text)
{
    return strstr(g_logs, text) != NULL;
}

static const char *__cdecl fake_game_dir(void)
{
    return ".";
}

static void __cdecl fake_log(
    OSMW_OWNER owner, OSMW_LOG_LEVEL level, const char *message)
{
    (void)level;
    if (owner == TEST_OWNER && message != NULL) {
        ++g_log_count;
        append_log(message);
    }
}

static int __cdecl fake_config_read_int(
    OSMW_OWNER owner, const char *section, const char *key, int fallback)
{
    if (owner != TEST_OWNER || section == NULL || key == NULL)
        return fallback;
    if (strcmp(section, "Addons") == 0 &&
            strcmp(key, "MinimapList") == 0)
        return g_addon_enabled;
    if (strcmp(section, "Minimap") == 0 &&
            strcmp(key, "AutoWalk") == 0)
        return g_auto_walk;
    if (strcmp(section, "Minimap") == 0 &&
            strcmp(key, "ShowEventNpcs") == 0)
        return 0;
    if (strcmp(section, "Debug") == 0 &&
            strcmp(key, "DumpGrid") == 0)
        return 0;
    return fallback;
}

static NODE_STATE *find_node(int handle)
{
    int index;
    for (index = 0; index < g_node_count; ++index)
        if (g_nodes[index].handle == handle)
            return &g_nodes[index];
    return NULL;
}

static int __cdecl fake_ui_create(
    int left, int top, int right, int bottom,
    void *draw_proc, void *input_proc, int arg4, int list_selector)
{
    NODE_STATE *node;
    (void)arg4;
    (void)list_selector;

    if (g_node_count >= MAX_NODES)
        return -1;
    node = &g_nodes[g_node_count++];
    ZeroMemory(node, sizeof(*node));
    node->handle = g_next_handle++;
    node->active = 1;
    node->left = left;
    node->top = top;
    node->right = right;
    node->bottom = bottom;
    node->draw = (void_callback)draw_proc;
    node->input = (void_callback)input_proc;
    ++g_create_count;
    if (g_create_count == 2) {
        g_panel_draw = node->draw;
        g_panel_input = node->input;
    }
    return node->handle;
}

static void __cdecl fake_ui_destroy(int handle, int list_selector)
{
    NODE_STATE *node = find_node(handle);
    (void)list_selector;
    if (node != NULL && node->active) {
        node->active = 0;
        ++g_destroy_count;
    }
}

static int __cdecl fake_node_geometry(
    int handle, void *rect_pointer, int *flag, int list_selector)
{
    NODE_STATE *node = find_node(handle);
    int *rect = (int *)rect_pointer;
    (void)list_selector;
    if (node == NULL || !node->active || rect == NULL)
        return -2;
    rect[0] = node->left;
    rect[1] = node->top;
    rect[2] = node->right;
    rect[3] = node->bottom;
    if (flag != NULL)
        *flag = 0;
    return 0;
}

static void __cdecl fake_node_set_geometry(
    int handle, const void *rect_pointer, int flag, int list_selector)
{
    NODE_STATE *node = find_node(handle);
    const int *rect = (const int *)rect_pointer;
    (void)flag;
    (void)list_selector;
    if (node != NULL && rect != NULL) {
        node->left = rect[0];
        node->top = rect[1];
        node->right = rect[2];
        node->bottom = rect[3];
    }
}

static void __cdecl fake_draw_text(
    int x, int y, const char *text, int mode, void *context)
{
    (void)x;
    (void)y;
    (void)text;
    (void)mode;
    (void)context;
}

static void __cdecl fake_draw_box(
    int x, int y, int width, int height, void *context)
{
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)context;
}

static void *resolved_address(const char *name)
{
    if (strcmp(name, "Open") == 0) return &g_open_flag;
    if (strcmp(name, "EnableTier") == 0) return &g_enable_tier;
    if (strcmp(name, "EnableFlag") == 0) return &g_enable_flag;
    if (strcmp(name, "Handle") == 0) return &g_minimap_handle;
    if (strcmp(name, "UiCreateNode") == 0) return (void *)fake_ui_create;
    if (strcmp(name, "MapCharCount") == 0) return &g_map_char_count;
    if (strcmp(name, "LocalSlot") == 0) return &g_local_slot;
    if (strcmp(name, "MapCharBase") == 0) return &g_map_chars_pointer;
    if (strcmp(name, "MarkerX") == 0) return &g_marker_x;
    if (strcmp(name, "MarkerY") == 0) return &g_marker_y;
    if (strcmp(name, "GridScaleY") == 0) return &g_scale_y;
    if (strcmp(name, "GridScaleX") == 0) return &g_scale_x;
    if (strcmp(name, "UiCreateCall") == 0) return g_open_call_site;
    if (strcmp(name, "NodeGeometry") == 0)
        return (void *)fake_node_geometry;
    if (strcmp(name, "DrawText") == 0) return (void *)fake_draw_text;
    if (strcmp(name, "DrawCtx") == 0) return &g_draw_context;
    if (strcmp(name, "MouseX") == 0) return &g_mouse_x;
    if (strcmp(name, "MouseY") == 0) return &g_mouse_y;
    if (strcmp(name, "EventCode") == 0) return &g_event_code;
    if (strcmp(name, "NodeSetGeometry") == 0)
        return (void *)fake_node_set_geometry;
    if (strcmp(name, "DrawInfoBox") == 0) return (void *)fake_draw_box;
    if (strcmp(name, "InfoFrames") == 0) return g_info_frames;
    if (strcmp(name, "CellBuffer") == 0) return &g_grid_pointer;
    if (strcmp(name, "GridWidth") == 0) return &g_grid_width;
    if (strcmp(name, "GridHeight") == 0) return &g_grid_height;
    if (strcmp(name, "SendPlayerMove") == 0) return g_send_target;
    return &g_dummy_data;
}

static OSMW_RESULT __cdecl fake_resolve_signature(
    OSMW_OWNER owner, const OSMW_SIGNATURE_V1 *request,
    OSMW_RESOLVED_ADDRESS_V1 *output)
{
    if (owner != TEST_OWNER || request == NULL || output == NULL ||
            request->struct_size != sizeof(*request) ||
            output->struct_size != sizeof(*output))
        return OSMW_E_INVALID_ARGUMENT;
    ++g_resolve_count;
    if (strcmp(request->name, "UiTick") == 0)
        g_saw_uitick_request = 1;
    if (g_resolve_miss != NULL &&
            strcmp(request->name, g_resolve_miss) == 0) {
        output->match_count = 0;
        return OSMW_E_NOT_FOUND;
    }
    output->match_count = 1;
    output->address = resolved_address(request->name);
    output->reference_rva = request->reference_rva;
    output->rva_delta = 0;
    return OSMW_OK;
}

static int overlaps(void *left, DWORD left_length, void *right, DWORD right_length)
{
    BYTE *a = (BYTE *)left;
    BYTE *b = (BYTE *)right;
    return a < b + right_length && b < a + left_length;
}

static OSMW_RESULT __cdecl fake_claim_region(
    OSMW_OWNER owner, const OSMW_REGION_CLAIM_V1 *claim,
    OSMW_TOKEN *output_token)
{
    CLAIM_STATE *state;
    int index;

    if (owner != TEST_OWNER || claim == NULL || output_token == NULL ||
            claim->struct_size != sizeof(*claim) || claim->address == NULL ||
            claim->length == 0 || claim->length > 24)
        return OSMW_E_INVALID_ARGUMENT;
    ++g_claim_calls;
    if (g_claim_fail_on_call == g_claim_calls)
        return OSMW_E_ALREADY_CLAIMED;
    for (index = 0; index < MAX_CLAIMS; ++index)
        if (g_claims[index].active &&
                overlaps(claim->address, claim->length,
                         g_claims[index].address, g_claims[index].length))
            return OSMW_E_ALREADY_CLAIMED;
    for (index = 0; index < MAX_CLAIMS; ++index)
        if (!g_claims[index].active)
            break;
    if (index == MAX_CLAIMS)
        return OSMW_E_INTERNAL;

    state = &g_claims[index];
    ZeroMemory(state, sizeof(*state));
    state->token = g_next_claim_token++;
    state->address = claim->address;
    state->length = claim->length;
    state->kind = claim->kind;
    memcpy(state->snapshot, claim->address, claim->length);
    state->active = 1;
    g_claim_history[g_claim_history_count++] = *state;
    *output_token = state->token;
    return OSMW_OK;
}

static OSMW_RESULT __cdecl fake_release_region(
    OSMW_OWNER owner, OSMW_TOKEN token)
{
    int index;
    if (owner != TEST_OWNER)
        return OSMW_E_WRONG_OWNER;
    g_cleanup_order[g_cleanup_count++] = token;
    if (token == g_force_release_token && !g_force_release_used) {
        g_force_release_used = 1;
        return OSMW_E_PATCH_STILL_ACTIVE;
    }
    for (index = 0; index < MAX_CLAIMS; ++index) {
        CLAIM_STATE *state = &g_claims[index];
        if (state->active && state->token == token) {
            if (memcmp(state->address, state->snapshot, state->length) != 0)
                return OSMW_E_PATCH_STILL_ACTIVE;
            state->active = 0;
            return OSMW_OK;
        }
    }
    return OSMW_E_NOT_FOUND;
}

static OSMW_RESULT __cdecl fake_subscribe_tick(
    OSMW_OWNER owner, LONG priority, OSMW_UI_TICK_CALLBACK_V1 callback,
    void *context, OSMW_TOKEN *output_token)
{
    if (owner != TEST_OWNER || priority != OSMW_PRIORITY_DEFAULT ||
            callback == NULL || output_token == NULL)
        return OSMW_E_INVALID_ARGUMENT;
    if (g_subscribe_fail)
        return OSMW_E_UNAVAILABLE;
    g_tick_callback = callback;
    g_saved_tick_callback = callback;
    g_tick_context = context;
    g_tick_token = 1000;
    *output_token = g_tick_token;
    return OSMW_OK;
}

static OSMW_RESULT __cdecl fake_unsubscribe(
    OSMW_OWNER owner, OSMW_TOKEN token)
{
    if (owner != TEST_OWNER)
        return OSMW_E_WRONG_OWNER;
    if (token != g_tick_token || g_tick_callback == NULL)
        return OSMW_E_NOT_FOUND;
    g_cleanup_order[g_cleanup_count++] = token;
    g_tick_callback = NULL;
    g_tick_context = NULL;
    g_tick_token = OSMW_INVALID_TOKEN;
    return OSMW_OK;
}

static int resource_count(void)
{
    int count = g_tick_callback != NULL;
    int index;
    for (index = 0; index < MAX_CLAIMS; ++index)
        if (g_claims[index].active)
            ++count;
    return count;
}

static void initialize_patch_targets(void)
{
    LONG displacement;
    DWORD counter_address = (DWORD)(ULONG_PTR)&g_send_count;

    memset(g_open_call_site, 0x90, sizeof(g_open_call_site));
    g_open_call_site[0] = 0xE8;
    displacement = (LONG)((BYTE *)(void *)fake_ui_create -
                          (g_open_call_site + 5));
    memcpy(g_open_call_site + 1, &displacement, sizeof(displacement));
    g_open_call_site[17] = 0xE8;
    displacement = (LONG)((BYTE *)(void *)fake_ui_destroy -
                          (g_open_call_site + 22));
    memcpy(g_open_call_site + 18, &displacement, sizeof(displacement));
    memcpy(g_open_call_original, g_open_call_site,
           sizeof(g_open_call_original));

    if (g_send_target == NULL)
        g_send_target = (BYTE *)VirtualAlloc(
            NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (g_send_target == NULL)
        return;
    memset(g_send_target, 0x90, 32);
    g_send_target[0] = 0x83;
    g_send_target[1] = 0xEC;
    g_send_target[2] = 0x0C;
    g_send_target[3] = 0x8B;
    g_send_target[4] = 0x44;
    g_send_target[5] = 0x24;
    g_send_target[6] = 0x10;
    g_send_target[7] = 0xFF;
    g_send_target[8] = 0x05;
    memcpy(g_send_target + 9, &counter_address, sizeof(counter_address));
    g_send_target[13] = 0x83;
    g_send_target[14] = 0xC4;
    g_send_target[15] = 0x0C;
    g_send_target[16] = 0xC3;
    memcpy(g_send_original, g_send_target, sizeof(g_send_original));
}

static void reset_core_state(void)
{
    ZeroMemory(g_claims, sizeof(g_claims));
    ZeroMemory(g_claim_history, sizeof(g_claim_history));
    g_claim_history_count = 0;
    g_next_claim_token = 1;
    g_force_release_token = OSMW_INVALID_TOKEN;
    g_force_release_used = 0;
    g_resolve_miss = NULL;
    g_resolve_count = 0;
    g_saw_uitick_request = 0;
    g_claim_calls = 0;
    g_claim_fail_on_call = 0;
    g_tick_callback = NULL;
    g_saved_tick_callback = NULL;
    g_tick_context = NULL;
    g_tick_token = OSMW_INVALID_TOKEN;
    g_subscribe_fail = 0;
    g_cleanup_count = 0;
    ZeroMemory(g_cleanup_order, sizeof(g_cleanup_order));
    g_log_count = 0;
    g_logs_used = 0;
    g_logs[0] = '\0';
    g_send_count = 0;
    g_addon_enabled = 1;
    g_auto_walk = 1;
}

static void reset_client_state(void)
{
    ZeroMemory(g_nodes, sizeof(g_nodes));
    g_node_count = 0;
    g_next_handle = 10;
    g_create_count = 0;
    g_destroy_count = 0;
    g_panel_draw = NULL;
    g_panel_input = NULL;
    g_open_flag = 0;
    g_enable_tier = 1;
    g_enable_flag = 1;
    g_minimap_handle = 0;
    g_map_char_count = 0;
    g_local_slot = 0;
    ZeroMemory(g_map_chars, sizeof(g_map_chars));
    g_map_chars_pointer = NULL;
    g_marker_x = 0;
    g_marker_y = 0;
    g_scale_x = 0;
    g_scale_y = 0;
    g_mouse_x = 0;
    g_mouse_y = 0;
    g_event_code = 0;
    ZeroMemory(g_grid, sizeof(g_grid));
    g_grid_pointer = NULL;
    g_grid_width = 0;
    g_grid_height = 0;
    g_draw_context = (void *)1;
    ZeroMemory(g_info_frames, sizeof(g_info_frames));
    g_info_frames[0] = (void *)1;
}

static void reset_fixture(void)
{
    reset_core_state();
    reset_client_state();
    initialize_patch_targets();
}

static void setup_world(void)
{
    unsigned char *local;
    unsigned char *npc;

    g_scale_x = 16;
    g_scale_y = 8;
    g_grid_width = 10;
    g_grid_height = 10;
    g_grid_pointer = g_grid;
    g_grid[1 * 10 + 2] = 0x8000;

    g_map_chars_pointer = g_map_chars;
    g_map_char_count = 2;
    g_local_slot = 0;
    local = g_map_chars;
    *(int *)(local + SLOT_ID) = 1;
    strcpy((char *)(local + SLOT_NAME), "Local");
    *(unsigned short *)(local + SLOT_X) = 24;
    *(unsigned short *)(local + SLOT_Y) = 12;
    local[SLOT_TYPE] = 0;

    npc = g_map_chars + SLOT_STRIDE;
    *(int *)(npc + SLOT_ID) = -7;
    strcpy((char *)(npc + SLOT_NAME), "Merchant");
    *(unsigned short *)(npc + SLOT_X) = 88;
    *(unsigned short *)(npc + SLOT_Y) = 12;
    npc[SLOT_TYPE] = 1;
    g_minimap_handle = -1;
}

static void invoke_tick(void)
{
    if (g_tick_callback != NULL)
        g_tick_callback(TEST_OWNER, g_tick_context, 0);
}

static void *direct_call_target(BYTE *site)
{
    LONG displacement;
    memcpy(&displacement, site + 1, sizeof(displacement));
    return site + 5 + displacement;
}

static int invoke_open_minimap(void)
{
    ui_create_fn open = (ui_create_fn)direct_call_target(g_open_call_site);
    return open(196, 148, 604, 452,
                (void *)fake_draw_text, (void *)fake_draw_text, 0, 0);
}

static void click_first_row_twice(void)
{
    g_mouse_x = 520;
    g_mouse_y = 148 + 8 + 14 + 1;
    g_event_code = 0xF5;
    g_panel_input();
    g_event_code = 0;
    g_panel_input();
    g_event_code = 0xF5;
    g_panel_input();
}

static OSMW_CORE_API_V1 make_api(void)
{
    OSMW_CORE_API_V1 api;
    ZeroMemory(&api, sizeof(api));
    api.struct_size = sizeof(api);
    api.abi_version = OSMW_CORE_ABI_V1_0;
    api.capabilities = OSMW_CAP_CONFIG | OSMW_CAP_SIGNATURES |
                       OSMW_CAP_REGION_CLAIMS | OSMW_CAP_UI_TICK;
    api.game_dir = fake_game_dir;
    api.log = fake_log;
    api.config_read_int = fake_config_read_int;
    api.resolve_signature = fake_resolve_signature;
    api.claim_region = fake_claim_region;
    api.release_region = fake_release_region;
    api.subscribe_post_ui_tick = fake_subscribe_tick;
    api.unsubscribe = fake_unsubscribe;
    return api;
}

int main(int argc, char **argv)
{
    HMODULE module;
    OSMW_MOD_QUERY_V1 query;
    OSMW_MOD_DESCRIPTOR_V1 descriptor;
    OSMW_CORE_API_V1 api;
    OSMW_RESULT result;
    DWORD started;
    DWORD elapsed;
    int stock_handle;
    int before;

    if (argc != 2) {
        fprintf(stderr, "usage: test_lifecycle.exe osmw_minimap.dll\n");
        return 2;
    }

    reset_fixture();
    check(g_send_target != NULL, "host allocates executable SendPlayerMove target");
    if (g_send_target == NULL)
        return 2;

    module = LoadLibraryA(argv[1]);
    check(module != NULL, "real minimap DLL loads");
    if (module == NULL)
        return 2;
    check(g_resolve_count == 0 && resource_count() == 0 && g_log_count == 0,
          "DllMain is passive");

    query = (OSMW_MOD_QUERY_V1)GetProcAddress(module, "osmw_mod_query");
    check(query != NULL, "undecorated osmw_mod_query resolves");
    if (query == NULL)
        return 2;

    ZeroMemory(&descriptor, sizeof(descriptor));
    result = query(OSMW_ABI_MAKE(2, 0), sizeof(descriptor), &descriptor);
    check(result == OSMW_E_INCOMPATIBLE_ABI,
          "query rejects another ABI major");
    result = query(OSMW_CORE_ABI_V1_0,
                   OSMW_MOD_DESCRIPTOR_V1_MIN_SIZE - 1, &descriptor);
    check(result == OSMW_E_BUFFER_TOO_SMALL,
          "query rejects a short descriptor");
    result = query(OSMW_CORE_ABI_V1_0, sizeof(descriptor), &descriptor);
    check(result == OSMW_OK && descriptor.init != NULL &&
          descriptor.shutdown != NULL &&
          strcmp(descriptor.module_id, "minimap") == 0,
          "query publishes the minimap ABI 1 descriptor");
    check(g_resolve_count == 0 && resource_count() == 0,
          "query is side-effect free");

    api = make_api();
    g_addon_enabled = 0;
    result = descriptor.init(&api, TEST_OWNER);
    check(result == OSMW_MOD_DISABLED && g_resolve_count == 0 &&
          resource_count() == 0,
          "disabled init resolves nothing and owns no resources");
    check(descriptor.shutdown(&api, TEST_OWNER) == OSMW_OK,
          "disabled module shuts down cleanly");

    reset_fixture();
    api = make_api();
    api.capabilities &= ~OSMW_CAP_REGION_CLAIMS;
    result = descriptor.init(&api, TEST_OWNER);
    check(result == OSMW_E_UNAVAILABLE && g_resolve_count == 0 &&
          resource_count() == 0,
          "missing required core capability fails before binding or resolution");

    reset_fixture();
    api = make_api();
    g_resolve_miss = "DrawText";
    result = descriptor.init(&api, TEST_OWNER);
    check(result == OSMW_E_NOT_FOUND && g_resolve_count == MM_EXPECTED_SIGS &&
          resource_count() == 0,
          "required signature miss rolls back before any claim");

    reset_fixture();
    api = make_api();
    g_claim_fail_on_call = 2;
    result = descriptor.init(&api, TEST_OWNER);
    check(result == OSMW_E_ALREADY_CLAIMED && resource_count() == 0 &&
          memcmp(g_open_call_site, g_open_call_original,
                 sizeof(g_open_call_site)) == 0,
          "claim collision returns the earlier untouched claim");

    reset_fixture();
    api = make_api();
    g_subscribe_fail = 1;
    result = descriptor.init(&api, TEST_OWNER);
    check(result == OSMW_E_UNAVAILABLE && resource_count() == 0,
          "subscription failure rolls back every unique claim");
    check(memcmp(g_open_call_site, g_open_call_original,
                 sizeof(g_open_call_site)) == 0 &&
          memcmp(g_send_target, g_send_original, sizeof(g_send_original)) == 0,
          "rollback restores all three patched ranges before release");

    reset_fixture();
    api = make_api();
    result = descriptor.init(&api, TEST_OWNER);
    check(result == OSMW_OK && g_resolve_count == MM_EXPECTED_SIGS &&
          !g_saw_uitick_request && resource_count() == 5,
          "active init resolves every private signature and owns four claims plus one token");
    check(g_claim_history_count == 4 &&
          g_claim_history[0].address == g_open_call_site &&
          g_claim_history[0].length == 5 &&
          g_claim_history[0].kind == OSMW_REGION_CALL_SITE &&
          g_claim_history[1].address == g_open_call_site + 17 &&
          g_claim_history[1].length == 5 &&
          g_claim_history[1].kind == OSMW_REGION_CALL_SITE &&
          g_claim_history[2].address == g_send_target &&
          g_claim_history[2].length == 7 &&
          g_claim_history[2].kind == OSMW_REGION_CODE &&
          g_claim_history[3].length == 5 &&
          g_claim_history[3].kind == OSMW_REGION_CODE,
          "claims cover both call sites, SendPlayerMove and the draw proc");
    check(direct_call_target(g_open_call_site) != (void *)fake_ui_create &&
          direct_call_target(g_open_call_site + 17) != (void *)fake_ui_destroy &&
          g_send_target[0] == 0xE9,
          "all three runtime ranges are patched only under claims");

    started = GetTickCount();
    invoke_tick();
    elapsed = GetTickCount() - started;
    check(elapsed < 100 && g_create_count == 0 && g_destroy_count == 0,
          "map-not-yet-loaded tick is nonblocking and inert");

    setup_world();
    started = GetTickCount();
    invoke_tick();
    elapsed = GetTickCount() - started;
    check(elapsed < 100 && g_create_count == 0 &&
          log_contains("world: map loaded"),
          "minimap-never-opened state is observed without waiting");

    stock_handle = invoke_open_minimap();
    g_minimap_handle = stock_handle;
    check(stock_handle >= 0 && g_create_count == 2 &&
          g_panel_draw != NULL && g_panel_input != NULL,
          "claimed open call creates shifted map and companion panel");
    g_panel_draw();
    click_first_row_twice();
    check(g_send_count == 1 && log_contains("routed around"),
          "second row click starts a routed walk through the original trampoline");
    ((send_move_fn)g_send_target)(0, 40, 24, 1);
    check(g_send_count == 2 && log_contains("walk: stopped (player moved)"),
          "manual movement is forwarded and cancels the active route");

    g_minimap_handle = -1;
    invoke_tick();
    check(g_destroy_count == 1,
          "post-UiTick close cleanup destroys a panel left by a bypass path");

    g_cleanup_count = 0;
    result = descriptor.shutdown(&api, TEST_OWNER);
    check(result == OSMW_OK && resource_count() == 0,
          "clean shutdown returns the callback and all unique claims");
    check(g_cleanup_count == 5 && g_cleanup_order[0] == 1000 &&
          g_cleanup_order[1] == 4 && g_cleanup_order[2] == 3 &&
          g_cleanup_order[3] == 2 && g_cleanup_order[4] == 1,
          "shutdown revokes tick then releases draw, send, close, and open ownership");
    check(memcmp(g_open_call_site, g_open_call_original,
                 sizeof(g_open_call_site)) == 0 &&
          memcmp(g_send_target, g_send_original, sizeof(g_send_original)) == 0,
          "shutdown restores every claimed byte before release");
    before = g_log_count;
    g_saved_tick_callback(TEST_OWNER, NULL, 0);
    check(g_log_count == before,
          "revoked post-tick callback is inert before DLL unmap");

    reset_fixture();
    api = make_api();
    g_auto_walk = 0;
    result = descriptor.init(&api, TEST_OWNER);
    check(result == OSMW_OK && resource_count() == 4 &&
          g_claim_history_count == 3 && g_send_target[0] != 0xE9,
          "AutoWalk=0 omits the SendPlayerMove claim and hook");
    setup_world();
    invoke_tick();
    stock_handle = invoke_open_minimap();
    g_minimap_handle = stock_handle;
    g_panel_draw();
    click_first_row_twice();
    check(g_send_count == 0 && log_contains("auto-walk disabled"),
          "disabled auto-walk cannot fall through to the raw send address");
    g_minimap_handle = -1;
    invoke_tick();
    check(descriptor.shutdown(&api, TEST_OWNER) == OSMW_OK &&
          resource_count() == 0,
          "panel-only mode cleans up without SendPlayerMove ownership");

    reset_fixture();
    api = make_api();
    result = descriptor.init(&api, TEST_OWNER);
    check(result == OSMW_OK, "final eject scenario initialises");
    g_force_release_token = 3;
    result = descriptor.shutdown(&api, TEST_OWNER);
    check(result == OSMW_E_PATCH_STILL_ACTIVE && resource_count() == 1 &&
          memcmp(g_send_target, g_send_original, sizeof(g_send_original)) == 0,
          "release failure keeps the restored claim and DLL binding mapped");
    result = descriptor.shutdown(&api, TEST_OWNER);
    check(result == OSMW_OK && resource_count() == 0,
          "retrying shutdown releases the retained claim cleanly");

    check(FreeLibrary(module) != 0,
          "FreeLibrary follows clean managed shutdown");
    VirtualFree(g_send_target, 0, MEM_RELEASE);

    if (g_failures != 0) {
        printf("FAILED: %d check(s)\n", g_failures);
        return 1;
    }
    printf("PASSED: managed minimap lifecycle and ownership\n");
    return 0;
}
