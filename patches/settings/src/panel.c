/* The settings window: its UI node, its layout, its drawing and its input.
 *
 * The panel is an ordinary client UI node created through UiCreateNode
 * (0x45E5A0), the same call that builds every window in the game. Unlike
 * osmw_minimap's NPC list there is no host window to hang off, so this mod
 * creates and destroys its own node -- which makes WHERE it does that the
 * central problem of the file. See st_panel_tick.
 *
 * Drawing is the recipe the minimap panel uses:
 *   - 0x4615B0 tiles the nine system\info.mda pieces (the client's own tooltip
 *     frame) over an arbitrary rect
 *   - 0x49C8D0 blits bitmap-font text, with ~C colour markup inline
 * so the window looks like part of the client rather than like an overlay.
 */
#include "osmw_settings.h"

#include <stdio.h>
#include <string.h>

/* ---- layout ---------------------------------------------------------------
 *
 * The client is 800x600. The panel is centred, the same treatment the stock
 * minimap gets at (196,148)-(604,452).
 *
 * The SIZE IS NOT FREE CHOICE. 0x4615B0 tiles the frame as 16px corners with
 * 8px edge pieces between them, so it needs (w-16)/8 and (h-16)/8 to come out
 * whole or the edges do not meet the corners. 320 gives 38 and 400 gives 48.
 * (The minimap panel is 200x304 for exactly this reason: 23 and 36.)
 *
 * If you resize this window, check that arithmetic first.
 */
#define ST_PANEL_W   320
#define ST_PANEL_H   400
#define ST_SCREEN_W  800
#define ST_SCREEN_H  600

#define ST_PANEL_L   ((ST_SCREEN_W - ST_PANEL_W) / 2)   /* 240 */
#define ST_PANEL_T   ((ST_SCREEN_H - ST_PANEL_H) / 2)   /* 100 */

#define ST_ROW_H     14
#define ST_PAD_X     12
#define ST_PAD_Y     8

#define ST_TITLE_DY  ST_PAD_Y
#define ST_ROWS_DY   (ST_PAD_Y + ST_ROW_H + 4)
/* The footer sits a row above the bottom edge, inside the frame's 16px corner. */
#define ST_FOOTER_DY (ST_PANEL_H - ST_PAD_Y - ST_ROW_H - 6)

/* Where the control column starts, relative to the panel's left edge. Rows
 * draw the label first and the control at a fixed x so the column lines up
 * regardless of label length. */
#define ST_CTRL_DX   210

/* The Close hot zone, relative to the panel's left edge. */
#define ST_CLOSE_L   (ST_PANEL_W - ST_PAD_X - 60)
#define ST_CLOSE_R   (ST_PANEL_W - ST_PAD_X)

/* ---- the client's routines ------------------------------------------------ */

/* __cdecl: the call site cleans 0x20 bytes of arguments itself. The RECT is
 * passed by value, so it arrives as four separate ints. */
typedef int(__cdecl *st_uicreate_fn)(int left, int top, int right, int bottom,
                                     void *draw_proc, void *input_proc,
                                     int arg4, int list_sel);

/* UiDestroyNode(handle, list_sel) -- TWO arguments.
 *
 * This cost osmw_minimap most of a day and the generated signature makes it
 * plain: the prologue is `mov al,[esp+8]` (list_sel) then `mov edx,[esp+4]`
 * (handle). Called with one argument the callee reads rubbish for list_sel,
 * goes hunting the wrong list, does not find the node and therefore never
 * frees it -- and the node stays linked, invisible because its draw proc has
 * stopped painting, silently swallowing every click inside its rect until the
 * process exits.
 *
 * The general rule that came out of it: confirm a client function's arity from
 * the CALLER'S STACK CLEANUP, not from the pushes visible beside the call.
 */
typedef void(__cdecl *st_uidestroy_fn)(int handle, int list_sel);

