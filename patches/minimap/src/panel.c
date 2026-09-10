/* The list panel: layout, its UI node, drawing, and selection.
 *
 * Layout. The client is 800x600 -- the stock minimap rect (196,148)-(604,452)
 * is exactly centred in it -- so a 200px panel cannot simply be attached at
 * 604, which would run 4px off the right edge. MW2 solved this by moving its
 * map left and we do the same: map to (96,148)-(504,452), panel to
 * (504,148)-(704,452), leaving symmetric 96px margins.
 *
 * How, without patching main.exe or reverse-engineering the node layout:
 * OpenMinimap builds the rect as four immediates and passes it BY VALUE to
 * UiCreateNode through a direct `call rel32`. Repointing that one call site
 * hands us the rect as ordinary arguments before the node exists, so we reshape
 * it on the way in. Only the displacement at that site changes -- UiCreateNode
 * itself is untouched and every other window still reaches it directly, which
 * matters because it builds all of them.
 *
 * The panel is then created through the same function, so it is an ordinary
 * client window as far as the client is concerned.
 */
#include "osmw_minimap.h"

#include <stdio.h>
#include <string.h>

/* The stock rect, read off OpenMinimap's immediates at 0x44239E..0x4423BC.
 * Not taken on faith: the detour compares against these and leaves the rect
 * alone if the client hands it something else, so a client whose minimap has
 * moved gets no layout change rather than a misplaced one. */
#define MM_STOCK_LEFT    196
#define MM_STOCK_TOP     148
#define MM_STOCK_RIGHT   604
#define MM_STOCK_BOTTOM  452

/* 196 - 100 = 96, matching MW2. */
#define MM_MAP_SHIFT     100
#define MM_PANEL_WIDTH   200

#define MM_ROW_HEIGHT    14
#define MM_ROWS_VISIBLE  16
#define MM_PAD_X         10
#define MM_PAD_Y         8

/* Vertical bands inside the panel, all relative to its top edge:
 * a header line, then the rows, then the scroll controls. */
#define MM_HEADER_DY     MM_PAD_Y
#define MM_ROWS_DY       (MM_PAD_Y + MM_ROW_HEIGHT)
#define MM_FOOTER_DY     (MM_ROWS_DY + MM_ROWS_VISIBLE * MM_ROW_HEIGHT + 4)
/* Marks mode adds an action row under the scroll controls. 8 + 14 + 16*14 + 4
 * is 250, so a second line at 264 still clears the 304px panel. */
#define MM_ACTION_DY     (MM_FOOTER_DY + MM_ROW_HEIGHT)
#define MM_ACTION_COLS   5

#define MM_SCROLL_STEP   4
#define MM_MAX_NPCS      64

/* The destroy call sits 17 bytes into the UiCreateCall pattern, on the close
 * branch of the same toggle:
 *
 *   +0   E8 ?? ?? ?? ??   call UiCreateNode     <- the open path
 *   +5   83 C4 20
 *   +8   A3 ?? ?? ?? ??   mov [handle], eax
 *   +13  5F 5E C3         pop/pop/ret
 *   +16  50               push eax              <- the close path
 *   +17  E8 ?? ?? ?? ??   call UiDestroyNode
 *
 * Deriving it rather than giving it its own signature is deliberate: a pattern
 * anchored on the destroy call had to run into the next function's padding
 * before it was unique, which would break on an unrelated change downstream.
 * Offset 17 is a fixed E8 byte inside a pattern that already matched, so it
 * cannot be anything else, and it moves with the function.
 */
/* __cdecl: the call site cleans 0x20 bytes of arguments itself. The RECT is
 * passed by value, so it arrives as four separate ints. */
typedef int(__cdecl *mm_uicreate_fn)(int left, int top, int right, int bottom,
                                     void *draw_proc, void *input_proc,
                                     int arg4, int list_sel);
/* UiDestroyNode(handle, list_sel) -- TWO arguments.
 *
 * It reads the list selector from [esp+8] at 0x45E650 and picks 0x6293A0 or
 * 0x629374 from it, and OpenMinimap's close path cleans with `add esp, 8`; the
 * matching `push 0` sits back at 0x44238B, before the branch that chooses
 * between opening and closing, which is why it is easy to miss when reading
 * only the close path.
 *
 * This was declared with one argument. Forwarding a single push left the callee
 * reading rubbish for list_sel, so a non-zero value sent it hunting the wrong
 * list, where the node was not found and therefore never freed. The node stayed
 * linked and kept intercepting clicks across its whole rect -- invisible,
 * because its draw proc had stopped painting -- and the log still said
 * "destroying node N" because that line is printed before the call, not after
 * a result.
 *
 * The client's own destroy of the minimap node also passes through the detour,
 * so that was leaking on every close too.
 */
typedef void(__cdecl *mm_uidestroy_fn)(int handle, int list_sel);

/* Text drawing, copied from the idiom the stock minimap uses for its coord
 * readout at 0x43EF73:
 *
 *     DrawText(x, y, text, 0, ctx)      ctx = *(void **)0x7E6A04
 *
 * mode 1 applies a control code ("~Z0") rather than drawing; we only ever draw.
 * Colour markup is inline in the string -- ~C1 sets, ~C0 resets -- which is how
 * the stock coord line gets its highlight.
 */
typedef void(__cdecl *mm_drawtext_fn)(int x, int y, const char *text,
                                      int mode, void *ctx);

/* The client's own tooltip frame, so the panel matches the rest of the UI
 * instead of being a rectangle we invented.
 *
 * 0x45CDD6 loads nine pieces from system\\info.mda into a 0x14-stride array at
 * 0x703224, in draw order -- note MDA frame 8, the centre fill, goes into slot
 * 4. 0x4615B0 tiles them over an arbitrary rect: 16px corners with 8px edge
 * pieces between, so it needs (w-16)/8 and (h-16)/8 to come out whole.
 *
 * The panel is 200x304, which gives 23 and 36 exactly.
 */
typedef void(__cdecl *mm_drawbox_fn)(int x, int y, int w, int h, void *ctx);

static mm_hook_t       g_call_hook;
static mm_hook_t       g_destroy_hook;
static mm_uicreate_fn  g_orig_uicreate;
static mm_uidestroy_fn g_orig_uidestroy;

