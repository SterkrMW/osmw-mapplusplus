/* Chat item and pet links.
 *
 * ALT+Click an inventory item or pet and its compose token goes into the chat input.
 * The server resolves the token against that player's own inventory, registers
 * a snapshot of the item, and broadcasts the item's name wrapped in markers the
 * stock renderer swallows -- so an unmodded client reads the line as ordinary
 * text. Click a line carrying a link and the item's tooltip pops up, drawn by
 * the client's own tooltip renderer.
 *
 * The wire contract lives in mw-tools, tools/client-dll/item-links.md; this
 * file must agree with src/game-server/Utils/ChatLinks.ts on the grammar.
 *
 *
 * The chat input is not a Win32 EDIT
 * ----------------------------------
 * It is a static instance of the client's own edit-box widget class, and the
 * client already does to it exactly what this file needs to do. At 0x433dce,
 * the quick-chat panel that puts "Heal me please!" into the box on a click:
 *
 *     mov ecx, <instance> ; call <Clear>
 *     push edx            ; the phrase, a plain C string
 *     mov ecx, <instance> ; call <AppendString>
 *
 * ChatInputInstance is anchored on that very site, so the signature both finds
 * the instance and pins the call pair being copied. We use AppendString
 * WITHOUT Clear, so the token lands at the caret and whatever the player has
 * already typed survives.
 *
 * Deliberately NOT the widget's key handler (vtable+0x18), which is what the
 * login mod drives: that gates on the box being enabled and focused, routes
 * 0x08..0x1b through a special-key table, and drops the character outright
 * while Ctrl is held. AppendString bypasses all of it.
 *
 *
 * Where the ALT+click is caught
 * -----------------------------
 * The inventory lives in the MAIN game window, which nothing subclasses, so
 * this cannot ride the chat window's message handler like every other feature
 * here.
 *
 * The inventory panel's callback is 0x42da70 -- the one that sets hoverKind to
 * 0 at 0x42e13a and to 5 at 0x42e1be, drives the page tabs, and calls the
 * ItemInfo request builder. It dispatches twice: first on the UI event byte
 * (0x42e1e2), then on `ebx`, which says WHICH control was hit. On a left
 * button down the inventory's 24 slots are controls 0x64..0x7b, and that case
 * is the whole of:
 *
 *     0042e68c  mov edx, [InvPage]        ; <-- hooked, 6 bytes
 *     0042e692  add ebx, -0x64            ; ebx = slot, 0..23
 *     0042e695  mov [InvSlotIndex], ebx   ; the client publishes the slot
 *     0042e69b  mov [0x743154], edx       ; and remembers the page
 *     0042e6a1  jmp <exit>
 *
 * So a press on a slot only RECORDS it; the pick-up happens on a later event.
 * At the hook ebx is still the raw control index, hence CONTROL_FIRST below.
 *
 * Consuming is a jump to the block's own `jmp <exit>` at +0x15, which skips
 * both stores, so the slot is never selected and nothing is picked up. That
 * needs no rel32 arithmetic and no second signature.
 *
 *
 * TWO WRONG GUESSES, recorded so nobody repeats them
 * ---------------------------------------------------
 * This hook was placed twice inside the callback at 0x428xxx, once at
 * 0x4284a4 and once at 0x428653, on the strength of that function comparing a
 * slot against the inventory grid at 0x6aa748 and the RE notes calling it "the
 * inventory click handler". Both installed cleanly, resolved perfectly, and
 * did nothing: that function is a DIFFERENT panel which merely reads the same
 * array, and an inventory click never enters it.
 *
 * What settled it was not more reading. A probe on the window manager's own
 * dispatch (`call [esi+0x24]` at 0x45e93e, with the window id at [esi+8])
 * logged the callback actually reached while ALT was held, and it was
 * 0x42da70. Two lessons worth keeping:
 *
 *   - "It references the inventory grid" identifies nothing. Several panels
 *     do. The hoverKind writes (0 at 0x42e13a, 5 at 0x42e1be) are what tie a
 *     callback to the inventory.
 *   - Inside a callback, resolve the `ebx` control table before choosing a
 *     block. Both wrong hooks sat in single-button cases (ebx 1), not in the
 *     24-entry grid case.
 *
 *
 * No edge detection is needed, and that is checked rather than assumed
 * --------------------------------------------------------------------
 * Window callbacks are dispatched every frame, so a naive hook would fire
 * continuously while the button is held. Resolving the panel's event table
 * (index 0x42e904, targets 0x42e8f0) shows the block this hook sits in is
 * reached only from 0xF5, WM_LBUTTONDOWN. The synthesised "still down" event
 * 0xF2 goes to 0x42e6b7 and mousemove 0xF4 goes straight to the exit. Once per
 * press, on the press.
 */
#include "osmw_chat.h"

#include <stdio.h>
#include <string.h>
#include <windowsx.h>

/* Widget field: the text cap, -1 for unlimited. The chat box ships as 0xa0. */
#define INPUT_MAX_OFFSET 0x120

/* The visible inventory grid is one page of 24, 20 bytes a slot. An empty slot
 * is a negative id; the panel tests it with `jl`, not `cmp -1`. */
#define GRID_STRIDE   20
#define PAGE_SLOTS    24
#define PAGE_COUNT     4

/* Control index of inventory slot 0 in the panel's `ebx` dispatch. */
#define CONTROL_FIRST 0x64

/* The pet bag has 20 records, 0x74 bytes each. Its first dword is the
 * persistent pet id, and zero marks an empty row. */
#define PET_SLOTS      20
#define PET_STRIDE     0x74
#define PET_ROW_FIRST  0x64
#define PET_VISIBLE_ROWS 6

/* Hooked instruction: mov edx, [InvPage] (6 bytes). Jumping to +0x15 lands on
 * the block's own `jmp <exit>`, skipping the two stores that would select the
 * slot. */
#define CLICK_PROLOGUE     6
#define CLICK_CONSUME_OFF  0x15

/* Chat-window line records, for mapping a click to a message. See tabs.c. */
#define OBJ_CLIENT_RECT   0x28
#define OBJ_LINE_STORE    0x80
#define OBJ_LINE_COUNT    0x7A63
#define LINE_STRIDE       0x410
#define LINE_HEIGHT_OFF   0x408
#define LINE_TEXT_MAX     0x400

/* DrawLines anchors its newest record at top+bottom-25 (0x47A7F6..0x47A807),
 * not at the bottom edge of the client rect. Hit-testing must start at the
 * identical coordinate or a click resolves to an older message. */
