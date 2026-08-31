/* Client-level chat blocking: hide a character by name on this PC.
 *
 * The game's own ignore, if used, is per-character. This list lives in
 * [Chat] BlockedNames in the game folder's config.ini, so every character
 * on this install -- and every multi-boxed client sharing the folder --
 * shares it.
 *
 * Two gates:
 *   1. Append (0x004620C0): drop the message before it enters the real ring,
 *      so the overlay and every other reader never see it.
 *   2. keep_message: skip leftovers already in the ring when the chat window
 *      rebuilds. Overlay leftovers age out with the 1024-slot ring; we do
 *      not rewrite the real ring in place (that class of change crashed the
 *      client during tab work).
 *
 * Ctrl+left-click a sender name: the client's HitTest already has the name
 * in hand at the whisper-build site (HitTest+0xAA). Diverting there swallows
 * the whisper without reimplementing line geometry. Unblock is Settings-only.
 *
 * Append is an OPTIONAL signature: a miss stands this feature's overlay drop
 * down, not the rest of the chat mod. keep_message still hides leftovers in
 * the chat window from the name list alone.
 */
#include "osmw_chat.h"

#include <stdio.h>
#include <string.h>

#define MW_BLOCK_CAP      32
#define MW_BLOCK_NAME_MAX 20
#define MW_BLOCK_INI_MAX  1024

#define MSG_CHANNEL 0x04
#define MSG_NAME    0x08

#define CH_SYSTEM 6
#define CH_INFO   7

#define WHISPER_SITE  0xAA      /* HitTest + 0xAA: lea esi, [eax+MsgArray+8] */

#define MW_BLOCK_ADDED  1
#define MW_BLOCK_NOOP   0
#define MW_BLOCK_FULL  -1

static char g_names[MW_BLOCK_CAP][MW_BLOCK_NAME_MAX + 1];
static int  g_used;
static char g_raw[MW_BLOCK_INI_MAX];

static mw_hook_t g_append_hook;
static mw_hook_t g_whisper_hook;
static int       g_append_hooked;
static int       g_whisper_hooked;
static void     *g_whisper_cont;

static void *g_append_cont;

