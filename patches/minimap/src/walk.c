/* Walking to a selected NPC.
 *
 * MW1's client has no pathfinder, but it does walk a straight line to wherever
 * it is told. So a route is issued as a series of straight legs: one
 * SendPlayerMove per leg, and the client covers each leg itself. A destination
 * in line of sight is therefore one packet and no queue at all, which is the
 * common case in a town.
 *
 * When the line is blocked, A* over the client's own collision grid produces a
 * cell path, which is then collapsed to the fewest straight legs that cover it
 * (mm_simplify_path). Each leg is a straight move the server validates exactly
 * as it would a manual click.
 *
 * The queue is advanced once per frame from core's post-UiTick callback, so a
 * route keeps moving even after the minimap closes without another prologue
 * detour or timer thread.
 *
 * SendPlayerMove (0x43A740) is __cdecl with four arguments, confirmed rather
 * than assumed: it ends in a plain `ret`, the caller at 0x43AA60 cleans with
 * `add esp, 0x10`, and it reads x and y as 16-bit words.
 */
#include "osmw_minimap.h"

typedef void(__cdecl *mm_sendmove_fn)(int slot_index, unsigned short x,
                                      unsigned short y, unsigned char run);

/* 0x43A740 is SendPlayerMove, a unique 7-byte prologue hooked so a move the
 * player makes cancels
 * the route. Anything else -- watching for position jumps, or for clicks -- is
 * guesswork about intent; a move packet the mod did not send is definitionally
 * the player steering, so this is exact rather than heuristic.
 *
 * Our own sends go through the trampoline rather than the patched address, so
 * the detour never sees them and no "is this ours" flag is needed. Core grants
 * the code claim before this file is allowed to install the detour. */
static mm_hook_t      g_send_hook;
static mm_sendmove_fn g_orig_send;
static int            g_walk_enabled;

/* The server rate-limits movement to one per 50 ms, burst 30/s
 * (ActionRateLimiter). Legs are far rarer than that, but the guard is cheap. */
#define MM_MOVE_MIN_INTERVAL_MS 50

/* Server-side party gating uses minWaypointDistanceSq = 48^2; the same radius
 * decides when a leg is close enough to count as reached. */
#define MM_ARRIVE_PX      48
#define MM_ARRIVE_PX_SQ   (MM_ARRIVE_PX * MM_ARRIVE_PX)

/* If the character has not moved this long while a leg is outstanding, it is
 * wedged on geometry the grid disagrees with. Stop rather than sit there. */
#define MM_STALL_MS       2000

#define MM_MAX_CELLS      4096
#define MM_MAX_LEGS       64

static mm_cell_t g_cells[MM_MAX_CELLS];
static mm_cell_t g_legs[MM_MAX_LEGS];

static int          g_leg_count;
static int          g_leg_index;
static int          g_active;
static unsigned int g_last_move;
static unsigned int g_last_progress;
static int          g_last_px, g_last_py;

/* The final destination, kept so a stalled walk can re-plan from wherever the
 * character actually ended up rather than abandoning the trip. */
static int          g_dest_px, g_dest_py;
static int          g_repaths;
static unsigned int g_route_started;
/* Where the character was when the route began, so a stall can tell "wedged
 * immediately" from "wedged after covering ground". */
static int          g_origin_px, g_origin_py;

/* One recovery attempt, then stop for good.
 *
 * This used to be two, and the retry loop turned a stuck route into a real
 * problem: getting wedged on a corner is a minor annoyance, but a route that
 * keeps re-planning and re-issuing moves competes with the player's own clicks
 * for control of the character. Losing the ability to steer is far worse than
 * failing to arrive.
 *
 * So the loop is deliberately weak. One attempt covers the common case where a
 * character clips a corner and needs a route from where it actually stopped;
 * beyond that the route gives up and hands the character back. Clicking the NPC
 * again is a deliberate retry, which is the right way to ask for another go.
 */
#define MM_MAX_REPATHS 1

