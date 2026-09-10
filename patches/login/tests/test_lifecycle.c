/* Host-side lifecycle fixture for the real managed osmw_login.dll. */
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "osmw_core_api.h"
#include "roster.h"
#include "claim.h"

#define TEST_OWNER ((OSMW_OWNER)0x6100u)
#define TEST_SIGNATURE_COUNT 10
#define TEST_HOOK_LENGTH 6

typedef struct test_claim {
    OSMW_TOKEN token;
    BYTE *address;
    BYTE saved[TEST_HOOK_LENGTH];
    int active;
} test_claim_t;

static int g_failures;
static int g_service_calls;
static int g_gate_enabled = 1;
static int g_resolve_calls;
static const char *g_missing_signature;
static int g_claim_calls;
static int g_fail_claim_call;
static int g_live_claims;
static int g_release_calls;
static OSMW_TOKEN g_release_order[2];
static test_claim_t g_claims[2];
static BYTE *g_login_tick;
static BYTE *g_char_tick;
static BYTE *g_char_select;
static DWORD g_dummy;
static char g_game_directory[MAX_PATH];
static char g_roster_path[MAX_PATH];

static void check(int condition, const char *message)
{
    if (condition)
        printf("PASS: %s\n", message);
    else {
        printf("FAIL: %s\n", message);
        ++g_failures;
    }
}

static int __cdecl fake_send4(unsigned op, unsigned a, unsigned b, unsigned c)
{
    (void)op; (void)a; (void)b; (void)c;
    return 0;
}

static const char *__cdecl fake_game_dir(void)
{
    ++g_service_calls;
    return g_game_directory;
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
    if (strcmp(section, "Addons") == 0 && strcmp(key, "AutoLogin") == 0)
        return g_gate_enabled;
    return fallback;
}

static OSMW_RESULT __cdecl fake_resolve_signature(
    OSMW_OWNER owner, const OSMW_SIGNATURE_V1 *signature,
    OSMW_RESOLVED_ADDRESS_V1 *output)
{
    ++g_service_calls;
    ++g_resolve_calls;
    if (owner != TEST_OWNER || signature == NULL || output == NULL ||
            signature->struct_size < sizeof(*signature) ||
            output->struct_size < sizeof(*output) || signature->flags != 0 ||
            signature->name == NULL)
        return OSMW_E_INVALID_ARGUMENT;

    ZeroMemory(output, sizeof(*output));
    output->struct_size = sizeof(*output);
    if (g_missing_signature != NULL &&
            strcmp(signature->name, g_missing_signature) == 0)
        return OSMW_E_NOT_FOUND;

    if (strcmp(signature->name, "Tick") == 0)
        output->address = g_login_tick;
    else if (strcmp(signature->name, "CharTick") == 0)
        output->address = g_char_tick;
    else if (strcmp(signature->name, "CharSelectNext") == 0)
        output->address = g_char_select;
    else
        output->address = &g_dummy;
    output->match_count = 1;
    output->reference_rva = signature->reference_rva;
    output->match_rva = signature->reference_rva;
    return OSMW_OK;
}

static OSMW_RESULT __cdecl fake_claim_region(
    OSMW_OWNER owner, const OSMW_REGION_CLAIM_V1 *claim, OSMW_TOKEN *token)
{
    test_claim_t *record;
    ++g_service_calls;
    ++g_claim_calls;
    if (owner != TEST_OWNER || claim == NULL || token == NULL ||
            claim->struct_size < sizeof(*claim) || claim->flags != 0 ||
            claim->address == NULL || claim->length != TEST_HOOK_LENGTH ||
            claim->kind != OSMW_REGION_CODE || claim->label == NULL)
        return OSMW_E_INVALID_ARGUMENT;
    if (g_fail_claim_call == g_claim_calls)
        return OSMW_E_ALREADY_CLAIMED;
    if (g_live_claims >= 2)
        return OSMW_E_INTERNAL;

    record = &g_claims[g_live_claims];
    record->token = (OSMW_TOKEN)(0x6110u + (DWORD)g_live_claims);
    record->address = (BYTE *)claim->address;
    memcpy(record->saved, record->address, TEST_HOOK_LENGTH);
    record->active = 1;
    *token = record->token;
    ++g_live_claims;
    return OSMW_OK;
}

static OSMW_RESULT __cdecl fake_release_region(
    OSMW_OWNER owner, OSMW_TOKEN token)
{
    int index;
    ++g_service_calls;
    if (owner != TEST_OWNER)
        return OSMW_E_WRONG_OWNER;
    for (index = 0; index < 2; ++index) {
        test_claim_t *record = &g_claims[index];
        if (!record->active || record->token != token)
            continue;
        check(memcmp(record->address, record->saved, TEST_HOOK_LENGTH) == 0,
              "hook bytes are restored before their claim is released");
        record->active = 0;
        --g_live_claims;
        if (g_release_calls < 2)
            g_release_order[g_release_calls] = token;
        ++g_release_calls;
        return OSMW_OK;
    }
    return OSMW_E_WRONG_OWNER;
}

