#include "core_internal.h"
#include "sigs.h"
#include "test_pe_fixture.h"

#include <stdio.h>
#include <string.h>

typedef int (__cdecl *SYNTHETIC_FN)(void);
typedef void (__cdecl *SYNTHETIC_UI_FN)(LONG list_selector);

static SYNTHETIC_FN g_synthetic_original;
static SYNTHETIC_UI_FN g_dispatch_target;
static LONG g_detour_calls;
static int g_events[64];
static DWORD g_event_count;
static LONG g_original_calls;
static LONG g_blocking_calls;
static HANDLE g_callback_entered;
static HANDLE g_callback_release;
static BOOL g_mutate_once;
static OSMW_OWNER g_remove_owner;
static OSMW_TOKEN g_remove_token;
static OSMW_OWNER g_add_owner;
static OSMW_TOKEN g_added_token;
static OSMW_TOKEN g_self_token;
static OSMW_RESULT g_self_unsubscribe_result;
static volatile BYTE g_spin_entered;
static volatile BYTE g_spin_stop;
static int g_failures;

typedef struct CALLBACK_CONTEXT {
    int marker;
} CALLBACK_CONTEXT;

static int __cdecl synthetic_detour(void)
{
    InterlockedIncrement(&g_detour_calls);
    return g_synthetic_original() + 1;
}

static DWORD WINAPI busy_detour(LPVOID unused)
{
    (void)unused;
    return 0;
}

static void record_event(int marker)
{
    if (g_event_count < sizeof(g_events) / sizeof(g_events[0])) {
        g_events[g_event_count++] = marker;
    }
}

static void __cdecl fixture_original(LONG list_selector)
{
    InterlockedIncrement(&g_original_calls);
    record_event(1000 + (int)list_selector);
}

static void __cdecl simple_callback(
    OSMW_OWNER owner, void *context, LONG list_selector)
{
    CALLBACK_CONTEXT *callback_context = (CALLBACK_CONTEXT *)context;
    (void)owner;
    (void)list_selector;
    record_event(callback_context->marker);
}

static void __cdecl self_unsubscribing_callback(
    OSMW_OWNER owner, void *context, LONG list_selector)
{
    const OSMW_CORE_API_V1 *api = osmw_loader_api();
    CALLBACK_CONTEXT *callback_context = (CALLBACK_CONTEXT *)context;
    (void)list_selector;
    record_event(callback_context->marker);
    g_self_unsubscribe_result = api != NULL
        ? api->unsubscribe(owner, g_self_token) : OSMW_E_INTERNAL;
}

static void __cdecl mutating_callback(
    OSMW_OWNER owner, void *context, LONG list_selector)
{
    CALLBACK_CONTEXT *callback_context = (CALLBACK_CONTEXT *)context;
    static CALLBACK_CONTEXT added_context = {12};
    (void)owner;
    (void)list_selector;
    record_event(callback_context->marker);
    if (g_mutate_once) {
        g_mutate_once = FALSE;
        if (osmw_dispatch_unsubscribe(g_remove_owner, g_remove_token) !=
                OSMW_OK) {
            record_event(-1);
        }
        if (osmw_dispatch_subscribe_ui(
                g_add_owner, 5, simple_callback, &added_context,
                &g_added_token) != OSMW_OK) {
            record_event(-2);
        }
    }
}

static void __cdecl blocking_callback(
    OSMW_OWNER owner, void *context, LONG list_selector)
{
    (void)owner;
    (void)context;
    (void)list_selector;
    InterlockedIncrement(&g_blocking_calls);
    SetEvent(g_callback_entered);
    /* The hook writer intentionally performs many suspend/context retries.
     * Keep this fixture blocked well beyond slow CI scheduler granularity. */
    WaitForSingleObject(g_callback_release, 30000u);
}

static DWORD WINAPI dispatch_thread(LPVOID unused)
{
    (void)unused;
    if (g_dispatch_target != NULL) {
        g_dispatch_target(77);
    } else {
        osmw_dispatch_ui_invoke(fixture_original, 77);
    }
    return 0;
}

