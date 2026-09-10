/* Host-side managed lifecycle and early-claim fixture for osmw_chat.dll. */
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "osmw_core_api.h"

#define TEST_OWNER ((OSMW_OWNER)0xC100u)
#define TEST_SIGNATURE_COUNT 42
#define TEST_MAX_CLAIMS 64

typedef struct {
    OSMW_TOKEN token;
    BYTE *address;
    BYTE saved[24];
    DWORD length;
    OSMW_REGION_KIND kind;
    int active;
    char label[64];
} test_claim_t;

static int g_failures;
static int g_service_calls;
static int g_gate_enabled = 1;
static int g_resolve_calls;
static int g_claim_calls;
static int g_fail_claim_call;
static int g_total_claims;
static int g_live_claims;
static int g_release_calls;
static int g_expect_reverse;
static int g_subscribe_claim_count;
static int g_subscribed;
static const char *g_missing_signature;
static OSMW_UI_TICK_CALLBACK_V1 g_tick_callback;
static void *g_tick_context;
static test_claim_t g_claims[TEST_MAX_CLAIMS];

static BYTE *g_generic_code;
static BYTE *g_window_code;
static BYTE *g_font_small;
static BYTE *g_font_large;
static BYTE *g_click_code;
static BYTE *g_pet_click_code;
static BYTE *g_reply_code;
static BYTE *g_chat_object;
static BYTE *g_message_block;
static BYTE *g_messages;
static BYTE *g_input_object;
static BYTE *g_inventory_grid;
static BYTE *g_pet_grid;
static int g_inventory_page;
static int g_pet_page;
static DWORD g_dirty_flag;
static DWORD g_channel;
static DWORD g_last_send;
static DWORD g_now;
static DWORD g_cooldown_table[10];
static int g_background_index;
static int g_background_calls;
static int g_background_fail_slot;
static int g_background_writes;
static int g_stock_background_messages;
static char g_background_config[256];
static int g_remember_active;
static int g_active_tab_writes;
static char g_active_tab_config[256];
static HWND g_main_hwnd;
typedef void (__cdecl *present_fn)(void *context);
static present_fn g_present_slot;

static LRESULT CALLBACK fake_window_proc(HWND hwnd, UINT message,
                                         WPARAM wparam, LPARAM lparam)
{
    if (message == WM_LBUTTONDBLCLK)
        ++g_stock_background_messages;
    return DefWindowProcA(hwnd, message, wparam, lparam);
}

static void check(int condition, const char *message)
{
    if (condition)
        printf("PASS: %s\n", message);
    else {
        printf("FAIL: %s\n", message);
        ++g_failures;
    }
}

static void __cdecl fake_present(void *context)
{
    (void)context;
}

/* __fastcall is call-compatible with the stock no-argument __thiscall
 * routine: the chat object arrives in ecx and nothing is placed on the stack. */
static HRESULT __fastcall fake_background_cycle(void *self, void *edx)
{
    (void)edx;
    ++g_background_calls;
    if (self != g_chat_object)
        ++g_failures;
    g_background_index = (g_background_index + 1) % 5;
    return g_background_index == g_background_fail_slot ? E_FAIL : S_OK;
}

static const char *__cdecl fake_game_dir(void)
{
    ++g_service_calls;
    return ".";
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
    if (owner != TEST_OWNER)
        ++g_failures;
    if (!strcmp(section, "Addons") && !strcmp(key, "ChatMod"))
        return g_gate_enabled;
    if (!strcmp(section, "Chat") && !strcmp(key, "RememberActiveTab"))
        return g_remember_active;
    return fallback;
}

static OSMW_RESULT __cdecl fake_config_read_string(
    OSMW_OWNER owner, const char *section, const char *key,
    const char *fallback, char *output, DWORD output_size)
{
    const char *value = fallback != NULL ? fallback : "";
    size_t length;
    ++g_service_calls;
    if (owner != TEST_OWNER || output == NULL || output_size == 0)
        return OSMW_E_INVALID_ARGUMENT;
    if (!strcmp(section, "Chat") && !strcmp(key, "TabBackgrounds"))
        value = g_background_config;
    if (!strcmp(section, "Chat") && !strcmp(key, "ActiveTab"))
        value = g_active_tab_config;
    length = strlen(value);
    if (length >= output_size)
        return OSMW_E_BUFFER_TOO_SMALL;
    memcpy(output, value, length + 1);
    return OSMW_OK;
}

