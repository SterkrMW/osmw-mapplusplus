/* Loads test_module_fixture.dll exactly as core will load a managed feature:
 * resolve the stable undecorated query name, negotiate ABI 1, then call the
 * function pointers returned in the descriptor. */
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "osmw_core_api.h"

static int g_failures;

static void expect(int condition, const char *message)
{
    if (!condition) {
        printf("FAIL: %s\n", message);
        g_failures++;
    } else {
        printf("ok:   %s\n", message);
    }
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "test_module_fixture.dll";
    OSMW_MOD_DESCRIPTOR_V1 descriptor;
    OSMW_CORE_API_V1 api;
    OSMW_MOD_QUERY_V1 query;
    union {
        FARPROC raw;
        OSMW_MOD_QUERY_V1 query;
    } proc;
    HMODULE module;
    OSMW_RESULT result;

    module = LoadLibraryA(path);
    expect(module != NULL, "fixture DLL loads");
    if (!module)
        return 1;

    proc.raw = GetProcAddress(module, OSMW_MOD_QUERY_EXPORT_NAME);
    expect(proc.raw != NULL, "undecorated osmw_mod_query resolves");
    if (!proc.raw) {
        FreeLibrary(module);
        return 1;
    }
    query = proc.query;

    memset(&descriptor, 0, sizeof(descriptor));
    result = query(OSMW_CORE_ABI_CURRENT,
                   OSMW_MOD_DESCRIPTOR_V1_MIN_SIZE - 1u, &descriptor);
    expect(result == OSMW_E_BUFFER_TOO_SMALL,
           "query rejects an incomplete descriptor prefix");

    memset(&descriptor, 0, sizeof(descriptor));
    result = query(OSMW_ABI_MAKE(2, 0), sizeof(descriptor), &descriptor);
    expect(result == OSMW_E_INCOMPATIBLE_ABI,
           "query rejects a different ABI major");

    memset(&descriptor, 0, sizeof(descriptor));
    result = query(OSMW_CORE_ABI_CURRENT, sizeof(descriptor), &descriptor);
    expect(result == OSMW_OK, "query accepts ABI 1.0");
    expect(descriptor.struct_size == sizeof(descriptor),
           "descriptor reports its full size");
    expect(descriptor.required_core_abi == OSMW_CORE_ABI_V1_0,
           "descriptor reports its minimum core ABI");
    expect(descriptor.module_id && strcmp(descriptor.module_id, "fixture") == 0,
           "descriptor carries the stable module id");
    expect(descriptor.init != NULL && descriptor.shutdown != NULL,
           "descriptor carries lifecycle callbacks");

    memset(&descriptor, 0, sizeof(descriptor));
    result = query(OSMW_ABI_MAKE(1, 1), sizeof(descriptor), &descriptor);
    expect(result == OSMW_OK,
           "query accepts a newer core minor within ABI major 1");

    memset(&api, 0, sizeof(api));
    api.struct_size = sizeof(api);
    api.abi_version = OSMW_CORE_ABI_CURRENT;
    result = descriptor.init(&api, (OSMW_OWNER)1u);
    expect(result == OSMW_OK, "host calls module init through the descriptor");
    result = descriptor.shutdown(&api, (OSMW_OWNER)1u);
    expect(result == OSMW_OK,
           "host calls module shutdown through the descriptor");

    expect(FreeLibrary(module) != 0, "passive fixture DLL unloads");
    printf("%s: ABI module boundary, %d failure(s)\n",
           g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
