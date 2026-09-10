#ifndef OSMW_LOADER_FIXTURE_IO_H
#define OSMW_LOADER_FIXTURE_IO_H

#include <windows.h>

static BOOL fixture_append_event(const char *event_name)
{
    char path[MAX_PATH];
    HANDLE file;
    DWORD length;
    DWORD written;

    if (GetEnvironmentVariableA("OSMW_FIXTURE_EVENTS", path,
                                sizeof(path)) == 0) {
        return FALSE;
    }
    file = CreateFileA(path, GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                       OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return FALSE;
    }
    if (SetFilePointer(file, 0, NULL, FILE_END) == INVALID_SET_FILE_POINTER &&
            GetLastError() != NO_ERROR) {
        CloseHandle(file);
        return FALSE;
    }
    length = lstrlenA(event_name);
    if (!WriteFile(file, event_name, length, &written, NULL) ||
            written != length ||
            !WriteFile(file, "\r\n", 2, &written, NULL) || written != 2) {
        CloseHandle(file);
        return FALSE;
    }
    CloseHandle(file);
    return TRUE;
}

#endif /* OSMW_LOADER_FIXTURE_IO_H */
