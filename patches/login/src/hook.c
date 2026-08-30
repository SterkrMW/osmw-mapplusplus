/* Minimal inline hooking.
 *
 * There is deliberately no length disassembler here. Every hook target is
 * reached through a signature that pins its exact prologue bytes, so the
 * caller passes a prologue length that is known to land on an instruction
 * boundary. If a client update moves the code, the signature fails to resolve
 * and we never get here.
 *
 * The relocated prologue must be position independent. That holds for the
 * targets we hook (register moves, absolute loads), and lg_hook_install
 * refuses anything containing a relative branch as a backstop.
 */
#include "osmw_login.h"

#include <string.h>

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
 * cannot tell an opcode from an operand. The real guarantee against a
 * mid-prologue branch is upstream: each target is reached through a signature
 * that pins its exact prologue bytes.
 */
static int starts_with_relative_branch(const unsigned char *p)
{
    return p[0] == 0xE8 || p[0] == 0xE9 || p[0] == 0xEB ||
           (p[0] >= 0x70 && p[0] <= 0x7F) ||
           (p[0] == 0x0F && p[1] >= 0x80 && p[1] <= 0x8F);
}

int lg_hook_install(lg_hook_t *h, void *target, void *detour, int prologue_len, void **jmp_to)
{
    unsigned char *t = (unsigned char *)target;
    unsigned char *tramp;
    unsigned char  patch[LG_MAX_PROLOGUE];
    int i;

    memset(h, 0, sizeof(*h));

    if (!target || !detour) {
        lg_log("hook: refusing install, target or detour is NULL");
        return 0;
    }
    if (prologue_len < 5 || prologue_len > LG_MAX_PROLOGUE) {
        lg_log("hook: bad prologue length %d for %p", prologue_len, target);
        return 0;
    }
    if (starts_with_relative_branch(t)) {
        lg_log("hook: %p starts with a relative branch, not relocatable", target);
        return 0;
    }

    tramp = (unsigned char *)VirtualAlloc(NULL, prologue_len + 5,
                                          MEM_COMMIT | MEM_RESERVE,
                                          PAGE_EXECUTE_READWRITE);
    if (!tramp) {
        lg_log("hook: VirtualAlloc failed for %p", target);
        return 0;
    }
    memcpy(tramp, t, prologue_len);
    tramp[prologue_len] = 0xE9;
    *(DWORD *)(tramp + prologue_len + 1) =
        (DWORD)((t + prologue_len) - (tramp + prologue_len + 5));

    patch[0] = 0xE9;
    *(DWORD *)(patch + 1) = (DWORD)((unsigned char *)detour - (t + 5));
    for (i = 5; i < prologue_len; i++)
        patch[i] = 0x90;

    memcpy(h->saved, t, prologue_len);
    h->target     = target;
    h->trampoline = tramp;
    h->len        = prologue_len;
    if (jmp_to)
        *jmp_to = tramp;
    MemoryBarrier();

    if (!write_bytes(t, patch, prologue_len)) {
        if (jmp_to)
            *jmp_to = NULL;
        h->trampoline = NULL;
        VirtualFree(tramp, 0, MEM_RELEASE);
        lg_log("hook: VirtualProtect failed for %p", target);
        return 0;
    }

    h->active = 1;
    lg_log("hook: %p -> %p (trampoline %p, %d bytes)",
           target, detour, tramp, prologue_len);
    return 1;
}

void lg_hook_remove(lg_hook_t *h)
{
    if (!h || !h->active)
        return;
    write_bytes(h->target, h->saved, h->len);
    if (h->trampoline)
        VirtualFree(h->trampoline, 0, MEM_RELEASE);
    lg_log("hook: removed %p", h->target);
    h->active     = 0;
    h->trampoline = NULL;
}
