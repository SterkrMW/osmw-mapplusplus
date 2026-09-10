/* Host-side lifecycle fixture for the real osmw_settings.dll.
 *
 * This deliberately loads the DLL before constructing an API table: any work
 * from DllMain would therefore be observable as a crash or an unexpected fake
 * service call.  The remaining cases drive the exported descriptor exactly as
 * core does.
 */
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "osmw_core_api.h"

#define TEST_OWNER ((OSMW_OWNER)0x5100u)
#define TEST_TOKEN ((OSMW_TOKEN)0x5101u)
#define TEST_SIGNATURE_COUNT 14

typedef int (__cdecl *test_create_fn)(
    int, int, int, int, void *, void *, int, int);
typedef void (__cdecl *test_destroy_fn)(int, int);

static int g_failures;
static int g_service_calls;
static int g_gate_enabled = 1;
static const char *g_missing_signature;
static int g_resolve_calls;
static int g_saw_private_uitick;
static int g_subscription_count;
static int g_unsubscribe_calls;
static LONG g_subscribe_priority;
static OSMW_UI_TICK_CALLBACK_V1 g_tick_callback;
static void *g_tick_context;
static int g_original_complete;
static int g_in_post_tick;
static int g_create_calls;
static int g_destroy_calls;
static int g_destroy_in_post_tick;
static int g_destroy_after_unsubscribe;
static int g_destroy_handle;
static int g_destroy_list;
static int g_grid_scale = 1;
static BYTE g_keys[0x59];
static DWORD g_dummy_data;

static void check(int condition, const char *message)
{
    if (condition) {
        printf("PASS: %s\n", message);
    } else {
        printf("FAIL: %s\n", message);
        ++g_failures;
    }
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
    if (!g_original_complete || !g_in_post_tick)
        ++g_failures;
    return 77;
}

static void __cdecl fake_destroy(int handle, int list_selector)
{
    ++g_destroy_calls;
    g_destroy_handle = handle;
    g_destroy_list = list_selector;
    if (g_in_post_tick) {
        ++g_destroy_in_post_tick;
        if (!g_original_complete)
            ++g_failures;
    }
    if (g_subscription_count == 0)
        g_destroy_after_unsubscribe = 1;
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
    if (owner != TEST_OWNER || level > OSMW_LOG_ERROR || message == NULL)
        ++g_failures;
}

static int __cdecl fake_config_read_int(
    OSMW_OWNER owner, const char *section, const char *key, int fallback)
{
    ++g_service_calls;
    if (owner != TEST_OWNER)
        ++g_failures;
    if (strcmp(section, "Addons") == 0 && strcmp(key, "Settings") == 0)
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
    union {
        test_create_fn function;
        void *address;
    } create_address;
    union {
        test_destroy_fn function;
        void *address;
    } destroy_address;

    ++g_service_calls;
    ++g_resolve_calls;
    if (owner != TEST_OWNER || signature == NULL || output == NULL ||
            signature->struct_size < sizeof(*signature) ||
            output->struct_size < sizeof(*output) ||
            signature->flags != 0 || signature->name == NULL)
        return OSMW_E_INVALID_ARGUMENT;
    if (strcmp(signature->name, "UiTick") == 0) {
        g_saw_private_uitick = 1;
        return OSMW_E_INTERNAL;
    }

    ZeroMemory(output, sizeof(*output));
    output->struct_size = sizeof(*output);
    if (g_missing_signature != NULL &&
            strcmp(signature->name, g_missing_signature) == 0)
        return OSMW_E_NOT_FOUND;

    create_address.function = fake_create;
    destroy_address.function = fake_destroy;
    if (strcmp(signature->name, "UiCreateNode") == 0)
        output->address = create_address.address;
    else if (strcmp(signature->name, "UiDestroyNode") == 0)
        output->address = destroy_address.address;
    else if (strcmp(signature->name, "GridScaleX") == 0)
        output->address = &g_grid_scale;
    else if (strcmp(signature->name, "KeyArray") == 0)
        output->address = g_keys;
    else
        output->address = &g_dummy_data;
    output->match_count = 1;
    output->reference_rva = signature->reference_rva;
    output->match_rva = signature->reference_rva;
    output->rva_delta = 0;
    return OSMW_OK;
}

