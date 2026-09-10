#include "osmw_core_api.h"
#include "../fixture_io.h"

static BYTE g_target[4] = {0x11, 0x22, 0x33, 0x44};
static OSMW_TOKEN g_claim_token;
static DWORD g_shutdown_attempts;

static OSMW_RESULT __cdecl fixture_init(
    const OSMW_CORE_API_V1 *api, OSMW_OWNER owner)
{
    OSMW_REGION_CLAIM_V1 claim;
    OSMW_RESULT result;

    if (api == NULL ||
            (api->capabilities & OSMW_CAP_REGION_CLAIMS) == 0) {
        return OSMW_E_UNAVAILABLE;
    }
    ZeroMemory(&claim, sizeof(claim));
    claim.struct_size = sizeof(claim);
    claim.address = g_target;
    claim.length = sizeof(g_target);
    claim.kind = OSMW_REGION_DATA;
    claim.label = "unrestored-fixture-target";
    result = api->claim_region(owner, &claim, &g_claim_token);
    if (result != OSMW_OK) {
        return result;
    }
    g_target[0] = 0x90;
    return fixture_append_event("unrestored.init")
        ? OSMW_OK : OSMW_E_INTERNAL;
}

static OSMW_RESULT __cdecl fixture_shutdown(
    const OSMW_CORE_API_V1 *api, OSMW_OWNER owner)
{
    OSMW_RESULT result;

    ++g_shutdown_attempts;
    if (g_shutdown_attempts == 1) {
        (void)fixture_append_event("unrestored.shutdown.blocked");
        return api->release_region(owner, g_claim_token);
    }
    g_target[0] = 0x11;
    result = api->release_region(owner, g_claim_token);
    if (result == OSMW_OK) {
        g_claim_token = OSMW_INVALID_TOKEN;
    }
    (void)fixture_append_event("unrestored.shutdown.restored");
    return result;
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
    output->module_version = OSMW_MODULE_VERSION(1, 0);
    output->module_id = "unrestored";
    output->display_name = "Unrestored claim fixture";
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
