/* Owns the chat window subclass.
 *
 * Exactly one place calls SetWindowLongA(GWL_WNDPROC); each feature exposes a
 * message handler and this file dispatches to them in order. Features must not
 * subclass the window themselves -- competing subclasses are how you end up
 * unable to unwind them in the right order on eject.
 *
 * Everything here runs on the game's own thread.
 */
#include "osmw_chat.h"

static WNDPROC g_orig_proc;
static HWND    g_hwnd;
static int     g_installed;

/* Re-read the ini at most every couple of seconds, on the game thread, so
 * edits from the settings file take effect without a restart. Doing it here
 * rather than from a background thread means no thread is ever parked inside
 * this DLL waiting to be unloaded -- see NOTES.txt. */
#define SETTINGS_POLL_MS 2000

static void poll_settings(void)
{
    static DWORD last;
    DWORD now = GetTickCount();

    if (last && (now - last) < SETTINGS_POLL_MS)
        return;
    last = now;
    mw_settings_refresh();
    mw_clear_sync_hotkey(g_hwnd);
    mw_tabs_sync_hotkey(g_hwnd);
    /* Runs here rather than from a worker so the font swap lands between
     * frames, on the thread that owns the device contexts. */
    mw_font_poll();
    /* The caption only carries the character name once you are in the world,
     * so this keeps looking rather than giving up at startup. */
    mw_mention_refresh();
    /* After mention_refresh: adding your own name is refused, so the caption
     * parse should have had a chance to run. */
    mw_block_poll();
    mw_scroll_refresh();
    mw_cooldown_refresh();
    mw_links_poll();
    mw_backgrounds_poll();
    /* After mention_refresh: a change here rebuilds the ring, and the rebuild
     * re-applies both decorations, so the mention settings should be current
     * before it runs. */
    mw_time_poll();
}

/* Also called from the render, which ticks about ten times a second whenever
 * the pane is up. The window procedure alone was not enough: an idle or hidden
 * chat window receives almost nothing, so a settings edit only took effect
 * when you happened to interact with it. The throttle makes the second caller
 * cost nothing. */
void mw_window_poll_settings(void)
{
    poll_settings();
}

LRESULT mw_window_call_original(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (!g_orig_proc)
        return DefWindowProcA(hwnd, msg, wp, lp);
    return CallWindowProcA(g_orig_proc, hwnd, msg, wp, lp);
}

HWND mw_window_hwnd(void)
{
    return g_hwnd;
}

static LRESULT CALLBACK chat_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    LRESULT result = 0;

    poll_settings();

    if (mw_scroll_on_message(hwnd, msg, wp, lp, &result))
        return result;
    if (mw_clear_on_message(hwnd, msg, wp, lp, &result))
        return result;
    if (mw_tabs_on_message(hwnd, msg, wp, lp, &result))
        return result;
    /* After tabs: a click on the strip is the strip's, not a link's. */
    if (mw_links_on_message(hwnd, msg, wp, lp, &result))
        return result;
    if (mw_backgrounds_on_message(hwnd, msg, wp, lp, &result))
        return result;
    if (mw_geometry_on_message(hwnd, msg, wp, lp, &result))
        return result;

    return mw_window_call_original(hwnd, msg, wp, lp);
}

int mw_window_install(HWND hwnd)
{
    WNDPROC live;

    if (g_installed)
        return 1;
    if (!hwnd || !IsWindow(hwnd)) {
        mw_log("window: no chat window to subclass");
        return 0;
    }

    live = (WNDPROC)(UINT_PTR)GetWindowLongA(hwnd, GWL_WNDPROC);
    if (live != (WNDPROC)(UINT_PTR)mw_addr[MW_WNDPROC]) {
        mw_log("window: current WndProc %p is not resolved stock %p; "
               "refusing competing subclass", live, mw_addr[MW_WNDPROC]);
        return 0;
    }

    g_orig_proc = (WNDPROC)(UINT_PTR)SetWindowLongA(hwnd, GWL_WNDPROC,
                                                    (LONG)(UINT_PTR)chat_proc);
    if (!g_orig_proc) {
        mw_log("window: SetWindowLongA(GWL_WNDPROC) failed: %lu", GetLastError());
        return 0;
    }

    g_hwnd = hwnd;
    g_installed = 1;
    mw_log("window: subclassed %p (original proc %p)", hwnd, g_orig_proc);
    return 1;
}

OSMW_RESULT mw_window_remove(void)
{
    WNDPROC live;

    if (!g_installed)
        return OSMW_OK;
    if (g_hwnd && IsWindow(g_hwnd) && g_orig_proc) {
        live = (WNDPROC)(UINT_PTR)GetWindowLongA(g_hwnd, GWL_WNDPROC);
        if (live != chat_proc) {
            mw_log("window: live WndProc %p no longer belongs to chat; "
                   "refusing unsafe restore", live);
            return OSMW_E_BUSY;
        }
        SetLastError(ERROR_SUCCESS);
        if (SetWindowLongA(g_hwnd, GWL_WNDPROC,
                           (LONG)(UINT_PTR)g_orig_proc) == 0 &&
                GetLastError() != ERROR_SUCCESS) {
            mw_log("window: WndProc restore failed: %lu", GetLastError());
            return OSMW_E_INTERNAL;
        }
    }
    g_installed = 0;
    g_hwnd = NULL;
    g_orig_proc = NULL;
    mw_log("window: subclass removed");
    return OSMW_OK;
}
