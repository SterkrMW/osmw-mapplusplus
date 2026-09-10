/* Claimed raw patch transactions with undo records.
 *
 * Weather redirects verified direct call sites and leaves the callees intact.
 * Core arbitrates each exact range; this module retains the thread freeze/EIP
 * exclusion needed to make each multi-byte rewrite and restoration safe.
 */
#include "osmw_weather.h"

#include <tlhelp32.h>

#include <string.h>


/* Freeze every other thread in this process across a code write.
 *
 * A multi-byte patch is NOT atomic. FrameSync is 17 bytes and the client calls
 * it about thirty times a second - at the login screen, constantly - so the
 * main thread can be executing inside the very bytes being replaced and run on
 * into a half-written instruction stream. That is timing dependent, which is
 * why builds carrying an identical patch survived for a while and then died at
 * login with nothing in the log after "startup complete".
 *
 * Suspending is safe here because the write path takes no lock another thread
 * could be holding: VirtualProtect, memcpy, FlushInstructionCache. Logging is
 * deliberately kept OUTSIDE the frozen window - wx_log takes a critical
 * section, and taking one while other threads are suspended is how deadlocks
 * get written.
 */
#define WX_MAX_FROZEN 32
static HANDLE g_frozen[WX_MAX_FROZEN];
static int    g_frozen_count;

static void freeze_others(void)
{
    HANDLE        snap;
    THREADENTRY32 te;
    DWORD         pid = GetCurrentProcessId(), self = GetCurrentThreadId();

    g_frozen_count = 0;
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return;
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            HANDLE h;
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == self)
                continue;
            if (g_frozen_count >= WX_MAX_FROZEN)
                break;
            /* THREAD_GET_CONTEXT as well as SUSPEND_RESUME. Without it
             * GetThreadContext fails with ACCESS_DENIED, anyone_inside cannot
             * tell where the thread is, and the conservative answer ("assume
             * it is inside") turns every patch into 200 futile retries and a
             * refusal. That is exactly what happened on a ONE BYTE patch. */
            h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT,
                           FALSE, te.th32ThreadID);
            if (!h)
                continue;
            if (SuspendThread(h) == (DWORD)-1) {
                CloseHandle(h);
                continue;
            }
            g_frozen[g_frozen_count++] = h;
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

/* Is any frozen thread parked INSIDE the bytes we are about to replace?
 *
 * Freezing alone is not enough. Suspending a thread whose EIP already sits in
 * the middle of the patch region does not help: it resumes into a modified
 * instruction stream and dies exactly as if we had written underneath it. So
 * the region has to be clear as well as quiet, and if it is not, the only
 * correct move is to let everyone run for a moment and try again. */
static DWORD g_ctx_error;   /* reported after thawing, never inside the freeze */

static int anyone_inside(void *at, int len)
{
    unsigned char *lo = (unsigned char *)at, *hi = lo + len;
    CONTEXT ctx;
    int i;

    for (i = 0; i < g_frozen_count; i++) {
        memset(&ctx, 0, sizeof(ctx));
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (!GetThreadContext(g_frozen[i], &ctx)) {
            /* Still the conservative answer, but record WHY - a silent
             * "assume the worst" here cost a full test round when the real
             * problem was a missing access right on OpenThread. */
            g_ctx_error = GetLastError();
            return 1;
        }
        if ((unsigned char *)ctx.Eip >= lo && (unsigned char *)ctx.Eip < hi)
            return 1;
    }
    return 0;
}

static void thaw_others(void)
{
    while (g_frozen_count > 0) {
        HANDLE h = g_frozen[--g_frozen_count];
        ResumeThread(h);
        CloseHandle(h);
    }
}

static OSMW_RESULT claim_region(
    void *address, DWORD length, OSMW_REGION_KIND kind,
    const char *label, OSMW_TOKEN *token)
{
    const OSMW_CORE_API_V1 *api = wx_core_api();
    OSMW_REGION_CLAIM_V1 claim;
    if (api == NULL || api->claim_region == NULL)
        return OSMW_E_UNAVAILABLE;
    ZeroMemory(&claim, sizeof(claim));
    claim.struct_size = sizeof(claim);
    claim.address = address;
    claim.length = length;
    claim.kind = kind;
    claim.label = label;
    return api->claim_region(wx_core_owner(), &claim, token);
}