/* Our node. -1 when it does not exist. */
static int g_panel_handle = -1;

/* Where the panel goes. Zeroed until the map has been opened once. */
static int g_panel[4];

/* Where the map itself ended up, recorded at the same moment for the same
 * reason: the marker layer projects into this rect, and it is not the stock
 * 196/148 immediates once the panel has shifted the map left. */
static mm_rect_t g_map;

/* Which array slot each drawn row came from, rebuilt every frame.
 *
 * The draw and the click have to agree on what "row 3" means, and the array is
 * live -- an NPC can appear or vanish between the two. Recording the mapping as
 * the rows are drawn means a click always resolves against what was actually on
 * screen rather than re-deriving it and possibly landing on a different NPC.
 */
static int g_row_slot[MM_ROWS_VISIBLE];
static unsigned short g_row_x[MM_ROWS_VISIBLE];
static unsigned short g_row_y[MM_ROWS_VISIBLE];
static int g_row_count;
static int g_selected = -1;     /* array slot, not row index */

/* Which list the panel is showing. The two share the row geometry, the scroll
 * and the click edge detection; only what fills the rows differs. */
enum { MM_MODE_NPCS = 0, MM_MODE_MARKS };
static int g_mode = MM_MODE_NPCS;

/* Marks mode: the marker id each drawn row belongs to, and the selected one.
 * Ids rather than indices because the store is reloaded from disk underneath
 * us -- another client's edit can renumber every index between a draw and the
 * click that follows it, but an id still names the same marker. */
static char g_row_id[MM_ROWS_VISIBLE][MM_MARKER_ID_LEN + 1];
static char g_selected_id[MM_MARKER_ID_LEN + 1];

/* Delete is two clicks. Armed by the first, cleared by anything else, so a
 * stray click cannot destroy a marker and the armed state is always visible. */
static int g_delete_armed;

/* A snapshot of the NPCs, rebuilt each frame before drawing.
 *
 * Taken as a copy rather than sorted in place over the live array for two
 * reasons: the client reallocates that array, so a comparison function that
 * re-read it mid-sort could dereference a stale base; and the coordinates the
 * marker uses then match the ones the row displayed, instead of drifting a few
 * pixels for a walking NPC between the draw and the click.
 */
typedef struct {
    int            slot;        /* index into the live map-character array */
    unsigned short x, y;        /* map pixels, as displayed */
    char           name[15];    /* the 14-byte field, terminated */
} mm_npc_t;

static mm_npc_t g_list[MM_MAX_NPCS];
static int      g_list_count;
static int      g_scroll;       /* index of the first visible row */

/* Set once the stock minimap has gone away underneath us.
 *
 * The map has more than one close path and only one of them is ours. Tab and
 * the in-window right-click go through the minimap's own input proc, which
 * writes the handle to -1 directly at 0x42002E / 0x4200B3 and never calls the
 * destructor we hooked -- so the panel was left on screen after the map had
 * gone.
 *
 * It cannot be destroyed at the moment we notice: we are inside a node walk,
 * and both walkers iterate with the global cursor at 0x7BAD98, so relinking
 * the list would corrupt the iteration. Collapsing the rect is safe because it
 * only assigns fields, and it makes the panel both invisible and un-hittable.
 * The node itself is destroyed on the next open, where we are inside
 * OpenMinimap rather than a walk.
 */
static int g_orphaned;

/* Edge state for clicks. See panel_input. */
static int g_click_armed = 1;

/* Both stock procs take no arguments and read global state, so ours do too.
 * They are __cdecl, so even if the client did push something the caller cleans
 * it and ignoring it is safe. */
static void __cdecl panel_draw(void);
static void panel_text_boxed(int x, int y, const char *text);
static void panel_consume_event(void);
static void panel_release_click(void);
static void __cdecl panel_input(void);

mm_marker_text_fn mm_panel_text_boxed(void)
{
    return panel_text_boxed;
}

int mm_map_rect(mm_rect_t *out)
{
    if (!g_map.right)
        return 0;
    if (out)
        *out = g_map;
    return 1;
}

int mm_panel_rect(int *left, int *top, int *right, int *bottom)
{
    if (!g_panel[2])
        return 0;
    if (left)   *left   = g_panel[0];
    if (top)    *top    = g_panel[1];
    if (right)  *right  = g_panel[2];
    if (bottom) *bottom = g_panel[3];
    return 1;
}

/* Draw one line.
 *
 * The fourth argument picks the renderer, and which one you want is not
 * obvious from the minimap:
 *
 *     mode 1  -> 0x49B920, plain glyphs. 780 of the client's 863 DrawText
 *                call sites use this. It is the normal case.
 *     mode 0  -> 0x49A390, glyphs on a filled backdrop. 28 sites, and the
 *                stock minimap's coordinate readout is one of them -- it draws
 *                over map terrain, where the backdrop is what keeps it legible.
 *
 * This panel copied its call from that readout and inherited the backdrop with
 * it, which put a black box behind every row. On a panel with its own frame
 * there is nothing to be legible against, so mode 1 is right here.
 *
 * (The `~Z` codes are a red herring for this: they are inline markup the client
 * strstr's for at 0x49C8EE, and the lookup is gated on a byte at 0x660608 that
 * reads 0.)
 */
static void panel_text(int x, int y, const char *text)
{
    mm_drawtext_fn draw;
    void *ctx;

    if (!mm_addr[MM_DRAWTEXT] || !mm_addr[MM_DRAWCTX])
        return;

    /* Null until the client is actually rendering, so read it per call rather
     * than caching it at install time. */
    ctx = *(void **)mm_addr[MM_DRAWCTX];
    if (!ctx)
        return;

    draw = (mm_drawtext_fn)mm_addr[MM_DRAWTEXT];
    draw(x, y, text, 1, ctx);
}

/* The same primitive in mode 0, the renderer that fills a backdrop behind the
 * glyphs.
 *
 * Mode 1 is right for panel rows, which sit on the panel's own frame. Marker
 * labels do not: they are drawn over map terrain, where a plain glyph is very
 * hard to read against the artwork -- which is exactly why the stock minimap's
 * coordinate readout, the other thing drawn over terrain, is one of the 28
 * sites that use mode 0. */
