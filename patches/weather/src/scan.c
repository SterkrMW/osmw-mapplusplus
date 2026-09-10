/* Resolve generated signatures through core's pristine image. Weather still
 * reads the live PE section bounds for its feature-specific enumeration of
 * callers to already-resolved functions; it does not privately match any
 * generated signature bytes. */
#include "osmw_weather.h"

unsigned char *wx_addr[WX_ADDR_COUNT];
unsigned char *wx_base;
unsigned int   wx_timestamp;

/* Locate .text so we scan code only -- scanning .data invites false hits. */
int wx_find_text(unsigned char *base, unsigned char **out_lo, unsigned char **out_hi)
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

    wx_timestamp = nt->FileHeader.TimeDateStamp;

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

void wx_resolve_reset(void)
{
    ZeroMemory(wx_addr, sizeof(wx_addr));
    wx_base = NULL;
    wx_timestamp = 0;
}

OSMW_RESULT wx_resolve_all(void)
{
    const OSMW_CORE_API_V1 *api = wx_core_api();
    OSMW_OWNER owner = wx_core_owner();
    unsigned char *lo, *hi;
    OSMW_RESULT first_failure = OSMW_OK;
    int index;

    wx_resolve_reset();
    wx_base = (unsigned char *)GetModuleHandleA(NULL);
    if (!wx_base || !wx_find_text(wx_base, &lo, &hi)) {
        wx_log("scan: cannot locate .text in the host image");
        return OSMW_E_INTERNAL;
    }
    if (api == NULL || api->resolve_signature == NULL ||
            owner == OSMW_INVALID_OWNER)
        return OSMW_E_UNAVAILABLE;

    for (index = 0; index < WX_SIG_COUNT; ++index) {
        const wx_sig_t *source = &WX_SIGS[index];
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
            wx_log("resolve: %-16s failed (%ld, %lu match%s)%s",
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
        wx_addr[index] = (unsigned char *)resolved.address;
        wx_log("resolve: %-16s %p rva 0x%08lX%s", source->name,
               wx_addr[index],
               (unsigned long)(source->reference_rva + resolved.rva_delta),
               resolved.rva_delta == 0 ? "" : " (differs from reference build)");
    }
    return first_failure;
}

/* The host's .text span, for callers that need to scan it themselves.
 * Returns 0 before wx_resolve_all has run. */
int wx_text_span(unsigned char **out_lo, unsigned char **out_hi)
{
    if (!wx_base)
        return 0;
    return wx_find_text(wx_base, out_lo, out_hi);
}
