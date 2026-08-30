/* Phase 5 -- channel separation.
 *
 * Each visible line is a 0x410-byte record at chatobj+0x80 + i*0x410:
 *
 *   +0x000  text, 0x400 bytes, handed to the client's rich-text engine
 *   +0x400  Y offset into the channel chip atlas, or -1 for "no chip"
 *   +0x404  slot index into the global message array
 *   +0x408  line height in pixels
 *   +0x40C  chip Y offset within the line
 *
 * The atlas is a vertical strip of 19px rows, one per channel, so +0x400 is
 * simply channel * 19 -- the channel of every line is readable straight off
 * the record, with no lookup into the global array.
 *
 * Filtering happens at the SOURCE, not at the view. When the render walks the
 * message history to build its list of visible lines, the very first thing it
 * does with each message is
 *
 *     if (channel > 9) skip this message
 *
 * so temporarily giving a message an out-of-range channel makes the client
 * itself behave as though that message never arrived. The rebuild skips it,
 * pulls further back through history to fill the pane, scrolls correctly, and
 * every drawing path agrees -- because there is only one filter, and it is the
 * client's own.
 *
 * This file only decides WHAT is visible. mirror.c does the work of keeping
 * the window's private ring in step; see its header for why the window reads a
 * private copy at all, and NOTES.txt for the three approaches that failed
 * before this one.
 */
#include "osmw_chat.h"

#include <stdio.h>
#include <string.h>

#define CH_LOCAL  0
#define CH_PARTY  1
#define CH_PM     2
#define CH_GUILD  3
#define CH_MARKET 4
#define CH_WORLD  5
#define CH_SYSTEM 6
#define CH_INFO   7
#define CH_FD     8
#define CH_GM     9

#define MASK_ALL     0x3FFu                                  /* every channel */
#define MASK_SYSTEM  ((1u << CH_SYSTEM) | (1u << CH_INFO))
#define MASK_CHAT    (MASK_ALL & ~MASK_SYSTEM)

#define TAB_HOTKEY_ID 0x0C1F
#define TAB_MESSAGE_NAME "osmw_chat_tab"

/* The chat window's client rect, which the render reads to lay the pane out. */
#define OBJ_CLIENT_RECT 0x28
#define OBJ_MEM_DC      0x44

/* Height of the tab strip, in pixels.
 *
 * The render anchors chat text at (top + bottom - 25) and fills upwards until
 * it has used (bottom - 25) pixels. Adding S to `top` and subtracting S from
 * `bottom` for the duration of a pass leaves the anchor exactly where it was
 * while shortening the fill by S -- which frees S pixels at the TOP with the
 * text still hugging the bottom.
 *
 * S must stay under 25. The render's background blit covers (0,0) to
 * (width, bottom - S); the lowest text sits at (bottom - 25), so S < 25 keeps
 * every line over freshly painted background. Go past that and the newest line
 * draws onto stale pixels. */
#define STRIP_H 20

#define TAB_MIN_W 56
#define TAB_MAX_W 110

/* Tuned against the pane's blue; see draw_strip. */
#define COL_STRIP_BG   RGB(14, 20, 56)
#define COL_TAB_ACTIVE RGB(52, 78, 150)
#define COL_TAB_IDLE   RGB(24, 34, 86)
#define COL_SEP        RGB(96, 116, 178)
#define COL_TEXT_ON    RGB(255, 255, 255)
#define COL_TEXT_OFF   RGB(168, 180, 214)
#define COL_UNREAD     RGB(255, 196, 64)
#define COL_MENTION    RGB(255, 72, 72)

/* The render is __thiscall with no stack arguments and returns with a plain
 * `ret`, so __fastcall matches it exactly. */
typedef void (__fastcall *render_fn)(void *ecx, void *edx);

#define MAX_TABS  6
#define TAB_NAME_MAX 16

typedef struct {
    char     name[TAB_NAME_MAX];
    unsigned mask;
} mw_tab_t;

static mw_tab_t TABS[MAX_TABS];
static int      TAB_COUNT;
static int      g_active_tab;
static char     g_tab_spec[256];

