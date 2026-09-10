/* Give the chat window its own private copy of the message ring.
 *
 * The client keeps chat in a 1024-slot ring at MsgArray, with three counters
 * immediately below it:
 *
 *   base-0x10  write head, advanced on every append, wraps at 1024
 *   base-0x0C  tail, the oldest slot still held; advanced when the head laps it
 *   base-0x04  view cursor, the slot at the bottom of the displayed range
 *
 * The chat window's render walks backwards from the cursor towards the tail,
 * formatting messages until the pane is full. Several OTHER subsystems read
 * the same ring, so filtering it in place would filter them too.
 *
 * Instead we allocate a private ring in the same layout and rewrite the twelve
 * absolute addresses in the chat window's own code to point at it. The window
 * then renders entirely from our copy -- natively, so channel chips, #C colour
 * markup, #1 emoticons, wrapping and scrolling all remain the client's own
 * code -- while every other consumer keeps reading the untouched original.
 *
 * The mirror is built APPEND-ONLY, exactly like the ring it shadows. That is
 * the whole trick: a message keeps the same slot index in our ring for as long
 * as it lives, so the chat window's cached line list -- which is keyed on slot
 * index and carried between render passes -- stays valid. Rebuilding or
 * compacting the ring on every pass would renumber it, which is what made
 * earlier attempts flash and then duplicate lines.
 *
 * Changing the filter does renumber everything, so that path rebuilds the ring
 * and then invalidates the window's cached line list in one go.
 */
#include "osmw_chat.h"

#include <stdio.h>
#include <string.h>

#define MSG_STRIDE   0xC0
#define MSG_SLOTS    1024
#define RING_BYTES   (MSG_SLOTS * MSG_STRIDE)

/* The counters sit in the 0x10 bytes below the ring, so the private block is
 * laid out the same way and `base` points past them. */
#define COUNTER_BYTES 0x10
#define BLOCK_BYTES   (COUNTER_BYTES + RING_BYTES)

/* How far around the resolved render/hit-test to look for references. The
 * twelve known ones span roughly 0x600 bytes; these bounds are generous. */
#define SCAN_BACK    0x0200
#define SCAN_FORWARD 0x0800

/* The reference build has exactly this many. A different number means the
 * client changed and we should not be patching blind. */
#define EXPECTED_REFS 12
#define MAX_REFS      32

typedef struct {
    unsigned char *at;      /* address of the disp32 field itself */
    DWORD          original;
    OSMW_TOKEN     claim;
} ref_t;

static unsigned char *g_block;
static unsigned char *g_base;       /* g_block + COUNTER_BYTES */
static ref_t          g_refs[MAX_REFS];
static int            g_ref_count;
static int            g_installed;

unsigned char *mw_mirror_base(void)
{
    return g_base;
}

static int write_dword(unsigned char *at, DWORD value)
{
    return mw_write_bytes(at, &value, sizeof(value));
}

/* Find every dword in the chat window's code that points into the real ring or
 * its counters. Scanning beats a signature per site: there are twelve of them,
 * they are all plain [reg+disp32] or [disp32] operands, and requiring an exact
 * count guards against both a changed client and a stray false positive. */
static int collect_refs(unsigned char *real_base)
{
    unsigned char *lo, *hi, *p;
    DWORD ring_lo = (DWORD)(UINT_PTR)(real_base - COUNTER_BYTES);
    DWORD ring_hi = (DWORD)(UINT_PTR)(real_base + RING_BYTES);

    if (!mw_addr[MW_HITTEST] || !mw_addr[MW_RENDER])
        return 0;

    lo = mw_addr[MW_HITTEST] - SCAN_BACK;
    hi = mw_addr[MW_RENDER] + SCAN_FORWARD;
    if (hi <= lo)
        return 0;

    g_ref_count = 0;
    for (p = lo; p <= hi - 4; p++) {
        DWORD v = *(DWORD *)p;
        if (v < ring_lo || v >= ring_hi)
            continue;
        if (g_ref_count >= MAX_REFS) {
            mw_log("mirror: more than %d references, refusing to patch", MAX_REFS);
            return 0;
        }
        g_refs[g_ref_count].at = p;
        g_refs[g_ref_count].original = v;
        g_ref_count++;
    }
    return g_ref_count;
}

