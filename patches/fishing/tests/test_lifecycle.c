/* Real-DLL host fixture for managed fishing lifecycle and protocol routing. */
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "osmw_core_api.h"

#define TEST_OWNER ((OSMW_OWNER)0x6200u)
#define SNAPSHOT_TOKEN ((OSMW_TOKEN)0x6201u)
#define MAP_TOKEN ((OSMW_TOKEN)0x6202u)
#define TICK_TOKEN ((OSMW_TOKEN)0x6203u)
#define FISHING_SIGNATURE_COUNT 15
#define PACKET_MAP_DATA 0x00030100u
#define PACKET_HELLO 0x7F000040u
#define PACKET_SNAPSHOT 0x7F000041u
#define PACKET_ACTION 0x7F000042u
#define FISHING_PROTOCOL 1u
#define SNAPSHOT_EXTRA 112u
#define ACTION_EXTRA 12u

typedef int (__cdecl *test_create_fn)(
    int, int, int, int, void *, void *, int, int);
typedef void (__cdecl *test_destroy_fn)(int, int);
typedef int (__cdecl *test_geometry_fn)(int, void *, int *, int);
typedef void (__cdecl *test_loadmda_fn)(const char *, void **);
typedef int (__cdecl *test_blit_fn)(
    int, const char *, int, int, void *, int, int, int, int);

static int g_failures;
static int g_service_calls;
static int g_gate_enabled = 1;
static const char *g_missing_signature;
static int g_resolve_calls;
static int g_saw_shared_signature;
static int g_ignored_logs;
static OSMW_PACKET_HANDLER_V1 g_snapshot_handler;
static void *g_snapshot_context;
static OSMW_PACKET_OBSERVER_V1 g_map_observer;
static void *g_map_context;
static OSMW_UI_TICK_CALLBACK_V1 g_tick_callback;
static void *g_tick_context;
static int g_snapshot_active;
static int g_map_active;
static int g_tick_active;
static OSMW_TOKEN g_unsubscribe_order[3];
static int g_unsubscribe_count;
static OSMW_RESULT g_queue_result = OSMW_OK;
static int g_queue_calls;
static DWORD g_last_queue_type;
static DWORD g_last_queue_field12;
static DWORD g_last_queue_extra_size;
static unsigned char g_last_queue_extra[16];
static int g_bad_queue;
static int g_grid_scale = 1;
static DWORD g_dummy_data;
static int g_create_calls;
static int g_destroy_calls;
static int g_node_handle = 81;
static void *g_draw_proc;
static void *g_input_proc;
static int g_mouse_x = 200;
static int g_mouse_y = 370;
static unsigned char g_event_code;
static int g_load_calls;
static int g_blit_calls;
static int g_load_fails;
static int g_blit_fails;
static void *g_blit_surface = (void *)0x51;
static char g_last_blit_anim[32];
static char g_draw_dump[1024];
static int g_draw_calls;

static void check(int condition, const char *message)
{
    if (condition) {
        printf("PASS: %s\n", message);
    } else {
        printf("FAIL: %s\n", message);
        ++g_failures;
    }
}

static int resource_count(void)
{
    return g_snapshot_active + g_map_active + g_tick_active;
}

static int __cdecl fake_create(
    int left, int top, int right, int bottom,
    void *draw_proc, void *input_proc, int arg4, int list_selector)
{
    (void)arg4;
    if (left != 172 || top != 160 || right != 628 || bottom != 440 ||
            list_selector != 0)
        ++g_failures;
    g_draw_proc = draw_proc;
    g_input_proc = input_proc;
    ++g_create_calls;
    return g_node_handle;
}

static void __cdecl fake_destroy(int handle, int list_selector)
{
    if (handle != g_node_handle || list_selector != 0)
        ++g_failures;
    ++g_destroy_calls;
}

static int __cdecl fake_geometry(
    int handle, void *rect, int *flag, int list_selector)
{
    int *values = (int *)rect;
    if (handle != g_node_handle || rect == NULL || list_selector != 0)
        return -1;
    values[0] = 172;
    values[1] = 160;
    values[2] = 628;
    values[3] = 440;
    if (flag != NULL)
        *flag = 0;
    return 0;
}