#define LINE_BOTTOM_PAD   25

/* ItemInfo kind reserved for chat links; see ItemListType.ChatLink. */
#define KIND_CHAT_LINK    200
#define PACKET_ITEM_INFO  0x0007000D

/* Swallowed markup the server wraps a link in, and the reply prefix. */
#define LINK_OPEN_A  'O'
#define LINK_ID_LEN  4
static const char link_digits[] = "ADFHIJLQTVZ";
static const char reply_sentinel[] = "#O#J";

/* __thiscall modelled as __fastcall: ecx = this, edx unused. AppendString
 * takes one stack argument and returns with `ret 4`, which __fastcall's
 * callee-cleans matches exactly. */
typedef int (__fastcall *input_append_fn)(void *ecx, void *edx, const char *text);
typedef int (__fastcall *input_length_fn)(void *ecx, void *edx);

static int       g_enabled = 1;
static int       g_installed;
static mw_hook_t g_click_hook;
static void     *g_click_cont;      /* trampoline: re-runs the stolen mov */
static void     *g_click_consume;   /* the block's own exit jump */
static unsigned char *g_grid;       /* the 24 visible inventory slots */
static int       g_diag_left = 12;  /* first clicks get a log line */
static mw_hook_t g_pet_click_hook;
static void     *g_pet_click_cont;
static void     *g_pet_click_consume;
static unsigned char *g_pet_grid;
static int      *g_pet_page;
static int       g_pet_diag_left = 12;
static mw_hook_t g_reply_hook;
static void     *g_reply_cont;
typedef void(__cdecl * present_fn)(void *context);
static present_fn *g_present_slot;  /* the client's own screen-present pointer */
static present_fn  g_present_orig;
static present_fn  g_present_saved; /* claim-time value restored on shutdown */
static OSMW_TOKEN  g_present_claim;
static int         g_present_disabled; /* a claim/write failure is permanent */
static int       g_awaiting;        /* a link reply we asked for is outstanding */
static int       g_pending_id = -1; /* which link that reply is for */
static int       g_line_diag_left = 12;

/* --- composing ------------------------------------------------------------ */

static void *input_instance(void)
{
    return (void *)mw_addr[MW_CHATINPUTINSTANCE];
}

/* The widget's own cap on how much text it will hold. Read from the instance
 * rather than assumed, so a client that ships a different limit is followed. */
static int input_capacity(void)
{
    void *inst = input_instance();
    int   cap;

    if (!inst)
        return 0;
    cap = *(int *)((unsigned char *)inst + INPUT_MAX_OFFSET);
    return cap < 0 ? 0x7FFFFFFF : cap;   /* -1 means unlimited */
}

static int input_length(void)
{
    if (!mw_addr[MW_CHATINPUTLENGTH] || !input_instance())
        return 0;
    return ((input_length_fn)mw_addr[MW_CHATINPUTLENGTH])(input_instance(), NULL);
}

/* [[i<kind><hi><lo>]] -- 'I' is inventory and 'P' is a pet-bag row.
 *
 * Letters, never digits: '#' plus a digit is an emoticon to the client's own
 * send path, which refuses a message carrying more than three of them, and the
 * slot has to survive that. Two base-26 letters cover the 96 global slots with
 * room to spare. Must match parseComposeTokens in ChatLinks.ts.
 */
static void build_token(char *out, int size, char kind, int slot)
{
    _snprintf(out, size, "[[i%c%c%c]]", kind,
              'A' + (slot / 26), 'A' + (slot % 26));
    out[size - 1] = 0;
}

/* Put the token in the box. Returns 1 when it went in whole. */
static int insert_token(char kind, int slot)
{
    char token[16];
    int  used, room, need;

    if (!input_instance() || !mw_addr[MW_CHATINPUTAPPEND])
        return 0;

    build_token(token, sizeof(token), kind, slot);
    need = (int)strlen(token);

    /* The widget enforces its cap one character at a time: once full it drops
     * the rest and reports failure, so AppendString on a nearly-full box
     * inserts a PREFIX of the token. Half a token is not a token -- it passes
     * the server's sanitiser untouched and gets broadcast as literal text --
     * so the room is checked up front and the whole thing is refused. */
    used = input_length();
    room = input_capacity() - used;
    if (room < need) {
        mw_log("links: no room for a link (%d of %d used, need %d) -- not inserted",
               used, input_capacity(), need);
        return 0;
    }

    ((input_append_fn)mw_addr[MW_CHATINPUTAPPEND])(input_instance(), NULL, token);
    mw_log("links: inserted %s (slot %d)", token, slot);
    return 1;
}

/* Decide whether this click is ours. 1 = consumed, 0 = let the client have it.
 *
 * `control` is the panel's raw control index, straight out of ebx. Called from
 * the naked gate with the caller-saved registers already pushed, so this is
 * ordinary C.
 */
static int __cdecl link_click(int control)
{
    int slot, page, global, alt, id;

    slot = control - CONTROL_FIRST;
    if (slot < 0 || slot >= PAGE_SLOTS)
        return 0;

    /* GetKeyState, not GetAsyncKeyState. It reads the calling thread's own
     * message-queue state, so it is focus-correct for free and stays right
     * when several clients are running -- the reason block.c reads Ctrl this
     * way too. This runs on the game thread, inside the click being handled. */
    alt = (GetKeyState(VK_MENU) & 0x8000) ? 1 : 0;

    page = mw_addr[MW_INVPAGE] ? *(int *)mw_addr[MW_INVPAGE] : 0;
    id   = g_grid ? *(int *)(g_grid + (size_t)slot * GRID_STRIDE) : 0;

    /* Bounded so a play session cannot fill the log, and silent unless
     * [Debug] ModLogging is on. */
    if (g_diag_left > 0) {
        g_diag_left--;
        mw_log("links: slot click -- control=0x%02x slot=%d page=%d id=%d alt=%d enabled=%d",
               control, slot, page, id, alt, g_enabled);
    }

    if (!g_enabled || !alt)
        return 0;

    /* Linking an empty slot would cost a round trip to be told nothing is
     * there. A negative id is the client's own empty marker. */
    if (id < 0)
        return 0;

    if (page < 0 || page >= PAGE_COUNT)
        page = 0;

    /* Inventory slots are global across the four pages of 24. The client's own
     * ItemInfo request builder does this same arithmetic for kind 0. */
    global = page * PAGE_SLOTS + slot;

    if (!insert_token('I', global))
        return 0;

    /* Only report consumption once the token is actually in the box: a refused
     * insert must leave the click alone rather than silently eating it. */
    return 1;
}

