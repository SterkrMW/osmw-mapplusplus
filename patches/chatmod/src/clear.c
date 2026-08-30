/* Phase 4 -- clearing the chat pane.
 *
 * The obvious target is wrong. chatobj+0x484 looks like the chat window's line
 * store, and the client's own show routine does zero it (0x1E79 dwords from
 * chatobj+0x80). But those records hold *slot indices* into the global message
 * array, and the window rebuilds that view from the array every render pass.
 * Measured: zeroing the view left the pane unchanged and the records were
 * identical again 1.5 seconds later.
 *
 * So we clear the source. The client already has a routine that resets the
 * message arrays and their counters -- it is what runs when you enter the
 * world -- and calling it is more robust than reproducing its loop. The one
 * thing it also does that we do not want is reset the visible-line count
 * (ChatMiniSize, the Maps++ chatToggleSize setting), so that is saved and put
 * back around the call.
 *
 * Finally the render only repaints fully when a dirty flag is set; it clears
 * that flag at the end of each pass. Without setting it, the pixels already on
 * the offscreen surface stay put and the clear is invisible.
 *
 * This clears the message source, so the in-game overlay empties too, not just
 * the chat window.
 *
 * The clear must run on the game thread or it races the render loop, so it is
 * driven entirely from the window subclass: a hotkey delivered as WM_HOTKEY to
 * the chat window, or a registered window message any external tool can post.
 */
#include "osmw_chat.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OBJ_LINE_STORE   0x80
#define OBJ_STORE_DWORDS 0x1E79     /* what the client's own clear zeroes */

/* The four counters sit immediately below the message array, written together
 * by the client's reset routine. base-0x08 is the visible-line count. */
#define COUNTER_VISIBLE_LINES (-0x08)

#define CLEAR_HOTKEY_ID  0x0C1E

/* The reset routine takes no arguments and cleans up after itself. */
typedef void (__cdecl *reset_buffers_fn)(void);

/* Any tool that wants to clear the pane can
 *     PostMessageA(chatHwnd, RegisterWindowMessageA("osmw_chat_clear"), 0, 0);
 * which is how a future Maps++ integration will drive this. */
#define CLEAR_MESSAGE_NAME "osmw_chat_clear"

static UINT g_clear_msg;
static int  g_hotkey_registered;
static int  g_installed;
static char g_hotkey_spec[64];

/* "CTRL+SHIFT+C", "ALT+F1", "CTRL+DELETE" ... empty string = unbound.
 * Shared with tabs.c, which binds its own hotkey the same way. */
int mw_parse_hotkey(const char *spec, UINT *mods, UINT *vk)
{
    char  buf[64];
    char *tok, *next;

    *mods = 0;
    *vk = 0;
    if (!spec || !spec[0])
        return 0;

    _snprintf(buf, sizeof(buf), "%s", spec);
    buf[sizeof(buf) - 1] = '\0';
    _strupr(buf);

    for (tok = buf; tok; tok = next) {
        next = strchr(tok, '+');
        if (next)
            *next++ = '\0';
        while (*tok == ' ')
            tok++;

        if (!strcmp(tok, "CTRL") || !strcmp(tok, "CONTROL")) {
            *mods |= MOD_CONTROL;
        } else if (!strcmp(tok, "ALT")) {
            *mods |= MOD_ALT;
        } else if (!strcmp(tok, "SHIFT")) {
            *mods |= MOD_SHIFT;
        } else if (!strcmp(tok, "WIN")) {
            *mods |= MOD_WIN;
        } else if (!strcmp(tok, "DELETE") || !strcmp(tok, "DEL")) {
            *vk = VK_DELETE;
        } else if (!strcmp(tok, "HOME")) {
            *vk = VK_HOME;
        } else if (!strcmp(tok, "END")) {
            *vk = VK_END;
        } else if (!strcmp(tok, "BACKSPACE") || !strcmp(tok, "BACK")) {
            *vk = VK_BACK;
        } else if (tok[0] == 'F' && tok[1] >= '1' && tok[1] <= '9') {
            int n = atoi(tok + 1);
            if (n >= 1 && n <= 24)
                *vk = (UINT)(VK_F1 + n - 1);
        } else if (strlen(tok) == 1 &&
                   ((tok[0] >= 'A' && tok[0] <= 'Z') ||
                    (tok[0] >= '0' && tok[0] <= '9'))) {
            *vk = (UINT)tok[0];
        }
    }

    return *vk != 0;
}