/* Channel names accepted in the [Chat] Tabs setting. */
static const struct { const char *name; int channel; } CHANNEL_NAMES[] = {
    { "local", CH_LOCAL }, { "party", CH_PARTY }, { "pm", CH_PM },
    { "guild", CH_GUILD }, { "market", CH_MARKET }, { "world", CH_WORLD },
    { "system", CH_SYSTEM }, { "info", CH_INFO }, { "fd", CH_FD },
    { "gm", CH_GM },
};
#define CHANNEL_NAME_COUNT ((int)(sizeof(CHANNEL_NAMES) / sizeof(CHANNEL_NAMES[0])))

#define DEFAULT_TABS "All:*|Chat:*,-system,-info|System:system,info"

static int channel_by_name(const char *name)
{
    int i;
    for (i = 0; i < CHANNEL_NAME_COUNT; i++) {
        if (!_stricmp(name, CHANNEL_NAMES[i].name))
            return CHANNEL_NAMES[i].channel;
    }
    return -1;
}

/* "All:*|Chat:*,-system,-info|System:system,info"
 *
 * Tabs are separated by '|', a tab is "name:channels", and the channel list is
 * comma separated where '*' means every channel and a leading '-' removes one.
 * Order matters, so "*,-system" reads as "everything except system". */
static void parse_tabs(const char *spec)
{
    char  work[sizeof(g_tab_spec)];
    char *tab, *next_tab;

    TAB_COUNT = 0;
    _snprintf(work, sizeof(work), "%s", spec);
    work[sizeof(work) - 1] = '\0';

    for (tab = work; tab && *tab && TAB_COUNT < MAX_TABS; tab = next_tab) {
        char *colon, *item, *next_item;
        unsigned mask = 0;

        next_tab = strchr(tab, '|');
        if (next_tab)
            *next_tab++ = '\0';

        while (*tab == ' ')
            tab++;
        colon = strchr(tab, ':');
        if (!colon || colon == tab)
            continue;                       /* no name, or no channel list */
        *colon = '\0';

        for (item = colon + 1; item && *item; item = next_item) {
            int remove = 0, ch;

            next_item = strchr(item, ',');
            if (next_item)
                *next_item++ = '\0';
            while (*item == ' ')
                item++;

            if (*item == '-') {
                remove = 1;
                item++;
            }
            if (!strcmp(item, "*")) {
                mask = remove ? 0 : MASK_ALL;
                continue;
            }
            ch = channel_by_name(item);
            if (ch < 0) {
                mw_log("tabs: unknown channel \"%s\" in Tabs, ignored", item);
                continue;
            }
            if (remove)
                mask &= ~(1u << ch);
            else
                mask |= (1u << ch);
        }

        _snprintf(TABS[TAB_COUNT].name, TAB_NAME_MAX, "%s", tab);
        TABS[TAB_COUNT].name[TAB_NAME_MAX - 1] = '\0';
        TABS[TAB_COUNT].mask = mask;
        TAB_COUNT++;
    }

    if (TAB_COUNT == 0) {
        mw_log("tabs: Tabs setting produced nothing usable, using the default");
        parse_tabs(DEFAULT_TABS);
        return;
    }

    {
        int i;
        for (i = 0; i < TAB_COUNT; i++)
            mw_log("tabs:   [%d] %-10s mask 0x%03X", i, TABS[i].name, TABS[i].mask);
    }
}

/* Re-read the tab definitions if the setting changed. Returns 1 if it did. */
static int refresh_tabs(void)
{
    const char *spec = mw_cfg_str("Tabs", DEFAULT_TABS);

    if (!strcmp(spec, g_tab_spec))
        return 0;

    _snprintf(g_tab_spec, sizeof(g_tab_spec), "%s", spec);
    g_tab_spec[sizeof(g_tab_spec) - 1] = '\0';
    mw_log("tabs: reading Tabs=%s", g_tab_spec);
    parse_tabs(g_tab_spec);

    if (g_active_tab >= TAB_COUNT)
        g_active_tab = 0;
    return 1;
}

/* Enough of the message ring to spot traffic on the tabs you are not looking
 * at; mirror.c owns the ring itself. */
#define MSG_STRIDE    0xC0
#define MSG_SLOTS     1024
#define MSG_SENDER    0x00
#define MSG_CHANNEL   0x04
#define MSG_EMPTY     0x80000000u
#define CNT_HEAD      (-0x10)
#define CHANNEL_COUNT 10

