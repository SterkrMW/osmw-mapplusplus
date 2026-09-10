/* Resolve the chat-owned catalog through core's pristine executable image. */
#include "osmw_chat.h"

unsigned char *mw_addr[MW_ADDR_COUNT];

void mw_resolve_reset(void)
{
    ZeroMemory(mw_addr, sizeof(mw_addr));
}

OSMW_RESULT mw_resolve_all(void)
{
    const OSMW_CORE_API_V1 *api = mw_core_api();
    OSMW_OWNER owner = mw_core_owner();
    OSMW_RESULT first_failure = OSMW_OK;
    int i;

    mw_resolve_reset();
    if (api == NULL || api->resolve_signature == NULL ||
            owner == OSMW_INVALID_OWNER)
        return OSMW_E_UNAVAILABLE;

    for (i = 0; i < MW_SIG_COUNT; ++i) {
        const mw_sig_t *s = &MW_SIGS[i];
        OSMW_SIGNATURE_V1 request;
        OSMW_RESOLVED_ADDRESS_V1 resolved;
        OSMW_RESULT result;

        ZeroMemory(&request, sizeof(request));
        request.struct_size = sizeof(request);
        request.name = s->name;
        request.pattern = s->pattern;
        request.mask = s->mask;
        request.length = (DWORD)s->length;
        request.operand_offset = (LONG)s->op_offset;
        request.reference_rva = s->reference_rva;

        ZeroMemory(&resolved, sizeof(resolved));
        resolved.struct_size = sizeof(resolved);
        result = api->resolve_signature(owner, &request, &resolved);
        if (result != OSMW_OK) {
            mw_log("resolve: %-20s failed (%ld, %lu match%s)%s",
                   s->name, (long)result,
                   (unsigned long)resolved.match_count,
                   resolved.match_count == 1 ? "" : "es",
                   s->optional ? " (optional)" : "");
            if (!s->optional && first_failure == OSMW_OK)
                first_failure = result;
            continue;
        }

        mw_addr[i] = (unsigned char *)resolved.address;
        mw_log("resolve: %-20s %p rva 0x%08lX%s", s->name, mw_addr[i],
               (unsigned long)(s->reference_rva + resolved.rva_delta),
               resolved.rva_delta == 0 ?
                   "" : " (differs from reference build)");
    }

    return first_failure;
}
