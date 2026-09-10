/* Real-DLL host fixture for inventory UI, packet protocol, and teardown. */
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "osmw_core_api.h"

#define TEST_OWNER ((OSMW_OWNER)0x7100U)
#define SNAPSHOT_TOKEN ((OSMW_TOKEN)0x7101U)
#define INVENTORY_TOKEN ((OSMW_TOKEN)0x7102U)
#define TICK_TOKEN ((OSMW_TOKEN)0x7103U)
#define SIGNATURE_COUNT 17
#define PACKET_INVENTORY_DATA 0x00070001U
#define PACKET_PREPARE 0x7F000010U
#define PACKET_SNAPSHOT 0x7F000011U
#define PACKET_APPLY 0x7F000012U
#define PACKET_LOCK_TOGGLE 0x7F000020U
#define LOCK_PROTOCOL 1U
#define PROTOCOL 2U

/* The two sprite buttons the panel draws inside the stock window. */
#define SPRITE_W 21
#define SPRITE_PITCH 26
#define STRIP_W 47
#define STRIP_H 21
/* Inventory grid: 6x4 cells of 32 on a 40 pitch, origin (44, 268). */
#define GRID_X 44
#define GRID_Y 268
#define GRID_CELL 32
#define GRID_PITCH 40
#define GRID_W 232
#define GRID_H 152
#define MAX_BLITS 8

typedef int (__cdecl *test_create_fn)(int, int, int, int, void *, void *,
                                      int, int);
typedef void (__cdecl *test_destroy_fn)(int, int);
typedef int (__cdecl *test_geometry_fn)(int, void *, int *, int);
typedef void (__cdecl *test_set_geometry_fn)(int, const void *, int, int);
typedef int (__cdecl *test_load_mda_fn)(const char *, void **);
typedef int (__cdecl *test_blit_mda_fn)(int, const char *, int, int, void *,
                                        int, int, int, int);

static int g_failures;
static int g_service_calls;
static int g_gate_enabled = 1;
static int g_sort_cooldown_seconds;
static const char *g_missing_signature;
static int g_resolve_calls;
static OSMW_PACKET_HANDLER_V1 g_snapshot_handler;
static void *g_snapshot_context;
static OSMW_PACKET_OBSERVER_V1 g_inventory_observer;
static void *g_inventory_context;
static OSMW_UI_TICK_CALLBACK_V1 g_tick_callback;
static void *g_tick_context;
static int g_snapshot_active;
static int g_inventory_active;
static int g_tick_active;
static OSMW_TOKEN g_unsubscribe_order[3];
static int g_unsubscribe_count;
static OSMW_RESULT g_queue_result = OSMW_OK;
static int g_queue_calls;
static DWORD g_last_queue_type;
static DWORD g_last_queue_field12;
static BYTE g_last_queue_extra[128];
static DWORD g_last_queue_extra_size;
static int g_grid_scale = 1;
static int g_inventory_handle = 44;
static int g_inv_rect[4] = {180, 80, 556, 538};
static BYTE g_event_code;
static DWORD g_draw_context;
static DWORD g_info_frames = 1;
static DWORD g_dummy;
static int g_create_calls;
static int g_destroy_calls;
static int g_node_handle = 77;
static void (__cdecl *g_panel_draw)(void);
static void (__cdecl *g_panel_input)(void);
static int g_button_rect[4];
/* The lock-mode grid capture node, 232x152, distinct from the 47x21 strip. */
static void (__cdecl *g_grid_draw)(void);
static void (__cdecl *g_grid_input)(void);
static int g_grid_rect[4];
static int g_grid_handle = 78;
static int g_inv_page;
static int g_inv_slot;
static int g_grid_creates;
static int g_grid_destroys;
static int g_grid_open;
static int g_mouse_x;
static int g_mouse_y;
static DWORD g_mda_bank;
static int g_load_calls;
static char g_load_path[64];
static int g_blit_result = 1;
static int g_blit_count;
static char g_blit_anim[MAX_BLITS][32];
static int g_blit_x[MAX_BLITS];
static int g_blit_y[MAX_BLITS];

static void check(int condition, const char *message)
{
    if (condition)
        printf("PASS: %s\n", message);
    else {
        printf("FAIL: %s\n", message);
        ++g_failures;
    }
}

static int resource_count(void)
{
    return g_snapshot_active + g_inventory_active + g_tick_active;
}

