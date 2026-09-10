/* Translate generated timer signatures into core resolver requests. */
#include "osmw_timers.h"

unsigned char *tm_addr[TM_ADDR_COUNT];

void tm_resolve_reset(void)
{
    ZeroMemory(tm_addr, sizeof(tm_addr));
}

OSMW_RESULT tm_resolve_all(void)
{
    const OSMW_CORE_API_V1 *api = tm_core_api();
    OSMW_OWNER owner = tm_core_owner();
    OSMW_RESULT first_failure = OSMW_OK;
    int index;

    tm_resolve_reset();
    if (api == NULL || api->resolve_signature == NULL ||
            owner == OSMW_INVALID_OWNER)
        return OSMW_E_UNAVAILABLE;

    for (index = 0; index < TM_SIG_COUNT; ++index) {
        const tm_sig_t *source = &TM_SIGS[index];
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
            tm_log("resolve: %-16s failed (%ld, %lu match%s)%s",
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

        tm_addr[index] = (unsigned char *)resolved.address;
        tm_log("resolve: %-16s %p rva 0x%08lX%s",
               source->name, tm_addr[index],
               (unsigned long)(source->reference_rva + resolved.rva_delta),
               resolved.rva_delta == 0 ? "" : " (differs from reference build)");
    }
    return first_failure;
}