/* Counter layout, relative to a ring base. */
#define CNT_HEAD   (-0x10)      /* next slot to write */
#define CNT_TAIL   (-0x0C)      /* oldest slot still held */
#define CNT_LINES  (-0x08)      /* visible line count; a user preference */
#define CNT_CURSOR (-0x04)      /* slot at the bottom of the displayed range */

#define MSG_SENDER   0x00
#define MSG_CHANNEL  0x04
#define MSG_EMPTY    0x80000000u
#define CHANNEL_COUNT 10

/* Chat object fields we touch when the filter changes. */
#define OBJ_LINE_STORE   0x80
#define OBJ_STORE_DWORDS 0x1E79

static int g_our_head;
static int g_our_tail;
static int g_seen_head;     /* how far through the real ring we have consumed */

/* Which slot of the CLIENT's ring each of our slots came from, or -1 for a
 * synthetic record. The timestamp is keyed on the client's index because that
 * one is stable; ours is renumbered by every rebuild. The line composer sees
 * only our ring, so it needs this to get back. */
static int g_real_of[MSG_SLOTS];

/* The reverse: which of our slots holds a given slot of the client's ring, or
 * -1 if that message was filtered out or has been lapped. Needed to translate
 * the client's scroll position into ours -- see translate_cursor. */
static int g_mirror_of[MSG_SLOTS];

int mw_mirror_real_slot(int mirror_slot)
{
    if (!g_installed || mirror_slot < 0 || mirror_slot >= MSG_SLOTS)
        return -1;
    return g_real_of[mirror_slot];
}

static void forget_slot_map(void)
{
    int i;
    for (i = 0; i < MSG_SLOTS; i++) {
        g_real_of[i] = -1;
        g_mirror_of[i] = -1;
    }
}

static int counter(unsigned char *base, int which)
{
    return *(int *)(base + which);
}

static void set_counter(unsigned char *base, int which, int value)
{
    *(int *)(base + which) = value;
}

#define MASK_EVERY ((1u << CHANNEL_COUNT) - 1u)

static int keep_message(const unsigned char *rec, unsigned mask)
{
    unsigned channel;

    if (*(const unsigned *)(rec + MSG_SENDER) == MSG_EMPTY)
        return 0;                       /* unused slot */

    if (mw_block_blocked(rec))
        return 0;

    channel = *(const unsigned *)(rec + MSG_CHANNEL);
    if (channel >= CHANNEL_COUNT) {
        /* Channels outside the ten named ones are real -- the character info
         * panel uses 99, for instance -- but they belong to no tab. Show them
         * on an unfiltered tab only, so "Trade" means trade rather than trade
         * plus whatever else the client happens to route through here. */
        return mask == MASK_EVERY;
    }
    return (mask & (1u << channel)) != 0;
}

/* `real_slot` is where this record lives in the CLIENT's ring, or -1 for a
 * synthetic record that has no counterpart there. The timestamp is keyed on it,
 * since that index is what stays stable across our rebuilds. */
static void append(const unsigned char *rec, int real_slot)
{
    unsigned char *slot = g_base + (size_t)g_our_head * MSG_STRIDE;

    memcpy(slot, rec, MSG_STRIDE);
    g_real_of[g_our_head] = real_slot;
    if (real_slot >= 0 && real_slot < MSG_SLOTS)
        g_mirror_of[real_slot] = g_our_head;
    mw_time_note(real_slot);
    /* Decorate the copy, never the client's own record. The timestamp is put
     * on by the line composer instead, which gets it in front of the sender
     * name; mw_time_decorate is only the fallback for when that hook could not
     * be installed, and knows to stand down otherwise. */
    mw_mention_decorate(slot);
    mw_time_decorate(slot, real_slot);

    g_our_head = (g_our_head + 1) % MSG_SLOTS;
    if (g_our_head == g_our_tail)
        g_our_tail = (g_our_tail + 1) % MSG_SLOTS;   /* lapped, drop oldest */
}

