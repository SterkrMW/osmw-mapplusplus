/* Phase 3 -- free the chat window's position and size.
 *
 * Two separate mechanisms hold the window in place, and fixing one without the
 * other gets you a window that moves but cannot be resized:
 *
 *  1. Snap-back. Clicking the title bar makes the client's WndProc set a flag;
 *     the next WM_NCHITTEST sees it, calls the reposition routine with
 *     force=1, and SetWindowPos's the window back against the game window's
 *     right edge. We stop this by not arming the flag (WM_NCLBUTTONDOWN) and
 *     by intercepting the reposition routine itself, which the show path also
 *     calls with force=1.
 *
 *  2. Size lock. WM_GETMINMAXINFO sets ptMinTrackSize.y == ptMaxTrackSize.y,
 *     so the border is draggable but inert. We widen the limits afterwards.
 *
 * The reposition routine is NOT simply disabled: it is also the legitimate
 * "owner window moved" follow, and it is where the cached owner/window/client
 * rects get refreshed. The render and hit-test paths read those, so the detour
 * still refreshes them truthfully -- it only skips the move.
 */
#include "osmw_chat.h"

#include <limits.h>
#include <stdio.h>

#define OBJ_HWND         0x04
#define OBJ_OWNER_RECT   0x08
#define OBJ_WINDOW_RECT  0x18
#define OBJ_CLIENT_RECT  0x28

/* The client blits this offscreen surface to the window, so the window cannot
 * usefully be larger than it -- past this you are looking at uninitialised
 * bitmap, not more chat. */
#define SURFACE_W 760
#define SURFACE_H 600

#define MIN_TRACK_W 120
#define MIN_TRACK_H 80

/* sub_4793F0 is __thiscall(this, int force). __fastcall matches it: ecx = this,
 * edx unused, the int on the stack, callee cleans with `ret 4`. */
typedef void (__fastcall *reposition_fn)(void *ecx, void *edx, int force);

static mw_hook_t     g_repos_hook;
static reposition_fn g_repos_orig;
static int           g_installed;
static int           g_restored;

/* Settings snapshot. The worker thread writes these; the game thread reads
 * them from the WndProc, where hitting the ini on every message would be
 * absurd. Plain aligned ints, so torn reads are not a concern. */
static volatile LONG g_free_position;
static volatile LONG g_unlock_size;
static volatile LONG g_remember;

void mw_settings_refresh(void)
{
    LONG freepos = mw_cfg_int("FreePosition", 0);
    LONG unlock  = mw_cfg_int("UnlockSize", 0);
    LONG remember = mw_cfg_int("RememberGeometry", 1);

    if (freepos != g_free_position || unlock != g_unlock_size ||
        remember != g_remember) {
        mw_log("settings: FreePosition=%ld UnlockSize=%ld RememberGeometry=%ld",
               freepos, unlock, remember);
    }
    g_free_position = freepos;
    g_unlock_size   = unlock;
    g_remember      = remember;
}

/* Keep the client's cached rects honest even when we skip the move. */
static void refresh_caches(unsigned char *obj, HWND hwnd)
{
    HWND owner = mw_addr[MW_MAINHWND] ? *(HWND *)mw_addr[MW_MAINHWND] : NULL;
    RECT *client = (RECT *)(obj + OBJ_CLIENT_RECT);
    int shift;

    if (owner && IsWindow(owner))
        GetWindowRect(owner, (RECT *)(obj + OBJ_OWNER_RECT));
    GetWindowRect(hwnd, (RECT *)(obj + OBJ_WINDOW_RECT));
    GetClientRect(hwnd, client);

    /* This runs inside the render, via the reposition call the render makes.
     * If the tab strip has reserved space at the top of the pane, a raw
     * GetClientRect silently throws that reservation away and the strip ends
     * up blitted off the top of the window. Put it back. */
    shift = mw_tabs_rect_shift();
    if (shift) {
        client->top += shift;
        client->bottom -= shift;
    }
}

static void save_geometry(HWND hwnd)
{
    RECT r;
    if (!GetWindowRect(hwnd, &r))
        return;
    mw_cfg_set_int("WinX", r.left);
    mw_cfg_set_int("WinY", r.top);
    mw_cfg_set_int("WinW", r.right - r.left);
    mw_cfg_set_int("WinH", r.bottom - r.top);
    mw_log("geometry: saved %d,%d %dx%d",
           r.left, r.top, r.right - r.left, r.bottom - r.top);
}