typedef void(__cdecl *st_drawtext_fn)(int x, int y, const char *text,
                                      int mode, void *ctx);
typedef void(__cdecl *st_drawbox_fn)(int x, int y, int w, int h, void *ctx);
typedef int(__cdecl *st_nodegeom_fn)(int handle, st_rect_t *rect, int *flag,
                                     int list_sel);
typedef void(__cdecl *st_nodesetgeom_fn)(int handle, const st_rect_t *rect,
                                         int flag, int list_sel);

/* Both stock procs take no arguments and read global state, so ours do too.
 * They are __cdecl, so even if the client did push something the caller cleans
 * it and ignoring it is safe. */
static void __cdecl panel_draw(void);
static void __cdecl panel_input(void);

static int g_handle = -1;       /* our node; -1 when it does not exist */
static int g_want_open;         /* the hotkey's request, acted on in the tick */
static int g_click_armed = 1;
static int g_open_sc;           /* scancode of [Settings] OpenKey */

/* Which row each drawn line came from, rebuilt every draw.
 *
 * Today every row gets a line, so this is an identity mapping and a click
 * could just as well divide by ST_ROW_H. It is kept because the draw and the
 * click have to agree on what "line 7" means, and the first thing that breaks
 * that is a row the draw decides to skip -- hiding the rows of a mod that is
 * not loaded, say, which is an obvious next step. Recording the mapping as the
 * lines are drawn means a click always resolves against what was actually on
 * screen, so that change stays a one-line change. */
#define ST_MAX_LINES 26
static int g_line_row[ST_MAX_LINES];
static int g_line_count;

int st_node_rect(int handle, st_rect_t *out_rect, int *out_flag)
{
    st_nodegeom_fn fn = (st_nodegeom_fn)st_addr[ST_NODEGEOMETRY];
    int flag = 0;

    if (!fn || !out_rect)
        return -1;
    /* out_rect must be a full 16-byte rect: the callee writes four dwords
     * through it unconditionally. A single int here smashed a return address
     * in the minimap mod and took the render loop down with it. */
    return fn(handle, out_rect, out_flag ? out_flag : &flag, 0);
}

int st_node_set_rect(int handle, const st_rect_t *rect, int flag)
{
    st_nodesetgeom_fn fn = (st_nodesetgeom_fn)st_addr[ST_NODESETGEOMETRY];

    if (!fn)
        return -1;
    fn(handle, rect, flag, 0);
    return 0;
}

int st_client_in_world(void)
{
    /* The grid scale is written by the .map loader and is never legitimately
     * zero once a map is up.
     *
     * This test also protects early core startup, before the client has
     * initialised its world: every global reads as BSS zero rather than -1,
     * so a handle checked with >= 0 looks open when nothing exists yet.
     * osmw_minimap's first observation run reported
     * "minimap: opened" 60 ms after start with every field zero. */
    if (!st_addr[ST_GRIDSCALEX])
        return 0;
    return *(int *)st_addr[ST_GRIDSCALEX] != 0;
}

/* Draw one line.
 *
 * MODE 1, not 0, and the difference is not cosmetic:
 *   mode 1 -> 0x49B920, plain glyphs. 780 of the client's 863 DrawText call
 *             sites use this. It is the normal case.
 *   mode 0 -> 0x49A390, glyphs on a filled backdrop. 28 sites, and the stock
 *             minimap's coordinate readout is one of them -- it draws over map
 *             terrain, where the backdrop is what keeps it legible.
 *
 * The minimap panel copied its call from that readout and inherited the
 * backdrop with it, which put a black box behind every row. On a panel that
 * has its own frame there is nothing to be legible against.
 */
static void panel_text(int x, int y, const char *text)
{
    st_drawtext_fn draw;
    void *ctx;

    if (!st_addr[ST_DRAWTEXT] || !st_addr[ST_DRAWCTX])
        return;
    /* Null until the client is actually rendering, so read it per call rather
     * than caching it at install time. */
    ctx = *(void **)st_addr[ST_DRAWCTX];
    if (!ctx)
        return;
    draw = (st_drawtext_fn)st_addr[ST_DRAWTEXT];
    draw(x, y, text, 1, ctx);
}

