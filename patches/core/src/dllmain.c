/* Loader boundary for osmw_core.dll.
 *
 * DllMain starts one bounded worker and does no feature work.  Normal unload
 * must call osmw_prepare_unload first so the worker is joined before the DLL's
 * code can be unmapped.
 */
#include "core_internal.h"

#define OSMW_UNLOAD_WAIT_MS 5000u
#define OSMW_UNLOAD_BUSY 1u
#define OSMW_UNLOAD_WAIT_FAILED 2u
#define OSMW_UNLOAD_UNSAFE 3u

static HANDLE g_worker;
static volatile LONG g_prepare_running;
static volatile LONG g_prepared;

/* Exported with this exact undecorated name by osmw_core.def.  A zero return
 * guarantees that the core has no running worker or live resources.  A
 * nonzero return tells the caller not to follow with FreeLibrary. */
DWORD WINAPI osmw_prepare_unload(LPVOID unused)
{
    DWORD wait_result;

    (void)unused;

    if (InterlockedCompareExchange(&g_prepared, 0, 0) != 0) {
        return 0;
    }
    if (InterlockedCompareExchange(&g_prepare_running, 1, 0) != 0) {
        return OSMW_UNLOAD_BUSY;
    }

    osmw_core_request_stop();
    if (g_worker != NULL) {
        wait_result = WaitForSingleObject(g_worker, OSMW_UNLOAD_WAIT_MS);
        if (wait_result != WAIT_OBJECT_0) {
            InterlockedExchange(&g_prepare_running, 0);
            return wait_result == WAIT_TIMEOUT
                ? OSMW_UNLOAD_BUSY
                : OSMW_UNLOAD_WAIT_FAILED;
        }
        CloseHandle(g_worker);
        g_worker = NULL;
    }

    if (!osmw_core_shutdown()) {
        InterlockedExchange(&g_prepare_running, 0);
        return OSMW_UNLOAD_UNSAFE;
    }
    InterlockedExchange(&g_prepared, 1);
    InterlockedExchange(&g_prepare_running, 0);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)reserved;

    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(instance);
        g_worker = CreateThread(NULL, 0, osmw_core_startup_worker,
                                NULL, 0, NULL);
        if (g_worker == NULL) {
            OutputDebugStringA("osmw_core: startup worker creation failed\n");
        }
        break;

    case DLL_PROCESS_DETACH:
        /* Never block or run shutdown under the loader lock.  The normal path
         * has already joined and closed the worker in prepare_unload. */
        osmw_core_request_stop();
        if (g_worker != NULL &&
                WaitForSingleObject(g_worker, 0) == WAIT_OBJECT_0) {
            CloseHandle(g_worker);
            g_worker = NULL;
        }
        break;
    }
    return TRUE;
}
