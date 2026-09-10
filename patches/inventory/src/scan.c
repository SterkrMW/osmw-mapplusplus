#include "osmw_inventory.h"

unsigned char *iv_addr[IV_ADDR_COUNT];

void iv_resolve_reset(void)
{
    ZeroMemory(iv_addr, sizeof(iv_addr));
}

OSMW_RESULT iv_resolve_all(void)
{
    const OSMW_CORE_API_V1 *api = iv_core_api();
    OSMW_OWNER owner = iv_core_owner();
    OSMW_RESULT first = OSMW_OK;
    int index;

    iv_resolve_reset();
    if (!api || !api->resolve_signature || owner == OSMW_INVALID_OWNER)
        return OSMW_E_UNAVAILABLE;

    for (index = 0; index < IV_SIG_COUNT; ++index) {
        const iv_sig_t *source = &IV_SIGS[index];
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
            /* An optional miss costs one feature, not the module. The sprite
             * bank and the cursor globals are optional: without them the panel
             * falls back to its text control and Sort still works. Standing
             * the whole module down because an ornament moved would be worse
             * than drawing no ornament. */
            iv_log("resolve: %-16s failed (%ld, %lu matches)%s", source->name,
                   (long)result, (unsigned long)resolved.match_count,
                   source->optional ? " -- optional, continuing" : "");
            if (!source->optional && first == OSMW_OK)
                first = result;
            continue;
        }
        iv_addr[index] = (unsigned char *)resolved.address;
        iv_log("resolve: %-16s %p rva 0x%08lX", source->name,
               iv_addr[index],
               (unsigned long)(source->reference_rva + resolved.rva_delta));
    }
    return first;
}