static void __cdecl fake_loadmda(const char *path, void **out_handle)
{
    ++g_load_calls;
    if (out_handle == NULL)
        return;
    if (g_load_fails || path == NULL) {
        *out_handle = NULL;
        return;
    }
    *out_handle = (void *)0x71;
}

static int __cdecl fake_blit(
    int handle, const char *anim, int x, int y, void *surface,
    int flags, int a, int b, int c)
{
    (void)x;
    (void)y;
    (void)flags;
    (void)a;
    (void)b;
    (void)c;
    ++g_blit_calls;
    ZeroMemory(g_last_blit_anim, sizeof(g_last_blit_anim));
    if (anim != NULL)
        lstrcpynA(g_last_blit_anim, anim, sizeof(g_last_blit_anim));
    if (g_blit_fails || handle == 0 || anim == NULL || surface == NULL)
        return 0;
    return 1;
}

static void __cdecl fake_drawtext(
    int x, int y, const char *text, int mode, void *ctx)
{
    size_t used;
    size_t room;
    size_t copy;

    (void)x;
    (void)y;
    (void)mode;
    (void)ctx;
    ++g_draw_calls;
    if (text == NULL || text[0] == '\0')
        return;
    used = strlen(g_draw_dump);
    if (used + 1 >= sizeof(g_draw_dump))
        return;
    if (used != 0)
        g_draw_dump[used++] = '|';
    room = sizeof(g_draw_dump) - used - 1;
    copy = strlen(text);
    if (copy > room)
        copy = room;
    memcpy(g_draw_dump + used, text, copy);
    g_draw_dump[used + copy] = '\0';
}

static const char *__cdecl fake_game_dir(void)
{
    ++g_service_calls;
    return "C:\\fixture";
}

static void __cdecl fake_log(
    OSMW_OWNER owner, OSMW_LOG_LEVEL level, const char *message)
{
    ++g_service_calls;
    if (owner != TEST_OWNER || level > OSMW_LOG_ERROR || message == NULL) {
        ++g_failures;
        return;
    }
    if (strstr(message, "ignored ") != NULL)
        ++g_ignored_logs;
}

static int __cdecl fake_config_read_int(
    OSMW_OWNER owner, const char *section, const char *key, int fallback)
{
    ++g_service_calls;
    if (owner != TEST_OWNER)
        ++g_failures;
    if (strcmp(section, "Addons") == 0 && strcmp(key, "Fishing") == 0)
        return g_gate_enabled;
    return fallback;
}

static OSMW_RESULT __cdecl fake_resolve_signature(
    OSMW_OWNER owner, const OSMW_SIGNATURE_V1 *signature,
    OSMW_RESOLVED_ADDRESS_V1 *output)
{
    union { test_create_fn function; void *address; } create_address;
    union { test_destroy_fn function; void *address; } destroy_address;
    union { test_geometry_fn function; void *address; } geometry_address;
    union { test_loadmda_fn function; void *address; } load_address;
    union { test_blit_fn function; void *address; } blit_address;
    union { void (__cdecl *function)(int, int, const char *, int, void *); void *address; } draw_address;

    ++g_service_calls;
    ++g_resolve_calls;
    if (owner != TEST_OWNER || signature == NULL || output == NULL ||
            signature->struct_size < sizeof(*signature) ||
            output->struct_size < sizeof(*output) ||
            signature->flags != 0 || signature->name == NULL)
        return OSMW_E_INVALID_ARGUMENT;
    if (strcmp(signature->name, "UiTick") == 0 ||
            strcmp(signature->name, "HandlePacket") == 0 ||
            strcmp(signature->name, "SendPacket") == 0) {
        g_saw_shared_signature = 1;
        return OSMW_E_INTERNAL;
    }

    ZeroMemory(output, sizeof(*output));
    output->struct_size = sizeof(*output);
    if (g_missing_signature != NULL &&
            strcmp(signature->name, g_missing_signature) == 0)
        return OSMW_E_NOT_FOUND;

    create_address.function = fake_create;
    destroy_address.function = fake_destroy;
    geometry_address.function = fake_geometry;
    load_address.function = fake_loadmda;
    blit_address.function = fake_blit;
    draw_address.function = fake_drawtext;
    if (strcmp(signature->name, "UiCreateNode") == 0)
        output->address = create_address.address;
    else if (strcmp(signature->name, "UiDestroyNode") == 0)
        output->address = destroy_address.address;
    else if (strcmp(signature->name, "NodeGeometry") == 0)
        output->address = geometry_address.address;
    else if (strcmp(signature->name, "GridScaleX") == 0)
        output->address = &g_grid_scale;
    else if (strcmp(signature->name, "MouseX") == 0)
        output->address = &g_mouse_x;
    else if (strcmp(signature->name, "MouseY") == 0)
        output->address = &g_mouse_y;
    else if (strcmp(signature->name, "EventCode") == 0)
        output->address = &g_event_code;
    else if (strcmp(signature->name, "LoadMda") == 0)
        output->address = load_address.address;
    else if (strcmp(signature->name, "BlitMda") == 0)
        output->address = blit_address.address;
    else if (strcmp(signature->name, "DrawText") == 0)
        output->address = draw_address.address;
    else if (strcmp(signature->name, "DrawCtx") == 0)
        output->address = &g_blit_surface;
    else
        output->address = &g_dummy_data;
    output->match_count = 1;
    output->reference_rva = signature->reference_rva;
    output->match_rva = signature->reference_rva;
    return OSMW_OK;
}

