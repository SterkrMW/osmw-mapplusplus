#include "core_internal.h"

#include <stdio.h>
#include <string.h>

#define EVENT_STOCK       20
#define EVENT_UI_STOCK    60
#define EVENT_UI_CALLBACK 70
#define EVENT_SEND        80

typedef struct HANDLER_CONTEXT {
    int marker;
    OSMW_PACKET_ACTION action;
} HANDLER_CONTEXT;

typedef struct OBSERVER_CONTEXT {
    int marker;
} OBSERVER_CONTEXT;

static int g_events[512];
static DWORD g_event_count;
static DWORD g_stock_calls;
static DWORD g_expected_type;
static DWORD g_expected_field12;
static const void *g_expected_extra;
static DWORD g_expected_extra_size;
static BOOL g_packet_view_ok;
static DWORD g_send_count;
static DWORD g_sent_types[OSMW_SEND_QUEUE_CAPACITY + 4u];
static DWORD g_sent_fields[OSMW_SEND_QUEUE_CAPACITY + 4u];
static DWORD g_sent_sizes[OSMW_SEND_QUEUE_CAPACITY + 4u];
static BYTE g_sent_prefix[OSMW_SEND_QUEUE_CAPACITY + 4u][8];
static DWORD g_send_thread;
static const OSMW_CORE_API_V1 *g_api;
static OSMW_RESULT g_queue_callback_result;
static OSMW_RESULT g_receive_queue_result;
static BYTE g_queue_callback_payload[3] = {0x31, 0x32, 0x33};
static int g_failures;

static void check(BOOL condition, const char *name)
{
    if (condition) {
        printf("ok:   %s\n", name);
    } else {
        printf("FAIL: %s\n", name);
        ++g_failures;
    }
}

static void record_event(int marker)
{
    if (g_event_count < sizeof(g_events) / sizeof(g_events[0])) {
        g_events[g_event_count++] = marker;
    }
}

static BOOL events_equal(const int *expected, DWORD count)
{
    return g_event_count == count &&
        memcmp(g_events, expected, count * sizeof(expected[0])) == 0;
}

static void reset_events(void)
{
    ZeroMemory(g_events, sizeof(g_events));
    g_event_count = 0;
}

static void expect_packet(
    DWORD type, DWORD field12, const void *extra, DWORD extra_size)
{
    g_expected_type = type;
    g_expected_field12 = field12;
    g_expected_extra = extra;
    g_expected_extra_size = extra_size;
    g_packet_view_ok = TRUE;
    g_stock_calls = 0;
    reset_events();
}

static BOOL packet_matches(const OSMW_GAME_PACKET_V1 *packet)
{
    return packet != NULL && packet->struct_size == sizeof(*packet) &&
        packet->type == g_expected_type &&
        packet->field12 == g_expected_field12 &&
        packet->extra == g_expected_extra &&
        packet->extra_size == g_expected_extra_size;
}

static OSMW_PACKET_ACTION __cdecl packet_handler(
    OSMW_OWNER owner, void *context, const OSMW_GAME_PACKET_V1 *packet)
{
    HANDLER_CONTEXT *handler = (HANDLER_CONTEXT *)context;
    (void)owner;
    if (!packet_matches(packet)) {
        g_packet_view_ok = FALSE;
    }
    record_event(handler->marker);
    return handler->action;
}

static OSMW_PACKET_ACTION __cdecl queue_receive_handler(
    OSMW_OWNER owner, void *context, const OSMW_GAME_PACKET_V1 *packet)
{
    (void)context;
    if (!packet_matches(packet)) {
        g_packet_view_ok = FALSE;
    }
    g_receive_queue_result = g_api->queue_game_packet(
        owner, 0x00005400u, packet->field12,
        packet->extra, packet->extra_size);
    return OSMW_PACKET_CONSUME;
}

static void __cdecl packet_observer(
    OSMW_OWNER owner, void *context, const OSMW_GAME_PACKET_V1 *packet)
{
    OBSERVER_CONTEXT *observer = (OBSERVER_CONTEXT *)context;
    (void)owner;
    if (!packet_matches(packet)) {
        g_packet_view_ok = FALSE;
    }
    record_event(observer->marker);
}