/* Naked, and JMPed to rather than CALLed -- the same rule as block.c's gates.
 *
 * Order matters twice: `add esp, 4` writes flags, so it has to come before the
 * `test`, and `pop` does not, so the registers can be restored between the
 * test and the branch. ebx is the control index the block is about to decode.
 */
static void __declspec(naked) click_gate(void)
{
    __asm {
        push eax
        push ecx
        push edx
        push ebx
        call link_click
        add  esp, 4
        test eax, eax
        pop  edx
        pop  ecx
        pop  eax
        jnz  consume
        jmp  DWORD PTR [g_click_cont]
    consume:
        jmp  DWORD PTR [g_click_consume]
    }
}

/* --- the panel -------------------------------------------------------------
 *
 * The client's OWN tooltip renderer draws it, so a linked item looks exactly
 * like the same item hovered in the inventory. No GDI, no markup parser, no
 * palette to keep in step. The first version hand-drew a panel in the chat
 * pane; matching the game's look by hand is a losing game, and it also put the
 * panel in the wrong window.
 *
 *     0044c760  tooltip(int zero, void *record)   __cdecl
 *
 * It lays the text at the client's tooltip buffer out, frames it, and
 * positions itself from the mouse globals, clamped to the 800x600 surface:
 *
 *     0044c7c2  mov ecx, [mouseY]      ; renderer + 0x64
 *     0044c7c8  mov eax, [mouseX]      ; renderer + 0x69
 *     0044c7cd  ...  x = mouseX - 0x64 ; y = mouseY - height - 0x10
 *
 * Both globals are read out of the renderer itself at those fixed offsets, so
 * they cannot drift from the function that consumes them. Centring is then
 * just parking the mouse position for the duration of the call and putting it
 * straight back -- nothing else observes it in between, because this all
 * happens inside one frame's draw.
 *
 * The `record` argument is only dereferenced for two validity tests ([+0] >= 0
 * and word [+8] >= 0) and the "locked" byte at +0x10, which a zeroed struct
 * leaves unset. So a static zeroed record draws the text and nothing else.
 *
 * WHERE IT IS DRAWN. Not when the reply arrives: the draw context 0x7e6a04 is
 * only valid inside a render, so the call has to be made from the frame.
 *
 * The obvious site is wrong, and it is worth recording why. The window
 * manager's draw pass 0x45e7b0(list) walks its list calling [node+0x20] and
 * falls out at 0x45e8ee, which looks like "after every window has drawn". But
 * the pass opens with
 *
 *     0045e7c6  mov eax, [esi]      ; list head
 *     0045e7c8  cmp eax, esi
 *     0045e7cf  je  0x45e95b        ; EMPTY LIST -> straight to the exit
 *
 * so with no panel open it never reaches 0x45e8ee at all. The chat window is a
 * real Win32 window, not a manager node, so in ordinary play both lists are
 * empty and the hook simply never ran. That is exactly what the symptom said:
 * the log reported the panel shown and nothing appeared, until F9 opened the
 * timer panel, at which point the tooltip appeared alongside it -- one window
 * in the list was all it took.
 *
 * The site that always runs is the screen present, an indirect call through a
 * function pointer at 0x7e69f4 that every one of the client's ~60 frame loops
 * ends with:
 *
 *     push [0x7e6a04]  ; the draw context
 *     call [0x7e69f4]
 *     add  esp, 4
 *
 * So this hooks no code at all -- it swaps that pointer for a __cdecl thunk
 * that draws and chains. One slot covers every loop the client has. The only
 * writer is the display-mode setter 0x4a0bb0, which picks between 0x4a0bf0 and
 * 0x4a0d20; mw_links_poll re-asserts the swap in case it ever runs again.
 *
 * The text is written into the client's buffer only for the duration of the
 * call and restored afterwards, so hovering an item while the panel is open
 * still shows that item rather than this one.
 */
#define TOOLTIP_MOUSE_Y_OFF  0x64   /* disp32 inside the renderer */
#define TOOLTIP_MOUSE_X_OFF  0x69
#define TOOLTIP_BUF_MAX      0x133  /* the client's own bound at 0x4567be */

/* Stock trade/vendor entries use file+10000 to distinguish an animated pet
 * model from an item sprite. The server puts that same display code in the
 * swallowed reply field. Never pass a tagged value to the item renderer: it
 * would become an out-of-bounds index into the item-sprite pointer table. */
#define PET_FILE_TAG          10000
#define PET_FALLBACK_SPRITE      50

/* Horizontally centred on the 800x600 surface: the renderer places the box at
 * mouseX - 0x64 with a fixed 0xc8 width, so 400 puts it in the middle.
 *
 * Vertically it is pinned near the TOP, and the height the renderer computes
 * for itself no longer matters. The layout is
 *
 *     0044c7cf  sub esi, ebp        ; y = mouseY - height
 *     0044c7d4  sub esi, 0x10       ;   - 0x10
 *     0044c7df  jge 0x44c7e4
 *     0044c7e1  lea esi, [ecx+0x20] ; NEGATIVE -> y = mouseY + 0x20
 *
 * so an anchor low enough to drive that subtraction negative lands the box at
 * anchor + 0x20 flat, with no height term at all. The renderer floors height at
 * 0x50 (0044c7b8), so any anchor below 0x50 + 0x10 = 96 always takes that
 * branch. That is what stops a long tooltip running off the bottom: it grows
 * downwards from a fixed top instead of upwards from a fixed bottom.
 *
 * Below the box the renderer still clamps a bottom overrun to 600 - height, so
 * a tooltip taller than the screen is pushed back up rather than cut off. */
#define PANEL_TOP       72
#define PANEL_ANCHOR_X  400
#define PANEL_ANCHOR_Y  (PANEL_TOP - 0x20)   /* must stay below 96; see above */

/* The item record the renderer is handed. Same 20-byte shape the shop list
 * ships (see ShopPackets.npcVendor): int32 id at +0, uint16 sprite at +8. Only
 * three fields are touched:
 *
 *   +0   int32   validity only -- `cmp [eax],0 / jl` and never read again
 *   +8   uint16  the SPRITE, `movsx ecx,[eax+8] / mov edx,[ecx*4+0x817000]`
 *   +0x10 byte   stack count, drawn only when the first argument is non-zero,
 *                and we pass zero, so it is left alone
 *
 * There is no "draw no icon" value: a negative sprite fails the renderer's own
 * validity test at 0x44c778 and it returns having drawn nothing at all. So the
 * sprite has to be supplied, and a zeroed record means sprite 0 for every item
 * -- which is why the first build drew the same fruit whatever you linked. The
 * server sends the real index encoded onto the front of the reply. */