enum {
    BRUSH_STRIP_BG, BRUSH_TAB_ACTIVE, BRUSH_TAB_IDLE,
    BRUSH_SEP, BRUSH_UNREAD, BRUSH_MENTION, BRUSH_COUNT
};

static mw_hook_t     g_render_hook;
static render_fn     g_render_orig;
static mw_hook_t     g_present_hook;
static render_fn     g_present_orig;
static int           g_installed;
static int           g_strip_on = 1;
static int           g_shifted;      /* the client rect is currently shifted */
static HFONT         g_strip_font;
static HBRUSH        g_brush[BRUSH_COUNT];
static int           g_unread[MAX_TABS];
static int           g_mention[MAX_TABS];
static int           g_unread_seen = -1;
static int           g_hotkey_registered;
static char          g_hotkey_spec[64];
static UINT          g_tab_msg;
static UINT          g_probe_msg;
static char          g_base_title[96];


static unsigned active_mask(void)
{
    if (g_active_tab < 0 || g_active_tab >= TAB_COUNT)
        return MASK_ALL;
    return TABS[g_active_tab].mask;
}

unsigned mw_tabs_mask(void)
{
    return active_mask();
}

/* How much the chat window's client rect is currently displaced to make room
 * for the strip. Anything that refreshes that rect from the real window has to
 * re-apply this, or it will silently undo the reservation -- which is exactly
 * what geometry.c's cache refresh did, since the reposition it hangs off runs
 * inside the render. */
int mw_tabs_rect_shift(void)
{
    return g_shifted ? STRIP_H : 0;
}

/* Reflect the active tab in the window caption. Until the tab strip is drawn
 * inside the pane, this is the only indication of which filter is on. */
static void update_title(void)
{
    HWND hwnd = mw_window_hwnd();
    char title[160];

    if (!hwnd || !IsWindow(hwnd))
        return;
    if (!g_base_title[0]) {
        GetWindowTextA(hwnd, g_base_title, sizeof(g_base_title) - 1);
        g_base_title[sizeof(g_base_title) - 1] = '\0';
    }

    if (g_active_tab == 0)
        _snprintf(title, sizeof(title), "%s", g_base_title);
    else
        _snprintf(title, sizeof(title), "%s  -  %s only",
                  g_base_title, TABS[g_active_tab].name);
    title[sizeof(title) - 1] = '\0';
    SetWindowTextA(hwnd, title);
}

/* Cached, because the render and the present both consult it many times a
 * second and mw_cfg_int hits the ini file. Refreshed from the settings poll. */
static int strip_enabled(void)
{
    return g_strip_on;
}

static void fill_solid(HDC dc, const RECT *r, int which)
{
    if (g_brush[which])
        FillRect(dc, r, g_brush[which]);
}

/* Mark tabs that have taken traffic while you were looking elsewhere. Reads
 * the real ring, not the mirror: the mirror only holds what passes the current
 * filter, which is exactly what we are NOT interested in here. */
static void note_unread(void)
{
    unsigned char *msgs = mw_addr[MW_MSGARRAY];
    int head, i;

    if (!msgs)
        return;
    head = *(int *)(msgs + CNT_HEAD);

    if (g_unread_seen < 0) {        /* first look: take the ring as read */
        g_unread_seen = head;
        return;
    }

    while (g_unread_seen != head) {
        const unsigned char *rec = msgs + (size_t)g_unread_seen * MSG_STRIDE;
        unsigned channel = *(const unsigned *)(rec + MSG_CHANNEL);

        if (*(const unsigned *)(rec + MSG_SENDER) != MSG_EMPTY &&
            channel < CHANNEL_COUNT) {
            int mentions = mw_mention_matches(rec);

            for (i = 0; i < TAB_COUNT; i++) {
                if (i == g_active_tab || !(TABS[i].mask & (1u << channel)))
                    continue;
                g_unread[i] = 1;
                if (mentions)
                    g_mention[i] = 1;
            }
        }
        g_unread_seen = (g_unread_seen + 1) % MSG_SLOTS;
    }
}