static void __cdecl fixture_stock_packet(
    DWORD type, DWORD field12, const void *extra, DWORD extra_size)
{
    ++g_stock_calls;
    if (type != g_expected_type || field12 != g_expected_field12 ||
            extra != g_expected_extra || extra_size != g_expected_extra_size) {
        g_packet_view_ok = FALSE;
    }
    record_event(EVENT_STOCK);
}

static void __cdecl fixture_ui_stock(LONG list_selector)
{
    (void)list_selector;
    record_event(EVENT_UI_STOCK);
}

static int __cdecl fixture_send_packet(
    DWORD type, DWORD field12, const void *extra, DWORD extra_size)
{
    DWORD index = g_send_count;
    DWORD copy_size = extra_size < 8u ? extra_size : 8u;

    if (index < OSMW_SEND_QUEUE_CAPACITY + 4u) {
        g_sent_types[index] = type;
        g_sent_fields[index] = field12;
        g_sent_sizes[index] = extra_size;
        if (copy_size != 0 && extra != NULL) {
            memcpy(g_sent_prefix[index], extra, copy_size);
        }
    }
    ++g_send_count;
    g_send_thread = GetCurrentThreadId();
    record_event(EVENT_SEND);
    return 0;
}

static void __cdecl queue_from_ui(
    OSMW_OWNER owner, void *context, LONG list_selector)
{
    (void)context;
    (void)list_selector;
    record_event(EVENT_UI_CALLBACK);
    g_queue_callback_result = g_api->queue_game_packet(
        owner, 0x00000501u, 0x51u,
        g_queue_callback_payload, sizeof(g_queue_callback_payload));
}

static BOOL write_text(const char *path, const char *value)
{
    HANDLE file;
    DWORD length = (DWORD)strlen(value);
    DWORD written = 0;
    BOOL result;

    file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return FALSE;
    }
    result = WriteFile(file, value, length, &written, NULL) &&
        written == length;
    CloseHandle(file);
    return result;
}

static BOOL make_fixture_directory(
    char *directory, DWORD directory_size,
    char *config_path, DWORD config_path_size)
{
    char temporary[MAX_PATH];
    DWORD length = GetTempPathA(sizeof(temporary), temporary);
    int written;

    if (length == 0 || length >= sizeof(temporary)) {
        return FALSE;
    }
    written = _snprintf(
        directory, directory_size, "%sosmw-packets-%lu-%lu",
        temporary, (unsigned long)GetCurrentProcessId(),
        (unsigned long)GetTickCount());
    if (written < 0 || (DWORD)written >= directory_size ||
            !CreateDirectoryA(directory, NULL)) {
        return FALSE;
    }
    written = _snprintf(
        config_path, config_path_size, "%s\\config.ini", directory);
    if (written < 0 || (DWORD)written >= config_path_size) {
        RemoveDirectoryA(directory);
        directory[0] = '\0';
        return FALSE;
    }
    return write_text(config_path, "[Core]\r\nModules=,\r\n");
}

static void build_packet_target(BYTE *code)
{
    /* Verified HandlePacket prologue, then forward unchanged to the C fixture
     * and return with the raw client's ret 0x10 stack cleanup. */
    code[0] = 0x55;
    code[1] = 0x8B;
    code[2] = 0xEC;
    code[3] = 0x83;
    code[4] = 0xE4;
    code[5] = 0xF8;
    code[6] = 0x81;
    code[7] = 0xEC;
    code[8] = 0x54;
    code[9] = 0x05;
    code[10] = 0x00;
    code[11] = 0x00;
    code[12] = 0xFF;
    code[13] = 0x75;
    code[14] = 0x14;
    code[15] = 0xFF;
    code[16] = 0x75;
    code[17] = 0x10;
    code[18] = 0xFF;
    code[19] = 0x75;
    code[20] = 0x0C;
    code[21] = 0xFF;
    code[22] = 0x75;
    code[23] = 0x08;
    code[24] = 0xE8;
    *(LONG *)(code + 25) = (LONG)(
        (ULONG_PTR)fixture_stock_packet - ((ULONG_PTR)code + 29u));
    code[29] = 0x83;
    code[30] = 0xC4;
    code[31] = 0x10;
    code[32] = 0x8B;
    code[33] = 0xE5;
    code[34] = 0x5D;
    code[35] = 0xC2;
    code[36] = 0x10;
    code[37] = 0x00;
}