static OSMW_RESULT __cdecl fake_config_write_string(
    OSMW_OWNER owner, const char *section, const char *key, const char *value)
{
    ++g_service_calls;
    if (owner != TEST_OWNER || !section || !key || !value)
        return OSMW_E_INVALID_ARGUMENT;
    if (!strcmp(section, "Chat") && !strcmp(key, "TabBackgrounds")) {
        _snprintf(g_background_config, sizeof(g_background_config), "%s", value);
        g_background_config[sizeof(g_background_config) - 1] = '\0';
        ++g_background_writes;
    }
    if (!strcmp(section, "Chat") && !strcmp(key, "ActiveTab")) {
        _snprintf(g_active_tab_config, sizeof(g_active_tab_config), "%s", value);
        g_active_tab_config[sizeof(g_active_tab_config) - 1] = '\0';
        ++g_active_tab_writes;
    }
    return OSMW_OK;
}

static void *signature_address(const char *name)
{
    union { WNDPROC function; void *address; } wndproc;
    union {
        HRESULT (__fastcall *function)(void *, void *);
        void *address;
    } background;
    wndproc.function = fake_window_proc;
    background.function = fake_background_cycle;
    if (!strcmp(name, "WndProc")) return wndproc.address;
    if (!strcmp(name, "Reposition")) return g_window_code + 0x6200;
    if (!strcmp(name, "ChipAtlas")) return g_window_code + 0x6300;
    if (!strcmp(name, "HitTest")) return g_window_code + 0x1000;
    if (!strcmp(name, "Render")) return g_window_code + 0x1400;
    if (!strcmp(name, "FontSmall")) return g_font_small;
    if (!strcmp(name, "FontLarge")) return g_font_large;
    if (!strcmp(name, "Object")) return g_chat_object;
    if (!strcmp(name, "MsgArray")) return g_messages;
    if (!strcmp(name, "MainHwnd")) return &g_main_hwnd;
    if (!strcmp(name, "DirtyFlag")) return &g_dirty_flag;
    if (!strcmp(name, "DrawLines")) return g_window_code + 0x3000;
    if (!strcmp(name, "LineCompose")) return g_window_code + 0x4000;
    if (!strcmp(name, "ChatDispatch")) return g_window_code + 0x4200;
    if (!strcmp(name, "FloodGate")) return g_window_code + 0x4300;
    if (!strcmp(name, "FloodMessage")) return g_window_code + 0x4100;
    if (!strcmp(name, "Present")) return g_window_code + 0x6100;
    if (!strcmp(name, "Append")) return g_window_code + 0x6000;
    if (!strcmp(name, "ChatInputInstance")) return g_input_object;
    if (!strcmp(name, "InvClickSlot")) return g_click_code;
    if (!strcmp(name, "InvPage")) return &g_inventory_page;
    if (!strcmp(name, "InvGrid")) return g_inventory_grid;
    if (!strcmp(name, "PetClickSlot")) return g_pet_click_code;
    if (!strcmp(name, "ItemInfoReply")) return g_reply_code;
    if (!strcmp(name, "ScreenPresent")) return &g_present_slot;
    if (!strcmp(name, "BackgroundCycle")) return background.address;
    if (!strcmp(name, "BackgroundIndex")) return &g_background_index;
    return g_generic_code;
}

static OSMW_RESULT __cdecl fake_resolve_signature(
    OSMW_OWNER owner, const OSMW_SIGNATURE_V1 *signature,
    OSMW_RESOLVED_ADDRESS_V1 *output)
{
    ++g_service_calls;
    ++g_resolve_calls;
    if (owner != TEST_OWNER || signature == NULL || output == NULL ||
            signature->struct_size < sizeof(*signature) ||
            output->struct_size < sizeof(*output) || signature->flags ||
            signature->name == NULL)
        return OSMW_E_INVALID_ARGUMENT;
    ZeroMemory(output, sizeof(*output));
    output->struct_size = sizeof(*output);
    if (g_missing_signature &&
            !strcmp(signature->name, g_missing_signature))
        return OSMW_E_NOT_FOUND;
    output->address = signature_address(signature->name);
    output->match_count = 1;
    output->reference_rva = signature->reference_rva;
    output->match_rva = signature->reference_rva;
    return OSMW_OK;
}