static void panel_text_boxed(int x, int y, const char *text)
{
    mm_drawtext_fn draw;
    void *ctx;

    if (!mm_addr[MM_DRAWTEXT] || !mm_addr[MM_DRAWCTX])
        return;

    ctx = *(void **)mm_addr[MM_DRAWCTX];
    if (!ctx)
        return;

    draw = (mm_drawtext_fn)mm_addr[MM_DRAWTEXT];
    draw(x, y, text, 0, ctx);
}

/* Fill the panel with the tooltip frame. Drawn before any text, so the rows
 * land on top of it. */
static void panel_box(const mm_rect_t *r)
{
    mm_drawbox_fn box;
    void *ctx;

    if (!mm_addr[MM_DRAWINFOBOX] || !mm_addr[MM_DRAWCTX])
        return;

    /* The nine frames are loaded during client init. If that has not happened
     * -- or failed -- the array is still null and the blits would draw
     * nothing useful, so skip the background rather than risk it. */
    if (!mm_addr[MM_INFOFRAMES] || !*(void **)mm_addr[MM_INFOFRAMES])
        return;

    ctx = *(void **)mm_addr[MM_DRAWCTX];
    if (!ctx)
        return;

    box = (mm_drawbox_fn)mm_addr[MM_DRAWINFOBOX];
    box(r->left, r->top, r->right - r->left, r->bottom - r->top, ctx);
}

/* Transient spawns that should not appear in the list.
 *
 * Taken by id rather than by name, from the server's own table in
 * GameActions/Actions/Events/EventScannerExecutable.ts, which is the single
 * place these are enumerated. Ids are stable; names are not a safe key, since
 * two Questicon entries share a name and one of the family is spelled
 * "Doplicon".
 *
 * They are filtered because they are not landmarks. Treasure chests spawn at
 * random intervals and are gone again; the -icon families wander and are only
 * present during their events. A list that reorders itself as they come and go
 * is worse than one that leaves them out -- and everything here already has its
 * own in-game scanner for finding it.
 *
 * [Minimap] ShowEventNpcs=1 brings them back. That is there mainly for the
 * Questicon family, which unlike the others is a quest giver, so knowing where
 * it currently stands has some argument for it. Defaulted off because the
 * request was to filter the wandering NPCs as a group.
 */
static const unsigned int mm_event_npcs[] = {
    /* treasure chests, two ranges */
    0x80003010, 0x80003011, 0x80003012,
    0x80003412, 0x80003413, 0x80003414,
    /* Mysticon, Shifticon, Doplicon */
    0x80003013, 0x80003019, 0x8000302E,
    /* Questicon, Taskicon, Questicon */
    0x80003032, 0x80003033, 0x80003034,
    /* Gamblicon, Wagericon, Riskicon */
    0x80003035, 0x80003036, 0x80003037,
};

static int is_event_npc(unsigned int id)
{
    int i;

    for (i = 0; i < (int)(sizeof(mm_event_npcs) / sizeof(mm_event_npcs[0])); i++)
        if (mm_event_npcs[i] == id)
            return 1;
    return 0;
}

/* The map carries invisible NPCs to hang interactions off -- reading a mural,
 * throwing a coin into a well. They have no name, so an empty field is the
 * marker. Whitespace counts as empty: a name that draws as nothing is nothing
 * as far as a list of places to walk to is concerned. */
static int is_nameless(const char *name)
{
    int i;

    for (i = 0; i < 14 && name[i]; i++)
        if (name[i] != ' ' && name[i] != '\t')
            return 0;
    return 1;
}

/* Snapshot the NPCs and sort them by name.
 *
 * MW2 lists in packet order; sorted is materially easier to use once a town has
 * forty of them. Insertion sort because the list is a few tens of entries and
 * runs once per frame -- the cost is noise next to the draw calls.
 */
static void panel_build_list(void)
{
    int count = mm_map_char_count();
    int show_events = mm_cfg_int("ShowEventNpcs", 0);
    int n = 0, i, j;

    for (i = 0; i < count && n < MM_MAX_NPCS; i++) {
        unsigned char *slot = mm_map_char_slot(i);

        if (!slot || slot[MM_SLOT_TYPE] != MM_TYPE_NPC)
            continue;
        if (is_nameless((const char *)(slot + MM_SLOT_NAME)))
            continue;
        if (!show_events &&
            is_event_npc((unsigned int)*(int *)(slot + MM_SLOT_ID)))
            continue;

        g_list[n].slot = i;
        g_list[n].x = *(unsigned short *)(slot + MM_SLOT_X);
        g_list[n].y = *(unsigned short *)(slot + MM_SLOT_Y);
        /* The field is 14 bytes and is not guaranteed to be terminated. */
        memcpy(g_list[n].name, slot + MM_SLOT_NAME, 14);
        g_list[n].name[14] = 0;
        n++;
    }

    for (i = 1; i < n; i++) {
        mm_npc_t v = g_list[i];
        for (j = i; j > 0 && _strnicmp(g_list[j - 1].name, v.name, 14) > 0; j--)
            g_list[j] = g_list[j - 1];
        g_list[j] = v;
    }

    g_list_count = n;

    /* NPCs can leave while the list is scrolled to the bottom. */
    if (g_scroll > n - MM_ROWS_VISIBLE)
        g_scroll = n - MM_ROWS_VISIBLE;
    if (g_scroll < 0)
        g_scroll = 0;
}

/* The mode switch, drawn on the header line so the rows keep their geometry.
 * The active tab is highlighted; the count sits to the right of both. */
static void panel_draw_tabs(const mm_rect_t *r, int shown, int total)
{
    char line[96];

    _snprintf(line, sizeof(line), "%s[NPCs]~C0 %s[Marks]~C0 ~C1%d~C0/~C1%d~C0",
              g_mode == MM_MODE_NPCS ? "~C4" : "",
              g_mode == MM_MODE_MARKS ? "~C4" : "",
              shown, total);
    line[sizeof(line) - 1] = 0;
    panel_text(r->left + MM_PAD_X, r->top + MM_HEADER_DY, line);
}