static void fill_api(OSMW_CORE_API_V1 *api)
{
    ZeroMemory(api, sizeof(*api));
    api->struct_size = sizeof(*api);
    api->abi_version = OSMW_CORE_ABI_CURRENT;
    api->capabilities = OSMW_CAP_CONFIG | OSMW_CAP_SIGNATURES |
                        OSMW_CAP_REGION_CLAIMS;
    api->game_dir = fake_game_dir;
    api->log = fake_log;
    api->config_read_int = fake_config_read_int;
    api->resolve_signature = fake_resolve_signature;
    api->claim_region = fake_claim_region;
    api->release_region = fake_release_region;
}

static void reset_fixture(void)
{
    static const BYTE login_bytes[TEST_HOOK_LENGTH] =
        {0x39, 0x1D, 0, 0, 0, 0};
    static const BYTE char_bytes[TEST_HOOK_LENGTH] =
        {0x8B, 0x3D, 0, 0, 0, 0};
    LONG displacement;

    g_service_calls = 0;
    g_gate_enabled = 1;
    g_resolve_calls = 0;
    g_missing_signature = NULL;
    g_claim_calls = 0;
    g_fail_claim_call = 0;
    g_live_claims = 0;
    g_release_calls = 0;
    ZeroMemory(g_claims, sizeof(g_claims));
    ZeroMemory(g_release_order, sizeof(g_release_order));
    memcpy(g_login_tick, login_bytes, sizeof(login_bytes));
    memcpy(g_char_tick, char_bytes, sizeof(char_bytes));
    memset(g_char_select, 0x90, 0x40);
    g_char_select[0] = 0x68;
    *(DWORD *)(g_char_select + 1) = 0x00030008u;
    g_char_select[5] = 0xE8;
    displacement = (LONG)((BYTE *)fake_send4 - (g_char_select + 10));
    *(LONG *)(g_char_select + 6) = displacement;
    lg_claim_clear();
}

static int create_roster(void)
{
    lg_roster_t roster;
    int result;
    ZeroMemory(&roster, sizeof(roster));
    if (!lg_roster_add(&roster, "fixture-account", "fixture-password", "#1"))
        return 0;
    SetEnvironmentVariableA("OSMW_LOGIN_ROW", roster.rows[0].id);
    result = lg_roster_save(g_roster_path, &roster);
    lg_roster_free(&roster);
    return result;
}

