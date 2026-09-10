#include "core_internal.h"

#include <stdio.h>
#include <string.h>

#define FEATURE_COUNT 9

static const char *g_features[FEATURE_COUNT] = {
    "osmw_chat.dll",
    "osmw_login.dll",
    "osmw_minimap.dll",
    "osmw_fightspeed.dll",
    "osmw_weather.dll",
    "osmw_settings.dll",
    "osmw_timers.dll",
    "osmw_inventory.dll",
    "osmw_fishing.dll"
};

static BOOL make_path(
    char *output, DWORD output_size, const char *directory, const char *name)
{
    int length = _snprintf(output, output_size, "%s\\%s", directory, name);
    return length >= 0 && (DWORD)length < output_size;
}

static BOOL write_manifest(
    const char *path, const char *present, const char *missing)
{
    char content[512];
    DWORD length;
    DWORD written;
    HANDLE file;

    if (_snprintf(content, sizeof(content),
                  "[Debug]\r\nModLogging=0\r\n"
                  "[Core]\r\nModules=%s,%s\r\n",
                  present, missing) < 0) {
        return FALSE;
    }
    content[sizeof(content) - 1] = '\0';
    length = (DWORD)strlen(content);
    file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return FALSE;
    }
    if (!WriteFile(file, content, length, &written, NULL)) {
        CloseHandle(file);
        return FALSE;
    }
    CloseHandle(file);
    return written == length;
}

static BOOL expect_state(
    const char *lookup, OSMW_MODULE_STATE state,
    OSMW_RESULT last_result, const char *module_id)
{
    OSMW_MODULE_STATUS_V1 status;

    ZeroMemory(&status, sizeof(status));
    status.struct_size = sizeof(status);
    if (osmw_api_module_status(lookup, &status) != OSMW_OK ||
            status.state != state || status.last_result != last_result ||
            strcmp(status.module_id, module_id) != 0) {
        printf("%s: state=%lu last=%ld id=%s\n", lookup,
               (unsigned long)status.state, (long)status.last_result,
               status.module_id);
        return FALSE;
    }
    return TRUE;
}

int main(int argc, char **argv)
{
    const char *present_name = "fixture_present.dll";
    char temp_base[MAX_PATH];
    char temp_dir[MAX_PATH];
    char present_path[MAX_PATH] = "";
    char config_path[MAX_PATH] = "";
    DWORD index;
    BOOL started = FALSE;
    int failed = 0;

    if (argc != 2) {
        printf("usage: test_missing_features <passive fixture DLL>\n");
        return 2;
    }
    if (GetTempPathA(sizeof(temp_base), temp_base) == 0 ||
            _snprintf(temp_dir, sizeof(temp_dir), "%sosmw-missing-%lu-%lu",
                      temp_base, (unsigned long)GetCurrentProcessId(),
                      (unsigned long)GetTickCount()) < 0 ||
            !CreateDirectoryA(temp_dir, NULL) ||
            !make_path(present_path, sizeof(present_path), temp_dir,
                       present_name) ||
            !make_path(config_path, sizeof(config_path), temp_dir,
                       "config.ini") ||
            !CopyFileA(argv[1], present_path, FALSE)) {
        printf("could not prepare missing-feature fixture (error %lu)\n",
               (unsigned long)GetLastError());
        return 1;
    }

    for (index = 0; index < FEATURE_COUNT; ++index) {
        if (!write_manifest(config_path, present_name, g_features[index])) {
            printf("could not write manifest for %s\n", g_features[index]);
            failed = 1;
            break;
        }
        if (!osmw_core_services_startup(temp_dir)) {
            printf("core startup failed with %s absent\n", g_features[index]);
            failed = 1;
            break;
        }
        started = TRUE;
        if (osmw_loader_module_count() != 2u ||
                !expect_state(present_name, OSMW_MODULE_ACTIVE,
                              OSMW_OK, "fixture") ||
                !expect_state(g_features[index], OSMW_MODULE_MISSING,
                              OSMW_E_NOT_FOUND, "") ||
                GetModuleHandleA(present_name) == NULL) {
            printf("remaining feature did not stay active with %s absent\n",
                   g_features[index]);
            failed = 1;
        }
        if (!osmw_core_shutdown()) {
            printf("shutdown failed after %s missing case\n", g_features[index]);
            failed = 1;
            break;
        }
        started = FALSE;
        if (GetModuleHandleA(present_name) != NULL) {
            printf("passive fixture remained mapped after %s case\n",
                   g_features[index]);
            failed = 1;
        }
        if (failed)
            break;
    }

    if (!failed)
        printf("all %d independently missing feature cases passed\n",
               FEATURE_COUNT);
    if (started)
        (void)osmw_core_shutdown();
    DeleteFileA(config_path);
    DeleteFileA(present_path);
    RemoveDirectoryA(temp_dir);
    return failed;
}
