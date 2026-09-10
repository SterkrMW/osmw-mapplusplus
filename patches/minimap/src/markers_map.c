/* Persistent map markers: map identity and the map-unit -> screen transform.
 *
 * Spec: mw-tools/tools/client-dll/map-markers.md sections 2 and 3.
 *
 * Two things the marker layer needs before it can draw anything:
 *
 *   which map this is    the client formats "MAPnnn.MAP" into a buffer at map
 *                        load, so the persistence key is a parse, not a packet
 *                        observer. The minimap *image* index next to it is a
 *                        different number and must not be used as the key.
 *
 *   where a marker goes  the client keeps a fixed 400x300 minimap surface and
 *                        stretches whatever art a map ships into it, so the
 *                        same two scale globals work for every map whether its
 *                        art is 128x96, 160x120 or already 400x300. The origin
 *                        is the node's own rect, not the stock 196/148
 *                        immediates -- the panel shifts the map 100px left.
 *
 * mm_markers_verify() exists to prove the transform in the live client without
 * drawing anything: it projects the local player's own map position, which the
 * client independently draws as the player indicator, so the two can be
 * compared by eye against one log line.
 */
#include "osmw_minimap.h"

#include <stdio.h>
#include <string.h>

/* The client's minimap image is composited into a fixed-size surface. */
#define MM_MAP_IMAGE_W 400
#define MM_MAP_IMAGE_H 300

/* Border between the node rect and the image, read off the stock draw proc:
 * x is a flat 4, y is whatever centring the node height leaves over. */
#define MM_MAP_BORDER_X 4

/* ---- map identity -------------------------------------------------------- */

/* "MAPnnn.MAP" -> nnn, or -1 while no map is loaded. Deliberately strict: a
 * buffer that does not look exactly like the client's own format is a sign we
 * are reading the wrong global, and guessing would key markers to nonsense. */
int mm_markers_map_file(void)
{
    const char *s;
    int value = 0, digits = 0;

    if (mm_addr[MM_MAPFILENAME] == NULL)
        return -1;

    s = (const char *)mm_addr[MM_MAPFILENAME];
    if (IsBadStringPtrA(s, 16))
        return -1;
    if (s[0] != 'M' || s[1] != 'A' || s[2] != 'P')
        return -1;

    s += 3;
    while (*s >= '0' && *s <= '9' && digits < 5) {
        value = value * 10 + (*s - '0');
        s++;
        digits++;
    }
    if (digits == 0 || value > 0xFFFF)
        return -1;
    /* The extension is what separates "MAP037.MAP" from a truncated read. */
    if (s[0] != '.')
        return -1;
    return value;
}

/* ---- transform ----------------------------------------------------------- */

int mm_markers_map_extent(int *width, int *height)
{
    int w, h;

    if (mm_addr[MM_MAPUNITSW] == NULL || mm_addr[MM_MAPUNITSH] == NULL)
        return 0;
    w = *(int *)mm_addr[MM_MAPUNITSW];
    h = *(int *)mm_addr[MM_MAPUNITSH];
    if (w <= 0 || h <= 0)
        return 0;
    if (width)  *width  = w;
    if (height) *height = h;
    return 1;
}

int mm_markers_in_bounds(unsigned x, unsigned y)
{
    int w = 0, h = 0;
    if (!mm_markers_map_extent(&w, &h))
        return 0;
    return (int)x >= 0 && (int)x < w && (int)y >= 0 && (int)y < h;
}

/* Map units -> screen pixels, the same arithmetic the stock banner uses.
 *
 * The scales are floats the client recomputes on every map load, so they are
 * read per call rather than cached: a cached scale would be silently wrong for
 * one frame after a map change, which is exactly when a marker is most likely
 * to be drawn in the wrong place.
 */
int mm_markers_project(unsigned x, unsigned y, int *screen_x, int *screen_y)
{
    mm_rect_t map;
    float sx, sy;
    int origin_x, origin_y;

    if (mm_addr[MM_SCALEX] == NULL || mm_addr[MM_SCALEY] == NULL)
        return 0;
    if (!mm_map_rect(&map))
        return 0;
    if (!mm_markers_in_bounds(x, y))
        return 0;

    sx = *(float *)mm_addr[MM_SCALEX];
    sy = *(float *)mm_addr[MM_SCALEY];
    if (sx <= 0.0f || sy <= 0.0f)
        return 0;

    origin_x = map.left + MM_MAP_BORDER_X;
    origin_y = map.top + ((map.bottom - map.top) - MM_MAP_IMAGE_H) / 2;

    if (screen_x) *screen_x = (int)((float)x * sx) + origin_x;
    if (screen_y) *screen_y = (int)((float)y * sy) + origin_y;
    return 1;
}

