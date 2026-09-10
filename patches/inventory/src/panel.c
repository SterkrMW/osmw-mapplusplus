/* Stock-styled companion control for the stock inventory window. The node is
 * created and destroyed only from core's post-UiTick callback, so it never
 * changes the list while the client's UI walker is traversing it.
 *
 * Two modes, chosen once at install time:
 *
 *   SPRITE   Two 21x21 buttons, Sort and Lock, blitted from the module's own
 *            system\osmw_inventory.mda into the empty band under the stock
 *            Vendor label, INSIDE the Equipment window. One node covers both.
 *            The node is there to OWN THE RECT so the manager consumes clicks
 *            on it; the pixels are painted from the post-UiTick overlay, not
 *            from the node's draw proc. See "Z-order" below.
 *   FALLBACK The original 80x24 sidecar: the client's tooltip nine-slice plus
 *            bitmap text, parked outside the window. Kept because a missing
 *            asset or an unresolved blitter should cost the buttons their
 *            looks, not cost the player the Sort feature.
 */
#include "osmw_inventory.h"

#include <string.h>

#define IV_SCREEN_W 800
#define IV_SCREEN_H 600
#define IV_BUTTON_W 80
#define IV_BUTTON_H 24
#define IV_BUTTON_GAP 4
#define IV_SCREEN_PAD 4

/* The Equipment window is MapMenu.MDA sub-file 4, 376x458. Measured off the
 * extracted artwork: the Vendor label spans x 302..347 / y 376..385, the
 * inventory grid frame ends at x 283, and the corner flourish starts at x 350
 * and y 428 - leaving a clear band at x 284..349, y 388..427.
 *
 * The pair sits on the exact x span of Vendor, one ~40px row below it, which is
 * the window's own label pitch. Offsets are taken from the window's
 * BOTTOM-RIGHT so an unexpected window size slides the buttons along the frame
 * they belong to rather than off it.
 */
#define IV_SPRITE_W 21
#define IV_SPRITE_H 21
#define IV_SPRITE_GAP 5
#define IV_SPRITE_PITCH (IV_SPRITE_W + IV_SPRITE_GAP)   /* 26 */
#define IV_STRIP_W (IV_SPRITE_W * 2 + IV_SPRITE_GAP)    /* 47 */
#define IV_STRIP_H IV_SPRITE_H

#define IV_STRIP_DX 75      /* 376 - 301 */
#define IV_STRIP_DY 59      /* 458 - 399 */

#define IV_PANEL_ART_W 376
#define IV_PANEL_ART_H 458

#define IV_MDA_PATH "system\\osmw_inventory.mda"

/* The blit's 6th argument is a bitmask the blitter decomposes, not a frame
 * index. The stock banner passes 0x12; the player indicator passes 7. */
#define IV_BLIT_FLAGS 0x12

/* UI event byte values. 0xF5 is the click the stock windows act on; 0xF2/0xF4
 * are the synthesised held-button ticks the pressed art keys off. */
#define IV_EVENT_CLICK 0xF5
#define IV_EVENT_HELD_A 0xF2
#define IV_EVENT_HELD_B 0xF4

enum { IV_BTN_SORT = 0, IV_BTN_LOCK, IV_BTN_COUNT, IV_BTN_NONE = -1 };

typedef int (__cdecl *iv_uicreate_fn)(int, int, int, int, void *, void *,
                                      int, int);
typedef void (__cdecl *iv_uidestroy_fn)(int, int);
typedef int (__cdecl *iv_nodegeom_fn)(int, iv_rect_t *, int *, int);
typedef void (__cdecl *iv_nodesetgeom_fn)(int, const iv_rect_t *, int, int);
typedef void (__cdecl *iv_drawtext_fn)(int, int, const char *, int, void *);
typedef void (__cdecl *iv_drawbox_fn)(int, int, int, int, void *);
typedef int (__cdecl *iv_loadmda_fn)(const char *path, void **out_handle);
typedef int (__cdecl *iv_blitmda_fn)(int handle, const char *anim, int x, int y,
                                     void *surface, int flags, int a, int b,
                                     int c);