static OSMW_RESULT __cdecl fake_claim_region(
    OSMW_OWNER owner, const OSMW_REGION_CLAIM_V1 *claim, OSMW_TOKEN *token)
{
    test_claim_t *record;
    int index;
    ++g_service_calls;
    ++g_claim_calls;
    if (owner != TEST_OWNER || claim == NULL || token == NULL ||
            claim->struct_size < sizeof(*claim) || claim->flags ||
            claim->address == NULL || claim->length == 0 ||
            claim->length > sizeof(record->saved) || claim->label == NULL)
        return OSMW_E_INVALID_ARGUMENT;
    if (g_fail_claim_call == g_claim_calls)
        return OSMW_E_ALREADY_CLAIMED;
    for (index = 0; index < g_total_claims; ++index) {
        BYTE *claim_start;
        BYTE *claim_end;
        BYTE *live_start;
        BYTE *live_end;

        if (!g_claims[index].active)
            continue;
        claim_start = (BYTE *)claim->address;
        claim_end = claim_start + claim->length;
        live_start = g_claims[index].address;
        live_end = live_start + g_claims[index].length;
        if (claim_start < live_end && live_start < claim_end)
            return OSMW_E_ALREADY_CLAIMED;
    }
    if (g_total_claims >= TEST_MAX_CLAIMS)
        return OSMW_E_INTERNAL;

    if (strstr(claim->label, "CreateFontA") != NULL)
        check(claim->kind == OSMW_REGION_CALL_SITE && claim->length == 6,
              "font call has an exact six-byte call-site claim");
    else if (strstr(claim->label, "inventory item-link") != NULL)
        check(claim->kind == OSMW_REGION_CODE && claim->length == 6,
              "item-link click has an exact six-byte code claim");
    else if (strstr(claim->label, "pet item-link") != NULL)
        check(claim->kind == OSMW_REGION_CODE && claim->length == 6,
              "pet-link click has an exact six-byte code claim");
    else if (strstr(claim->label, "reply prologue") != NULL)
        check(claim->kind == OSMW_REGION_CODE && claim->length == 5,
              "item-link reply has an exact five-byte code claim");
    else if (strstr(claim->label, "whisper-click") != NULL)
        check(claim->kind == OSMW_REGION_CODE && claim->length == 5,
              "whisper click has an adjacent five-byte code claim");
    else if (strstr(claim->label, "screen-present") != NULL)
        check(claim->kind == OSMW_REGION_DATA &&
              claim->length == sizeof(void *),
              "screen-present owner claims one x86 pointer");
    else if (strstr(claim->label, "background index") != NULL)
        check(claim->kind == OSMW_REGION_DATA && claim->length == sizeof(int),
              "background owner claims the exact stock index");

    record = &g_claims[g_total_claims];
    record->token = (OSMW_TOKEN)(0xC110u + (DWORD)g_total_claims);
    record->address = (BYTE *)claim->address;
    record->length = claim->length;
    record->kind = claim->kind;
    _snprintf(record->label, sizeof(record->label), "%s", claim->label);
    record->label[sizeof(record->label) - 1] = '\0';
    memcpy(record->saved, record->address, record->length);
    record->active = 1;
    *token = record->token;
    ++g_total_claims;
    ++g_live_claims;
    return OSMW_OK;
}

static OSMW_RESULT __cdecl fake_release_region(OSMW_OWNER owner,
                                                OSMW_TOKEN token)
{
    int index;
    ++g_service_calls;
    if (owner != TEST_OWNER)
        return OSMW_E_WRONG_OWNER;
    if (g_expect_reverse && g_release_calls < g_total_claims)
        check(token == g_claims[g_total_claims - 1 - g_release_calls].token,
              "early chat claims release in reverse order");
    for (index = 0; index < g_total_claims; ++index) {
        test_claim_t *record = &g_claims[index];
        if (!record->active || record->token != token)
            continue;
        check(!memcmp(record->address, record->saved, record->length),
              "chat bytes/pointer restore before claim release");
        record->active = 0;
        --g_live_claims;
        ++g_release_calls;
        return OSMW_OK;
    }
    return OSMW_E_WRONG_OWNER;
}

static OSMW_RESULT __cdecl fake_subscribe(
    OSMW_OWNER owner, LONG priority, OSMW_UI_TICK_CALLBACK_V1 callback,
    void *context, OSMW_TOKEN *token)
{
    ++g_service_calls;
    if (owner != TEST_OWNER || priority != OSMW_PRIORITY_DEFAULT ||
            callback == NULL || token == NULL)
        return OSMW_E_INVALID_ARGUMENT;
    g_subscribe_claim_count = g_claim_calls;
    g_tick_callback = callback;
    g_tick_context = context;
    g_subscribed = 1;
    *token = (OSMW_TOKEN)0xC1F0u;
    return OSMW_OK;
}