/* Rows for the marker store, filtered to this map.
 *
 * Walked fresh every frame rather than cached: the store is shared and reloaded
 * on a timer, so a marker another client added two seconds ago should appear
 * without the panel being reopened.
 */
static void panel_draw_marker_rows(const mm_rect_t *r)
{
    const mm_marker_set_t *set = mm_store_get();
    char line[96];
    int map_file = mm_markers_map_file();
    int total = 0, matched = 0, rows = 0, i;

    g_row_count = 0;

    if (set == NULL || map_file < 0) {
        panel_draw_tabs(r, 0, 0);
        panel_text(r->left + MM_PAD_X, r->top + MM_ROWS_DY, "No map");
        return;
    }

    for (i = 0; i < set->count; i++) {
        if ((int)set->items[i].map_file == map_file)
            total++;
    }

    panel_draw_tabs(r, total ? g_scroll + 1 : 0, total);

    /* An empty map still needs the action row: [Add] is how the first marker
     * on it gets created, and returning early here left the player with no way
     * to make one. Only the rows are skipped. */
    if (total == 0)
        panel_text(r->left + MM_PAD_X, r->top + MM_ROWS_DY,
                   "No marks on this map");

    /* While a label is being typed the field replaces the list: the rows are
     * what the player is editing away from, and leaving them clickable behind
     * a modal invites selecting a different marker mid-edit. */
    if (mm_edit_active()) {
        char field[96];
        _snprintf(field, sizeof(field), "Name: ~C1%s~C0_", mm_edit_text());
        field[sizeof(field) - 1] = 0;
        panel_text(r->left + MM_PAD_X, r->top + MM_ROWS_DY, field);
        panel_text(r->left + MM_PAD_X, r->top + MM_ROWS_DY + MM_ROW_HEIGHT * 2,
                   "~C1Enter~C0 saves, ~C1Esc~C0 cancels");
        panel_text(r->left + MM_PAD_X, r->top + MM_ACTION_DY, "~C1[Editing]~C0");
        return;
    }

    for (i = 0; i < set->count && rows < MM_ROWS_VISIBLE; i++) {
        const mm_marker_t *m = &set->items[i];
        int gx = 0, gy = 0;

        if ((int)m->map_file != map_file)
            continue;
        if (matched++ < g_scroll)
            continue;

        mm_pixel_to_grid((int)m->x, (int)m->y, &gx, &gy);
        /* A hidden marker still lists, prefixed, so it can be found and
         * turned back on -- otherwise hiding one loses it. */
        _snprintf(line, sizeof(line), "%s%s%s ~C1(%d,%d)~C0",
                  strcmp(g_selected_id, m->id) == 0 ? "~C4" : "",
                  (m->flags & MM_MARKER_FLAG_VISIBLE) ? "" : "- ",
                  m->label[0] ? m->label : "(unnamed)", gx, gy);
        line[sizeof(line) - 1] = 0;

        panel_text(r->left + MM_PAD_X,
                   r->top + MM_ROWS_DY + rows * MM_ROW_HEIGHT, line);

        _snprintf(g_row_id[rows], sizeof(g_row_id[rows]), "%s", m->id);
        g_row_x[rows] = (unsigned short)m->x;
        g_row_y[rows] = (unsigned short)m->y;
        g_row_slot[rows] = -1;
        rows++;
    }
    g_row_count = rows;

    if (total > MM_ROWS_VISIBLE)
        panel_text(r->left + MM_PAD_X, r->top + MM_FOOTER_DY,
                   "~C1[ Up ]~C0      ~C1[ Down ]~C0");

    /* The action row.
     *
     * Each button is drawn AT the origin of the column it is hit-tested in.
     * Drawing them as one string does not work: the labels are not all the
     * same width, so the drawn positions drift away from the equal-width hit
     * zones and clicks land on the neighbouring action -- which made [Name]
     * fire [Kind]. Delete needs a second click and says so while armed. */
    {
        static const char *const labels[MM_ACTION_COLS] = {
            "[Add]", "[Move]", "[Kind]", "[Name]", "[Del]"
        };
        int colw = (MM_PANEL_WIDTH - 2 * MM_PAD_X) / MM_ACTION_COLS;
        int c;

        for (c = 0; c < MM_ACTION_COLS; c++) {
            char cell[24];
            _snprintf(cell, sizeof(cell), "%s%s~C0",
                      (c == MM_ACTION_COLS - 1 && g_delete_armed) ? "~C4" : "~C1",
                      (c == MM_ACTION_COLS - 1 && g_delete_armed) ? "[Sure?]"
                                                                  : labels[c]);
            cell[sizeof(cell) - 1] = 0;
            panel_text(r->left + MM_PAD_X + c * colw,
                       r->top + MM_ACTION_DY, cell);
        }
    }
}

/* One row per NPC: name, then grid coords in the same colour scheme the stock
 * coord readout uses. The selected row is drawn in ~C4. */
static void panel_draw_rows(const mm_rect_t *r)
{
    char line[96];
    int rows = 0;
    int i;

    panel_box(r);

    if (g_mode == MM_MODE_MARKS) {
        panel_draw_marker_rows(r);
        return;
    }

    panel_build_list();

    if (!g_list_count) {
        panel_draw_tabs(r, 0, 0);
        panel_text(r->left + MM_PAD_X, r->top + MM_ROWS_DY, "No NPCs on this map");
        g_row_count = 0;
        return;
    }

    for (i = g_scroll; i < g_list_count && rows < MM_ROWS_VISIBLE; i++, rows++) {
        int gx = 0, gy = 0;

        mm_pixel_to_grid(g_list[i].x, g_list[i].y, &gx, &gy);
        _snprintf(line, sizeof(line), "%s%s ~C1(%d,%d)~C0",
                  g_list[i].slot == g_selected ? "~C4" : "",
                  g_list[i].name, gx, gy);
        line[sizeof(line) - 1] = 0;

        panel_text(r->left + MM_PAD_X,
                   r->top + MM_ROWS_DY + rows * MM_ROW_HEIGHT, line);

        g_row_slot[rows] = g_list[i].slot;
        g_row_x[rows] = g_list[i].x;
        g_row_y[rows] = g_list[i].y;
    }
    g_row_count = rows;

    panel_draw_tabs(r, g_scroll + rows, g_list_count);

    if (g_list_count > MM_ROWS_VISIBLE)
        panel_text(r->left + MM_PAD_X, r->top + MM_FOOTER_DY,
                   "~C1[ Up ]~C0      ~C1[ Down ]~C0");
}

