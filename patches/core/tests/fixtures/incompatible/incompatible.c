#include "osmw_core_api.h"
#include "../fixture_io.h"

static OSMW_RESULT __cdecl must_not_init(
    const OSMW_CORE_API_V1 *api, OSMW_OWNER owner)
{
    (void)api;
    (void)owner;
    (void)fixture_append_event("incompatible.init.ERROR");
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
    (void)core_abi_version;
    if (output == NULL || output_size < sizeof(*output)) {
        return OSMW_E_BUFFER_TOO_SMALL;
    }
    ZeroMemory(output, sizeof(*output));
    output->struct_size = sizeof(*output);
    output->required_core_abi = OSMW_ABI_MAKE(2, 0);
    output->module_version = OSMW_MODULE_VERSION(1, 0);
    output->module_id = "future";
    output->display_name = "Future ABI fixture";
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
