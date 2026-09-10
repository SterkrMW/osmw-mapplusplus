/* Real-DLL host fixture for managed timer lifecycle and protocol routing. */
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "osmw_core_api.h"

#define TEST_OWNER ((OSMW_OWNER)0x6100u)
#define SNAPSHOT_TOKEN ((OSMW_TOKEN)0x6101u)
#define MAP_TOKEN ((OSMW_TOKEN)0x6102u)
#define TICK_TOKEN ((OSMW_TOKEN)0x6103u)
#define TIMER_SIGNATURE_COUNT 13
#define PACKET_MAP_DATA 0x00030100u
#define PACKET_HELLO 0x7F000001u
#define PACKET_SNAPSHOT 0x7F000002u
#define TIMER_PROTOCOL 2u

typedef int (__cdecl *test_create_fn)(
    int, int, int, int, void *, void *, int, int);
typedef void (__cdecl *test_destroy_fn)(int, int);
typedef int (__cdecl *test_geometry_fn)(int, void *, int *, int);
typedef void (__cdecl *test_set_geometry_fn)(int, const void *, int, int);

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
static int g_bad_queue;
static int g_grid_scale = 1;
static BYTE g_keys[0x59];
static DWORD g_dummy_data;
static int g_create_calls;
static int g_destroy_calls;
static int g_node_handle = 77;

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
    (void)left;
    (void)top;
    (void)right;
    (void)bottom;
    (void)draw_proc;
    (void)input_proc;
    (void)arg4;
    (void)list_selector;
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
    values[0] = 332;
    values[1] = 8;
    values[2] = 468;
    values[3] = 64;
    if (flag != NULL)
        *flag = 0;
    return 0;
}

static void __cdecl fake_set_geometry(
    int handle, const void *rect, int flag, int list_selector)
{
    (void)rect;
    (void)flag;
    if (handle != g_node_handle || list_selector != 0)
        ++g_failures;
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
    if (strcmp(section, "Addons") == 0 && strcmp(key, "Timers") == 0)
        return g_gate_enabled;
    return fallback;
}

static OSMW_RESULT __cdecl fake_config_read_string(
    OSMW_OWNER owner, const char *section, const char *key,
    const char *fallback, char *output, DWORD output_size)
{
    const char *value = fallback != NULL ? fallback : "";
    size_t length = strlen(value);
    (void)section;
    (void)key;
    ++g_service_calls;
    if (owner != TEST_OWNER || output == NULL || output_size == 0)
        return OSMW_E_INVALID_ARGUMENT;
    if (length >= output_size) {
        memcpy(output, value, output_size - 1);
        output[output_size - 1] = '\0';
        return OSMW_E_BUFFER_TOO_SMALL;
    }
    memcpy(output, value, length + 1);
    return OSMW_OK;
}

static OSMW_RESULT __cdecl fake_config_write_string(
    OSMW_OWNER owner, const char *section, const char *key, const char *value)
{
    ++g_service_calls;
    return owner == TEST_OWNER && section != NULL && key != NULL && value != NULL
        ? OSMW_OK : OSMW_E_INVALID_ARGUMENT;
}

