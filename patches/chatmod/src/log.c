/* Line-buffered log to the game root, alongside mapsplusplus.log. */
#include "osmw_chat.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static HANDLE          g_file = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_lock;
static int             g_ready = 0;

void mw_log_open(const char *path)
{
    if (g_ready)
        return;
    InitializeCriticalSection(&g_lock);
    g_ready = 1;

    /* truncate on each launch -- these logs are for the current session */
    g_file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

/* One log per PID means the game root fills up over a few weeks of play, since
 * a PID is never reused often enough to overwrite. Keep only the newest few.
 *
 * Deleting a log that a running client still holds cannot happen: the handle
 * below is opened with FILE_SHARE_READ and no FILE_SHARE_DELETE, so DeleteFileA
 * simply fails on a live client's file and we move on. */
struct log_file {
    FILETIME when;
    char     name[64];
};

void mw_log_prune(const char *dir, int keep)
{
    WIN32_FIND_DATAA  fd;
    HANDLE            find;
    char              pattern[MAX_PATH];
    struct log_file   found[64];
    int               n = 0, i, j;

    _snprintf(pattern, sizeof(pattern), "%s\\osmw_chat.*.log", dir);
    pattern[sizeof(pattern) - 1] = '\0';

    find = FindFirstFileA(pattern, &fd);
    if (find == INVALID_HANDLE_VALUE)
        return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        if (strlen(fd.cFileName) >= sizeof(found[0].name))
            continue;
        found[n].when = fd.ftLastWriteTime;
        strcpy(found[n].name, fd.cFileName);
        n++;
    } while (n < (int)(sizeof(found) / sizeof(found[0])) &&
             FindNextFileA(find, &fd));
    FindClose(find);

    /* Insertion sort, newest first. n is at most 64. */
    for (i = 1; i < n; i++) {
        for (j = i; j > 0 &&
             CompareFileTime(&found[j - 1].when, &found[j].when) < 0; j--) {
            struct log_file tmp = found[j - 1];
            found[j - 1] = found[j];
            found[j] = tmp;
        }
    }

    for (i = keep; i < n; i++) {
        char path[MAX_PATH];
        _snprintf(path, sizeof(path), "%s\\%s", dir, found[i].name);
        path[sizeof(path) - 1] = '\0';
        DeleteFileA(path);
    }
}

void mw_log_close(void)
{
    if (!g_ready)
        return;
    EnterCriticalSection(&g_lock);
    if (g_file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
    LeaveCriticalSection(&g_lock);
    DeleteCriticalSection(&g_lock);
    g_ready = 0;
}

void mw_log(const char *fmt, ...)
{
    char      line[1024];
    int       n;
    SYSTEMTIME st;
    DWORD     written;
    va_list   ap;

    if (!g_ready || g_file == INVALID_HANDLE_VALUE)
        return;

    GetLocalTime(&st);
    n = _snprintf(line, sizeof(line), "[%02u:%02u:%02u.%03u] ",
                  st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    if (n < 0 || n >= (int)sizeof(line))
        return;

    va_start(ap, fmt);
    {
        int m = _vsnprintf(line + n, sizeof(line) - n - 3, fmt, ap);
        n = (m < 0) ? (int)sizeof(line) - 3 : n + m;
    }
    va_end(ap);

    line[n++] = '\r';
    line[n++] = '\n';

    EnterCriticalSection(&g_lock);
    WriteFile(g_file, line, (DWORD)n, &written, NULL);
    FlushFileBuffers(g_file);
    LeaveCriticalSection(&g_lock);
}
