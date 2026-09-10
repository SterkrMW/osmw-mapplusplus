/* Chat-owned patch helpers with exact core region claims. */
#include "osmw_chat.h"

static volatile LONG g_claim_count;
static volatile LONG g_last_release_failure;

OSMW_RESULT mw_claim_region(void *address, DWORD length,
                            OSMW_REGION_KIND kind, const char *label,
                            OSMW_TOKEN *token)
{
    const OSMW_CORE_API_V1 *api = mw_core_api();
    OSMW_REGION_CLAIM_V1 claim;

    if (api == NULL || api->claim_region == NULL || token == NULL)
        return OSMW_E_UNAVAILABLE;
    ZeroMemory(&claim, sizeof(claim));
    claim.struct_size = sizeof(claim);
    claim.address = address;
    claim.length = length;
    claim.kind = kind;
    claim.label = label;
    {
        OSMW_RESULT result = api->claim_region(
            mw_core_owner(), &claim, token);
        if (result == OSMW_OK)
            InterlockedIncrement(&g_claim_count);
        return result;
    }
}

OSMW_RESULT mw_release_region(OSMW_TOKEN *token)
{
    const OSMW_CORE_API_V1 *api = mw_core_api();
    OSMW_RESULT result;

    if (token == NULL)
        return OSMW_E_INVALID_ARGUMENT;
    if (*token == OSMW_INVALID_TOKEN)
        return OSMW_OK;
    if (api == NULL || api->release_region == NULL)
        return OSMW_E_UNAVAILABLE;
    result = api->release_region(mw_core_owner(), *token);
    if (result == OSMW_OK) {
        *token = OSMW_INVALID_TOKEN;
        InterlockedDecrement(&g_claim_count);
    } else {
        InterlockedExchange(&g_last_release_failure, result);
    }
    return result;
}

DWORD mw_claim_count(void)
{
    return (DWORD)InterlockedCompareExchange(&g_claim_count, 0, 0);
}

