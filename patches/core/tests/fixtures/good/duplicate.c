#include "osmw_core_api.h"
#include "../fixture_io.h"

static OSMW_RESULT __cdecl must_not_init(
    const OSMW_CORE_API_V1 *api, OSMW_OWNER owner)
{
    (void)api;
    (void)owner;
    (void)fixture_append_event("duplicate.init.ERROR");
    return OSMW_E_INTERNAL;
}

static OSMW_RESULT __cdecl fixture_shutdown(
    const OSMW_CORE_API_V1 *api, OSMW_OWNER owner)
{
    (void)api;
    (void)owner;
    return OSMW_OK;
}

OSMW_RESULT __cdecl osmw_mod_query(
    DWORD core_abi_version, DWORD output_size,
    OSMW_MOD_DESCRIPTOR_V1 *output)
{
    if (core_abi_version != OSMW_CORE_ABI_CURRENT) {
        return OSMW_E_INCOMPATIBLE_ABI;
    }
    if (output == NULL || output_size < sizeof(*output)) {
        return OSMW_E_BUFFER_TOO_SMALL;
    }
    ZeroMemory(output, sizeof(*output));
    output->struct_size = sizeof(*output);
    output->required_core_abi = OSMW_CORE_ABI_V1_0;
    output->module_version = OSMW_MODULE_VERSION(9, 9);
    output->module_id = "good";
    output->display_name = "Duplicate id fixture";
    output->init = must_not_init;
    output->shutdown = fixture_shutdown;
    return OSMW_OK;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
