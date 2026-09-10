/* A* over the client's collision grid.
 *
 * A port of the server's PathFinder (PositionManager/Worker/PathFinder.ts),
 * kept behaviourally faithful where it matters: the same 10/14 step costs, the
 * same octile heuristic and the same eight neighbours, so a routed path obeys
 * the same movement model the straight-line walk does.
 *
 * Three places it deliberately differs, all because this runs in the client
 * rather than in the server's model of the client:
 *
 *   - **It has a diagonal corner rule and the server does not.** The server may
 *     squeeze a diagonal between two blocked cells, because at grid level a
 *     diagonal only occupies its two endpoints. The client walks pixel by
 *     pixel, so it genuinely crosses one of the orthogonal neighbours and
 *     collides if that is a wall corner -- which wedged a 7-leg route at leg 5
 *     in testing. See the longer note in grid.c.
 *
 *   - **No maxPathCost.** The server's 650 budget exists to mirror how far the
 *     client is willing to route on its own, so the server's copy does not walk
 *     somewhere the real client would not. Here we ARE the client: we drive it
 *     leg by leg, and every leg is a straight segment the server validates the
 *     same way it validates a manual click. A destination 98 cells away is a
 *     perfectly good walk, it just needs more than one packet. What bounds the
 *     search instead is an expansion ceiling and the grid itself.
 *   - **Parents are stored as a direction, not a cell index.** One byte per
 *     cell instead of four, which matters at 375k cells.
 *
 * Memory. Scratch is allocated once, lazily, sized to the live grid, and freed
 * when the map changes. On a 500x750 map that is about 3 MB:
 *
 *     gCost   uint16   750 KB      parent  uint8    375 KB
 *     hCost   uint16   750 KB      heapPos uint16   750 KB
 *     stamp   uint16   750 KB      heap    int32    256 KB (capped)
 *
 * A full-grid A* rather than a window around the start, because a window big
 * enough to be correct for any destination is most of the grid anyway once the
 * cost cap is gone.
 */
#include "osmw_minimap.h"

#include <stdlib.h>
#include <string.h>

#define MM_COST_STRAIGHT 10
#define MM_COST_DIAGONAL 14

/* Crash guard. The grid bounds the search long before this bites. */
#define MM_MAX_ITERATIONS 200000

/* The open set never approaches this on a real map; exceeding it means
 * something is wrong, and bailing to the straight-line walk is safer than
 * growing without bound inside the client's render thread. */
#define MM_MAX_HEAP 65000

static const int mm_adj_x[8]    = { -1,  0,  1, -1,  1, -1,  0,  1 };
static const int mm_adj_y[8]    = { -1, -1, -1,  0,  0,  1,  1,  1 };
static const int mm_adj_cost[8] = {
    MM_COST_DIAGONAL, MM_COST_STRAIGHT, MM_COST_DIAGONAL,
    MM_COST_STRAIGHT,                   MM_COST_STRAIGHT,
    MM_COST_DIAGONAL, MM_COST_STRAIGHT, MM_COST_DIAGONAL
};

#define MM_PARENT_START 0xFF

static struct {
    unsigned short *g;
    unsigned short *h;
    unsigned short *stamp_open;
    unsigned short *stamp_closed;
    unsigned char  *parent;     /* index into mm_adj_*, or MM_PARENT_START */
    unsigned short *heap_pos;
    int            *heap;
    int             cells;
    int             heap_size;
    unsigned short  generation;
} g_s;

static void scratch_free(void)
{
    free(g_s.g);            free(g_s.h);
    free(g_s.stamp_open);   free(g_s.stamp_closed);
    free(g_s.parent);       free(g_s.heap_pos);
    free(g_s.heap);
    memset(&g_s, 0, sizeof(g_s));
}