/* Translate the client's scroll position into ours.
 *
 * The chat window has three vanilla controls that all work through the ring's
 * view cursor, and all three broke when the window started rendering from our
 * private copy:
 *
 *   scroll up / down   0x00462040 walks the CLIENT's cursor between its tail
 *                      and head (VK_UP 0x48 / VK_DOWN 0x50).
 *   freeze             0x00702858. Every append path reads it and skips the
 *                      "cursor = head" step while it is set, which is what
 *                      stops new messages dragging the view back down.
 *
 * All of them live outside the span the mirror redirects, so they still move
 * the real ring -- and we were overwriting our cursor with our own head on
 * every sync, which pinned the window to the newest message and made the
 * buttons look dead.
 *
 * Reading the client's cursor instead of inventing one makes all three work
 * again, freeze included, without hooking any of them: when the client stops
 * moving its cursor, ours stops too.
 *
 * The cursor is an EXCLUSIVE end -- the client's render starts at cursor-1 and
 * walks back towards the tail -- so the translation looks for the newest kept
 * message at or before cursor-1 and returns one past it.
 */
static int translate_cursor(unsigned char *real)
{
    int head_r = counter(real, CNT_HEAD);
    int tail_r = counter(real, CNT_TAIL);
    int cur_r  = counter(real, CNT_CURSOR);
    int i, steps;

    if (cur_r < 0 || cur_r >= MSG_SLOTS)
        return g_our_head;
    if (cur_r == head_r)
        return g_our_head;              /* not scrolled: follow the newest */

    i = (cur_r - 1 + MSG_SLOTS) % MSG_SLOTS;
    steps = (cur_r - tail_r + MSG_SLOTS) % MSG_SLOTS;
    while (steps-- > 0) {
        int m = g_mirror_of[i];
        /* Confirm the mapping still holds: our ring laps independently, so an
         * entry can name a slot that has since been reused. */
        if (m >= 0 && m < MSG_SLOTS && g_real_of[m] == i)
            return (m + 1) % MSG_SLOTS;
        i = (i - 1 + MSG_SLOTS) % MSG_SLOTS;
    }

    /* Scrolled back past everything this tab keeps. */
    return g_our_tail;
}

static void publish_counters(unsigned char *real)
{
    set_counter(g_base, CNT_HEAD, g_our_head);
    set_counter(g_base, CNT_TAIL, g_our_tail);
    set_counter(g_base, CNT_CURSOR, translate_cursor(real));
    /* The visible-line count is the player's own setting; take it as-is. */
    set_counter(g_base, CNT_LINES, counter(real, CNT_LINES));
}

/* Wipe the window's cached line list so it re-derives from the new ring. This
 * is the same span the client's own show path clears, and it covers the line
 * records, the line count and the packed range indices at +0x7A60. */
static void invalidate_view(void)
{
    unsigned char *obj = mw_addr[MW_OBJECT];
    unsigned char *dirty = mw_addr[MW_DIRTYFLAG];

    if (obj)
        memset(obj + OBJ_LINE_STORE, 0, (size_t)OBJ_STORE_DWORDS * 4);
    if (dirty)
        *(DWORD *)dirty = 1;
}

/* Drop the window's cached line list without touching the ring.
 *
 * Those records carry layout measured with whatever font was current when they
 * were built -- wrap points and line heights. After a font change they are
 * stale, and the pane renders with the old wrap width: text clipped at the
 * right edge and lines overlapping. Clearing them makes the next render
 * re-derive everything at the new metrics. */
void mw_mirror_invalidate_view(void)
{
    invalidate_view();
}

/* Rebuild from scratch. Renumbers every slot, so the window's cached line list
 * has to go with it. */