/* Fill the panel with the client's tooltip frame, before any text. */
static void panel_box(const st_rect_t *r)
{
    st_drawbox_fn box;
    void *ctx;

    if (!st_addr[ST_DRAWINFOBOX] || !st_addr[ST_DRAWCTX])
        return;
    /* The nine frames are loaded during client init. If that has not happened
     * -- or failed -- the array is still null and the blits would draw nothing
     * useful, so skip the background rather than risk it. */
    if (!st_addr[ST_INFOFRAMES] || !*(void **)st_addr[ST_INFOFRAMES])
        return;
    ctx = *(void **)st_addr[ST_DRAWCTX];
    if (!ctx)
        return;
    box = (st_drawbox_fn)st_addr[ST_DRAWINFOBOX];
    box(r->left, r->top, r->right - r->left, r->bottom - r->top, ctx);
}

/* ~C1 sets the highlight colour and ~C0 resets, the same markup the stock
 * coord readout uses. ~C4 is the second highlight; here it marks a row whose
 * module is not loaded, so it reads as unavailable rather than as off. */
static void panel_draw_row(const st_rect_t *r, int y, const st_row_t *row)
{
    char line[128];
    const char *ctrl;
    int value = st_row_value(row);
    int present = st_row_present(row);

    if (row->kind == ST_ROW_HEADING) {
        _snprintf(line, sizeof(line), "~C1%s~C0", row->label);
        line[sizeof(line) - 1] = 0;
        panel_text(r->left + ST_PAD_X, y, line);
        return;
    }

    if (row->kind == ST_ROW_CYCLE)
        ctrl = row->choices[value] ? row->choices[value] : "?";
    else
        ctrl = value ? "[X]" : "[ ]";

    /* Label, then the control at a fixed column so the panel reads as a table
     * rather than as ragged text. The relaunch marker sits after the control,
     * where the eye lands last. */
    _snprintf(line, sizeof(line), "%s%s", present ? "  " : "~C4  ", row->label);
    line[sizeof(line) - 1] = 0;
    panel_text(r->left + ST_PAD_X, y, line);

    _snprintf(line, sizeof(line), "%s%s%s", present ? "~C1" : "~C4", ctrl,
              row->applies == ST_APPLY_RELAUNCH ? "~C0 *" : "~C0");
    line[sizeof(line) - 1] = 0;
    panel_text(r->left + ST_CTRL_DX, y, line);
}

static void panel_draw_all(const st_rect_t *r)
{
    int i, y;

    panel_box(r);

    panel_text(r->left + ST_PAD_X, r->top + ST_TITLE_DY,
               "~C1osMW Settings~C0");

    y = r->top + ST_ROWS_DY;
    g_line_count = 0;
    for (i = 0; i < ST_ROW_COUNT && g_line_count < ST_MAX_LINES; i++) {
        panel_draw_row(r, y, &ST_ROWS[i]);
        g_line_row[g_line_count++] = i;
        y += ST_ROW_H;
    }

    panel_text(r->left + ST_PAD_X, r->top + ST_FOOTER_DY,
               "~C1*~C0 next launch");
    panel_text(r->left + ST_CLOSE_L, r->top + ST_FOOTER_DY,
               "~C1[ Close ]~C0");
}

static void __cdecl panel_draw(void)
{
    st_rect_t r = {0, 0, 0, 0};

    if (g_handle < 0)
        return;
    if (st_node_rect(g_handle, &r, 0) != 0)
        return;
    panel_draw_all(&r);
}

/* Mark the pending mouse event handled.
 *
 * Each node's input proc is responsible for clearing this -- the stock minimap
 * proc does it at 0x4200C2 -- and only the node under the cursor is called.
 * Without it a single click stays pending across frames and repeats. */