static int scratch_ensure(int cells)
{
    if (g_s.cells == cells && g_s.g)
        return 1;

    scratch_free();

    g_s.g            = (unsigned short *)malloc((size_t)cells * sizeof(unsigned short));
    g_s.h            = (unsigned short *)malloc((size_t)cells * sizeof(unsigned short));
    g_s.stamp_open   = (unsigned short *)calloc((size_t)cells, sizeof(unsigned short));
    g_s.stamp_closed = (unsigned short *)calloc((size_t)cells, sizeof(unsigned short));
    g_s.parent       = (unsigned char  *)malloc((size_t)cells);
    g_s.heap_pos     = (unsigned short *)malloc((size_t)cells * sizeof(unsigned short));
    g_s.heap         = (int *)malloc((size_t)MM_MAX_HEAP * sizeof(int));

    if (!g_s.g || !g_s.h || !g_s.stamp_open || !g_s.stamp_closed ||
        !g_s.parent || !g_s.heap_pos || !g_s.heap) {
        mm_log("path: out of memory for a %d-cell grid", cells);
        scratch_free();
        return 0;
    }

    g_s.cells = cells;
    g_s.generation = 0;
    mm_log("path: scratch allocated for %d cells (~%u KB)",
           cells, (unsigned)((size_t)cells * 11 / 1024));
    return 1;
}

void mm_path_release(void)
{
    if (g_s.g) {
        mm_log("path: scratch released");
        scratch_free();
    }
}

/* Stamps make the previous search invisible without clearing any array. */
static void next_generation(void)
{
    g_s.generation++;
    if (g_s.generation == 0xFFFF) {
        memset(g_s.stamp_open, 0, (size_t)g_s.cells * sizeof(unsigned short));
        memset(g_s.stamp_closed, 0, (size_t)g_s.cells * sizeof(unsigned short));
        g_s.generation = 1;
    }
}

/* Lower f first, then lower h so ties resolve towards the destination rather
 * than fanning out sideways. */
static int scores_before(int a, int b)
{
    int fa = (int)g_s.g[a] + g_s.h[a];
    int fb = (int)g_s.g[b] + g_s.h[b];

    if (fa != fb) return fa < fb;
    return g_s.h[a] < g_s.h[b];
}

static void sift_up(int pos)
{
    int item = g_s.heap[pos];

    while (pos > 0) {
        int parent_pos = (pos - 1) >> 1;
        int parent = g_s.heap[parent_pos];

        if (!scores_before(item, parent)) break;
        g_s.heap[pos] = parent;
        g_s.heap_pos[parent] = (unsigned short)pos;
        pos = parent_pos;
    }
    g_s.heap[pos] = item;
    g_s.heap_pos[item] = (unsigned short)pos;
}

static void sift_down(int pos)
{
    int item = g_s.heap[pos];

    for (;;) {
        int left = pos * 2 + 1, right, child;

        if (left >= g_s.heap_size) break;
        right = left + 1;
        child = (right < g_s.heap_size && scores_before(g_s.heap[right], g_s.heap[left]))
                ? right : left;
        if (!scores_before(g_s.heap[child], item)) break;

        g_s.heap[pos] = g_s.heap[child];
        g_s.heap_pos[g_s.heap[pos]] = (unsigned short)pos;
        pos = child;
    }
    g_s.heap[pos] = item;
    g_s.heap_pos[item] = (unsigned short)pos;
}

static int heap_push(int index)
{
    if (g_s.heap_size >= MM_MAX_HEAP)
        return 0;
    g_s.heap[g_s.heap_size] = index;
    g_s.heap_pos[index] = (unsigned short)g_s.heap_size;
    g_s.heap_size++;
    sift_up(g_s.heap_size - 1);
    return 1;
}

static int heap_pop(void)
{
    int top = g_s.heap[0];

    g_s.heap_size--;
    if (g_s.heap_size > 0) {
        int moved = g_s.heap[g_s.heap_size];
        g_s.heap[0] = moved;
        g_s.heap_pos[moved] = 0;
        sift_down(0);
    }
    return top;
}

/* Octile distance, scaled to the movement costs. */
static int octile(int ax, int ay, int bx, int by)
{
    int dx = ax > bx ? ax - bx : bx - ax;
    int dy = ay > by ? ay - by : by - ay;
    int min = dx < dy ? dx : dy;

    return (dx + dy) * MM_COST_STRAIGHT
         + (MM_COST_DIAGONAL - 2 * MM_COST_STRAIGHT) * min;
}