int main(int argc, char **argv)
{
    HMODULE module;
    union { FARPROC raw; OSMW_MOD_QUERY_V1 query; } query_address;
    OSMW_MOD_DESCRIPTOR_V1 descriptor;
    OSMW_CORE_API_V1 api;
    OSMW_RESULT result;

    if (argc != 2) {
        fprintf(stderr, "usage: test_lifecycle.exe osmw_login.dll\n");
        return 2;
    }
    GetTempPathA(sizeof(g_game_directory), g_game_directory);
    _snprintf(g_game_directory + strlen(g_game_directory),
              sizeof(g_game_directory) - strlen(g_game_directory),
              "osmw_login_lifecycle_%lu", (unsigned long)GetCurrentProcessId());
    CreateDirectoryA(g_game_directory, NULL);
    _snprintf(g_roster_path, sizeof(g_roster_path), "%s\\%s",
              g_game_directory, LG_ROSTER_NAME);
    g_login_tick = (BYTE *)VirtualAlloc(NULL, 0x40, MEM_COMMIT | MEM_RESERVE,
                                        PAGE_EXECUTE_READWRITE);
    g_char_tick = (BYTE *)VirtualAlloc(NULL, 0x40, MEM_COMMIT | MEM_RESERVE,
                                       PAGE_EXECUTE_READWRITE);
    g_char_select = (BYTE *)VirtualAlloc(NULL, 0x40, MEM_COMMIT | MEM_RESERVE,
                                         PAGE_EXECUTE_READWRITE);
    check(g_login_tick != NULL && g_char_tick != NULL && g_char_select != NULL,
          "fixture hook pages allocate");
    check(create_roster(), "fixture roster is written with DPAPI protection");
    SetEnvironmentVariableA("OSMW_AUTOLOGIN", "1");

    reset_fixture();
    module = LoadLibraryA(argv[1]);
    check(module != NULL, "real login DLL loads");
    check(g_service_calls == 0, "DllMain is passive");
    if (module == NULL)
        return 1;
    query_address.raw = GetProcAddress(module, OSMW_MOD_QUERY_EXPORT_NAME);
    check(query_address.raw != NULL, "undecorated osmw_mod_query resolves");

    ZeroMemory(&descriptor, sizeof(descriptor));
    result = query_address.query(OSMW_ABI_MAKE(2, 0), sizeof(descriptor), &descriptor);
    check(result == OSMW_E_INCOMPATIBLE_ABI, "query rejects another ABI major");
    result = query_address.query(OSMW_CORE_ABI_CURRENT, sizeof(descriptor), NULL);
    check(result == OSMW_E_INVALID_ARGUMENT, "query rejects null output");
    result = query_address.query(OSMW_CORE_ABI_CURRENT,
                                 OSMW_MOD_DESCRIPTOR_V1_MIN_SIZE - 1, &descriptor);
    check(result == OSMW_E_BUFFER_TOO_SMALL, "query rejects a short buffer");
    ZeroMemory(&descriptor, sizeof(descriptor));
    result = query_address.query(OSMW_CORE_ABI_CURRENT, sizeof(descriptor), &descriptor);
    check(result == OSMW_OK && strcmp(descriptor.module_id, "login") == 0 &&
              descriptor.init != NULL && descriptor.shutdown != NULL,
          "query publishes the login ABI 1 descriptor");
    check(g_service_calls == 0, "query is side-effect free");

    fill_api(&api);
    g_gate_enabled = 0;
    result = descriptor.init(&api, TEST_OWNER);
    check(result == OSMW_MOD_DISABLED && g_resolve_calls == 0 && g_live_claims == 0,
          "disabled gate resolves and claims nothing");
    check(descriptor.shutdown(&api, TEST_OWNER) == OSMW_OK,
          "disabled instance shuts down cleanly");

    reset_fixture();
    fill_api(&api);
    SetEnvironmentVariableA("OSMW_AUTOLOGIN", NULL);
    result = descriptor.init(&api, TEST_OWNER);
    check(result == OSMW_MOD_DISABLED && g_resolve_calls == 0 && g_live_claims == 0,
          "absent per-launch consent leaves the client untouched");
    check(descriptor.shutdown(&api, TEST_OWNER) == OSMW_OK,
          "non-consenting instance shuts down cleanly");
    SetEnvironmentVariableA("OSMW_AUTOLOGIN", "1");

    reset_fixture();
    fill_api(&api);
    g_missing_signature = "Focus";
    result = descriptor.init(&api, TEST_OWNER);
    check(result == OSMW_E_NOT_FOUND && g_live_claims == 0,
          "signature failure occurs before any claim");

    reset_fixture();
    fill_api(&api);
    g_fail_claim_call = 1;
    result = descriptor.init(&api, TEST_OWNER);
    check(result == OSMW_E_INTERNAL && g_live_claims == 0 &&
              g_login_tick[0] != 0xE9,
          "first claim conflict writes no hook and rolls back roster state");

    reset_fixture();
    fill_api(&api);
    result = descriptor.init(&api, TEST_OWNER);
    check(result == OSMW_OK && g_resolve_calls == TEST_SIGNATURE_COUNT &&
              g_live_claims == 2 && g_login_tick[0] == 0xE9 &&
              g_char_tick[0] == 0xE9,
          "active init resolves ten requests and installs two claimed hooks");
    check(descriptor.shutdown(&api, TEST_OWNER + 1) == OSMW_E_WRONG_OWNER &&
              g_live_claims == 2,
          "wrong-owner shutdown cannot disturb live hooks");
    result = descriptor.shutdown(&api, TEST_OWNER);
    check(result == OSMW_OK && g_live_claims == 0 && g_release_calls == 2,
          "shutdown restores and releases both hooks");
    check(g_release_order[0] == (OSMW_TOKEN)0x6111u &&
              g_release_order[1] == (OSMW_TOKEN)0x6110u,
          "shutdown releases character then login prologue in reverse order");

    FreeLibrary(module);
    lg_claim_clear();
    SetEnvironmentVariableA("OSMW_AUTOLOGIN", NULL);
    SetEnvironmentVariableA("OSMW_LOGIN_ROW", NULL);
    DeleteFileA(g_roster_path);
    RemoveDirectoryA(g_game_directory);
    if (g_login_tick) VirtualFree(g_login_tick, 0, MEM_RELEASE);
    if (g_char_tick) VirtualFree(g_char_tick, 0, MEM_RELEASE);
    if (g_char_select) VirtualFree(g_char_select, 0, MEM_RELEASE);

    if (g_failures != 0) {
        printf("FAILED: %d login lifecycle check(s)\n", g_failures);
        return 1;
    }
    printf("PASSED: managed login lifecycle\n");
    return 0;
}
