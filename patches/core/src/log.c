/* One opt-in, tagged log for the core and every managed feature module. */
#include "core_internal.h"

#include <stdio.h>
#include <string.h>

#ifndef OSMW_LOG_MAX_BYTES
#define OSMW_LOG_MAX_BYTES (1024u * 1024u)
#endif

#define OSMW_LOG_KEEP 5

static HANDLE g_file = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_log_lock;
static BOOL g_log_ready;
static DWORD g_written;

struct osmw_log_file {
    FILETIME when;
    char name[64];
};

static void prune_logs(const char *game_dir, int keep)
{
    WIN32_FIND_DATAA found_data;
    HANDLE find;
    char pattern[MAX_PATH];
    struct osmw_log_file found[64];
    int count = 0;
    int index;
    int position;
    int length;

    length = _snprintf(pattern, sizeof(pattern), "%s\\osmw_core.*.log",
                       game_dir);
    if (length < 0 || length >= (int)sizeof(pattern)) {
        return;
    }
    find = FindFirstFileA(pattern, &found_data);
    if (find == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        if ((found_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
                strlen(found_data.cFileName) >= sizeof(found[0].name)) {
            continue;
        }
        found[count].when = found_data.ftLastWriteTime;
        strcpy(found[count].name, found_data.cFileName);
        ++count;
    } while (count < (int)(sizeof(found) / sizeof(found[0])) &&
             FindNextFileA(find, &found_data));
    FindClose(find);

    for (index = 1; index < count; ++index) {
        for (position = index; position > 0 &&
             CompareFileTime(&found[position - 1].when,
                             &found[position].when) < 0; --position) {
            struct osmw_log_file temporary = found[position - 1];
            found[position - 1] = found[position];
            found[position] = temporary;
        }
    }
    if (keep < 0) {
        keep = 0;
    }
    for (index = keep; index < count; ++index) {
        char path[MAX_PATH];
        length = _snprintf(path, sizeof(path), "%s\\%s", game_dir,
                           found[index].name);
        if (length < 0 || length >= (int)sizeof(path)) {
            continue;
        }
        (void)DeleteFileA(path);
    }
}

BOOL osmw_log_start(const char *game_dir, BOOL enabled)
{
    char path[MAX_PATH];
    int written;

    if (g_log_ready) {
        return TRUE;
    }
    if (game_dir == NULL || game_dir[0] == '\0') {
        return FALSE;
    }

    prune_logs(game_dir, enabled ? OSMW_LOG_KEEP : 0);
    if (!enabled) {
        return TRUE;
    }

    written = _snprintf(path, sizeof(path), "%s\\osmw_core.%lu.log",
                        game_dir, (unsigned long)GetCurrentProcessId());
    if (written < 0 || written >= (int)sizeof(path)) {
        return FALSE;
    }

    InitializeCriticalSection(&g_log_lock);
    g_log_ready = TRUE;
    g_written = 0;
    g_file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    return g_file != INVALID_HANDLE_VALUE;
}

void osmw_log_close(void)
{
    if (!g_log_ready) {
        return;
    }
    EnterCriticalSection(&g_log_lock);
    if (g_file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
    LeaveCriticalSection(&g_log_lock);
    DeleteCriticalSection(&g_log_lock);
    g_log_ready = FALSE;
    g_written = 0;
}

static const char *level_name(OSMW_LOG_LEVEL level)
{
    switch (level) {
    case OSMW_LOG_DEBUG: return "DEBUG";
    case OSMW_LOG_INFO:  return "INFO";
    case OSMW_LOG_WARN:  return "WARN";
    case OSMW_LOG_ERROR: return "ERROR";
    default:             return NULL;
    }
}

void __cdecl osmw_api_log(OSMW_OWNER owner, OSMW_LOG_LEVEL level,
                          const char *message)
{
    char module_id[OSMW_MODULE_ID_CAP];
    char line[1024];
    const char *level_text;
    SYSTEMTIME time;
    DWORD file_written;
    int length;
    size_t input;

    if (message == NULL ||
            !osmw_core_owner_copy_id(owner, module_id, sizeof(module_id))) {
        return;
    }
    level_text = level_name(level);
    if (level_text == NULL || !g_log_ready || g_file == INVALID_HANDLE_VALUE) {
        return;
    }

    GetLocalTime(&time);
    length = _snprintf(line, sizeof(line),
                       "[%02u:%02u:%02u.%03u] [%s] [%s] ",
                       time.wHour, time.wMinute, time.wSecond,
                       time.wMilliseconds, level_text, module_id);
    if (length < 0 || length >= (int)sizeof(line) - 2) {
        return;
    }
    for (input = 0; message[input] != '\0' &&
            length < (int)sizeof(line) - 2; ++input) {
        char ch = message[input];
        line[length++] = (ch == '\r' || ch == '\n') ? ' ' : ch;
    }
    line[length++] = '\r';
    line[length++] = '\n';

    EnterCriticalSection(&g_log_lock);
    if (g_file != INVALID_HANDLE_VALUE) {
        if (WriteFile(g_file, line, (DWORD)length, &file_written, NULL)) {
            g_written += file_written;
        }
        FlushFileBuffers(g_file);
        if (g_written >= OSMW_LOG_MAX_BYTES) {
            static const char capped[] =
                "[core] [WARN] size cap reached -- nothing further will be written\r\n";
            (void)WriteFile(g_file, capped, sizeof(capped) - 1,
                            &file_written, NULL);
            FlushFileBuffers(g_file);
            CloseHandle(g_file);
            g_file = INVALID_HANDLE_VALUE;
        }
    }
    LeaveCriticalSection(&g_log_lock);
}