static void __cdecl panel_draw(void);
static void __cdecl panel_input(void);

static int g_handle = -1;
static int g_inventory_handle = -1;
static int g_click_armed = 1;

/* Sprite mode state. g_sprite_mode is decided once, at install. */
static int   g_sprite_mode;
static void *g_bank;
static int   g_bank_tried;
static int   g_blit_warned;
static int   g_size_warned;
static int   g_was_hovered;

/* Z-order. The manager raises the window the player clicked, so clicking any
 * stock control in the inventory - a slot, Use, Drop, Vendor - moves that
 * window above our node and buries the buttons. Two halves to the fix:
 *
 *   Drawing  happens in iv_panel_overlay, called from the post-UiTick
 *            callback after the manager has walked and drawn every window.
 *            That is above everything by construction, so no detection and no
 *            list surgery is needed for the pixels.
 *   Input    needs our node found before the stock window, so it is recreated
 *            - which puts it at the end of the list - but ONLY on the frame
 *            the cursor enters the strip. Raising it unconditionally made the
 *            module the top node at all times, and the manager routes clicks
 *            to the top node, so every click anywhere in the inventory came
 *            here and was swallowed. Raise on hover-enter and the stock window
 *            keeps the top slot whenever the cursor is not on a button.
 *
 * panel_input must therefore never touch the event byte unless the cursor is
 * genuinely over one of the two buttons. Being dispatched is not evidence that
 * the click was ours.
 *
 * The recreate deliberately preserves g_click_armed. Re-arming it would turn a
 * held mouse button into one request per frame.
 */

/* [state][button]: normal, hover, pressed. */
static const char *const IV_ANIM[3][IV_BTN_COUNT] = {
    {"BtnSort",      "BtnLock"},
    {"BtnSortHover", "BtnLockHover"},
    {"BtnSortPress", "BtnLockPress"},
};

int iv_node_rect(int handle, iv_rect_t *rect, int *flag)
{
    iv_nodegeom_fn fn = (iv_nodegeom_fn)iv_addr[IV_NODEGEOMETRY];
    int local_flag = 0;

    if (!fn || !rect)
        return -1;
    return fn(handle, rect, flag ? flag : &local_flag, 0);
}

int iv_node_set_rect(int handle, const iv_rect_t *rect, int flag)
{
    iv_nodesetgeom_fn fn = (iv_nodesetgeom_fn)iv_addr[IV_NODESETGEOMETRY];

    if (!fn || !rect)
        return -1;
    fn(handle, rect, flag, 0);
    return 0;
}

int iv_client_in_world(void)
{
    return iv_addr[IV_GRIDSCALEX] && *(int *)iv_addr[IV_GRIDSCALEX] != 0;
}

static int stock_inventory_handle(void)
{
    if (!iv_client_in_world() || !iv_addr[IV_INVENTORYHANDLE])
        return -1;
    return *(int *)iv_addr[IV_INVENTORYHANDLE];
}

/* ---- sprite bank --------------------------------------------------------- */

/* Loaded once, lazily: the loader is only safe to call once the client's own
 * asset system is up, which it is by the time anything draws. */
static int sprite_bank(void)
{
    iv_loadmda_fn load;

    if (g_bank_tried)
        return g_bank != NULL;
    g_bank_tried = 1;

    load = (iv_loadmda_fn)iv_addr[IV_LOADMDA];
    if (!load)
        return 0;

    load(IV_MDA_PATH, &g_bank);
    if (!g_bank) {
        iv_log("panel: %s did not load; buttons will not draw", IV_MDA_PATH);
        return 0;
    }
    iv_log("panel: %s loaded, handle %p", IV_MDA_PATH, g_bank);
    return 1;
}

