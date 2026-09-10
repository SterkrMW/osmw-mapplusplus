/* The client's collision grid, and walking a straight line across it.
 *
 * The client keeps its own collision data in memory, so nothing has to be
 * shipped alongside the mod. 0x461270 is its query:
 *
 *     int __cdecl CellWalkable(int gx, int gy)
 *     {
 *         unsigned short *cells = *(unsigned short **)0x6A6ADC;
 *         if (!cells)                    return -2;
 *         if (gx >= *(int *)0x6A6AD0)    return -2;
 *         if (gy >= *(int *)0x6A6AD4)    return -2;
 *         return (cells[gy * width + gx] & 0x8000) ? 0 : -2;
 *     }
 *
 * The bit test is reproduced here rather than called, because the walk needs to
 * read many cells in a row and this is one AND against a word. The globals come
 * from a signature taken across that same function, so they cannot drift apart
 * without the pattern failing first.
 *
 * **0x8000 set means BLOCKED, not walkable.** The return value alone reads the
 * other way -- in-bounds with the bit set returns 0 and everything else returns
 * -2, and -2 is this codebase's usual "not found" -- but the caller at 0x43A857
 * jumps away on a non-zero result and falls into a correction routine on zero,
 * which is the shape of "this position is bad". A dump of the live buffer
 * settled it: standing in open ground, the whole 5x5 neighbourhood reads
 * 0x0000, so clear ground is zero and the flag marks obstruction.
 *
 * Two things the client's version does not do, and we must:
 *
 *   - It never checks for negative coordinates, only `>=` against the
 *     dimensions with signed compares. A negative index would read behind the
 *     buffer. The client only ever passes values derived from unsigned pixels.
 *   - Cells are 16-BIT. The server's shipped cells/*.bin are one byte each with
 *     a different bit layout (0x80 blocked, 0x40 map-link); the two formats are
 *     unrelated and must not be mixed up.
 *
 * Sibling queries exist at 0x4611D0 and 0x461220 testing bits 0x2000 and
 * 0x1000 of the same word. What they mean is not known; one may be the map-link
 * flag the server's format carries at 0x40.
 */
#include "osmw_minimap.h"

#include <stdio.h>

/* The cell word's layers, established by diffing a live client grid against the
 * server's own cells/<map>.bin for the same map (map 5, 500x750):
 *
 *   0x8000  blocked      100.00% agreement with the server's 0x80. The two
 *                        datasets are identical on walkability, which also
 *                        retires the theory that they might disagree.
 *   0x4000  map link     covers 100.00% of the server's 695 link cells. The
 *                        client marks 957, a superset -- it flags the visual
 *                        transition area, the server the trigger cells.
 *   0x2000  pacing       15182 cells and zero overlap with links. An NPC placed
 *                        in this layer walks or paces.
 *   0x000F  link id      mirrors the server's low nibble: which link, not
 *                        whether one.
 *   0x1000  unknown      does not occur on this map at all.
 *
 * Note the map-link bit is NOT one of the two the sibling queries test
 * (0x4611D0 reads 0x2000, 0x461220 reads 0x1000), which is why reading those
 * routines did not turn it up.
 */
#define MM_CELL_BLOCKED_BIT 0x8000
#define MM_CELL_LINK_BIT    0x4000

static unsigned short *g_last_cells;
static int g_last_width;
static int g_last_height;

int mm_grid_dims(int *width, int *height)
{
    int w, h;

    if (!mm_addr[MM_GRIDWIDTH] || !mm_addr[MM_GRIDHEIGHT] || !mm_addr[MM_CELLBUFFER])
        return 0;
    if (!*(void **)mm_addr[MM_CELLBUFFER])
        return 0;   /* between maps */

    w = *(int *)mm_addr[MM_GRIDWIDTH];
    h = *(int *)mm_addr[MM_GRIDHEIGHT];
    if (w <= 0 || h <= 0)
        return 0;

    if (width)  *width = w;
    if (height) *height = h;
    return 1;
}

/* Whether a cell is a map transition. Walking onto one changes map, so a route
 * must never be planted on one by accident. */
int mm_cell_is_link(int gx, int gy)
{
    unsigned short *cells;
    int w, h;

    if (!mm_grid_dims(&w, &h))
        return 0;
    if (gx < 0 || gy < 0 || gx >= w || gy >= h)
        return 0;

    cells = *(unsigned short **)mm_addr[MM_CELLBUFFER];
    return (cells[(size_t)gy * w + gx] & MM_CELL_LINK_BIT) != 0;
}

