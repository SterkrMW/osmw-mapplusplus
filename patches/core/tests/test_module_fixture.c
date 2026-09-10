/* Minimal passive feature DLL used to prove the frozen ABI crosses a real
 * LoadLibrary/GetProcAddress boundary without an import library. */
#include <string.h>
#include <windows.h>
#include "osmw_core_api.h"

static OSMW_RESULT __cdecl fixture_init(
    const OSMW_CORE_API_V1 *api,
    OSMW_OWNER owner)
{
    if (!api || api->struct_size < sizeof(*api))
        return OSMW_E_BUFFER_TOO_SMALL;
    if (OSMW_ABI_MAJOR(api->abi_version) != 1)
        return OSMW_E_INCOMPATIBLE_ABI;
    if (owner == OSMW_INVALID_OWNER)
        return OSMW_E_INVALID_ARGUMENT;
    return OSMW_OK;
}

static OSMW_RESULT __cdecl fixture_shutdown(
    const OSMW_CORE_API_V1 *api,
    OSMW_OWNER owner)
{
    if (!api || owner == OSMW_INVALID_OWNER)
        return OSMW_E_INVALID_ARGUMENT;
    return OSMW_OK;
}

OSMW_RESULT __cdecl osmw_mod_query(
    DWORD core_abi_version,
    DWORD output_size,
    OSMW_MOD_DESCRIPTOR_V1 *output)
{
    if (OSMW_ABI_MAJOR(core_abi_version) !=
        OSMW_ABI_MAJOR(OSMW_CORE_ABI_V1_0))
        return OSMW_E_INCOMPATIBLE_ABI;
    if (!output)
        return OSMW_E_INVALID_ARGUMENT;
    if (output_size < OSMW_MOD_DESCRIPTOR_V1_MIN_SIZE)
        return OSMW_E_BUFFER_TOO_SMALL;

    memset(output, 0, sizeof(*output));
    output->struct_size = sizeof(*output);
    output->required_core_abi = OSMW_CORE_ABI_V1_0;
    output->module_version = OSMW_MODULE_VERSION(1, 0);
    output->module_id = "fixture";
    output->display_name = "ABI fixture";
    output->init = fixture_init;
    output->shutdown = fixture_shutdown;
    return OSMW_OK;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(instance);
    return TRUE;
}