#define RECORD_SIZE        20
#define RECORD_SPRITE_OFF   8

/* Stable operands/calls within MW_TOOLTIP. They supply the exact text engine,
 * draw context, skin and frame functions used by both stock item and pet
 * tooltips, without another set of signatures for the same dependencies. */
#define TOOLTIP_TEXT_SLOT_OFF       0x20
#define TOOLTIP_DRAW_CONTEXT_OFF    0xAA
#define TOOLTIP_SKIN_OFF            0xC6
#define TOOLTIP_FRAME_CALL_OFF      0xB7
#define TOOLTIP_INNER_CALL_OFF      0xDD

/* Calls within MW_PETTOOLTIPBRANCH, whose optional signature is anchored on
 * the stock `file >= 10000` test. */
#define PET_FIND_CALL_OFF           0x289
#define PET_RELEASE_CALL_OFF        0x2A3
#define PET_LOAD_CALL_OFF           0x2B8
#define PET_DRAW_CALL_OFF           0x30A

#define PET_PANEL_WIDTH             0x128
#define PET_TEXT_X                  0x78
#define PET_MODEL_X                 0x44
#define PET_MODEL_Y                 0x60

typedef void (__cdecl *panel_frame_fn)(int x, int y, int width, int height,
                                        void *context);
typedef void (__cdecl *panel_inner_fn)(int x, int y, int width, int height,
                                        int skin, void *context);
/* MSVC's C front-end has no __thiscall function-pointer syntax. Model these
 * vtable calls as __fastcall: self goes in ecx, the explicit unused argument
 * occupies edx, and the real arguments retain the stock callee-clean stack. */
typedef int  (__fastcall *tooltip_measure_fn)(void *self, void *unused,
                                               const char *text, RECT *bounds,
                                               int, int, int);
typedef void (__fastcall *tooltip_text_fn)(void *self, void *unused,
                                            int x, int y, int width, int height,
                                            const char *text);
typedef void (__fastcall *tooltip_flush_fn)(void *self, void *unused,
                                             int, int, DWORD, void *context,
                                             int);
typedef int  (__cdecl *pet_mda_find_fn)(const char *path);
typedef void (__cdecl *pet_mda_release_fn)(int handle);
typedef void (__cdecl *pet_mda_load_fn)(const char *path, int *handle);
typedef void (__cdecl *pet_mda_draw_fn)(int handle, const char *sequence,
                                         int x, int y, void *context, int frame,
                                         int, int, int);

static int  g_panel_on;
static int  g_panel_is_pet;
static int  g_panel_pet_file;
static char g_panel_text[TOOLTIP_BUF_MAX];
static unsigned char g_panel_record[RECORD_SIZE];
static int  g_rbutton_down;   /* edge state for right-click-to-close */
static int  g_escape_down;
static int  g_pet_renderer_ready;
static int  g_pet_model_handle = -1;
static DWORD g_pet_anim_deadline;
static pet_mda_find_fn    g_pet_mda_find;
static pet_mda_release_fn g_pet_mda_release;
static pet_mda_load_fn    g_pet_mda_load;
static pet_mda_draw_fn    g_pet_mda_draw;

static void *call_target(unsigned char *site)
{
    if (!site || site[0] != 0xE8)
        return NULL;
    return site + 5 + *(LONG *)(site + 1);
}

static int prepare_pet_renderer(void)
{
    unsigned char *branch = mw_addr[MW_PETTOOLTIPBRANCH];
    unsigned char *renderer = mw_addr[MW_TOOLTIP];

    g_pet_renderer_ready = 0;
    g_pet_mda_find = NULL;
    g_pet_mda_release = NULL;
    g_pet_mda_load = NULL;
    g_pet_mda_draw = NULL;
    if (!branch || !renderer)
        return 0;

    /* Guard every internal offset before trusting it. The signature proves
     * the branch identity; these opcode checks prove its remaining layout. */
    if (renderer[0x1E] != 0x8B || renderer[0x1F] != 0x0D ||
        renderer[0xA8] != 0x8B || renderer[0xA9] != 0x15 ||
        renderer[0xC3] != 0x66 || renderer[0xC4] != 0x8B ||
        renderer[0xC5] != 0x0D ||
        !call_target(renderer + TOOLTIP_FRAME_CALL_OFF) ||
        !call_target(renderer + TOOLTIP_INNER_CALL_OFF) ||
        !call_target(branch + PET_FIND_CALL_OFF) ||
        !call_target(branch + PET_RELEASE_CALL_OFF) ||
        !call_target(branch + PET_LOAD_CALL_OFF) ||
        !call_target(branch + PET_DRAW_CALL_OFF)) {
        mw_log("links: stock pet tooltip layout changed -- using safe item-icon fallback");
        return 0;
    }

    g_pet_mda_find = (pet_mda_find_fn)call_target(branch + PET_FIND_CALL_OFF);
    g_pet_mda_release = (pet_mda_release_fn)call_target(branch + PET_RELEASE_CALL_OFF);
    g_pet_mda_load = (pet_mda_load_fn)call_target(branch + PET_LOAD_CALL_OFF);
    g_pet_mda_draw = (pet_mda_draw_fn)call_target(branch + PET_DRAW_CALL_OFF);
    g_pet_renderer_ready = 1;
    mw_log("links: stock animated pet tooltip renderer available");
    return 1;
}

static void panel_show(const char *text, int display_code)
{
    int sprite = display_code;

    strncpy(g_panel_text, text, sizeof(g_panel_text) - 1);
    g_panel_text[sizeof(g_panel_text) - 1] = 0;
    g_panel_is_pet = display_code >= PET_FILE_TAG;
    g_panel_pet_file = g_panel_is_pet ? display_code - PET_FILE_TAG : -1;
    if (g_panel_is_pet)
        sprite = PET_FALLBACK_SPRITE;
    memset(g_panel_record, 0, sizeof(g_panel_record));
    *(unsigned short *)(g_panel_record + RECORD_SPRITE_OFF) = (unsigned short)sprite;
    g_panel_on = 1;
}

void mw_links_dismiss(void)
{
    g_panel_on = 0;
}