static void trim_copy(char *dst, int dst_size, const char *src)
{
    int n;

    if (!dst || dst_size <= 0)
        return;
    dst[0] = '\0';
    if (!src)
        return;
    while (*src == ' ' || *src == '\t')
        src++;
    n = (int)strlen(src);
    while (n > 0 && (src[n - 1] == ' ' || src[n - 1] == '\t'))
        n--;
    if (n >= dst_size)
        n = dst_size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int name_on_list(const char *name)
{
    int i;

    if (!name || !name[0])
        return 0;
    for (i = 0; i < g_used; i++) {
        if (_stricmp(g_names[i], name) == 0)
            return 1;
    }
    return 0;
}

static void persist(void)
{
    char buf[MW_BLOCK_INI_MAX];
    int  i, n = 0;

    buf[0] = '\0';
    for (i = 0; i < g_used; i++) {
        if (i) {
            if (n + 1 >= MW_BLOCK_INI_MAX)
                break;
            buf[n++] = ',';
            buf[n] = '\0';
        }
        _snprintf(buf + n, (size_t)(MW_BLOCK_INI_MAX - n), "%s", g_names[i]);
        buf[MW_BLOCK_INI_MAX - 1] = '\0';
        n = (int)strlen(buf);
    }
    mw_cfg_set_str("BlockedNames", buf);
    _snprintf(g_raw, sizeof(g_raw), "%s", buf);
    g_raw[sizeof(g_raw) - 1] = '\0';
}

static void parse_into(const char *raw)
{
    char  copy[MW_BLOCK_INI_MAX];
    char *p, *next;

    g_used = 0;
    if (!raw || !raw[0])
        return;

    _snprintf(copy, sizeof(copy), "%s", raw);
    copy[sizeof(copy) - 1] = '\0';

    p = copy;
    while (p && g_used < MW_BLOCK_CAP) {
        char one[MW_BLOCK_NAME_MAX + 1];
        int  i, dup;

        next = strchr(p, ',');
        if (next)
            *next++ = '\0';
        trim_copy(one, sizeof(one), p);
        p = next;

        if (!one[0] || strchr(one, ','))
            continue;
        if ((int)strlen(one) > MW_BLOCK_NAME_MAX)
            continue;

        dup = 0;
        for (i = 0; i < g_used; i++) {
            if (_stricmp(g_names[i], one) == 0) {
                dup = 1;
                break;
            }
        }
        if (dup)
            continue;
        _snprintf(g_names[g_used], sizeof(g_names[0]), "%s", one);
        g_names[g_used][MW_BLOCK_NAME_MAX] = '\0';
        g_used++;
    }
}

static int reload(void)
{
    char buf[MW_BLOCK_INI_MAX];

    mw_cfg_copy("BlockedNames", "", buf, sizeof(buf));
    if (strcmp(buf, g_raw) == 0)
        return 0;
    _snprintf(g_raw, sizeof(g_raw), "%s", buf);
    g_raw[sizeof(g_raw) - 1] = '\0';
    parse_into(buf);
    mw_log("block: %d name(s) loaded", g_used);
    return 1;
}

static void rebuild_view(void)
{
    if (mw_mirror_base())
        mw_mirror_rebuild(mw_tabs_mask());
}

/* Info, not the tab's first channel: Local looked like a player line, and a
 * later rebuild would drop it if someone blocked "Chat mod". Info is never
 * filtered by keep_message / append, and inject still paints it onto the
 * current tab until the next rebuild (the existing ephemeral contract). */
static void confirm(const char *text)
{
    if (!text || !text[0])
        return;
    mw_mirror_inject(CH_INFO, "Chat mod", text);
}

int mw_block_blocked(const unsigned char *rec)
{
    unsigned channel;

    if (!rec)
        return 0;
    channel = *(const unsigned *)(rec + MSG_CHANNEL);
    if (channel == CH_SYSTEM || channel == CH_INFO)
        return 0;
    return name_on_list((const char *)(rec + MSG_NAME));
}

int mw_block_add(const char *name)
{
    char one[MW_BLOCK_NAME_MAX + 1];
    const char *mine;
    int i;

    trim_copy(one, sizeof(one), name);
    if (!one[0] || strchr(one, ','))
        return MW_BLOCK_NOOP;
    if ((int)strlen(one) > MW_BLOCK_NAME_MAX)
        return MW_BLOCK_NOOP;

    mine = mw_mention_name();
    if (mine && mine[0] && _stricmp(one, mine) == 0)
        return MW_BLOCK_NOOP;

    for (i = 0; i < g_used; i++) {
        if (_stricmp(g_names[i], one) == 0)
            return MW_BLOCK_NOOP;
    }
    if (g_used >= MW_BLOCK_CAP)
        return MW_BLOCK_FULL;

    _snprintf(g_names[g_used], sizeof(g_names[0]), "%s", one);
    g_names[g_used][MW_BLOCK_NAME_MAX] = '\0';
    g_used++;
    persist();
    rebuild_view();
    mw_log("block: added \"%s\" (%d/%d)", one, g_used, MW_BLOCK_CAP);
    return MW_BLOCK_ADDED;
}

void mw_block_poll(void)
{
    if (reload())
        rebuild_view();
}

/* 1 = drop this append. cdecl args match the client's (channel, sender, name, text). */
static int __cdecl should_drop(int channel, unsigned sender,
                              const char *name, const char *text)
{
    (void)sender;
    (void)text;
    return (channel != CH_SYSTEM && channel != CH_INFO && name_on_list(name));
}

/* JMP'd into from the append prologue. Do not CALL the trampoline from C --
 * that would push a second frame and original `ret` would skip the real
 * caller. Check, then JMP to the relocated prologue or RET to the caller. */
static void __declspec(naked) append_gate(void)
{
    __asm {
        push dword ptr [esp+16]     /* text */
        push dword ptr [esp+16]     /* name */
        push dword ptr [esp+16]     /* sender */
        push dword ptr [esp+16]     /* channel */
        call should_drop
        add  esp, 16
        test eax, eax
        jnz  drop
        jmp  DWORD PTR [g_append_cont]
    drop:
        ret
    }
}

/* The chat window's HitTest is one of the twelve ring references the mirror
 * rewrites. After mw_mirror_install the LEA at +0xAA points at OUR copy, and
 * eax is a slot in that copy -- reading the real MsgArray would pick the
 * wrong name (or none). Fall back to the real ring only if the mirror is
 * not installed. */
static unsigned char *name_ring(void)
{
    unsigned char *m = mw_mirror_base();
    return m ? m : mw_addr[MW_MSGARRAY];
}

/* HitTest+0xAA: eax is slot*0xC0, name lives at ring+eax+8.
 * Return 1 to swallow the whisper, 0 to let the client build it. */
static int __cdecl whisper_ctrl(unsigned slot_bytes)
{
    unsigned char *ring;
    char           copied[MW_BLOCK_NAME_MAX + 1];
    int            rc;

    if (!(GetKeyState(VK_CONTROL) & 0x8000))
        return 0;
    ring = name_ring();
    if (!ring)
        return 1;
    if (slot_bytes >= 1024u * 0xC0u)
        return 1;

    /* Copy before add: mw_block_add rebuilds the mirror, which is where
     * this pointer lives. Confirming from it afterwards printed
     * "Blocked ." with a hole where the name had been. */
    trim_copy(copied, sizeof(copied),
              (const char *)(ring + slot_bytes + MSG_NAME));
    rc = mw_block_add(copied);
    if (rc == MW_BLOCK_ADDED) {
        char text[48];
        _snprintf(text, sizeof(text), "Blocked %s.", copied);
        text[sizeof(text) - 1] = '\0';
        confirm(text);
    } else if (rc == MW_BLOCK_FULL) {
        confirm("Block list is full.");
    }
    return 1;
}

static void __declspec(naked) whisper_gate(void)
{
    __asm {
        push eax
        push ecx
        push edx
        push eax
        call whisper_ctrl
        add  esp, 4
        test eax, eax
        pop  edx
        pop  ecx
        pop  eax
        jnz  swallow
        jmp  DWORD PTR [g_whisper_cont]
    swallow:
        pop  edi
        pop  esi
        pop  ebp
        pop  ebx
        ret  8
    }
}

static int hook_append(void)
{
    unsigned char *target = mw_addr[MW_APPEND];

    if (g_append_hooked)
        return 1;
    if (!target) {
        mw_log("block: append site missing; overlay drop disabled");
        return 0;
    }
    /* 7 bytes: mov ecx,[esp+4]; cmp ecx,5 -- next insn is a ja we must not
     * relocate. Signature pins these exact bytes. */
    if (!mw_hook_install(&g_append_hook, target, (void *)append_gate, 7)) {
        mw_log("block: could not hook append");
        return 0;
    }
    g_append_cont = g_append_hook.trampoline;
    g_append_hooked = 1;
    mw_log("block: append hooked at %p", target);
    return 1;
}

static int hook_whisper(void)
{
    unsigned char *hit = mw_addr[MW_HITTEST];
    unsigned char *site;
    DWORD          want, have;

    if (g_whisper_hooked)
        return 1;
    if (!hit || !mw_addr[MW_MSGARRAY])
        return 0;

    site = hit + WHISPER_SITE;
    if (!name_ring())
        return 0;
    want = (DWORD)(UINT_PTR)(name_ring() + MSG_NAME);
    have = *(DWORD *)(site + 2);
    /* 8D B0 disp32 = lea esi, [eax+ring+8]. After the mirror this is our
     * copy, not the real MsgArray -- that is why name_ring() exists. */
    if (site[0] != 0x8D || site[1] != 0xB0 || have != want) {
        mw_log("block: whisper site %p is not lea esi,[eax+name] "
               "(have %02X %02X disp 0x%08X, want 0x%08X)",
               site, site[0], site[1], have, want);
        return 0;
    }
    if (!mw_hook_install(&g_whisper_hook, site, (void *)whisper_gate, 6)) {
        mw_log("block: could not hook whisper site");
        return 0;
    }
    g_whisper_cont = g_whisper_hook.trampoline;
    g_whisper_hooked = 1;
    mw_log("block: Ctrl+click hooked at HitTest+%02X (%p)", WHISPER_SITE, site);
    return 1;
}

int mw_block_install(void)
{
    reload();
    hook_append();
    hook_whisper();
    if (g_used)
        rebuild_view();
    return 1;
}

void mw_block_remove(void)
{
    if (g_whisper_hooked) {
        mw_hook_remove(&g_whisper_hook);
        g_whisper_hooked = 0;
        g_whisper_cont = NULL;
    }
    if (g_append_hooked) {
        mw_hook_remove(&g_append_hook);
        g_append_hooked = 0;
        g_append_cont = NULL;
    }
}