/* A whole route also gets a deadline. Nothing legitimate takes this long, and
 * without it a pathological map could keep a route nominally alive -- and
 * therefore still issuing moves -- indefinitely. */
#define MM_ROUTE_DEADLINE_MS 45000

static void send_move(int px, int py)
{
    int slot = mm_local_slot_index();

    /* Always use the trampoline so our own moves do not read as the player
     * steering. No claim/hook means auto-walk is disabled, never a raw call. */
    if (!g_walk_enabled || !g_orig_send || slot < 0)
        return;

    g_orig_send(slot, (unsigned short)px, (unsigned short)py, 1 /* run */);
    g_last_move = GetTickCount();
}

/* Any move we did not send is the player steering, so the route is abandoned.
 * Clicking to walk somewhere while a route is running should mean what it says.
 */
/* Set when the panel is torn down, to absorb the click it was holding.
 *
 * Clearing the event byte on teardown is not sufficient on its own. The client
 * re-asserts 0xF5 on a later frame -- the same behaviour that made a held button
 * arrive as a click per frame -- so on the close paths that run synchronously
 * through the destroy hook (Tab and the minimap button) the re-assertion lands
 * after the panel has gone, finds nothing absorbing it, and is dispatched to the
 * world at the cursor position. That is a row of the list, so the character is
 * sent there and the route it had just been given is cancelled.
 *
 * Right-clicking to close does not show this: it frees the panel a frame later
 * via the idle reaper, by which point the click has already been consumed.
 *
 * The move is FORWARDED, and only the cancellation is suppressed.
 *
 * An earlier version dropped the packet entirely. That was worse: by the time
 * SendPlayerMove is reached the client has already begun moving locally, so
 * withholding the packet leaves the server believing the character is somewhere
 * it is not. The server corrects that, and the correction is what made the
 * character visibly jitter. Telling the server the truth and simply declining to
 * treat the move as the player taking over costs one small step towards the
 * phantom destination before the next leg redirects -- which is a wobble rather
 * than a fight between two authorities.
 *
 * Never suppress a packet for a movement the client has already made.
 */
#define MM_SWALLOW_MS 300

static unsigned int g_swallow_until;

void mm_walk_keep_route_next_move(void)
{
    g_swallow_until = GetTickCount() + MM_SWALLOW_MS;
}

static void __cdecl sendmove_detour(int slot, unsigned short x,
                                    unsigned short y, unsigned char run)
{
    if (g_swallow_until) {
        unsigned int now = GetTickCount();

        if ((int)(g_swallow_until - now) > 0) {
            g_swallow_until = 0;
            mm_log("walk: click the panel was holding; forwarded, route kept");
            g_orig_send(slot, x, y, run);
            return;
        }
        g_swallow_until = 0;
    }

    if (g_active)
        mm_walk_stop("player moved");
    g_orig_send(slot, x, y, run);
}

int mm_walk_install(int enabled)
{
    g_walk_enabled = enabled != 0;
    if (!g_walk_enabled) {
        mm_log("walk: [Minimap] AutoWalk=0 -- walking disabled, panel unaffected");
        return 1;
    }

    if (!mm_addr[MM_SENDPLAYERMOVE] ||
            !mm_hook_install(&g_send_hook, mm_addr[MM_SENDPLAYERMOVE],
                             (void *)sendmove_detour,
                             MM_SENDPLAYERMOVE_PROLOGUE_LENGTH)) {
        mm_log("walk: could not hook claimed SendPlayerMove range");
        g_walk_enabled = 0;
        return 0;
    }
    g_orig_send = (mm_sendmove_fn)g_send_hook.trampoline;

    return 1;
}

void mm_walk_remove(void)
{
    mm_walk_stop("unloading");
    if (g_send_hook.active) {
        mm_hook_remove(&g_send_hook);
        g_orig_send = NULL;
    }
    g_walk_enabled = 0;
    g_swallow_until = 0;
    g_last_move = 0;
    g_last_progress = 0;
    g_route_started = 0;
}

