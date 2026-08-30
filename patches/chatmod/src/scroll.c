/* Mouse-wheel scrolling for the chat window.
 *
 * The window has scroll-up and scroll-down buttons but no wheel support: its
 * WndProc dispatches only 0x02..0x24, 0x7E, 0x84..0x108, 0x201, 0x203, 0x204
 * and 0x231, and WM_MOUSEWHEEL (0x20A) is in none of them. There are no
 * ScrollWindow or SetScrollInfo imports either, so there is no scrollbar to
 * drive instead.
 *
 * Rather than move the view cursor here, this calls the CLIENT's own scroll
 * routine, the one the two buttons call:
 *
 *     0x00462040   __cdecl, takes a virtual key. 0x48 (VK_UP) steps the view
 *                  back towards the tail, 0x50 (VK_DOWN) forwards towards the
 *                  head, both clamped at the ends.
 *
 * That keeps the wheel exactly equivalent to pressing a button -- same
 * clamping, same wrap handling, and the in-game overlay scrolls in step,
 * because both read the same cursor. Reimplementing the arithmetic here would
 * be a second copy to keep true.
 *
 * Three near-identical copies of that routine exist, for three different
 * buffers (0x00462040 chat, 0x00462E50 and 0x004633B0 for others), so the
 * signature pins the low half of the two globals it touches to pick ours.
 */
#include "osmw_chat.h"

/* winuser.h defines WHEEL_PAGESCROLL as UINT_MAX, which lives here. */
#include <limits.h>

#ifndef WM_MOUSEWHEEL
#define WM_MOUSEWHEEL 0x020A
#endif
#ifndef WHEEL_DELTA
#define WHEEL_DELTA 120
#endif
#ifndef SPI_GETWHEELSCROLLLINES
#define SPI_GETWHEELSCROLLLINES 0x0068
#endif

#define VK_SCROLL_BACK    0x48      /* VK_UP   -- towards older messages */
#define VK_SCROLL_FORWARD 0x50      /* VK_DOWN -- towards newer messages */

/* WHEEL_PAGESCROLL comes from winuser.h. Windows reports it in place of a line
 * count to mean "scroll a screen at a time". */

typedef void (__cdecl *scroll_fn)(int vk);

static scroll_fn g_scroll;
static int       g_enabled = 1;
static int       g_lines;           /* 0 = follow the system setting */
static int       g_residual;        /* sub-notch wheel movement, carried over */

void mw_scroll_refresh(void)
{
    g_enabled = mw_cfg_int("WheelScroll", 1) != 0;
    g_lines   = mw_cfg_int("WheelScrollLines", 0);
}

int mw_scroll_install(void)
{
    void *fn = mw_addr[MW_SCROLLVIEW];

    if (!fn) {
        mw_log("scroll: view scroller unresolved; wheel scrolling unavailable");
        return 0;
    }
    /* void* to function goes through UINT_PTR, as elsewhere in this mod. */
    g_scroll = (scroll_fn)(UINT_PTR)fn;
    g_residual = 0;
    mw_scroll_refresh();
    mw_log("scroll: wheel scrolling via the client's own scroller at %p", fn);
    return 1;
}

void mw_scroll_remove(void)
{
    g_scroll = NULL;
}

/* How many lines one notch moves. The system setting is what every other
 * application obeys, so follow it unless the player has overridden it. */
static int lines_per_notch(void)
{
    UINT   lines = 3;
    int    visible;

    if (g_lines > 0)
        return g_lines;

    if (!SystemParametersInfoA(SPI_GETWHEELSCROLLLINES, 0, &lines, 0))
        return 3;

    if (lines == WHEEL_PAGESCROLL) {
        /* "One screen at a time". The client keeps the visible line count in
         * the ring's counters; fall back to something sane if it is unset. */
        unsigned char *real = mw_addr[MW_MSGARRAY];
        visible = real ? *(int *)(real - 0x08) : 0;
        return (visible > 0) ? visible : 10;
    }
    return (lines > 0) ? (int)lines : 3;
}

int mw_scroll_on_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                         LRESULT *result)
{
    int delta, notches, steps, i, vk;

    (void)hwnd;
    (void)lp;

    if (msg != WM_MOUSEWHEEL)
        return 0;
    if (!g_enabled || !g_scroll)
        return 0;

    /* Carry the remainder: a high-resolution wheel sends deltas smaller than
     * one notch, and dropping them makes it feel dead. */
    delta = (short)HIWORD(wp);
    g_residual += delta;
    notches = g_residual / WHEEL_DELTA;      /* truncates towards zero */
    g_residual -= notches * WHEEL_DELTA;
    if (!notches) {
        *result = 0;
        return 1;                            /* consumed, nothing to do yet */
    }

    /* Wheel forward (positive) shows older messages, the same direction the
     * scroll-up button goes. */
    vk = (notches > 0) ? VK_SCROLL_BACK : VK_SCROLL_FORWARD;
    if (notches < 0)
        notches = -notches;

    steps = notches * lines_per_notch();
    for (i = 0; i < steps; i++)
        g_scroll(vk);

    /* The window caches its line list between frames and that cache is keyed
     * on the cursor we just moved, so force the next render to re-derive it. */
    if (mw_addr[MW_DIRTYFLAG])
        *(DWORD *)mw_addr[MW_DIRTYFLAG] = 1;

    *result = 0;
    return 1;
}