int mm_cell_walkable(int gx, int gy)
{
    unsigned short *cells;
    int w, h;

    if (!mm_grid_dims(&w, &h))
        return 0;
    /* The negative half of the bounds check the client omits. */
    if (gx < 0 || gy < 0 || gx >= w || gy >= h)
        return 0;

    cells = *(unsigned short **)mm_addr[MM_CELLBUFFER];
    return (cells[(size_t)gy * w + gx] & MM_CELL_BLOCKED_BIT) == 0;
}

/* Walk the straight line between two cells, testing each one it crosses.
 *
 * A port of the server's walkGridLine (PositionManager/LineWalk.ts), kept
 * deliberately faithful so the client-side walk and the server's own position
 * recovery cannot disagree about which lines are walkable.
 *
 * **This has a diagonal corner rule and the server's version does not**, which
 * is a deliberate divergence rather than a porting slip.
 *
 * The server argues a diagonal step only occupies the cells at its two ends, so
 * it may squeeze between blocked diagonals. That holds at grid level, and it is
 * the right call there: the server is *modelling* how the client moves, and a
 * model that refuses a move the client would make desynchronises them.
 *
 * But we are *driving* the client, and it walks pixel by pixel. A diagonal from
 * one cell centre to the next physically passes through one of the two
 * orthogonal neighbours -- cells are 16x8, so the line genuinely crosses that
 * ground -- and if it holds a wall corner the character collides with geometry
 * the route claimed was clear. That is exactly what happened in testing: a
 * 7-leg route wedged at leg 5 on a wall corner beside open ground.
 *
 * Being stricter than the server is safe in a way that being looser is not. A
 * move we refuse simply routes around; a move we issue that the client cannot
 * physically make leaves the character stuck.
 *
 * The start cell is never tested -- it is wherever the player already stands,
 * which is never a reason to refuse to move.
 *
 * Returns 1 when the whole line is clear. On a blocked line it returns 0 and
 * leaves the last cell reached before the obstruction in *stop_x / *stop_y,
 * which is how far the player can actually get.
 */
int mm_walk_line(int sx, int sy, int ex, int ey, int *stop_x, int *stop_y)
{
    int w, h;
    int x = sx, y = sy;
    int dx, dy, step_x, step_y, error;

    if (stop_x) *stop_x = sx;
    if (stop_y) *stop_y = sy;

    if (!mm_grid_dims(&w, &h))
        return 0;
    if (sx < 0 || sy < 0 || sx >= w || sy >= h)
        return 0;
    if (ex < 0 || ey < 0 || ex >= w || ey >= h)
        return 0;

    dx = ex > x ? ex - x : x - ex;
    dy = ey > y ? ey - y : y - ey;
    step_x = x < ex ? 1 : -1;
    step_y = y < ey ? 1 : -1;
    error = dx - dy;

    while (x != ex || y != ey) {
        int e2 = error * 2;
        int mx = 0, my = 0;

        if (e2 > -dy) { error -= dy; mx = step_x; }
        if (e2 <  dx) { error += dx; my = step_y; }

        /* Corner rule: a diagonal must have both orthogonal neighbours clear,
         * because the pixel-level path between the two cell centres crosses
         * one of them. */
        if (mx && my &&
            (!mm_cell_walkable(x + mx, y) || !mm_cell_walkable(x, y + my)))
            return 0;

        x += mx;
        y += my;

        if (!mm_cell_walkable(x, y))
            return 0;

        if (stop_x) *stop_x = x;
        if (stop_y) *stop_y = y;
    }

    return 1;
}

/* One-shot summary of the whole collision grid.
 *
 * Confirms the buffer holds real map data rather than being empty or wrong: an
 * all-zero grid would make every cell look walkable and the walk would happily
 * route through walls. Also reports which bits actually occur, which is the
 * cheapest way to learn what the sibling queries at 0x4611D0 (0x2000) and
 * 0x461220 (0x1000) are for.
 */