static int pet_model(int file)
{
    char path[64];
    int handle;

    if (!g_pet_renderer_ready || file < 0)
        return -1;

    _snprintf(path, sizeof(path), "NPC\\NPC%da.MDA", file);
    path[sizeof(path) - 1] = 0;
    handle = g_pet_mda_find(path);
    if (handle < 0) {
        /* This is the stock branch's lifetime policy: release the previous
         * tooltip handle only when a different model must actually load. */
        if (g_pet_model_handle >= 0)
            g_pet_mda_release(g_pet_model_handle);
        handle = -1;
        g_pet_mda_load(path, &handle);
    }
    if (handle >= 0)
        g_pet_model_handle = handle;
    return handle;
}

static void panel_draw_pet(void)
{
    unsigned char     *renderer = mw_addr[MW_TOOLTIP];
    void              **text_slot;
    void              *text;
    void              **vtable;
    void              **context_slot;
    void              *context;
    unsigned short    *skin;
    panel_frame_fn     frame;
    panel_inner_fn     inner;
    tooltip_measure_fn measure;
    tooltip_text_fn    draw_text;
    tooltip_flush_fn   flush;
    RECT               bounds;
    int                x, y, height, handle, anim_frame;
    DWORD              now;

    if (!g_pet_renderer_ready || !renderer)
        return;

    text_slot = *(void ***)(renderer + TOOLTIP_TEXT_SLOT_OFF);
    context_slot = *(void ***)(renderer + TOOLTIP_DRAW_CONTEXT_OFF);
    skin = *(unsigned short **)(renderer + TOOLTIP_SKIN_OFF);
    frame = (panel_frame_fn)call_target(renderer + TOOLTIP_FRAME_CALL_OFF);
    inner = (panel_inner_fn)call_target(renderer + TOOLTIP_INNER_CALL_OFF);
    if (!text_slot || !*text_slot || !context_slot || !*context_slot ||
        !skin || !frame || !inner)
        return;

    text = *text_slot;
    context = *context_slot;
    vtable = *(void ***)text;
    if (!vtable || !vtable[5] || !vtable[8] || !vtable[4])
        return;
    measure = (tooltip_measure_fn)vtable[5];
    draw_text = (tooltip_text_fn)vtable[8];
    flush = (tooltip_flush_fn)vtable[4];

    bounds.left = 0;
    bounds.top = 0;
    bounds.right = 0xA0;
    bounds.bottom = 0x320;
    measure(text, NULL, g_panel_text, &bounds, 0, 0, 0);
    height = bounds.bottom + 0x20;
    if (height < 0x50)
        height = 0x50;

    x = (800 - PET_PANEL_WIDTH) / 2;
    y = PANEL_TOP;
    if (y + height > 600)
        y = 600 - height;
    if (y < 0)
        y = 0;

    frame(x, y, PET_PANEL_WIDTH, height, context);
    inner(x + 2, y + 2, PET_PANEL_WIDTH - 4, height - 6,
          (int)*skin, context);
    draw_text(text, NULL, x + PET_TEXT_X, y + 0x10, bounds.right, height,
              g_panel_text);
    flush(text, NULL,
          *(int *)((unsigned char *)text_slot + 4),
          *(int *)((unsigned char *)text_slot + 8),
          0x3D8F5C29U, context, 0);

    handle = pet_model(g_panel_pet_file);
    if (handle < 0)
        return;

    anim_frame = 0x10;
    now = GetTickCount();
    if ((LONG)(now - g_pet_anim_deadline) > 0) {
        g_pet_anim_deadline = now + 40;
        anim_frame = 0x12;
    }
    g_pet_mda_draw(handle, "MainCommand3",
                   x + PET_MODEL_X, y + PET_MODEL_Y, context, anim_frame,
                   0, 0, 0);
}

/* Called once per frame from the end of the manager's draw loop. */
static void __cdecl panel_draw(void)
{
    typedef void(__cdecl * tooltip_fn)(int zero, void *record);
    unsigned char *renderer = mw_addr[MW_TOOLTIP];
    char          *buffer;
    int           *mouse_x, *mouse_y;
    char           saved[TOOLTIP_BUF_MAX];
    int            saved_x, saved_y;

    if (!g_panel_on || !renderer || !g_panel_text[0])
        return;

    if (g_panel_is_pet && g_pet_renderer_ready) {
        panel_draw_pet();
        return;
    }

    mouse_y = *(int **)(renderer + TOOLTIP_MOUSE_Y_OFF);
    mouse_x = *(int **)(renderer + TOOLTIP_MOUSE_X_OFF);
    buffer  = (char *)mw_addr[MW_TOOLTIPBUF];
    if (!mouse_x || !mouse_y || !buffer)
        return;

    memcpy(saved, buffer, sizeof(saved));
    memcpy(buffer, g_panel_text, sizeof(g_panel_text));

    saved_x  = *mouse_x;
    saved_y  = *mouse_y;
    *mouse_x = PANEL_ANCHOR_X;
    *mouse_y = PANEL_ANCHOR_Y;

    ((tooltip_fn)renderer)(0, g_panel_record);

    *mouse_x = saved_x;
    *mouse_y = saved_y;
    memcpy(buffer, saved, sizeof(saved));
}

/* Ways the panel closes, all polled from the same per-frame thunk that draws it.
 *
 * Right-click, because that is what closes every other window in this client.
 * Escape, because it opens the logoff menu and the panel would otherwise sit on
 * top of it. Both have to be caught here rather than in the chat window's
 * message handler: the panel is drawn over the GAME window, and the left click
 * the first build relied on only dismisses it while the cursor happens to be in
 * the chat pane -- no use at all once an active channel has scrolled the link
 * away.
 *
 * GetKeyState, not GetAsyncKeyState, for the reason block.c records: it reports
 * the state as of the message this thread is processing, so it follows focus and
 * stays correct with several clients running. This runs on the game thread,
 * inside its own frame, so that is exactly the state we want.
 *
 * And leaving the world closes it, which is not a key at all. Drawing from the
 * screen present means drawing on EVERY screen the client has, so a panel left
 * open at logout reappears over the server-select list. The client's game state
 * is the gate: below GAME_STATE_READY it is loading or at login, and Maps++ has
 * used the same global for the same purpose for years. */
#define GAME_STATE_READY 5

static int in_world(void)
{
    const int *state = (const int *)mw_addr[MW_GAMESTATE];

    return !state || *state >= GAME_STATE_READY;
}

