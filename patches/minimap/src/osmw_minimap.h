/* osmw_minimap -- core-managed minimap NPC list and auto-walk for the MythWar
 * (osMW) client.
 *
 * Core owns lifecycle, the shared UiTick detour, pristine-image signature
 * resolution and runtime patch claims. Minimap retains only its specialised
 * panel call-site redirects and SendPlayerMove detour.
 *
 * Spec: mw-tools/tools/client-dll/minimap-autowalk.md
 */
#ifndef OSMW_MINIMAP_H
#define OSMW_MINIMAP_H

#include <windows.h>
#include "osmw_core_api.h"
#include "sigs.h"

/* ---- managed core binding ---------------------------------------------- */

/* Valid from module init until shutdown returns. */
const OSMW_CORE_API_V1 *mm_core_api(void);
OSMW_OWNER mm_core_owner(void);

/* ---- resolved target addresses ------------------------------------------ */

/* Absolute addresses in the host process, indexed by the MM_* enum in sigs.h.
 * NULL means that signature did not resolve. */
extern unsigned char *mm_addr[MM_ADDR_COUNT];

/* ---- log.c --------------------------------------------------------------- */

void mm_log(const char *fmt, ...);

/* ---- config.c ------------------------------------------------------------ */

const char *mm_game_dir(void);
int         mm_cfg_int(const char *key, int fallback);
int         mm_cfg_int_in(const char *section, const char *key, int fallback);
void        mm_cfg_string(const char *key, const char *fallback,
                         char *out, unsigned size);

/* ---- scan.c -------------------------------------------------------------- */

/* Resolve every module-owned signature through core's pristine executable
 * image. Optional misses leave mm_addr[] NULL. */
OSMW_RESULT mm_resolve_all(void);
void mm_resolve_reset(void);

/* ---- hook.c -------------------------------------------------------------- */

#define MM_MAX_PROLOGUE 24
#define MM_DIRECT_CALL_PATCH_LENGTH 5
#define MM_PANEL_DESTROY_CALL_OFFSET 17
#define MM_SENDPLAYERMOVE_PROLOGUE_LENGTH 7
/* Minimap draw proc prologue: `A1 A0 56 5E 00` = `mov eax,[0x5E56A0]`, exactly
 * five bytes and exactly the size of the jmp that replaces it.
 *
 * It must be 5, not 6: the next instruction is `83 EC 30` (`sub esp,0x30`), so
 * a six-byte copy takes `83` without `EC 30` and the trampoline then executes a
 * truncated instruction. That crashed the client the moment the minimap opened.
 * No relative branch is involved, so the copy is position-independent. */
#define MM_MARKER_DRAWPROC_PROLOGUE 5

typedef struct {
    void          *target;
    void          *trampoline;
    unsigned char  saved[MM_MAX_PROLOGUE];
    int            len;
    int            active;
} mm_hook_t;

int  mm_hook_install(mm_hook_t *h, void *target, void *detour, int prologue_len);
void mm_hook_remove(mm_hook_t *h);
int  mm_patch_call(mm_hook_t *h, void *site, void *detour);
int  mm_patch_direct_call(mm_hook_t *h, void *site, void *detour, void **original);

/* ---- map characters ------------------------------------------------------ */

/* Slot layout, from the NpcList loader at 0x46DC00. The first 84 bytes are the
 * server's MapCharacterStruct; the slot itself is longer. */
#define MM_SLOT_STRIDE   0xCC
#define MM_SLOT_ID       0x00   /* int32; NPC ids are negative */
#define MM_SLOT_NAME     0x04   /* 14-byte C string */
#define MM_SLOT_X        0x3A   /* uint16, map pixels */
#define MM_SLOT_Y        0x3C   /* uint16, map pixels */
#define MM_SLOT_TYPE     0x58   /* 0 player, 1 NPC, 2 other */

#define MM_TYPE_PLAYER   0
#define MM_TYPE_NPC      1

/* The array base is reached THROUGH mm_addr[MM_MAPCHARBASE]: that address
 * holds a pointer to the array, it is not the array. The client reallocates
 * it, so re-read on every use rather than caching the result.
 *
 * Returns NULL when the array is not currently allocated. */
unsigned char *mm_map_char_slot(int index);
int            mm_map_char_count(void);
int            mm_local_slot_index(void);

/* Map pixels to grid cells, using the scales the .map loader wrote rather than
 * assuming 16/8. Returns 0 and leaves the outputs alone if the scales are not
 * resolved or are zero. */