static OSMW_RESULT __cdecl fake_subscribe(
    OSMW_OWNER owner, LONG priority, OSMW_UI_TICK_CALLBACK_V1 callback,
    void *context, OSMW_TOKEN *token)
{
    ++g_service_calls;
    if (owner != TEST_OWNER || callback == NULL || token == NULL ||
            g_subscription_count != 0)
        return OSMW_E_INVALID_ARGUMENT;
    g_subscribe_priority = priority;
    g_tick_callback = callback;
    g_tick_context = context;
    g_subscription_count = 1;
    *token = TEST_TOKEN;
    return OSMW_OK;
}

static OSMW_RESULT __cdecl fake_unsubscribe(
    OSMW_OWNER owner, OSMW_TOKEN token)
{
    ++g_service_calls;
    ++g_unsubscribe_calls;
    if (owner != TEST_OWNER || token != TEST_TOKEN ||
            g_subscription_count != 1)
        return OSMW_E_WRONG_OWNER;
    g_subscription_count = 0;
    return OSMW_OK;
}

static OSMW_RESULT __cdecl fake_module_status(
    const char *name, OSMW_MODULE_STATUS_V1 *output)
{
    ++g_service_calls;
    if (name == NULL || output == NULL || output->struct_size < sizeof(*output))
        return OSMW_E_INVALID_ARGUMENT;
    ZeroMemory(output, sizeof(*output));
    output->struct_size = sizeof(*output);
    output->state = OSMW_MODULE_ACTIVE;
    return OSMW_OK;
}

static void reset_fixture(void)
{
    g_service_calls = 0;
    g_gate_enabled = 1;
    g_missing_signature = NULL;
    g_resolve_calls = 0;
    g_saw_private_uitick = 0;
    g_subscription_count = 0;
    g_unsubscribe_calls = 0;
    g_subscribe_priority = -1;
    g_tick_callback = NULL;
    g_tick_context = NULL;
    g_original_complete = 0;
    g_in_post_tick = 0;
    g_create_calls = 0;
    g_destroy_calls = 0;
    g_destroy_in_post_tick = 0;
    g_destroy_after_unsubscribe = 0;
    g_destroy_handle = -1;
    g_destroy_list = -1;
    g_grid_scale = 1;
    ZeroMemory(g_keys, sizeof(g_keys));
}

static void fill_api(OSMW_CORE_API_V1 *api)
{
    ZeroMemory(api, sizeof(*api));
    api->struct_size = sizeof(*api);
    api->abi_version = OSMW_CORE_ABI_CURRENT;
    api->capabilities = OSMW_CAP_CONFIG | OSMW_CAP_SIGNATURES |
                        OSMW_CAP_UI_TICK | OSMW_CAP_MODULE_STATUS;
    api->game_dir = fake_game_dir;
    api->log = fake_log;
    api->config_read_int = fake_config_read_int;
    api->config_read_string = fake_config_read_string;
    api->config_write_string = fake_config_write_string;
    api->resolve_signature = fake_resolve_signature;
    api->subscribe_post_ui_tick = fake_subscribe;
    api->unsubscribe = fake_unsubscribe;
    api->module_status = fake_module_status;
}

static void run_post_original_tick(void)
{
    if (g_tick_callback == NULL)
        return;
    g_original_complete = 1;
    g_in_post_tick = 1;
    g_tick_callback(TEST_OWNER, g_tick_context, 0);
    g_in_post_tick = 0;
    g_original_complete = 0;
}