static int __cdecl fake_create(int left, int top, int right, int bottom,
    void *draw_proc, void *input_proc, int arg4, int list_selector)
{
    union { void *address; void (__cdecl *function)(void); } draw, input;
    if (arg4 != 0 || list_selector != 0)
        ++g_failures;
    draw.address = draw_proc;
    input.address = input_proc;

    if (right - left == STRIP_W && bottom - top == STRIP_H) {
        g_panel_draw = draw.function;
        g_panel_input = input.function;
        g_button_rect[0] = left;
        g_button_rect[1] = top;
        g_button_rect[2] = right;
        g_button_rect[3] = bottom;
        ++g_create_calls;
        return g_node_handle;
    }
    if (right - left == GRID_W && bottom - top == GRID_H) {
        g_grid_draw = draw.function;
        g_grid_input = input.function;
        g_grid_rect[0] = left;
        g_grid_rect[1] = top;
        g_grid_rect[2] = right;
        g_grid_rect[3] = bottom;
        ++g_grid_creates;
        g_grid_open = 1;
        return g_grid_handle;
    }
    /* The fallback sidecar is the only other shape. */
    g_panel_draw = draw.function;
    g_panel_input = input.function;
    g_button_rect[0] = left;
    g_button_rect[1] = top;
    g_button_rect[2] = right;
    g_button_rect[3] = bottom;
    ++g_create_calls;
    return g_node_handle;
}

static void __cdecl fake_destroy(int handle, int list_selector)
{
    if (list_selector != 0)
        ++g_failures;
    if (handle == g_grid_handle) {
        ++g_grid_destroys;
        g_grid_open = 0;
        return;
    }
    if (handle != g_node_handle)
        ++g_failures;
    ++g_destroy_calls;
}

static int __cdecl fake_geometry(int handle, void *rect, int *flag,
                                 int list_selector)
{
    int *values = (int *)rect;
    if (!rect || list_selector != 0)
        return -1;
    if (handle == g_inventory_handle) {
        memcpy(values, g_inv_rect, sizeof(g_inv_rect));
    } else if (handle == g_node_handle) {
        memcpy(values, g_button_rect, sizeof(g_button_rect));
    } else if (handle == g_grid_handle) {
        memcpy(values, g_grid_rect, sizeof(g_grid_rect));
    } else {
        return -1;
    }
    if (flag)
        *flag = 0;
    return 0;
}

static void __cdecl fake_set_geometry(int handle, const void *rect, int flag,
                                      int list_selector)
{
    if (!rect || flag != 0 || list_selector != 0) {
        ++g_failures;
        return;
    }
    /* Repositioning is how both nodes follow the window without touching the
     * node list, so both handles have to be accepted here. */
    if (handle == g_grid_handle) {
        memcpy(g_grid_rect, rect, sizeof(g_grid_rect));
        return;
    }
    if (handle != g_node_handle) {
        ++g_failures;
        return;
    }
    memcpy(g_button_rect, rect, sizeof(g_button_rect));
}

/* The client's own MDA loader. One handle, loaded once and lazily. */
static int __cdecl fake_load_mda(const char *path, void **out_handle)
{
    ++g_load_calls;
    if (!path || !out_handle) {
        ++g_failures;
        return 0;
    }
    strncpy(g_load_path, path, sizeof(g_load_path) - 1);
    g_load_path[sizeof(g_load_path) - 1] = '\0';
    *out_handle = &g_mda_bank;
    return 1;
}

/* BlitMdaFrame selects by animation NAME and returns 0 without drawing when
 * the handle or the name is unknown, which is what g_blit_result models. */
static int __cdecl fake_blit_mda(int handle, const char *anim, int x, int y,
    void *surface, int flags, int a, int b, int c)
{
    (void)surface;
    if (handle != (int)&g_mda_bank || !anim || flags != 0x12 || a || b || c)
        ++g_failures;
    if (g_blit_count < MAX_BLITS) {
        strncpy(g_blit_anim[g_blit_count], anim ? anim : "",
                sizeof(g_blit_anim[0]) - 1);
        g_blit_anim[g_blit_count][sizeof(g_blit_anim[0]) - 1] = '\0';
        g_blit_x[g_blit_count] = x;
        g_blit_y[g_blit_count] = y;
    }
    ++g_blit_count;
    return g_blit_result;
}

static const char *__cdecl fake_game_dir(void)
{
    ++g_service_calls;
    return "C:\\fixture";
}

static void __cdecl fake_log(OSMW_OWNER owner, OSMW_LOG_LEVEL level,
                             const char *message)
{
    ++g_service_calls;
    if (owner != TEST_OWNER || level > OSMW_LOG_ERROR || !message)
        ++g_failures;
}

static int __cdecl fake_config_read_int(OSMW_OWNER owner,
    const char *section, const char *key, int fallback)
{
    ++g_service_calls;
    if (owner != TEST_OWNER)
        ++g_failures;
    if (strcmp(section, "Addons") == 0 && strcmp(key, "Inventory") == 0)
        return g_gate_enabled;
    if (strcmp(section, "Inventory") == 0 &&
            strcmp(key, "SortCooldownSeconds") == 0)
        return g_sort_cooldown_seconds;
    return fallback;
}