static void panel_consume_event(void)
{
    if (st_addr[ST_EVENTCODE])
        *(unsigned char *)st_addr[ST_EVENTCODE] = 0;
}

/* Closing has to take any pending click with it.
 *
 * While the node exists it absorbs clicks over its rect and clears the event
 * byte itself. The moment it is freed nothing is absorbing them, and a click
 * still latched in 0x7029BC is dispatched to the world at the same cursor
 * position -- which reads as a ground click and walks the character to
 * wherever the last row was. */
static void panel_release_click(void)
{
    panel_consume_event();
    g_click_armed = 1;
}

static void __cdecl panel_input(void)
{
    st_rect_t r = {0, 0, 0, 0};
    int mx, my, line, row;

    if (g_handle < 0)
        return;
    if (!st_addr[ST_EVENTCODE] || !st_addr[ST_MOUSEX] || !st_addr[ST_MOUSEY])
        return;

    /* 0xF5 is the click. 0xF1 is hover, 0xF2/0xF4 are press states and 0xF8 is
     * deactivate; none of them is ours to act on. */
    if (*(unsigned char *)st_addr[ST_EVENTCODE] != 0xF5) {
        g_click_armed = 1;       /* button released; the next press counts */
        return;
    }

    /* One press must count once.
     *
     * Clearing the event byte is not enough: the client re-asserts 0xF5 every
     * frame while the button is held. osmw_minimap proved this by watching a
     * 250 ms cooldown turn a single press into a steady stream of clicks
     * exactly 250 ms apart. So trigger on the edge -- act once, then wait for
     * a frame with the event no longer set before arming again. */
    if (!g_click_armed) {
        /* Consume rather than leave it set. While the panel is up a latched
         * click is harmless because the panel absorbs it, but it stays latched,
         * and a latched click is exactly what leaks to the world the moment
         * the panel is destroyed. */
        panel_consume_event();
        return;
    }
    g_click_armed = 0;

    if (st_node_rect(g_handle, &r, 0) != 0)
        return;

    mx = *(int *)st_addr[ST_MOUSEX] - r.left;
    my = *(int *)st_addr[ST_MOUSEY] - r.top;

    /* Close, before the row hit test: the footer is below every row, but an
     * out-of-range line index would otherwise have to be distinguished from a
     * miss, and this is clearer. */
    if (my >= ST_FOOTER_DY && my < ST_FOOTER_DY + ST_ROW_H &&
        mx >= ST_CLOSE_L && mx < ST_CLOSE_R) {
        g_want_open = 0;         /* the tick destroys the node, not us */
        panel_consume_event();
        return;
    }

    line = (my - ST_ROWS_DY) / ST_ROW_H;
    if (my < ST_ROWS_DY || line < 0 || line >= g_line_count) {
        panel_consume_event();   /* a click on the frame is still ours */
        return;
    }

    row = g_line_row[line];
    if (row >= 0 && row < ST_ROW_COUNT &&
        ST_ROWS[row].kind != ST_ROW_HEADING)
        st_row_advance(&ST_ROWS[row]);

    panel_consume_event();
}

static void panel_open(void)
{
    st_uicreate_fn create = (st_uicreate_fn)st_addr[ST_UICREATENODE];

    if (!create || g_handle >= 0)
        return;

    g_line_count = 0;
    g_click_armed = 1;
    /* list_sel 0 and arg4 0, the pair the stock minimap passes. */
    g_handle = create(ST_PANEL_L, ST_PANEL_T,
                      ST_PANEL_L + ST_PANEL_W, ST_PANEL_T + ST_PANEL_H,
                      (void *)panel_draw, (void *)panel_input, 0, 0);
    st_log("panel: opened, node %d at (%d,%d)-(%d,%d)", g_handle,
           ST_PANEL_L, ST_PANEL_T, ST_PANEL_L + ST_PANEL_W,
           ST_PANEL_T + ST_PANEL_H);
}

