#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    char path[MAX_PATH];
    HANDLE marker;
    (void)reserved;

    if (reason != DLL_PROCESS_ATTACH) {
        return TRUE;
    }
    DisableThreadLibraryCalls(instance);
    if (GetEnvironmentVariableA("OSMW_LEGACY_MARKER", path,
                                sizeof(path)) == 0) {
        return TRUE;
    }
    marker = CreateFileA(path, GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (marker != INVALID_HANDLE_VALUE) {
        CloseHandle(marker);
    }
    return TRUE;
}