static OSMW_RESULT __cdecl fake_config_read_string(OSMW_OWNER owner,
    const char *section, const char *key, const char *fallback,
    char *output, DWORD output_size)
{
    const char *value = fallback ? fallback : "";
    size_t length = strlen(value);
    (void)section;
    (void)key;
    ++g_service_calls;
    if (owner != TEST_OWNER || !output || !output_size)
        return OSMW_E_INVALID_ARGUMENT;
    if (length >= output_size)
        length = output_size - 1;
    memcpy(output, value, length);
    output[length] = '\0';
    return OSMW_OK;
}

static OSMW_RESULT __cdecl fake_config_write_string(OSMW_OWNER owner,
    const char *section, const char *key, const char *value)
{
    ++g_service_calls;
    return owner == TEST_OWNER && section && key && value
        ? OSMW_OK : OSMW_E_INVALID_ARGUMENT;
}

static OSMW_RESULT __cdecl fake_resolve_signature(OSMW_OWNER owner,
    const OSMW_SIGNATURE_V1 *signature, OSMW_RESOLVED_ADDRESS_V1 *output)
{
    union { test_create_fn function; void *address; } create;
    union { test_destroy_fn function; void *address; } destroy;
    union { test_geometry_fn function; void *address; } geometry;
    union { test_set_geometry_fn function; void *address; } set_geometry;
    union { test_load_mda_fn function; void *address; } load_mda;
    union { test_blit_mda_fn function; void *address; } blit_mda;

    ++g_service_calls;
    ++g_resolve_calls;
    if (owner != TEST_OWNER || !signature || !output ||
            signature->struct_size < sizeof(*signature) ||
            output->struct_size < sizeof(*output) || !signature->name)
        return OSMW_E_INVALID_ARGUMENT;
    ZeroMemory(output, sizeof(*output));
    output->struct_size = sizeof(*output);
    if (g_missing_signature &&
            strcmp(signature->name, g_missing_signature) == 0)
        return OSMW_E_NOT_FOUND;

    create.function = fake_create;
    destroy.function = fake_destroy;
    geometry.function = fake_geometry;
    set_geometry.function = fake_set_geometry;
    load_mda.function = fake_load_mda;
    blit_mda.function = fake_blit_mda;
    if (strcmp(signature->name, "UiCreateNode") == 0)
        output->address = create.address;
    else if (strcmp(signature->name, "UiDestroyNode") == 0)
        output->address = destroy.address;
    else if (strcmp(signature->name, "NodeGeometry") == 0)
        output->address = geometry.address;
    else if (strcmp(signature->name, "NodeSetGeometry") == 0)
        output->address = set_geometry.address;
    else if (strcmp(signature->name, "DrawCtx") == 0)
        output->address = &g_draw_context;
    else if (strcmp(signature->name, "InfoFrames") == 0)
        output->address = &g_info_frames;
    else if (strcmp(signature->name, "EventCode") == 0)
        output->address = &g_event_code;
    else if (strcmp(signature->name, "GridScaleX") == 0)
        output->address = &g_grid_scale;
    else if (strcmp(signature->name, "InventoryHandle") == 0)
        output->address = &g_inventory_handle;
    else if (strcmp(signature->name, "LoadMda") == 0)
        output->address = load_mda.address;
    else if (strcmp(signature->name, "BlitMda") == 0)
        output->address = blit_mda.address;
    else if (strcmp(signature->name, "MouseX") == 0)
        output->address = &g_mouse_x;
    else if (strcmp(signature->name, "MouseY") == 0)
        output->address = &g_mouse_y;
    else if (strcmp(signature->name, "InvPage") == 0)
        output->address = &g_inv_page;
    else if (strcmp(signature->name, "InvSlotIndex") == 0)
        output->address = &g_inv_slot;
    else
        output->address = &g_dummy;
    output->match_count = 1;
    output->reference_rva = signature->reference_rva;
    output->match_rva = signature->reference_rva;
    return OSMW_OK;
}

static OSMW_RESULT __cdecl fake_claim_packet(OSMW_OWNER owner,
    DWORD packet_type, OSMW_PACKET_HANDLER_V1 callback, void *context,
    OSMW_TOKEN *token)
{
    ++g_service_calls;
    if (owner != TEST_OWNER || packet_type != PACKET_SNAPSHOT || !callback ||
            !token || g_snapshot_active)
        return OSMW_E_INVALID_ARGUMENT;
    g_snapshot_handler = callback;
    g_snapshot_context = context;
    g_snapshot_active = 1;
    *token = SNAPSHOT_TOKEN;
    return OSMW_OK;
}

