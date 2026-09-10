/* A small, on-demand consumable timer window rendered entirely with the
 * client's stock UI primitives. The panel owns no game state: timers.c keeps
 * a server-authoritative snapshot and this file only formats that view.
 *
 * Effects are shown as their own item.mda sprites laid out side by side, one
 * cell each, so the panel reads as a client feature rather than an overlay.
 * An effect that is not running has no cell at all, which is why the panel
 * width is a function of the snapshot and can change while it is open.
 */
#include "osmw_timers.h"

#include <stdio.h>

#define TM_SCREEN_W  800
#define TM_PANEL_H    56
#define TM_PANEL_T     8
#define TM_AUTO_CLOSE_MS 5000U

/* Widths per state. 0x4615B0 tiles nine info.mda pieces as 16px corners with
 * 8px edges, so (w-16)/8 and (h-16)/8 must both come out whole: 15/13/25 and
 * 5. Fixed per state rather than fitted to the text, so a timer crossing the
 * hour boundary never resizes the panel under the eye. */
#define TM_PANEL_W_TEXT  136    /* "Synchronising..." / "No timers active" */
#define TM_PANEL_W_ONE   120
#define TM_PANEL_W_TWO   216
#define TM_PANEL_W_THREE 312

/* 20, not 8: the frame's corner pieces are 16px wide and a sprite inset by 8
 * would sit underneath one. */
#define TM_PAD_X          20
#define TM_ICON_W         32    /* every item.mda frame is 32x32 */
#define TM_ICON_DY        12    /* centres a 32px sprite in a 56px frame */
#define TM_TEXT_DY        22    /* text baseline against that sprite */
#define TM_ICON_GAP        4
#define TM_LABEL_DX       10    /* centres a 2-char fallback in the 32px column */

/* "2:59:41", 7 characters at 6px. Reserved whole so MM:SS and H:MM:SS give
 * the same cell width. */
#define TM_VALUE_W        42
#define TM_CELL_W   (TM_ICON_W + TM_ICON_GAP + TM_VALUE_W)
#define TM_CELL_GAP       16
#define TM_MAX_CELLS       3

#define TM_ICON_DEVIL_TEARS  24
#define TM_ICON_SHAPESHIFT  344
/* Double and triple EXP are mutually exclusive, so they share one cell and the
 * sprite is chosen by the multiplier the server reports. */
#define TM_ICON_DOUBLE_EXP  505
#define TM_ICON_TRIPLE_EXP  434

/* The stock array holds 512 entries and a capacity-patched client holds more,
 * but reading past whichever one this client has would hand the blitter a
 * pointer from an adjacent global. 511 is the bound every client satisfies,
 * and both sprites we draw are far below it. */
#define TM_ICON_MAX      511

typedef int (__cdecl *tm_uicreate_fn)(int left, int top, int right, int bottom,
                                       void *draw_proc, void *input_proc,
                                       int arg4, int list_sel);
typedef void (__cdecl *tm_uidestroy_fn)(int handle, int list_sel);
typedef void (__cdecl *tm_drawtext_fn)(int x, int y, const char *text,
                                       int mode, void *ctx);
typedef void (__cdecl *tm_drawbox_fn)(int x, int y, int w, int h, void *ctx);
typedef int (__cdecl *tm_nodegeom_fn)(int handle, tm_rect_t *rect, int *flag,
                                      int list_sel);
typedef void (__cdecl *tm_nodesetgeom_fn)(int handle, const tm_rect_t *rect,
                                          int flag, int list_sel);

/* The client's item-icon blitter, 0x44ADD0. x,y is the sprite's top-left
 * corner: it subtracts the frame's own signed 16-bit hotspot at +0x1c/+0x1e,
 * and every item.mda frame has hotspot (0,0). It is the same RLE decoder that
 * draws the info.mda frame, so skip runs leave the destination untouched and
 * sprites composite correctly over the frame's transparent interior. It does
 * not clip. */
typedef void (__cdecl *tm_drawicon_fn)(int x, int y, void *frame, void *surface);

typedef struct {
    int          icon;
    const char  *label;     /* drawn instead when the sprite is unavailable */
    unsigned int seconds;
} tm_cell_t;

static void __cdecl panel_draw(void);
static void __cdecl panel_input(void);

static int g_handle = -1;
static int g_want_open;
static int g_click_armed = 1;
static int g_open_sc;
static DWORD g_auto_close_at;
static int g_icon_devil_tears = TM_ICON_DEVIL_TEARS;
static int g_icon_shapeshift = TM_ICON_SHAPESHIFT;
static int g_icon_double_exp = TM_ICON_DOUBLE_EXP;
static int g_icon_triple_exp = TM_ICON_TRIPLE_EXP;