static OSMW_RESULT __cdecl fake_claim_packet(
    OSMW_OWNER owner, DWORD packet_type, OSMW_PACKET_HANDLER_V1 callback,
    void *context, OSMW_TOKEN *token)
{
    ++g_service_calls;
    if (owner != TEST_OWNER || packet_type != PACKET_SNAPSHOT ||
            callback == NULL || token == NULL || g_snapshot_active)
        return OSMW_E_INVALID_ARGUMENT;
    g_snapshot_handler = callback;
    g_snapshot_context = context;
    g_snapshot_active = 1;
    *token = SNAPSHOT_TOKEN;
    return OSMW_OK;
}

static OSMW_RESULT __cdecl fake_observe_packet(
    OSMW_OWNER owner, DWORD packet_type, LONG priority,
    OSMW_PACKET_OBSERVER_V1 callback, void *context, OSMW_TOKEN *token)
{
    ++g_service_calls;
    if (owner != TEST_OWNER || packet_type != PACKET_MAP_DATA ||
            priority != OSMW_PRIORITY_DEFAULT || callback == NULL ||
            token == NULL || g_map_active)
        return OSMW_E_INVALID_ARGUMENT;
    g_map_observer = callback;
    g_map_context = context;
    g_map_active = 1;
    *token = MAP_TOKEN;
    return OSMW_OK;
}

static OSMW_RESULT __cdecl fake_subscribe_tick(
    OSMW_OWNER owner, LONG priority, OSMW_UI_TICK_CALLBACK_V1 callback,
    void *context, OSMW_TOKEN *token)
{
    ++g_service_calls;
    if (owner != TEST_OWNER || priority != OSMW_PRIORITY_DEFAULT ||
            callback == NULL || token == NULL || g_tick_active)
        return OSMW_E_INVALID_ARGUMENT;
    g_tick_callback = callback;
    g_tick_context = context;
    g_tick_active = 1;
    *token = TICK_TOKEN;
    return OSMW_OK;
}

static OSMW_RESULT __cdecl fake_unsubscribe(
    OSMW_OWNER owner, OSMW_TOKEN token)
{
    ++g_service_calls;
    if (owner != TEST_OWNER || g_unsubscribe_count >= 3)
        return OSMW_E_WRONG_OWNER;
    if (token == TICK_TOKEN && g_tick_active)
        g_tick_active = 0;
    else if (token == MAP_TOKEN && g_map_active)
        g_map_active = 0;
    else if (token == SNAPSHOT_TOKEN && g_snapshot_active)
        g_snapshot_active = 0;
    else
        return OSMW_E_NOT_FOUND;
    g_unsubscribe_order[g_unsubscribe_count++] = token;
    return OSMW_OK;
}