static OSMW_RESULT __cdecl fake_observe_packet(OSMW_OWNER owner,
    DWORD packet_type, LONG priority, OSMW_PACKET_OBSERVER_V1 callback,
    void *context, OSMW_TOKEN *token)
{
    ++g_service_calls;
    if (owner != TEST_OWNER || packet_type != PACKET_INVENTORY_DATA ||
            priority != OSMW_PRIORITY_DEFAULT || !callback || !token ||
            g_inventory_active)
        return OSMW_E_INVALID_ARGUMENT;
    g_inventory_observer = callback;
    g_inventory_context = context;
    g_inventory_active = 1;
    *token = INVENTORY_TOKEN;
    return OSMW_OK;
}

static OSMW_RESULT __cdecl fake_subscribe_tick(OSMW_OWNER owner,
    LONG priority, OSMW_UI_TICK_CALLBACK_V1 callback, void *context,
    OSMW_TOKEN *token)
{
    ++g_service_calls;
    if (owner != TEST_OWNER || priority != OSMW_PRIORITY_DEFAULT ||
            !callback || !token || g_tick_active)
        return OSMW_E_INVALID_ARGUMENT;
    g_tick_callback = callback;
    g_tick_context = context;
    g_tick_active = 1;
    *token = TICK_TOKEN;
    return OSMW_OK;
}

static OSMW_RESULT __cdecl fake_unsubscribe(OSMW_OWNER owner, OSMW_TOKEN token)
{
    ++g_service_calls;
    if (owner != TEST_OWNER || g_unsubscribe_count >= 3)
        return OSMW_E_WRONG_OWNER;
    if (token == TICK_TOKEN && g_tick_active)
        g_tick_active = 0;
    else if (token == INVENTORY_TOKEN && g_inventory_active)
        g_inventory_active = 0;
    else if (token == SNAPSHOT_TOKEN && g_snapshot_active)
        g_snapshot_active = 0;
    else
        return OSMW_E_NOT_FOUND;
    g_unsubscribe_order[g_unsubscribe_count++] = token;
    return OSMW_OK;
}

static OSMW_RESULT __cdecl fake_queue_packet(OSMW_OWNER owner,
    DWORD packet_type, DWORD field12, const void *extra, DWORD extra_size)
{
    ++g_service_calls;
    ++g_queue_calls;
    g_last_queue_type = packet_type;
    g_last_queue_field12 = field12;
    g_last_queue_extra_size = extra_size;
    /* Each private type carries its own protocol version in field12; the sort
     * trio and the lock toggle version independently. */
    if (owner != TEST_OWNER ||
            field12 != (packet_type == PACKET_LOCK_TOGGLE
                            ? LOCK_PROTOCOL
                            : PROTOCOL) ||
            extra_size > sizeof(g_last_queue_extra) ||
            (extra_size && !extra)) {
        ++g_failures;
        return OSMW_E_INVALID_ARGUMENT;
    }
    if (extra_size)
        memcpy(g_last_queue_extra, extra, extra_size);
    return g_queue_result;
}

static void reset_fixture(void)
{
    g_service_calls = 0;
    g_gate_enabled = 1;
    g_missing_signature = NULL;
    g_resolve_calls = 0;
    g_snapshot_handler = NULL;
    g_inventory_observer = NULL;
    g_tick_callback = NULL;
    g_snapshot_active = g_inventory_active = g_tick_active = 0;
    g_unsubscribe_count = 0;
    ZeroMemory(g_unsubscribe_order, sizeof(g_unsubscribe_order));
    g_queue_result = OSMW_OK;
    g_queue_calls = 0;
    g_last_queue_type = g_last_queue_field12 = 0;
    g_last_queue_extra_size = 0;
    ZeroMemory(g_last_queue_extra, sizeof(g_last_queue_extra));
    g_grid_scale = 1;
    g_inventory_handle = 44;
    g_inv_rect[0] = 180;
    g_inv_rect[1] = 80;
    g_inv_rect[2] = 556;
    g_inv_rect[3] = 538;
    g_event_code = 0;
    g_draw_context = g_dummy = 0;
    g_info_frames = 1;
    g_create_calls = g_destroy_calls = 0;
    g_sort_cooldown_seconds = 0;
    g_panel_draw = g_panel_input = NULL;
    ZeroMemory(g_button_rect, sizeof(g_button_rect));
    g_mouse_x = g_mouse_y = -1000;
    g_grid_draw = NULL;
    g_grid_input = NULL;
    ZeroMemory(g_grid_rect, sizeof(g_grid_rect));
    g_grid_creates = g_grid_destroys = 0;
    g_grid_open = 0;
    g_inv_page = 0;
    g_inv_slot = 0;
    g_mda_bank = 0;
    g_load_calls = 0;
    g_load_path[0] = '\0';
    g_blit_result = 1;
    g_blit_count = 0;
    ZeroMemory(g_blit_anim, sizeof(g_blit_anim));
    ZeroMemory(g_blit_x, sizeof(g_blit_x));
    ZeroMemory(g_blit_y, sizeof(g_blit_y));
}