void mw_mirror_rebuild(unsigned mask)
{
    unsigned char *real = mw_addr[MW_MSGARRAY];
    int head, tail, i, kept = 0;

    if (!g_installed || !real)
        return;

    head = counter(real, CNT_HEAD);
    tail = counter(real, CNT_TAIL);

    g_our_head = 0;
    g_our_tail = 0;
    memset(g_base, 0, RING_BYTES);
    forget_slot_map();
    for (i = 0; i < MSG_SLOTS; i++)
        *(unsigned *)(g_base + (size_t)i * MSG_STRIDE + MSG_SENDER) = MSG_EMPTY;

    for (i = tail; i != head; i = (i + 1) % MSG_SLOTS) {
        const unsigned char *rec = real + (size_t)i * MSG_STRIDE;
        if (keep_message(rec, mask)) {
            append(rec, i);
            kept++;
        }
    }

    g_seen_head = head;
    publish_counters(real);
    invalidate_view();

    {
        int stamped, no_time;
        mw_time_stats(&stamped, &no_time);
        if (mw_time_enabled())
            mw_log("mirror: rebuilt, %d of %d message(s) kept; "
                   "%d stamped, %d with no observed time",
                   kept, (head - tail + MSG_SLOTS) % MSG_SLOTS,
                   stamped, no_time);
        else
            mw_log("mirror: rebuilt, %d of %d message(s) kept", kept,
                   (head - tail + MSG_SLOTS) % MSG_SLOTS);
    }
}

/* Put a synthetic message into the window's ring.
 *
 * Diagnostics only. The message exists solely in our copy, so it is never sent
 * anywhere, never reaches the client's own ring, and disappears on the next
 * rebuild (any tab switch). Used by the colour probe to render known markup
 * without having to say anything on a live server. */
void mw_mirror_inject(unsigned channel, const char *name, const char *text)
{
    unsigned char *real = mw_addr[MW_MSGARRAY];
    unsigned char  rec[MSG_STRIDE];

    if (!g_installed || !real)
        return;

    memset(rec, 0, sizeof(rec));
    *(unsigned *)(rec + MSG_SENDER) = 1;        /* anything but MSG_EMPTY */
    *(unsigned *)(rec + MSG_CHANNEL) = channel;
    _snprintf((char *)(rec + 0x08), 0x1D - 0x08, "%s", name ? name : "");
    _snprintf((char *)(rec + 0x1D), MSG_STRIDE - 0x1D, "%s", text ? text : "");
    rec[0x1C] = '\0';
    rec[MSG_STRIDE - 1] = '\0';

    append(rec, -1);        /* synthetic: no slot in the client's ring */
    publish_counters(real);
}

/* Bring the mirror up to date with whatever has arrived since last time.
 * Append-only, so existing slot indices do not move. */
void mw_mirror_sync(unsigned mask)
{
    unsigned char *real = mw_addr[MW_MSGARRAY];
    int head, tail, added = 0;

    if (!g_installed || !real)
        return;

    head = counter(real, CNT_HEAD);
    tail = counter(real, CNT_TAIL);

    /* A reset (the clear feature, or entering the world) rewinds the ring.
     * Anything we cannot explain as forward movement means start over. */
    if (head == tail && head == 0) {
        if (g_our_head != 0 || g_our_tail != 0) {
            mw_mirror_rebuild(mask);
        }
        return;
    }

    while (g_seen_head != head) {
        const unsigned char *rec = real + (size_t)g_seen_head * MSG_STRIDE;
        /* Note the arrival time for EVERY message, including ones this tab
         * filters out: it may be showing on another tab, and switching to that
         * tab must not have to invent a time. */
        mw_time_observe(g_seen_head);
        if (keep_message(rec, mask)) {
            append(rec, g_seen_head);
            added++;
        }
        g_seen_head = (g_seen_head + 1) % MSG_SLOTS;
    }

    /* Every pass, not just when something arrived: the scroll buttons move the
     * client's cursor without adding anything, and republishing is how that
     * reaches the window. */
    (void)added;
    publish_counters(real);
}

