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
 *
 * Placement, once free positioning is on, is a ONE-SHOT decision:
 *
 *     saved geometry?  -> put it back where the player left it
 *     nothing saved?   -> let the client dock it, exactly as it would unmodified
 *
 * and every later call skips the move, which is what free positioning is for.
 * The second branch is the one that was missing: with RememberGeometry off
 * there was nothing to restore and nothing docked it either, so the window sat
 * whereever CreateWindowExA had put it -- a fixed screen position that ignores
 * where the client actually is. On a second monitor, or on any client not at
 * the default spot, that looks exactly like the window going to the wrong
 * place, because it is.
 *
 * Saved geometry is stored RELATIVE to the game window (WinRelX/WinRelY), not
 * as absolute screen coordinates. Every client shares one config.ini, so an
 * absolute pair put every multi-boxed client's chat window in the same place,
 * on top of each other; an offset puts each one beside its own client and
 * survives the player moving that client. WinX/WinY are the legacy absolute
 * pair and are migrated on the first restore, then removed.
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

/* "Has this window been placed yet", not "has it been restored". The rename is
 * the fix: asking only about restoring is why the no-saved-geometry case had
 * no branch at all. Re-armed by mw_settings_refresh when the answer to "where
 * does this window belong" changes. */
static int           g_positioned;

/* Settings snapshot. The worker thread writes these; the game thread reads
 * them from the WndProc, where hitting the ini on every message would be
 * absurd. Plain aligned ints, so torn reads are not a concern. */
static volatile LONG g_free_position;
static volatile LONG g_unlock_size;
static volatile LONG g_remember;

static void forget_geometry(void);

void mw_settings_refresh(void)
{
    LONG freepos = mw_cfg_int("FreePosition", 0);
    LONG unlock  = mw_cfg_int("UnlockSize", 0);
    LONG remember = mw_cfg_int("RememberGeometry", 1);

    /* A one-shot command rather than a setting: Maps++ writes 1, we do the
     * work and write 0 back. The ini is the only channel there is, so a button
     * that has to make something happen has to look like this. */
    if (mw_cfg_int("ResetPosition", 0)) {
        mw_log("geometry: ResetPosition requested -- forgetting saved geometry");
        forget_geometry();
        mw_cfg_set_int("ResetPosition", 0);
        g_positioned = 0;        /* the next pass finds nothing and docks */
    }

    if (freepos != g_free_position || unlock != g_unlock_size ||
        remember != g_remember) {
        mw_log("settings: FreePosition=%ld UnlockSize=%ld RememberGeometry=%ld",
               freepos, unlock, remember);
    }

    /* Either of these changes the answer to "where does this window belong",
     * so let the next pass decide again instead of waiting for a restart.
     * UnlockSize is not in the test: it has nothing to do with position.
     * Note the deliberate consequence -- turning RememberGeometry OFF re-docks
     * the window, which is the honest reading of "stop remembering where I put
     * it". */
    if (g_installed &&
        (freepos != g_free_position || remember != g_remember))
        g_positioned = 0;

    g_free_position = freepos;
    g_unlock_size   = unlock;
    g_remember      = remember;
}

/* The game window this chat window belongs to. Everything positional is
 * expressed against it, so this is the one place that resolves it. */
static int owner_rect(RECT *out)
{
    HWND owner = mw_addr[MW_MAINHWND] ? *(HWND *)mw_addr[MW_MAINHWND] : NULL;

    if (!owner || !IsWindow(owner))
        return 0;
    return GetWindowRect(owner, out) ? 1 : 0;
}

/* Pull a restore target back onto a visible display.
 *
 * A saved offset is only as good as the client it was saved against: move that
 * client to a monitor that is later unplugged, or to one the chat window then
 * hangs off the edge of, and the restore would put the window somewhere the
 * player cannot reach it. MonitorFromRect rather than MonitorFromWindow --
 * the question is where the window is about to be, not where it is now. */
static void clamp_to_monitor(int *x, int *y, int w, int h)
{
    RECT target;
    HMONITOR mon;
    MONITORINFO mi;

    if (w <= 0 || h <= 0)
        return;

    target.left   = *x;
    target.top    = *y;
    target.right  = *x + w;
    target.bottom = *y + h;

    mon = MonitorFromRect(&target, MONITOR_DEFAULTTONEAREST);
    if (!mon)
        return;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoA(mon, &mi))
        return;

    /* Right and bottom first, then left and top, so a window larger than the
     * work area ends up at its top-left corner rather than off the far side. */
    if (*x + w > mi.rcWork.right)
        *x = mi.rcWork.right - w;
    if (*y + h > mi.rcWork.bottom)
        *y = mi.rcWork.bottom - h;
    if (*x < mi.rcWork.left)
        *x = mi.rcWork.left;
    if (*y < mi.rcWork.top)
        *y = mi.rcWork.top;
}

/* Drop every saved geometry key, including the legacy absolute pair. Clearing
 * an absent key is a no-op, so this is safe to call unconditionally. */
static void forget_geometry(void)
{
    mw_cfg_clear("WinRelX");
    mw_cfg_clear("WinRelY");
    mw_cfg_clear("WinX");
    mw_cfg_clear("WinY");
    mw_cfg_clear("WinW");
    mw_cfg_clear("WinH");
}