static OSMW_RESULT __cdecl fake_unsubscribe(OSMW_OWNER owner,
                                             OSMW_TOKEN token)
{
    ++g_service_calls;
    if (owner != TEST_OWNER)
        return OSMW_E_WRONG_OWNER;
    if (!g_subscribed || token != (OSMW_TOKEN)0xC1F0u)
        return OSMW_E_NOT_FOUND;
    g_subscribed = 0;
    g_tick_callback = NULL;
    g_tick_context = NULL;
    return OSMW_OK;
}

static OSMW_RESULT __cdecl fake_queue(OSMW_OWNER owner, DWORD type,
                                      DWORD field12, const void *extra,
                                      DWORD extra_size)
{
    (void)type;
    (void)field12;
    (void)extra;
    (void)extra_size;
    ++g_service_calls;
    return owner == TEST_OWNER ? OSMW_OK : OSMW_E_WRONG_OWNER;
}

static void fill_api(OSMW_CORE_API_V1 *api)
{
    ZeroMemory(api, sizeof(*api));
    api->struct_size = sizeof(*api);
    api->abi_version = OSMW_CORE_ABI_CURRENT;
    api->capabilities = OSMW_CAP_CONFIG | OSMW_CAP_SIGNATURES |
                        OSMW_CAP_REGION_CLAIMS | OSMW_CAP_UI_TICK |
                        OSMW_CAP_SEND_QUEUE;
    api->game_dir = fake_game_dir;
    api->log = fake_log;
    api->config_read_int = fake_config_read_int;
    api->config_read_string = fake_config_read_string;
    api->config_write_string = fake_config_write_string;
    api->resolve_signature = fake_resolve_signature;
    api->claim_region = fake_claim_region;
    api->release_region = fake_release_region;
    api->subscribe_post_ui_tick = fake_subscribe;
    api->unsubscribe = fake_unsubscribe;
    api->queue_game_packet = fake_queue;
}

static void make_direct_call(BYTE *site, BYTE *target)
{
    site[0] = 0xE8;
    *(LONG *)(site + 1) = (LONG)(target - (site + 5));
}