static void fill_api(OSMW_CORE_API_V1 *api)
{
    ZeroMemory(api, sizeof(*api));
    api->struct_size = sizeof(*api);
    api->abi_version = OSMW_CORE_ABI_CURRENT;
    api->capabilities = OSMW_CAP_CONFIG | OSMW_CAP_SIGNATURES |
                        OSMW_CAP_UI_TICK | OSMW_CAP_GAME_PACKETS |
                        OSMW_CAP_SEND_QUEUE;
    api->game_dir = fake_game_dir;
    api->log = fake_log;
    api->config_read_int = fake_config_read_int;
    api->config_read_string = fake_config_read_string;
    api->config_write_string = fake_config_write_string;
    api->resolve_signature = fake_resolve_signature;
    api->subscribe_post_ui_tick = fake_subscribe_tick;
    api->unsubscribe = fake_unsubscribe;
    api->claim_game_packet = fake_claim_packet;
    api->observe_game_packet_after = fake_observe_packet;
    api->queue_game_packet = fake_queue_packet;
}

static void invoke_tick(void)
{
    if (g_tick_callback)
        g_tick_callback(TEST_OWNER, g_tick_context, 0);
}

static OSMW_PACKET_ACTION invoke_snapshot(DWORD protocol, const void *extra,
                                          DWORD extra_size)
{
    OSMW_GAME_PACKET_V1 packet;
    ZeroMemory(&packet, sizeof(packet));
    packet.struct_size = sizeof(packet);
    packet.type = PACKET_SNAPSHOT;
    packet.field12 = protocol;
    packet.extra = extra;
    packet.extra_size = extra_size;
    return g_snapshot_handler
        ? g_snapshot_handler(TEST_OWNER, g_snapshot_context, &packet)
        : OSMW_PACKET_PASS;
}

static void invoke_inventory_data(void)
{
    OSMW_GAME_PACKET_V1 packet;
    ZeroMemory(&packet, sizeof(packet));
    packet.struct_size = sizeof(packet);
    packet.type = PACKET_INVENTORY_DATA;
    if (g_inventory_observer)
        g_inventory_observer(TEST_OWNER, g_inventory_context, &packet);
}

/* Park the cursor over one of the two buttons, or off the strip entirely. */
static void hover_button(int button)
{
    if (button < 0) {
        g_mouse_x = g_mouse_y = -1000;
        return;
    }
    g_mouse_x = g_button_rect[0] + button * SPRITE_PITCH + SPRITE_W / 2;
    g_mouse_y = g_button_rect[1] + STRIP_H / 2;
}

/* Park it in the 5px gap between them, which is inside our node but on
 * neither button. */
static void hover_gap(void)
{
    g_mouse_x = g_button_rect[0] + SPRITE_W + 2;
    g_mouse_y = g_button_rect[1] + STRIP_H / 2;
}

/* Sprite mode paints from the post-UiTick overlay, not from the node's draw
 * proc, so a frame is what produces blits. */
static void invoke_draw(void)
{
    g_blit_count = 0;
    invoke_tick();
}

static int drew(int index, const char *anim, int x, int y)
{
    return index < g_blit_count && index < MAX_BLITS &&
           strcmp(g_blit_anim[index], anim) == 0 &&
           g_blit_x[index] == x && g_blit_y[index] == y;
}

static void click_at(void)
{
    if (!g_panel_input)
        return;
    g_event_code = 0;
    g_panel_input();
    g_event_code = 0xF5;
    g_panel_input();
}

static void click_panel(void)
{
    hover_button(0);
    click_at();
}

/* A click delivered to the lock-mode grid node rather than the button strip. */
static void click_grid(void)
{
    if (!g_grid_input)
        return;
    g_event_code = 0;
    g_grid_input();
    g_event_code = 0xF5;
    g_grid_input();
}