/* Keep the client's cached rects honest even when we skip the move. */
static void refresh_caches(unsigned char *obj, HWND hwnd)
{
    RECT *client = (RECT *)(obj + OBJ_CLIENT_RECT);
    RECT owner;
    int shift;

    if (owner_rect(&owner))
        *(RECT *)(obj + OBJ_OWNER_RECT) = owner;
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

/* Size is absolute -- it has no frame of reference to be relative to. Position
 * is an offset from the game window, so each client's chat window comes back
 * beside its own client rather than on top of every other client's. */
static void save_geometry(HWND hwnd)
{
    RECT r, owner;

    if (!GetWindowRect(hwnd, &r))
        return;

    mw_cfg_set_int("WinW", r.right - r.left);
    mw_cfg_set_int("WinH", r.bottom - r.top);

    if (owner_rect(&owner)) {
        mw_cfg_set_int("WinRelX", r.left - owner.left);
        mw_cfg_set_int("WinRelY", r.top - owner.top);
        /* The absolute pair is retired the first time we save over it, so an
         * upgraded install stops carrying both. */
        mw_cfg_clear("WinX");
        mw_cfg_clear("WinY");
        mw_log("geometry: saved %+d,%+d %dx%d (relative to the game window)",
               r.left - owner.left, r.top - owner.top,
               r.right - r.left, r.bottom - r.top);
        return;
    }

    /* No game window to measure against -- absolute is worse but it is better
     * than forgetting where the player put it. */
    mw_cfg_set_int("WinX", r.left);
    mw_cfg_set_int("WinY", r.top);
    mw_log("geometry: saved %d,%d %dx%d (absolute -- no game window)",
           r.left, r.top, r.right - r.left, r.bottom - r.top);
}

/* Returns 1 if a saved position was applied, 0 if there was nothing to apply
 * -- in which case the caller docks the window instead. */
static int restore_geometry(HWND hwnd)
{
    int relx = mw_cfg_int("WinRelX", INT_MIN);
    int rely = mw_cfg_int("WinRelY", INT_MIN);
    int w = mw_cfg_int("WinW", 0);
    int h = mw_cfg_int("WinH", 0);
    UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
    RECT owner;
    int have_owner = owner_rect(&owner);
    int x, y;

    if (relx != INT_MIN && rely != INT_MIN && have_owner) {
        x = owner.left + relx;
        y = owner.top + rely;
    } else {
        /* Legacy absolute pair, from before this was relative. Use it as-is
         * this once so nothing visibly moves on the first launch after an
         * update, convert it against the game window, and drop it. If there is
         * no game window yet we keep it and try the migration again next
         * launch rather than converting against nothing. */
        int ax = mw_cfg_int("WinX", INT_MIN);
        int ay = mw_cfg_int("WinY", INT_MIN);

        if (ax == INT_MIN || ay == INT_MIN)
            return 0;

        x = ax;
        y = ay;
        if (have_owner) {
            mw_cfg_set_int("WinRelX", ax - owner.left);
            mw_cfg_set_int("WinRelY", ay - owner.top);
            mw_cfg_clear("WinX");
            mw_cfg_clear("WinY");
            mw_log("geometry: migrated WinX/WinY to WinRelX/WinRelY (%+d,%+d)",
                   ax - owner.left, ay - owner.top);
        }
    }

    if (w <= 0 || h <= 0) {
        RECT r;
        if (!GetWindowRect(hwnd, &r))
            return 0;
        w = r.right - r.left;
        h = r.bottom - r.top;
    }

    clamp_to_monitor(&x, &y, w, h);
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

    /* One shot. Every later call is the client's own owner-moved follow or a
     * render-driven refresh, and turning those into a move is exactly what
     * free positioning exists to prevent.
     *
     * `force` is deliberately NOT used as the cue, despite being the obvious
     * candidate: it is set by the snap-back path as well as by the show path,
     * and re-docking on a snap-back is the behaviour FreePosition removes. The
     * two cannot be told apart from here, whereas "has this window been placed
     * yet" is answerable and is the question that actually matters. */
    if (!g_positioned) {
        g_positioned = 1;
        /* Nothing saved to go back to -- RememberGeometry is off, or it is on
         * and this is a first run. Let the client dock the window beside its
         * own window, as it would with the mod absent, rather than leaving it
         * at the fixed screen position CreateWindowExA gave it. */
        if (!(g_remember && restore_geometry(hwnd))) {
            mw_log("geometry: nothing saved -- docking against the game window");
            /* force=1, not the incoming value. Most calls that reach here are
             * the render's periodic refresh, which arrives with force=0 and
             * which the client's routine may treat as "nothing to do". We are
             * not refreshing, we are asking for a placement, so we ask the way
             * the show path asks. The latch above means this happens once, so
             * it cannot become the repeated snap-back that FreePosition
             * exists to suppress. */
            g_repos_orig(self, edx, 1);
        }
    }

    /* Unconditional, including on the dock path above: the original refreshes
     * these rects itself but knows nothing about mw_tabs_rect_shift(), so
     * without this the tab strip blits off the top of the window on exactly
     * the pass that docked it. */
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
                         (void *)(UINT_PTR)reposition_detour, 5,
                         "chat reposition prologue")) {
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

OSMW_RESULT mw_geometry_remove(void)
{
    OSMW_RESULT result = mw_hook_remove(&g_repos_hook);
    if (result != OSMW_OK)
        return result;
    g_installed = 0;
    mw_log("geometry: removed");
    return OSMW_OK;
}