static OSMW_RESULT __cdecl fake_resolve_signature(
    OSMW_OWNER owner, const OSMW_SIGNATURE_V1 *signature,
    OSMW_RESOLVED_ADDRESS_V1 *output)
{
    union { test_create_fn function; void *address; } create_address;
    union { test_destroy_fn function; void *address; } destroy_address;
    union { test_geometry_fn function; void *address; } geometry_address;
    union { test_set_geometry_fn function; void *address; } set_address;

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
    set_address.function = fake_set_geometry;
    if (strcmp(signature->name, "UiCreateNode") == 0)
        output->address = create_address.address;
    else if (strcmp(signature->name, "UiDestroyNode") == 0)
        output->address = destroy_address.address;
    else if (strcmp(signature->name, "NodeGeometry") == 0)
        output->address = geometry_address.address;
    else if (strcmp(signature->name, "NodeSetGeometry") == 0)
        output->address = set_address.address;
    else if (strcmp(signature->name, "GridScaleX") == 0)
        output->address = &g_grid_scale;
    else if (strcmp(signature->name, "KeyArray") == 0)
        output->address = g_keys;
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
    if (owner != TEST_OWNER || packet_type != PACKET_HELLO ||
            field12 != TIMER_PROTOCOL || extra != NULL || extra_size != 0)
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
    g_bad_queue = 0;
    g_grid_scale = 1;
    ZeroMemory(g_keys, sizeof(g_keys));
    g_dummy_data = 0;
    g_create_calls = 0;
    g_destroy_calls = 0;
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

int main(int argc, char **argv)
{
    HMODULE module;
    union { FARPROC raw; OSMW_MOD_QUERY_V1 query; } query_address;
    OSMW_MOD_DESCRIPTOR_V1 descriptor;
    OSMW_CORE_API_V1 api;
    OSMW_RESULT result;
    OSMW_PACKET_ACTION action;
    DWORD snapshot[4];
    int queued_before;

    if (argc != 2) {
        fprintf(stderr, "usage: test_lifecycle.exe osmw_timers.dll\n");
        return 2;
    }

    reset_fixture();
    module = LoadLibraryA(argv[1]);
    check(module != NULL, "real timers DLL loads");
    check(g_service_calls == 0, "DllMain is passive");
    if (module == NULL)
        return 1;
    query_address.raw = GetProcAddress(module, OSMW_MOD_QUERY_EXPORT_NAME);
    check(query_address.raw != NULL, "undecorated osmw_mod_query resolves");
    if (query_address.raw == NULL) {
        FreeLibrary(module);
        return 1;
    }

    ZeroMemory(&descriptor, sizeof(descriptor));
    check(query_address.query(OSMW_ABI_MAKE(2, 0), sizeof(descriptor),
                              &descriptor) == OSMW_E_INCOMPATIBLE_ABI,
          "query rejects another ABI major");
    check(query_address.query(OSMW_CORE_ABI_CURRENT,
                              OSMW_MOD_DESCRIPTOR_V1_MIN_SIZE - 1,
                              &descriptor) == OSMW_E_BUFFER_TOO_SMALL,
          "query rejects a short descriptor");
    result = query_address.query(
        OSMW_CORE_ABI_CURRENT, sizeof(descriptor), &descriptor);
    check(result == OSMW_OK && descriptor.module_id != NULL &&
              strcmp(descriptor.module_id, "timers") == 0 &&
              descriptor.init != NULL && descriptor.shutdown != NULL,
          "query publishes the timers ABI 1 descriptor");
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
    g_missing_signature = "DrawItemIcon";
    result = descriptor.init(&api, TEST_OWNER);
    check(result == OSMW_OK && g_resolve_calls == TIMER_SIGNATURE_COUNT &&
              !g_saw_shared_signature && resource_count() == 3,
          "active init resolves 13 private signatures and owns three tokens");
    check(g_snapshot_handler != NULL && g_map_observer != NULL &&
              g_tick_callback != NULL,
          "typed snapshot, MapData, and tick callbacks are registered");

    g_queue_result = OSMW_E_QUEUE_FULL;
    invoke_tick();
    check(g_queue_calls == 1 && !g_bad_queue &&
              g_last_queue_type == PACKET_HELLO &&
              g_last_queue_field12 == TIMER_PROTOCOL,
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

    snapshot[0] = 1;
    snapshot[1] = 0;
    snapshot[2] = 0;
    snapshot[3] = 0;
    action = invoke_snapshot(TIMER_PROTOCOL, snapshot, sizeof(snapshot));
    check(action == OSMW_PACKET_CONSUME,
          "valid private snapshot is exclusively consumed");
    action = invoke_snapshot(1, snapshot, sizeof(snapshot));
    check(action == OSMW_PACKET_CONSUME, "unknown snapshot version is consumed");
    action = invoke_snapshot(TIMER_PROTOCOL, snapshot, 12);
    check(action == OSMW_PACKET_CONSUME, "short snapshot is consumed");
    action = invoke_snapshot(TIMER_PROTOCOL, NULL, sizeof(snapshot));
    check(action == OSMW_PACKET_CONSUME, "null snapshot payload is consumed");
    snapshot[0] = 1801;
    action = invoke_snapshot(TIMER_PROTOCOL, snapshot, sizeof(snapshot));
    check(action == OSMW_PACKET_CONSUME, "out-of-range snapshot is consumed");
    snapshot[0] = 0;
    snapshot[2] = 1;
    snapshot[3] = 1;
    action = invoke_snapshot(TIMER_PROTOCOL, snapshot, sizeof(snapshot));
    check(action == OSMW_PACKET_CONSUME && g_ignored_logs >= 5,
          "inconsistent snapshots are consumed and ignored without state use");

    queued_before = g_queue_calls;
    Sleep(1100);
    invoke_tick();
    check(g_queue_calls == queued_before + 1,
          "expired authoritative duration queues one bounded refresh");

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
        printf("FAILED: %d timer lifecycle check(s)\n", g_failures);
        return 1;
    }
    printf("PASSED: managed timer packet/tick lifecycle\n");
    return 0;
}
