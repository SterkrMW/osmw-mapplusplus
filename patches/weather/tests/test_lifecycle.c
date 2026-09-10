/* Host-side lifecycle and exact-claim fixture for the real weather DLL. */
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "osmw_core_api.h"

#define TEST_OWNER ((OSMW_OWNER)0x7100u)
#define TEST_SIGNATURE_COUNT 10
#define TEST_MAX_CLAIMS 8

typedef struct {
    OSMW_TOKEN token;
    BYTE *address;
    BYTE saved[5];
    int active;
} test_claim_t;

static int g_failures, g_service_calls, g_resolve_calls;
static int g_gate_enabled = 1, g_icons_enabled = 1, g_names_enabled = 1;
static int g_claim_calls, g_fail_claim_call, g_live_claims, g_release_calls;
static const char *g_missing_signature;
static test_claim_t g_claims[TEST_MAX_CLAIMS];
static BYTE *g_render_anchor;
/* Two synthetic weather-name call sites, laid out byte for byte the way the
 * signatures find the client's: the E8 sits at exactly the offset the mod
 * adds to the resolved address, and the target opens the way both of the
 * client's builders do. */
static BYTE *g_tooltip_anchor;
static BYTE *g_status_anchor;
static int   g_name_probe;
static char  g_name_line[256];
static volatile LONG g_anchor_calls;
static int g_icon_bank_value;
static void *g_colour_converter;

static void check(int condition, const char *message)
{
    if (condition) printf("PASS: %s\n", message);
    else { printf("FAIL: %s\n", message); ++g_failures; }
}

static void __declspec(noinline) __cdecl fake_setweather(unsigned code)
{ InterlockedExchangeAdd(&g_anchor_calls, (LONG)code); }
static void __declspec(noinline) __cdecl fake_draw(void *surface)
{ (void)surface; InterlockedIncrement(&g_anchor_calls); }
static int __declspec(noinline) __cdecl fake_icon_picker(void)
{ InterlockedIncrement(&g_anchor_calls); return 0; }

/* Two call sites, and both carry a real code: heavy sandstorm, so driving
 * this anchor after init leaves the mod with a group running and an intensity
 * to name it with. */
#define TEST_HEAVY_SANDSTORM 0x02000010u
static void __declspec(noinline) setweather_call_anchor(void)
{
    fake_setweather(TEST_HEAVY_SANDSTORM);
    fake_setweather(TEST_HEAVY_SANDSTORM);
    InterlockedIncrement(&g_anchor_calls);
}

/* Mirrors the client invariant: one picker call, then an A1 bank load at
 * call-site + 0x1B. */
__declspec(naked) static int icon_call_anchor(void)
{
    __asm {
        call fake_icon_picker
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        mov eax, g_icon_bank_value
        ret
    }
}

/* What the client's builders produce for a weather it has no word for. */
static void __cdecl tooltip_body(char *dest)
{ strcpy(dest, "Sunny - Northwest Wind"); }
static void __cdecl status_body(char *dest)
{ strcpy(dest, "#YOranus#N: #YSunny#N and #YNorthwest#N Wind "); }

/* `mov eax, [abs32]` then `sub esp, 0A0h` -- the prologue the mod checks for
 * before it believes a resolved call target is a name builder.
 *
 * Naked, so `dest` is documentation for the reader and for the call: it is
 * reached through [esp+0A4h] in the body rather than by name. */
#pragma warning(push)
#pragma warning(disable: 4100)
__declspec(naked) static void __cdecl fake_tooltip_name(char *dest)
{
    __asm {
        mov  eax, g_name_probe
        sub  esp, 0A0h
        mov  eax, [esp+0A4h]
        push eax
        call tooltip_body
        add  esp, 4
        add  esp, 0A0h
        ret
    }
}

__declspec(naked) static void __cdecl fake_status_name(char *dest)
{
    __asm {
        mov  eax, g_name_probe
        sub  esp, 0A0h
        mov  eax, [esp+0A4h]
        push eax
        call status_body
        add  esp, 4
        add  esp, 0A0h
        ret
    }
}
#pragma warning(pop)

/* void anchor(char *dest): re-push the argument, then `call rel32` at the
 * offset the signature guarantees. Built as bytes rather than as inline asm
 * so the E8 lands exactly where the mod will look for it. */