static int local_pixel(int *px, int *py)
{
    unsigned char *slot = mm_map_char_slot(mm_local_slot_index());

    if (!slot)
        return 0;
    *px = *(unsigned short *)(slot + MM_SLOT_X);
    *py = *(unsigned short *)(slot + MM_SLOT_Y);
    return 1;
}

void mm_walk_stop(const char *why)
{
    if (g_active)
        mm_log("walk: stopped (%s) at leg %d of %d", why, g_leg_index, g_leg_count);
    g_active = 0;
    g_leg_count = 0;
    g_leg_index = 0;
}

int mm_walk_active(void)
{
    return g_active;
}

/* Send the current leg, aimed at the centre of its cell. */
static void issue_leg(void)
{
    int sx = *(int *)mm_addr[MM_GRIDSCALEX];
    int sy = *(int *)mm_addr[MM_GRIDSCALEY];

    send_move(g_legs[g_leg_index].x * sx + sx / 2,
              g_legs[g_leg_index].y * sy + sy / 2);
}

static int start_walk(int dest_px, int dest_py)
{
    int w, h, sx, sy, ex, ey, stop_x, stop_y, cells, legs;

    g_dest_px = dest_px;
    g_dest_py = dest_py;

    if (!mm_grid_dims(&w, &h))
        return MM_WALK_NO_MAP;
    if (GetTickCount() - g_last_move < MM_MOVE_MIN_INTERVAL_MS)
        return MM_WALK_THROTTLED;

    {
        int px, py;
        if (!local_pixel(&px, &py))
            return MM_WALK_NO_MAP;
        if (!mm_pixel_to_grid(px, py, &sx, &sy))
            return MM_WALK_NO_MAP;
    }
    if (!mm_pixel_to_grid(dest_px, dest_py, &ex, &ey))
        return MM_WALK_NO_MAP;

    if (sx == ex && sy == ey)
        return MM_WALK_ARRIVED;

    /* Straight line first: one packet, and the client does the walking. Aimed
     * at the NPC's own position so the player ends up where the marker is. */
    if (mm_walk_line(sx, sy, ex, ey, &stop_x, &stop_y)) {
        send_move(dest_px, dest_py);
        mm_log("walk: clear line to (%d,%d) grid (%d,%d)", dest_px, dest_py, ex, ey);
        return MM_WALK_CLEAR;
    }

    /* Blocked: route around it. */
    cells = mm_find_path(sx, sy, ex, ey, g_cells, MM_MAX_CELLS);
    if (cells > 0) {
        legs = mm_simplify_path(g_cells, cells, g_legs, MM_MAX_LEGS);
        if (legs > 0) {
            g_leg_count = legs;
            g_leg_index = 0;
            g_active = 1;
            g_last_progress = GetTickCount();
            local_pixel(&g_last_px, &g_last_py);
            issue_leg();
            mm_log("walk: routed around, %d cells collapsed to %d legs", cells, legs);
            return MM_WALK_ROUTED;
        }
    }

    /* No route. Walk the line as far as it goes and stop, which is what the
     * client does when a destination sits past an obstruction. */
    if (stop_x == sx && stop_y == sy) {
        mm_log("walk: blocked at the first step from grid (%d,%d), no route", sx, sy);
        return MM_WALK_BLOCKED;
    }

    {
        int scale_x = *(int *)mm_addr[MM_GRIDSCALEX];
        int scale_y = *(int *)mm_addr[MM_GRIDSCALEY];

        send_move(stop_x * scale_x + scale_x / 2, stop_y * scale_y + scale_y / 2);
        mm_log("walk: no route, stopping at grid (%d,%d) of (%d,%d)",
               stop_x, stop_y, ex, ey);
    }
    return MM_WALK_TRUNCATED;
}

int mm_walk_to_pixel(int dest_px, int dest_py)
{
    if (!g_walk_enabled || !g_orig_send)
        return MM_WALK_DISABLED;
    mm_walk_stop("new destination");
    g_repaths = 0;
    g_route_started = GetTickCount();
    if (!local_pixel(&g_origin_px, &g_origin_py)) {
        g_origin_px = 0;
        g_origin_py = 0;
    }
    return start_walk(dest_px, dest_py);
}