static OSMW_RESULT __cdecl fake_queue_packet(
    OSMW_OWNER owner, DWORD packet_type, DWORD field12,
    const void *extra, DWORD extra_size)
{
    ++g_service_calls;
    ++g_queue_calls;
    g_last_queue_type = packet_type;
    g_last_queue_field12 = field12;
    g_last_queue_extra_size = extra_size;
    ZeroMemory(g_last_queue_extra, sizeof(g_last_queue_extra));
    if (extra_size && extra && extra_size <= sizeof(g_last_queue_extra))
        memcpy(g_last_queue_extra, extra, extra_size);
    if (owner != TEST_OWNER || field12 != FISHING_PROTOCOL)
        g_bad_queue = 1;
    else if (packet_type == PACKET_HELLO && (extra != NULL || extra_size != 0))
        g_bad_queue = 1;
    else if (packet_type == PACKET_ACTION && extra_size != ACTION_EXTRA)
        g_bad_queue = 1;
    else if (packet_type != PACKET_HELLO && packet_type != PACKET_ACTION)
        g_bad_queue = 1;
    return g_queue_result;
}

static void reset_fixture(void)
{
    g_service_calls = 0;
    g_gate_enabled = 1;
    g_missing_signature = NULL;
    g_resolve_calls = 0;
    g_saw_shared_signature = 0;
    g_ignored_logs = 0;
    g_snapshot_handler = NULL;
    g_snapshot_context = NULL;
    g_map_observer = NULL;
    g_map_context = NULL;
    g_tick_callback = NULL;
    g_tick_context = NULL;
    g_snapshot_active = 0;
    g_map_active = 0;
    g_tick_active = 0;
    ZeroMemory(g_unsubscribe_order, sizeof(g_unsubscribe_order));
    g_unsubscribe_count = 0;
    g_queue_result = OSMW_OK;
    g_queue_calls = 0;
    g_last_queue_type = 0;
    g_last_queue_field12 = 0;
    g_last_queue_extra_size = 0;
    g_bad_queue = 0;
    g_grid_scale = 1;
    g_dummy_data = 0;
    g_create_calls = 0;
    g_destroy_calls = 0;
    g_draw_proc = NULL;
    g_input_proc = NULL;
    g_mouse_x = 200;
    g_mouse_y = 370;
    g_event_code = 0;
    g_load_calls = 0;
    g_blit_calls = 0;
    g_load_fails = 0;
    g_blit_fails = 0;
    ZeroMemory(g_last_blit_anim, sizeof(g_last_blit_anim));
    ZeroMemory(g_draw_dump, sizeof(g_draw_dump));
    g_draw_calls = 0;
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
    api->resolve_signature = fake_resolve_signature;
    api->subscribe_post_ui_tick = fake_subscribe_tick;
    api->unsubscribe = fake_unsubscribe;
    api->claim_game_packet = fake_claim_packet;
    api->observe_game_packet_after = fake_observe_packet;
    api->queue_game_packet = fake_queue_packet;
}

static void invoke_tick(void)
{
    if (g_tick_callback != NULL)
        g_tick_callback(TEST_OWNER, g_tick_context, 0);
}

static OSMW_PACKET_ACTION invoke_snapshot(
    DWORD protocol, const void *extra, DWORD extra_size)
{
    OSMW_GAME_PACKET_V1 packet;
    ZeroMemory(&packet, sizeof(packet));
    packet.struct_size = sizeof(packet);
    packet.type = PACKET_SNAPSHOT;
    packet.field12 = protocol;
    packet.extra = extra;
    packet.extra_size = extra_size;
    return g_snapshot_handler != NULL
        ? g_snapshot_handler(TEST_OWNER, g_snapshot_context, &packet)
        : OSMW_PACKET_PASS;
}

static void invoke_map_data(void)
{
    OSMW_GAME_PACKET_V1 packet;
    ZeroMemory(&packet, sizeof(packet));
    packet.struct_size = sizeof(packet);
    packet.type = PACKET_MAP_DATA;
    if (g_map_observer != NULL)
        g_map_observer(TEST_OWNER, g_map_context, &packet);
}