/* Where tab `i` sits along the top of the pane. */
static void tab_rect(int i, int pane_w, RECT *out)
{
    int w = pane_w / TAB_COUNT;

    if (w > TAB_MAX_W)
        w = TAB_MAX_W;
    if (w < TAB_MIN_W)
        w = TAB_MIN_W;

    out->left = i * w;
    out->right = out->left + w - 1;
    out->top = 0;
    out->bottom = STRIP_H - 1;
}

static void draw_strip(unsigned char *obj)
{
    HDC   dc = *(HDC *)(obj + OBJ_MEM_DC);
    RECT *client = (RECT *)(obj + OBJ_CLIENT_RECT);
    int   pane_w = client->right - client->left;
    RECT  band;
    HGDIOBJ old_font;
    int    old_mode, i;
    COLORREF old_colour;

    if (!dc || pane_w <= 0)
        return;

    band.left = 0;
    band.top = 0;
    band.right = pane_w;
    band.bottom = STRIP_H;
    fill_solid(dc, &band, BRUSH_STRIP_BG);

    old_font = g_strip_font ? SelectObject(dc, g_strip_font) : NULL;
    old_mode = SetBkMode(dc, TRANSPARENT);
    old_colour = GetTextColor(dc);

    for (i = 0; i < TAB_COUNT; i++) {
        RECT r;
        int active = (i == g_active_tab);

        tab_rect(i, pane_w, &r);
        if (r.left >= pane_w)
            break;
        if (r.right > pane_w)
            r.right = pane_w;

        fill_solid(dc, &r, active ? BRUSH_TAB_ACTIVE : BRUSH_TAB_IDLE);

        SetTextColor(dc, active ? COL_TEXT_ON : COL_TEXT_OFF);
        DrawTextA(dc, TABS[i].name, -1, &r,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        /* A dot on an inactive tab that has taken traffic since you left it. */
        if (!active && g_unread[i]) {
            RECT dot;
            dot.left = r.right - 9;
            dot.top = 4;
            dot.right = r.right - 4;
            dot.bottom = 9;
            fill_solid(dc, &dot, g_mention[i] ? BRUSH_MENTION : BRUSH_UNREAD);
        }
    }

    /* A hairline under the strip, so it reads as chrome rather than as chat. */
    band.top = STRIP_H - 1;
    band.bottom = STRIP_H;
    fill_solid(dc, &band, BRUSH_SEP);

    SetTextColor(dc, old_colour);
    SetBkMode(dc, old_mode);
    if (old_font)
        SelectObject(dc, old_font);
}

static void __fastcall render_detour(void *self, void *edx)
{
    unsigned char *obj = (unsigned char *)self;
    RECT *client = (RECT *)(obj + OBJ_CLIENT_RECT);
    int shifted = 0;

    /* The window reads a private copy of the message ring; keeping that copy
     * filtered is the whole of the feature. Everything downstream -- chips,
     * markup, emoticons, wrapping, scrolling -- is the client's own code
     * working on an ordinary, well-formed ring. */
    mw_window_poll_settings();
    mw_mirror_sync(active_mask());
    note_unread();

    if (g_strip_on && (client->bottom - client->top) > STRIP_H * 3) {
        client->top += STRIP_H;
        client->bottom -= STRIP_H;
        shifted = 1;
    }

    g_shifted = shifted;
    g_render_orig(self, edx);
    g_shifted = 0;

    if (shifted) {
        client->top -= STRIP_H;
        client->bottom += STRIP_H;
    }
}

/* Draw the strip immediately before the offscreen surface is copied to the
 * window.
 *
 * It cannot be drawn at the end of the render: the render presents near its
 * own end, and the next pass opens by blitting the background over the whole
 * surface. Anything drawn after the present is wiped before it is ever shown,
 * which is exactly how the first version behaved -- the pane rendered with the
 * gap reserved at the top and nothing in it.
 *
 * This helper is shared with the PHIZ window, so only decorate our own object.
 */
static void __fastcall present_detour(void *self, void *edx)
{
    unsigned char *obj = (unsigned char *)self;
    RECT *client;

    if (!g_strip_on || self != (void *)mw_addr[MW_OBJECT]) {
        g_present_orig(self, edx);
        return;
    }

    /* The render calls this from inside itself, while the client rect is still
     * shifted down by the strip height. The blit takes its destination x/y and
     * its width/height straight from that rect, so a shifted rect would copy
     * the surface 20px down the window and leave a stale band at the top.
     * Put the true rect back for the duration of the copy. */
    client = (RECT *)(obj + OBJ_CLIENT_RECT);
    if (g_shifted) {
        client->top -= STRIP_H;
        client->bottom += STRIP_H;
    }

    draw_strip(obj);
    g_present_orig(self, edx);

    if (g_shifted) {
        client->top += STRIP_H;
        client->bottom -= STRIP_H;
    }
}

void mw_tabs_set(int index, const char *why)
{
    HWND hwnd = mw_window_hwnd();

    if (index < 0)
        index = TAB_COUNT - 1;
    if (index >= TAB_COUNT)
        index = 0;
    g_active_tab = index;
    g_unread[index] = 0;
    g_mention[index] = 0;

    update_title();

    /* Renumbers the private ring, so this also drops the window's cached line
     * list and forces a repaint. */
    mw_mirror_rebuild(active_mask());
    if (hwnd && IsWindow(hwnd))
        InvalidateRect(hwnd, NULL, FALSE);

    mw_log("tabs: showing \"%s\" (mask 0x%03X) (%s)",
           TABS[g_active_tab].name, TABS[g_active_tab].mask, why);
}

void mw_tabs_cycle(const char *why)
{
    mw_tabs_set(g_active_tab + 1, why);
}

int mw_tabs_on_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT *result)
{
    if (msg == WM_LBUTTONDOWN && strip_enabled()) {
        int x = (int)(short)LOWORD(lp);
        int y = (int)(short)HIWORD(lp);
        RECT client;

        if (y >= 0 && y < STRIP_H && GetClientRect(hwnd, &client)) {
            int pane_w = client.right - client.left;
            int i;

            for (i = 0; i < TAB_COUNT; i++) {
                RECT r;
                tab_rect(i, pane_w, &r);
                if (x >= r.left && x <= r.right) {
                    mw_tabs_set(i, "tab clicked");
                    *result = 0;
                    return 1;       /* swallow it: not a drag on the pane */
                }
            }
            /* A click on the empty part of the strip is still not chat. */
            *result = 0;
            return 1;
        }
    }

    if (g_probe_msg && msg == g_probe_msg) {
        mw_probe_colours();
        *result = 1;
        return 1;
    }

    if (g_tab_msg && msg == g_tab_msg) {
        /* wParam 0 cycles; 1..N selects a tab directly */
        if (wp == 0)
            mw_tabs_cycle("posted message");
        else
            mw_tabs_set((int)wp - 1, "posted message");
        *result = 1;
        return 1;
    }

    if (msg == WM_HOTKEY && wp == TAB_HOTKEY_ID) {
        mw_tabs_cycle("hotkey");
        *result = 0;
        return 1;
    }

    (void)lp;
    return 0;
}