static void reset_fixture(void)
{
    BYTE *hit_test;
    BYTE *render;
    BYTE *dispatch;
    BYTE *gate;
    int index;

    g_service_calls = 0;
    g_resolve_calls = 0;
    g_claim_calls = 0;
    g_fail_claim_call = 0;
    g_total_claims = 0;
    g_live_claims = 0;
    g_release_calls = 0;
    g_expect_reverse = 0;
    g_subscribe_claim_count = 0;
    g_subscribed = 0;
    g_tick_callback = NULL;
    g_tick_context = NULL;
    g_gate_enabled = 1;
    g_missing_signature = NULL;
    ZeroMemory(g_claims, sizeof(g_claims));
    memset(g_generic_code, 0x90, 0x1000);
    memset(g_window_code, 0x90, 0x8000);
    memset(g_font_small, 0x90, 0x40);
    memset(g_font_large, 0x90, 0x40);
    g_font_small[17] = 0xFF;
    g_font_small[18] = 0x15;
    g_font_large[17] = 0xFF;
    g_font_large[18] = 0x15;
    memset(g_click_code, 0x90, 0x40);
    memset(g_pet_click_code, 0x90, 0x40);
    memset(g_reply_code, 0x90, 0x40);
    ZeroMemory(g_chat_object, 0x8000);
    ZeroMemory(g_message_block, 0x10u + 0xC0u * 1024u);
    for (index = 0; index < 1024; ++index)
        *(DWORD *)(g_messages + (size_t)index * 0xC0u) = 0x80000000u;
    ZeroMemory(g_input_object, 0x200);
    *(int *)(g_input_object + 0x120) = 160;
    ZeroMemory(g_inventory_grid, 24u * 20u);
    ZeroMemory(g_pet_grid, 20u * 0x74u);
    g_inventory_page = 0;
    g_pet_page = 0;
    g_dirty_flag = 0;
    g_channel = 0;
    g_last_send = 0;
    g_now = 5000;
    g_background_index = 0;
    g_background_calls = 0;
    g_background_fail_slot = -1;
    g_background_writes = 0;
    g_stock_background_messages = 0;
    g_background_config[0] = '\0';
    g_remember_active = 1;
    g_active_tab_writes = 0;
    g_active_tab_config[0] = '\0';
    g_main_hwnd = NULL;
    g_present_slot = fake_present;

    g_pet_click_code[0] = 0x8B;
    g_pet_click_code[1] = 0x0D;
    *(DWORD *)(g_pet_click_code + 2) = (DWORD)(UINT_PTR)&g_pet_page;
    g_pet_click_code[6] = 0x8D;
    g_pet_click_code[7] = 0x54;
    g_pet_click_code[8] = 0x39;
    g_pet_click_code[9] = 0x9C;
    g_pet_click_code[10] = 0x6B;
    g_pet_click_code[11] = 0xD2;
    g_pet_click_code[12] = 0x74;
    g_pet_click_code[13] = 0x39;
    g_pet_click_code[14] = 0x9A;
    *(DWORD *)(g_pet_click_code + 15) = (DWORD)(UINT_PTR)g_pet_grid;
    g_pet_click_code[19] = 0x0F;
    g_pet_click_code[20] = 0x86;
    *(LONG *)(g_pet_click_code + 21) =
        (LONG)((g_pet_click_code + 0x30) - (g_pet_click_code + 25));
    g_pet_click_code[25] = 0x8D;
    g_pet_click_code[26] = 0x44;
    g_pet_click_code[27] = 0x24;
    g_pet_click_code[28] = 0x10;
    hit_test = g_window_code + 0x1000;
    render = g_window_code + 0x1400;
    for (index = 0; index < 11; ++index)
        *(DWORD *)(hit_test + 0x20 + index * 8) =
            (DWORD)(UINT_PTR)(g_messages + index * 4);
    hit_test[0xAA] = 0x8D;
    hit_test[0xAB] = 0xB0;
    *(DWORD *)(hit_test + 0xAC) = (DWORD)(UINT_PTR)(g_messages + 8);
    hit_test[0xB0] = 0xBA;
    *(DWORD *)(hit_test + 0xB1) = (DWORD)(UINT_PTR)(g_window_code + 0x7000);

    render[0x367] = 0x6A;
    render[0x368] = 18;
    render[0x58D] = 0x6A;
    render[0x58E] = 18;
    g_window_code[0x3000 + 0x62] = 0x6A;
    g_window_code[0x3000 + 0x63] = 18;

    make_direct_call(g_window_code + 0x4000 + 25, g_generic_code);
    make_direct_call(g_window_code + 0x4100 + 14, g_generic_code);

    dispatch = g_window_code + 0x4200;
    *(DWORD *)(dispatch + 2) = (DWORD)(UINT_PTR)&g_channel;
    *(DWORD *)(dispatch + 17) = (DWORD)(UINT_PTR)g_cooldown_table;
    gate = g_window_code + 0x4300;
    *(DWORD *)(gate + 2) = (DWORD)(UINT_PTR)&g_last_send;
    *(DWORD *)(gate + 9) = (DWORD)(UINT_PTR)&g_now;
    for (index = 0; index < 10; ++index) {
        BYTE *cooldown = g_window_code + 0x5000 + index * 0x40;
        cooldown[0] = 0xB8;
        *(DWORD *)(cooldown + 1) = 2000;
        g_cooldown_table[index] = (DWORD)(UINT_PTR)cooldown;
    }
}