/* BlitMdaFrame searches the client's MDA table for the handle and then the
 * animation by name, and returns 0 without drawing if either lookup misses.
 * Both failures are silent, so the return value is the only way to see them. */
static int sprite_blit(const char *anim, int x, int y)
{
    iv_blitmda_fn blit = (iv_blitmda_fn)iv_addr[IV_BLITMDA];

    if (!blit || !iv_addr[IV_DRAWCTX] || !sprite_bank())
        return 0;
    return blit((int)g_bank, anim, x, y, *(void **)iv_addr[IV_DRAWCTX],
                IV_BLIT_FLAGS, 0, 0, 0) != 0;
}

static int cursor_xy(int *x, int *y)
{
    if (!iv_addr[IV_MOUSEX] || !iv_addr[IV_MOUSEY])
        return 0;
    *x = *(int *)iv_addr[IV_MOUSEX];
    *y = *(int *)iv_addr[IV_MOUSEY];
    return 1;
}

/* Which button the cursor is over, or IV_BTN_NONE for the gap and everything
 * else. One node covers both buttons, so this is the module's own hit test. */
static int button_at_cursor(const iv_rect_t *strip)
{
    int mx, my, offset, index;

    if (!cursor_xy(&mx, &my))
        return IV_BTN_NONE;
    if (my < strip->top || my >= strip->top + IV_SPRITE_H)
        return IV_BTN_NONE;

    offset = mx - strip->left;
    if (offset < 0)
        return IV_BTN_NONE;

    index = offset / IV_SPRITE_PITCH;
    if (index >= IV_BTN_COUNT)
        return IV_BTN_NONE;
    if (offset - index * IV_SPRITE_PITCH >= IV_SPRITE_W)
        return IV_BTN_NONE;    /* the gap between them */

    return index;
}

static int event_code(void)
{
    if (!iv_addr[IV_EVENTCODE])
        return 0;
    return *(unsigned char *)iv_addr[IV_EVENTCODE];
}

/* Sort holds its pressed art for the whole authoritative round trip and then
 * for the rest of its cooldown, so the art always says whether a click would
 * do anything. Lock has no feature bound yet, so it only presses while held. */
static int button_state(int button, int hovered)
{
    int code;

    if (button == IV_BTN_SORT &&
            (iv_sort_busy() || iv_sort_cooldown_ms() > 0U))
        return 2;
    if (!hovered)
        return 0;

    code = event_code();
    if (code == IV_EVENT_CLICK || code == IV_EVENT_HELD_A ||
            code == IV_EVENT_HELD_B)
        return 2;
    return 1;
}

/* ---- placement ----------------------------------------------------------- */

/* Sprite mode: the two buttons sit inside the window, under Vendor. */
static int place_strip(const iv_rect_t *inventory, iv_rect_t *strip)
{
    strip->left = inventory->right - IV_STRIP_DX;
    strip->top = inventory->bottom - IV_STRIP_DY;
    strip->right = strip->left + IV_STRIP_W;
    strip->bottom = strip->top + IV_STRIP_H;
    return 1;
}

