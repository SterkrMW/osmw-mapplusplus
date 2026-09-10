/* Signature resolution over the one immutable on-disk .text image. */
#include "core_internal.h"

#include <stdio.h>
#include <string.h>

#define OSMW_SCAN_CACHE_CAP 256u
#define OSMW_SCAN_CACHE_COPY_LIMIT 4096u

struct osmw_scan_cache_entry {
    BYTE *key;                  /* pattern bytes followed by mask bytes */
    DWORD length;
    LONG operand_offset;
    DWORD match_count;
    DWORD match_rva;
    DWORD address_value;
    OSMW_RESULT result;
};

static struct osmw_scan_cache_entry g_cache[OSMW_SCAN_CACHE_CAP];
static DWORD g_cache_count;
static BOOL g_scan_ready;
static CRITICAL_SECTION g_scan_lock;

BOOL osmw_scan_start(void)
{
    OSMW_IMAGE_VIEW image;

    if (g_scan_ready) {
        return TRUE;
    }
    if (!osmw_image_get_view(&image)) {
        return FALSE;
    }
    ZeroMemory(g_cache, sizeof(g_cache));
    g_cache_count = 0;
    InitializeCriticalSection(&g_scan_lock);
    g_scan_ready = TRUE;
    return TRUE;
}

void osmw_scan_reset(void)
{
    DWORD index;

    if (!g_scan_ready) {
        return;
    }
    EnterCriticalSection(&g_scan_lock);
    for (index = 0; index < g_cache_count; ++index) {
        if (g_cache[index].key != NULL) {
            HeapFree(GetProcessHeap(), 0, g_cache[index].key);
        }
    }
    ZeroMemory(g_cache, sizeof(g_cache));
    g_cache_count = 0;
    LeaveCriticalSection(&g_scan_lock);
    DeleteCriticalSection(&g_scan_lock);
    g_scan_ready = FALSE;
}

DWORD osmw_scan_cache_entries(void)
{
    DWORD count;
    if (!g_scan_ready) {
        return 0;
    }
    EnterCriticalSection(&g_scan_lock);
    count = g_cache_count;
    LeaveCriticalSection(&g_scan_lock);
    return count;
}

static BOOL valid_request(const OSMW_SIGNATURE_V1 *signature)
{
    DWORD index;

    if (signature == NULL ||
            signature->struct_size < sizeof(OSMW_SIGNATURE_V1) ||
            signature->name == NULL || signature->name[0] == '\0' ||
            signature->pattern == NULL || signature->mask == NULL ||
            signature->length == 0 || signature->operand_offset < -1 ||
            signature->flags != 0) {
        return FALSE;
    }
    if (signature->operand_offset >= 0 &&
            ((DWORD)signature->operand_offset > signature->length ||
             signature->length - (DWORD)signature->operand_offset <
                 sizeof(DWORD))) {
        return FALSE;
    }
    for (index = 0; index < signature->length; ++index) {
        if (signature->mask[index] != 'x' && signature->mask[index] != '?') {
            return FALSE;
        }
    }
    return signature->mask[signature->length] == '\0';
}

static BOOL cache_key_equal(const struct osmw_scan_cache_entry *entry,
                            const OSMW_SIGNATURE_V1 *signature)
{
    return entry->length == signature->length &&
           entry->operand_offset == signature->operand_offset &&
           memcmp(entry->key, signature->pattern, signature->length) == 0 &&
           memcmp(entry->key + signature->length,
                  signature->mask, signature->length) == 0;
}

static struct osmw_scan_cache_entry *cache_find(
    const OSMW_SIGNATURE_V1 *signature)
{
    DWORD index;
    for (index = 0; index < g_cache_count; ++index) {
        if (cache_key_equal(&g_cache[index], signature)) {
            return &g_cache[index];
        }
    }
    return NULL;
}

static void cache_store(const OSMW_SIGNATURE_V1 *signature,
                        const struct osmw_scan_cache_entry *result)
{
    struct osmw_scan_cache_entry *entry;
    SIZE_T key_size;

    if (g_cache_count >= OSMW_SCAN_CACHE_CAP ||
            signature->length > OSMW_SCAN_CACHE_COPY_LIMIT) {
        return;
    }
    key_size = (SIZE_T)signature->length * 2u;
    entry = &g_cache[g_cache_count];
    *entry = *result;
    entry->key = (BYTE *)HeapAlloc(GetProcessHeap(), 0, key_size);
    if (entry->key == NULL) {
        ZeroMemory(entry, sizeof(*entry));
        return;
    }
    memcpy(entry->key, signature->pattern, signature->length);
    memcpy(entry->key + signature->length,
           signature->mask, signature->length);
    entry->length = signature->length;
    entry->operand_offset = signature->operand_offset;
    ++g_cache_count;
}

