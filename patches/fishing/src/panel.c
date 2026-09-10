#include "osmw_fishing.h"

#include <stdio.h>
#include <string.h>

#define FG_SCREEN_W  800
#define FG_SCREEN_H  600
#define FG_PANEL_W   456
#define FG_PANEL_H   280
#define FG_PANEL_L   ((FG_SCREEN_W - FG_PANEL_W) / 2)
#define FG_PANEL_T   ((FG_SCREEN_H - FG_PANEL_H) / 2)

#define FG_BTN_W      96
#define FG_BTN_H      24
#define FG_BTN_GAP     8
#define FG_BTN_COUNT   4
#define FG_BTN_X      24
#define FG_BTN_Y     200

#define FG_MDA_PATH   "system\\osmw_fishing.mda"
#define FG_BLIT_FLAGS 0x12
#define FG_EVENT_CLICK 0xF5

typedef int (__cdecl *fg_uicreate_fn)(int left, int top, int right, int bottom,
                                       void *draw_proc, void *input_proc,
                                       int arg4, int list_sel);
typedef void (__cdecl *fg_uidestroy_fn)(int handle, int list_sel);
typedef void (__cdecl *fg_drawtext_fn)(int x, int y, const char *text,
                                       int mode, void *ctx);
typedef void (__cdecl *fg_drawbox_fn)(int x, int y, int w, int h, void *ctx);
typedef int (__cdecl *fg_nodegeom_fn)(int handle, fg_rect_t *rect, int *flag,
                                      int list_sel);
typedef void (__cdecl *fg_loadmda_fn)(const char *path, void **out_handle);
typedef int (__cdecl *fg_blitmda_fn)(int handle, const char *anim, int x, int y,
                                     void *surface, int flags, int a, int b,
                                     int c);
typedef int (__cdecl *fg_textwidth_fn)(const char *text);

static void __cdecl panel_draw(void);
static void __cdecl panel_input(void);

static int g_handle = -1;
static int g_click_armed = 1;
static int g_bank_tried;
static int g_blit_warned;
static void *g_bank;
static DWORD g_anim_tick;

int fg_node_rect(int handle, fg_rect_t *out_rect, int *out_flag)
{
    fg_nodegeom_fn fn = (fg_nodegeom_fn)fg_addr[FG_NODEGEOMETRY];
    int flag = 0;

    if (!fn || !out_rect)
        return -1;
    return fn(handle, out_rect, out_flag ? out_flag : &flag, 0);
}

int fg_client_in_world(void)
{
    if (!fg_addr[FG_GRIDSCALEX])
        return 0;
    return *(int *)fg_addr[FG_GRIDSCALEX] != 0;
}

static void panel_text(int x, int y, const char *text)
{
    fg_drawtext_fn draw;
    void *ctx;

    if (!fg_addr[FG_DRAWTEXT] || !fg_addr[FG_DRAWCTX] || !text)
        return;
    ctx = *(void **)fg_addr[FG_DRAWCTX];
    if (!ctx)
        return;
    draw = (fg_drawtext_fn)fg_addr[FG_DRAWTEXT];
    draw(x, y, text, 1, ctx);
}

static void panel_box(const fg_rect_t *rect)
{
    fg_drawbox_fn box;
    void *ctx;

    if (!fg_addr[FG_DRAWINFOBOX] || !fg_addr[FG_DRAWCTX] ||
        !fg_addr[FG_INFOFRAMES] || !*(void **)fg_addr[FG_INFOFRAMES])
        return;
    ctx = *(void **)fg_addr[FG_DRAWCTX];
    if (!ctx)
        return;
    box = (fg_drawbox_fn)fg_addr[FG_DRAWINFOBOX];
    box(rect->left, rect->top, rect->right - rect->left,
        rect->bottom - rect->top, ctx);
}