static int allocate_fixture(void)
{
    g_generic_code = (BYTE *)VirtualAlloc(NULL, 0x1000,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    g_window_code = (BYTE *)VirtualAlloc(NULL, 0x8000,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    g_font_small = (BYTE *)VirtualAlloc(NULL, 0x40,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    g_font_large = (BYTE *)VirtualAlloc(NULL, 0x40,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    g_click_code = (BYTE *)VirtualAlloc(NULL, 0x40,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    g_pet_click_code = (BYTE *)VirtualAlloc(NULL, 0x40,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    g_reply_code = (BYTE *)VirtualAlloc(NULL, 0x40,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    g_chat_object = (BYTE *)VirtualAlloc(NULL, 0x8000,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_message_block = (BYTE *)VirtualAlloc(NULL, 0x10u + 0xC0u * 1024u,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_messages = g_message_block != NULL ? g_message_block + 0x10 : NULL;
    g_input_object = (BYTE *)VirtualAlloc(NULL, 0x200,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_inventory_grid = (BYTE *)VirtualAlloc(NULL, 24u * 20u,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_pet_grid = (BYTE *)VirtualAlloc(NULL, 20u * 0x74u,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    return g_generic_code && g_window_code && g_font_small && g_font_large &&
           g_click_code && g_pet_click_code && g_reply_code &&
           g_chat_object && g_messages && g_input_object &&
           g_inventory_grid && g_pet_grid;
}

int main(int argc, char **argv)
{
    HMODULE module;
    union { FARPROC raw; OSMW_MOD_QUERY_V1 query; } query_address;
    OSMW_MOD_DESCRIPTOR_V1 descriptor;
    OSMW_CORE_API_V1 api;
    OSMW_RESULT result;
    WNDCLASSEXA window_class;
    HWND window;
    const char *class_name = "osmw_chat_lifecycle_fixture";

    if (argc != 2) {
        fprintf(stderr, "usage: test_lifecycle.exe dll\n");
        return 2;
    }
    check(allocate_fixture(), "chat host patch fixtures allocate");
    if (g_failures)
        return 1;
    reset_fixture();

    module = LoadLibraryA(argv[1]);
    check(module != NULL, "real chat DLL loads");
    check(g_service_calls == 0, "DllMain is passive");
    if (!module)
        return 1;
    query_address.raw = GetProcAddress(module, OSMW_MOD_QUERY_EXPORT_NAME);
    check(query_address.raw != NULL, "undecorated osmw_mod_query resolves");

    ZeroMemory(&descriptor, sizeof(descriptor));
    result = query_address.query(OSMW_ABI_MAKE(2, 0),
                                 sizeof(descriptor), &descriptor);
    check(result == OSMW_E_INCOMPATIBLE_ABI,
          "query rejects another ABI major");
    result = query_address.query(OSMW_CORE_ABI_CURRENT,
                                 sizeof(descriptor), NULL);
    check(result == OSMW_E_INVALID_ARGUMENT, "query rejects null output");
    result = query_address.query(OSMW_CORE_ABI_CURRENT,
        OSMW_MOD_DESCRIPTOR_V1_MIN_SIZE - 1, &descriptor);
    check(result == OSMW_E_BUFFER_TOO_SMALL, "query rejects short output");
    ZeroMemory(&descriptor, sizeof(descriptor));
    result = query_address.query(OSMW_CORE_ABI_CURRENT,
                                 sizeof(descriptor), &descriptor);
    check(result == OSMW_OK && !strcmp(descriptor.module_id, "chat") &&
          descriptor.init && descriptor.shutdown,
          "query publishes the chat ABI 1 descriptor");
    check(g_service_calls == 0, "query is side-effect free");

    fill_api(&api);
    g_gate_enabled = 0;
    result = descriptor.init(&api, TEST_OWNER);
    check(result == OSMW_MOD_DISABLED && !g_resolve_calls && !g_live_claims,
          "disabled chat resolves and claims nothing");
    check(descriptor.shutdown(&api, TEST_OWNER) == OSMW_OK,
          "disabled chat shuts down cleanly");

    reset_fixture();
    fill_api(&api);
    g_missing_signature = "FontSmall";
    result = descriptor.init(&api, TEST_OWNER);
    check(result == OSMW_E_NOT_FOUND && !g_live_claims,
          "missing required chat signature fails before patching");
    check(descriptor.shutdown(&api, TEST_OWNER) == OSMW_OK,
          "failed signature instance rolls back cleanly");

    reset_fixture();
    fill_api(&api);
    g_fail_claim_call = 2;
    result = descriptor.init(&api, TEST_OWNER);
    check(result == OSMW_E_INTERNAL && !g_live_claims,
          "second font claim conflict restores and rolls back the first");
    check(descriptor.shutdown(&api, TEST_OWNER) == OSMW_OK,
          "claim-conflict instance leaves no binding behind");

    reset_fixture();
    fill_api(&api);
    g_present_slot = NULL;
    result = descriptor.init(&api, TEST_OWNER);
    check(result == OSMW_OK && g_live_claims == 5 && g_subscribed,
          "early core load defers a null screen-present slot");
    g_present_slot = fake_present;
    if (g_tick_callback)
        g_tick_callback(TEST_OWNER, g_tick_context, 0);
    check(g_live_claims == 6 && g_present_slot != fake_present,
          "first live game tick acquires the deferred screen-present slot");
    {
        int claims_after_attach = g_total_claims;
        if (g_tick_callback)
            g_tick_callback(TEST_OWNER, g_tick_context, 0);
        check(g_total_claims == claims_after_attach,
              "deferred screen-present acquisition is idempotent");
    }
    g_expect_reverse = 1;
    check(descriptor.shutdown(&api, TEST_OWNER) == OSMW_OK,
          "deferred-present chat shuts down cleanly");
    check(!g_live_claims && !g_subscribed && g_release_calls == 6 &&
          g_present_slot == fake_present,
          "deferred screen-present pointer and claims restore exactly");

    reset_fixture();
    fill_api(&api);
    g_missing_signature = "Append";
    result = descriptor.init(&api, TEST_OWNER);
    check(result == OSMW_OK && g_resolve_calls == TEST_SIGNATURE_COUNT,
          "optional Append miss preserves managed chat startup");
    check(g_subscribe_claim_count == 6,
          "both font hooks and early item-link hooks precede tick subscription");
    check(g_live_claims == 6 && g_subscribed,
          "chat owns six early ranges and one callback without an HWND");
    if (g_tick_callback)
        g_tick_callback(TEST_OWNER, g_tick_context, 0);
    check(g_live_claims == 6,
          "nonblocking window tick adds nothing while the HWND is absent");
    check(descriptor.shutdown(&api, TEST_OWNER + 1) == OSMW_E_WRONG_OWNER &&
          g_live_claims == 6, "wrong owner cannot shut chat down");
    g_expect_reverse = 1;
    check(descriptor.shutdown(&api, TEST_OWNER) == OSMW_OK,
          "managed chat shutdown succeeds");
    check(!g_live_claims && !g_subscribed && g_release_calls == 6,
          "chat unsubscribes and releases every early range");
    check(g_present_slot == fake_present,
          "screen-present pointer is restored exactly");
    check(g_font_small[17] == 0xFF && g_font_small[18] == 0x15 &&
          g_font_large[17] == 0xFF && g_font_large[18] == 0x15,
          "both one-shot font calls are restored exactly");

    reset_fixture();
    fill_api(&api);
    g_missing_signature = "PetClickSlot";
    result = descriptor.init(&api, TEST_OWNER);
    check(result == OSMW_OK && g_resolve_calls == TEST_SIGNATURE_COUNT,
          "optional pet-click miss preserves managed chat startup");
    check(g_subscribe_claim_count == 5 && g_live_claims == 5,
          "missing pet-click signature leaves existing item links active");
    g_expect_reverse = 1;
    check(descriptor.shutdown(&api, TEST_OWNER) == OSMW_OK,
          "item-only chat startup shuts down cleanly");
    check(!g_live_claims && !g_subscribed && g_release_calls == 5,
          "item-only startup releases each early range");

    reset_fixture();
    fill_api(&api);
    g_missing_signature = "PetTooltipBranch";
    result = descriptor.init(&api, TEST_OWNER);
    check(result == OSMW_OK && g_resolve_calls == TEST_SIGNATURE_COUNT,
          "optional pet-tooltip miss preserves managed chat startup");
    check(g_live_claims == 6 && g_subscribed,
          "missing pet-tooltip signature retains compose and item renderer");
    g_expect_reverse = 1;
    check(descriptor.shutdown(&api, TEST_OWNER) == OSMW_OK,
          "pet-tooltip-fallback startup shuts down cleanly");
    check(!g_live_claims && !g_subscribed && g_release_calls == 6,
          "pet-tooltip-fallback startup releases every early range");

    reset_fixture();
    fill_api(&api);
    ZeroMemory(&window_class, sizeof(window_class));
    window_class.cbSize = sizeof(window_class);
    window_class.hInstance = GetModuleHandleA(NULL);
    window_class.lpfnWndProc = fake_window_proc;
    window_class.lpszClassName = class_name;
    check(RegisterClassExA(&window_class) != 0,
          "chat fixture window class registers");
    window = CreateWindowExA(0, class_name, "chat fixture", WS_OVERLAPPED,
                             0, 0, 320, 200, NULL, NULL,
                             window_class.hInstance, NULL);
    check(window != NULL, "chat fixture HWND is created");
    g_main_hwnd = window;
    *(HWND *)(g_chat_object + 4) = window;
    g_background_index = 2;
    _snprintf(g_background_config, sizeof(g_background_config),
              "All:1|Chat:3|System:4");
    _snprintf(g_active_tab_config, sizeof(g_active_tab_config), "Chat");
    result = descriptor.init(&api, TEST_OWNER);
    check(result == OSMW_OK && g_live_claims == 6,
          "full chat fixture starts with only early claims");
    if (g_tick_callback)
        g_tick_callback(TEST_OWNER, g_tick_context, 0);
    printf("INFO: full window owns %d claims (%d total acquisitions)\n",
           g_live_claims, g_total_claims);
    if (g_total_claims != 31) {
        int claim_index;
        for (claim_index = 0; claim_index < g_total_claims; ++claim_index)
            printf("INFO: claim %02d %s\n", claim_index + 1,
                   g_claims[claim_index].label);
    }
    check(g_live_claims == 31,
          "window transition installs all 25 deferred exact claims");
    check(g_background_index == 3 && g_background_calls == 1,
          "startup restores the active tab by name and applies its background");
    SendMessageA(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(20, 10));
    check(g_background_index == 1 && g_background_calls == 2 &&
          g_active_tab_writes == 1 && !strcmp(g_active_tab_config, "All"),
          "tab switch persists its name and applies that tab's background");
    SendMessageA(window, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(120, 60));
    check(g_background_index == 2 && g_background_calls == 3 &&
          g_background_writes == 1 && strstr(g_background_config, "All:2") != NULL,
          "unused-background double-click cycles and persists the active tab");
    SendMessageA(window, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(120, 10));
    check(g_background_index == 2 && g_background_calls == 3,
          "double-clicking the tab strip does not cycle the background");
    {
        RECT client;
        int line_y;
        GetClientRect(window, &client);
        *(RECT *)(g_chat_object + 0x28) = client;
        g_chat_object[0x7A63] = 1;
        *(int *)(g_chat_object + 0x80 + 0x408) = 20;
        line_y = client.bottom - 30;
        SendMessageA(window, WM_LBUTTONDBLCLK, MK_LBUTTON,
                     MAKELPARAM(120, line_y));
    check(g_background_index == 2 && g_background_calls == 3,
          "double-clicking an occupied chat row does not cycle the background");
        g_chat_object[0x7A63] = 0;
    }
    g_background_fail_slot = 3;
    SendMessageA(window, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(120, 60));
    check(g_background_index == 4 && g_background_calls == 5 &&
          g_background_writes == 2 && strstr(g_background_config, "All:4") != NULL,
          "failed BMP rolls back, skips forward, and persists only a valid slot");
    check(g_stock_background_messages == 0,
          "managed background gestures never fall through to the stock global cycle");
    check((WNDPROC)(UINT_PTR)GetWindowLongA(window, GWL_WNDPROC) !=
              fake_window_proc,
          "one chat-owned WndProc dispatcher is installed");
    g_expect_reverse = 1;
    check(descriptor.shutdown(&api, TEST_OWNER) == OSMW_OK,
          "full managed chat shutdown succeeds");
    check(!g_live_claims && !g_subscribed && g_release_calls == 31,
          "full chat restores and releases all 31 ranges in reverse");
    check(g_background_index == 2 && g_background_calls == 6,
          "shutdown restores the startup background through the stock loader");
    check((WNDPROC)(UINT_PTR)GetWindowLongA(window, GWL_WNDPROC) ==
              fake_window_proc,
          "chat WndProc is restored before shutdown returns");

    reset_fixture();
    fill_api(&api);
    g_missing_signature = "BackgroundCycle";
    g_remember_active = 0;
    _snprintf(g_active_tab_config, sizeof(g_active_tab_config), "System");
    g_main_hwnd = window;
    *(HWND *)(g_chat_object + 4) = window;
    g_background_index = 2;
    result = descriptor.init(&api, TEST_OWNER);
    check(result == OSMW_OK && g_resolve_calls == TEST_SIGNATURE_COUNT,
          "missing optional background signature preserves managed chat startup");
    if (g_tick_callback)
        g_tick_callback(TEST_OWNER, g_tick_context, 0);
    check(g_live_claims == 30,
          "unavailable backgrounds add no data claim or partial feature state");
    {
        char title[128];
        GetWindowTextA(window, title, sizeof(title));
        check(strstr(title, "System only") == NULL,
              "disabled active-tab memory starts on the first tab");
    }
    SendMessageA(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(120, 10));
    check(g_active_tab_writes == 0 && !strcmp(g_active_tab_config, "System"),
          "disabled active-tab memory does not overwrite the saved choice");
    SendMessageA(window, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(120, 60));
    check(g_stock_background_messages == 1 && g_background_calls == 0,
          "unavailable feature passes double-click through to the stock handler");
    g_expect_reverse = 1;
    check(descriptor.shutdown(&api, TEST_OWNER) == OSMW_OK &&
          !g_live_claims && g_release_calls == 30,
          "background-unavailable lifecycle restores every remaining claim");
    if (window)
        DestroyWindow(window);
    UnregisterClassA(class_name, window_class.hInstance);

    check(FreeLibrary(module) != 0, "real chat DLL unloads after shutdown");
    printf("chat lifecycle fixture: %d failure(s)\n", g_failures);
    return g_failures ? 1 : 0;
}
