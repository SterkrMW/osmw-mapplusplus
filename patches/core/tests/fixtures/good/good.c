#include "osmw_core_api.h"
#include "../fixture_io.h"

static OSMW_RESULT __cdecl fixture_init(
    const OSMW_CORE_API_V1 *api, OSMW_OWNER owner)
{
    OSMW_MODULE_STATUS_V1 status;

    if (api == NULL || owner == OSMW_INVALID_OWNER ||
            api->struct_size < OSMW_CORE_API_V1_0_SIZE ||
            api->abi_version != OSMW_CORE_ABI_CURRENT ||
            (api->capabilities & (OSMW_CAP_CONFIG | OSMW_CAP_MODULE_STATUS)) !=
                (OSMW_CAP_CONFIG | OSMW_CAP_MODULE_STATUS)) {
        return OSMW_E_INCOMPATIBLE_ABI;
    }
    ZeroMemory(&status, sizeof(status));
    status.struct_size = sizeof(status);
    if (api->module_status("good", &status) != OSMW_OK ||
            status.state != OSMW_MODULE_LOADED || status.filename[0] == '\0') {
        return OSMW_E_INTERNAL;
    }
    if (api->queue_game_packet(owner, 0, 0, NULL, 0) !=
            OSMW_E_UNAVAILABLE) {
        return OSMW_E_INTERNAL;
    }
    return fixture_append_event("good.init") ? OSMW_OK : OSMW_E_INTERNAL;
}

static OSMW_RESULT __cdecl fixture_shutdown(
    const OSMW_CORE_API_V1 *api, OSMW_OWNER owner)
{
    (void)api;
    (void)owner;
    return fixture_append_event("good.shutdown")
        ? OSMW_OK : OSMW_E_INTERNAL;
}

OSMW_RESULT __cdecl osmw_mod_query(
    DWORD core_abi_version, DWORD output_size,
    OSMW_MOD_DESCRIPTOR_V1 *output)
{
    if (OSMW_ABI_MAJOR(core_abi_version) != 1u) {
        return OSMW_E_INCOMPATIBLE_ABI;
    }
    if (output == NULL || output_size < sizeof(*output)) {
        return OSMW_E_BUFFER_TOO_SMALL;
    }
    ZeroMemory(output, sizeof(*output));
    output->struct_size = sizeof(*output);
    output->required_core_abi = OSMW_CORE_ABI_V1_0;
    output->module_version = OSMW_MODULE_VERSION(1, 2);
    output->module_id = "good";
    output->display_name = "Good fixture";
    output->init = fixture_init;
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