static int sprite_bank(void)
{
    fg_loadmda_fn load;

    if (g_bank_tried)
        return g_bank != NULL;
    g_bank_tried = 1;
    load = (fg_loadmda_fn)fg_addr[FG_LOADMDA];
    if (!load) {
        fg_log("panel: LoadMda unresolved; scene falls back to text");
        return 0;
    }
    load(FG_MDA_PATH, &g_bank);
    if (!g_bank) {
        fg_log("panel: %s did not load; scene falls back to text", FG_MDA_PATH);
        return 0;
    }
    fg_log("panel: %s loaded, handle %p", FG_MDA_PATH, g_bank);
    return 1;
}

static int sprite_blit(const char *anim, int x, int y)
{
    fg_blitmda_fn blit = (fg_blitmda_fn)fg_addr[FG_BLITMDA];
    int ok;

    if (!blit || !fg_addr[FG_DRAWCTX] || !anim || !sprite_bank())
        return 0;
    ok = blit((int)g_bank, anim, x, y, *(void **)fg_addr[FG_DRAWCTX],
              FG_BLIT_FLAGS, 0, 0, 0) != 0;
    if (!ok && !g_blit_warned) {
        g_blit_warned = 1;
        fg_log("panel: blit refused \"%s\"; handle or animation name is not in %s",
               anim, FG_MDA_PATH);
    }
    return ok;
}

static const char *scene_anim(const fg_view_t *view)
{
    static char reveal[16];

    switch (view->phase) {
    case FG_PHASE_READY:
    case FG_PHASE_NOBITE:
    case FG_PHASE_NOBAIT:
    case FG_PHASE_SLACK:
        return "Casting";
    case FG_PHASE_SNAP:
        return "Snap";
    case FG_PHASE_ESCAPE:
        return "Escape";
    case FG_PHASE_LAND:
        if (view->species_index >= 0 && view->species_index <= 9) {
            _snprintf(reveal, sizeof(reveal), "Reveal%d", view->species_index);
            reveal[sizeof(reveal) - 1] = '\0';
            return reveal;
        }
        return "Shadow";
    case FG_PHASE_HOOKED:
        if (view->hint == FG_HINT_LEFT)
            return "ThrashLeft";
        if (view->hint == FG_HINT_RIGHT)
            return "ThrashRight";
        return "Shadow";
    default:
        return "Shadow";
    }
}

static void strip_codes(const char *in, char *out, int size)
{
    int i = 0, o = 0;
    if (!in || !out || size <= 0)
        return;
    while (in[i] != '\0' && o < size - 1) {
        if (in[i] == '#' && in[i + 1] != '\0') {
            i += 1;
            if (in[i] == 'c' || in[i] == 'C') {
                i += 1;
                while (in[i] != '\0' && i < 8 &&
                       ((in[i] >= '0' && in[i] <= '9') ||
                        (in[i] >= 'A' && in[i] <= 'F') ||
                        (in[i] >= 'a' && in[i] <= 'f')))
                    i += 1;
                continue;
            }
            if (in[i] >= '0' && in[i] <= '9') {
                while (in[i] >= '0' && in[i] <= '9')
                    i += 1;
                continue;
            }
            i += 1;
            continue;
        }
        out[o++] = in[i++];
    }
    out[o] = '\0';
}

static void draw_bar(int x, int y, unsigned int value, const char *label,
                     const char *fill_colour)
{
    char line[64];
    unsigned int filled = value / 10;
    unsigned int i;

    if (filled > 10)
        filled = 10;
    _snprintf(line, sizeof(line), "%s %s", label, fill_colour);
    line[sizeof(line) - 1] = '\0';
    for (i = 0; i < filled && strlen(line) < sizeof(line) - 2; ++i)
        strcat(line, "|");
    strcat(line, "~C0");
    for (i = filled; i < 10 && strlen(line) < sizeof(line) - 2; ++i)
        strcat(line, ".");
    panel_text(x, y, line);
}

static void button_rect(const fg_rect_t *panel, int index, fg_rect_t *out)
{
    int left = panel->left + FG_BTN_X + index * (FG_BTN_W + FG_BTN_GAP);
    out->left = left;
    out->top = panel->top + FG_BTN_Y;
    out->right = left + FG_BTN_W;
    out->bottom = out->top + FG_BTN_H;
}

