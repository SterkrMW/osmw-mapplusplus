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

/* Tell the core the bytes we just wrote are the ones we intend.
 *
 * A claim snapshots the bytes we are about to overwrite, so that snapshot is the
 * pre-patch original - compared against live memory it differs for every patch we
 * have applied, which is the normal state of this module.  Sealing supplies the
 * other half, and only sealed claims are ever reported as tampered.  Without this
 * call our own correct patches read as intrusions in the client attestation.
 *
 * Sealing everything we own rather than one token: this sits at the choke point
 * every write in the module goes through, which knows the address but not the
 * claim covering it.  A seal that is awkward to call is a seal that gets skipped,
 * and a write site added later is covered here without anyone remembering to.
 *
 * Advisory.  An old core has no seal_region and our regions simply go unwatched,
 * which is exactly where they were before this existed.
 */
static void lg_seal_claims(void)
{
    const OSMW_CORE_API_V1 *api = lg_core_api();

    if (!OSMW_STRUCT_HAS(api, OSMW_CORE_API_V1, seal_region) ||
            api->seal_region == NULL) {
        return;
    }
    (void)api->seal_region(lg_core_owner(), OSMW_INVALID_TOKEN);
}
static int write_bytes(void *dst, const void *src, int len)
{
    DWORD old, dummy;
    if (!VirtualProtect(dst, len, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    memcpy(dst, src, len);
    VirtualProtect(dst, len, old, &dummy);
    FlushInstructionCache(GetCurrentProcess(), dst, len);
    lg_seal_claims();
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

int lg_hook_install(lg_hook_t *h, void *target, void *detour, int prologue_len,
                    void **jmp_to, const char *label)
{
    const OSMW_CORE_API_V1 *api = lg_core_api();
    unsigned char *t = (unsigned char *)target;
    unsigned char *tramp;
    unsigned char  patch[LG_MAX_PROLOGUE];
    OSMW_REGION_CLAIM_V1 claim;
    OSMW_RESULT result;
    int i;

    memset(h, 0, sizeof(*h));

    if (!target || !detour || !label || !api || !api->claim_region ||
            !api->release_region || lg_core_owner() == OSMW_INVALID_OWNER) {
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

    ZeroMemory(&claim, sizeof(claim));
    claim.struct_size = sizeof(claim);
    claim.address = target;
    claim.length = (DWORD)prologue_len;
    claim.kind = OSMW_REGION_CODE;
    claim.label = label;
    result = api->claim_region(lg_core_owner(), &claim, &h->claim_token);
    if (result != OSMW_OK) {
        lg_log("hook: claim failed for %s at %p (%ld)",
               label, target, (long)result);
        h->trampoline = NULL;
        VirtualFree(tramp, 0, MEM_RELEASE);
        return 0;
    }

    if (jmp_to)
        *jmp_to = tramp;
    MemoryBarrier();

    if (!write_bytes(t, patch, prologue_len)) {
        if (jmp_to)
            *jmp_to = NULL;
        h->trampoline = NULL;
        VirtualFree(tramp, 0, MEM_RELEASE);
        result = api->release_region(lg_core_owner(), h->claim_token);
        if (result == OSMW_OK)
            h->claim_token = OSMW_INVALID_TOKEN;
        else
            lg_log("hook: claim release failed after write refusal (%ld)",
                   (long)result);
        lg_log("hook: VirtualProtect failed for %p", target);
        return 0;
    }

    h->active = 1;
    lg_log("hook: %p -> %p (trampoline %p, %d bytes)",
           target, detour, tramp, prologue_len);
    return 1;
}

OSMW_RESULT lg_hook_remove(lg_hook_t *h)
{
    const OSMW_CORE_API_V1 *api = lg_core_api();
    OSMW_RESULT result;

    if (!h)
        return OSMW_OK;
    if (h->active) {
        if (!write_bytes(h->target, h->saved, h->len)) {
            lg_log("hook: could not restore %p; claim retained", h->target);
            return OSMW_E_PATCH_STILL_ACTIVE;
        }
        h->active = 0;
        if (h->trampoline)
            VirtualFree(h->trampoline, 0, MEM_RELEASE);
        h->trampoline = NULL;
        lg_log("hook: removed %p", h->target);
    }
    if (h->claim_token == OSMW_INVALID_TOKEN)
        return OSMW_OK;
    if (!api || !api->release_region)
        return OSMW_E_UNAVAILABLE;
    result = api->release_region(lg_core_owner(), h->claim_token);
    if (result == OSMW_OK)
        h->claim_token = OSMW_INVALID_TOKEN;
    return result;
}