static void build_name_anchor(BYTE *anchor, int call_offset, void *target)
{
    int i = 0;
    LONG displacement;

    memset(anchor, 0x90, 0x40);
    anchor[i++] = 0x8B; anchor[i++] = 0x44; anchor[i++] = 0x24; anchor[i++] = 0x04;
    anchor[i++] = 0x50;
    anchor[call_offset] = 0xE8;
    displacement = (LONG)((BYTE *)target - (anchor + call_offset + 5));
    *(LONG *)(anchor + call_offset + 1) = displacement;
    i = call_offset + 5;
    anchor[i++] = 0x83; anchor[i++] = 0xC4; anchor[i++] = 0x04;
    anchor[i++] = 0xC3;
}

static void call_name_anchor(BYTE *anchor, char *dest)
{
    union { BYTE *bytes; void (__cdecl *fn)(char *); } entry;
    entry.bytes = anchor;
    entry.fn(dest);
}

static void __cdecl fake_log(OSMW_OWNER owner, OSMW_LOG_LEVEL level,
                             const char *message)
{
    ++g_service_calls;
    if (owner != TEST_OWNER || level > OSMW_LOG_ERROR || message == NULL)
        ++g_failures;
}

static int __cdecl fake_config_read_int(OSMW_OWNER owner, const char *section,
                                        const char *key, int fallback)
{
    ++g_service_calls;
    if (owner != TEST_OWNER) ++g_failures;
    if (!strcmp(section, "Addons") && !strcmp(key, "Weather"))
        return g_gate_enabled;
    if (!strcmp(section, "Weather") && !strcmp(key, "Icons"))
        return g_icons_enabled;
    if (!strcmp(section, "Weather") && !strcmp(key, "Names"))
        return g_names_enabled;
    return fallback;
}

static OSMW_RESULT __cdecl fake_config_read_string(
    OSMW_OWNER owner, const char *section, const char *key,
    const char *fallback, char *output, DWORD output_size)
{
    const char *value = fallback ? fallback : "";
    size_t length = strlen(value);
    (void)section; (void)key; ++g_service_calls;
    if (owner != TEST_OWNER || output == NULL || output_size == 0)
        return OSMW_E_INVALID_ARGUMENT;
    if (length >= output_size) return OSMW_E_BUFFER_TOO_SMALL;
    memcpy(output, value, length + 1);
    return OSMW_OK;
}

static OSMW_RESULT __cdecl fake_config_write_string(
    OSMW_OWNER owner, const char *section, const char *key, const char *value)
{
    ++g_service_calls;
    return owner == TEST_OWNER && section && key && value
        ? OSMW_OK : OSMW_E_INVALID_ARGUMENT;
}

static OSMW_RESULT __cdecl fake_resolve_signature(
    OSMW_OWNER owner, const OSMW_SIGNATURE_V1 *signature,
    OSMW_RESOLVED_ADDRESS_V1 *output)
{
    union { void (__cdecl *fn)(unsigned); void *address; } set_address;
    union { void (__cdecl *fn)(void *); void *address; } draw_address;
    union { int (__cdecl *fn)(void); void *address; } icon_address;
    ++g_service_calls; ++g_resolve_calls;
    if (owner != TEST_OWNER || !signature || !output ||
        signature->struct_size < sizeof(*signature) ||
        output->struct_size < sizeof(*output) || signature->flags ||
        !signature->name) return OSMW_E_INVALID_ARGUMENT;
    ZeroMemory(output, sizeof(*output));
    output->struct_size = sizeof(*output);
    if (g_missing_signature && !strcmp(signature->name, g_missing_signature))
        return OSMW_E_NOT_FOUND;
    set_address.fn = fake_setweather;
    draw_address.fn = fake_draw;
    icon_address.fn = fake_icon_picker;
    if (!strcmp(signature->name, "SetCode")) output->address = set_address.address;
    else if (!strcmp(signature->name, "RenderCallSite")) output->address = g_render_anchor;
    else if (!strcmp(signature->name, "Draw")) output->address = draw_address.address;
    else if (!strcmp(signature->name, "ColourConv")) output->address = &g_colour_converter;
    else if (!strcmp(signature->name, "IconPicker")) output->address = icon_address.address;
    else if (!strcmp(signature->name, "TooltipCallSite")) output->address = g_tooltip_anchor;
    else if (!strcmp(signature->name, "StatusCallSite")) output->address = g_status_anchor;
    else output->address = draw_address.address;
    output->match_count = 1;
    output->reference_rva = signature->reference_rva;
    output->match_rva = signature->reference_rva;
    return OSMW_OK;
}