static void check(BOOL condition, const char *name)
{
    if (condition) {
        printf("ok:   %s\n", name);
    } else {
        printf("FAIL: %s\n", name);
        ++g_failures;
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
    InterlockedExchange(&g_original_calls, 0);
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

static BOOL make_broker_directory(
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
        directory, directory_size, "%sosmw-broker-%lu-%lu",
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

static void place_core_signature(
    BYTE *text, DWORD offset, const OSMW_CORE_SIGNATURE_DEF *definition,
    BYTE wildcard_seed)
{
    DWORD index;
    memcpy(text + offset, definition->pattern, definition->length);
    for (index = 0; index < definition->length; ++index) {
        if (definition->mask[index] == '?') {
            text[offset + index] = (BYTE)(wildcard_seed + index);
        }
    }
}

int main(void)
{
    static const BYTE synthetic_code[] = {
        0x55, 0x8B, 0xEC, 0xB8, 0x07, 0x00, 0x00, 0x00, 0x5D, 0xC3
    };
    static const DWORD signature_offsets[OSMW_SHARED_ADDRESS_COUNT] = {
        0x20u, 0x80u, 0x120u
    };
    static const int ordered_expected[] = {1042, 2, 1, 3};
    static const int self_removed_expected[] = {1043, 1, 3};
    static const int restored_expected[] = {1044};
    static const int mutation_first[] = {1001, 10, 11};
    static const int mutation_second[] = {1002, 10, 12};
    CALLBACK_CONTEXT first_context = {1};
    CALLBACK_CONTEXT second_context = {2};
    CALLBACK_CONTEXT third_context = {3};
    CALLBACK_CONTEXT mutator_context = {10};
    CALLBACK_CONTEXT removed_context = {11};
    BYTE file[TEST_PE_FILE_SIZE];
    BYTE text[0x300];
    BYTE *code_page = NULL;
    BYTE expected[OSMW_HOOK_MAX_PROLOGUE];
    BYTE saved_patch[OSMW_HOOK_MAX_PROLOGUE];
    char pe_path[MAX_PATH] = "";
    char broker_dir[MAX_PATH] = "";
    char broker_config[MAX_PATH] = "";
    OSMW_HOOK hook;
    const OSMW_CORE_API_V1 *api = NULL;
    OSMW_OWNER core_owner = OSMW_INVALID_OWNER;
    OSMW_OWNER alpha_owner = OSMW_INVALID_OWNER;
    OSMW_OWNER beta_owner = OSMW_INVALID_OWNER;
    OSMW_OWNER gamma_owner = OSMW_INVALID_OWNER;
    OSMW_TOKEN first_token = OSMW_INVALID_TOKEN;
    OSMW_TOKEN second_token = OSMW_INVALID_TOKEN;
    OSMW_TOKEN third_token = OSMW_INVALID_TOKEN;
    OSMW_TOKEN mutator_token = OSMW_INVALID_TOKEN;
    OSMW_TOKEN blocking_token = OSMW_INVALID_TOKEN;
    HANDLE worker = NULL;
    SYNTHETIC_UI_FN ui_target = NULL;
    BOOL loader_started = FALSE;
    DWORD index;
    DWORD wait_count;
    OSMW_RESULT result;

    check(osmw_core_owners_init(), "owner registry starts");
    core_owner = osmw_core_owner_register("core");
    alpha_owner = osmw_core_owner_register("alpha");
    beta_owner = osmw_core_owner_register("beta");
    gamma_owner = osmw_core_owner_register("gamma");
    check(core_owner != OSMW_INVALID_OWNER &&
              alpha_owner != OSMW_INVALID_OWNER &&
              beta_owner != OSMW_INVALID_OWNER &&
              gamma_owner != OSMW_INVALID_OWNER,
          "dispatch fixture owners issued");
    check(osmw_claims_init(), "claim registry starts");
    check(osmw_hook_init(), "hook substrate starts");
    check(osmw_dispatch_init(), "dispatch substrate starts");

    memset(text, 0xCC, sizeof(text));
    for (index = 0; index < OSMW_CORE_SIGNATURE_COUNT; ++index) {
        place_core_signature(text, signature_offsets[index],
                             &OSMW_CORE_SIGNATURES[index],
                             (BYTE)(0x20 + index * 0x20));
    }
    check(test_pe_temp_path(pe_path, sizeof(pe_path)),
          "core signature PE path created");
    check(test_pe_build(file, text, sizeof(text)) == TEST_PE_FILE_SIZE &&
              test_pe_write(pe_path, file, sizeof(file)),
          "core signature PE fixture written");
    check(osmw_image_load_file(pe_path, TEST_PE_IMAGE_BASE) &&
              osmw_scan_start(),
          "pristine resolver starts for core signatures");
    check(osmw_hook_resolve_shared(core_owner),
          "UiTick, HandlePacket and SendPacket resolve together");
    check(osmw_hook_shared_length(OSMW_SHARED_UI_TICK) == 7u &&
              osmw_hook_shared_length(OSMW_SHARED_HANDLE_PACKET) == 12u &&
              osmw_hook_shared_length(OSMW_SHARED_SEND_PACKET) == 5u,
          "shared hook lengths retain proven instruction boundaries");
    /* SendPacket was 0 while core only ever called it.  Attestation counts outbound
     * packets, so it is now declared hookable: the prologue is a single mov al,[imm32],
     * which is exactly five bytes and lands a jmp on an instruction boundary. */
    for (index = 0; index < OSMW_SHARED_ADDRESS_COUNT; ++index) {
        check((DWORD)(ULONG_PTR)osmw_hook_shared_address(index) ==
                  TEST_PE_IMAGE_BASE + TEST_PE_TEXT_RVA +
                      signature_offsets[index],
              OSMW_CORE_SIGNATURES[index].name);
    }
    osmw_scan_reset();
    osmw_image_reset();

    code_page = (BYTE *)VirtualAlloc(NULL, 4096u,
                                     MEM_COMMIT | MEM_RESERVE,
                                     PAGE_EXECUTE_READWRITE);
    check(code_page != NULL, "synthetic executable page allocated");
    if (code_page != NULL) {
        SYNTHETIC_FN target;
        memcpy(code_page, synthetic_code, sizeof(synthetic_code));
        memcpy(expected, synthetic_code, 8);
        ZeroMemory(&hook, sizeof(hook));
        result = osmw_hook_install(core_owner, &hook, code_page,
                                   (void *)synthetic_detour,
                                   expected, 8, "synthetic-target");
        check(result == OSMW_OK && osmw_hook_is_active(&hook) &&
                  osmw_claims_owner_count(core_owner) == 1,
              "synthetic hook installed through a core claim");
        if (result == OSMW_OK) {
            target = (SYNTHETIC_FN)code_page;
            g_synthetic_original = (SYNTHETIC_FN)hook.trampoline;
            InterlockedExchange(&g_detour_calls, 0);
            check(target() == 8 && g_detour_calls == 1,
                  "detour calls its trampoline exactly once");

            memcpy(saved_patch, hook.patch, hook.length);
            code_page[0] = 0xCC;
            FlushInstructionCache(GetCurrentProcess(), code_page, hook.length);
            check(osmw_hook_remove(&hook) == OSMW_E_PATCH_STILL_ACTIVE &&
                      osmw_hook_is_active(&hook) &&
                      osmw_claims_owner_count(core_owner) == 1,
                  "externally changed live hook is not overwritten");
            memcpy(code_page, saved_patch, 8);
            FlushInstructionCache(GetCurrentProcess(), code_page, 8);
            check(osmw_hook_remove(&hook) == OSMW_OK &&
                      !osmw_hook_is_active(&hook) &&
                      osmw_claims_owner_count(core_owner) == 0,
                  "hook removal restores bytes and releases its claim");
            g_synthetic_original = NULL;
            check(target() == 7,
                  "restored synthetic target executes stock code");
        }

        memcpy(code_page + 64, synthetic_code, sizeof(synthetic_code));
        code_page[64] = 0x90;
        ZeroMemory(&hook, sizeof(hook));
        result = osmw_hook_install(core_owner, &hook, code_page + 64,
                                   (void *)synthetic_detour,
                                   synthetic_code, 8,
                                   "prepatched-target");
        check(result == OSMW_E_ALREADY_CLAIMED &&
                  !osmw_hook_is_active(&hook) &&
                  osmw_claims_owner_count(core_owner) == 0,
              "already-modified target is refused without chaining");

        /* Thread proc: mark entered; spin inside the nine-byte target until
         * g_spin_stop changes; return DWORD and pop the LPVOID argument. */
        code_page[128] = 0xC6;
        code_page[129] = 0x05;
        *(DWORD *)(code_page + 130) = (DWORD)(ULONG_PTR)&g_spin_entered;
        code_page[134] = 0x01;
        code_page[135] = 0x80;
        code_page[136] = 0x3D;
        *(DWORD *)(code_page + 137) = (DWORD)(ULONG_PTR)&g_spin_stop;
        code_page[141] = 0x00;
        code_page[142] = 0x74;
        code_page[143] = 0xF7;
        code_page[144] = 0x33;
        code_page[145] = 0xC0;
        code_page[146] = 0xC2;
        code_page[147] = 0x04;
        code_page[148] = 0x00;
        memcpy(expected, code_page + 135, 9);
        g_spin_entered = 0;
        g_spin_stop = 0;
        worker = CreateThread(NULL, 0,
                              (LPTHREAD_START_ROUTINE)(code_page + 128),
                              NULL, 0, NULL);
        for (wait_count = 0; wait_count < 100 && !g_spin_entered;
                ++wait_count) {
            Sleep(1);
        }
        check(worker != NULL && g_spin_entered,
              "synthetic thread enters the patch range");
        ZeroMemory(&hook, sizeof(hook));
        result = osmw_hook_install(core_owner, &hook, code_page + 135,
                                   (void *)busy_detour, expected, 9,
                                   "busy-target");
        g_spin_stop = 1;
        if (worker != NULL) {
            (void)WaitForSingleObject(worker, 5000u);
            CloseHandle(worker);
            worker = NULL;
        }
        check(result == OSMW_E_BUSY &&
                  memcmp(code_page + 135, expected, 9) == 0 &&
                  !osmw_hook_is_active(&hook) &&
                  osmw_claims_owner_count(core_owner) == 0,
              "EIP inside target causes bounded refusal without a write");
        if (osmw_hook_is_active(&hook)) {
            (void)osmw_hook_remove(&hook);
        }
    }

    if (code_page != NULL) {
        BYTE *ui_code = code_page + 256;

        /* UiTick-shaped cdecl function: retain the proven seven-byte prologue,
         * then call fixture_original(list_selector) and return. */
        ui_code[0] = 0x8A;
        ui_code[1] = 0x44;
        ui_code[2] = 0x24;
        ui_code[3] = 0x04;
        ui_code[4] = 0x83;
        ui_code[5] = 0xEC;
        ui_code[6] = 0x08;
        ui_code[7] = 0xFF;
        ui_code[8] = 0x74;
        ui_code[9] = 0x24;
        ui_code[10] = 0x0C;
        ui_code[11] = 0xE8;
        *(LONG *)(ui_code + 12) = (LONG)(
            (ULONG_PTR)fixture_original - ((ULONG_PTR)ui_code + 16u));
        ui_code[16] = 0x83;
        ui_code[17] = 0xC4;
        ui_code[18] = 0x04;
        ui_code[19] = 0x83;
        ui_code[20] = 0xC4;
        ui_code[21] = 0x08;
        ui_code[22] = 0xC3;
        memcpy(expected, ui_code, 7u);
        ui_target = (SYNTHETIC_UI_FN)ui_code;
        result = osmw_hook_install_ui_target(
            core_owner, ui_code, expected, 7u);
        check(result == OSMW_OK && osmw_hook_ui_active() &&
                  osmw_claims_owner_count(core_owner) == 1,
              "one core-owned seven-byte UiTick hook installed");
    }

    check(make_broker_directory(
              broker_dir, sizeof(broker_dir),
              broker_config, sizeof(broker_config)) &&
              osmw_config_init_for_dir(broker_dir),
          "broker API fixture configuration created");
    loader_started = osmw_loader_start(core_owner);
    check(loader_started, "versioned API starts for the broker fixture");
    api = osmw_loader_api();
    check(api != NULL &&
              (api->capabilities & OSMW_CAP_UI_TICK) != 0,
          "UiTick capability advertised only for the active hook");
    if (api != NULL && ui_target != NULL) {
        first_token = 123u;
        check(api->subscribe_post_ui_tick(
                  alpha_owner, -1, simple_callback, &first_context,
                  &first_token) == OSMW_E_INVALID_ARGUMENT &&
                  first_token == OSMW_INVALID_TOKEN,
              "negative public callback priority is reserved");
        check(api->subscribe_post_ui_tick(
                  alpha_owner, 10, simple_callback, &first_context,
                  &first_token) == OSMW_OK &&
                  api->subscribe_post_ui_tick(
                      beta_owner, OSMW_PRIORITY_DEFAULT,
                      self_unsubscribing_callback, &second_context,
                      &g_self_token) == OSMW_OK &&
                  api->subscribe_post_ui_tick(
                      gamma_owner, 10, simple_callback, &third_context,
                      &third_token) == OSMW_OK,
              "three public post-UiTick subscribers registered");
        check(osmw_dispatch_owner_count(alpha_owner) == 1 &&
                  osmw_core_owner_resource_count(alpha_owner) == 1,
              "subscription appears in owner resource counts");

        g_self_unsubscribe_result = OSMW_E_INTERNAL;
        reset_events();
        ui_target(42);
        check(events_equal(
                  ordered_expected,
                  sizeof(ordered_expected) / sizeof(ordered_expected[0])) &&
                  g_original_calls == 1 &&
                  g_self_unsubscribe_result == OSMW_OK,
              "live detour calls stock once, orders callbacks, and self-removes");
        reset_events();
        ui_target(43);
        check(events_equal(
                  self_removed_expected,
                  sizeof(self_removed_expected) /
                      sizeof(self_removed_expected[0])) &&
                  g_original_calls == 1,
              "self-unsubscribe prevents the next UiTick invocation");
        check(api->unsubscribe(alpha_owner, first_token) == OSMW_OK &&
                  api->unsubscribe(gamma_owner, third_token) == OSMW_OK &&
                  api->unsubscribe(beta_owner, g_self_token) ==
                      OSMW_E_NOT_FOUND,
              "owner-scoped public subscription tokens retire once");
    }
    if (loader_started) {
        check(osmw_loader_shutdown(),
              "empty managed fixture shuts down before hook removal");
    }
    result = osmw_hook_stop_ui();
    check(result == OSMW_OK && !osmw_hook_ui_active() &&
              osmw_claims_owner_count(core_owner) == 0,
          "core UiTick hook restores stock bytes and releases its claim");
    if (api != NULL) {
        first_token = 123u;
        check(api->subscribe_post_ui_tick(
                  alpha_owner, 0, simple_callback, &first_context,
                  &first_token) == OSMW_E_UNAVAILABLE &&
                  first_token == OSMW_INVALID_TOKEN,
              "broker API becomes unavailable when the hook is absent");
    }
    if (ui_target != NULL) {
        reset_events();
        ui_target(44);
        check(events_equal(
                  restored_expected,
                  sizeof(restored_expected) / sizeof(restored_expected[0])) &&
                  g_original_calls == 1,
              "restored UiTick target executes stock code directly");
    }
    osmw_loader_reset();
    osmw_config_reset();

    g_mutate_once = TRUE;
    g_remove_owner = beta_owner;
    g_add_owner = gamma_owner;
    g_added_token = OSMW_INVALID_TOKEN;
    check(osmw_dispatch_subscribe_ui(alpha_owner, 0, mutating_callback,
                                     &mutator_context,
                                     &mutator_token) == OSMW_OK &&
              osmw_dispatch_subscribe_ui(beta_owner, 10, simple_callback,
                                         &removed_context,
                                         &g_remove_token) == OSMW_OK,
          "mutation callbacks registered");
    reset_events();
    osmw_dispatch_ui_invoke(fixture_original, 1);
    check(events_equal(mutation_first,
                       sizeof(mutation_first) / sizeof(mutation_first[0])),
          "mid-dispatch remove/add leaves the current snapshot stable");
    reset_events();
    osmw_dispatch_ui_invoke(fixture_original, 2);
    check(events_equal(mutation_second,
                       sizeof(mutation_second) / sizeof(mutation_second[0])),
          "deferred mutations take effect on the next dispatch");
    check(osmw_dispatch_unsubscribe(alpha_owner, mutator_token) == OSMW_OK &&
              osmw_dispatch_unsubscribe(gamma_owner, g_added_token) == OSMW_OK,
          "mutation callbacks removed after dispatch");
    check(osmw_dispatch_unsubscribe(beta_owner, g_remove_token) ==
              OSMW_E_NOT_FOUND,
          "deferred removal invalidates its token exactly once");

    g_callback_entered = CreateEventA(NULL, TRUE, FALSE, NULL);
    g_callback_release = CreateEventA(NULL, TRUE, FALSE, NULL);
    g_blocking_calls = 0;
    reset_events();
    if (ui_target != NULL) {
        memcpy(expected, (const void *)ui_target, 7u);
        result = osmw_hook_install_ui_target(
            core_owner, (void *)ui_target, expected, 7u);
        check(result == OSMW_OK && osmw_hook_ui_active(),
              "UiTick hook reinstalled for the shutdown race");
        g_dispatch_target = ui_target;
    }
    check(g_callback_entered != NULL && g_callback_release != NULL &&
              osmw_dispatch_subscribe_ui(
                  alpha_owner, 0, blocking_callback, NULL,
                  &blocking_token) == OSMW_OK,
          "blocking shutdown-race callback registered");
    worker = CreateThread(NULL, 0, dispatch_thread, NULL, 0, NULL);
    check(worker != NULL &&
              WaitForSingleObject(g_callback_entered, 5000u) == WAIT_OBJECT_0,
          "callback is in flight on a peer thread");
    osmw_dispatch_begin_stop();
    check(!osmw_dispatch_wait_quiescent(10u),
          "bounded quiescence wait observes the in-flight callback");
    check(osmw_hook_stop_ui() == OSMW_E_BUSY &&
              osmw_hook_ui_active(),
          "live callback prevents UiTick trampoline reclamation");
    osmw_dispatch_ui_invoke(fixture_original, 88);
    check(g_blocking_calls == 1 && g_original_calls == 2,
          "stopping gate calls stock original but starts no callback");
    SetEvent(g_callback_release);
    check(osmw_dispatch_wait_quiescent(5000u),
          "dispatcher becomes quiescent after callback return");
    if (worker != NULL) {
        (void)WaitForSingleObject(worker, 5000u);
        CloseHandle(worker);
        worker = NULL;
    }
    check(osmw_dispatch_subscribe_ui(
              beta_owner, 0, simple_callback, &second_context,
              &second_token) == OSMW_E_STOPPING,
          "stopping registry rejects new callbacks");
    check(osmw_dispatch_unsubscribe(alpha_owner, blocking_token) == OSMW_OK,
          "quiescent callback can be removed while stopping");
    check(osmw_hook_stop_ui() == OSMW_OK && !osmw_hook_ui_active(),
          "quiescent UiTick hook is removed safely");
    g_dispatch_target = NULL;
    osmw_dispatch_resume();

    if (g_callback_release != NULL) {
        CloseHandle(g_callback_release);
    }
    if (g_callback_entered != NULL) {
        CloseHandle(g_callback_entered);
    }
    if (code_page != NULL) {
        VirtualFree(code_page, 0, MEM_RELEASE);
    }
    osmw_dispatch_begin_stop();
    check(osmw_dispatch_wait_quiescent(100u),
          "final dispatcher shutdown is quiescent");
    check(osmw_dispatch_total_count() == 0,
          "hook/dispatch fixture leaves no callbacks");
    osmw_dispatch_reset();
    osmw_hook_reset();
    check(osmw_claims_total_count() == 0,
          "hook/dispatch fixture leaves no claims");
    osmw_claims_reset();
    (void)osmw_core_owner_unregister(gamma_owner);
    (void)osmw_core_owner_unregister(beta_owner);
    (void)osmw_core_owner_unregister(alpha_owner);
    (void)osmw_core_owner_unregister(core_owner);
    osmw_core_owners_shutdown();
    if (pe_path[0] != '\0') {
        DeleteFileA(pe_path);
    }
    if (broker_config[0] != '\0') {
        DeleteFileA(broker_config);
    }
    if (broker_dir[0] != '\0') {
        RemoveDirectoryA(broker_dir);
    }

    printf("%s: core post-UiTick broker, %d failure(s)\n",
           g_failures == 0 ? "PASSED" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
