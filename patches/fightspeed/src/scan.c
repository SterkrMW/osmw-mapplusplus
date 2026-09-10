/* Resolve generated signatures through core's pristine image. Fight-speed
 * still reads live .text bounds for its specialised caller/bounds enumeration,
 * but performs no private generated-pattern matching. */
#include "osmw_fightspeed.h"

unsigned char *fs_addr[FS_ADDR_COUNT];
unsigned char *fs_base;
unsigned int   fs_timestamp;

/* Locate .text so we scan code only -- scanning .data invites false hits. */
int fs_find_text(unsigned char *base, unsigned char **out_lo, unsigned char **out_hi)
{
    IMAGE_DOS_HEADER     *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS32   *nt;
    IMAGE_SECTION_HEADER *sec;
    int i;

    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return 0;
    nt = (IMAGE_NT_HEADERS32 *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return 0;

    fs_timestamp = nt->FileHeader.TimeDateStamp;

    sec = IMAGE_FIRST_SECTION(nt);
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        if (sec->Name[0] == '.' && sec->Name[1] == 't' &&
            sec->Name[2] == 'e' && sec->Name[3] == 'x' && sec->Name[4] == 't') {
            DWORD size = sec->Misc.VirtualSize;
            if (size == 0 || size > sec->SizeOfRawData)
                size = sec->SizeOfRawData;
            *out_lo = base + sec->VirtualAddress;
            *out_hi = base + sec->VirtualAddress + size;
            return 1;
        }
    }
    return 0;
}

void fs_resolve_reset(void)
{
    ZeroMemory(fs_addr, sizeof(fs_addr));
    fs_base = NULL;
    fs_timestamp = 0;
}

OSMW_RESULT fs_resolve_all(void)
{
    const OSMW_CORE_API_V1 *api = fs_core_api();
    OSMW_OWNER owner = fs_core_owner();
    unsigned char *lo, *hi;
    OSMW_RESULT first_failure = OSMW_OK;
    int index;

    fs_resolve_reset();
    fs_base = (unsigned char *)GetModuleHandleA(NULL);
    if (!fs_base || !fs_find_text(fs_base, &lo, &hi)) {
        fs_log("scan: cannot locate .text in the host image");
        return OSMW_E_INTERNAL;
    }
    if (api == NULL || api->resolve_signature == NULL ||
            owner == OSMW_INVALID_OWNER)
        return OSMW_E_UNAVAILABLE;

    for (index = 0; index < FS_SIG_COUNT; ++index) {
        const fs_sig_t *source = &FS_SIGS[index];
        OSMW_SIGNATURE_V1 request;
        OSMW_RESOLVED_ADDRESS_V1 resolved;
        OSMW_RESULT result;
        ZeroMemory(&request, sizeof(request));
        request.struct_size = sizeof(request);
        request.name = source->name;
        request.pattern = source->pattern;
        request.mask = source->mask;
        request.length = (DWORD)source->length;
        request.operand_offset = (LONG)source->op_offset;
        request.reference_rva = source->reference_rva;
        ZeroMemory(&resolved, sizeof(resolved));
        resolved.struct_size = sizeof(resolved);
        result = api->resolve_signature(owner, &request, &resolved);
        if (result != OSMW_OK) {
            fs_log("resolve: %-16s failed (%ld, %lu match%s)%s",
                   source->name, (long)result,
                   (unsigned long)resolved.match_count,
                   resolved.match_count == 1 ? "" : "es",
                   source->optional ? " (optional)" : "");
            if (source->optional &&
                    (result == OSMW_E_NOT_FOUND ||
                     result == OSMW_E_MULTIPLE_MATCHES))
                continue;
            if (first_failure == OSMW_OK)
                first_failure = result;
            continue;
        }
        fs_addr[index] = (unsigned char *)resolved.address;
        fs_log("resolve: %-16s %p rva 0x%08lX%s", source->name,
               fs_addr[index],
               (unsigned long)(source->reference_rva + resolved.rva_delta),
               resolved.rva_delta == 0 ? "" : " (differs from reference build)");
    }
    return first_failure;
}

/* The host's .text span, for callers that need to scan it themselves.
 * Returns 0 before fs_resolve_all has run. */
int fs_text_span(unsigned char **out_lo, unsigned char **out_hi)
{
    if (!fs_base)
        return 0;
    return fs_find_text(fs_base, out_lo, out_hi);
}