static OSMW_RESULT __cdecl fake_claim_region(
    OSMW_OWNER owner, const OSMW_REGION_CLAIM_V1 *claim, OSMW_TOKEN *token)
{
    test_claim_t *record;
    ++g_service_calls; ++g_claim_calls;
    if (owner != TEST_OWNER || !claim || !token ||
        claim->struct_size < sizeof(*claim) || claim->flags ||
        !claim->address || claim->length != 5 ||
        claim->kind != OSMW_REGION_CALL_SITE || !claim->label)
        return OSMW_E_INVALID_ARGUMENT;
    check(*(BYTE *)claim->address == 0xE8,
          "weather call site is claimed before rewrite");
    if (g_fail_claim_call == g_claim_calls) return OSMW_E_ALREADY_CLAIMED;
    if (g_live_claims >= TEST_MAX_CLAIMS) return OSMW_E_INTERNAL;
    record = &g_claims[g_live_claims];
    record->token = (OSMW_TOKEN)(0x7110u + (DWORD)g_live_claims);
    record->address = (BYTE *)claim->address;
    memcpy(record->saved, record->address, 5);
    record->active = 1;
    *token = record->token;
    ++g_live_claims;
    return OSMW_OK;
}

static OSMW_RESULT __cdecl fake_release_region(OSMW_OWNER owner, OSMW_TOKEN token)
{
    int index;
    ++g_service_calls;
    if (owner != TEST_OWNER) return OSMW_E_WRONG_OWNER;
    for (index = 0; index < TEST_MAX_CLAIMS; ++index) {
        test_claim_t *record = &g_claims[index];
        if (!record->active || record->token != token) continue;
        check(!memcmp(record->address, record->saved, 5),
              "weather bytes are restored before claim release");
        record->active = 0;
        --g_live_claims; ++g_release_calls;
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
    api->log = fake_log;
    api->config_read_int = fake_config_read_int;
    api->config_read_string = fake_config_read_string;
    api->config_write_string = fake_config_write_string;
    api->resolve_signature = fake_resolve_signature;
    api->claim_region = fake_claim_region;
    api->release_region = fake_release_region;
}

static void reset_fixture(void)
{
    LONG displacement;
    g_service_calls = g_resolve_calls = g_claim_calls = 0;
    g_fail_claim_call = g_live_claims = g_release_calls = 0;
    g_gate_enabled = g_icons_enabled = g_names_enabled = 1;
    g_missing_signature = NULL;
    ZeroMemory(g_claims, sizeof(g_claims));
    memset(g_render_anchor, 0x90, 0x40);
    g_render_anchor[0x23] = 0xE8;
    displacement = (LONG)((BYTE *)fake_draw - (g_render_anchor + 0x28));
    *(LONG *)(g_render_anchor + 0x24) = displacement;
    build_name_anchor(g_tooltip_anchor, 0x12, (void *)fake_tooltip_name);
    build_name_anchor(g_status_anchor, 0x05, (void *)fake_status_name);
}

int main(int argc, char **argv)
{
    HMODULE module;
    union { FARPROC raw; OSMW_MOD_QUERY_V1 query; } query_address;
    OSMW_MOD_DESCRIPTOR_V1 descriptor;
    OSMW_CORE_API_V1 api;
    OSMW_RESULT result;
    if (argc != 2) { fprintf(stderr, "usage: test_lifecycle.exe dll\n"); return 2; }
    g_render_anchor = (BYTE *)VirtualAlloc(NULL, 0x40, MEM_COMMIT | MEM_RESERVE,
                                           PAGE_EXECUTE_READWRITE);
    g_tooltip_anchor = (BYTE *)VirtualAlloc(NULL, 0x40, MEM_COMMIT | MEM_RESERVE,
                                            PAGE_EXECUTE_READWRITE);
    g_status_anchor = (BYTE *)VirtualAlloc(NULL, 0x40, MEM_COMMIT | MEM_RESERVE,
                                           PAGE_EXECUTE_READWRITE);
    check(g_render_anchor && g_tooltip_anchor && g_status_anchor,
          "call-site fixtures allocate");
    if (!g_render_anchor || !g_tooltip_anchor || !g_status_anchor) return 1;
    setweather_call_anchor(); (void)icon_call_anchor(); reset_fixture();
    module = LoadLibraryA(argv[1]);
    check(module != NULL, "real weather DLL loads");
    check(g_service_calls == 0, "DllMain is passive");
    if (!module) return 1;
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
    check(result == OSMW_OK && !strcmp(descriptor.module_id, "weather") &&
          descriptor.init && descriptor.shutdown,
          "query publishes the weather ABI 1 descriptor");
    check(g_service_calls == 0, "query is side-effect free");

    fill_api(&api); g_gate_enabled = 0;
    result = descriptor.init(&api, TEST_OWNER);
    check(result == OSMW_MOD_DISABLED && !g_resolve_calls && !g_live_claims,
          "disabled weather resolves and claims nothing");
    check(descriptor.shutdown(&api, TEST_OWNER) == OSMW_OK,
          "disabled instance shuts down cleanly");

    reset_fixture(); fill_api(&api); g_missing_signature = "DrawPixel";
    result = descriptor.init(&api, TEST_OWNER);
    check(result == OSMW_E_NOT_FOUND && !g_live_claims,
          "required signature miss occurs before claims");

    /* Claims go out in install order: icon, tooltip name, status name, two
     * SetWeather callers, draw. Failing the last one must unwind all five. */
    reset_fixture(); fill_api(&api); g_fail_claim_call = 6;
    result = descriptor.init(&api, TEST_OWNER);
    check(result == OSMW_E_INTERNAL && !g_live_claims && g_release_calls == 5,
          "draw claim conflict restores icon, name and SetWeather callers");

    /* A name call site that is already spoken for costs the names and nothing
     * else: the effects still install. */
    reset_fixture(); fill_api(&api); g_fail_claim_call = 2;
    result = descriptor.init(&api, TEST_OWNER);
    check(result == OSMW_OK && g_live_claims == 4,
          "a refused name call site leaves the weather effects installed");
    check(descriptor.shutdown(&api, TEST_OWNER) == OSMW_OK && !g_live_claims,
          "shutdown after a refused name call site is clean");

    /* Names=0 is the honest off switch: the icon hook is unaffected. */
    reset_fixture(); fill_api(&api); g_names_enabled = 0;
    result = descriptor.init(&api, TEST_OWNER);
    check(result == OSMW_OK && g_live_claims == 4,
          "Names=0 installs everything but the two name callers");
    check(descriptor.shutdown(&api, TEST_OWNER) == OSMW_OK && !g_live_claims,
          "shutdown with Names=0 is clean");

    reset_fixture(); fill_api(&api);
    result = descriptor.init(&api, TEST_OWNER);
    check(result == OSMW_OK && g_resolve_calls == TEST_SIGNATURE_COUNT &&
          g_claim_calls == 6 && g_live_claims == 6,
          "active init claims icon, both name callers, two SetWeather callers "
          "and the draw caller");

    /* End to end: with no group running the detours must be invisible, and
     * with one running both lines must name it and keep everything the client
     * built around it. */
    call_name_anchor(g_tooltip_anchor, g_name_line);
    check(!strcmp(g_name_line, "Sunny - Northwest Wind"),
          "an idle sky still reads exactly as the client built it");
    setweather_call_anchor();
    call_name_anchor(g_tooltip_anchor, g_name_line);
    check(strstr(g_name_line, "Sandstorm - Northwest Wind") != NULL &&
          strstr(g_name_line, "Sunny") == NULL,
          "the tooltip names the running group and keeps the wind");
    call_name_anchor(g_status_anchor, g_name_line);
    check(strstr(g_name_line, "#YOranus#N: #Y") == g_name_line &&
          strstr(g_name_line, "Sandstorm#N and #YNorthwest#N Wind ") != NULL &&
          strstr(g_name_line, "Sunny") == NULL,
          "the status line substitutes only the weather field");

    check(descriptor.shutdown(&api, TEST_OWNER + 1) == OSMW_E_WRONG_OWNER &&
          g_live_claims == 6, "wrong-owner shutdown preserves weather patches");
    result = descriptor.shutdown(&api, TEST_OWNER);
    check(result == OSMW_OK && !g_live_claims && g_release_calls == 6,
          "shutdown restores and releases every weather call site");
    call_name_anchor(g_tooltip_anchor, g_name_line);
    check(!strcmp(g_name_line, "Sunny - Northwest Wind"),
          "after shutdown the client builds its own text again");
    FreeLibrary(module);
    if (g_render_anchor) VirtualFree(g_render_anchor, 0, MEM_RELEASE);
    if (g_tooltip_anchor) VirtualFree(g_tooltip_anchor, 0, MEM_RELEASE);
    if (g_status_anchor) VirtualFree(g_status_anchor, 0, MEM_RELEASE);
    if (g_failures) { printf("FAILED: %d weather checks\n", g_failures); return 1; }
    printf("PASSED: managed weather lifecycle\n");
    return 0;
}