static unsigned int button_command(const fg_view_t *view, int index)
{
    if (view->phase == FG_PHASE_HOOKED) {
        if (index == 0) return FG_CMD_REEL_LEFT;
        if (index == 1) return FG_CMD_REEL_RIGHT;
        if (index == 2) return FG_CMD_HOLD;
        if (index == 3) return FG_CMD_SLACK;
        return 0;
    }
    if (index == 0 && (view->flags & FG_FLAG_CAN_CAST))
        return FG_CMD_CAST;
    if (index == 3)
        return FG_CMD_PACK_UP;
    return 0;
}

static const char *button_label(const fg_view_t *view, int index)
{
    unsigned int command = button_command(view, index);
    switch (command) {
    case FG_CMD_CAST: return "Cast";
    case FG_CMD_REEL_LEFT: return "Reel Left";
    case FG_CMD_REEL_RIGHT: return "Reel Right";
    case FG_CMD_HOLD: return "Hold";
    case FG_CMD_SLACK: return "Slack";
    case FG_CMD_PACK_UP: return "Pack Up";
    default: return "";
    }
}

static int cursor_xy(int *x, int *y)
{
    if (!fg_addr[FG_MOUSEX] || !fg_addr[FG_MOUSEY])
        return 0;
    *x = *(int *)fg_addr[FG_MOUSEX];
    *y = *(int *)fg_addr[FG_MOUSEY];
    return 1;
}

static int button_at_cursor(const fg_rect_t *panel)
{
    fg_rect_t rect;
    int mx, my, index;

    if (!cursor_xy(&mx, &my))
        return -1;
    for (index = 0; index < FG_BTN_COUNT; ++index) {
        button_rect(panel, index, &rect);
        if (mx >= rect.left && mx < rect.right &&
            my >= rect.top && my < rect.bottom)
            return index;
    }
    return -1;
}

static int event_code(void)
{
    if (!fg_addr[FG_EVENTCODE])
        return 0;
    return *(unsigned char *)fg_addr[FG_EVENTCODE];
}

static void panel_consume_event(void)
{
    if (fg_addr[FG_EVENTCODE])
        *(unsigned char *)fg_addr[FG_EVENTCODE] = 0;
}

static void __cdecl panel_draw(void)
{
    fg_rect_t rect = {0, 0, 0, 0};
    fg_view_t view;
    char plain[FG_MESSAGE_BYTES];
    char status[96];
    char slots[32];
    const char *cue;
    int i, x;
    unsigned int bit;

    if (g_handle < 0 || fg_node_rect(g_handle, &rect, 0) != 0)
        return;

    fg_fishing_view(&view);
    panel_box(&rect);
    if (!view.valid)
        return;

    if (!sprite_blit(scene_anim(&view), rect.left + 180, rect.top + 28)) {
        panel_text(rect.left + 20, rect.top + 24, "~C4Fishing~C0");
        if (view.hint == FG_HINT_LEFT)
            panel_text(rect.left + 200, rect.top + 48, "~C1<==~C0");
        else if (view.hint == FG_HINT_RIGHT)
            panel_text(rect.left + 200, rect.top + 48, "~C1==>~C0");
        else
            panel_text(rect.left + 188, rect.top + 48, "  ~  ");
    }

    cue = view.hint == FG_HINT_LEFT ? "~C1Pulls LEFT~C0" :
          view.hint == FG_HINT_RIGHT ? "~C1Pulls RIGHT~C0" : "";
    panel_text(rect.left + 24, rect.top + 108, cue);

    draw_bar(rect.left + 24, rect.top + 128, view.progress, "Line", "~C5");
    draw_bar(rect.left + 24, rect.top + 148,
             view.tension, "Tension",
             view.tension >= 80 ? "~C1" : view.tension >= 50 ? "~C3" : "~C2");

    strip_codes(view.message, plain, sizeof(plain));
    panel_text(rect.left + 24, rect.top + 170, plain);

    for (i = 0; i < FG_BTN_COUNT; ++i) {
        fg_rect_t btn;
        const char *label = button_label(&view, i);
        button_rect(&rect, i, &btn);
        if (label[0] == '\0')
            continue;
        panel_text(btn.left + 8, btn.top + 6,
                   fg_fishing_input_locked() ? "~C4...~C0" : label);
    }

    _snprintf(status, sizeof(status), "Bait %u   Lv %u  %u/%u",
              view.bait_count, view.fishing_level, view.fishing_xp,
              view.next_level_xp);
    status[sizeof(status) - 1] = '\0';
    panel_text(rect.left + 24, rect.top + 232, status);

    x = 0;
    slots[0] = '\0';
    for (bit = 0; bit < 10 && x < (int)sizeof(slots) - 2; ++bit) {
        slots[x++] = (view.collection_mask & (1u << bit)) ? 'o' : '.';
        slots[x++] = ' ';
    }
    slots[x] = '\0';
    panel_text(rect.left + 24, rect.top + 252, slots);
    g_anim_tick++;
}