/* Fallback mode: the original outside-the-window sidecar. */
static int place_button(const iv_rect_t *inventory, iv_rect_t *button)
{
    int left;
    int top;

    /* Prefer a lower-edge sidecar. The strip below the stock inventory is
     * occupied by the permanent HUD, including its inventory button. */
    top = inventory->bottom - IV_BUTTON_H;
    if (top < IV_SCREEN_PAD)
        top = IV_SCREEN_PAD;
    if (top + IV_BUTTON_H > IV_SCREEN_H - IV_SCREEN_PAD)
        top = IV_SCREEN_H - IV_SCREEN_PAD - IV_BUTTON_H;

    if (inventory->right + IV_BUTTON_GAP + IV_BUTTON_W <=
            IV_SCREEN_W - IV_SCREEN_PAD) {
        left = inventory->right + IV_BUTTON_GAP;
    } else if (inventory->left - IV_BUTTON_GAP - IV_BUTTON_W >=
            IV_SCREEN_PAD) {
        left = inventory->left - IV_BUTTON_GAP - IV_BUTTON_W;
    } else {
        left = inventory->left +
            ((inventory->right - inventory->left - IV_BUTTON_W) / 2);
        if (left < IV_SCREEN_PAD)
            left = IV_SCREEN_PAD;
        if (left + IV_BUTTON_W > IV_SCREEN_W - IV_SCREEN_PAD)
            left = IV_SCREEN_W - IV_SCREEN_PAD - IV_BUTTON_W;

        if (inventory->top - IV_BUTTON_GAP - IV_BUTTON_H >=
                IV_SCREEN_PAD) {
            top = inventory->top - IV_BUTTON_GAP - IV_BUTTON_H;
        } else if (inventory->bottom + IV_BUTTON_GAP + IV_BUTTON_H <=
            IV_SCREEN_H - IV_SCREEN_PAD) {
            top = inventory->bottom + IV_BUTTON_GAP;
        } else {
            return 0;
        }
    }

    button->left = left;
    button->top = top;
    button->right = left + IV_BUTTON_W;
    button->bottom = top + IV_BUTTON_H;
    return 1;
}

static int place_control(const iv_rect_t *inventory, iv_rect_t *rect)
{
    if (g_sprite_mode)
        return place_strip(inventory, rect);
    return place_button(inventory, rect);
}

/* ---- drawing ------------------------------------------------------------- */

static void panel_text(int x, int y, const char *text)
{
    iv_drawtext_fn draw = (iv_drawtext_fn)iv_addr[IV_DRAWTEXT];
    void *ctx;

    if (!draw || !iv_addr[IV_DRAWCTX])
        return;
    ctx = *(void **)iv_addr[IV_DRAWCTX];
    if (ctx)
        draw(x, y, text, 1, ctx);
}

static void panel_box(const iv_rect_t *rect)
{
    iv_drawbox_fn draw = (iv_drawbox_fn)iv_addr[IV_DRAWINFOBOX];
    void *ctx;

    if (!draw || !iv_addr[IV_DRAWCTX] || !iv_addr[IV_INFOFRAMES] ||
            !*(void **)iv_addr[IV_INFOFRAMES])
        return;
    ctx = *(void **)iv_addr[IV_DRAWCTX];
    if (ctx)
        draw(rect->left, rect->top, rect->right - rect->left,
             rect->bottom - rect->top, ctx);
}

static void draw_sprites(const iv_rect_t *strip)
{
    int hovered = button_at_cursor(strip);
    int button;

    for (button = 0; button < IV_BTN_COUNT; button++) {
        int state = button_state(button, button == hovered);
        int x = strip->left + button * IV_SPRITE_PITCH;

        if (sprite_blit(IV_ANIM[state][button], x, strip->top))
            continue;
        if (!g_blit_warned) {
            g_blit_warned = 1;
            iv_log("panel: blit refused \"%s\"; handle or animation name is "
                   "not in %s", IV_ANIM[state][button], IV_MDA_PATH);
        }
    }
}

static void draw_text_control(const iv_rect_t *rect)
{
    panel_box(rect);
    if (iv_sort_busy())
        panel_text(rect->left + 10, rect->top + 7, "~C4Sorting...~C0");
    else
        panel_text(rect->left + 28, rect->top + 7, "Sort");
}

/* The node's own draw proc runs inside the manager's walk, which is exactly
 * where a raised stock window would paint over us. Sprite mode paints from the
 * overlay instead and leaves this empty. */
static void __cdecl panel_draw(void)
{
    iv_rect_t rect;

    if (g_sprite_mode || g_handle < 0)
        return;
    if (iv_node_rect(g_handle, &rect, NULL) != 0)
        return;
    draw_text_control(&rect);
}

void iv_panel_overlay(void)
{
    iv_rect_t rect;

    if (!g_sprite_mode || g_handle < 0)
        return;
    if (iv_node_rect(g_handle, &rect, NULL) != 0)
        return;
    draw_sprites(&rect);
}