/* Nearest walkable cell to a blocked one, searched outwards ring by ring.
 *
 * A port of the server's findNearestWalkable, and it exists for the same reason
 * its comment gives: the character is standing wherever it thinks it stands,
 * and refusing to plan from there strands it for good. The client's own
 * collision is finer than this 16x8 grid, so a character can genuinely come to
 * rest on a cell the grid calls blocked -- which is what a route that stalls
 * immediately at leg 1, twice running, looks like.
 *
 * Cells are 16px wide and 8px tall, so a step along x covers twice the ground
 * of a step along y. Weighting dx by that makes "nearest" mean nearest on
 * screen rather than nearest in cells.
 *
 * Map-link cells are disqualified, matching the server's version and for its
 * stated reason: recovering a position must not drop the character onto a
 * transition it never walked into. Being wedged is a nuisance; being moved to
 * another map because the nearest clear cell happened to be a doorway is not.
 * This check was missing until the link bit was identified as 0x4000.
 */
#define MM_SNAP_RINGS 4
#define MM_CELL_ASPECT 2

int mm_nearest_walkable(int x, int y, int *out_x, int *out_y)
{
    int w, h, ring, best = -1, best_x = 0, best_y = 0;

    if (!mm_grid_dims(&w, &h))
        return 0;

    if (mm_cell_walkable(x, y)) {
        if (out_x) *out_x = x;
        if (out_y) *out_y = y;
        return 1;
    }

    for (ring = 1; ring <= MM_SNAP_RINGS; ring++) {
        int dx, dy;

        for (dy = -ring; dy <= ring; dy++) {
            for (dx = -ring; dx <= ring; dx++) {
                int cx, cy, sx, distance;

                /* Only the outer edge of the ring is new. */
                if ((dx > -ring && dx < ring) && (dy > -ring && dy < ring))
                    continue;

                cx = x + dx;
                cy = y + dy;
                if (cx < 0 || cy < 0 || cx >= w || cy >= h) continue;
                if (!mm_cell_walkable(cx, cy)) continue;
                if (mm_cell_is_link(cx, cy)) continue;

                sx = dx * MM_CELL_ASPECT;
                distance = sx * sx + dy * dy;
                if (best < 0 || distance < best) {
                    best = distance;
                    best_x = cx;
                    best_y = cy;
                }
            }
        }

        if (best >= 0) {
            if (out_x) *out_x = best_x;
            if (out_y) *out_y = best_y;
            return 1;
        }
    }

    return 0;
}