int mw_mirror_install(void)
{
    unsigned char *real = mw_addr[MW_MSGARRAY];
    char label[64];
    int i, n;

    if (g_installed)
        return 1;
    if (!real) {
        mw_log("mirror: message array unresolved, not installing");
        return 0;
    }

    n = collect_refs(real);
    if (n != EXPECTED_REFS) {
        mw_log("mirror: found %d references to the message ring, expected %d "
               "-- standing down rather than patching blind", n, EXPECTED_REFS);
        return 0;
    }

    g_block = (unsigned char *)VirtualAlloc(NULL, BLOCK_BYTES,
                                            MEM_COMMIT | MEM_RESERVE,
                                            PAGE_READWRITE);
    if (!g_block) {
        mw_log("mirror: could not allocate %d bytes", BLOCK_BYTES);
        return 0;
    }
    g_base = g_block + COUNTER_BYTES;

    /* Nothing has been observed arriving yet, so nothing has a known time and
     * no slot mapping exists. tabs.c rebuilds through the normal path straight
     * after install, which fills both in. */
    mw_time_reset();
    forget_slot_map();

    /* Seed it before redirecting, so the first render after the patch sees a
     * populated ring rather than an empty one. Unfiltered at this point; the
     * first sync applies whatever filter is active. */
    memcpy(g_block, real - COUNTER_BYTES, BLOCK_BYTES);
    g_our_head = counter(real, CNT_HEAD);
    g_our_tail = counter(real, CNT_TAIL);
    g_seen_head = g_our_head;

    for (i = 0; i < n; ++i) {
        OSMW_RESULT result;
        _snprintf(label, sizeof(label), "chat mirror address operand %d", i + 1);
        label[sizeof(label) - 1] = '\0';
        result = mw_claim_region(g_refs[i].at, sizeof(DWORD),
                                 OSMW_REGION_CODE, label,
                                 &g_refs[i].claim);
        if (result != OSMW_OK) {
            mw_log("mirror: claim %d failed at %p (%ld)",
                   i, g_refs[i].at, (long)result);
            while (i-- > 0)
                (void)mw_release_region(&g_refs[i].claim);
            VirtualFree(g_block, 0, MEM_RELEASE);
            g_block = NULL;
            g_base = NULL;
            return 0;
        }
    }

    for (i = 0; i < n; i++) {
        DWORD moved = (DWORD)(UINT_PTR)(g_base + (g_refs[i].original -
                                                  (DWORD)(UINT_PTR)real));
        if (!write_dword(g_refs[i].at, moved)) {
            mw_log("mirror: failed to patch reference %d at %p", i, g_refs[i].at);
            while (i-- > 0)
                write_dword(g_refs[i].at, g_refs[i].original);
            for (i = n - 1; i >= 0; --i)
                (void)mw_release_region(&g_refs[i].claim);
            VirtualFree(g_block, 0, MEM_RELEASE);
            g_block = NULL;
            g_base = NULL;
            return 0;
        }
    }

    g_installed = 1;
    mw_log("mirror: %d references redirected from %p to %p (%d bytes)",
           n, real, g_base, BLOCK_BYTES);
    return 1;
}

OSMW_RESULT mw_mirror_remove(void)
{
    int i;
    int restored = 1;
    int released = 1;

    if (!g_installed)
        return OSMW_OK;
    for (i = 0; i < g_ref_count; i++) {
        if (!write_dword(g_refs[i].at, g_refs[i].original))
            restored = 0;
    }
    if (!restored) {
        mw_log("mirror: one or more operands could not be restored");
        return OSMW_E_INTERNAL;
    }
    for (i = g_ref_count - 1; i >= 0; --i) {
        if (mw_release_region(&g_refs[i].claim) != OSMW_OK)
            released = 0;
    }
    if (!released) {
        mw_log("mirror: one or more restored claims could not be released");
        return mw_claim_cleanup_result();
    }
    if (g_block) {
        VirtualFree(g_block, 0, MEM_RELEASE);
        g_block = NULL;
        g_base = NULL;
    }
    g_ref_count = 0;
    g_installed = 0;
    mw_log("mirror: references restored");
    return OSMW_OK;
}