void mm_walk_tick(void)
{
    int px, py, tx, ty, dx, dy, sx, sy;
    unsigned int now;

    if (!g_active)
        return;

    if (GetTickCount() - g_route_started > MM_ROUTE_DEADLINE_MS) {
        mm_walk_stop("took too long");
        return;
    }

    /* The map going away underneath us takes the route with it: cell
     * coordinates from the old map mean nothing on the new one. */
    if (!mm_grid_dims(&sx, &sy)) {
        mm_walk_stop("map changed");
        return;
    }
    if (!local_pixel(&px, &py)) {
        mm_walk_stop("lost the local character");
        return;
    }

    sx = *(int *)mm_addr[MM_GRIDSCALEX];
    sy = *(int *)mm_addr[MM_GRIDSCALEY];
    tx = g_legs[g_leg_index].x * sx + sx / 2;
    ty = g_legs[g_leg_index].y * sy + sy / 2;

    dx = px - tx;
    dy = py - ty;

    if (dx * dx + dy * dy <= MM_ARRIVE_PX_SQ) {
        g_leg_index++;
        if (g_leg_index >= g_leg_count) {
            mm_log("walk: arrived");
            g_active = 0;
            g_leg_count = 0;
            return;
        }
        now = GetTickCount();
        g_last_progress = now;
        if (now - g_last_move >= MM_MOVE_MIN_INTERVAL_MS)
            issue_leg();
        return;
    }

    /* Progress watchdog. The grid and the client's own collision can disagree
     * on a corner, and a character wedged against one would otherwise sit there
     * with a leg outstanding forever. */
    now = GetTickCount();
    if (px != g_last_px || py != g_last_py) {
        g_last_px = px;
        g_last_py = py;
        g_last_progress = now;
        return;
    }

    if (now - g_last_progress > MM_STALL_MS) {
        /* Wedged. The grid and the client's own collision can still disagree on
         * a corner even with the corner rule, so re-plan from where the
         * character actually is before giving up -- the new route starts from
         * the obstruction rather than from where the trip began. */
        {
            /* A route that stalls without having covered any ground is wedged
             * at the start, and re-planning from the same place produces the
             * same route. Only a route that got somewhere earns a retry. */
            int moved_x = px - g_origin_px, moved_y = py - g_origin_py;
            int moved = moved_x * moved_x + moved_y * moved_y;

            if (moved <= MM_ARRIVE_PX_SQ && g_repaths > 0) {
                mm_walk_stop("wedged without making progress");
                return;
            }
        }

        if (g_repaths < MM_MAX_REPATHS) {
            int dest_px = g_dest_px, dest_py = g_dest_py;
            int cgx = 0, cgy = 0, tgx = 0, tgy = 0;

            mm_pixel_to_grid(px, py, &cgx, &cgy);
            mm_pixel_to_grid(tx, ty, &tgx, &tgy);

            g_repaths++;
            mm_log("walk: stalled at leg %d of %d, re-planning (attempt %d)",
                   g_leg_index, g_leg_count, g_repaths);
            /* The three facts that separate the candidate causes: standing on a
             * cell the grid calls blocked, a leg target that is itself blocked,
             * or simply not having got there yet. */
            mm_log("walk:   at px (%d,%d) cell (%d,%d) walkable=%d; "
                   "target px (%d,%d) cell (%d,%d) walkable=%d; %d px away",
                   px, py, cgx, cgy, mm_cell_walkable(cgx, cgy),
                   tx, ty, tgx, tgy, mm_cell_walkable(tgx, tgy),
                   (int)(dx * dx + dy * dy));
            g_active = 0;
            g_leg_count = 0;
            g_leg_index = 0;
            start_walk(dest_px, dest_py);
            return;
        }
        mm_walk_stop("no progress; click the NPC again to retry");
    }
}