int mm_find_path(int sx, int sy, int ex, int ey, mm_cell_t *out, int max_out)
{
    int w, h, start, end, iterations = 0, reached = 0, count = 0;

    if (!mm_grid_dims(&w, &h) || !out || max_out <= 0)
        return 0;
    if (sx < 0 || sy < 0 || sx >= w || sy >= h) return 0;
    if (ex < 0 || ey < 0 || ex >= w || ey >= h) return 0;
    if (!mm_cell_walkable(ex, ey)) return 0;

    /* Recover a blocked start rather than refusing. See mm_nearest_walkable. */
    if (!mm_cell_walkable(sx, sy)) {
        int rx, ry;

        if (!mm_nearest_walkable(sx, sy, &rx, &ry)) {
            mm_log("path: start (%d,%d) is blocked and nothing walkable within "
                   "%d cells", sx, sy, MM_SNAP_RINGS);
            return 0;
        }
        mm_log("path: start (%d,%d) is blocked, planning from (%d,%d) instead",
               sx, sy, rx, ry);
        sx = rx;
        sy = ry;
    }

    if (sx == ex && sy == ey) return 0;
    if (!scratch_ensure(w * h)) return 0;

    start = sy * w + sx;
    end   = ey * w + ex;

    next_generation();
    g_s.heap_size = 0;
    g_s.g[start] = 0;
    g_s.h[start] = (unsigned short)octile(sx, sy, ex, ey);
    g_s.parent[start] = MM_PARENT_START;
    g_s.stamp_open[start] = g_s.generation;
    heap_push(start);

    while (g_s.heap_size > 0) {
        int index, x, y, a;

        if (++iterations >= MM_MAX_ITERATIONS) {
            mm_log("path: gave up after %d expansions", iterations);
            return 0;
        }

        index = heap_pop();
        g_s.stamp_closed[index] = g_s.generation;

        if (index == end) { reached = 1; break; }

        x = index % w;
        y = index / w;

        for (a = 0; a < 8; a++) {
            int ax = x + mm_adj_x[a], ay = y + mm_adj_y[a], ai, cost;

            if (ax < 0 || ay < 0 || ax >= w || ay >= h) continue;
            ai = ay * w + ax;
            if (g_s.stamp_closed[ai] == g_s.generation) continue;
            if (!mm_cell_walkable(ax, ay)) continue;

            /* Corner rule, matching mm_walk_line and diverging from the server
             * on purpose -- see the note in grid.c. The client walks pixel by
             * pixel, so a diagonal crosses one of the two orthogonal
             * neighbours; letting the route cut a wall corner wedges it. */
            if (mm_adj_x[a] && mm_adj_y[a] &&
                (!mm_cell_walkable(x + mm_adj_x[a], y) ||
                 !mm_cell_walkable(x, y + mm_adj_y[a])))
                continue;

            cost = (int)g_s.g[index] + mm_adj_cost[a];
            if (cost > 0xFFFE) continue;    /* gCost is 16-bit */

            if (g_s.stamp_open[ai] == g_s.generation) {
                if (g_s.g[ai] <= cost) continue;
                g_s.g[ai] = (unsigned short)cost;
                g_s.parent[ai] = (unsigned char)a;
                sift_up(g_s.heap_pos[ai]);
                continue;
            }

            g_s.stamp_open[ai] = g_s.generation;
            g_s.parent[ai] = (unsigned char)a;
            g_s.g[ai] = (unsigned short)cost;
            g_s.h[ai] = (unsigned short)octile(ax, ay, ex, ey);
            if (!heap_push(ai)) {
                mm_log("path: open set overflowed");
                return 0;
            }
        }
    }

    if (!reached)
        return 0;

    /* Follow the parent directions back, then reverse into start-to-end order.
     * Walking backwards means stepping the OPPOSITE way to the stored one. */
    {
        int index = end, guard = 0;

        while (guard++ < g_s.cells) {
            int a;

            if (count < max_out) {
                out[count].x = (short)(index % w);
                out[count].y = (short)(index / w);
            }
            count++;

            a = g_s.parent[index];
            if (a == MM_PARENT_START) break;
            index = (index / w - mm_adj_y[a]) * w + (index % w - mm_adj_x[a]);
        }

        if (count > max_out) {
            mm_log("path: %d cells is longer than the %d-cell buffer", count, max_out);
            return 0;
        }

        /* end-to-start -> start-to-end */
        {
            int i, j;
            for (i = 0, j = count - 1; i < j; i++, j--) {
                mm_cell_t t = out[i];
                out[i] = out[j];
                out[j] = t;
            }
        }
    }

    return count;
}

int mm_simplify_path(const mm_cell_t *cells, int count, mm_cell_t *out, int max_out)
{
    int n = 0, anchor = 0;

    if (count <= 0 || !cells || !out || max_out <= 0)
        return 0;

    /* Keep only the cells where the route has to change direction, by extending
     * each leg as far as the straight-line walk can actually reach. The client
     * walks the line itself, so a leg is one packet however long it is -- and
     * the server validates each one exactly as it would a manual click. */
    while (anchor < count - 1) {
        int furthest = anchor + 1, i;

        for (i = anchor + 2; i < count; i++) {
            if (!mm_walk_line(cells[anchor].x, cells[anchor].y,
                              cells[i].x, cells[i].y, NULL, NULL))
                break;
            furthest = i;
        }

        if (n >= max_out)
            return n;
        out[n++] = cells[furthest];
        anchor = furthest;
    }

    return n;
}