OSMW_RESULT mw_claim_cleanup_result(void)
{
    LONG count = InterlockedCompareExchange(&g_claim_count, 0, 0);
    LONG failure = InterlockedCompareExchange(&g_last_release_failure, 0, 0);
    if (count != 0)
        return failure != OSMW_OK ? (OSMW_RESULT)failure : OSMW_E_INTERNAL;
    InterlockedExchange(&g_last_release_failure, OSMW_OK);
    return OSMW_OK;
}

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
static void mw_seal_claims(void)
{
    const OSMW_CORE_API_V1 *api = mw_core_api();

    if (!OSMW_STRUCT_HAS(api, OSMW_CORE_API_V1, seal_region) ||
            api->seal_region == NULL) {
        return;
    }
    (void)api->seal_region(mw_core_owner(), OSMW_INVALID_TOKEN);
}
int mw_write_bytes(void *dst, const void *src, DWORD len)
{
    DWORD old, dummy;
    if (dst == NULL || src == NULL || len == 0)
        return 0;
    if (!VirtualProtect(dst, len, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    memcpy(dst, src, len);
    if (!VirtualProtect(dst, len, old, &dummy))
        return 0;
    FlushInstructionCache(GetCurrentProcess(), dst, len);
    mw_seal_claims();
    return 1;
}

/* A generated signature pins the complete instruction sequence, so only the
 * first opcode needs the relocation backstop used by the legacy hooker. */
static int starts_with_relative_branch(const unsigned char *p)
{
    return p[0] == 0xE8 || p[0] == 0xE9 || p[0] == 0xEB ||
           (p[0] >= 0x70 && p[0] <= 0x7F) ||
           (p[0] == 0x0F && p[1] >= 0x80 && p[1] <= 0x8F);
}

static void abandon_unwritten_hook(mw_hook_t *h)
{
    OSMW_RESULT result = mw_release_region(&h->claim);
    if (result != OSMW_OK)
        mw_log("hook: could not release unwritten claim for %p (%ld)",
               h->target, (long)result);
    if (h->trampoline != NULL) {
        VirtualFree(h->trampoline, 0, MEM_RELEASE);
        h->trampoline = NULL;
    }
}

int mw_hook_install(mw_hook_t *h, void *target, void *detour,
                    int prologue_len, const char *label)
{
    unsigned char *t = (unsigned char *)target;
    unsigned char *tramp;
    unsigned char patch[MW_MAX_PROLOGUE];
    OSMW_RESULT result;
    int i;

    if (h == NULL)
        return 0;
    ZeroMemory(h, sizeof(*h));
    if (!target || !detour || !label) {
        mw_log("hook: refusing install, target, detour or label is NULL");
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

    result = mw_claim_region(target, (DWORD)prologue_len, OSMW_REGION_CODE,
                             label, &h->claim);
    if (result != OSMW_OK) {
        mw_log("hook: claim refused for %s at %p (%ld)",
               label, target, (long)result);
        return 0;
    }
    h->target = target;
    h->len = prologue_len;
    memcpy(h->saved, t, (size_t)prologue_len);

    tramp = (unsigned char *)VirtualAlloc(NULL, (SIZE_T)prologue_len + 5,
                                          MEM_COMMIT | MEM_RESERVE,
                                          PAGE_EXECUTE_READWRITE);
    if (!tramp) {
        mw_log("hook: VirtualAlloc failed for %p", target);
        abandon_unwritten_hook(h);
        return 0;
    }
    h->trampoline = tramp;
    memcpy(tramp, t, (size_t)prologue_len);
    tramp[prologue_len] = 0xE9;
    *(DWORD *)(tramp + prologue_len + 1) =
        (DWORD)((t + prologue_len) - (tramp + prologue_len + 5));

    patch[0] = 0xE9;
    *(DWORD *)(patch + 1) = (DWORD)((unsigned char *)detour - (t + 5));
    for (i = 5; i < prologue_len; ++i)
        patch[i] = 0x90;
    if (!mw_write_bytes(t, patch, (DWORD)prologue_len)) {
        mw_log("hook: write failed for %p", target);
        abandon_unwritten_hook(h);
        return 0;
    }

    h->active = 1;
    mw_log("hook: %s %p -> %p (trampoline %p, %d bytes)",
           label, target, detour, tramp, prologue_len);
    return 1;
}

OSMW_RESULT mw_hook_remove(mw_hook_t *h)
{
    OSMW_RESULT result;

    if (h == NULL)
        return OSMW_E_INVALID_ARGUMENT;
    if (!h->active && h->claim == OSMW_INVALID_TOKEN)
        return OSMW_OK;
    if (h->active && !mw_write_bytes(h->target, h->saved, (DWORD)h->len)) {
        mw_log("hook: restore failed for %p", h->target);
        return OSMW_E_INTERNAL;
    }
    result = mw_release_region(&h->claim);
    if (result != OSMW_OK) {
        mw_log("hook: release failed for %p (%ld)", h->target, (long)result);
        return result;
    }
    if (h->trampoline != NULL)
        VirtualFree(h->trampoline, 0, MEM_RELEASE);
    mw_log("hook: removed %p", h->target);
    ZeroMemory(h, sizeof(*h));
    return OSMW_OK;
}

int mw_patch_direct_call(mw_hook_t *h, void *site, void *detour,
                         void **original, const char *label)
{
    unsigned char *s = (unsigned char *)site;
    unsigned char patch[5];
    OSMW_RESULT result;

    if (h == NULL)
        return 0;
    ZeroMemory(h, sizeof(*h));
    if (!site || !detour || !label)
        return 0;
    if (s[0] != 0xE8) {
        mw_log("call-patch: %p is not `call rel32` (%02X)", site, s[0]);
        return 0;
    }
    result = mw_claim_region(site, 5, OSMW_REGION_CALL_SITE, label, &h->claim);
    if (result != OSMW_OK) {
        mw_log("call-patch: claim refused for %s at %p (%ld)",
               label, site, (long)result);
        return 0;
    }
    h->target = site;
    h->len = 5;
    memcpy(h->saved, s, 5);
    if (original)
        *original = (void *)(s + 5 + *(LONG *)(s + 1));
    patch[0] = 0xE8;
    *(DWORD *)(patch + 1) = (DWORD)((unsigned char *)detour - (s + 5));
    if (!mw_write_bytes(s, patch, 5)) {
        mw_log("call-patch: write failed for %p", site);
        abandon_unwritten_hook(h);
        return 0;
    }
    h->active = 1;
    mw_log("call-patch: %s %p -> %p (was %p)", label, site, detour,
           original ? *original : NULL);
    return 1;
}

int mw_patch_call(mw_hook_t *h, void *site, void *detour,
                  const char *label)
{
    unsigned char *s = (unsigned char *)site;
    unsigned char patch[6];
    OSMW_RESULT result;

    if (h == NULL)
        return 0;
    ZeroMemory(h, sizeof(*h));
    if (!site || !detour || !label)
        return 0;
    if (s[0] != 0xFF || s[1] != 0x15) {
        mw_log("call-patch: %p is not `call [abs32]` (%02X %02X)",
               site, s[0], s[1]);
        return 0;
    }
    result = mw_claim_region(site, 6, OSMW_REGION_CALL_SITE, label, &h->claim);
    if (result != OSMW_OK) {
        mw_log("call-patch: claim refused for %s at %p (%ld)",
               label, site, (long)result);
        return 0;
    }
    h->target = site;
    h->len = 6;
    memcpy(h->saved, s, 6);
    patch[0] = 0xE8;
    *(DWORD *)(patch + 1) = (DWORD)((unsigned char *)detour - (s + 5));
    patch[5] = 0x90;
    if (!mw_write_bytes(s, patch, 6)) {
        mw_log("call-patch: write failed for %p", site);
        abandon_unwritten_hook(h);
        return 0;
    }
    h->active = 1;
    mw_log("call-patch: %s %p -> %p", label, site, detour);
    return 1;
}