/* Called by the client's node walker every frame while the map is open.
 *
 * Nothing here may create, destroy or relink a node: the walker iterates with a
 * global cursor at 0x7BAD98 and re-reads it after this returns. Reading the
 * map-character array and calling DrawText are both safe.
 */
static void __cdecl panel_draw(void)
{
    static int logged;
    mm_rect_t r = {0, 0, 0, 0};
    int flag = 0;

    if (g_panel_handle < 0)
        return;

    /* Nothing to draw with the map shut. The node is freed by mm_panel_idle,
     * which runs from the UI tick rather than from here -- this proc stops
     * being called, so it cannot be what cleans up after itself. */
    if (mm_minimap_handle() < 0)
        return;

    if (mm_node_rect(g_panel_handle, &r, &flag) != 0)
        return;

    if (logged < 2) {
        logged++;
        mm_log("panel: draw #%d, rect (%d,%d)-(%d,%d), ctx %p",
               logged, r.left, r.top, r.right, r.bottom,
               mm_addr[MM_DRAWCTX] ? *(void **)mm_addr[MM_DRAWCTX] : NULL);
    }

    panel_draw_rows(&r);

    /* The marker layer is NOT drawn here. It was, and the glyphs were painted
     * over whenever the map redrew its content after this node -- see the
     * comment on the draw-proc hook in markers_map.c. It now draws from the
     * map's own proc instead. */
}

/* Put the map marker on an NPC.
 *
 * Purely client-side: the marker globals are what the stock minimap draws its
 * red banner from, and writing them is exactly what the client's own 0x00110004
 * handler does at 0x458DA1. No packet is sent.
 */
/* Where the player is, in the map units markers are stored in. */
static int panel_player_pos(unsigned *x, unsigned *y)
{
    unsigned char *slot = mm_map_char_slot(mm_local_slot_index());

    if (slot == NULL)
        return 0;
    *x = *(unsigned short *)(slot + MM_SLOT_X);
    *y = *(unsigned short *)(slot + MM_SLOT_Y);
    return 1;
}

/* Copy the currently selected marker out of the store.
 *
 * A copy, not a pointer: every mutation goes through the store, which reloads
 * from disk under its lock and can move or replace the array in the process. A
 * pointer taken before that call is not valid after it. */
static int panel_selected_marker(mm_marker_t *out)
{
    const mm_marker_set_t *set = mm_store_get();
    int at;

    if (set == NULL || g_selected_id[0] == '\0')
        return 0;
    at = mm_markers_find(set, g_selected_id);
    if (at < 0)
        return 0;
    *out = set->items[at];
    return 1;
}

static void panel_action_add(void)
{
    mm_marker_t m;
    unsigned px = 0, py = 0;
    int map_file = mm_markers_map_file();
    int rc;

    if (map_file < 0 || !panel_player_pos(&px, &py))
        return;
    if (!mm_store_writable()) {
        mm_log("panel: store is read-only; not adding");
        return;
    }

    memset(&m, 0, sizeof(m));
    mm_markers_make_id(m.id);
    m.map_file = (unsigned short)map_file;
    m.x = px;
    m.y = py;
    m.kind = (unsigned char)mm_cfg_int("DefaultMarkerKind", MM_MARKER_KIND_NOTE);
    if (m.kind >= MM_MARKER_KIND_COUNT)
        m.kind = MM_MARKER_KIND_NOTE;
    m.flags = MM_MARKER_FLAG_VISIBLE;
    /* Auto-named until the editor can take text. Deliberately not left blank:
     * an unnamed row is hard to tell from its neighbours in the list. */
    _snprintf(m.label, sizeof(m.label), "Mark %u,%u", px / 16u, py / 8u);
    m.label[sizeof(m.label) - 1] = 0;

    rc = mm_store_put(&m);
    if (rc != MM_MARKER_OK) {
        mm_log("panel: add refused (%d)", rc);
        return;
    }
    _snprintf(g_selected_id, sizeof(g_selected_id), "%s", m.id);
    mm_markers_set_selected(g_selected_id);
    mm_log("panel: added mark %s at (%u,%u)", m.id, px, py);
}

static void panel_action_move(void)
{
    mm_marker_t m;
    unsigned px = 0, py = 0;

    if (!panel_selected_marker(&m) || !panel_player_pos(&px, &py))
        return;
    m.x = px;
    m.y = py;
    if (mm_store_put(&m) == MM_MARKER_OK)
        mm_log("panel: moved mark %s to (%u,%u)", m.id, px, py);
}

static void panel_action_kind(void)
{
    mm_marker_t m;

    if (!panel_selected_marker(&m))
        return;
    m.kind = (unsigned char)((m.kind + 1) % MM_MARKER_KIND_COUNT);
    if (mm_store_put(&m) == MM_MARKER_OK)
        mm_log("panel: mark %s is now kind %d", m.id, m.kind);
}

static void panel_action_delete(void)
{
    mm_marker_t m;

    if (!panel_selected_marker(&m))
        return;
    if (mm_store_remove(m.id) == MM_MARKER_OK) {
        mm_log("panel: deleted mark %s", m.id);
        g_selected_id[0] = '\0';
        mm_markers_set_selected(NULL);
    }
}

static void panel_action_name(void)
{
    mm_marker_t m;

    if (!panel_selected_marker(&m))
        return;
    if (!mm_store_writable()) {
        mm_log("panel: store is read-only; not renaming");
        return;
    }
    mm_edit_begin(m.id, m.label);
}

/* Selecting a marker row does exactly what selecting an NPC row does: write the
 * stock banner globals and, on a second click, walk. Markers are in the same
 * coordinate space as map characters, so the existing walk path takes them
 * unchanged. */