static OSMW_RESULT release_token(OSMW_TOKEN *token)
{
    const OSMW_CORE_API_V1 *api = wx_core_api();
    OSMW_RESULT result;
    if (token == NULL || *token == OSMW_INVALID_TOKEN)
        return OSMW_OK;
    if (api == NULL || api->release_region == NULL)
        return OSMW_E_UNAVAILABLE;
    result = api->release_region(wx_core_owner(), *token);
    if (result == OSMW_OK)
        *token = OSMW_INVALID_TOKEN;
    return result;
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
static void wx_seal_claims(void)
{
    const OSMW_CORE_API_V1 *api = wx_core_api();

    if (!OSMW_STRUCT_HAS(api, OSMW_CORE_API_V1, seal_region) ||
            api->seal_region == NULL) {
        return;
    }
    (void)api->seal_region(wx_core_owner(), OSMW_INVALID_TOKEN);
}
int wx_patch_write(wx_patch_t *p, void *at, const void *bytes, int len,
                   OSMW_REGION_KIND kind, const char *label)
{
    DWORD old, dummy;
    OSMW_RESULT result;

    if (!p)
        return 0;
    memset(p, 0, sizeof(*p));

    if (!at || !bytes || !label || len <= 0 || len > WX_MAX_PATCH) {
        wx_log("patch: refusing %p, bad length %d", at, len);
        return 0;
    }

    p->at = at;
    p->len = len;
    result = claim_region(at, (DWORD)len, kind, label, &p->claim_token);
    if (result != OSMW_OK) {
        wx_log("patch: claim failed for %s at %p (%ld)",
               label, at, (long)result);
        return 0;
    }

    if (!VirtualProtect(at, (SIZE_T)len, PAGE_EXECUTE_READWRITE, &old)) {
        wx_log("patch: VirtualProtect failed for %p (%lu)", at, GetLastError());
        (void)release_token(&p->claim_token);
        return 0;
    }
    {
        int tries = 0;
        for (;;) {
            freeze_others();
            if (!anyone_inside(at, len))
                break;
            thaw_others();
            if (++tries >= 100) {
                VirtualProtect(at, (SIZE_T)len, old, &dummy);
                if (g_ctx_error)
                    wx_log("patch: %p - could not read a thread context "
                           "(error %lu), so the region could not be proven "
                           "clear; refusing to write", at, g_ctx_error);
                else
                    wx_log("patch: %p is busy - a thread stayed inside the "
                           "patch region for 100 tries, refusing to write", at);
                (void)release_token(&p->claim_token);
                return 0;
            }
            Sleep(1);
        }
    }
    memcpy(p->saved, at, (size_t)len);
    memcpy(at, bytes, (size_t)len);
    FlushInstructionCache(GetCurrentProcess(), at, (SIZE_T)len);
    thaw_others();
    VirtualProtect(at, (SIZE_T)len, old, &dummy);

    p->active = 1;
    wx_seal_claims();
    wx_log("patch: %p, %d byte(s)", at, len);
    return 1;
}

OSMW_RESULT wx_patch_undo(wx_patch_t *p)
{
    DWORD old, dummy;
    OSMW_RESULT result;

    if (!p)
        return OSMW_OK;
    if (p->active && VirtualProtect(
            p->at, (SIZE_T)p->len, PAGE_EXECUTE_READWRITE, &old)) {
        int tries = 0;
        for (;;) {
            freeze_others();
            if (!anyone_inside(p->at, p->len))
                break;
            thaw_others();
            if (++tries >= 100)
                break;      /* restore anyway on the way out; see below */
            Sleep(1);
        }
        memcpy(p->at, p->saved, (size_t)p->len);
        FlushInstructionCache(GetCurrentProcess(), p->at, (SIZE_T)p->len);
        thaw_others();
        VirtualProtect(p->at, (SIZE_T)p->len, old, &dummy);
        /* Re-seal on the original bytes.  The release below usually clears the
         * claim outright, but a release that fails leaves it standing, and a seal
         * still describing the removed patch would report the restored original
         * as an intrusion for the rest of the session. */
        wx_seal_claims();
        wx_log("patch: restored %p", p->at);
        p->active = 0;
    } else if (p->active) {
        wx_log("patch: could NOT restore %p (%lu)", p->at, GetLastError());
        return OSMW_E_PATCH_STILL_ACTIVE;
    }
    result = release_token(&p->claim_token);
    if (result != OSMW_OK)
        wx_log("patch: restored %p but claim release failed (%ld)",
               p->at, (long)result);
    return result;
}


/* Repoint one direct `call rel32`. Only the displacement changes. */
int wx_patch_call_site(wx_patch_t *p, void *site, void *detour,
                       const char *label)
{
    unsigned char *s = (unsigned char *)site;
    unsigned char  patch[5];

    if (!site || !detour)
        return 0;
    if (s[0] != 0xE8) {
        wx_log("call-patch: %p is not `call rel32` (%02X)", site, s[0]);
        return 0;
    }
    patch[0] = 0xE8;
    *(DWORD *)(patch + 1) = (DWORD)((unsigned char *)detour - (s + 5));
    return wx_patch_write(p, site, patch, 5,
                          OSMW_REGION_CALL_SITE, label);
}


/* ---- import table hooking ------------------------------------------------ */

#define WX_MAX_IAT 4

static struct {
    void **slot;
    void  *orig;
    OSMW_TOKEN claim_token;
} g_iat[WX_MAX_IAT];
static int g_iat_count;

int wx_iat_hook(const char *dll, const char *func, void *detour, void **orig,
                const char *label)
{
    unsigned char           *base = (unsigned char *)GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER        *dos  = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS32      *nt;
    IMAGE_IMPORT_DESCRIPTOR *imp;
    DWORD                    rva;

    if (g_iat_count >= WX_MAX_IAT || !base)
        return 0;
    nt = (IMAGE_NT_HEADERS32 *)(base + dos->e_lfanew);
    rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!rva)
        return 0;

    for (imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + rva); imp->Name; imp++) {
        const char           *name = (const char *)(base + imp->Name);
        IMAGE_THUNK_DATA32   *orig_thunk, *iat;

        if (_stricmp(name, dll) != 0)
            continue;
        /* OriginalFirstThunk can legitimately be zero (bound imports). Walking
         * from base + 0 would read the DOS header as a thunk array and could
         * write a detour into an unrelated import slot - a corruption that
         * would surface as a crash somewhere else entirely. This image has it
         * populated for every DLL, but the fallback costs one line. */
        orig_thunk = (IMAGE_THUNK_DATA32 *)(base + (imp->OriginalFirstThunk
                                                    ? imp->OriginalFirstThunk
                                                    : imp->FirstThunk));
        iat        = (IMAGE_THUNK_DATA32 *)(base + imp->FirstThunk);

        for (; orig_thunk->u1.AddressOfData; orig_thunk++, iat++) {
            IMAGE_IMPORT_BY_NAME *ibn;
            DWORD old, dummy;

            if (orig_thunk->u1.Ordinal & IMAGE_ORDINAL_FLAG32)
                continue;
            ibn = (IMAGE_IMPORT_BY_NAME *)(base + orig_thunk->u1.AddressOfData);
            if (strcmp((const char *)ibn->Name, func) != 0)
                continue;

            {
                OSMW_RESULT result = claim_region(
                    &iat->u1.Function, sizeof(void *), OSMW_REGION_IAT,
                    label, &g_iat[g_iat_count].claim_token);
                if (result != OSMW_OK) {
                    wx_log("iat: claim failed for %s!%s (%ld)",
                           dll, func, (long)result);
                    return 0;
                }
            }

            if (!VirtualProtect(&iat->u1.Function, sizeof(void *),
                                PAGE_READWRITE, &old)) {
                wx_log("iat: VirtualProtect failed for %s!%s", dll, func);
                (void)release_token(&g_iat[g_iat_count].claim_token);
                return 0;
            }
            if (orig)
                *orig = (void *)iat->u1.Function;
            g_iat[g_iat_count].slot = (void **)&iat->u1.Function;
            g_iat[g_iat_count].orig = (void *)iat->u1.Function;
            g_iat_count++;
            iat->u1.Function = (DWORD)(ULONG_PTR)detour;
            VirtualProtect(&iat->u1.Function, sizeof(void *), old, &dummy);
            wx_log("iat: %s!%s %p -> %p", dll, func, orig ? *orig : NULL, detour);
            return 1;
        }
    }
    wx_log("iat: %s!%s not imported", dll, func);
    return 0;
}

OSMW_RESULT wx_iat_unhook(void)
{
    DWORD old, dummy;
    OSMW_RESULT result;
    while (g_iat_count > 0) {
        int index = g_iat_count - 1;
        if (VirtualProtect(g_iat[index].slot, sizeof(void *),
                           PAGE_READWRITE, &old)) {
            *g_iat[index].slot = g_iat[index].orig;
            VirtualProtect(g_iat[index].slot, sizeof(void *), old, &dummy);
        } else
            return OSMW_E_PATCH_STILL_ACTIVE;
        result = release_token(&g_iat[index].claim_token);
        if (result != OSMW_OK)
            return result;
        --g_iat_count;
    }
    return OSMW_OK;
}
