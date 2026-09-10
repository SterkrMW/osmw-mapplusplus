/* Passive managed entry point and nonblocking chat startup state machine. */
#include "osmw_chat.h"

#include <stdio.h>

#define CHAT_REQUIRED_CAPABILITIES \
    (OSMW_CAP_CONFIG | OSMW_CAP_SIGNATURES | OSMW_CAP_REGION_CLAIMS | \
     OSMW_CAP_UI_TICK | OSMW_CAP_SEND_QUEUE)

#define CHAT_OBJ_HWND        0x04
#define CHAT_OBJ_WINDOW_RECT 0x18
#define CHAT_OBJ_CLIENT_RECT 0x28
#define CHAT_OBJ_MEM_DC      0x44
#define CHAT_OBJ_WND_DC      0x48
#define CHAT_OBJ_LINE_COUNT  0x7A63
#define CHAT_OBJ_FONT        0x7A80

#define MSG_RECORD_STRIDE 0xC0
#define MSG_RECORD_COUNT  1024
#define MSG_EMPTY_SENDER  0x80000000U

static const char *CHANNEL_NAMES[] = {
    "Local", "Party", "PM", "Guild", "Market",
    "World", "System", "Info", "FD", "GM"
};
#define CHANNEL_COUNT ((int)(sizeof(CHANNEL_NAMES) / sizeof(CHANNEL_NAMES[0])))

static const OSMW_CORE_API_V1 *g_api;
static OSMW_OWNER g_owner = OSMW_INVALID_OWNER;
static OSMW_TOKEN g_tick_token = OSMW_INVALID_TOKEN;
static OSMW_TOKEN g_wndproc_claim = OSMW_INVALID_TOKEN;
static int g_active;
static int g_window_features;
static DWORD g_wait_log_at;

const OSMW_CORE_API_V1 *mw_core_api(void)
{
    return g_api;
}

OSMW_OWNER mw_core_owner(void)
{
    return g_owner;
}

static const char *channel_name(unsigned int channel)
{
    return channel < (unsigned int)CHANNEL_COUNT
        ? CHANNEL_NAMES[channel] : "?";
}

static void dump_chat_state(unsigned char *obj, HWND hwnd)
{
    RECT *window_rect = (RECT *)(obj + CHAT_OBJ_WINDOW_RECT);
    RECT *client_rect = (RECT *)(obj + CHAT_OBJ_CLIENT_RECT);
    char title[128] = {0};
    char class_name[128] = {0};
    LONG style;
    HFONT font;
    LOGFONTA description;

    GetWindowTextA(hwnd, title, sizeof(title) - 1);
    GetClassNameA(hwnd, class_name, sizeof(class_name) - 1);
    style = GetWindowLongA(hwnd, GWL_STYLE);
    mw_log("chat: hwnd %p class \"%s\" title \"%s\" style 0x%08lX",
           hwnd, class_name, title, (unsigned long)style);
    mw_log("chat: window rect %d,%d %dx%d client %dx%d",
           window_rect->left, window_rect->top,
           window_rect->right - window_rect->left,
           window_rect->bottom - window_rect->top,
           client_rect->right - client_rect->left,
           client_rect->bottom - client_rect->top);
    mw_log("chat: memDC %p windowDC %p font %p lines %d",
           *(HDC *)(obj + CHAT_OBJ_MEM_DC),
           *(HDC *)(obj + CHAT_OBJ_WND_DC),
           *(HGDIOBJ *)(obj + CHAT_OBJ_FONT),
           obj[CHAT_OBJ_LINE_COUNT] & 0x7F);

    font = *(HFONT *)(obj + CHAT_OBJ_FONT);
    if (font && GetObjectA(font, sizeof(description), &description) ==
            sizeof(description)) {
        mw_log("font: \"%s\" h=%d w=%d weight=%d charset=%d pitch=0x%02X",
               description.lfFaceName, description.lfHeight,
               description.lfWidth, description.lfWeight,
               description.lfCharSet, description.lfPitchAndFamily);
    }
}

static void dump_message_channels(unsigned char *messages)
{
    int counts[16] = {0};
    int used = 0;
    int shown = 0;
    int index;

    if (messages == NULL)
        return;
    for (index = 0; index < MSG_RECORD_COUNT; ++index) {
        unsigned char *record = messages +
            (size_t)index * MSG_RECORD_STRIDE;
        unsigned int sender = *(unsigned int *)(record + 0);
        unsigned int channel = *(unsigned int *)(record + 4);
        const char *message = (const char *)(record + 8);
        if (sender == MSG_EMPTY_SENDER)
            continue;
        ++used;
        if (channel < 16)
            ++counts[channel];
        if (shown < 8 && message[0]) {
            mw_log("msg[%4d] sender 0x%08X ch %u (%-6s) \"%.60s\"",
                   index, sender, channel, channel_name(channel), message);
            ++shown;
        }
    }
    mw_log("msgs: %d/%d slots in use", used, MSG_RECORD_COUNT);
    for (index = 0; index < CHANNEL_COUNT; ++index) {
        if (counts[index])
            mw_log("msgs: ch %d %-6s = %d", index,
                   CHANNEL_NAMES[index], counts[index]);
    }
}