static void invoke_input(void)
{
    if (g_input_proc != NULL)
        ((void (__cdecl *)(void))g_input_proc)();
}

static void invoke_draw(void)
{
    ZeroMemory(g_draw_dump, sizeof(g_draw_dump));
    g_draw_calls = 0;
    g_blit_calls = 0;
    ZeroMemory(g_last_blit_anim, sizeof(g_last_blit_anim));
    if (g_draw_proc != NULL)
        ((void (__cdecl *)(void))g_draw_proc)();
}

static void write_u32(unsigned char *bytes, unsigned int offset, unsigned int value)
{
    memcpy(bytes + offset, &value, 4);
}

static void fill_hooked(unsigned char *extra, unsigned int revision)
{
    ZeroMemory(extra, SNAPSHOT_EXTRA);
    extra[0] = 2;
    extra[1] = 1;
    extra[2] = 30;
    extra[3] = 22;
    write_u32(extra, 4, 0x55);
    write_u32(extra, 8, revision);
    extra[12] = 3;
    extra[13] = 4;
    extra[14] = (unsigned char)-1;
    extra[15] = 0;
    extra[16] = 5;
    extra[18] = 1;
    memcpy(extra + 32, "A steady tug.", 13);
}

static void fill_phase(unsigned char *extra, unsigned int phase,
                       unsigned int revision, int species_index,
                       unsigned int flags)
{
    ZeroMemory(extra, SNAPSHOT_EXTRA);
    extra[0] = (unsigned char)phase;
    extra[14] = (unsigned char)species_index;
    extra[15] = (unsigned char)flags;
    write_u32(extra, 4, 0x55);
    write_u32(extra, 8, revision);
}