static void poll_dismiss(void)
{
    int rbutton = (GetKeyState(VK_RBUTTON) & 0x8000) != 0;
    int escape  = (GetKeyState(VK_ESCAPE) & 0x8000) != 0;

    if (!g_panel_on) {
        g_rbutton_down = rbutton;
        g_escape_down  = escape;
        return;
    }

    if ((rbutton && !g_rbutton_down) || (escape && !g_escape_down) || !in_world())
        g_panel_on = 0;

    g_rbutton_down = rbutton;
    g_escape_down  = escape;
}

static void __cdecl present_gate(void *context)
{
    poll_dismiss();
    panel_draw();
    g_present_orig(context);
}

/* The owned-pet panel shows six roster rows at control ids 0x64..0x69. Its
 * scroll position is the first visible pet, so row + page gives the stable
 * 0..19 slot used by the server's player.pets array. */
static int __cdecl pet_link_click(int row)
{
    int slot;
    int alt;
    DWORD id;

    if (!g_pet_page || row < PET_ROW_FIRST ||
        row >= PET_ROW_FIRST + PET_VISIBLE_ROWS)
        return 0;

    slot = *g_pet_page + row - PET_ROW_FIRST;
    if (slot < 0 || slot >= PET_SLOTS)
        return 0;

    alt = (GetKeyState(VK_MENU) & 0x8000) ? 1 : 0;
    id = g_pet_grid ? *(DWORD *)(g_pet_grid + (size_t)slot * PET_STRIDE) : 0;

    if (g_pet_diag_left > 0) {
        g_pet_diag_left--;
        mw_log("links: pet roster click -- row=%d slot=%d id=%lu alt=%d enabled=%d",
               row, slot, (unsigned long)id, alt, g_enabled);
    }

    if (!g_enabled || !alt || id == 0)
        return 0;

    /* Once a populated ALT+click is ours, always consume it. If the input is
     * full, insert_token refuses the whole token; falling through to stock
     * here would still select the pet, violating the link gesture contract. */
    (void)insert_token('P', slot);
    return 1;
}

/* edi is the roster control id. The stolen six-byte instruction only loads the
 * current page; the trampoline then performs the stock row-to-slot mapping. */
static void __declspec(naked) pet_click_gate(void)
{
    __asm {
        push eax
        push ecx
        push edx
        push edi
        call pet_link_click
        add  esp, 4
        test eax, eax
        pop  edx
        pop  ecx
        pop  eax
        jnz  consume
        jmp  DWORD PTR [g_pet_click_cont]
    consume:
        jmp  DWORD PTR [g_pet_click_consume]
    }
}

/* Core autoload reaches this DLL before the client has necessarily selected
 * and published its screen-present implementation. The resolved data address
 * is valid then, but the pointer stored there can still be NULL. Acquire this
 * one data slot later from a game-thread tick once the client fills it in. */
static int try_install_present_hook(void)
{
    present_fn *slot;
    present_fn  replacement = present_gate;
    OSMW_RESULT result;

    if (g_present_slot)
        return 1;
    if (g_present_disabled || !g_installed)
        return 0;

    slot = (present_fn *)mw_addr[MW_SCREENPRESENT];
    if (!slot || !*slot)
        return 0;

    g_present_orig  = *slot;
    g_present_saved = *slot;
    result = mw_claim_region(slot, sizeof(*slot), OSMW_REGION_DATA,
                             "screen-present function pointer",
                             &g_present_claim);
    if (result != OSMW_OK) {
        mw_log("links: deferred present slot claim failed (%ld) -- links "
               "open but draw nothing", (long)result);
        g_present_orig = NULL;
        g_present_saved = NULL;
        g_present_disabled = 1;
        return 0;
    }
    if (!mw_write_bytes(slot, &replacement, sizeof(replacement))) {
        mw_log("links: deferred present slot write failed -- links open but "
               "draw nothing");
        (void)mw_release_region(&g_present_claim);
        g_present_orig = NULL;
        g_present_saved = NULL;
        g_present_disabled = 1;
        return 0;
    }

    g_present_slot = slot;
    mw_log("links: screen-present hook active (%p -> %p)",
           g_present_saved, present_gate);
    return 1;
}

/* --- clicking a link ------------------------------------------------------
 *
 * The chat window's visible lines are 0x410-byte records at chatobj+0x80,
 * drawn BOTTOM-UP from index 0, one record per message however many rows it
 * wraps to (see tabs.c). The bottom anchor is top+bottom-25, copied from the
 * stock DrawLines arithmetic; it is not the client edge. A click's y maps to
 * a record by walking up from that anchor and accumulating +0x408 heights.
 *
 * The record's text at +0x000 is the COMPOSED line, and composition happens
 * before markup is stripped, so the server's link markers are still in it.
 *
 * Whole message, not a sub-span: a click anywhere on a line carrying a link
 * opens that link. Measuring x would mean reproducing the client's wrap, and
 * the client's own HitTest cannot help -- it fires on the sender NAME, for
 * click-to-PM, not on the body. A line without a link is passed straight
 * through, so click-to-PM and everything else is untouched.
 */

/* Decode LINK_ID_LEN swallowed `#x` pairs. Returns the value, or -1. Must
 * agree with encodeChatLinkId in ChatLinks.ts: base strlen(link_digits), most
 * significant digit first. */
static int decode_digits(const char *p)
{
    int id = 0, i;

    for (i = 0; i < LINK_ID_LEN; i++) {
        const char *d;
        if (p[0] != '#' || !p[1])
            return -1;
        d = strchr(link_digits, p[1]);
        if (!d)
            return -1;
        id = id * (int)(sizeof(link_digits) - 1) + (int)(d - link_digits);
        p += 2;
    }
    return id;
}

/* Decode one `#O` + LINK_ID_LEN swallowed `#x` pairs. Returns the id, or -1. */
static int decode_link_id(const char *p)
{
    if (p[0] != '#' || p[1] != LINK_OPEN_A)
        return -1;
    return decode_digits(p + 2);
}

/* The first link in a composed line, or -1. */
static int first_link_in(const char *text)
{
    int i;

    for (i = 0; text[i] && i < LINE_TEXT_MAX - 2; i++) {
        if (text[i] == '#' && text[i + 1] == LINK_OPEN_A) {
            int id = decode_link_id(text + i);
            if (id >= 0)
                return id;
        }
    }
    return -1;
}

/* Which visible line record contains window y, or NULL. Records are drawn
 * bottom-up from index 0, one per message, so walk up from the pane bottom
 * accumulating heights. */