void mw_clear_now(const char *why)
{
    unsigned char *obj   = mw_addr[MW_OBJECT];
    unsigned char *msgs  = mw_addr[MW_MSGARRAY];
    unsigned char *reset = mw_addr[MW_RESETBUFFERS];
    unsigned char *dirty = mw_addr[MW_DIRTYFLAG];
    HWND  hwnd = mw_window_hwnd();
    DWORD saved_lines = 0;
    int   have_saved = 0;

    if (!reset || !msgs) {
        mw_log("clear: reset routine or message array unresolved, ignoring");
        return;
    }

    /* Preserve the player's visible-line count across the client's reset,
     * which would otherwise force it back to its startup value of 7. */
    saved_lines = *(DWORD *)(msgs + COUNTER_VISIBLE_LINES);
    have_saved = 1;

    ((reset_buffers_fn)reset)();

    if (have_saved)
        *(DWORD *)(msgs + COUNTER_VISIBLE_LINES) = saved_lines;

    /* Drop the window's view of the now-empty array as well, so nothing is
     * left pointing at slots that no longer hold anything. */
    if (obj)
        memset(obj + OBJ_LINE_STORE, 0, (size_t)OBJ_STORE_DWORDS * 4);

    /* The window reads a filtered private copy of the ring, so that has to be
     * rebuilt from the now-empty original too. */
    mw_mirror_rebuild(mw_tabs_mask());

    /* Force one full repaint. Without this the surface keeps the pixels it
     * already has and the clear is invisible. */
    if (dirty)
        *(DWORD *)dirty = 1;
    if (hwnd && IsWindow(hwnd))
        InvalidateRect(hwnd, NULL, FALSE);

    mw_log("clear: chat cleared (%s), visible-line count kept at %lu",
           why, saved_lines);
}

int mw_clear_on_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT *result)
{
    (void)lp;

    if (g_clear_msg && msg == g_clear_msg) {
        mw_clear_now("posted message");
        *result = 1;
        return 1;
    }

    if (msg == WM_HOTKEY && wp == CLEAR_HOTKEY_ID) {
        mw_clear_now("hotkey");
        *result = 0;
        return 1;
    }

    (void)hwnd;
    return 0;
}

/* Called on the settings poll so the hotkey can be rebound without a restart. */
void mw_clear_sync_hotkey(HWND hwnd)
{
    const char *spec = mw_cfg_str("ClearHotkey", "");
    UINT mods = 0, vk = 0;

    if (!hwnd || !IsWindow(hwnd))
        return;
    if (!strcmp(spec, g_hotkey_spec))
        return;                     /* unchanged */

    _snprintf(g_hotkey_spec, sizeof(g_hotkey_spec), "%s", spec);
    g_hotkey_spec[sizeof(g_hotkey_spec) - 1] = '\0';

    if (g_hotkey_registered) {
        UnregisterHotKey(hwnd, CLEAR_HOTKEY_ID);
        g_hotkey_registered = 0;
    }
    if (!mw_parse_hotkey(spec, &mods, &vk)) {
        if (spec[0])
            mw_log("clear: could not parse ClearHotkey \"%s\" -- unbound", spec);
        return;
    }

    if (RegisterHotKey(hwnd, CLEAR_HOTKEY_ID, mods, vk)) {
        g_hotkey_registered = 1;
        mw_log("clear: hotkey \"%s\" registered", spec);
    } else {
        mw_log("clear: RegisterHotKey(\"%s\") failed: %lu -- another app may "
               "already own that combination", spec, GetLastError());
    }
}

int mw_clear_install(HWND hwnd)
{
    if (g_installed)
        return 1;

    g_clear_msg = RegisterWindowMessageA(CLEAR_MESSAGE_NAME);
    if (!g_clear_msg)
        mw_log("clear: RegisterWindowMessage failed: %lu", GetLastError());

    g_hotkey_spec[0] = '\0';
    mw_clear_sync_hotkey(hwnd);

    g_installed = 1;
    mw_log("clear: installed (post message 0x%04X \"%s\" to clear externally)",
           g_clear_msg, CLEAR_MESSAGE_NAME);
    return 1;
}

void mw_clear_remove(void)
{
    HWND hwnd = mw_window_hwnd();

    if (!g_installed)
        return;
    if (g_hotkey_registered && hwnd && IsWindow(hwnd))
        UnregisterHotKey(hwnd, CLEAR_HOTKEY_ID);
    g_hotkey_registered = 0;
    g_installed = 0;
    mw_log("clear: removed");
}