static void panel_select_marker(int row)
{
    if (!mm_addr[MM_MARKERX] || !mm_addr[MM_MARKERY])
        return;

    if (strcmp(g_selected_id, g_row_id[row]) == 0) {
        int rc = mm_walk_to_pixel(g_row_x[row], g_row_y[row]);
        mm_log("panel: walk to mark %s -> %d", g_row_id[row], rc);
        return;
    }

    g_delete_armed = 0;
    _snprintf(g_selected_id, sizeof(g_selected_id), "%s", g_row_id[row]);
    mm_markers_set_selected(g_selected_id);
    *(int *)mm_addr[MM_MARKERX] = g_row_x[row];
    *(int *)mm_addr[MM_MARKERY] = g_row_y[row];

    mm_log("panel: selected mark %s -> marker (%d,%d)",
           g_selected_id, g_row_x[row], g_row_y[row]);
}

static void panel_select(int row)
{
    if (!mm_addr[MM_MARKERX] || !mm_addr[MM_MARKERY])
        return;

    /* Second click on the row already selected: walk there. Marking and walking
     * are deliberately separate actions -- clicking a name to see where it is
     * should not commit the character to crossing the map. */
    if (g_row_slot[row] == g_selected) {
        int rc = mm_walk_to_pixel(g_row_x[row], g_row_y[row]);
        static const char *why[] = {
            "walking", "walking as far as the line goes", "blocked",
            "already there", "no map", "too soon", "routed around",
            "auto-walk disabled"
        };
        mm_log("panel: walk to slot %d -> %s",
               g_selected, rc >= 0 && rc <= MM_WALK_DISABLED ? why[rc] : "?");
        return;
    }

    /* The coordinates the row was drawn with, not a fresh read: what the player
     * clicked is what the marker gets. */
    g_selected = g_row_slot[row];
    *(int *)mm_addr[MM_MARKERX] = g_row_x[row];
    *(int *)mm_addr[MM_MARKERY] = g_row_y[row];

    mm_log("panel: selected slot %d -> marker (%d,%d)",
           g_selected, g_row_x[row], g_row_y[row]);
}

/* Mark the pending event handled.
 *
 * Each node's input proc is responsible for clearing this -- the stock minimap
 * proc does it at 0x4200C2 -- and only the node under the cursor is called.
 * Without this a single click stays pending across frames and repeats, which
 * for the scroll controls means one click running the list to the end.
 */
static void panel_consume_event(void)
{
    if (mm_addr[MM_EVENTCODE])
        *(unsigned char *)mm_addr[MM_EVENTCODE] = 0;
}

/* Destroying the panel has to take any pending click with it.
 *
 * While the node exists it absorbs clicks over its rect and clears the event
 * byte itself. The moment it is freed nothing is absorbing them any more, and a
 * click still latched in 0x7029BC is dispatched to the world at the same cursor
 * position -- which is wherever the player last clicked, i.e. on a row of the
 * list. That reads as a ground click: the character is sent to that spot, and
 * the SendPlayerMove hook sees a move the mod did not issue and cancels the
 * route. Clicking an NPC and then closing the map therefore cancelled the walk
 * it had just started.
 *
 * Clearing on the way out is safe: the interaction that closed the window is
 * finished, and no other node was going to receive that click anyway.
 */
static void panel_release_click(void)
{
    panel_consume_event();
    g_click_armed = 1;
    /* The event byte alone does not settle it -- the client re-asserts the
     * click after we clear it, so the resulting move has to be recognised on
     * the way out and not mistaken for the player steering. */
    mm_walk_keep_route_next_move();
}

/* Called by the node walker only when the mouse is inside our rect: the
 * dispatcher at 0x45E909 hit-tests the node's own bounds before calling, so
 * there is no need to re-check them here.
 *
 * Same non-re-entrancy rule as the draw proc -- no node may be created,
 * destroyed or relinked from in here.
 */