static unsigned char *line_at(int y, int *record_index, int *bottom_anchor)
{
    unsigned char *obj = mw_addr[MW_OBJECT];
    RECT          *cr;
    int            count, i, bottom;

    if (!obj)
        return NULL;

    cr     = (RECT *)(obj + OBJ_CLIENT_RECT);
    bottom = cr->top + cr->bottom - LINE_BOTTOM_PAD;
    if (bottom_anchor)
        *bottom_anchor = bottom;
    count  = obj[OBJ_LINE_COUNT] & 0x7F;

    for (i = 0; i < count; i++) {
        unsigned char *rec = obj + OBJ_LINE_STORE + (size_t)i * LINE_STRIDE;
        int            h   = *(int *)(rec + LINE_HEIGHT_OFF);

        if (h <= 0)
            break;
        if (y > bottom - h && y <= bottom) {
            if (record_index)
                *record_index = i;
            return rec;
        }
        bottom -= h;
        if (bottom <= 0)
            break;
    }
    return NULL;
}

int mw_links_line_occupied(int y)
{
    return line_at(y, NULL, NULL) != NULL;
}

/* Recently opened links, so re-opening one costs no round trip.
 *
 * Re-opening is the ordinary way this feature is used -- open, read, close,
 * check it again -- and every one of those was a fresh ItemInfo request. It is
 * also the shape of the obvious abuse, holding a link and clicking it flat out.
 * The server rate-limits that (ActionType.ChatLinkInspect) because a modified
 * client can ignore everything here; this just stops honest clients generating
 * the traffic in the first place.
 *
 * Safe to cache because the server froze the item at link time, and the reply
 * was rendered for THIS viewer, so nothing in it belongs to anyone else. What
 * can drift is the market-price line and the reader's own level colouring, so
 * entries expire; the window only has to outlast a bout of clicking. */
#define LINK_CACHE_SLOTS  4
#define LINK_CACHE_TTL_MS 60000

typedef struct {
    int   id;                      /* -1 when the slot is empty */
    int   display_code;
    DWORD stamp;
    char  text[TOOLTIP_BUF_MAX];
} link_cache_t;

static link_cache_t g_cache[LINK_CACHE_SLOTS];
static int          g_cache_next;

/* A live cached reply for `id`, or NULL. DWORD subtraction, so the tick
 * counter's 49-day wrap needs no special case. */
static const link_cache_t *cache_find(int id)
{
    int i;

    for (i = 0; i < LINK_CACHE_SLOTS; i++) {
        if (g_cache[i].id == id && GetTickCount() - g_cache[i].stamp <= LINK_CACHE_TTL_MS)
            return &g_cache[i];
    }
    return NULL;
}

static void cache_put(int id, int display_code, const char *text)
{
    link_cache_t *slot;
    int           i;

    if (id < 0)
        return;

    /* Refresh in place if it is already here, so clicking one link repeatedly
     * cannot evict the other three. */
    slot = NULL;
    for (i = 0; i < LINK_CACHE_SLOTS; i++) {
        if (g_cache[i].id == id)
            slot = &g_cache[i];
    }
    if (!slot) {
        slot = &g_cache[g_cache_next];
        g_cache_next = (g_cache_next + 1) % LINK_CACHE_SLOTS;
    }

    slot->id     = id;
    slot->display_code = display_code;
    slot->stamp  = GetTickCount();
    strncpy(slot->text, text, sizeof(slot->text) - 1);
    slot->text[sizeof(slot->text) - 1] = 0;
}

/* Ask the server for a link's item. Built and sent directly rather than
 * through the client's request builder, which exists to add the inventory page
 * offset and is no use here. */
static void request_link(int id)
{
    const OSMW_CORE_API_V1 *api = mw_core_api();
    OSMW_RESULT result;

    if (api == NULL || api->queue_game_packet == NULL)
        return;

    result = api->queue_game_packet(
        mw_core_owner(), PACKET_ITEM_INFO,
        (DWORD)((id & 0xFFFF) | (KIND_CHAT_LINK << 16)), NULL, 0);
    if (result != OSMW_OK) {
        mw_log("links: queue request %d failed (%ld)", id, (long)result);
        return;
    }
    g_awaiting = 1;
    g_pending_id = id;
    mw_log("links: requested link %d", id);
}

int mw_links_on_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT *result)
{
    const link_cache_t *cached;
    unsigned char      *rec;
    int                 id, y, record_index, bottom_anchor, had_panel;

    (void)hwnd;
    (void)wp;

    if (msg != WM_LBUTTONDOWN || !g_enabled)
        return 0;

    /* An ordinary click dismisses the panel. A click on another linked line
     * continues below and replaces it immediately; requiring a second click
     * to open the new link makes a valid first click look broken. */
    had_panel = g_panel_on;
    if (had_panel)
        g_panel_on = 0;

    y = GET_Y_LPARAM(lp);
    record_index = -1;
    bottom_anchor = -1;
    rec = line_at(y, &record_index, &bottom_anchor);
    if (!rec) {
        if (had_panel) {
            *result = 0;
            return 1;
        }
        return 0;
    }

    id = first_link_in((const char *)rec);
    if (id < 0) {
        if (had_panel) {
            *result = 0;
            return 1;
        }
        return 0;   /* no link on this line: leave the click alone */
    }

    if (g_line_diag_left > 0) {
        g_line_diag_left--;
        mw_log("links: chat line click -- y=%d anchor=%d record=%d id=%d",
               y, bottom_anchor, record_index, id);
    }

    cached = cache_find(id);
    if (cached) {
        panel_show(cached->text, cached->display_code);
        mw_log("links: link %d from cache", id);
    } else {
        request_link(id);
    }
    *result = 0;
    return 1;
}

/* The reply. esi points at the incoming text just before the client copies it
 * into its own tooltip buffer.
 *
 * Not consumed: letting the stock copy proceed costs nothing, because the
 * tooltip only draws while a game panel is hovering a slot. The sentinel is
 * what tells a link reply from an ordinary hover reply, so the two cannot
 * race. */
static void __cdecl link_reply(const char *text)
{
    const size_t n = sizeof(reply_sentinel) - 1;
    const char  *body;
    int          display_code;

    if (!text || !g_awaiting || strncmp(text, reply_sentinel, n) != 0)
        return;

    /* Values below 10000 are item sprite files. Values at or above it are the
     * stock pet discriminator and carry pet.file in the remainder. */
    display_code = decode_digits(text + n);
    body = text + n + (display_code < 0 ? 0 : LINK_ID_LEN * 2);
    if (display_code < 0) {
        display_code = 0;
        mw_log("links: reply carries no display code -- server is older than this mod");
    }

    g_awaiting = 0;
    cache_put(g_pending_id, display_code, body);
    panel_show(body, display_code);
    if (display_code >= PET_FILE_TAG)
        mw_log("links: pet panel shown (link %d, file %d, %d chars)%s",
               g_pending_id, display_code - PET_FILE_TAG, (int)strlen(body),
               g_pet_renderer_ready ? "" : " -- item-icon fallback");
    else
        mw_log("links: item panel shown (link %d, sprite %d, %d chars)",
               g_pending_id, display_code, (int)strlen(body));
}