/* ---- input --------------------------------------------------------------- */

static void consume_event(void)
{
    if (iv_addr[IV_EVENTCODE])
        *(unsigned char *)iv_addr[IV_EVENTCODE] = 0;
}

static void click_button(int button)
{
    if (button == IV_BTN_SORT) {
        iv_sort_request();
        return;
    }
    if (button == IV_BTN_LOCK)
        iv_lock_request_active();
}

static void __cdecl panel_input(void)
{
    iv_rect_t rect;
    int button;

    if (g_handle < 0 || !iv_addr[IV_EVENTCODE])
        return;
    if (event_code() != IV_EVENT_CLICK) {
        g_click_armed = 1;
        return;
    }

    if (!g_sprite_mode) {
        /* The sidecar owns its whole rect and sits outside every stock
         * window, so being dispatched is enough. */
        if (g_click_armed) {
            g_click_armed = 0;
            iv_sort_request();
        }
        consume_event();
        return;
    }

    /* Sprite mode. The manager dispatches to the top node, which is us
     * whenever the cursor is on the strip - but a click can still arrive here
     * for a rect we do not own. Leave those completely alone: consuming one
     * eats an inventory click and the player loses the whole window. The 5px
     * gap between the buttons counts as not ours. */
    if (iv_node_rect(g_handle, &rect, NULL) != 0)
        return;
    button = button_at_cursor(&rect);
    if (button == IV_BTN_NONE)
        return;
    if (g_click_armed) {
        g_click_armed = 0;
        click_button(button);
    }
    consume_event();
}

/* ---- lifecycle ----------------------------------------------------------- */

static void panel_close(void)
{
    iv_uidestroy_fn destroy = (iv_uidestroy_fn)iv_addr[IV_UIDESTROYNODE];

    if (g_handle < 0)
        return;
    if (destroy)
        destroy(g_handle, 0);
    g_handle = -1;
    g_inventory_handle = -1;
    g_click_armed = 1;
    g_was_hovered = 0;
    consume_event();
}

/* The window is MapMenu.MDA sub-file 4 and is a fixed 376x458. Sprite placement
 * is measured against that artwork, so say so once if it ever is not. */
static void check_panel_size(const iv_rect_t *inventory)
{
    int width = inventory->right - inventory->left;
    int height = inventory->bottom - inventory->top;

    if (g_size_warned || (width == IV_PANEL_ART_W && height == IV_PANEL_ART_H))
        return;
    g_size_warned = 1;
    iv_log("panel: inventory window is %dx%d, expected %dx%d; button placement "
           "is measured against the stock artwork", width, height,
           IV_PANEL_ART_W, IV_PANEL_ART_H);
}

static void panel_open(int inventory_handle, const iv_rect_t *inventory)
{
    iv_uicreate_fn create = (iv_uicreate_fn)iv_addr[IV_UICREATENODE];
    iv_rect_t rect;

    if (!create || g_handle >= 0)
        return;
    if (g_sprite_mode)
        check_panel_size(inventory);
    if (!place_control(inventory, &rect))
        return;
    g_click_armed = 1;
    g_handle = create(rect.left, rect.top, rect.right, rect.bottom,
                      (void *)panel_draw, (void *)panel_input, 0, 0);
    if (g_handle >= 0) {
        g_inventory_handle = inventory_handle;
        iv_log("panel: %s control opened as node %d",
               g_sprite_mode ? "sort/lock" : "sort", g_handle);
    } else {
        iv_log("panel: UiCreateNode failed");
    }
}

/* Back to the end of the node list, so the manager finds our rect before the
 * stock inventory window that the player's last click raised. Called only on
 * the frame the cursor enters the strip; see the Z-order note above. Click
 * state is carried across deliberately. */