void mw_tabs_sync_hotkey(HWND hwnd)
{
    const char *spec;

    /* Piggy-backs on the settings poll; see strip_enabled. */
    g_strip_on = mw_cfg_int("TabStrip", 1) != 0;

    /* Editing Tabs redefines what each one shows, so the mirror has to be
     * rebuilt under the new mask. */
    if (refresh_tabs() && g_installed)
        mw_tabs_set(g_active_tab, "tab definitions changed");

    spec = mw_cfg_str("TabHotkey", "");
    if (!hwnd || !IsWindow(hwnd))
        return;
    if (!strcmp(spec, g_hotkey_spec))
        return;

    _snprintf(g_hotkey_spec, sizeof(g_hotkey_spec), "%s", spec);
    g_hotkey_spec[sizeof(g_hotkey_spec) - 1] = '\0';

    if (g_hotkey_registered) {
        UnregisterHotKey(hwnd, TAB_HOTKEY_ID);
        g_hotkey_registered = 0;
    }
    if (!spec[0])
        return;

    {
        UINT mods = 0, vk = 0;
        if (!mw_parse_hotkey(spec, &mods, &vk)) {
            mw_log("tabs: could not parse TabHotkey \"%s\" -- unbound", spec);
            return;
        }
        if (RegisterHotKey(hwnd, TAB_HOTKEY_ID, mods, vk)) {
            g_hotkey_registered = 1;
            mw_log("tabs: hotkey \"%s\" registered (cycles All / Chat / System)", spec);
        } else {
            mw_log("tabs: RegisterHotKey(\"%s\") failed: %lu -- another app may "
                   "already own that combination", spec, GetLastError());
        }
    }
}