static void __cdecl panel_input(void)
{
    mm_rect_t r = {0, 0, 0, 0};
    int flag = 0, my, row;

    if (g_panel_handle < 0 || g_orphaned || mm_minimap_handle() < 0)
        return;
    if (!mm_addr[MM_EVENTCODE] || !mm_addr[MM_MOUSEY])
        return;

    /* 0xF5 is the click. 0xF2 is mouse-down and 0xF8 is the close button;
     * neither is ours to act on. */
    if (*(unsigned char *)mm_addr[MM_EVENTCODE] != 0xF5) {
        g_click_armed = 1;      /* button released; the next press counts */
        return;
    }

    /* One press must count once.
     *
     * Clearing the event byte is not enough: the client re-asserts 0xF5 every
     * frame while the button is held, which was confirmed by a 250 ms cooldown
     * turning a single press into a steady stream of clicks exactly 250 ms
     * apart. So this triggers on the edge instead -- act once, then wait until
     * a frame arrives with the event no longer set before arming again.
     */
    if (!g_click_armed) {
        /* A repeat of a click already handled. Consume it rather than leaving
         * it set: while the panel is up the click is harmless, because the
         * panel is under the cursor and absorbs it -- but it stays latched, and
         * a latched click is exactly what leaks to the world the moment the
         * panel is destroyed. Closing with Tab showed this, because unlike the
         * close button it is a keypress and so consumes nothing itself.
         *
         * A window should hold no input state it is not actively using. */
        panel_consume_event();
        return;
    }
    g_click_armed = 0;

    if (mm_node_rect(g_panel_handle, &r, &flag) != 0)
        return;

    my = *(int *)mm_addr[MM_MOUSEY] - r.top;

    /* A click while the field is open dismisses it rather than acting. Keeping
     * the field and the list both live invites a rename landing on whichever
     * marker the click happened to hit. */
    if (mm_edit_active()) {
        mm_edit_cancel();
        panel_consume_event();
        return;
    }

    /* The mode switch shares the header line. Left half is NPCs, right half is
     * Marks, matching the order they are drawn in. Switching resets the scroll
     * because the two lists have nothing to do with each other. */
    if (my >= MM_HEADER_DY && my < MM_HEADER_DY + MM_ROW_HEIGHT) {
        int mx = mm_addr[MM_MOUSEX] ? *(int *)mm_addr[MM_MOUSEX] : 0;
        int want = (mx < r.left + MM_PAD_X + 52) ? MM_MODE_NPCS : MM_MODE_MARKS;

        if (want != g_mode) {
            g_mode = want;
            g_scroll = 0;
            g_row_count = 0;
            mm_log("panel: switched to %s",
                   g_mode == MM_MODE_MARKS ? "Marks" : "NPCs");
        }
        panel_consume_event();
        return;
    }

    /* The Marks action row: Add, Move, Kind, Del across four equal columns.
     *
     * Any click that is not Del disarms the delete confirmation, so an armed
     * delete cannot survive the player doing something else first. */
    if (g_mode == MM_MODE_MARKS &&
        my >= MM_ACTION_DY && my < MM_ACTION_DY + MM_ROW_HEIGHT) {
        int mx = mm_addr[MM_MOUSEX] ? *(int *)mm_addr[MM_MOUSEX] : 0;
        int col = (mx - (r.left + MM_PAD_X)) /
                  ((MM_PANEL_WIDTH - 2 * MM_PAD_X) / MM_ACTION_COLS);

        if (col < 0)
            col = 0;
        if (col >= MM_ACTION_COLS)
            col = MM_ACTION_COLS - 1;

        if (col == 4) {
            if (g_delete_armed) {
                g_delete_armed = 0;
                panel_action_delete();
            } else if (g_selected_id[0] != '\0') {
                g_delete_armed = 1;
            }
        } else {
            g_delete_armed = 0;
            if (col == 0)
                panel_action_add();
            else if (col == 1)
                panel_action_move();
            else if (col == 2)
                panel_action_kind();
            else
                panel_action_name();
        }
        panel_consume_event();
        return;
    }

    /* The scroll controls, when there is anything to scroll. */
    if ((g_mode == MM_MODE_MARKS
             ? (g_row_count == MM_ROWS_VISIBLE || g_scroll > 0)
             : g_list_count > MM_ROWS_VISIBLE) &&
        my >= MM_FOOTER_DY && my < MM_FOOTER_DY + MM_ROW_HEIGHT) {
        int mx = mm_addr[MM_MOUSEX] ? *(int *)mm_addr[MM_MOUSEX] : 0;

        g_scroll += (mx < (r.left + r.right) / 2)
                    ? -MM_SCROLL_STEP : MM_SCROLL_STEP;
        /* NPCs clamp against a known total. Marks does not keep one here, so it
         * clamps by result: a scroll that lands past the end draws no rows, and
         * this pulls it back on the next click rather than stranding the list. */
        if (g_mode == MM_MODE_NPCS && g_scroll > g_list_count - MM_ROWS_VISIBLE)
            g_scroll = g_list_count - MM_ROWS_VISIBLE;
        if (g_mode == MM_MODE_MARKS && g_row_count < MM_ROWS_VISIBLE)
            g_scroll -= MM_SCROLL_STEP;
        if (g_scroll < 0)
            g_scroll = 0;
        panel_consume_event();
        return;
    }

    row = (my - MM_ROWS_DY) / MM_ROW_HEIGHT;
    if (my < MM_ROWS_DY || row < 0 || row >= g_row_count)
        return;

    if (g_mode == MM_MODE_MARKS)
        panel_select_marker(row);
    else
        panel_select(row);
    panel_consume_event();
}

static int __cdecl uicreate_detour(int left, int top, int right, int bottom,
                                   void *draw_proc, void *input_proc,
                                   int arg4, int list_sel)
{
    int handle;

    if (left == MM_STOCK_LEFT && top == MM_STOCK_TOP &&
        right == MM_STOCK_RIGHT && bottom == MM_STOCK_BOTTOM) {
        left  -= MM_MAP_SHIFT;
        right -= MM_MAP_SHIFT;

        g_panel[0] = right;
        g_panel[1] = top;
        g_panel[2] = right + MM_PANEL_WIDTH;
        g_panel[3] = bottom;
    } else {
        /* Not the rect we know. Say so once and pass it through untouched -- a
         * client whose minimap has moved should look stock, not wrong. */
        static int warned;
        if (!warned) {
            warned = 1;
            mm_log("layout: unexpected minimap rect (%d,%d)-(%d,%d), leaving it alone",
                   left, top, right, bottom);
        }
    }

    g_map.left = left;
    g_map.top = top;
    g_map.right = right;
    g_map.bottom = bottom;

    handle = g_orig_uicreate(left, top, right, bottom,
                             draw_proc, input_proc, arg4, list_sel);

    mm_log("layout: minimap node %d at (%d,%d)-(%d,%d); panel slot (%d,%d)-(%d,%d)",
           handle, left, top, right, bottom,
           g_panel[0], g_panel[1], g_panel[2], g_panel[3]);

    /* Created after the map so it sits later in the node order and draws on
     * top. Safe here: we are inside OpenMinimap, not inside a node walk. */
    /* A panel left behind by a close path that bypassed our detour. We are
     * inside OpenMinimap here, not a node walk, so it is safe to free. */
    if (g_panel_handle >= 0 && g_orphaned) {
        g_orig_uidestroy(g_panel_handle, list_sel);
        mm_log("panel: freed orphaned node %d", g_panel_handle);
        g_panel_handle = -1;
        g_orphaned = 0;
        panel_release_click();
    }

    if (handle >= 0 && g_panel[2] && g_panel_handle < 0) {
        g_selected = -1;
        g_row_count = 0;
        g_scroll = 0;
        g_orphaned = 0;
        g_panel_handle = g_orig_uicreate(g_panel[0], g_panel[1],
                                         g_panel[2], g_panel[3],
                                         (void *)panel_draw, (void *)panel_input,
                                         arg4, list_sel);
        mm_log("panel: node %d created at (%d,%d)-(%d,%d)", g_panel_handle,
               g_panel[0], g_panel[1], g_panel[2], g_panel[3]);
    }
    return handle;
}

/* The close half of the toggle. Tear the panel down with the map, and before
 * it -- the client destroys in call order and our node was created last. */