int mm_pixel_to_grid(int pixel_x, int pixel_y, int *grid_x, int *grid_y);

/* ---- minimap state ------------------------------------------------------- */

/* The stock minimap node handle; < 0 means closed. */
int mm_minimap_handle(void);

/* Whether this map has a minimap at all -- the same gates Tab, macro 88 and
 * HUD widget 0x13 check. The list panel must respect them. */
int mm_minimap_enabled(void);

/* ---- UI nodes ------------------------------------------------------------ */

/* UiCreateNode (0x45E5A0) mallocs a 0x2C-byte node and links it into one of two
 * circular lists, chosen by its last argument:
 *
 *   +0x00 next        +0x10 left    +0x20 draw proc
 *   +0x04 prev        +0x14 top     +0x24 input proc
 *   +0x08 handle      +0x18 right   +0x28 flag
 *   +0x0C arg4        +0x1C bottom
 *
 * Read the rect through the accessor rather than the offsets: 0x45E6F0 does
 * the handle lookup and hands back all four at once.
 */
typedef struct {
    int left, top, right, bottom;
} mm_rect_t;

/* 0x45E6F0. Returns 0 on success, -2 when no node has that handle.
 *
 * `out_rect` MUST be a full 16-byte rect: the callee writes four dwords
 * through it unconditionally. Passing anything smaller overruns the caller's
 * frame -- a single int here smashed a return address and took the render loop
 * down with it. */
int mm_node_rect(int handle, mm_rect_t *out_rect, int *out_flag);

/* 0x45E750. Writes the node's rect and flag in place. Unlike creating or
 * destroying a node this does not relink anything, so it is the one geometry
 * change that is safe to make from inside a draw or input proc. */
int mm_node_set_rect(int handle, const mm_rect_t *rect, int flag);

/* ---- grid.c -------------------------------------------------------------- */

/* The client's own collision grid. Dimensions in cells; 0 when no map is
 * loaded, which is also when the cell buffer is null. */
int mm_grid_dims(int *width, int *height);

/* 1 when the cell can be walked. Mirrors the client's CellWalkable (0x461270),
 * including its bounds check -- and adds the negative-coordinate check that
 * function omits. */
int mm_cell_walkable(int gx, int gy);

/* Whether the cell is a map transition (0x4000). Stepping onto one changes map,
 * so a blocked-start recovery must not choose one. */
int mm_cell_is_link(int gx, int gy);

/* Straight line between two cells, testing each one crossed. Returns 1 when the
 * whole line is clear; on a blocked line returns 0 and reports the last cell
 * reached in *stop_x / *stop_y. Port of the server's walkGridLine. */
int mm_walk_line(int sx, int sy, int ex, int ey, int *stop_x, int *stop_y);

/* Summarise the collision grid: dimensions, blocked share, bits in use. */
void mm_grid_dump(int cx, int cy);

/* Write the raw 16-bit grid beside main.exe for offline comparison with the
 * server's cells/<map>.bin. Opt-in via [Debug] DumpGrid. */
void mm_grid_write(void);

/* Detect a map change and report the new grid. Call once a frame. */
void mm_grid_poll(void);
void mm_grid_reset(void);

/* ---- pathfind.c ----------------------------------------------------------- */

/* A grid cell. short is plenty: the largest map seen is 500x750 cells. */
typedef struct {
    short x, y;
} mm_cell_t;

/* A* from start to end over the client's collision grid. Writes the route into
 * `out` in start-to-end order and returns its length in cells, or 0 when there
 * is no route, the endpoints are unusable, or the route is longer than
 * `max_out`.
 *
 * Scratch is allocated lazily, sized to the live grid, and kept until the map
 * changes -- about 3 MB on a 500x750 map. */
int mm_find_path(int sx, int sy, int ex, int ey, mm_cell_t *out, int max_out);

/* Collapse a cell route to the fewest straight legs that cover it, each leg
 * being as far as mm_walk_line can actually reach. Returns the leg count. */
int mm_simplify_path(const mm_cell_t *cells, int count, mm_cell_t *out, int max_out);

/* Nearest walkable cell to (x,y), searched outwards up to four rings. Returns 0
 * when nothing walkable is near. The character can come to rest on a cell this
 * grid calls blocked, because the client's own collision is finer. */
int mm_nearest_walkable(int x, int y, int *out_x, int *out_y);

/* Drop the search scratch. Called when the map changes. */
void mm_path_release(void);

/* ---- walk.c -------------------------------------------------------------- */