/* Returns 1 if a saved position was applied. */
static int restore_geometry(HWND hwnd)
{
    int x = mw_cfg_int("WinX", INT_MIN);
    int y = mw_cfg_int("WinY", INT_MIN);
    int w = mw_cfg_int("WinW", 0);
    int h = mw_cfg_int("WinH", 0);
    UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;

    if (x == INT_MIN || y == INT_MIN)
        return 0;

    if (w <= 0 || h <= 0) {
        RECT r;
        if (!GetWindowRect(hwnd, &r))
            return 0;
        w = r.right - r.left;
        h = r.bottom - r.top;
    }

    SetWindowPos(hwnd, NULL, x, y, w, h, flags);
    mw_log("geometry: restored %d,%d %dx%d", x, y, w, h);
    return 1;
}

static void __fastcall reposition_detour(void *self, void *edx, int force)
{
    unsigned char *obj = (unsigned char *)self;
    HWND hwnd;

    if (!g_free_position) {
        g_repos_orig(self, edx, force);
        return;
    }

    hwnd = *(HWND *)(obj + OBJ_HWND);
    if (!hwnd || !IsWindow(hwnd))
        return;

    /* The show path repositions with force=1; that is our cue to put the
     * window back where the player left it rather than back against the
     * game window. */
    if (g_remember && !g_restored) {
        g_restored = 1;
        restore_geometry(hwnd);
    }
    refresh_caches(obj, hwnd);
}

int mw_geometry_on_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT *result)
{
    switch (msg) {
    case WM_NCLBUTTONDOWN:
        /* The client's own handler for this message does exactly one thing --
         * arm the snap flag -- and then falls through to DefWindowProc. Going
         * straight to DefWindowProc gives us the drag without the snap. */
        if (g_free_position) {
            *result = DefWindowProcA(hwnd, msg, wp, lp);
            return 1;
        }
        break;

    case WM_GETMINMAXINFO:
        if (g_unlock_size) {
            MINMAXINFO *mmi = (MINMAXINFO *)lp;
            RECT bounds;

            /* Let the client fill the struct first, then widen the limits it
             * pinned shut. */
            *result = mw_window_call_original(hwnd, msg, wp, lp);

            bounds.left = 0;
            bounds.top = 0;
            bounds.right = SURFACE_W;
            bounds.bottom = SURFACE_H;
            AdjustWindowRect(&bounds, (DWORD)GetWindowLongA(hwnd, GWL_STYLE), FALSE);

            mmi->ptMinTrackSize.x = MIN_TRACK_W;
            mmi->ptMinTrackSize.y = MIN_TRACK_H;
            mmi->ptMaxTrackSize.x = bounds.right - bounds.left;
            mmi->ptMaxTrackSize.y = bounds.bottom - bounds.top;
            return 1;
        }
        break;

    case WM_EXITSIZEMOVE:
        if (g_remember && (g_free_position || g_unlock_size))
            save_geometry(hwnd);
        break;
    }

    return 0;
}

int mw_geometry_install(HWND hwnd)
{
    if (g_installed)
        return 1;
    if (!hwnd || !IsWindow(hwnd) || !mw_addr[MW_REPOSITION]) {
        mw_log("geometry: nothing to install against");
        return 0;
    }

    mw_settings_refresh();

    /* prologue is `mov eax, [abs32]` -- exactly 5 bytes, no relative branch */
    if (!mw_hook_install(&g_repos_hook, mw_addr[MW_REPOSITION],
                         (void *)(UINT_PTR)reposition_detour, 5)) {
        mw_log("geometry: could not hook the reposition routine");
        return 0;
    }
    g_repos_orig = (reposition_fn)g_repos_hook.trampoline;

    g_installed = 1;
    mw_log("geometry: installed");
    mw_log("geometry: max client size is %dx%d, the size of the client's own "
           "offscreen surface", SURFACE_W, SURFACE_H);

    /* Nudge the window so the new MINMAXINFO limits take effect immediately
     * rather than at the next user-initiated move. */
    if (g_unlock_size)
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                     SWP_FRAMECHANGED);
    return 1;
}

void mw_geometry_remove(void)
{
    if (!g_installed)
        return;
    mw_hook_remove(&g_repos_hook);
    g_installed = 0;
    mw_log("geometry: removed");
}
