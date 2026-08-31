/* Entry point and Phase 1 bring-up.
 *
 * Phase 1 deliberately changes no client behaviour. It resolves every hook
 * point, waits for the chat window to exist (it is created on character
 * login, not at startup), and dumps enough state to prove the address map is
 * correct before any later phase starts patching.
 */
#include "osmw_chat.h"

#include <stdio.h>

#define CHAT_OBJ_HWND        0x04
#define CHAT_OBJ_WINDOW_RECT 0x18
#define CHAT_OBJ_CLIENT_RECT 0x28
#define CHAT_OBJ_MEM_DC      0x44
#define CHAT_OBJ_WND_DC      0x48
#define CHAT_OBJ_LINE_COUNT  0x7A63
#define CHAT_OBJ_FONT        0x7A80

#define MSG_RECORD_STRIDE    0xC0
#define MSG_RECORD_COUNT     1024
#define MSG_EMPTY_SENDER     0x80000000U

static const char *CHANNEL_NAMES[] = {
    "Local", "Party", "PM", "Guild", "Market", "World", "System", "Info", "FD", "GM"
};
#define CHANNEL_COUNT ((int)(sizeof(CHANNEL_NAMES) / sizeof(CHANNEL_NAMES[0])))

static HMODULE g_self;
static HANDLE  g_worker;
static volatile LONG g_stop;

static const char *channel_name(unsigned int ch)
{
    return (ch < (unsigned int)CHANNEL_COUNT) ? CHANNEL_NAMES[ch] : "?";
}

/* Wait for the chat window. It appears on character login, so this can be a
 * long wait -- or never, if the player quits at the title screen. */
static HWND wait_for_chat_window(unsigned char *obj, int timeout_ms)
{
    int waited = 0;
    while (!g_stop && (timeout_ms <= 0 || waited < timeout_ms)) {
        HWND hwnd = *(HWND *)(obj + CHAT_OBJ_HWND);
        if (hwnd && IsWindow(hwnd))
            return hwnd;
        Sleep(250);
        waited += 250;
    }
    return NULL;
}

static void dump_chat_state(unsigned char *obj, HWND hwnd)
{
    RECT *wr = (RECT *)(obj + CHAT_OBJ_WINDOW_RECT);
    RECT *cr = (RECT *)(obj + CHAT_OBJ_CLIENT_RECT);
    char  title[128] = {0};
    char  cls[128]   = {0};
    LONG  style;

    GetWindowTextA(hwnd, title, sizeof(title) - 1);
    GetClassNameA(hwnd, cls, sizeof(cls) - 1);
    style = GetWindowLongA(hwnd, GWL_STYLE);

    mw_log("chat: hwnd %p  class \"%s\"  title \"%s\"  style 0x%08X", hwnd, cls, title, style);
    mw_log("chat: window rect %d,%d %dx%d   client %dx%d",
           wr->left, wr->top, wr->right - wr->left, wr->bottom - wr->top,
           cr->right - cr->left, cr->bottom - cr->top);
    mw_log("chat: memDC %p  windowDC %p  font %p  lines %d",
           *(HDC *)(obj + CHAT_OBJ_MEM_DC),
           *(HDC *)(obj + CHAT_OBJ_WND_DC),
           *(HGDIOBJ *)(obj + CHAT_OBJ_FONT),
           obj[CHAT_OBJ_LINE_COUNT] & 0x7F);

    /* Confirm the current WndProc is still the one we resolved -- if another
     * tool has already subclassed the window we need to know before Phase 3. */
    {
        void *live = (void *)GetWindowLongA(hwnd, GWL_WNDPROC);
        mw_log("chat: live WndProc %p  resolved %p%s", live, mw_addr[MW_WNDPROC],
               live == (void *)mw_addr[MW_WNDPROC] ? "" : "  (ALREADY SUBCLASSED)");
    }

    /* Log the font the client actually built, so Phase 2 has a baseline. */
    {
        HFONT   f = *(HFONT *)(obj + CHAT_OBJ_FONT);
        LOGFONTA lf;
        if (f && GetObjectA(f, sizeof(lf), &lf) == sizeof(lf)) {
            mw_log("font: \"%s\" h=%d w=%d weight=%d charset=%d pitch=0x%02X",
                   lf.lfFaceName, lf.lfHeight, lf.lfWidth, lf.lfWeight,
                   lf.lfCharSet, lf.lfPitchAndFamily);
        }
    }
}

/* Walk the global message array and report what channels are actually live.
 * This is the model Phase 5's tab filter depends on, so prove it early. */
static void dump_message_channels(unsigned char *msgs)
{
    int counts[16] = {0};
    int used = 0, shown = 0, i;

    for (i = 0; i < MSG_RECORD_COUNT; i++) {
        unsigned char *rec = msgs + (size_t)i * MSG_RECORD_STRIDE;
        unsigned int   sender  = *(unsigned int *)(rec + 0);
        unsigned int   channel = *(unsigned int *)(rec + 4);
        const char    *text    = (const char *)(rec + 8);

        if (sender == MSG_EMPTY_SENDER)
            continue;
        used++;
        if (channel < 16)
            counts[channel]++;
        if (shown < 8 && text[0]) {
            mw_log("msg[%4d] sender 0x%08X  ch %u (%-6s)  \"%.60s\"",
                   i, sender, channel, channel_name(channel), text);
            shown++;
        }
    }

    mw_log("msgs: %d/%d slots in use", used, MSG_RECORD_COUNT);
    for (i = 0; i < CHANNEL_COUNT; i++) {
        if (counts[i])
            mw_log("msgs:   ch %d %-6s = %d", i, CHANNEL_NAMES[i], counts[i]);
    }
}