void mm_grid_dump(int cx, int cy)
{
    unsigned short *cells;
    int w, h;
    size_t i, total, zero = 0, blocked = 0;
    unsigned short seen = 0;

    if (!mm_grid_dims(&w, &h)) {
        mm_log("grid: no map loaded");
        return;
    }
    cells = *(unsigned short **)mm_addr[MM_CELLBUFFER];
    total = (size_t)w * h;

    for (i = 0; i < total; i++) {
        unsigned short v = cells[i];
        seen |= v;
        if (!v) zero++;
        if (v & MM_CELL_BLOCKED_BIT) blocked++;
    }

    mm_log("grid: %dx%d cells (%u total); scales %d x %d px -> %d x %d px map",
           w, h, (unsigned)total,
           *(int *)mm_addr[MM_GRIDSCALEX], *(int *)mm_addr[MM_GRIDSCALEY],
           w * *(int *)mm_addr[MM_GRIDSCALEX], h * *(int *)mm_addr[MM_GRIDSCALEY]);
    mm_log("grid: %u zero, %u with 0x8000 (%u%%), bits ever set: %04X",
           (unsigned)zero, (unsigned)blocked,
           total ? (unsigned)(blocked * 100 / total) : 0, seen);
    mm_log("grid: player cell (%d,%d) = %04X, walkable=%d",
           cx, cy,
           (cx >= 0 && cy >= 0 && cx < w && cy < h) ? cells[(size_t)cy * w + cx] : 0xFFFF,
           mm_cell_walkable(cx, cy));
}

/* Write the live collision grid next to main.exe, for comparison with the
 * server's own cells/<map>.bin.
 *
 * These are two independently authored descriptions of the same map: the client
 * reads 16-bit cells with 0x8000 = blocked, the server ships one byte per cell
 * with 0x80 = blocked and 0x40 = map-link. The mod routes on the first and the
 * server validates against the second, so anywhere they disagree is a place a
 * perfectly good-looking route gets truncated or corrected -- which is what a
 * region of map that feels "sticky" to click in looks like from the inside.
 *
 * Raw 16-bit is written rather than a reduced blocked/clear byte, because which
 * of the other bits mean what is still unknown and throwing them away here
 * would mean another round trip to get them back.
 *
 * Opt-in via [Debug] DumpGrid, and named by dimensions since the client's own
 * map id has not been located.
 */
void mm_grid_write(void)
{
    unsigned short *cells;
    int w, h;
    char path[MAX_PATH];
    HANDLE f;
    DWORD written = 0;

    if (!mm_cfg_int_in("Debug", "DumpGrid", 0))
        return;
    if (!mm_grid_dims(&w, &h))
        return;

    cells = *(unsigned short **)mm_addr[MM_CELLBUFFER];

    _snprintf(path, sizeof(path), "%s\\clientgrid_%dx%d.bin", mm_game_dir(), w, h);
    path[sizeof(path) - 1] = 0;

    f = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        mm_log("grid: could not write %s (%lu)", path, GetLastError());
        return;
    }

    WriteFile(f, cells, (DWORD)((size_t)w * h * sizeof(unsigned short)), &written, NULL);
    CloseHandle(f);
    mm_log("grid: wrote %s (%lu bytes, %dx%d cells)", path, written, w, h);
}

/* Notice when the map has changed, and report the new grid.
 *
 * This used to run once, from the worker, after it first saw a map. That is
 * only ever the map the client started on: walking somewhere else never
 * re-triggered it, so asking for a dump of a particular map meant logging in
 * on it, which is not obvious from the outside and was not written down.
 *
 * A map change is visible as the cell buffer moving or the dimensions changing.
 * Two different maps can share dimensions, so the pointer is what carries it --
 * the client reallocates per map.
 *
 * Called from the per-frame tick, so it costs one pointer compare per frame in
 * the steady state.
 */
void mm_grid_poll(void)
{
    unsigned short *cells;
    int w, h, gx = 0, gy = 0;
    unsigned char *slot;

    if (!mm_grid_dims(&w, &h)) {
        /* Between maps. Forget the old grid so the next one reports, and drop
         * the search scratch: it is sized to a grid that no longer exists. */
        if (g_last_cells) {
            g_last_cells = NULL;
            g_last_width = g_last_height = 0;
            mm_path_release();
        }
        return;
    }

    cells = *(unsigned short **)mm_addr[MM_CELLBUFFER];
    if (cells == g_last_cells &&
            w == g_last_width && h == g_last_height)
        return;

    g_last_cells = cells;
    g_last_width = w;
    g_last_height = h;

    slot = mm_map_char_slot(mm_local_slot_index());
    if (slot)
        mm_pixel_to_grid(*(unsigned short *)(slot + MM_SLOT_X),
                         *(unsigned short *)(slot + MM_SLOT_Y), &gx, &gy);

    mm_grid_dump(gx, gy);
    mm_grid_write();
}

void mm_grid_reset(void)
{
    g_last_cells = NULL;
    g_last_width = 0;
    g_last_height = 0;
}