/* ---- live verification --------------------------------------------------- */

/* ---- the marker sprite bank ---------------------------------------------- */

/* The five glyphs live in the module's own MDA rather than in the stock
 * system\icon.mda: that file is shared art with per-theme copies under
 * defaultGui/greenGui/purpleGui, and editing it would drift them apart. Loading
 * our own file through the client's own loader costs one handle and keeps a
 * missing asset to a disabled layer instead of a broken client.
 */
#define MM_MARKER_MDA_PATH "system\\osmw_markers.mda"

/* Blit flags. The stock banner passes 0x12 where the player indicator passes 7;
 * the argument is a bitmask the blit decomposes, not a frame index, so start
 * from the banner's value. */
#define MM_MARKER_BLIT_FLAGS 0x12

typedef int (__cdecl *mm_loadmda_fn)(const char *path, void **out_handle);
typedef int (__cdecl *mm_blitmda_fn)(int handle, const char *anim,
                                     int x, int y, void *surface,
                                     int flags, int a, int b, int c);

static void *g_marker_mda = NULL;
static int   g_marker_mda_tried = 0;

static const char *const MM_MARKER_ANIM[MM_MARKER_KIND_COUNT] = {
    "MarkerNpc", "MarkerShop", "MarkerPortal", "MarkerQuest", "MarkerNote"
};

/* Loaded once, lazily: the loader is only safe to call once the client's own
 * asset system is up, which it is by the time anything draws. */
static int marker_bank(void)
{
    mm_loadmda_fn load;

    if (g_marker_mda_tried)
        return g_marker_mda != NULL;
    g_marker_mda_tried = 1;

    if (mm_addr[MM_LOADMDA] == NULL) {
        mm_log("markers: no MDA loader; marker layer disabled");
        return 0;
    }

    load = (mm_loadmda_fn)mm_addr[MM_LOADMDA];
    load(MM_MARKER_MDA_PATH, &g_marker_mda);
    if (g_marker_mda == NULL) {
        mm_log("markers: %s did not load; marker layer disabled",
               MM_MARKER_MDA_PATH);
        return 0;
    }
    mm_log("markers: %s loaded, handle %p", MM_MARKER_MDA_PATH, g_marker_mda);
    return 1;
}

/* One glyph at one screen position. The sprite carries its own centre, so the
 * caller passes the point the marker sits on and does no offset arithmetic --
 * which is exactly what the text-based spike could not do. */
int mm_markers_blit(int kind, int screen_x, int screen_y)
{
    mm_blitmda_fn blit;

    if (kind < 0 || kind >= MM_MARKER_KIND_COUNT)
        return 0;
    if (mm_addr[MM_BLITMDA] == NULL || mm_addr[MM_RENDERSURFACE] == NULL)
        return 0;
    if (!marker_bank())
        return 0;

    /* The return value matters. BlitMdaFrame's first act is to SEARCH the
     * client's MDA table (`[0x74308C]` entries of 0x144 bytes from
     * `[0x743B1C]`) for an entry whose first dword equals the handle, and it
     * returns 0 without drawing if it does not find one -- or if the animation
     * name is not in that MDA. Both are silent, which is exactly the failure
     * to surface rather than swallow. */
    blit = (mm_blitmda_fn)mm_addr[MM_BLITMDA];
    return blit((int)g_marker_mda, MM_MARKER_ANIM[kind], screen_x, screen_y,
                *(void **)mm_addr[MM_RENDERSURFACE],
                MM_MARKER_BLIT_FLAGS, 0, 0, 0) != 0;
}

/* ---- the marker layer ---------------------------------------------------- */

/* Label modes. Hover is the default because a map full of always-on labels
 * covers the terrain the map exists to show. */
enum { MM_LABELS_OFF = 0, MM_LABELS_HOVER, MM_LABELS_ALWAYS };

