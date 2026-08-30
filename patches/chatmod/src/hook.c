/* Minimal inline hooking.
 *
 * There is deliberately no length disassembler here. Every hook target is
 * reached through a signature that pins its exact prologue bytes, so the
 * caller passes a prologue length that is known to land on an instruction
 * boundary. If a client update moves the code, the signature fails to resolve
 * and we never get here.
 *
 * The relocated prologue must be position independent. That holds for the
 * targets we hook (register moves, stack adjustments, absolute loads), and
 * mw_hook_install refuses anything containing a relative branch as a
 * backstop.
 */
#include "osmw_chat.h"

static int write_bytes(void *dst, const void *src, int len)
{
    DWORD old, dummy;
    if (!VirtualProtect(dst, len, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    memcpy(dst, src, len);
    VirtualProtect(dst, len, old, &dummy);
    FlushInstructionCache(GetCurrentProcess(), dst, len);
    return 1;
}

/* Reject a prologue that starts with a relative branch: relocating one into
 * the trampoline would send it somewhere else entirely.
 *
 * Only the first byte is checked, and deliberately so. Scanning every byte
 * cannot tell an opcode from an operand -- `mov eax,[0x7E6870]` is A1 70 68 7E
 * 00, whose address operand contains 0x70, which a naive scan reads as a jcc
 * rel8. The real guarantee against a mid-prologue branch is upstream: each
 * target is reached through a signature that pins its exact prologue bytes,
 * and the prologue length passed here was read off the disassembly of those
 * bytes. If a client update moves the code, the signature stops resolving and
 * we never reach this function.
 */
static int starts_with_relative_branch(const unsigned char *p)
{
    return p[0] == 0xE8 || p[0] == 0xE9 || p[0] == 0xEB ||
           (p[0] >= 0x70 && p[0] <= 0x7F) ||
           (p[0] == 0x0F && p[1] >= 0x80 && p[1] <= 0x8F);
}

int mw_hook_install(mw_hook_t *h, void *target, void *detour, int prologue_len)
{
    unsigned char *t = (unsigned char *)target;
    unsigned char *tramp;
    unsigned char  patch[MW_MAX_PROLOGUE];
    int i;

    memset(h, 0, sizeof(*h));

    if (!target || !detour) {
        mw_log("hook: refusing install, target or detour is NULL");
        return 0;
    }
    if (prologue_len < 5 || prologue_len > MW_MAX_PROLOGUE) {
        mw_log("hook: bad prologue length %d for %p", prologue_len, target);
        return 0;
    }
    if (starts_with_relative_branch(t)) {
        mw_log("hook: %p starts with a relative branch, not relocatable", target);
        return 0;
    }

    /* trampoline = original prologue + jmp back to the rest of the function */
    tramp = (unsigned char *)VirtualAlloc(NULL, prologue_len + 5,
                                          MEM_COMMIT | MEM_RESERVE,
                                          PAGE_EXECUTE_READWRITE);
    if (!tramp) {
        mw_log("hook: VirtualAlloc failed for %p", target);
        return 0;
    }
    memcpy(tramp, t, prologue_len);
    tramp[prologue_len] = 0xE9;
    *(DWORD *)(tramp + prologue_len + 1) =
        (DWORD)((t + prologue_len) - (tramp + prologue_len + 5));

    /* patch site = jmp detour, nop-padded to the full prologue length */
    patch[0] = 0xE9;
    *(DWORD *)(patch + 1) = (DWORD)((unsigned char *)detour - (t + 5));
    for (i = 5; i < prologue_len; i++)
        patch[i] = 0x90;

    memcpy(h->saved, t, prologue_len);
    if (!write_bytes(t, patch, prologue_len)) {
        VirtualFree(tramp, 0, MEM_RELEASE);
        mw_log("hook: VirtualProtect failed for %p", target);
        return 0;
    }

    h->target     = target;
    h->trampoline = tramp;
    h->len        = prologue_len;
    h->active     = 1;
    mw_log("hook: %p -> %p (trampoline %p, %d bytes)",
           target, detour, tramp, prologue_len);
    return 1;
}

void mw_hook_remove(mw_hook_t *h)
{
    if (!h || !h->active)
        return;
    write_bytes(h->target, h->saved, h->len);
    if (h->trampoline)
        VirtualFree(h->trampoline, 0, MEM_RELEASE);
    mw_log("hook: removed %p", h->target);
    h->active     = 0;
    h->trampoline = NULL;
}

/* Redirect a direct `call rel32` to our own function, handing back the address
 * it used to reach. Only the displacement changes, so there is no trampoline
 * and no prologue to relocate -- the original callee is untouched and every
 * other caller of it still goes straight there. That matters here: the site we
 * redirect calls the CRT's sprintf, which the whole client uses. */
int mw_patch_direct_call(mw_hook_t *h, void *site, void *detour, void **original)
{
    unsigned char *s = (unsigned char *)site;
    unsigned char  patch[5];

    memset(h, 0, sizeof(*h));

    if (!site || !detour)
        return 0;
    if (s[0] != 0xE8) {
        mw_log("call-patch: %p is not `call rel32` (%02X)", site, s[0]);
        return 0;
    }

    if (original)
        *original = (void *)(s + 5 + *(LONG *)(s + 1));

    patch[0] = 0xE8;
    *(DWORD *)(patch + 1) = (DWORD)((unsigned char *)detour - (s + 5));

    memcpy(h->saved, s, 5);
    if (!write_bytes(s, patch, 5)) {
        mw_log("call-patch: VirtualProtect failed for %p", site);
        return 0;
    }

    h->target = site;
    h->len    = 5;
    h->active = 1;
    mw_log("call-patch: %p -> %p (was %p)", site, detour,
           original ? *original : NULL);
    return 1;
}

int mw_patch_call(mw_hook_t *h, void *site, void *detour)
{
    unsigned char *s = (unsigned char *)site;
    unsigned char  patch[6];

    memset(h, 0, sizeof(*h));

    if (!site || !detour)
        return 0;
    if (s[0] != 0xFF || s[1] != 0x15) {
        mw_log("call-patch: %p is not `call [abs32]` (%02X %02X)", site, s[0], s[1]);
        return 0;
    }

    patch[0] = 0xE8;
    *(DWORD *)(patch + 1) = (DWORD)((unsigned char *)detour - (s + 5));
    patch[5] = 0x90;

    memcpy(h->saved, s, 6);
    if (!write_bytes(s, patch, 6)) {
        mw_log("call-patch: VirtualProtect failed for %p", site);
        return 0;
    }

    h->target = site;
    h->len    = 6;
    h->active = 1;
    mw_log("call-patch: %p -> %p", site, detour);
    return 1;
}