static BOOL match_at(const BYTE *candidate,
                     const OSMW_SIGNATURE_V1 *signature)
{
    DWORD index;
    for (index = 0; index < signature->length; ++index) {
        if (signature->mask[index] == 'x' &&
                candidate[index] != signature->pattern[index]) {
            return FALSE;
        }
    }
    return TRUE;
}

static void scan_image(const OSMW_IMAGE_VIEW *image,
                       const OSMW_SIGNATURE_V1 *signature,
                       struct osmw_scan_cache_entry *result)
{
    DWORD offset;
    DWORD first_offset = 0;
    DWORD limit = image->text_size - signature->length;

    ZeroMemory(result, sizeof(*result));
    for (offset = 0; offset <= limit; ++offset) {
        if (match_at(image->text + offset, signature)) {
            if (++result->match_count == 1) {
                first_offset = offset;
            } else {
                result->match_count = 2; /* public 2 means two-or-more */
                result->result = OSMW_E_MULTIPLE_MATCHES;
                return;
            }
        }
    }
    if (result->match_count == 0) {
        result->result = OSMW_E_NOT_FOUND;
        return;
    }

    result->match_rva = image->text_rva + first_offset;
    if (signature->operand_offset == -1) {
        result->address_value = image->loaded_base + result->match_rva;
    } else {
        memcpy(&result->address_value,
               image->text + first_offset + signature->operand_offset,
               sizeof(result->address_value));
    }
    result->result = OSMW_OK;
}

static void fill_output(const OSMW_IMAGE_VIEW *image,
                        const OSMW_SIGNATURE_V1 *signature,
                        const struct osmw_scan_cache_entry *result,
                        OSMW_RESOLVED_ADDRESS_V1 *output)
{
    DWORD resolved_rva;

    output->struct_size = sizeof(*output);
    output->match_count = result->match_count;
    output->reference_rva = signature->reference_rva;
    if (result->result != OSMW_OK) {
        return;
    }
    output->address = (void *)(ULONG_PTR)result->address_value;
    output->match_rva = result->match_rva;
    resolved_rva = result->address_value - image->loaded_base;
    output->rva_delta = (LONG)(resolved_rva - signature->reference_rva);
}

static void log_drift(OSMW_OWNER owner,
                      const OSMW_SIGNATURE_V1 *signature,
                      const OSMW_RESOLVED_ADDRESS_V1 *output)
{
    char message[320];
    DWORD resolved_rva;
    int length;

    if (signature->reference_rva == 0 || output->rva_delta == 0) {
        return;
    }
    resolved_rva = (DWORD)(ULONG_PTR)output->address;
    {
        OSMW_IMAGE_VIEW image;
        if (!osmw_image_get_view(&image)) {
            return;
        }
        resolved_rva -= image.loaded_base;
    }
    length = _snprintf(
        message, sizeof(message),
        "signature %s resolved RVA 0x%08lX; reference 0x%08lX (delta %+ld)",
        signature->name, (unsigned long)resolved_rva,
        (unsigned long)signature->reference_rva, (long)output->rva_delta);
    if (length < 0) {
        message[sizeof(message) - 1] = '\0';
    }
    osmw_api_log(owner, OSMW_LOG_WARN, message);
}

OSMW_RESULT __cdecl osmw_api_resolve_signature(
    OSMW_OWNER owner, const OSMW_SIGNATURE_V1 *signature,
    OSMW_RESOLVED_ADDRESS_V1 *output)
{
    OSMW_IMAGE_VIEW image;
    struct osmw_scan_cache_entry scanned;
    struct osmw_scan_cache_entry *cached;
    char module_id[OSMW_MODULE_ID_CAP];
    DWORD output_size;
    OSMW_RESULT result;

    if (output == NULL) {
        return OSMW_E_INVALID_ARGUMENT;
    }
    output_size = output->struct_size;
    if (output_size < sizeof(*output)) {
        return OSMW_E_INVALID_ARGUMENT;
    }
    ZeroMemory(output, sizeof(*output));
    output->struct_size = sizeof(*output);

    if (!osmw_core_owner_copy_id(owner, module_id, sizeof(module_id))) {
        return OSMW_E_WRONG_OWNER;
    }
    if (!valid_request(signature)) {
        return OSMW_E_INVALID_ARGUMENT;
    }
    if (!g_scan_ready || !osmw_image_get_view(&image) ||
            !image.address_compatible) {
        return OSMW_E_UNAVAILABLE;
    }
    if (signature->length > image.text_size) {
        return OSMW_E_NOT_FOUND;
    }

    EnterCriticalSection(&g_scan_lock);
    cached = cache_find(signature);
    if (cached == NULL) {
        scan_image(&image, signature, &scanned);
        cache_store(signature, &scanned);
        cached = &scanned;
    }
    fill_output(&image, signature, cached, output);
    result = cached->result;
    LeaveCriticalSection(&g_scan_lock);

    if (result == OSMW_OK) {
        log_drift(owner, signature, output);
    }
    return result;
}