static void raise_node(int inventory_handle, const iv_rect_t *inventory)
{
    iv_uidestroy_fn destroy = (iv_uidestroy_fn)iv_addr[IV_UIDESTROYNODE];
    iv_uicreate_fn create = (iv_uicreate_fn)iv_addr[IV_UICREATENODE];
    iv_rect_t rect;
    int armed = g_click_armed;
    int handle;

    if (!destroy || !create || g_handle < 0)
        return;
    if (!place_control(inventory, &rect))
        return;

    destroy(g_handle, 0);
    handle = create(rect.left, rect.top, rect.right, rect.bottom,
                    (void *)panel_draw, (void *)panel_input, 0, 0);
    if (handle < 0) {
        /* The node is gone either way; let the next tick reopen it cleanly. */
        g_handle = -1;
        g_inventory_handle = -1;
        g_click_armed = 1;
        return;
    }
    g_handle = handle;
    g_inventory_handle = inventory_handle;
    g_click_armed = armed;
}

static void panel_sync(const iv_rect_t *inventory)
{
    iv_rect_t current, wanted;
    int flag = 0;

    if (g_handle < 0 || iv_node_rect(g_handle, &current, &flag) != 0)
        return;
    if (!place_control(inventory, &wanted)) {
        panel_close();
        return;
    }
    if (memcmp(&current, &wanted, sizeof(current)) != 0)
        iv_node_set_rect(g_handle, &wanted, flag);
}

void iv_panel_tick(void)
{
    int inventory_handle = stock_inventory_handle();
    iv_rect_t inventory;

    if (inventory_handle < 0 ||
            iv_node_rect(inventory_handle, &inventory, NULL) != 0) {
        panel_close();
        return;
    }
    if (g_handle < 0)
        panel_open(inventory_handle, &inventory);
    else if (g_inventory_handle != inventory_handle) {
        panel_close();
        panel_open(inventory_handle, &inventory);
    } else if (g_sprite_mode) {
        iv_rect_t strip;
        int hovered = 0;

        /* Follow the window first. The raise below is edge-triggered and may
         * not run for many frames, so it cannot be what keeps the strip under
         * Vendor while the player drags the panel around. */
        panel_sync(&inventory);
        if (iv_node_rect(g_handle, &strip, NULL) == 0)
            hovered = button_at_cursor(&strip) != IV_BTN_NONE;
        /* Only on the entering edge. Holding the top slot for as long as the
         * cursor rests here would be harmless, but raising every frame is what
         * made the module swallow the whole window. */
        if (hovered && !g_was_hovered)
            raise_node(inventory_handle, &inventory);
        g_was_hovered = hovered;
    } else {
        panel_sync(&inventory);
    }
}

int iv_panel_install(void)
{
    g_handle = -1;
    g_inventory_handle = -1;
    g_click_armed = 1;
    g_bank = NULL;
    g_bank_tried = 0;
    g_blit_warned = 0;
    g_size_warned = 0;
    g_was_hovered = 0;

    /* Mode is decided here rather than per frame so the control cannot change
     * shape mid-session. The bank itself still loads lazily, and a load failure
     * is caught on the first draw by sprite_blit returning 0. */
    g_sprite_mode = iv_addr[IV_LOADMDA] != NULL &&
                    iv_addr[IV_BLITMDA] != NULL &&
                    iv_addr[IV_MOUSEX] != NULL &&
                    iv_addr[IV_MOUSEY] != NULL;

    if (g_sprite_mode)
        iv_log("panel: in-window sort/lock buttons ready (%s)", IV_MDA_PATH);
    else
        iv_log("panel: no sprite blitter; stock inventory companion control "
               "ready");
    return 1;
}

void iv_panel_remove(void)
{
    panel_close();
    /* The client owns the MDA it loaded; the module only drops its reference so
     * a reload re-resolves through the loader. */
    g_bank = NULL;
    g_bank_tried = 0;
    g_blit_warned = 0;
    g_size_warned = 0;
    g_was_hovered = 0;
    g_sprite_mode = 0;
}