int tm_node_rect(int handle, tm_rect_t *out_rect, int *out_flag)
{
    tm_nodegeom_fn fn = (tm_nodegeom_fn)tm_addr[TM_NODEGEOMETRY];
    int flag = 0;

    if (!fn || !out_rect)
        return -1;
    return fn(handle, out_rect, out_flag ? out_flag : &flag, 0);
}

int tm_node_set_rect(int handle, const tm_rect_t *rect, int flag)
{
    tm_nodesetgeom_fn fn = (tm_nodesetgeom_fn)tm_addr[TM_NODESETGEOMETRY];

    if (!fn)
        return -1;
    fn(handle, rect, flag, 0);
    return 0;
}

int tm_client_in_world(void)
{
    if (!tm_addr[TM_GRIDSCALEX])
        return 0;
    return *(int *)tm_addr[TM_GRIDSCALEX] != 0;
}

static void panel_text(int x, int y, const char *text)
{
    tm_drawtext_fn draw;
    void *ctx;

    if (!tm_addr[TM_DRAWTEXT] || !tm_addr[TM_DRAWCTX])
        return;
    ctx = *(void **)tm_addr[TM_DRAWCTX];
    if (!ctx)
        return;
    draw = (tm_drawtext_fn)tm_addr[TM_DRAWTEXT];
    draw(x, y, text, 1, ctx);
}

static void panel_box(const tm_rect_t *rect)
{
    tm_drawbox_fn box;
    void *ctx;

    if (!tm_addr[TM_DRAWINFOBOX] || !tm_addr[TM_DRAWCTX] ||
        !tm_addr[TM_INFOFRAMES] || !*(void **)tm_addr[TM_INFOFRAMES])
        return;
    ctx = *(void **)tm_addr[TM_DRAWCTX];
    if (!ctx)
        return;
    box = (tm_drawbox_fn)tm_addr[TM_DRAWINFOBOX];
    box(rect->left, rect->top, rect->right - rect->left,
        rect->bottom - rect->top, ctx);
}

/* Returns 1 if the sprite was drawn, 0 if the caller should draw its fallback
 * label instead. Nothing here stands the panel down: artwork is cosmetic. */
static int panel_icon(int x, int y, int index)
{
    tm_drawicon_fn draw;
    void **array;
    void *frame;
    void *ctx;

    if (!tm_addr[TM_DRAWITEMICON] || !tm_addr[TM_ITEMSPRITEARRAY] ||
        !tm_addr[TM_DRAWCTX])
        return 0;
    if (index < 0 || index > TM_ICON_MAX)
        return 0;
    ctx = *(void **)tm_addr[TM_DRAWCTX];
    if (!ctx)
        return 0;

    /* Unloaded slots stay NULL and the blitter dereferences [esi+0x20]
     * immediately, with no check of its own. */
    array = (void **)tm_addr[TM_ITEMSPRITEARRAY];
    frame = array[index];
    if (!frame)
        return 0;

    draw = (tm_drawicon_fn)tm_addr[TM_DRAWITEMICON];
    draw(x, y, frame, ctx);
    return 1;
}

static void format_duration(unsigned int seconds, char *out, int size)
{
    unsigned int hours = seconds / 3600U;
    unsigned int minutes = (seconds / 60U) % 60U;
    unsigned int secs = seconds % 60U;

    if (hours)
        _snprintf(out, size, "%u:%02u:%02u", hours, minutes, secs);
    else
        _snprintf(out, size, "%02u:%02u", minutes, secs);
    out[size - 1] = '\0';
}

static void panel_value(int x, int y, unsigned int seconds)
{
    char duration[24];
    char value[64];

    /* ~C takes 0-9: the handler at 0x49C0AE stores the digit at renderer+0x40
     * and the glyph loop reads a 16-bit colour from renderer + index*2. ~C0 is
     * the client's default body colour and ~C1 is red, so the running state is
     * left alone and only the final minute escalates. Drawn the other way round
     * a three-hour timer sits there in alarm red and the last minute reads as a
     * de-escalation. */
    format_duration(seconds, duration, sizeof(duration));
    _snprintf(value, sizeof(value), "%s%s~C0",
              seconds <= 60U ? "~C1" : "~C0", duration);
    value[sizeof(value) - 1] = '\0';
    panel_text(x, y, value);
}