static int label_mode(void)
{
    char value[16];

    mm_cfg_string("MarkerLabels", "hover", value, sizeof(value));
    if (value[0] == 'a' || value[0] == 'A')
        return MM_LABELS_ALWAYS;
    if (value[0] == 'o' || value[0] == 'O')
        return MM_LABELS_OFF;
    return MM_LABELS_HOVER;
}

/* A marker is "hovered" when the cursor is inside the glyph's footprint. The
 * sprite is 16x20 anchored bottom-centre, so its box runs half a width either
 * side of the point and a full height above it. */
#define MM_GLYPH_W 16
#define MM_GLYPH_H 20

static int cursor_over(int screen_x, int screen_y)
{
    int mx, my;

    if (mm_addr[MM_MOUSEX] == NULL || mm_addr[MM_MOUSEY] == NULL)
        return 0;
    mx = *(int *)mm_addr[MM_MOUSEX];
    my = *(int *)mm_addr[MM_MOUSEY];
    return mx >= screen_x - MM_GLYPH_W / 2 && mx <= screen_x + MM_GLYPH_W / 2 &&
           my >= screen_y - MM_GLYPH_H && my <= screen_y;
}

static const char *g_selected_id;

void mm_markers_set_selected(const char *id)
{
    g_selected_id = id;
}

/* Draw every visible marker for the current map.
 *
 * Culling is by map first and by node rect second: a marker whose map does not
 * match is not this map's business, and one that projects outside the window
 * would otherwise be drawn over the panel or the playfield. The bounds check
 * inside mm_markers_project already refuses coordinates outside the map, which
 * is what keeps a record written by a newer or buggier writer from painting
 * somewhere absurd.
 */
void mm_markers_draw_layer(mm_marker_text_fn draw_text)
{
    const mm_marker_set_t *set = NULL;
    mm_rect_t map = {0, 0, 0, 0};
    int map_file, mode, i;

    /* Diagnostic: report the first frame the layer stops drawing and why.
     * Rate-limited to transitions, so a healthy layer is silent. */
    static int last_state = -1;
    int state = 0;

    if (!mm_cfg_int("MarkerLayer", 1))
        state = 1;
    else if (!mm_client_in_world())
        state = 2;
    else if (!mm_map_rect(&map))
        state = 3;
    else if (mm_markers_map_file() < 0)
        state = 4;
    else if ((set = mm_store_get()) == NULL || set->count == 0)
        state = 5;
    else if (mm_addr[MM_BLITMDA] == NULL || mm_addr[MM_RENDERSURFACE] == NULL)
        state = 6;
    else if (*(void **)mm_addr[MM_RENDERSURFACE] == NULL)
        state = 7;
    else if (!marker_bank())
        state = 8;

    if (state != last_state) {
        static const char *why[] = {
            "drawing", "MarkerLayer=0", "not in world", "no map rect",
            "no map file", "store empty", "blit unresolved",
            "render surface null", "asset not loaded"
        };
        last_state = state;
        mm_log("markers: layer %s", why[state]);
    }
    if (state != 0)
        return;

    map_file = mm_markers_map_file();
    mode = label_mode();

    for (i = 0; i < set->count; i++) {
        const mm_marker_t *m = &set->items[i];
        int sx = 0, sy = 0, selected, hovered;

        if ((int)m->map_file != map_file)
            continue;
        if (!(m->flags & MM_MARKER_FLAG_VISIBLE))
            continue;
        if (!mm_markers_project(m->x, m->y, &sx, &sy))
            continue;
        if (sx < map.left || sx > map.right || sy < map.top || sy > map.bottom)
            continue;

        if (!mm_markers_blit(m->kind, sx, sy)) {
            /* Report the first frame the blit starts refusing, and keep going:
             * one glyph failing is not a reason to drop the rest, and a silent
             * bail here is what hid this in the first place. */
            static int blit_failing;
            if (!blit_failing) {
                blit_failing = 1;
                mm_log("markers: blit refused kind %d handle %p "
                       "(MDA table %d entries) -- glyph not drawn",
                       m->kind, g_marker_mda,
                       mm_addr[MM_MDACOUNT] ? *(int *)mm_addr[MM_MDACOUNT] : -1);
            }
            continue;
        }

        selected = g_selected_id != NULL && strcmp(g_selected_id, m->id) == 0;
        hovered = cursor_over(sx, sy);

        /* The label sits above the glyph, which is why the sprite anchors at
         * its base: text and sprite share one reference point. */
        if (m->label[0] != '\0' && draw_text != NULL &&
            (mode == MM_LABELS_ALWAYS || selected ||
             (mode == MM_LABELS_HOVER && hovered)))
            draw_text(sx - MM_GLYPH_W / 2, sy - MM_GLYPH_H - 12, m->label);
    }
}