static OSMW_RESULT remove_window_features(void)
{
    OSMW_RESULT result;

    if (!g_window_features && g_wndproc_claim == OSMW_INVALID_TOKEN)
        return OSMW_OK;

    result = mw_block_remove();
    if (result != OSMW_OK) return result;
    result = mw_backgrounds_remove();
    if (result != OSMW_OK) return result;
    result = mw_tabs_remove();
    if (result != OSMW_OK) return result;
    result = mw_time_remove();
    if (result != OSMW_OK) return result;
    result = mw_mirror_remove();
    if (result != OSMW_OK) return result;
    result = mw_chips_remove();
    if (result != OSMW_OK) return result;
    result = mw_cooldown_remove();
    if (result != OSMW_OK) return result;
    mw_scroll_remove();
    mw_clear_remove();
    result = mw_geometry_remove();
    if (result != OSMW_OK) return result;
    result = mw_window_remove();
    if (result != OSMW_OK) return result;
    result = mw_release_region(&g_wndproc_claim);
    if (result != OSMW_OK) return result;

    g_window_features = 0;
    mw_log("chat: deferred window features removed");
    return OSMW_OK;
}

static int install_window_features(unsigned char *obj, HWND hwnd)
{
    OSMW_RESULT result;

    result = mw_claim_region(mw_addr[MW_WNDPROC], 1, OSMW_REGION_CODE,
                             "chat WndProc subclass owner",
                             &g_wndproc_claim);
    if (result != OSMW_OK) {
        mw_log("window: ownership claim refused (%ld)", (long)result);
        return 0;
    }
    if (!mw_window_install(hwnd)) {
        (void)mw_release_region(&g_wndproc_claim);
        return 0;
    }

    /* Mark the state as soon as the dispatcher is live so any partial feature
     * set still follows the same reverse teardown transaction. */
    g_window_features = 1;
    dump_chat_state(obj, hwnd);
    dump_message_channels(mw_addr[MW_MSGARRAY]);

    (void)mw_geometry_install(hwnd);
    (void)mw_clear_install(hwnd);
    (void)mw_scroll_install();
    (void)mw_cooldown_install();
    (void)mw_chips_install();
    (void)mw_mirror_install();
    (void)mw_time_install();
    (void)mw_tabs_install(hwnd);
    (void)mw_backgrounds_install();
    (void)mw_block_install();
    mw_log("chat: deferred window features installed");
    return 1;
}

static void __cdecl chat_tick(OSMW_OWNER owner, void *context,
                              LONG list_selector)
{
    unsigned char *obj;
    HWND wanted;
    HWND current;
    OSMW_RESULT result;
    DWORD now;

    (void)context;
    (void)list_selector;
    if (!g_active || owner != g_owner)
        return;

    /* Core autoload can initialise chat before the client publishes its
     * screen-present pointer. Acquire it on the first tick where it is live,
     * without waiting for the chat HWND to exist. */
    mw_links_poll();

    obj = mw_addr[MW_OBJECT];
    wanted = obj != NULL ? *(HWND *)(obj + CHAT_OBJ_HWND) : NULL;
    current = mw_window_hwnd();
    if (g_window_features &&
            (!current || !IsWindow(current) || wanted != current)) {
        result = remove_window_features();
        if (result != OSMW_OK) {
            mw_log("chat: window transition cleanup failed (%ld); "
                   "module retained", (long)result);
            g_active = 0;
            return;
        }
    }

    if (!g_window_features) {
        if (wanted && IsWindow(wanted)) {
            (void)install_window_features(obj, wanted);
        } else {
            now = GetTickCount();
            if (now - g_wait_log_at >= 30000u) {
                mw_log("chat: waiting for chat window without blocking a worker");
                g_wait_log_at = now;
            }
        }
    }
    if (g_window_features)
        mw_window_poll_settings();
}

static void clear_binding(void)
{
    g_active = 0;
    g_window_features = 0;
    g_tick_token = OSMW_INVALID_TOKEN;
    g_wndproc_claim = OSMW_INVALID_TOKEN;
    g_wait_log_at = 0;
    g_owner = OSMW_INVALID_OWNER;
    g_api = NULL;
}

static OSMW_RESULT cleanup_resources(const OSMW_CORE_API_V1 *api,
                                     OSMW_OWNER owner)
{
    OSMW_RESULT result;

    g_active = 0;
    if (g_tick_token != OSMW_INVALID_TOKEN) {
        result = api->unsubscribe(owner, g_tick_token);
        if (result != OSMW_OK)
            return result;
        g_tick_token = OSMW_INVALID_TOKEN;
    }
    result = remove_window_features();
    if (result != OSMW_OK)
        return result;
    result = mw_links_remove();
    if (result != OSMW_OK)
        return result;
    result = mw_font_remove();
    if (result != OSMW_OK)
        return result;
    result = mw_claim_cleanup_result();
    if (result != OSMW_OK)
        return result;

    mw_resolve_reset();
    clear_binding();
    return OSMW_OK;
}