static void build_ui_target(BYTE *code)
{
    code[0] = 0x8A;
    code[1] = 0x44;
    code[2] = 0x24;
    code[3] = 0x04;
    code[4] = 0x83;
    code[5] = 0xEC;
    code[6] = 0x08;
    code[7] = 0xFF;
    code[8] = 0x74;
    code[9] = 0x24;
    code[10] = 0x0C;
    code[11] = 0xE8;
    *(LONG *)(code + 12) = (LONG)(
        (ULONG_PTR)fixture_ui_stock - ((ULONG_PTR)code + 16u));
    code[16] = 0x83;
    code[17] = 0xC4;
    code[18] = 0x04;
    code[19] = 0x83;
    code[20] = 0xC4;
    code[21] = 0x08;
    code[22] = 0xC3;
}

static BOOL invoke_packet_balanced(
    OSMW_PACKET_ORIGINAL_FN target, DWORD type, DWORD field12,
    const void *extra, DWORD extra_size)
{
    volatile DWORD left_canary = 0x13579BDFu;
    volatile DWORD right_canary = 0x2468ACE0u;
    DWORD before;
    DWORD after;

    __asm mov before, esp
    target(type, field12, extra, extra_size);
    __asm mov after, esp
    return before == after && left_canary == 0x13579BDFu &&
        right_canary == 0x2468ACE0u;
}

static void reset_sends(void)
{
    g_send_count = 0;
    g_send_thread = 0;
    ZeroMemory(g_sent_types, sizeof(g_sent_types));
    ZeroMemory(g_sent_fields, sizeof(g_sent_fields));
    ZeroMemory(g_sent_sizes, sizeof(g_sent_sizes));
    ZeroMemory(g_sent_prefix, sizeof(g_sent_prefix));
}

