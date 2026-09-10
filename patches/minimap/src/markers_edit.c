/* Persistent map markers: label text entry.
 *
 * Spec: mw-tools/tools/client-dll/map-markers.md section 8.
 *
 * The client has its own text-control class and about a dozen live instances of
 * it, and constructing one more would inherit its IME handling for free. This
 * does not do that. It keeps a plain buffer and reads the same input the
 * control class reads, for one reason: constructing a client C++ object means
 * getting its constructor chain, its size and its teardown all exactly right
 * from static analysis, and a mistake there is a crash rather than a wrong
 * pixel. A 31-byte label does not justify that risk while the rest of the
 * feature is being proven.
 *
 * The input source is the client's own per-frame typed-character queue -- the
 * one the login screen drains -- so this is neither global keyboard polling nor
 * a window subclass, and the character filtering matches what the label rules
 * allow rather than what a keyboard produces.
 *
 * Known limitation: no IME composition. A label is typed in the client's code
 * page directly. Reusing the stock control is the upgrade path if that matters.
 */
#include "osmw_minimap.h"

#include <stdio.h>
#include <string.h>

typedef unsigned char (__cdecl *mm_typed_count_fn)(void);
typedef unsigned char (__cdecl *mm_typed_at_fn)(int index);
typedef void          (__cdecl *mm_typed_clear_fn)(void);

static void focus_take(void);
static void focus_release(void);

static int  g_active;
static char g_buffer[MM_MARKER_LABEL_MAX + 1];
static int  g_length;
static char g_target[MM_MARKER_ID_LEN + 1];

/* Virtual keys we act on. Everything else is ignored rather than guessed at. */
#define MM_VK_BACK   0x08
#define MM_VK_RETURN 0x0D
#define MM_VK_ESCAPE 0x1B

int  mm_edit_active(void)      { return g_active; }
const char *mm_edit_text(void) { return g_buffer; }
const char *mm_edit_target(void) { return g_target; }

void mm_edit_begin(const char *marker_id, const char *initial)
{
    if (marker_id == NULL)
        return;
    _snprintf(g_target, sizeof(g_target), "%s", marker_id);
    _snprintf(g_buffer, sizeof(g_buffer), "%s", initial ? initial : "");
    g_buffer[sizeof(g_buffer) - 1] = '\0';
    g_length = (int)strlen(g_buffer);
    g_active = 1;

    /* Anything the player typed before opening the field belongs to whatever
     * had focus then, not to us. */
    if (mm_addr[MM_TYPEDCLEAR] != NULL)
        ((mm_typed_clear_fn)mm_addr[MM_TYPEDCLEAR])();
}

void mm_edit_cancel(void)
{
    focus_release();
    g_active = 0;
    g_length = 0;
    g_buffer[0] = '\0';
    g_target[0] = '\0';
}

/* Taking, and giving back, keyboard focus.
 *
 * Bit 4 of a control's flags dword at +0x1C is the client's "accepting input"
 * bit, and every drain path in the client gates on it. There is no global
 * arbiter: each screen sets the bit on one of ITS OWN controls and clears it on
 * the others, so nothing ever clears a bit belonging to somewhere else.
 *
 * The first attempt here declined to drain while any other control was
 * accepting, on the theory that a player typing in chat is not simultaneously
 * naming a marker. That is unusable in practice: the chat input holds the bit
 * permanently, so the field never received a single character.
 *
 * So the field takes focus the way the client's own modal windows do -- clear
 * the bit on whatever holds it, restore it on close. Re-applied every frame
 * because chat re-asserts its own bit, and always restored, so cancelling or
 * losing the panel cannot leave the player unable to type in chat.
 */
#define MM_CTRL_ACCEPTING 4
#define MM_MAX_SUSPENDED  16

static void *g_suspended[MM_MAX_SUSPENDED];
static int   g_suspended_count;

static void ctrl_set_accepting(void *ctrl, int on)
{
    void **vt;
    void *fn;

    if (ctrl == NULL || IsBadReadPtr(ctrl, 4))
        return;
    vt = *(void ***)ctrl;
    if (vt == NULL || IsBadReadPtr(vt, 0x50))
        return;
    fn = vt[0x48 / 4];
    if (fn == NULL)
        return;

    __asm {
        mov  ecx, ctrl
        push on
        mov  eax, fn
        call eax
    }
}