/* Cells for the effects that are actually running, in a fixed order so a cell
 * does not move sideways when a different effect expires. */
static int panel_cells(const tm_timer_view_t *view, tm_cell_t *out)
{
    int n = 0;

    if (!view->synced)
        return 0;
    if (view->devil_tears_seconds) {
        out[n].icon = g_icon_devil_tears;
        out[n].label = "DT";
        out[n].seconds = view->devil_tears_seconds;
        n++;
    }
    if (view->shapeshift_seconds) {
        out[n].icon = g_icon_shapeshift;
        out[n].label = "SS";
        out[n].seconds = view->shapeshift_seconds;
        n++;
    }
    if (view->exp_seconds) {
        /* Anything other than the two stock potions still gets a cell, drawn with
         * the triple-EXP sprite and its own multiplier as the fallback label. */
        out[n].icon = view->exp_multiplier == 2U ? g_icon_double_exp
                                                 : g_icon_triple_exp;
        out[n].label = view->exp_multiplier == 2U ? "x2" : "x3";
        out[n].seconds = view->exp_seconds;
        n++;
    }
    return n;
}

static int panel_width_for(int cells)
{
    if (cells >= 3)
        return TM_PANEL_W_THREE;
    if (cells == 2)
        return TM_PANEL_W_TWO;
    if (cells == 1)
        return TM_PANEL_W_ONE;
    return TM_PANEL_W_TEXT;
}

static int panel_wanted_width(void)
{
    tm_timer_view_t view;
    tm_cell_t cells[TM_MAX_CELLS];

    tm_timers_view(&view);
    return panel_width_for(panel_cells(&view, cells));
}

static void __cdecl panel_draw(void)
{
    tm_rect_t rect = {0, 0, 0, 0};
    tm_timer_view_t view;
    tm_cell_t cells[TM_MAX_CELLS];
    int n, i, x;

    if (g_handle < 0 || tm_node_rect(g_handle, &rect, 0) != 0)
        return;

    tm_timers_view(&view);
    panel_box(&rect);

    n = panel_cells(&view, cells);
    if (!n) {
        panel_text(rect.left + TM_PAD_X, rect.top + TM_TEXT_DY,
                   view.synced ? "~C1No timers active~C0"
                               : "~C4Synchronising...~C0");
        return;
    }

    x = rect.left + TM_PAD_X;
    for (i = 0; i < n; i++) {
        if (!panel_icon(x, rect.top + TM_ICON_DY, cells[i].icon))
            panel_text(x + TM_LABEL_DX, rect.top + TM_TEXT_DY, cells[i].label);
        panel_value(x + TM_ICON_W + TM_ICON_GAP, rect.top + TM_TEXT_DY,
                    cells[i].seconds);
        x += TM_CELL_W + TM_CELL_GAP;
    }
}

/* An effect expiring or arriving while the panel is open changes how many
 * cells it needs, so the live node is resized from the tick rather than only
 * at open time. This is a rect setter, not a relink, so it does not disturb
 * the node walker's cursor. */
static void panel_sync_width(void)
{
    tm_rect_t rect;
    int flag = 0;
    int want_w, want_l;

    if (g_handle < 0)
        return;
    if (tm_node_rect(g_handle, &rect, &flag) != 0)
        return;

    want_w = panel_wanted_width();
    want_l = (TM_SCREEN_W - want_w) / 2;
    if (rect.left == want_l && rect.right == want_l + want_w)
        return;

    rect.left = want_l;
    rect.right = want_l + want_w;
    rect.top = TM_PANEL_T;
    rect.bottom = TM_PANEL_T + TM_PANEL_H;
    tm_node_set_rect(g_handle, &rect, flag);
}

static void panel_consume_event(void)
{
    if (tm_addr[TM_EVENTCODE])
        *(unsigned char *)tm_addr[TM_EVENTCODE] = 0;
}

static void panel_release_click(void)
{
    panel_consume_event();
    g_click_armed = 1;
}

static void __cdecl panel_input(void)
{
    if (g_handle < 0 || !tm_addr[TM_EVENTCODE])
        return;

    if (*(unsigned char *)tm_addr[TM_EVENTCODE] != 0xF5) {
        g_click_armed = 1;
        return;
    }
    if (!g_click_armed) {
        panel_consume_event();
        return;
    }
    g_click_armed = 0;

    /* The node owns every click inside its rect. Consuming clicks while the
     * glance panel is briefly visible prevents them leaking through to the
     * world beneath it. There are deliberately no interactive controls. */
    panel_consume_event();
}