static void panel_close(void)
{
    st_uidestroy_fn destroy = (st_uidestroy_fn)st_addr[ST_UIDESTROYNODE];

    if (g_handle < 0)
        return;

    if (destroy) {
        destroy(g_handle, 0);        /* TWO arguments; see the typedef */
        st_log("panel: closed, node %d freed", g_handle);
        g_handle = -1;
        panel_release_click();
        return;
    }

    /* No destroy available. Collapsing the rect at least stops the node
     * intercepting clicks -- an invisible node that still swallows everything
     * inside a 320x400 block is the worst outcome here, much worse than a
     * leaked allocation. */
    {
        st_rect_t empty = {0, 0, 0, 0};
        st_node_set_rect(g_handle, &empty, 0);
        st_log("panel: no destroy routine; node %d collapsed instead", g_handle);
        panel_release_click();
    }
}

int st_panel_is_open(void)
{
    return g_handle >= 0;
}

/* Called from the core's post-UiTick subscription, AFTER the client's pass.
 *
 * THIS IS THE ONLY PLACE A NODE MAY BE CREATED OR DESTROYED. Both node walkers
 * iterate a global cursor at 0x7BAD98 and re-read it after each proc returns,
 * so relinking the list from inside a draw or input proc corrupts the
 * iteration. The hotkey and the Close button therefore only set a flag; the
 * actual open and close happen here.
 *
 * The related lesson from osmw_minimap, which applies to any cleanup: cleanup
 * cannot be driven by the thing whose absence it is cleaning up after. Its
 * teardown used to live in the draw proc, which stops being called once the
 * window is gone -- leaving an invisible node eating every click in its rect.
 */
void st_panel_tick(void)
{
    /* Nothing before the client has a world. Opening over the login screen
     * would draw a frame the client has not loaded and read globals that are
     * still BSS zero. */
    if (!st_client_in_world()) {
        if (g_handle >= 0) {
            g_want_open = 0;
            panel_close();
        }
        return;
    }

    if (g_open_sc > 0 && st_key_pressed(g_open_sc))
        g_want_open = !g_want_open;

    if (g_want_open && g_handle < 0)
        panel_open();
    else if (!g_want_open && g_handle >= 0)
        panel_close();
}

int st_panel_install(void)
{
    char buf[32];

    st_key_init();

    /* [Settings] OpenKey, default F12.
     *
     * F12 is free in the stock client (confirmed 2026-09-04) and neighbours
     * osmw_fightspeed's F11 rather than colliding with it. It is also the last
     * key the scancode table can hold -- the arrays are 0x59 entries and F12 is
     * 0x58.
     *
     * NOT Escape: the client already opens its log-off menu on Escape, and a
     * mod must not take a key the client has bound. The failure would not be
     * that the panel also closes, it is that someone reaching for log off gets
     * whichever handler wins. */
    st_cfg_copy("OpenKey", "F12", buf, sizeof(buf));
    g_open_sc = st_key_scancode_for(buf);
    if (g_open_sc < 0) {
        st_log("panel: OpenKey '%s' is not a key this mod knows (F1..F12) "
               "-- the panel cannot be opened", buf);
        g_open_sc = 0;
    } else if (g_open_sc == 0) {
        st_log("panel: OpenKey is empty -- the panel cannot be opened");
    } else {
        st_log("panel: %s (scancode 0x%02X) toggles the settings window",
               buf, g_open_sc);
    }

    st_log("panel: %dx%d at (%d,%d); frame tiles %d x %d",
           ST_PANEL_W, ST_PANEL_H, ST_PANEL_L, ST_PANEL_T,
           (ST_PANEL_W - 16) / 8, (ST_PANEL_H - 16) / 8);
    return 1;
}

void st_panel_remove(void)
{
    g_want_open = 0;
    panel_close();
}