int main(int argc, char **argv)
{
    HMODULE module;
    union { FARPROC raw; OSMW_MOD_QUERY_V1 query; } query;
    OSMW_MOD_DESCRIPTOR_V1 descriptor;
    OSMW_CORE_API_V1 api;
    OSMW_RESULT result;
    DWORD snapshot[13];
    unsigned int nonce;

    if (argc != 2) {
        fprintf(stderr, "usage: test_lifecycle.exe osmw_inventory.dll\n");
        return 2;
    }
    reset_fixture();
    module = LoadLibraryA(argv[1]);
    check(module != NULL, "real inventory DLL loads");
    check(g_service_calls == 0, "DllMain is passive");
    if (!module)
        return 1;
    query.raw = GetProcAddress(module, OSMW_MOD_QUERY_EXPORT_NAME);
    check(query.raw != NULL, "undecorated osmw_mod_query resolves");
    if (!query.raw)
        return 1;

    ZeroMemory(&descriptor, sizeof(descriptor));
    check(query.query(OSMW_ABI_MAKE(2, 0), sizeof(descriptor), &descriptor) ==
              OSMW_E_INCOMPATIBLE_ABI,
          "query rejects another ABI major");
    result = query.query(OSMW_CORE_ABI_CURRENT, sizeof(descriptor),
                         &descriptor);
    check(result == OSMW_OK && descriptor.module_id &&
              strcmp(descriptor.module_id, "inventory") == 0 &&
              descriptor.init && descriptor.shutdown,
          "query publishes the inventory ABI 1 descriptor");

    fill_api(&api);
    g_gate_enabled = 0;
    result = descriptor.init(&api, TEST_OWNER);
    check(result == OSMW_MOD_DISABLED && g_resolve_calls == 0 &&
              resource_count() == 0,
          "disabled init resolves nothing and owns no resources");
    check(descriptor.shutdown(&api, TEST_OWNER) == OSMW_OK,
          "disabled module shuts down cleanly");

    reset_fixture();
    fill_api(&api);
    g_missing_signature = "DrawText";
    result = descriptor.init(&api, TEST_OWNER);
    check(result == OSMW_E_NOT_FOUND && resource_count() == 0,
          "required signature miss rolls back before registration");

    /* The four sprite signatures are optional: losing them must cost the
     * artwork and nothing else. The module stays up and the panel reverts to
     * the 80x24 text sidecar outside the window. */
    reset_fixture();
    fill_api(&api);
    g_missing_signature = "BlitMda";
    result = descriptor.init(&api, TEST_OWNER);
    check(result == OSMW_OK && resource_count() == 3,
          "an optional signature miss does not stand the module down");
    invoke_tick();
    check(g_create_calls == 1 &&
              g_button_rect[2] - g_button_rect[0] == 80 &&
              g_button_rect[3] - g_button_rect[1] == 24 &&
              g_button_rect[0] == 560 && g_button_rect[1] == 514,
          "without the blitter the panel falls back to the text sidecar");
    click_at();
    invoke_tick();
    check(g_queue_calls == 1 && g_last_queue_type == PACKET_PREPARE &&
              g_blit_count == 0,
          "the fallback control still sorts and blits nothing");
    check(descriptor.shutdown(&api, TEST_OWNER) == OSMW_OK,
          "fallback module shuts down cleanly");

    reset_fixture();
    fill_api(&api);
    result = descriptor.init(&api, TEST_OWNER);
    check(result == OSMW_OK && g_resolve_calls == SIGNATURE_COUNT &&
              resource_count() == 3,
          "active init resolves 17 signatures and owns three tokens");
    invoke_tick();
    /* The strip is anchored to the window's bottom-right: the fixture window
     * ends at (556, 538), so 556-75 = 481 and 538-59 = 479. */
    check(g_create_calls == 1 && g_panel_input && g_panel_draw &&
              g_button_rect[2] - g_button_rect[0] == STRIP_W &&
              g_button_rect[3] - g_button_rect[1] == STRIP_H &&
              g_button_rect[0] == 481 && g_button_rect[1] == 479 &&
              g_button_rect[2] == 528 && g_button_rect[3] == 500,
          "open stock inventory places the button strip inside the window");

    hover_button(-1);
    invoke_draw();
    check(g_load_calls == 1 &&
              strcmp(g_load_path, "system\\osmw_inventory.mda") == 0 &&
              g_blit_count == 2 && drew(0, "BtnSort", 481, 479) &&
              drew(1, "BtnLock", 507, 479),
          "idle draw blits both buttons from the module's own MDA");

    hover_button(1);
    invoke_draw();
    check(g_blit_count == 2 && drew(0, "BtnSort", 481, 479) &&
              drew(1, "BtnLockHover", 507, 479),
          "cursor over Lock hovers only Lock");

    hover_gap();
    invoke_draw();
    check(g_blit_count == 2 && drew(0, "BtnSort", 481, 479) &&
              drew(1, "BtnLock", 507, 479),
          "the gap between the buttons hovers neither");

    hover_button(0);
    g_event_code = 0xF2;
    invoke_draw();
    g_event_code = 0;
    check(drew(0, "BtnSortPress", 481, 479) && drew(1, "BtnLock", 507, 479),
          "held button draws the pressed sprite");

    check(g_load_calls == 1, "the sprite bank loads once, not per frame");

    /* The raise is edge-triggered on hover, so it cannot be what keeps the
     * strip under Vendor. Moving the window with the cursor far away must
     * still reposition it. */
    hover_button(-1);
    g_inv_rect[0] += 40; g_inv_rect[2] += 40;
    g_inv_rect[1] -= 30; g_inv_rect[3] -= 30;
    invoke_tick();
    check(g_button_rect[0] == 521 && g_button_rect[1] == 449 &&
              g_button_rect[2] == 568 && g_button_rect[3] == 470,
          "the strip follows the window when it is dragged");
    invoke_draw();
    check(drew(0, "BtnSort", 521, 449) && drew(1, "BtnLock", 547, 449),
          "the sprites follow the moved strip");
    g_inv_rect[0] -= 40; g_inv_rect[2] -= 40;
    g_inv_rect[1] += 30; g_inv_rect[3] += 30;
    invoke_tick();

    /* ---- Lock ---------------------------------------------------------- */
    /* No mode and no node: the button toggles whichever slot the client is
     * already treating as active. Nothing covers the grid, so the stock hover
     * and tooltip path - where the padlock is drawn - is untouched. */
    g_inv_slot = 5;
    hover_button(1);
    click_at();
    invoke_tick();
    check(g_queue_calls == 1 && g_last_queue_type == PACKET_LOCK_TOGGLE &&
              g_last_queue_field12 == LOCK_PROTOCOL &&
              g_last_queue_extra_size == 4 &&
              g_last_queue_extra[0] == 5 && g_event_code == 0,
          "Lock toggles the client's active slot");
    check(g_grid_creates == 0,
          "Lock creates no node over the grid");

    /* A wire slot is global across the pages. */
    g_inv_page = 2;
    g_inv_slot = 7;
    click_at();
    invoke_tick();
    check(g_queue_calls == 2 && g_last_queue_extra[0] == 55,
          "the active slot is page-adjusted before it goes on the wire");
    g_inv_page = 0;

    /* Freshly opened, nothing clicked yet: the first slot is the active one. */
    g_inv_slot = -1;
    click_at();
    invoke_tick();
    check(g_queue_calls == 3 && g_last_queue_extra[0] == 0,
          "with nothing selected the button acts on slot 0");

    /* Out of range is treated the same way rather than sent on. */
    g_inv_slot = 99;
    click_at();
    invoke_tick();
    check(g_queue_calls == 4 && g_last_queue_extra[0] == 0,
          "an out-of-range active slot falls back to slot 0");
    g_inv_slot = 0;

    hover_button(-1);
    invoke_draw();
    check(drew(1, "BtnLock", 507, 479),
          "Lock is a plain button and holds no pressed state");
    g_queue_calls = 0;
    g_last_queue_type = 0;

    hover_gap();
    click_at();
    invoke_tick();
    check(g_event_code == 0xF5 && g_queue_calls == 0,
          "a click in the gap is left for the stock window");
    g_event_code = 0;

    /* The regression that made the inventory unusable: the module is the top
     * node while hovered, so it is dispatched clicks it does not own. It must
     * leave every one of them alone. */
    hover_button(-1);
    click_at();
    invoke_tick();
    check(g_event_code == 0xF5 && g_queue_calls == 0,
          "a click off the strip is never consumed");
    g_event_code = 0;

    click_panel();
    check(g_event_code == 0, "sort control consumes its stock click event");
    hover_button(-1);
    invoke_draw();
    check(drew(0, "BtnSortPress", 481, 479) && drew(1, "BtnLock", 507, 479),
          "Sort holds the pressed sprite for the round trip, Lock does not");
    invoke_tick();
    check(g_queue_calls == 1 && g_last_queue_type == PACKET_PREPARE &&
              g_last_queue_field12 == PROTOCOL &&
              g_last_queue_extra_size == 0,
          "click defers exact SortPrepare through the core send queue");

    snapshot[0] = 99;
    snapshot[1] = 2U | (4U << 8);
    snapshot[2] = 100;
    snapshot[3] = 3;
    snapshot[4] = 7U | (0U << 8);
    snapshot[5] = 11006;
    snapshot[6] = 1;
    snapshot[7] = 5U | (2U << 8);
    snapshot[8] = 50;
    snapshot[9] = 9;
    snapshot[10] = 9U | (4U << 8);
    snapshot[11] = 100;
    snapshot[12] = 8;
    check(invoke_snapshot(PROTOCOL, snapshot, sizeof(snapshot)) ==
              OSMW_PACKET_CONSUME,
          "private metadata snapshot is exclusively consumed");
    invoke_tick();
    memcpy(&nonce, g_last_queue_extra, sizeof(nonce));
    check(g_queue_calls == 2 && g_last_queue_type == PACKET_APPLY &&
              g_last_queue_extra_size == 8 && nonce == 99 &&
              g_last_queue_extra[4] == 7 &&
              g_last_queue_extra[5] == 5 &&
              g_last_queue_extra[6] == 9 &&
              g_last_queue_extra[7] == 2,
          "client computes category/base-ID/stack order and proposes slots");

    invoke_inventory_data();
    click_panel();
    invoke_tick();
    check(g_queue_calls == 3 && g_last_queue_type == PACKET_PREPARE,
          "authoritative InventoryData completes the operation for reuse");
    /* An unknown future protocol, not merely a different one: PROTOCOL itself
     * moves when the wire format changes. */
    check(invoke_snapshot(PROTOCOL + 1U, snapshot, sizeof(snapshot)) ==
              OSMW_PACKET_CONSUME,
          "malformed private snapshot is consumed without an apply");
    invoke_tick();
    check(g_queue_calls == 3,
          "malformed snapshot cannot advance protocol state");
    invoke_inventory_data();

    click_panel();
    g_queue_result = OSMW_E_QUEUE_FULL;
    invoke_tick();
    check(g_queue_calls == 4, "full queue leaves prepare pending");
    invoke_tick();
    check(g_queue_calls == 4, "queue retry is bounded to 500 ms");
    Sleep(550);
    g_queue_result = OSMW_OK;
    invoke_tick();
    check(g_queue_calls == 5 && g_last_queue_type == PACKET_PREPARE,
          "pending prepare retries after the bounded delay");

    /* Protocol 2: a locked entry (bit 16 of dword 0) keeps the slot it is in,
     * and the free entries pack into the lowest slots left over. Same four
     * items as above, but source slot 5 is locked - so the free ones sort to
     * 7, 9, 2 and take destinations 0, 1, 2, while 5 stays at 5. Emitted by
     * ascending destination that is 7, 9, 2, 5. */
    snapshot[0] = 123;
    snapshot[1] = 2U | (4U << 8);
    snapshot[2] = 100;
    snapshot[3] = 3;
    snapshot[4] = 7U | (0U << 8);
    snapshot[5] = 11006;
    snapshot[6] = 1;
    snapshot[7] = 5U | (2U << 8) | 0x10000U;   /* locked */
    snapshot[8] = 50;
    snapshot[9] = 9;
    snapshot[10] = 9U | (4U << 8);
    snapshot[11] = 100;
    snapshot[12] = 8;
    check(invoke_snapshot(PROTOCOL, snapshot, sizeof(snapshot)) ==
              OSMW_PACKET_CONSUME,
          "a snapshot carrying a locked entry is accepted");
    invoke_tick();
    memcpy(&nonce, g_last_queue_extra, sizeof(nonce));
    check(g_queue_calls == 6 && g_last_queue_type == PACKET_APPLY &&
              g_last_queue_extra_size == 8 && nonce == 123 &&
              g_last_queue_extra[4] == 7 &&
              g_last_queue_extra[5] == 9 &&
              g_last_queue_extra[6] == 2 &&
              g_last_queue_extra[7] == 5,
          "a locked slot is pinned and the rest pack around it");
    invoke_inventory_data();

    result = descriptor.shutdown(&api, TEST_OWNER);
    check(result == OSMW_OK && resource_count() == 0 &&
              g_unsubscribe_count == 3 &&
              g_destroy_calls == g_create_calls,
          "shutdown returns all tokens and destroys the companion node");
    check(g_unsubscribe_order[0] == TICK_TOKEN &&
              g_unsubscribe_order[1] == INVENTORY_TOKEN &&
              g_unsubscribe_order[2] == SNAPSHOT_TOKEN,
          "shutdown revokes tick, observer, then exclusive claim");
    /* The cooldown is off for every block above so the protocol flow can click
     * twice. Turn it on and prove one sort per window, with the art saying so. */
    reset_fixture();
    fill_api(&api);
    g_sort_cooldown_seconds = 30;
    result = descriptor.init(&api, TEST_OWNER);
    check(result == OSMW_OK, "cooldown build initialises");
    invoke_tick();
    click_panel();
    invoke_tick();
    check(g_queue_calls == 1 && g_last_queue_type == PACKET_PREPARE,
          "the first sort of a cooldown window is sent");
    invoke_inventory_data();
    click_panel();
    invoke_tick();
    check(g_queue_calls == 1,
          "a second sort inside the cooldown queues nothing");
    hover_button(-1);
    invoke_draw();
    check(drew(0, "BtnSortPress", 481, 479) && drew(1, "BtnLock", 507, 479),
          "Sort stays pressed while its cooldown runs");
    check(descriptor.shutdown(&api, TEST_OWNER) == OSMW_OK,
          "cooldown build shuts down cleanly");

    FreeLibrary(module);

    if (g_failures) {
        printf("FAILED: %d inventory lifecycle check(s)\n", g_failures);
        return 1;
    }
    printf("PASSED: managed inventory sort lifecycle and protocol\n");
    return 0;
}
