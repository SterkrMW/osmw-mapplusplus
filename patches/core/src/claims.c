/* Exclusive ownership registry for live process-memory ranges.
 *
 * Claims do not write memory.  They snapshot the bytes a module is about to
 * change and require those exact bytes to be restored before ownership can be
 * released.  This coordinates trusted managed modules; it is not a security
 * boundary against unrelated code in the process.
 *
 * A record therefore carries up to two pictures of the same bytes:
 *
 *   snapshot - the ORIGINAL, from before the owner wrote anything.  Restoring
 *              these exactly is the price of releasing the claim.  Never
 *              rewritten, or release_region would start accepting a region
 *              left in whatever state it happened to be in.
 *   sealed   - what the owner says the bytes SHOULD look like right now, set
 *              by seal_region after the owner writes.  NULL until the owner
 *              seals, which is the honest default: with no seal there is no
 *              way to tell the owner own patch from somebody else write.
 *
 * Conflating the two is a mistake that already shipped.  Comparing live bytes
 * against snapshot reports a difference for every patch that is applied, which
 * is the healthy state of a working mod - the first client to report that count
 * was accused of 92 intrusions, all 92 of them its own modules doing their job.
 * Only sealed records are ever counted as tampered.
 */
#include "core_internal.h"

#include <stdio.h>
#include <string.h>

#define OSMW_CLAIM_CAP 256u
#define OSMW_CLAIM_LABEL_CAP 64u

typedef struct OSMW_CLAIM_RECORD {
    OSMW_TOKEN token;
    OSMW_OWNER owner;
    BYTE *address;
    DWORD length;
    OSMW_REGION_KIND kind;
    BYTE *snapshot;
    /* Owner-declared expected bytes, or NULL if this claim has never been
     * sealed.  An unsealed claim is not monitored rather than presumed guilty. */
    BYTE *sealed;
    char label[OSMW_CLAIM_LABEL_CAP];
} OSMW_CLAIM_RECORD;

static BOOL g_claims_ready;
static CRITICAL_SECTION g_claim_lock;
static OSMW_CLAIM_RECORD g_claims[OSMW_CLAIM_CAP];
static DWORD g_claim_count;
static OSMW_TOKEN g_next_claim_token;