#define MM_WALK_CLEAR      0   /* whole line walkable, moving to the destination */
#define MM_WALK_TRUNCATED  1   /* moving as far as the line goes, then stopping */
#define MM_WALK_BLOCKED    2   /* the first step is blocked; not moving */
#define MM_WALK_ARRIVED    3   /* already in the destination cell */
#define MM_WALK_NO_MAP     4   /* no map loaded, or the local slot is unknown */
#define MM_WALK_THROTTLED  5   /* too soon after the last move packet */
#define MM_WALK_ROUTED     6   /* routed around an obstruction; legs queued */
#define MM_WALK_DISABLED   7   /* [Minimap] AutoWalk=0 */

/* Install only the uniquely claimed SendPlayerMove detour when enabled. Core's
 * post-UiTick service keeps routes moving with the minimap closed. */
int  mm_walk_install(int enabled);
void mm_walk_remove(void);

/* Advance the leg queue once from the core post-UiTick callback. */
void mm_walk_tick(void);

/* Abandon the current route. `why` is logged. */
void mm_walk_stop(const char *why);

/* Do not treat the next move packet as the player taking over, briefly. Called
 * when the panel is torn down: the click it was holding gets re-dispatched to
 * the world once it is gone, and that was never the player steering. The move
 * itself is still forwarded -- the client has already acted on it, so the server
 * must be told. */
void mm_walk_keep_route_next_move(void);

/* Whether a multi-leg route is in progress. */
int mm_walk_active(void);

/* Walk towards a map-pixel destination. One PlayerMove: the client walks the
 * straight line itself, so nothing needs to drive it step by step. */
int mm_walk_to_pixel(int dest_px, int dest_py);

/* ---- panel.c ------------------------------------------------------------- */

/* Repoint OpenMinimap's UiCreateNode call so the stock map opens 100px left,
 * clearing 200px on the right for the list panel. Install once, after the
 * signatures resolve and before the player first opens the map. */
int  mm_panel_install(void);
void mm_panel_remove(void);

/* Where the panel goes, once the map has been opened at least once. Returns 0
 * before that. */
int mm_panel_rect(int *left, int *top, int *right, int *bottom);

/* The minimap window's own rect as it was actually created, which is 100px
 * left of the stock immediates once the panel is in play. Zero until the map
 * has been opened once. */
int mm_map_rect(mm_rect_t *out);

/* Free the panel if the minimap closed by a path that bypassed our destroy
 * hook. Must be called from outside any node walk -- the UI tick hook, after
 * the client's own pass. A live node intercepts clicks in its rect even when it
 * draws nothing. */
void mm_panel_idle(void);

/* One frame of label editing. Post-tick, not a draw or input proc. */
void mm_panel_edit_tick(void);

/* Whether the client has finished loading a map. Everything else here reads
 * BSS globals that are zero until then, so check this first. */
int mm_client_in_world(void);

/* ---- markers.c / markers_io.c -------------------------------------------- */

/* Personal, client-local map markers persisted beside main.exe.
 * Spec: mw-tools/tools/client-dll/map-markers.md section 4. */

#define MM_MARKER_ID_LEN     16     /* lowercase hex, stable for the marker */
#define MM_MARKER_LABEL_MAX  31     /* bytes, client code page, after decode */
#define MM_MARKER_FIELDS     7
#define MM_MARKERS_MAX       2048
#define MM_MARKERS_PER_MAP   64

#define MM_STORE_FILE         "map-markers.tsv"
#define MM_STORE_TEMP         "map-markers.tsv.tmp"
#define MM_STORE_MUTEX_PREFIX "Global\\osmw_map_markers_"
#define MM_STORE_MAX_BYTES    (4u * 1024u * 1024u)

enum {
    MM_MARKER_KIND_NPC = 0,
    MM_MARKER_KIND_SHOP,
    MM_MARKER_KIND_PORTAL,
    MM_MARKER_KIND_QUEST,
    MM_MARKER_KIND_NOTE,
    MM_MARKER_KIND_COUNT
};

#define MM_MARKER_FLAG_VISIBLE 0x01
#define MM_MARKER_FLAG_MASK    0x01

enum {
    MM_MARKER_OK           =  0,
    MM_MARKER_ERR_INVALID  = -1,
    MM_MARKER_ERR_FULL     = -2,   /* 2048 global cap */
    MM_MARKER_ERR_MAP_FULL = -3,   /* 64 per base map */
    MM_MARKER_ERR_MISSING  = -4,
    MM_MARKER_ERR_HEADER   = -5,   /* not a marker file */
    MM_MARKER_ERR_VERSION  = -6,   /* a newer writer; do not overwrite */
    MM_MARKER_ERR_LOCKED   = -7,
    MM_MARKER_ERR_IO       = -8,
    MM_MARKER_ERR_READONLY = -9
};

