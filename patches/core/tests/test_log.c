#include "core_internal.h"

#include <stdio.h>
#include <string.h>

static int g_failures;

static void check(BOOL condition, const char *name)
{
    if (condition) {
        printf("ok:   %s\n", name);
    } else {
        printf("FAIL: %s\n", name);
        ++g_failures;
    }
}

static BOOL make_temp_dir(char *output, DWORD output_size)
{
    char base[MAX_PATH];
    DWORD length = GetTempPathA(sizeof(base), base);

    if (length == 0 || length >= sizeof(base) ||
            output_size < MAX_PATH ||
            !GetTempFileNameA(base, "ocl", 0, output)) {
        return FALSE;
    }
    return DeleteFileA(output) && CreateDirectoryA(output, NULL);
}

static void make_path(char *output, size_t output_size,
                      const char *dir, const char *name)
{
    int length = _snprintf(output, output_size, "%s\\%s", dir, name);
    if (length < 0 || length >= (int)output_size) {
        output[0] = '\0';
    }
}

static int count_logs(const char *dir)
{
    WIN32_FIND_DATAA data;
    HANDLE find;
    char pattern[MAX_PATH];
    int count = 0;

    make_path(pattern, sizeof(pattern), dir, "osmw_core.*.log");
    find = FindFirstFileA(pattern, &data);
    if (find == INVALID_HANDLE_VALUE) {
        return 0;
    }
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            ++count;
        }
    } while (FindNextFileA(find, &data));
    FindClose(find);
    return count;
}

static void make_old_logs(const char *dir, int count)
{
    int index;
    for (index = 0; index < count; ++index) {
        char name[64];
        char path[MAX_PATH];
        HANDLE file;
        _snprintf(name, sizeof(name), "osmw_core.old%d.log", index);
        name[sizeof(name) - 1] = '\0';
        make_path(path, sizeof(path), dir, name);
        file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
        }
    }
}

static DWORD read_file(const char *path, char *output, DWORD output_size)
{
    HANDLE file = CreateFileA(path, GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD read = 0;

    if (file == INVALID_HANDLE_VALUE || output_size == 0) {
        return 0;
    }
    if (!ReadFile(file, output, output_size - 1, &read, NULL)) {
        read = 0;
    }
    output[read] = '\0';
    CloseHandle(file);
    return read;
}

static void remove_logs(const char *dir)
{
    WIN32_FIND_DATAA data;
    HANDLE find;
    char pattern[MAX_PATH];

    make_path(pattern, sizeof(pattern), dir, "osmw_core.*.log");
    find = FindFirstFileA(pattern, &data);
    if (find == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        char path[MAX_PATH];
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        make_path(path, sizeof(path), dir, data.cFileName);
        (void)DeleteFileA(path);
    } while (FindNextFileA(find, &data));
    FindClose(find);
}

int main(void)
{
    char dir[MAX_PATH];
    char config_path[MAX_PATH];
    char log_name[64];
    char log_path[MAX_PATH];
    char content[8192];
    char large_message[901];
    OSMW_OWNER fixture;
    int index;

    check(make_temp_dir(dir, sizeof(dir)), "temporary log directory created");
    if (g_failures != 0) {
        return 1;
    }
    make_path(config_path, sizeof(config_path), dir, "config.ini");

    check(WritePrivateProfileStringA("Debug", "ModLogging", "0",
                                     config_path),
          "disabled logging setting written");
    check(osmw_core_services_startup(dir), "disabled services start");
    check(count_logs(dir) == 0, "disabled logging creates no file");
    osmw_core_shutdown();

    make_old_logs(dir, 7);
    check(count_logs(dir) == 7, "old log fixtures created");
    check(WritePrivateProfileStringA("Debug", "ModLogging", "1",
                                     config_path),
          "enabled logging setting written");
    check(osmw_core_services_startup(dir), "enabled services start");
    check(count_logs(dir) == 6,
          "startup keeps five old logs before creating the current log");

    fixture = osmw_core_owner_register("fixture");
    check(fixture != OSMW_INVALID_OWNER, "fixture log owner issued");
    osmw_api_log(fixture, OSMW_LOG_INFO, "hello\nforged");
    osmw_api_log(0xFFFFFFFFu, OSMW_LOG_ERROR,
                 "invalid-owner-must-not-appear");

    memset(large_message, 'x', sizeof(large_message) - 1);
    large_message[sizeof(large_message) - 1] = '\0';
    for (index = 0; index < 8; ++index) {
        osmw_api_log(fixture, OSMW_LOG_DEBUG, large_message);
    }
    osmw_api_log(fixture, OSMW_LOG_INFO,
                 "after-cap-must-not-appear");
    osmw_core_shutdown();

    _snprintf(log_name, sizeof(log_name), "osmw_core.%lu.log",
              (unsigned long)GetCurrentProcessId());
    log_name[sizeof(log_name) - 1] = '\0';
    make_path(log_path, sizeof(log_path), dir, log_name);
    check(read_file(log_path, content, sizeof(content)) != 0,
          "per-process core log can be read");
    check(strstr(content, "[core]") != NULL,
          "core line carries stable core id");
    check(strstr(content, "[fixture] hello forged") != NULL,
          "module line is tagged and embedded newline is flattened");
    check(strstr(content, "invalid-owner-must-not-appear") == NULL,
          "invalid owner line rejected");
    check(strstr(content, "size cap reached") != NULL,
          "size cap closes the session log with a diagnostic");
    check(strstr(content, "after-cap-must-not-appear") == NULL,
          "writes after the size cap are ignored");

    check(WritePrivateProfileStringA("Debug", "ModLogging", "0",
                                     config_path),
          "logging disabled again");
    check(osmw_core_services_startup(dir), "disabled services restart");
    check(count_logs(dir) == 0,
          "disabling logging prunes prior core logs");
    osmw_core_shutdown();

    remove_logs(dir);
    (void)DeleteFileA(config_path);
    (void)RemoveDirectoryA(dir);

    printf("%s: tagged core logging, %d failure(s)\n",
           g_failures == 0 ? "PASSED" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