static void __cdecl uidestroy_detour(int handle, int list_sel)
{
    if (g_panel_handle >= 0) {
        /* Same list the panel was created in -- the minimap passes 0 for both
         * trailing arguments and the panel was created alongside it. */
        g_orig_uidestroy(g_panel_handle, list_sel);
        mm_log("panel: destroyed node %d (list %d)", g_panel_handle, list_sel);
        g_panel_handle = -1;
        g_orphaned = 0;
        panel_release_click();
    }
    g_orig_uidestroy(handle, list_sel);
}

/* Free the panel if the minimap has gone without our destroy hook running.
 *
 * This must NOT live in panel_draw, which is where it used to. A UI node
 * swallows clicks inside its rect whether or not its draw proc paints
 * anything, and if the client stops calling that proc once the map closes then
 * the cleanup never runs -- leaving an invisible node eating every click in a
 * 200x304 block of screen. Which is exactly what was reported: a "sticky"
 * rectangle the size of the panel.
 *
 * The lesson is the general one: cleanup cannot be driven by the thing whose
 * absence it is cleaning up after.
 *
 * Called from core's post-UiTick service AFTER the client's own pass, so the
 * node walk has finished and destroying a node is safe here -- unlike inside a
 * draw or input proc, where it would corrupt the walker's global cursor.
 */
/* One frame of label editing, driven from the post-tick rather than a draw or
 * input proc: draining the typed-character queue is state, not rendering, and
 * it has to happen exactly once a frame whether or not anything was drawn. */
void mm_panel_edit_tick(void)
{
    mm_marker_t m;
    int cancelled = 0;

    if (!mm_edit_active())
        return;

    /* The field cannot outlive its own window. */
    if (g_panel_handle < 0 || g_orphaned || mm_minimap_handle() < 0) {
        mm_edit_cancel();
        return;
    }

    if (!mm_edit_tick(&cancelled)) {
        if (cancelled)
            mm_log("panel: rename cancelled");
        return;
    }

    /* Enter: re-read the marker rather than trusting a copy taken when the
     * field opened, since the store may have reloaded since. */
    if (panel_selected_marker(&m) &&
        strcmp(m.id, mm_edit_target()) == 0) {
        _snprintf(m.label, sizeof(m.label), "%s", mm_edit_text());
        m.label[sizeof(m.label) - 1] = 0;
        if (mm_store_put(&m) == MM_MARKER_OK)
            mm_log("panel: renamed mark %s", m.id);
        else
            mm_log("panel: rename refused for %s", m.id);
    } else {
        mm_log("panel: the marker being renamed is gone; discarded");
    }
    mm_edit_cancel();
}

void mm_panel_idle(void)
{
    if (g_panel_handle < 0)
        return;
    if (mm_minimap_handle() >= 0)
        return;     /* map still open, panel belongs on screen */

    if (g_orig_uidestroy) {
        g_orig_uidestroy(g_panel_handle, 0);
        mm_log("panel: minimap closed without our destroy hook; freed node %d",
               g_panel_handle);
        g_panel_handle = -1;
        g_orphaned = 0;
        panel_release_click();
        return;
    }

    /* No trampoline to destroy through. Collapsing the rect at least stops it
     * intercepting clicks until the next open frees it. */
    if (!g_orphaned) {
        mm_rect_t empty = {0, 0, 0, 0};
        g_orphaned = 1;
        mm_node_set_rect(g_panel_handle, &empty, 0);
        mm_log("panel: minimap closed; panel collapsed (node %d still allocated)",
               g_panel_handle);
    }
}

int mm_panel_install(void)
{
    void *site = mm_addr[MM_UICREATECALL];

    if (!site) {
        mm_log("layout: UiCreateCall unresolved, no layout change");
        return 0;
    }

    /* Lifecycle owns claims for both five-byte call sites before entering here.
     * mm_patch_direct_call also verifies each site is `call rel32`, so a
     * signature that matched the wrong place cannot turn into a wild branch. */
    if (!mm_patch_direct_call(&g_call_hook, site, (void *)uicreate_detour,
                              (void **)&g_orig_uicreate)) {
        mm_log("layout: could not repoint the UiCreateNode call");
        return 0;
    }

    /* The call site should reach the function our own signature found. If it
     * does not, one of the two signatures is on the wrong thing and we should
     * not be redirecting anything. */
    if (g_orig_uicreate != (mm_uicreate_fn)mm_addr[MM_UICREATENODE]) {
        mm_log("layout: call site reaches %p but UiCreateNode resolved to %p"
               " -- backing out",
               (void *)g_orig_uicreate, (void *)mm_addr[MM_UICREATENODE]);
        mm_hook_remove(&g_call_hook);
        g_orig_uicreate = NULL;
        return 0;
    }

    /* The close half. Without it the panel would outlive the map and be left
     * orphaned on screen, so a failure here backs the whole thing out rather
     * than shipping half a toggle. */
    if (!mm_patch_direct_call(&g_destroy_hook,
                              (unsigned char *)site +
                                  MM_PANEL_DESTROY_CALL_OFFSET,
                              (void *)uidestroy_detour,
                              (void **)&g_orig_uidestroy)) {
        mm_log("layout: could not repoint the destroy call -- backing out");
        mm_hook_remove(&g_call_hook);
        g_orig_uicreate = NULL;
        return 0;
    }

    mm_log("layout: minimap will open %dpx left, panel %dpx wide",
           MM_MAP_SHIFT, MM_PANEL_WIDTH);
    return 1;
}

void mm_panel_remove(void)
{
    /* Destroy our node before unpatching: once the detour is gone nothing else
     * knows the panel exists, and it would be left on screen. */
    if (g_panel_handle >= 0 && g_orig_uidestroy) {
        g_orig_uidestroy(g_panel_handle, 0);
        g_panel_handle = -1;
    }
    if (g_destroy_hook.active) {
        mm_hook_remove(&g_destroy_hook);
        g_orig_uidestroy = NULL;
    }
    if (g_call_hook.active) {
        mm_hook_remove(&g_call_hook);
        g_orig_uicreate = NULL;
    }
    ZeroMemory(g_panel, sizeof(g_panel));
    g_panel_handle = -1;
    g_row_count = 0;
    g_list_count = 0;
    g_scroll = 0;
    g_selected = -1;
    g_orphaned = 0;
    g_click_armed = 1;
}