typedef struct {
    char           id[MM_MARKER_ID_LEN + 1];
    unsigned short map_file;            /* MAPnnn.MAP, the persistence key */
    unsigned       x, y;                /* map units, see spec section 3 */
    unsigned char  kind;                /* MM_MARKER_KIND_* */
    unsigned char  flags;               /* MM_MARKER_FLAG_* */
    char           label[MM_MARKER_LABEL_MAX + 1];
} mm_marker_t;

typedef struct {
    mm_marker_t *items;
    int          count;
} mm_marker_set_t;

typedef struct {
    int accepted;
    int rejected;      /* malformed field, bad escape, out-of-range enum */
    int duplicates;    /* same id seen twice; first wins */
    int overflow;      /* would exceed a cap */
} mm_marker_stats_t;

/* markers.c: pure model and text format, no file access. */
int  mm_markers_parse(const char *text, int len, mm_marker_set_t *set,
                      mm_marker_stats_t *stats);
int  mm_markers_serialise(const mm_marker_set_t *set, char *out, int cap);
int  mm_markers_serialised_bound(int count);
int  mm_markers_find(const mm_marker_set_t *set, const char *id);
int  mm_markers_count_on_map(const mm_marker_set_t *set, unsigned map_file);
int  mm_markers_upsert(mm_marker_set_t *set, const mm_marker_t *m);
int  mm_markers_erase(mm_marker_set_t *set, const char *id);
void mm_markers_make_id(char out[MM_MARKER_ID_LEN + 1]);

/* markers_map.c: which map this is, and where a marker lands on screen. */
int  mm_markers_map_file(void);                    /* MAPnnn -> nnn, else -1 */
int  mm_markers_map_extent(int *width, int *height);
int  mm_markers_in_bounds(unsigned x, unsigned y);
int  mm_markers_project(unsigned x, unsigned y, int *screen_x, int *screen_y);
void mm_markers_verify(void);                      /* one log line per map */

/* The primitive the layer uses for its labels, supplied by panel.c. */
typedef void (*mm_marker_text_fn)(int x, int y, const char *text);

/* Blit one marker glyph from the module's own MDA. The sprite carries its own
 * centre, so screen_x/screen_y are the point the marker sits on. Returns 0 if
 * the asset or the blit is unavailable, which disables only this layer. */
int  mm_markers_blit(int kind, int screen_x, int screen_y);

/* Draw the layer from the MAP's own draw proc rather than the panel's, so the
 * glyphs cannot be painted over by the map content. Claims the draw proc
 * prologue; a failure disables only the marker layer. */
int  mm_markers_install_draw(mm_marker_text_fn draw_text);
void mm_markers_remove_draw(void);

/* The panel's backdrop text primitive, handed to the layer for its labels. */
mm_marker_text_fn mm_panel_text_boxed(void);

/* Draw every visible marker for the current map. Labels follow
 * [Minimap] MarkerLabels (hover, always, off); the whole layer follows
 * [Minimap] MarkerLayer. */
void mm_markers_draw_layer(mm_marker_text_fn draw_text);

/* The marker the list has selected, or NULL. Its label is always drawn and it
 * is what the Marks list highlights. The pointer must outlive the frame. */
void mm_markers_set_selected(const char *id);

/* markers_edit.c: label text entry.
 *
 * Reads the client's own per-frame typed-character queue into a plain buffer
 * rather than constructing one of the client's text controls. See the file
 * header for why, and for the IME limitation that costs. */
int         mm_edit_active(void);
const char *mm_edit_text(void);
const char *mm_edit_target(void);
void        mm_edit_begin(const char *marker_id, const char *initial);
void        mm_edit_cancel(void);
/* One frame. Returns non-zero when Enter committed the field; sets *cancelled
 * when Escape dismissed it. */
int         mm_edit_tick(int *cancelled);

/* markers_io.c: the shared file, its lock, and change detection. */
int  mm_store_init(void);
void mm_store_shutdown(void);
int  mm_store_load(void);
void mm_store_poll(void);
int  mm_store_writable(void);
const mm_marker_set_t *mm_store_get(void);
int  mm_store_put(const mm_marker_t *m);
int  mm_store_remove(const char *id);

#endif /* OSMW_MINIMAP_H */