int mw_tabs_install(HWND hwnd)
{
    if (g_installed)
        return 1;
    if (!mw_addr[MW_RENDER]) {
        mw_log("tabs: render routine unresolved, not installing");
        return 0;
    }

    /* prologue is `mov eax, 0x8230` -- exactly 5 bytes, no relative branch */
    if (!mw_hook_install(&g_render_hook, mw_addr[MW_RENDER],
                         (void *)(UINT_PTR)render_detour, 5)) {
        mw_log("tabs: could not hook the render routine");
        return 0;
    }
    g_render_orig = (render_fn)g_render_hook.trampoline;

    /* prologue is `mov eax,[ecx+0x44] / mov edx,[ecx+0x34]` -- 6 bytes, and
     * 5 would land mid-instruction. */
    if (mw_addr[MW_PRESENT] &&
        mw_hook_install(&g_present_hook, mw_addr[MW_PRESENT],
                        (void *)(UINT_PTR)present_detour, 6)) {
        g_present_orig = (render_fn)g_present_hook.trampoline;
    } else {
        mw_log("tabs: could not hook the present; the strip will not be drawn");
        g_strip_on = 0;
    }

    refresh_tabs();

    g_brush[BRUSH_STRIP_BG]   = CreateSolidBrush(COL_STRIP_BG);
    g_brush[BRUSH_TAB_ACTIVE] = CreateSolidBrush(COL_TAB_ACTIVE);
    g_brush[BRUSH_TAB_IDLE]   = CreateSolidBrush(COL_TAB_IDLE);
    g_brush[BRUSH_SEP]        = CreateSolidBrush(COL_SEP);
    g_brush[BRUSH_UNREAD]     = CreateSolidBrush(COL_UNREAD);
    g_brush[BRUSH_MENTION]    = CreateSolidBrush(COL_MENTION);

    /* Sized to the strip rather than to the chat font: the strip is chrome and
     * has to stay legible whatever the player sets FontSize to. */
    g_strip_font = CreateFontA(STRIP_H - 8, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE,
                               mw_cfg_str("TabStripFont", "Tahoma"));

    g_tab_msg = RegisterWindowMessageA(TAB_MESSAGE_NAME);
    g_probe_msg = RegisterWindowMessageA("osmw_chat_probe");
    g_hotkey_spec[0] = '\0';
    mw_tabs_sync_hotkey(hwnd);

    g_installed = 1;
    g_active_tab = 0;
    mw_log("tabs: installed (post message 0x%04X \"%s\", wParam 0 cycles, "
           "1..%d selects a tab)", g_tab_msg, TAB_MESSAGE_NAME, TAB_COUNT);

    /* mirror.c seeds itself with a straight copy, which is right for getting a
     * populated ring immediately but skips the per-message decoration. Rebuild
     * once through the normal path so existing history is marked up and
     * filtered the same way new arrivals will be. */
    mw_mirror_rebuild(active_mask());
    return 1;
}

void mw_tabs_remove(void)
{
    HWND hwnd = mw_window_hwnd();

    if (!g_installed)
        return;
    if (g_hotkey_registered && hwnd && IsWindow(hwnd))
        UnregisterHotKey(hwnd, TAB_HOTKEY_ID);
    g_hotkey_registered = 0;

    /* put the caption back before we let go of the window */
    if (g_base_title[0] && hwnd && IsWindow(hwnd))
        SetWindowTextA(hwnd, g_base_title);

    /* Present first: it draws into state the render hook owns. */
    mw_hook_remove(&g_present_hook);
    mw_hook_remove(&g_render_hook);

    {
        int i;
        for (i = 0; i < BRUSH_COUNT; i++) {
            if (g_brush[i]) {
                DeleteObject(g_brush[i]);
                g_brush[i] = NULL;
            }
        }
    }
    if (g_strip_font) {
        DeleteObject(g_strip_font);
        g_strip_font = NULL;
    }

    g_installed = 0;
    mw_log("tabs: removed");
}