static void __declspec(naked) reply_gate(void)
{
    __asm {
        push eax
        push ecx
        push edx
        push esi
        call link_reply
        add  esp, 4
        pop  edx
        pop  ecx
        pop  eax
        jmp  DWORD PTR [g_reply_cont]
    }
}

/* --- install -------------------------------------------------------------- */

void mw_links_poll(void)
{
    present_fn replacement = present_gate;

    g_enabled = mw_cfg_int("ItemLinks", 1);

    /* This is game-thread-safe and also preserves the display-mode repair
     * below once the delayed pointer has been acquired. */
    (void)try_install_present_hook();

    /* Re-assert the present swap if the display-mode setter has replaced it. */
    if (g_present_slot && *g_present_slot != present_gate && *g_present_slot) {
        g_present_orig = *g_present_slot;
        (void)mw_write_bytes(g_present_slot, &replacement,
                             sizeof(replacement));
    }
}

int mw_links_install(void)
{
    unsigned char *site = mw_addr[MW_INVCLICKSLOT];
    unsigned char *pet_site;
    int            i;

    if (g_installed)
        return 1;

    g_enabled = mw_cfg_int("ItemLinks", 1);

    if (!site || !mw_addr[MW_CHATINPUTINSTANCE] || !mw_addr[MW_CHATINPUTAPPEND] ||
        !mw_addr[MW_CHATINPUTLENGTH] || !mw_addr[MW_INVPAGE] || !mw_addr[MW_INVGRID]) {
        mw_log("links: required addresses missing -- item links disabled");
        return 0;
    }

    g_grid          = mw_addr[MW_INVGRID];
    g_click_consume = (void *)(site + CLICK_CONSUME_OFF);

    /* Link id 0 is real, so an empty slot has to be -1 rather than zeroed. */
    for (i = 0; i < LINK_CACHE_SLOTS; i++)
        g_cache[i].id = -1;

    (void)prepare_pet_renderer();

    mw_log("links: chat input %p (cap %d), grid %p, consume %p",
           input_instance(), input_capacity(), g_grid, g_click_consume);

    if (!mw_hook_install(&g_click_hook, site, (void *)click_gate,
                         CLICK_PROLOGUE, "inventory item-link click prologue")) {
        mw_log("links: could not hook the inventory click -- item links disabled");
        return 0;
    }
    g_click_cont = g_click_hook.trampoline;

    /* Optional pet compose path in the actual owned-pet panel. At entry, edi
     * is one of the six roster controls (0x64..0x69). The first instruction
     * loads the scroll position, the stock code maps it to a 0..19 slot, and
     * the unsigned-empty branch names the callback's common no-op exit. */
    pet_site = mw_addr[MW_PETCLICKSLOT];
    if (pet_site) {
        g_pet_page = *(int **)(pet_site + 2);
        g_pet_grid = *(unsigned char **)(pet_site + 15);
        g_pet_click_consume = pet_site + 25 + *(LONG *)(pet_site + 21);
        if (mw_hook_install(&g_pet_click_hook, pet_site, (void *)pet_click_gate,
                            6, "pet item-link click prologue")) {
            g_pet_click_cont = g_pet_click_hook.trampoline;
            mw_log("links: pet roster page %p, grid %p, consume %p",
                   g_pet_page, g_pet_grid, g_pet_click_consume);
        } else {
            g_pet_page = NULL;
            g_pet_grid = NULL;
            g_pet_click_consume = NULL;
            mw_log("links: could not hook the pet click -- pet compose disabled");
        }
    } else {
        mw_log("links: pet click signature unavailable -- pet compose disabled");
    }

    /* Optional: without these, composing still works and only opening a link
     * goes quiet. */
    if (mw_addr[MW_ITEMINFOREPLY] &&
        mw_hook_install(&g_reply_hook, mw_addr[MW_ITEMINFOREPLY],
                        (void *)reply_gate, 5,
                        "item-link reply prologue"))
        g_reply_cont = g_reply_hook.trampoline;
    else
        mw_log("links: reply hook unavailable -- links post but do not open");

    g_installed = 1;
    if (!try_install_present_hook())
        mw_log("links: present slot not initialised -- draw hook deferred");

    mw_log("links: installed (ALT+click an inventory item or pet to link it)%s",
           g_enabled ? "" : " -- currently off via [Chat] ItemLinks=0");
    return 1;
}

OSMW_RESULT mw_links_remove(void)
{
    OSMW_RESULT result;

    if (!g_installed)
        return OSMW_OK;
    if (g_present_slot && g_present_claim != OSMW_INVALID_TOKEN) {
        if (!mw_write_bytes(g_present_slot, &g_present_saved,
                            sizeof(g_present_saved))) {
            mw_log("links: present slot restore failed");
            return OSMW_E_INTERNAL;
        }
        result = mw_release_region(&g_present_claim);
        if (result != OSMW_OK)
            return result;
    }
    g_present_slot = NULL;
    g_present_orig = NULL;
    g_present_saved = NULL;
    g_present_disabled = 0;
    result = mw_hook_remove(&g_reply_hook);
    if (result != OSMW_OK)
        return result;
    result = mw_hook_remove(&g_pet_click_hook);
    if (result != OSMW_OK)
        return result;
    result = mw_hook_remove(&g_click_hook);
    if (result != OSMW_OK)
        return result;
    g_reply_cont    = NULL;
    g_click_cont    = NULL;
    g_click_consume = NULL;
    g_grid          = NULL;
    g_pet_click_cont = NULL;
    g_pet_click_consume = NULL;
    g_pet_grid = NULL;
    g_pet_page = NULL;
    g_pet_renderer_ready = 0;
    g_pet_mda_find = NULL;
    g_pet_mda_release = NULL;
    g_pet_mda_load = NULL;
    g_pet_mda_draw = NULL;
    g_pet_model_handle = -1;
    g_panel_is_pet = 0;
    g_panel_pet_file = -1;
    g_panel_on = 0;
    g_installed     = 0;
    return OSMW_OK;
}