int main(int argc, char **argv)
{
    HMODULE module;
    union { FARPROC raw; OSMW_MOD_QUERY_V1 query; } query_address;
    OSMW_MOD_DESCRIPTOR_V1 descriptor;
    OSMW_CORE_API_V1 api;
    OSMW_RESULT result;
    OSMW_PACKET_ACTION action;
    unsigned char snapshot[SNAPSHOT_EXTRA];
    unsigned int command;
    int queued_before;

    if (argc != 2) {
        fprintf(stderr, "usage: test_lifecycle.exe osmw_fishing.dll\n");
        return 2;
    }

    module = LoadLibraryA(argv[1]);
    if (module == NULL) {
        fprintf(stderr, "LoadLibrary failed (%lu)\n", GetLastError());
        return 1;
    }
    query_address.raw = GetProcAddress(module, "osmw_mod_query");
    check(query_address.raw != NULL, "osmw_mod_query is exported");
    check(GetProcAddress(module, "DllMain") == NULL,
          "DllMain is not exported");

    ZeroMemory(&descriptor, sizeof(descriptor));
    result = query_address.query(
        OSMW_CORE_ABI_CURRENT, sizeof(descriptor), &descriptor);
    check(result == OSMW_OK &&
              strcmp(descriptor.module_id, "fishing") == 0 &&
              descriptor.init != NULL && descriptor.shutdown != NULL,
          "query fills the fishing descriptor");
    check(g_service_calls == 0, "query is side-effect free");

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

    reset_fixture();
    fill_api(&api);
    g_missing_signature = "LoadMda";
    result = descriptor.init(&api, TEST_OWNER);
    check(result == OSMW_OK && g_resolve_calls == FISHING_SIGNATURE_COUNT &&
              !g_saw_shared_signature && resource_count() == 3,
          "optional MDA miss still activates the module");
    fill_hooked(snapshot, 1);
    invoke_snapshot(FISHING_PROTOCOL, snapshot, sizeof(snapshot));
    invoke_tick();
    invoke_tick();
    invoke_draw();
    check(g_create_calls == 1 && g_blit_calls == 0 &&
              strstr(g_draw_dump, "<==") != NULL,
          "missing LoadMda keeps geometry and uses text/arrow fallback");
    check(descriptor.shutdown(&api, TEST_OWNER) == OSMW_OK,
          "optional-MDA module shuts down cleanly");

    reset_fixture();
    fill_api(&api);
    result = descriptor.init(&api, TEST_OWNER);
    check(result == OSMW_OK && g_resolve_calls == FISHING_SIGNATURE_COUNT &&
              resource_count() == 3,
          "active init resolves 15 private signatures and owns three tokens");

    g_queue_result = OSMW_E_QUEUE_FULL;
    invoke_tick();
    check(g_queue_calls == 1 && !g_bad_queue &&
              g_last_queue_type == PACKET_HELLO &&
              g_last_queue_field12 == FISHING_PROTOCOL,
          "first world tick queues the exact capability hello through core");
    invoke_tick();
    check(g_queue_calls == 1, "failed hello observes the 500 ms retry bound");
    Sleep(550);
    g_queue_result = OSMW_OK;
    invoke_tick();
    check(g_queue_calls == 2, "failed hello retries and clears on queue success");

    queued_before = g_queue_calls;
    invoke_map_data();
    check(g_queue_calls == queued_before,
          "post-stock MapData observer never sends recursively");
    Sleep(550);
    invoke_tick();
    check(g_queue_calls == queued_before + 1,
          "MapData refresh is queued from the following post-tick callback");

    fill_hooked(snapshot, 4);
    action = invoke_snapshot(FISHING_PROTOCOL, snapshot, sizeof(snapshot));
    check(action == OSMW_PACKET_CONSUME, "valid private snapshot is consumed");
    invoke_tick();
    check(g_create_calls == 1, "hooked snapshot opens the modal after UiTick");
    invoke_draw();
    check(g_blit_calls == 1 && strcmp(g_last_blit_anim, "ThrashLeft") == 0,
          "hooked left cue blits ThrashLeft");
    g_blit_fails = 1;
    invoke_draw();
    check(g_blit_calls == 1 && strstr(g_draw_dump, "<==") != NULL,
          "blit refusal keeps geometry and uses text/arrow fallback");
    g_blit_fails = 0;

    fill_phase(snapshot, 1, 10, -1, 1);
    invoke_snapshot(FISHING_PROTOCOL, snapshot, sizeof(snapshot));
    invoke_draw();
    check(strcmp(g_last_blit_anim, "Casting") == 0, "ready phase blits Casting");
    fill_phase(snapshot, 3, 11, -1, 1);
    invoke_snapshot(FISHING_PROTOCOL, snapshot, sizeof(snapshot));
    invoke_draw();
    check(strcmp(g_last_blit_anim, "Casting") == 0, "no-bite phase blits Casting");
    fill_phase(snapshot, 4, 12, -1, 1);
    invoke_snapshot(FISHING_PROTOCOL, snapshot, sizeof(snapshot));
    invoke_draw();
    check(strcmp(g_last_blit_anim, "Snap") == 0, "snap phase blits Snap");
    fill_phase(snapshot, 5, 13, -1, 1);
    invoke_snapshot(FISHING_PROTOCOL, snapshot, sizeof(snapshot));
    invoke_draw();
    check(strcmp(g_last_blit_anim, "Escape") == 0, "escape phase blits Escape");
    fill_phase(snapshot, 6, 14, 4, 5);
    invoke_snapshot(FISHING_PROTOCOL, snapshot, sizeof(snapshot));
    invoke_draw();
    check(strcmp(g_last_blit_anim, "Reveal4") == 0,
          "landed species 4 blits Reveal4");
    fill_phase(snapshot, 7, 15, -1, 0);
    invoke_snapshot(FISHING_PROTOCOL, snapshot, sizeof(snapshot));
    invoke_draw();
    check(strcmp(g_last_blit_anim, "Casting") == 0, "no-bait phase blits Casting");
    fill_phase(snapshot, 8, 16, -1, 1);
    invoke_snapshot(FISHING_PROTOCOL, snapshot, sizeof(snapshot));
    invoke_draw();
    check(strcmp(g_last_blit_anim, "Casting") == 0, "slack-line phase blits Casting");
    fill_hooked(snapshot, 4);
    snapshot[1] = 2;
    invoke_snapshot(FISHING_PROTOCOL, snapshot, sizeof(snapshot));
    invoke_draw();
    check(strcmp(g_last_blit_anim, "ThrashRight") == 0,
          "hooked right cue blits ThrashRight");

    fill_phase(snapshot, 1, 20, -1, 1);
    invoke_snapshot(FISHING_PROTOCOL, snapshot, sizeof(snapshot));
    queued_before = g_queue_calls;
    g_event_code = 0xF5;
    invoke_input();
    memcpy(&command, g_last_queue_extra, 4);
    check(g_queue_calls == queued_before + 1 && command == 1,
          "ready Cast button queues command 1");
    fill_phase(snapshot, 1, 21, -1, 1);
    invoke_snapshot(FISHING_PROTOCOL, snapshot, sizeof(snapshot));
    g_mouse_x = 508;
    g_mouse_y = 366;
    g_event_code = 0;
    invoke_input();
    queued_before = g_queue_calls;
    g_event_code = 0xF5;
    invoke_input();
    memcpy(&command, g_last_queue_extra, 4);
    check(g_queue_calls == queued_before + 1 && command == 6,
          "Pack Up button queues command 6");
    g_mouse_x = 200;
    g_mouse_y = 370;
    fill_hooked(snapshot, 4);
    invoke_snapshot(FISHING_PROTOCOL, snapshot, sizeof(snapshot));
    g_event_code = 0;
    invoke_input();

    queued_before = g_queue_calls;
    g_event_code = 0xF5;
    invoke_input();
    check(g_queue_calls == queued_before + 1 &&
              g_last_queue_type == PACKET_ACTION &&
              g_last_queue_extra_size == ACTION_EXTRA,
          "first click queues a revision-locked action");
    memcpy(&command, g_last_queue_extra, 4);
    check(command == 2, "left-side button sends reel-left");
    g_event_code = 0;
    invoke_input();
    g_event_code = 0xF5;
    invoke_input();
    check(g_queue_calls == queued_before + 1,
          "a second click on the same revision does not queue");

    action = invoke_snapshot(99, snapshot, sizeof(snapshot));
    check(action == OSMW_PACKET_CONSUME, "unknown snapshot version is consumed");
    action = invoke_snapshot(FISHING_PROTOCOL, snapshot, 16);
    check(action == OSMW_PACKET_CONSUME, "short snapshot is consumed");
    action = invoke_snapshot(FISHING_PROTOCOL, NULL, sizeof(snapshot));
    check(action == OSMW_PACKET_CONSUME, "null snapshot payload is consumed");
    check(g_ignored_logs >= 3, "malformed snapshots are ignored without state use");
    invoke_draw();
    check(strcmp(g_last_blit_anim, "ThrashLeft") == 0,
          "malformed snapshots leave the last valid view in place");

    snapshot[0] = 0;
    write_u32(snapshot, 8, 5);
    action = invoke_snapshot(FISHING_PROTOCOL, snapshot, sizeof(snapshot));
    invoke_tick();
    check(g_destroy_calls == 1, "closed snapshot destroys the node after UiTick");

    g_grid_scale = 0;
    invoke_tick();
    queued_before = g_queue_calls;
    g_grid_scale = 1;
    invoke_tick();
    check(g_queue_calls == queued_before + 1,
          "leave-world/relog reset queues a fresh capability hello");

    result = descriptor.shutdown(&api, TEST_OWNER);
    check(result == OSMW_OK && resource_count() == 0 &&
              g_unsubscribe_count == 3,
          "shutdown returns all three managed tokens");
    check(g_unsubscribe_order[0] == TICK_TOKEN &&
              g_unsubscribe_order[1] == MAP_TOKEN &&
              g_unsubscribe_order[2] == SNAPSHOT_TOKEN,
          "shutdown revokes tick, observer, then exclusive handler");
    queued_before = g_queue_calls;
    invoke_tick();
    check(g_queue_calls == queued_before,
          "revoked callback is inert before DLL unmap");

    FreeLibrary(module);
    check(resource_count() == 0, "FreeLibrary follows clean managed shutdown");
    if (g_failures != 0) {
        printf("FAILED: %d fishing lifecycle check(s)\n", g_failures);
        return 1;
    }
    printf("PASSED: managed fishing packet/tick lifecycle\n");
    return 0;
}