static BOOL copy_label(const char *source, char *destination)
{
    DWORD length;

    if (source == NULL || source[0] == '\0') {
        return FALSE;
    }
    for (length = 0; length < OSMW_CLAIM_LABEL_CAP; ++length) {
        if (source[length] == '\0') {
            memcpy(destination, source, length + 1);
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL valid_kind(OSMW_REGION_KIND kind)
{
    return kind == OSMW_REGION_CODE || kind == OSMW_REGION_CALL_SITE ||
        kind == OSMW_REGION_IAT || kind == OSMW_REGION_DATA;
}

static BOOL valid_range(void *address, DWORD length)
{
    ULONG_PTR start = (ULONG_PTR)address;
    ULONG_PTR maximum = ~(ULONG_PTR)0;

    return start != 0 && length != 0 &&
        (ULONG_PTR)(length - 1u) <= maximum - start;
}

static BOOL ranges_overlap(
    const BYTE *left, DWORD left_length,
    const BYTE *right, DWORD right_length)
{
    ULONG_PTR left_start = (ULONG_PTR)left;
    ULONG_PTR right_start = (ULONG_PTR)right;
    ULONG_PTR left_end = left_start + left_length;
    ULONG_PTR right_end = right_start + right_length;

    return left_start < right_end && right_start < left_end;
}

static OSMW_TOKEN next_token(void)
{
    do {
        ++g_next_claim_token;
    } while (g_next_claim_token == OSMW_INVALID_TOKEN);
    return g_next_claim_token;
}

BOOL osmw_claims_init(void)
{
    if (g_claims_ready) {
        return TRUE;
    }
    ZeroMemory(g_claims, sizeof(g_claims));
    g_claim_count = 0;
    g_next_claim_token = OSMW_INVALID_TOKEN;
    InitializeCriticalSection(&g_claim_lock);
    g_claims_ready = TRUE;
    return TRUE;
}

void osmw_claims_reset(void)
{
    DWORD index;

    if (!g_claims_ready) {
        return;
    }
    EnterCriticalSection(&g_claim_lock);
    for (index = 0; index < OSMW_CLAIM_CAP; ++index) {
        if (g_claims[index].snapshot != NULL) {
            HeapFree(GetProcessHeap(), 0, g_claims[index].snapshot);
        }
        if (g_claims[index].sealed != NULL) {
            HeapFree(GetProcessHeap(), 0, g_claims[index].sealed);
        }
    }
    ZeroMemory(g_claims, sizeof(g_claims));
    g_claim_count = 0;
    LeaveCriticalSection(&g_claim_lock);
    DeleteCriticalSection(&g_claim_lock);
    g_claims_ready = FALSE;
    g_next_claim_token = OSMW_INVALID_TOKEN;
}

DWORD osmw_claims_owner_count(OSMW_OWNER owner)
{
    DWORD index;
    DWORD count = 0;

    if (!g_claims_ready || owner == OSMW_INVALID_OWNER) {
        return 0;
    }
    EnterCriticalSection(&g_claim_lock);
    for (index = 0; index < OSMW_CLAIM_CAP; ++index) {
        if (g_claims[index].token != OSMW_INVALID_TOKEN &&
                g_claims[index].owner == owner) {
            ++count;
        }
    }
    LeaveCriticalSection(&g_claim_lock);
    return count;
}

DWORD osmw_claims_total_count(void)
{
    DWORD count;

    if (!g_claims_ready) {
        return 0;
    }
    EnterCriticalSection(&g_claim_lock);
    count = g_claim_count;
    LeaveCriticalSection(&g_claim_lock);
    return count;
}

void osmw_claims_log_owner(OSMW_OWNER owner)
{
    DWORD index;

    if (!g_claims_ready || owner == OSMW_INVALID_OWNER) {
        return;
    }
    EnterCriticalSection(&g_claim_lock);
    for (index = 0; index < OSMW_CLAIM_CAP; ++index) {
        OSMW_CLAIM_RECORD *record = &g_claims[index];
        char message[256];
        int written;
        if (record->token == OSMW_INVALID_TOKEN || record->owner != owner) {
            continue;
        }
        written = _snprintf(
            message, sizeof(message),
            "claim still active: %s [%p,+%lu], kind %lu, token %lu",
            record->label, record->address, (unsigned long)record->length,
            (unsigned long)record->kind, (unsigned long)record->token);
        if (written < 0 || (DWORD)written >= sizeof(message)) {
            message[sizeof(message) - 1] = '\0';
        }
        osmw_api_log(owner, OSMW_LOG_ERROR, message);
    }
    LeaveCriticalSection(&g_claim_lock);
}

/* Live claims whose bytes no longer match what was there when the claim was granted.
 *
 * A managed module restores its own bytes before releasing, so a mismatch here means
 * something outside the managed set wrote into a range a module owns.  Read-only and
 * bounded by the claim table, so it is safe to call from a UiTick callback.
 *
 * ReadProcessMemory rather than a direct compare: a claimed range can be unmapped by a
 * third party, and a fault while counting would take the client down over telemetry. */
DWORD osmw_claims_tampered_count(void)
{
    DWORD index;
    DWORD tampered = 0;

    if (!g_claims_ready) {
        return 0;
    }
    EnterCriticalSection(&g_claim_lock);
    for (index = 0; index < OSMW_CLAIM_CAP; ++index) {
        OSMW_CLAIM_RECORD *record = &g_claims[index];
        BYTE current[64];
        DWORD remaining;
        DWORD offset = 0;
        BOOL differs = FALSE;

        /* Unsealed claims are skipped, not compared.  Their snapshot is the
         * pre-patch original, so comparing it live would report every applied
         * patch as an intrusion. */
        if (record->token == OSMW_INVALID_TOKEN || record->sealed == NULL ||
                record->address == NULL || record->length == 0) {
            continue;
        }
        remaining = record->length;
        while (remaining > 0 && !differs) {
            SIZE_T read = 0;
            DWORD chunk = remaining < sizeof(current) ? remaining : (DWORD)sizeof(current);

            if (!ReadProcessMemory(GetCurrentProcess(), record->address + offset,
                                   current, chunk, &read) ||
                    read != chunk) {
                /* Unreadable is not the same as altered.  Counting it would turn an
                 * unmapped page into an accusation, so it is skipped. */
                differs = FALSE;
                break;
            }
            if (memcmp(current, record->sealed + offset, chunk) != 0) {
                differs = TRUE;
            }
            offset += chunk;
            remaining -= chunk;
        }
        if (differs) {
            ++tampered;
        }
    }
    LeaveCriticalSection(&g_claim_lock);
    return tampered;
}

/* How many live claims carry a seal.  Only useful for telling "nothing was
 * written" apart from "nothing is being watched", which otherwise both report
 * zero tampered regions and look identical. */
DWORD osmw_claims_sealed_count(void)
{
    DWORD index;
    DWORD sealed = 0;

    if (!g_claims_ready) {
        return 0;
    }
    EnterCriticalSection(&g_claim_lock);
    for (index = 0; index < OSMW_CLAIM_CAP; ++index) {
        if (g_claims[index].token != OSMW_INVALID_TOKEN &&
                g_claims[index].sealed != NULL) {
            ++sealed;
        }
    }
    LeaveCriticalSection(&g_claim_lock);
    return sealed;
}

/* Re-read one record into its seal.  Caller holds g_claim_lock. */
static BOOL seal_record(OSMW_CLAIM_RECORD *record)
{
    BYTE *buffer = record->sealed;
    SIZE_T bytes_read = 0;

    if (buffer == NULL) {
        buffer = (BYTE *)HeapAlloc(GetProcessHeap(), 0, record->length);
        if (buffer == NULL) {
            return FALSE;
        }
    }
    if (!ReadProcessMemory(GetCurrentProcess(), record->address, buffer,
                           record->length, &bytes_read) ||
            bytes_read != record->length) {
        /* Leave any previous seal in place rather than replacing it with a
         * half-read buffer, which would report a difference forever. */
        if (buffer != record->sealed) {
            HeapFree(GetProcessHeap(), 0, buffer);
        }
        return FALSE;
    }
    record->sealed = buffer;
    return TRUE;
}

/* Record the bytes currently at a claim as the ones its owner intends.
 *
 * This is the only thing that makes the tampered count mean what its name says.
 * The owner calls it after every write it makes to the region - the initial
 * patch, any later change, and the restore on the way out - and from then on any
 * difference was written by something that does not own the range.
 *
 * It deliberately cannot be called on somebody else claim: sealing is an
 * assertion about your own writes, and letting one module vouch for another
 * would let a compromised module launder a patch it did not make.
 *
 * OSMW_INVALID_TOKEN means "every claim I own".  That exists because the natural
 * place to seal is a module write helper, which knows the address it just wrote
 * but usually not which token covers it - without the bulk form, adopting this
 * would mean threading tokens through every patch call site in every module, and
 * a seal that is inconvenient to call is a seal that gets skipped.  It is still
 * scoped to the caller own claims, so it grants nobody any reach they lacked.
 */
OSMW_RESULT __cdecl osmw_api_seal_region(OSMW_OWNER owner, OSMW_TOKEN token)
{
    DWORD index;

    if (!g_claims_ready) {
        return OSMW_E_UNAVAILABLE;
    }
    {
        char module_id[OSMW_MODULE_ID_CAP];
        if (!osmw_core_owner_copy_id(owner, module_id, sizeof(module_id))) {
            return OSMW_E_WRONG_OWNER;
        }
    }

    EnterCriticalSection(&g_claim_lock);
    if (token == OSMW_INVALID_TOKEN) {
        DWORD sealed = 0;
        DWORD failed = 0;

        for (index = 0; index < OSMW_CLAIM_CAP; ++index) {
            OSMW_CLAIM_RECORD *record = &g_claims[index];

            if (record->token == OSMW_INVALID_TOKEN ||
                    record->owner != owner) {
                continue;
            }
            if (seal_record(record)) {
                ++sealed;
            } else {
                ++failed;
            }
        }
        LeaveCriticalSection(&g_claim_lock);
        if (sealed == 0) {
            return failed > 0 ? OSMW_E_INTERNAL : OSMW_E_NOT_FOUND;
        }
        return OSMW_OK;
    }

    for (index = 0; index < OSMW_CLAIM_CAP; ++index) {
        OSMW_CLAIM_RECORD *record = &g_claims[index];

        if (record->token != token) {
            continue;
        }
        if (record->owner != owner) {
            LeaveCriticalSection(&g_claim_lock);
            return OSMW_E_WRONG_OWNER;
        }
        if (!seal_record(record)) {
            LeaveCriticalSection(&g_claim_lock);
            return OSMW_E_INTERNAL;
        }
        LeaveCriticalSection(&g_claim_lock);
        return OSMW_OK;
    }
    LeaveCriticalSection(&g_claim_lock);
    return OSMW_E_NOT_FOUND;
}
OSMW_RESULT __cdecl osmw_api_claim_region(
    OSMW_OWNER owner, const OSMW_REGION_CLAIM_V1 *claim,
    OSMW_TOKEN *token)
{
    DWORD index;
    DWORD free_index = OSMW_CLAIM_CAP;
    BYTE *snapshot;
    SIZE_T bytes_read;
    char label[OSMW_CLAIM_LABEL_CAP];
    char requester_id[OSMW_MODULE_ID_CAP];
    char existing_id[OSMW_MODULE_ID_CAP];
    char existing_label[OSMW_CLAIM_LABEL_CAP];
    BYTE *existing_address = NULL;
    DWORD existing_length = 0;
    char message[384];
    int written;

    if (token != NULL) {
        *token = OSMW_INVALID_TOKEN;
    }
    if (!g_claims_ready ||
            !osmw_core_owner_copy_id(owner, requester_id,
                                     sizeof(requester_id))) {
        return OSMW_E_WRONG_OWNER;
    }
    if (claim == NULL || token == NULL ||
            claim->struct_size < sizeof(*claim) || claim->flags != 0 ||
            !valid_range(claim->address, claim->length) ||
            !valid_kind(claim->kind) ||
            !copy_label(claim->label, label)) {
        return OSMW_E_INVALID_ARGUMENT;
    }

    EnterCriticalSection(&g_claim_lock);
    for (index = 0; index < OSMW_CLAIM_CAP; ++index) {
        OSMW_CLAIM_RECORD *record = &g_claims[index];
        if (record->token == OSMW_INVALID_TOKEN) {
            if (free_index == OSMW_CLAIM_CAP) {
                free_index = index;
            }
            continue;
        }
        if (ranges_overlap((const BYTE *)claim->address, claim->length,
                           record->address, record->length)) {
            (void)osmw_core_owner_copy_id(
                record->owner, existing_id, sizeof(existing_id));
            strcpy(existing_label, record->label);
            existing_address = record->address;
            existing_length = record->length;
            LeaveCriticalSection(&g_claim_lock);
            written = _snprintf(
                message, sizeof(message),
                "claim denied: %s/%s [%p,+%lu] overlaps %s/%s [%p,+%lu]",
                requester_id, label, claim->address,
                (unsigned long)claim->length, existing_id, existing_label,
                existing_address, (unsigned long)existing_length);
            if (written < 0 || (DWORD)written >= sizeof(message)) {
                message[sizeof(message) - 1] = '\0';
            }
            osmw_api_log(owner, OSMW_LOG_ERROR, message);
            return OSMW_E_ALREADY_CLAIMED;
        }
    }
    if (free_index == OSMW_CLAIM_CAP) {
        LeaveCriticalSection(&g_claim_lock);
        return OSMW_E_BUSY;
    }

    snapshot = (BYTE *)HeapAlloc(GetProcessHeap(), 0, claim->length);
    if (snapshot == NULL) {
        LeaveCriticalSection(&g_claim_lock);
        return OSMW_E_INTERNAL;
    }
    if (!ReadProcessMemory(GetCurrentProcess(), claim->address, snapshot,
                           claim->length, &bytes_read) ||
            bytes_read != claim->length) {
        HeapFree(GetProcessHeap(), 0, snapshot);
        LeaveCriticalSection(&g_claim_lock);
        return OSMW_E_INVALID_ARGUMENT;
    }

    g_claims[free_index].token = next_token();
    g_claims[free_index].owner = owner;
    g_claims[free_index].address = (BYTE *)claim->address;
    g_claims[free_index].length = claim->length;
    g_claims[free_index].kind = claim->kind;
    g_claims[free_index].snapshot = snapshot;
    strcpy(g_claims[free_index].label, label);
    *token = g_claims[free_index].token;
    ++g_claim_count;
    LeaveCriticalSection(&g_claim_lock);
    return OSMW_OK;
}

OSMW_RESULT __cdecl osmw_api_release_region(
    OSMW_OWNER owner, OSMW_TOKEN token)
{
    DWORD index;
    BYTE *current;
    BYTE *snapshot;
    DWORD length;
    SIZE_T bytes_read;

    if (!g_claims_ready) {
        return OSMW_E_UNAVAILABLE;
    }
    {
        char module_id[OSMW_MODULE_ID_CAP];
        if (!osmw_core_owner_copy_id(owner, module_id, sizeof(module_id))) {
            return OSMW_E_WRONG_OWNER;
        }
    }
    if (token == OSMW_INVALID_TOKEN) {
        return OSMW_E_INVALID_ARGUMENT;
    }

    EnterCriticalSection(&g_claim_lock);
    for (index = 0; index < OSMW_CLAIM_CAP; ++index) {
        OSMW_CLAIM_RECORD *record = &g_claims[index];
        if (record->token != token) {
            continue;
        }
        if (record->owner != owner) {
            LeaveCriticalSection(&g_claim_lock);
            return OSMW_E_WRONG_OWNER;
        }
        length = record->length;
        current = (BYTE *)HeapAlloc(GetProcessHeap(), 0, length);
        if (current == NULL) {
            LeaveCriticalSection(&g_claim_lock);
            return OSMW_E_INTERNAL;
        }
        if (!ReadProcessMemory(GetCurrentProcess(), record->address, current,
                               length, &bytes_read) || bytes_read != length ||
                memcmp(current, record->snapshot, length) != 0) {
            HeapFree(GetProcessHeap(), 0, current);
            LeaveCriticalSection(&g_claim_lock);
            return OSMW_E_PATCH_STILL_ACTIVE;
        }
        HeapFree(GetProcessHeap(), 0, current);
        snapshot = record->snapshot;
        if (record->sealed != NULL) {
            HeapFree(GetProcessHeap(), 0, record->sealed);
        }
        ZeroMemory(record, sizeof(*record));
        --g_claim_count;
        LeaveCriticalSection(&g_claim_lock);
        HeapFree(GetProcessHeap(), 0, snapshot);
        return OSMW_OK;
    }
    LeaveCriticalSection(&g_claim_lock);
    return OSMW_E_NOT_FOUND;
}