static OSMW_RESULT rollback_init(OSMW_RESULT failure)
{
    OSMW_RESULT cleanup = cleanup_resources(g_api, g_owner);
    return cleanup == OSMW_OK ? failure : cleanup;
}

static OSMW_RESULT __cdecl chat_init(const OSMW_CORE_API_V1 *api,
                                     OSMW_OWNER owner)
{
    OSMW_RESULT result;
    DWORD started;

    if (api == NULL || owner == OSMW_INVALID_OWNER)
        return OSMW_E_INVALID_ARGUMENT;
    if (api->struct_size < OSMW_CORE_API_V1_0_SIZE ||
            OSMW_ABI_MAJOR(api->abi_version) !=
                OSMW_ABI_MAJOR(OSMW_CORE_ABI_V1_0))
        return OSMW_E_INCOMPATIBLE_ABI;
    if ((api->capabilities & CHAT_REQUIRED_CAPABILITIES) !=
            CHAT_REQUIRED_CAPABILITIES ||
            api->game_dir == NULL || api->log == NULL ||
            api->config_read_int == NULL ||
            api->config_read_string == NULL ||
            api->config_write_string == NULL ||
            api->resolve_signature == NULL ||
            api->claim_region == NULL || api->release_region == NULL ||
            api->subscribe_post_ui_tick == NULL ||
            api->unsubscribe == NULL || api->queue_game_packet == NULL)
        return OSMW_E_UNAVAILABLE;
    if (g_api != NULL)
        return OSMW_E_BUSY;

    g_api = api;
    g_owner = owner;
    g_wait_log_at = GetTickCount();
    started = g_wait_log_at;
    mw_log("osmw_chat managed init");

    if (!mw_cfg_int_in("Addons", "ChatMod", 1)) {
        mw_log("[Addons] ChatMod=0 -- module disabled, client untouched");
        return OSMW_MOD_DISABLED;
    }

    result = mw_resolve_all();
    if (result != OSMW_OK)
        return rollback_init(result);

    /* These one-shot CreateFontA calls happen during client startup. Install
     * them synchronously in init, before links or deferred window discovery. */
    if (!mw_font_install())
        return rollback_init(OSMW_E_INTERNAL);
    mw_log("font: managed hooks ready %lu ms after chat init began",
           (unsigned long)(GetTickCount() - started));

    /* Item-link inventory hooks do not depend on the chat HWND. They keep
     * their existing optional reply/present degradation. */
    if (!mw_links_install())
        mw_log("links: unavailable; remaining chat features continue");

    result = api->subscribe_post_ui_tick(
        owner, OSMW_PRIORITY_DEFAULT, chat_tick, NULL, &g_tick_token);
    if (result != OSMW_OK)
        return rollback_init(result);

    g_active = 1;
    mw_log("startup complete; %d signatures resolved through core, "
           "%lu early claim(s), nonblocking post-UiTick window discovery",
           MW_SIG_COUNT, (unsigned long)mw_claim_count());
    return OSMW_OK;
}

static OSMW_RESULT __cdecl chat_shutdown(const OSMW_CORE_API_V1 *api,
                                         OSMW_OWNER owner)
{
    OSMW_RESULT result;

    if (g_api == NULL)
        return OSMW_OK;
    if (api != g_api)
        return OSMW_E_INVALID_ARGUMENT;
    if (owner != g_owner)
        return OSMW_E_WRONG_OWNER;
    result = cleanup_resources(api, owner);
    if (result != OSMW_OK)
        mw_log("shutdown: chat cleanup failed (%ld)", (long)result);
    return result;
}

OSMW_RESULT __cdecl osmw_mod_query(DWORD core_abi_version,
                                   DWORD output_size,
                                   OSMW_MOD_DESCRIPTOR_V1 *output)
{
    if (OSMW_ABI_MAJOR(core_abi_version) !=
            OSMW_ABI_MAJOR(OSMW_CORE_ABI_V1_0))
        return OSMW_E_INCOMPATIBLE_ABI;
    if (output == NULL)
        return OSMW_E_INVALID_ARGUMENT;
    if (output_size < OSMW_MOD_DESCRIPTOR_V1_MIN_SIZE)
        return OSMW_E_BUFFER_TOO_SMALL;

    ZeroMemory(output, sizeof(*output));
    output->struct_size = sizeof(*output);
    output->required_core_abi = OSMW_CORE_ABI_V1_0;
    output->module_version = OSMW_MODULE_VERSION(1, 0);
    output->module_id = "chat";
    output->display_name = "osMW Chat Window";
    output->init = chat_init;
    output->shutdown = chat_shutdown;
    return OSMW_OK;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(instance);
    return TRUE;
}