/* Clear the accepting bit on every control that has it, remembering them. */
static void focus_take(void)
{
    void **array;
    int count, i;

    if (mm_addr[MM_CTRLARRAY] == NULL || mm_addr[MM_CTRLCOUNT] == NULL)
        return;

    array = *(void ***)mm_addr[MM_CTRLARRAY];
    count = *(int *)mm_addr[MM_CTRLCOUNT];
    if (array == NULL || IsBadReadPtr(array, 4) || count <= 0 || count > 64)
        return;

    for (i = 0; i < count; i++) {
        void *ctrl = array[i];
        unsigned flags;

        if (ctrl == NULL || IsBadReadPtr(ctrl, 0x20))
            continue;
        flags = *(unsigned *)((unsigned char *)ctrl + 0x1C);
        if (!(flags & MM_CTRL_ACCEPTING))
            continue;

        /* Remember it once; the bit is re-cleared every frame but the list of
         * who to restore must not grow duplicates. */
        if (g_suspended_count < MM_MAX_SUSPENDED) {
            int seen = 0, j;
            for (j = 0; j < g_suspended_count; j++) {
                if (g_suspended[j] == ctrl) {
                    seen = 1;
                    break;
                }
            }
            if (!seen)
                g_suspended[g_suspended_count++] = ctrl;
        }
        ctrl_set_accepting(ctrl, 0);
    }
}

static void focus_release(void)
{
    int i;

    for (i = 0; i < g_suspended_count; i++)
        ctrl_set_accepting(g_suspended[i], 1);
    g_suspended_count = 0;
}

static void append_char(int ch)
{
    /* The store rejects control characters, so they are refused here too: a
     * label that cannot be written back is not worth accepting. */
    if (ch < 0x20 || ch == 0x7F)
        return;
    if (g_length >= MM_MARKER_LABEL_MAX)
        return;
    g_buffer[g_length++] = (char)ch;
    g_buffer[g_length] = '\0';
}

/* One frame of editing. Returns non-zero when the field has just been committed
 * with Enter, so the caller can save. */
int mm_edit_tick(int *cancelled)
{
    mm_typed_count_fn count_fn;
    mm_typed_at_fn at_fn;
    int count, i, typed = 0;

    if (cancelled)
        *cancelled = 0;
    if (!g_active)
        return 0;

    if (mm_addr[MM_TYPEDCOUNT] == NULL || mm_addr[MM_TYPEDAT] == NULL ||
        mm_addr[MM_TYPEDCLEAR] == NULL)
        return 0;

    /* Hold focus for as long as the field is open. */
    focus_take();

    count_fn = (mm_typed_count_fn)mm_addr[MM_TYPEDCOUNT];
    at_fn = (mm_typed_at_fn)mm_addr[MM_TYPEDAT];

    /* The queue is WM_CHAR-shaped, so the editing keys arrive here as control
     * characters rather than as virtual keys: backspace is 0x08, Enter 0x0D,
     * Escape 0x1B. An earlier version dropped everything below 0x20 as "not a
     * label character" and then hunted for backspace in the virtual-key bytes,
     * which is why deletion did nothing at all. */
    count = (int)count_fn();
    for (i = 0; i < count; i++) {
        int ch = (int)at_fn(i);

        typed = 1;
        if (ch == MM_VK_BACK) {
            if (g_length > 0)
                g_buffer[--g_length] = '\0';
        } else if (ch == MM_VK_RETURN) {
            ((mm_typed_clear_fn)mm_addr[MM_TYPEDCLEAR])();
            focus_release();
            g_active = 0;
            return 1;
        } else if (ch == MM_VK_ESCAPE) {
            ((mm_typed_clear_fn)mm_addr[MM_TYPEDCLEAR])();
            mm_edit_cancel();
            if (cancelled)
                *cancelled = 1;
            return 0;
        } else {
            append_char(ch);
        }
    }
    if (typed)
        ((mm_typed_clear_fn)mm_addr[MM_TYPEDCLEAR])();

    /* Non-printable keys.
     *
     * The client presents the virtual key in EITHER of two adjacent bytes --
     * the login screen reads 0x7029BD and falls back to 0x7029BE -- so both are
     * checked, and both are cleared once consumed. Reading only the first is
     * why backspace did nothing.
     *
     * This is also no longer gated on "no character arrived this frame".
     * Typing and correcting in the same frame is ordinary, and skipping the key
     * whenever a character came through made deletion feel dead while typing.
     */
    if (mm_addr[MM_KEYEVENT] != NULL) {
        unsigned char *k0 = (unsigned char *)mm_addr[MM_KEYEVENT];
        unsigned char *k1 = k0 + 1;
        unsigned char vk = *k0 ? *k0 : *k1;

        if (vk == MM_VK_BACK || vk == MM_VK_RETURN || vk == MM_VK_ESCAPE) {
            *k0 = 0;
            *k1 = 0;
        }

        if (vk == MM_VK_BACK) {
            if (g_length > 0)
                g_buffer[--g_length] = '\0';
        } else if (vk == MM_VK_RETURN) {
            focus_release();
            g_active = 0;
            return 1;
        } else if (vk == MM_VK_ESCAPE) {
            mm_edit_cancel();
            if (cancelled)
                *cancelled = 1;
        }
    }
    return 0;
}