int main(void)
{
    static const int pass_expected[] = {10, 20, 30, 31, 32};
    static const int stock_only_expected[] = {20};
    static const int consume_expected[] = {40};
    static const int invalid_expected[] = {50, 20, 51};
    static const int copied_send_expected[] = {60, 80};
    static const int callback_send_expected[] = {60, 70, 80};
    HANDLER_CONTEXT pass_context = {10, OSMW_PACKET_PASS};
    HANDLER_CONTEXT consume_context = {40, OSMW_PACKET_CONSUME};
    HANDLER_CONTEXT invalid_context = {50, (OSMW_PACKET_ACTION)99u};
    OBSERVER_CONTEXT low_context = {30};
    OBSERVER_CONTEXT tie_first_context = {31};
    OBSERVER_CONTEXT tie_second_context = {32};
    OBSERVER_CONTEXT consume_observer_context = {41};
    OBSERVER_CONTEXT invalid_observer_context = {51};
    OSMW_TOKEN pass_token = OSMW_INVALID_TOKEN;
    OSMW_TOKEN low_token = OSMW_INVALID_TOKEN;
    OSMW_TOKEN tie_first_token = OSMW_INVALID_TOKEN;
    OSMW_TOKEN tie_second_token = OSMW_INVALID_TOKEN;
    OSMW_TOKEN consume_token = OSMW_INVALID_TOKEN;
    OSMW_TOKEN consume_observer_token = OSMW_INVALID_TOKEN;
    OSMW_TOKEN invalid_token = OSMW_INVALID_TOKEN;
    OSMW_TOKEN invalid_observer_token = OSMW_INVALID_TOKEN;
    OSMW_TOKEN receive_queue_token = OSMW_INVALID_TOKEN;
    OSMW_TOKEN ui_queue_token = OSMW_INVALID_TOKEN;
    OSMW_TOKEN duplicate_token = 123u;
    OSMW_OWNER core_owner = OSMW_INVALID_OWNER;
    OSMW_OWNER alpha_owner = OSMW_INVALID_OWNER;
    OSMW_OWNER beta_owner = OSMW_INVALID_OWNER;
    OSMW_OWNER gamma_owner = OSMW_INVALID_OWNER;
    OSMW_PACKET_ORIGINAL_FN packet_target = NULL;
    OSMW_UI_ORIGINAL_FN ui_target = NULL;
    BYTE *code_page = NULL;
    BYTE expected[OSMW_HOOK_MAX_PROLOGUE];
    BYTE receive_payload[4] = {0xA1, 0xB2, 0xC3, 0xD4};
    BYTE send_payload[4] = {1, 2, 3, 4};
    char fixture_dir[MAX_PATH] = "";
    char config_path[MAX_PATH] = "";
    BOOL loader_started = FALSE;
    DWORD index;

    check(osmw_core_owners_init(), "packet owner registry starts");
    core_owner = osmw_core_owner_register("core");
    alpha_owner = osmw_core_owner_register("alpha");
    beta_owner = osmw_core_owner_register("beta");
    gamma_owner = osmw_core_owner_register("gamma");
    check(core_owner != OSMW_INVALID_OWNER &&
              alpha_owner != OSMW_INVALID_OWNER &&
              beta_owner != OSMW_INVALID_OWNER &&
              gamma_owner != OSMW_INVALID_OWNER,
          "packet fixture owners issued");
    check(osmw_claims_init() && osmw_hook_init() && osmw_dispatch_init(),
          "packet hook and dispatch services start");

    code_page = (BYTE *)VirtualAlloc(
        NULL, 4096u, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    check(code_page != NULL, "packet executable page allocated");
    if (code_page != NULL) {
        build_packet_target(code_page);
        build_ui_target(code_page + 128);
        packet_target = (OSMW_PACKET_ORIGINAL_FN)code_page;
        ui_target = (OSMW_UI_ORIGINAL_FN)(code_page + 128);
        memcpy(expected, code_page, 12u);
        check(osmw_hook_install_packet_target(
                  core_owner, code_page, expected, 12u) == OSMW_OK &&
                  osmw_hook_packet_active(),
              "one core-owned 12-byte HandlePacket hook installed");
        memcpy(expected, code_page + 128, 7u);
        check(osmw_hook_install_ui_target(
                  core_owner, code_page + 128, expected, 7u) == OSMW_OK &&
                  osmw_hook_ui_active(),
              "UiTick hook installed for queued-send flushing");
    }
    osmw_dispatch_set_send_target((void *)fixture_send_packet);

    check(make_fixture_directory(
              fixture_dir, sizeof(fixture_dir),
              config_path, sizeof(config_path)) &&
              osmw_config_init_for_dir(fixture_dir),
          "packet API fixture configuration created");
    loader_started = osmw_loader_start(core_owner);
    check(loader_started, "versioned API starts for packet fixture");
    g_api = osmw_loader_api();
    check(g_api != NULL &&
              (g_api->capabilities &
               (OSMW_CAP_UI_TICK | OSMW_CAP_GAME_PACKETS |
                OSMW_CAP_SEND_QUEUE)) ==
               (OSMW_CAP_UI_TICK | OSMW_CAP_GAME_PACKETS |
                OSMW_CAP_SEND_QUEUE),
          "packet and send capabilities advertised for active services");

    if (g_api != NULL && packet_target != NULL && ui_target != NULL) {
        check(g_api->observe_game_packet_after(
                  alpha_owner, 0x100u, -1, packet_observer,
                  &low_context, &duplicate_token) ==
                  OSMW_E_INVALID_ARGUMENT &&
                  duplicate_token == OSMW_INVALID_TOKEN,
              "negative public packet-observer priority is reserved");
        check(g_api->claim_game_packet(
                  alpha_owner, 0x100u, packet_handler, &pass_context,
                  &pass_token) == OSMW_OK &&
                  g_api->observe_game_packet_after(
                      alpha_owner, 0x100u, 10, packet_observer,
                      &tie_first_context, &tie_first_token) == OSMW_OK &&
                  g_api->observe_game_packet_after(
                      beta_owner, 0x100u, 0, packet_observer,
                      &low_context, &low_token) == OSMW_OK &&
                  g_api->observe_game_packet_after(
                      gamma_owner, 0x100u, 10, packet_observer,
                      &tie_second_context, &tie_second_token) == OSMW_OK,
              "exclusive handler and ordered observers register");
        check(g_api->claim_game_packet(
                  beta_owner, 0x100u, packet_handler, &consume_context,
                  &duplicate_token) == OSMW_E_ALREADY_CLAIMED &&
                  duplicate_token == OSMW_INVALID_TOKEN,
              "second exact-type handler is rejected");
        check(g_api->unsubscribe(beta_owner, pass_token) ==
                  OSMW_E_WRONG_OWNER,
              "packet token enforces owner scope");

        expect_packet(0x100u, 0x12345678u,
                      receive_payload, sizeof(receive_payload));
        check(invoke_packet_balanced(
                  packet_target, g_expected_type, g_expected_field12,
                  receive_payload, sizeof(receive_payload)),
              "raw stdcall detour preserves ESP and stack canaries");
        check(events_equal(
                  pass_expected, sizeof(pass_expected) /
                      sizeof(pass_expected[0])) &&
                  g_stock_calls == 1 && g_packet_view_ok,
              "PASS forwards unchanged stock arguments then ordered observers");

        expect_packet(0x101u, 9u, receive_payload, sizeof(receive_payload));
        check(invoke_packet_balanced(
                  packet_target, g_expected_type, g_expected_field12,
                  receive_payload, sizeof(receive_payload)) &&
                  events_equal(
                      stock_only_expected,
                      sizeof(stock_only_expected) /
                          sizeof(stock_only_expected[0])) &&
                  g_stock_calls == 1 && g_packet_view_ok,
              "packet registrations are isolated by exact type");

        check(g_api->claim_game_packet(
                  beta_owner, 0x200u, packet_handler, &consume_context,
                  &consume_token) == OSMW_OK &&
                  g_api->observe_game_packet_after(
                      gamma_owner, 0x200u, 0, packet_observer,
                      &consume_observer_context,
                      &consume_observer_token) == OSMW_OK,
              "consume fixture registrations created");
        expect_packet(0x200u, 10u, receive_payload, sizeof(receive_payload));
        check(invoke_packet_balanced(
                  packet_target, g_expected_type, g_expected_field12,
                  receive_payload, sizeof(receive_payload)) &&
                  events_equal(
                      consume_expected,
                      sizeof(consume_expected) / sizeof(consume_expected[0])) &&
                  g_stock_calls == 0 && g_packet_view_ok,
              "CONSUME skips both stock and post-stock observers");

        check(g_api->claim_game_packet(
                  gamma_owner, 0x300u, packet_handler, &invalid_context,
                  &invalid_token) == OSMW_OK &&
                  g_api->observe_game_packet_after(
                      beta_owner, 0x300u, 0, packet_observer,
                      &invalid_observer_context,
                      &invalid_observer_token) == OSMW_OK,
              "invalid-action fixture registrations created");
        expect_packet(0x300u, 11u, receive_payload, sizeof(receive_payload));
        check(invoke_packet_balanced(
                  packet_target, g_expected_type, g_expected_field12,
                  receive_payload, sizeof(receive_payload)) &&
                  events_equal(
                      invalid_expected,
                      sizeof(invalid_expected) / sizeof(invalid_expected[0])) &&
                  g_stock_calls == 1 && g_packet_view_ok,
              "invalid handler action is treated as PASS");

        check(g_api->claim_game_packet(
                  alpha_owner, 0x400u, queue_receive_handler, NULL,
                  &receive_queue_token) == OSMW_OK,
              "borrowed-payload queue handler registered");
        reset_sends();
        g_receive_queue_result = OSMW_E_INTERNAL;
        receive_payload[0] = 0xA1;
        receive_payload[1] = 0xB2;
        receive_payload[2] = 0xC3;
        receive_payload[3] = 0xD4;
        expect_packet(0x400u, 0x44u,
                      receive_payload, sizeof(receive_payload));
        check(invoke_packet_balanced(
                  packet_target, g_expected_type, g_expected_field12,
                  receive_payload, sizeof(receive_payload)) &&
                  g_receive_queue_result == OSMW_OK &&
                  g_stock_calls == 0 && g_packet_view_ok,
              "receive callback queues borrowed payload synchronously");
        memset(receive_payload, 0xEE, sizeof(receive_payload));
        reset_events();
        ui_target(0);
        check(g_send_count == 1 && g_sent_types[0] == 0x5400u &&
                  g_sent_fields[0] == 0x44u && g_sent_sizes[0] == 4u &&
                  g_sent_prefix[0][0] == 0xA1 &&
                  g_sent_prefix[0][1] == 0xB2 &&
                  g_sent_prefix[0][2] == 0xC3 &&
                  g_sent_prefix[0][3] == 0xD4,
              "queued receive payload outlives caller buffer by value");

        reset_sends();
        reset_events();
        check(g_api->queue_game_packet(
                  alpha_owner, 0x500u, 0x50u,
                  send_payload, sizeof(send_payload)) == OSMW_OK &&
                  g_send_count == 0,
              "queued send is deferred until post-UiTick");
        memset(send_payload, 0xEE, sizeof(send_payload));
        g_send_thread = 0;
        ui_target(1);
        check(events_equal(
                  copied_send_expected,
                  sizeof(copied_send_expected) /
                      sizeof(copied_send_expected[0])) &&
                  g_send_count == 1 && g_sent_types[0] == 0x500u &&
                  g_sent_fields[0] == 0x50u && g_sent_sizes[0] == 4u &&
                  g_sent_prefix[0][0] == 1 && g_sent_prefix[0][1] == 2 &&
                  g_sent_prefix[0][2] == 3 && g_sent_prefix[0][3] == 4 &&
                  g_send_thread == GetCurrentThreadId(),
              "queue copies payload and flushes on the UiTick thread");

        check(g_api->subscribe_post_ui_tick(
                  alpha_owner, 0, queue_from_ui, NULL,
                  &ui_queue_token) == OSMW_OK,
              "post-UiTick queueing callback registered");
        reset_sends();
        reset_events();
        g_queue_callback_result = OSMW_E_INTERNAL;
        ui_target(2);
        check(events_equal(
                  callback_send_expected,
                  sizeof(callback_send_expected) /
                      sizeof(callback_send_expected[0])) &&
                  g_queue_callback_result == OSMW_OK && g_send_count == 1 &&
                  g_sent_types[0] == 0x501u,
              "sends queued by callbacks flush after all post-UiTick work");
        check(g_api->unsubscribe(alpha_owner, ui_queue_token) == OSMW_OK,
              "queued-send UiTick callback removed");

        check(g_api->queue_game_packet(
                  alpha_owner, 1u, 2u, NULL,
                  OSMW_SEND_PACKET_MAX_EXTRA + 1u) ==
                  OSMW_E_BUFFER_TOO_SMALL &&
                  g_api->queue_game_packet(
                      alpha_owner, 1u, 2u, NULL, 1u) ==
                      OSMW_E_INVALID_ARGUMENT &&
                  g_api->queue_game_packet(
                      OSMW_INVALID_OWNER, 1u, 2u, NULL, 0u) ==
                      OSMW_E_WRONG_OWNER,
              "send queue rejects oversized, missing, and wrong-owner input");

        reset_sends();
        for (index = 0; index < OSMW_SEND_QUEUE_CAPACITY; ++index) {
            if (g_api->queue_game_packet(
                    alpha_owner, 0x8000u + index, index,
                    NULL, 0u) != OSMW_OK) {
                break;
            }
        }
        check(index == OSMW_SEND_QUEUE_CAPACITY &&
                  g_api->queue_game_packet(
                      alpha_owner, 0x9000u, 0u, NULL, 0u) ==
                      OSMW_E_QUEUE_FULL,
              "fixed send queue reports full synchronously");
        reset_events();
        ui_target(3);
        for (index = 0; index < OSMW_SEND_QUEUE_CAPACITY; ++index) {
            if (g_sent_types[index] != 0x8000u + index ||
                    g_sent_fields[index] != index) {
                break;
            }
        }
        check(g_send_count == OSMW_SEND_QUEUE_CAPACITY &&
                  index == OSMW_SEND_QUEUE_CAPACITY,
              "full queue flushes once in FIFO order");

        check(g_api->unsubscribe(alpha_owner, pass_token) == OSMW_OK &&
                  g_api->unsubscribe(alpha_owner, tie_first_token) == OSMW_OK &&
                  g_api->unsubscribe(beta_owner, low_token) == OSMW_OK &&
                  g_api->unsubscribe(gamma_owner, tie_second_token) == OSMW_OK &&
                  g_api->unsubscribe(beta_owner, consume_token) == OSMW_OK &&
                  g_api->unsubscribe(
                      gamma_owner, consume_observer_token) == OSMW_OK &&
                  g_api->unsubscribe(gamma_owner, invalid_token) == OSMW_OK &&
                  g_api->unsubscribe(
                      beta_owner, invalid_observer_token) == OSMW_OK &&
                  g_api->unsubscribe(
                      alpha_owner, receive_queue_token) == OSMW_OK,
              "all packet subscriptions release through the shared API");
        check(osmw_dispatch_owner_count(alpha_owner) == 0 &&
                  osmw_dispatch_owner_count(beta_owner) == 0 &&
                  osmw_dispatch_owner_count(gamma_owner) == 0,
              "packet resources leave owner counts at zero");
    }

    osmw_dispatch_begin_stop();
    if (g_api != NULL) {
        check(g_api->queue_game_packet(
                  alpha_owner, 1u, 2u, NULL, 0u) == OSMW_E_STOPPING,
              "stopping dispatcher rejects new queued sends");
    }
    check(osmw_dispatch_wait_quiescent(5000u),
          "packet dispatcher is quiescent for hook removal");
    if (loader_started) {
        check(osmw_loader_shutdown(), "empty packet fixture loader shuts down");
    }
    check(osmw_hook_stop_packet() == OSMW_OK &&
              !osmw_hook_packet_active(),
          "HandlePacket hook restores stock bytes safely");
    check(osmw_hook_stop_ui() == OSMW_OK && !osmw_hook_ui_active(),
          "queue-flush UiTick hook restores stock bytes safely");
    osmw_dispatch_set_send_target(NULL);

    if (packet_target != NULL) {
        expect_packet(0x999u, 0x77u,
                      receive_payload, sizeof(receive_payload));
        check(invoke_packet_balanced(
                  packet_target, g_expected_type, g_expected_field12,
                  receive_payload, sizeof(receive_payload)) &&
                  g_stock_calls == 1 && g_packet_view_ok,
              "restored stdcall target forwards unchanged without core");
    }

    osmw_loader_reset();
    osmw_config_reset();
    osmw_dispatch_reset();
    osmw_hook_reset();
    check(osmw_claims_total_count() == 0,
          "packet fixture leaves no hook claims");
    osmw_claims_reset();
    (void)osmw_core_owner_unregister(gamma_owner);
    (void)osmw_core_owner_unregister(beta_owner);
    (void)osmw_core_owner_unregister(alpha_owner);
    (void)osmw_core_owner_unregister(core_owner);
    osmw_core_owners_shutdown();
    if (code_page != NULL) {
        VirtualFree(code_page, 0, MEM_RELEASE);
    }
    if (config_path[0] != '\0') {
        DeleteFileA(config_path);
    }
    if (fixture_dir[0] != '\0') {
        RemoveDirectoryA(fixture_dir);
    }

    printf("%s: core packet broker and send queue, %d failure(s)\n",
           g_failures == 0 ? "PASSED" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
