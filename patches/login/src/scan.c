/* Resolve login-owned signatures through core's pristine executable image. */
#include "osmw_login.h"

unsigned char *lg_addr[LG_ADDR_COUNT];

void lg_resolve_reset(void)
{
    ZeroMemory(lg_addr, sizeof(lg_addr));
}

OSMW_RESULT lg_resolve_all(void)
{
    const OSMW_CORE_API_V1 *api = lg_core_api();
    OSMW_OWNER owner = lg_core_owner();
    OSMW_RESULT first_failure = OSMW_OK;
    int index;

    lg_resolve_reset();
    if (api == NULL || api->resolve_signature == NULL ||
            owner == OSMW_INVALID_OWNER)
        return OSMW_E_UNAVAILABLE;

    for (index = 0; index < LG_SIG_COUNT; ++index) {
        const lg_sig_t *source = &LG_SIGS[index];
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
            lg_log("resolve: %-16s failed (%ld, %lu match%s)",
                   source->name, (long)result,
                   (unsigned long)resolved.match_count,
                   resolved.match_count == 1 ? "" : "es");
            if (first_failure == OSMW_OK)
                first_failure = result;
            continue;
        }

        lg_addr[index] = (unsigned char *)resolved.address;
        lg_log("resolve: %-16s %p rva 0x%08lX%s",
               source->name, lg_addr[index],
               (unsigned long)(source->reference_rva + resolved.rva_delta),
               resolved.rva_delta == 0 ? "" : " (differs from reference build)");
    }
    return first_failure;
}