static void panel_open(void)
{
    tm_uicreate_fn create = (tm_uicreate_fn)tm_addr[TM_UICREATENODE];
    int w, left;

    if (!create || g_handle >= 0)
        return;
    w = panel_wanted_width();
    left = (TM_SCREEN_W - w) / 2;
    g_click_armed = 1;
    g_handle = create(left, TM_PANEL_T, left + w, TM_PANEL_T + TM_PANEL_H,
                      (void *)panel_draw, (void *)panel_input, 0, 0);
    if (g_handle < 0) {
        g_want_open = 0;
        tm_log("panel: UiCreateNode failed");
        return;
    }
    g_auto_close_at = GetTickCount() + TM_AUTO_CLOSE_MS;
    tm_log("panel: opened, node %d at (%d,%d)-(%d,%d)", g_handle,
           left, TM_PANEL_T, left + w, TM_PANEL_T + TM_PANEL_H);
}

static void panel_close(void)
{
    tm_uidestroy_fn destroy = (tm_uidestroy_fn)tm_addr[TM_UIDESTROYNODE];

    if (g_handle < 0)
        return;
    if (destroy) {
        destroy(g_handle, 0);
        tm_log("panel: closed, node %d freed", g_handle);
        g_handle = -1;
        g_auto_close_at = 0;
        panel_release_click();
        return;
    }

    {
        tm_rect_t empty = {0, 0, 0, 0};
        tm_node_set_rect(g_handle, &empty, 0);
        tm_log("panel: no destroy routine; node %d collapsed", g_handle);
        g_auto_close_at = 0;
        panel_release_click();
    }
}

int tm_panel_is_open(void)
{
    return g_handle >= 0;
}

void tm_panel_tick(void)
{
    if (!tm_client_in_world()) {
        if (g_handle >= 0) {
            g_want_open = 0;
            panel_close();
        }
        return;
    }

    if (g_open_sc > 0 && tm_key_pressed(g_open_sc))
        g_want_open = !g_want_open;

    if (g_want_open && g_handle >= 0 &&
        (LONG)(GetTickCount() - g_auto_close_at) >= 0)
        g_want_open = 0;

    if (g_want_open && g_handle < 0)
        panel_open();
    else if (!g_want_open && g_handle >= 0)
        panel_close();
    else if (g_handle >= 0)
        panel_sync_width();
}

int tm_panel_install(void)
{
    int sprites;

    tm_key_init();
    g_open_sc = tm_key_scancode_for("F9");
    if (g_open_sc <= 0) {
        tm_log("panel: F9 is unavailable -- the panel cannot be opened");
        g_open_sc = 0;
    } else {
        tm_log("panel: F9 (scancode 0x%02X) toggles the timer window",
               g_open_sc);
    }

    g_icon_devil_tears = tm_cfg_int("DevilTearsIcon", TM_ICON_DEVIL_TEARS);
    g_icon_shapeshift = tm_cfg_int("ShapeshiftIcon", TM_ICON_SHAPESHIFT);
    g_icon_double_exp = tm_cfg_int("DoubleExpIcon", TM_ICON_DOUBLE_EXP);
    g_icon_triple_exp = tm_cfg_int("TripleExpIcon", TM_ICON_TRIPLE_EXP);

    sprites = tm_addr[TM_DRAWITEMICON] && tm_addr[TM_ITEMSPRITEARRAY];
    if (sprites)
        tm_log("panel: sprites %d/%d/%d/%d, blitter %p, item array %p",
               g_icon_devil_tears, g_icon_shapeshift, g_icon_double_exp,
               g_icon_triple_exp, tm_addr[TM_DRAWITEMICON],
               tm_addr[TM_ITEMSPRITEARRAY]);
    else
        tm_log("panel: sprite signatures unavailable -- cells fall back to "
               "DT/SS labels, geometry unchanged");

    tm_log("panel: widths %d/%d/%d/%d x %d at y=%d; frame tiles %d/%d/%d/%d x %d",
           TM_PANEL_W_TEXT, TM_PANEL_W_ONE, TM_PANEL_W_TWO, TM_PANEL_W_THREE,
           TM_PANEL_H, TM_PANEL_T,
           (TM_PANEL_W_TEXT - 16) / 8, (TM_PANEL_W_ONE - 16) / 8,
           (TM_PANEL_W_TWO - 16) / 8, (TM_PANEL_W_THREE - 16) / 8,
           (TM_PANEL_H - 16) / 8);
    return g_open_sc > 0;
}

void tm_panel_remove(void)
{
    g_want_open = 0;
    panel_close();
}