/* One line per map, comparing our projection of the local player's position
 * against the indicator the client draws from the same numbers. If the two
 * agree on screen, sections 2 and 3 of the spec are correct on this map.
 *
 * Logged rather than drawn on purpose: this runs before any marker layer
 * exists, so it cannot disturb the stock minimap.
 */
void mm_markers_verify(void)
{
    /* Two separate once-per-map reports, because the two facts become
     * available at different times: the extent globals land a tick or two
     * after the map name, and the node rect only exists once the player has
     * opened the minimap at all. Latching both on the first of them is what
     * made the earlier build report a map and then go quiet. */
    static int extent_map = -2;
    static int projected_map = -2;
    unsigned char *slot;
    int map_file, w = 0, h = 0, sx = 0, sy = 0;
    unsigned px, py;
    mm_rect_t map;

    map_file = mm_markers_map_file();
    if (map_file < 0)
        return;

    if (map_file != extent_map && mm_markers_map_extent(&w, &h)) {
        extent_map = map_file;
        mm_log("markers: MAP%03d.MAP, %d x %d map units, "
               "scale %d/1000 x %d/1000",
               map_file, w, h,
               mm_addr[MM_SCALEX] ?
                   (int)(*(float *)mm_addr[MM_SCALEX] * 1000.0f) : 0,
               mm_addr[MM_SCALEY] ?
                   (int)(*(float *)mm_addr[MM_SCALEY] * 1000.0f) : 0);
    }

    if (map_file == projected_map)
        return;
    if (!mm_map_rect(&map))
        return;                          /* minimap never opened yet; retry */

    slot = mm_map_char_slot(mm_local_slot_index());
    if (slot == NULL)
        return;
    px = *(unsigned short *)(slot + MM_SLOT_X);
    py = *(unsigned short *)(slot + MM_SLOT_Y);

    if (!mm_markers_project(px, py, &sx, &sy)) {
        mm_log("markers: player at map unit (%u,%u) did not project", px, py);
        projected_map = map_file;
        return;
    }

    projected_map = map_file;
    mm_log("markers: player at map unit (%u,%u) projects to screen (%d,%d) "
           "in node (%d,%d)-(%d,%d) -- compare with the player indicator",
           px, py, sx, sy, map.left, map.top, map.right, map.bottom);
}

/* ---- drawing inside the map's own pass ----------------------------------- */

/* The layer was first drawn from the panel's draw proc, on the assumption that
 * the draw context is screen-wide. It is -- the glyphs appeared -- but the
 * panel is a separate node, and node draw order is creation order. Left-clicking
 * the map put the minimap's content back over the top of glyphs the panel had
 * already drawn, and they stayed hidden until the pair was recreated. The blit
 * was succeeding the whole time, which is why nothing was logged.
 *
 * Markers belong to the map window, so they are drawn from the map's own draw
 * proc, immediately after the stock content. That removes the ordering question
 * rather than answering it.
 */
static mm_hook_t g_draw_hook;
static void (__cdecl *g_orig_draw)(void);
static mm_marker_text_fn g_layer_text;

static void __cdecl markers_draw_detour(void)
{
    g_orig_draw();
    mm_markers_draw_layer(g_layer_text);
}

int mm_markers_install_draw(mm_marker_text_fn draw_text)
{
    g_layer_text = draw_text;

    if (mm_addr[MM_DRAWPROC] == NULL)
        return 0;
    if (!mm_hook_install(&g_draw_hook, mm_addr[MM_DRAWPROC],
                         (void *)markers_draw_detour,
                         MM_MARKER_DRAWPROC_PROLOGUE)) {
        mm_log("markers: could not hook the minimap draw proc; "
               "markers will not be drawn");
        return 0;
    }
    g_orig_draw = (void (__cdecl *)(void))g_draw_hook.trampoline;
    return 1;
}

void mm_markers_remove_draw(void)
{
    if (g_draw_hook.active) {
        mm_hook_remove(&g_draw_hook);
        g_orig_draw = NULL;
    }
    g_layer_text = NULL;
}