static DWORD WINAPI worker(LPVOID unused)
{
    char  logpath[MAX_PATH];
    int   failed;
    HWND  hwnd;
    unsigned char *obj;

    (void)unused;

    mw_config_load();

    /* Logging is opt-in, and off is the default.
     *
     * A player who is not investigating anything has no use for a file per
     * client per launch, and Maps++ launches up to five clients at a time, so
     * the game root filled with them -- pruning kept the count down but the
     * player still never asked for any of it. [Debug] ModLogging is the one
     * key, shared with osmw_login.dll and flipped from Maps++'s
     * Tray -> Debug -> Client Mod Logging; the answer is taken once, here, so a
     * client keeps whatever it started with for its whole life.
     *
     * Off also prunes to nothing, so switching it back off clears out what the
     * troubleshooting session before it wrote. */
    if (mw_cfg_int_in("Debug", "ModLogging", 0)) {
        /* One log per client: Maps++ runs up to five at once and they would
         * otherwise truncate each other's file on startup. */
        _snprintf(logpath, sizeof(logpath), "%s\\osmw_chat.%lu.log",
                  mw_game_dir(), (unsigned long)GetCurrentProcessId());
        logpath[sizeof(logpath) - 1] = '\0';
        /* Keep the last five, which is one full Maps++ multi-client session, so
         * a crash can still be read against the other clients that were up at
         * the time. Older ones are just clutter in the game root. */
        mw_log_prune(mw_game_dir(), 5);
        mw_log_open(logpath);
    } else {
        mw_log_prune(mw_game_dir(), 0);
    }

    mw_log("osmw_chat starting");

    /* One gate, checked in one place. The launcher always injects; whether the
     * mod does anything is decided here, alongside the other Maps++ addons. */
    if (!mw_cfg_int_in("Addons", "ChatMod", 1)) {
        mw_log("[Addons] ChatMod=0 -- standing down, client untouched");
        return 0;
    }

    failed = mw_resolve_all();
    if (failed) {
        mw_log("resolve: %d required signature(s) failed -- standing down, client untouched", failed);
        return 0;
    }
    mw_log("resolve: all required signatures OK (%d scanned)", MW_SIG_COUNT);

    if (mw_timestamp != 0x45650696U) {
        mw_log("note: build 0x%08X is not the reference build 0x45650696; "
               "signatures still matched, so continuing", mw_timestamp);
    }

    obj = mw_addr[MW_OBJECT];
    mw_log("chat object %p, message array %p", obj, mw_addr[MW_MSGARRAY]);

    /* Install before waiting for the window. The chat window and its two fonts
     * are built during the client's startup init -- created hidden, then merely
     * shown on login -- so injecting into an already-running client is too late
     * for the font hook. inject_test.py --launch (and Maps++) inject at launch
     * for this reason. */
    mw_font_install();

    hwnd = wait_for_chat_window(obj, 0 /* wait indefinitely */);
    if (!hwnd) {
        mw_log("chat: window never appeared (unloading?)");
        return 0;
    }

    dump_chat_state(obj, hwnd);
    dump_message_channels(mw_addr[MW_MSGARRAY]);

    /* window.c owns the subclass; the features register handlers with it. */
    if (mw_window_install(hwnd)) {
        mw_geometry_install(hwnd);
        mw_clear_install(hwnd);
        mw_scroll_install();
        mw_cooldown_install();
        /* Before anything can trigger an atlas rebuild. */
        mw_chips_install();
        /* The mirror must be in place before the render hook starts syncing
         * it, and torn down after that hook is gone. */
        mw_mirror_install();
        /* After the mirror: the composer hook resolves records through it. */
        mw_time_install();
        mw_tabs_install(hwnd);
        /* After the mirror and tabs: a loaded list rebuilds the view, and
         * confirm-inject picks a channel the active tab keeps. */
        mw_block_install();
    }

    /* Deliberately returns rather than looping. A thread parked inside this
     * DLL is a thread whose code disappears when the DLL is unloaded, which
     * turns an eject into an access violation. Settings are re-read from the
     * WndProc instead, on the game's own thread. */
    mw_log("startup complete");
    return 0;
}

/* Called remotely by the injector before FreeLibrary. Stops the worker and
 * waits for it, so unloading cannot pull the code out from under a thread that
 * is still running in it. Exported undecorated via osmw_chat.def. */
__declspec(dllexport) DWORD WINAPI osmw_prepare_unload(LPVOID unused)
{
    (void)unused;
    InterlockedExchange(&g_stop, 1);
    if (g_worker) {
        WaitForSingleObject(g_worker, 5000);
        CloseHandle(g_worker);
        g_worker = NULL;
    }
    mw_log("prepare_unload: worker stopped, safe to FreeLibrary");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;

    switch (reason) {
    case DLL_PROCESS_ATTACH:
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        g_worker = CreateThread(NULL, 0, worker, NULL, 0, NULL);
        break;

    case DLL_PROCESS_DETACH:
        InterlockedExchange(&g_stop, 1);
        /* Unwind in reverse: features first, then the subclass that dispatches
         * to them, then the hooks that predate the window. */
        mw_block_remove();
        mw_tabs_remove();
        /* Before the mirror: the composer hook reads records through it. */
        mw_time_remove();
        mw_mirror_remove();
        mw_chips_remove();
        mw_cooldown_remove();
        mw_scroll_remove();
        mw_clear_remove();
        mw_geometry_remove();
        mw_window_remove();
        mw_font_remove();
        mw_log("osmw_chat unloading");
        mw_log_close();
        break;
    }
    return TRUE;
}