static void __cdecl panel_input(void)
{
    fg_rect_t rect;
    fg_view_t view;
    int index;
    unsigned int command;

    if (g_handle < 0 || !fg_addr[FG_EVENTCODE])
        return;
    if (event_code() != FG_EVENT_CLICK) {
        g_click_armed = 1;
        return;
    }
    panel_consume_event();
    if (!g_click_armed)
        return;
    g_click_armed = 0;

    if (fg_node_rect(g_handle, &rect, 0) != 0)
        return;
    fg_fishing_view(&view);
    if (!view.valid || fg_fishing_input_locked())
        return;
    index = button_at_cursor(&rect);
    if (index < 0)
        return;
    command = button_command(&view, index);
    if (command)
        fg_fishing_queue_action(command);
}

static void panel_open(void)
{
    fg_uicreate_fn create = (fg_uicreate_fn)fg_addr[FG_UICREATENODE];

    if (!create || g_handle >= 0)
        return;
    g_click_armed = 1;
    g_handle = create(FG_PANEL_L, FG_PANEL_T, FG_PANEL_L + FG_PANEL_W,
                      FG_PANEL_T + FG_PANEL_H,
                      (void *)panel_draw, (void *)panel_input, 0, 0);
    if (g_handle < 0) {
        fg_log("panel: UiCreateNode failed");
        return;
    }
    fg_log("panel: opened, node %d at (%d,%d)-(%d,%d)", g_handle,
           FG_PANEL_L, FG_PANEL_T, FG_PANEL_L + FG_PANEL_W,
           FG_PANEL_T + FG_PANEL_H);
}

static void panel_close(void)
{
    fg_uidestroy_fn destroy = (fg_uidestroy_fn)fg_addr[FG_UIDESTROYNODE];

    if (g_handle < 0)
        return;
    if (destroy)
        destroy(g_handle, 0);
    fg_log("panel: closed, node %d", g_handle);
    g_handle = -1;
    g_click_armed = 1;
}

int fg_panel_is_open(void)
{
    return g_handle >= 0;
}

void fg_panel_tick(void)
{
    if (!fg_client_in_world()) {
        if (g_handle >= 0)
            panel_close();
        return;
    }
    if (fg_fishing_want_open()) {
        if (g_handle < 0)
            panel_open();
    } else if (g_handle >= 0) {
        panel_close();
    }
}

int fg_panel_install(void)
{
    fg_log("panel: %dx%d centered, frame tiles %d x %d",
           FG_PANEL_W, FG_PANEL_H,
           (FG_PANEL_W - 16) / 8, (FG_PANEL_H - 16) / 8);
    if (fg_addr[FG_LOADMDA] && fg_addr[FG_BLITMDA])
        fg_log("panel: MDA blitter available (%s)", FG_MDA_PATH);
    else
        fg_log("panel: MDA signatures missing -- text/arrow fallback");
    return 1;
}

void fg_panel_remove(void)
{
    panel_close();
    g_bank = NULL;
    g_bank_tried = 0;
    g_blit_warned = 0;
}