int main(int argc, char **argv)
{
    HMODULE module;
    union {
        FARPROC raw;
        OSMW_MOD_QUERY_V1 query;
    } query_address;
    OSMW_MOD_DESCRIPTOR_V1 descriptor;
    OSMW_CORE_API_V1 api;
    OSMW_RESULT result;

    if (argc != 2) {
        fprintf(stderr, "usage: test_lifecycle.exe osmw_settings.dll\n");
        return 2;
    }

    reset_fixture();
    module = LoadLibraryA(argv[1]);
    check(module != NULL, "real settings DLL loads");
    check(g_service_calls == 0,
          "DllMain is passive and performs no service or startup work");
    if (module == NULL)
        return 1;

    query_address.raw = GetProcAddress(module, OSMW_MOD_QUERY_EXPORT_NAME);
    check(query_address.raw != NULL, "undecorated osmw_mod_query resolves");
    if (query_address.raw == NULL) {
        FreeLibrary(module);
        return 1;
    }

    ZeroMemory(&descriptor, sizeof(descriptor));
    result = query_address.query(
        OSMW_ABI_MAKE(2, 0), sizeof(descriptor), &descriptor);
    check(result == OSMW_E_INCOMPATIBLE_ABI,
          "query rejects a different core ABI major");
    result = query_address.query(
        OSMW_CORE_ABI_CURRENT, sizeof(descriptor), NULL);
    check(result == OSMW_E_INVALID_ARGUMENT, "query rejects a null output");
    result = query_address.query(
        OSMW_CORE_ABI_CURRENT, OSMW_MOD_DESCRIPTOR_V1_MIN_SIZE - 1,
        &descriptor);
    check(result == OSMW_E_BUFFER_TOO_SMALL,
          "query rejects a short descriptor buffer");

    ZeroMemory(&descriptor, sizeof(descriptor));
    result = query_address.query(
        OSMW_CORE_ABI_CURRENT, sizeof(descriptor), &descriptor);
    check(result == OSMW_OK && descriptor.struct_size == sizeof(descriptor) &&
              descriptor.required_core_abi == OSMW_CORE_ABI_V1_0 &&
              descriptor.module_version == OSMW_MODULE_VERSION(1, 0) &&
              descriptor.module_id != NULL &&
              strcmp(descriptor.module_id, "settings") == 0 &&
              descriptor.init != NULL && descriptor.shutdown != NULL,
          "query publishes the settings ABI 1 descriptor");
    check(g_service_calls == 0, "query is side-effect free");

    fill_api(&api);
    g_gate_enabled = 0;
    result = descriptor.init(&api, TEST_OWNER);
    check(result == OSMW_MOD_DISABLED && g_resolve_calls == 0 &&
              g_subscription_count == 0,
          "disabled init resolves nothing and owns no subscription");
    check(descriptor.shutdown(&api, TEST_OWNER) == OSMW_OK,
          "disabled module shuts down cleanly");

    reset_fixture();
    fill_api(&api);
    g_missing_signature = "DrawText";
    result = descriptor.init(&api, TEST_OWNER);
    check(result == OSMW_E_NOT_FOUND && g_subscription_count == 0,
          "required signature miss fails before subscription");

    reset_fixture();
    fill_api(&api);
    g_missing_signature = "KeyArray";
    result = descriptor.init(&api, TEST_OWNER);
    check(result == OSMW_OK && g_resolve_calls == TEST_SIGNATURE_COUNT &&
              !g_saw_private_uitick && g_subscription_count == 1 &&
              g_subscribe_priority == OSMW_PRIORITY_DEFAULT,
          "optional miss still activates one priority-0 core subscription");
    check(descriptor.shutdown(&api, TEST_OWNER) == OSMW_OK &&
              g_subscription_count == 0 && g_unsubscribe_calls == 1,
          "optional-miss instance returns its only token on shutdown");

    reset_fixture();
    fill_api(&api);
    result = descriptor.init(&api, TEST_OWNER);
    check(result == OSMW_OK && g_subscription_count == 1 &&
              g_resolve_calls == TEST_SIGNATURE_COUNT &&
              !g_saw_private_uitick,
          "active init resolves only module-owned signatures through core");
    check(g_tick_callback != NULL, "active init publishes a tick callback");
    g_keys[0x58] = 1;
    run_post_original_tick();
    check(g_create_calls == 1,
          "panel node creation runs from the post-original tick callback");

    g_keys[0x58] = 0;
    run_post_original_tick();
    g_keys[0x58] = 1;
    run_post_original_tick();
    check(g_destroy_calls == 1 && g_destroy_in_post_tick == 1,
          "normal panel destruction runs from the post-original callback");

    g_keys[0x58] = 0;
    run_post_original_tick();
    g_keys[0x58] = 1;
    run_post_original_tick();
    check(g_create_calls == 2,
          "panel can reopen before managed shutdown");

    result = descriptor.shutdown(&api, TEST_OWNER);
    check(result == OSMW_OK && g_subscription_count == 0 &&
              g_unsubscribe_calls == 1,
          "active shutdown returns the UiTick token");
    check(g_destroy_calls == 2 && g_destroy_handle == 77 &&
              g_destroy_list == 0 && g_destroy_after_unsubscribe,
          "shutdown destroys the open node after callback revocation");

    FreeLibrary(module);
    check(g_subscription_count == 0,
          "FreeLibrary follows a clean managed shutdown");

    if (g_failures != 0) {
        printf("FAILED: %d settings lifecycle check(s)\n", g_failures);
        return 1;
    }
    printf("PASSED: managed settings lifecycle\n");
    return 0;
}
