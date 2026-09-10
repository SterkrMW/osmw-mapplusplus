#include "core_internal.h"

#include <stdio.h>
#include <string.h>

#define FIXTURE_COUNT 6

static const char *g_fixture_names[FIXTURE_COUNT] = {
    "fixture_good.dll",
    "fixture_disabled.dll",
    "fixture_failed.dll",
    "fixture_duplicate.dll",
    "fixture_incompatible.dll",
    "fixture_legacy.dll"
};

static BOOL write_text(const char *path, const char *text)
{
    HANDLE file;
    DWORD length = (DWORD)strlen(text);
    DWORD written;
    BOOL ok;

    file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return FALSE;
    }
    ok = WriteFile(file, text, length, &written, NULL);
    CloseHandle(file);
    return ok && written == length;
}

static BOOL read_text(const char *path, char *output, DWORD output_size)
{
    HANDLE file;
    DWORD read_size;
    BOOL ok;

    if (output_size == 0) {
        return FALSE;
    }
    file = CreateFileA(path, GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return FALSE;
    }
    ok = ReadFile(file, output, output_size - 1, &read_size, NULL);
    CloseHandle(file);
    if (!ok) {
        return FALSE;
    }
    output[read_size] = '\0';
    return TRUE;
}

static BOOL make_path(
    char *output, DWORD output_size, const char *directory, const char *name)
{
    int length = _snprintf(output, output_size, "%s\\%s", directory, name);
    return length >= 0 && (DWORD)length < output_size;
}

static BOOL expect_status(
    const char *lookup, OSMW_MODULE_STATE expected_state,
    OSMW_RESULT expected_result, const char *expected_id)
{
    OSMW_MODULE_STATUS_V1 status;
    OSMW_RESULT result;

    ZeroMemory(&status, sizeof(status));
    status.struct_size = sizeof(status);
    result = osmw_api_module_status(lookup, &status);
    if (result != OSMW_OK || status.struct_size != sizeof(status) ||
            status.state != expected_state ||
            status.last_result != expected_result ||
            status.owned_claim_count != 0 ||
            status.subscription_count != 0 ||
            strcmp(status.module_id, expected_id) != 0) {
        printf("status mismatch for %s: call=%ld state=%lu last=%ld id=%s\n",
               lookup, (long)result, (unsigned long)status.state,
               (long)status.last_result, status.module_id);
        return FALSE;
    }
    return TRUE;
}

int main(int argc, char **argv)
{
    char temp_base[MAX_PATH];
    char temp_dir[MAX_PATH];
    char destination[MAX_PATH] = "";
    char config_path[MAX_PATH] = "";
    char events_path[MAX_PATH] = "";
    char legacy_marker[MAX_PATH] = "";
    char events[512] = "";
    const char *expected_init =
        "good.init\r\ndisabled.init\r\nfailed.init\r\n";
    const char *expected_shutdown =
        "good.init\r\ndisabled.init\r\nfailed.init\r\n"
        "disabled.shutdown\r\ngood.shutdown\r\n";
    const OSMW_CORE_API_V1 *api;
    OSMW_MODULE_STATUS_V1 short_status;
    DWORD index;
    BOOL services_started = FALSE;
    int failed = 0;

    if (argc != FIXTURE_COUNT + 1) {
        printf("usage: test_loader <six fixture DLLs>\n");
        return 2;
    }
    if (GetTempPathA(sizeof(temp_base), temp_base) == 0 ||
            _snprintf(temp_dir, sizeof(temp_dir), "%sosmw-loader-%lu-%lu",
                      temp_base, (unsigned long)GetCurrentProcessId(),
                      (unsigned long)GetTickCount()) < 0 ||
            !CreateDirectoryA(temp_dir, NULL)) {
        printf("could not create loader test directory\n");
        return 1;
    }

    for (index = 0; index < FIXTURE_COUNT; ++index) {
        if (!make_path(destination, sizeof(destination), temp_dir,
                       g_fixture_names[index]) ||
                !CopyFileA(argv[index + 1], destination, FALSE)) {
            printf("could not copy %s (error %lu)\n", g_fixture_names[index],
                   (unsigned long)GetLastError());
            failed = 1;
            goto cleanup;
        }
    }
    if (!make_path(config_path, sizeof(config_path), temp_dir, "config.ini") ||
            !make_path(events_path, sizeof(events_path), temp_dir,
                       "events.txt") ||
            !make_path(legacy_marker, sizeof(legacy_marker), temp_dir,
                       "legacy-attached.marker") ||
            !write_text(
                config_path,
                "[Debug]\r\nModLogging=0\r\n"
                "[Core]\r\n"
                "Modules=C:\\outside.dll,..\\escape.dll,osmw_core.dll,"
                "fixture_good.dll,FIXTURE_GOOD.DLL,fixture_disabled.dll,"
                "fixture_failed.dll,fixture_duplicate.dll,"
                "fixture_incompatible.dll,fixture_legacy.dll,"
                "fixture_missing.dll,not-a-dll.txt\r\n")) {
        printf("could not create loader config\n");
        failed = 1;
        goto cleanup;
    }
    SetEnvironmentVariableA("OSMW_FIXTURE_EVENTS", events_path);
    SetEnvironmentVariableA("OSMW_LEGACY_MARKER", legacy_marker);

    if (!osmw_core_services_startup(temp_dir)) {
        printf("core services did not start\n");
        failed = 1;
        goto cleanup;
    }
    services_started = TRUE;
    if (osmw_loader_module_count() != 7u) {
        printf("expected seven accepted manifest entries, got %lu\n",
               (unsigned long)osmw_loader_module_count());
        failed = 1;
        goto cleanup;
    }
    if (!expect_status("good", OSMW_MODULE_ACTIVE, OSMW_OK, "good") ||
            !expect_status("fixture_disabled.dll", OSMW_MODULE_DISABLED,
                           OSMW_MOD_DISABLED, "disabled") ||
            !expect_status("fixture_failed.dll", OSMW_MODULE_FAILED,
                           OSMW_E_INTERNAL, "failed") ||
            !expect_status("fixture_duplicate.dll", OSMW_MODULE_INCOMPATIBLE,
                           OSMW_E_DUPLICATE_ID, "good") ||
            !expect_status("fixture_incompatible.dll",
                           OSMW_MODULE_INCOMPATIBLE,
                           OSMW_E_INCOMPATIBLE_ABI, "") ||
            !expect_status("fixture_legacy.dll", OSMW_MODULE_LEGACY,
                           OSMW_E_INCOMPATIBLE_ABI, "") ||
            !expect_status("fixture_missing.dll", OSMW_MODULE_MISSING,
                           OSMW_E_NOT_FOUND, "")) {
        failed = 1;
        goto cleanup;
    }
    if (GetFileAttributesA(legacy_marker) != INVALID_FILE_ATTRIBUTES ||
            GetModuleHandleA("fixture_legacy.dll") != NULL) {
        printf("legacy DLL executed or remained mapped\n");
        failed = 1;
        goto cleanup;
    }
    if (GetModuleHandleA("fixture_good.dll") == NULL ||
            GetModuleHandleA("fixture_disabled.dll") == NULL ||
            GetModuleHandleA("fixture_failed.dll") != NULL ||
            GetModuleHandleA("fixture_duplicate.dll") != NULL ||
            GetModuleHandleA("fixture_incompatible.dll") != NULL) {
        printf("fixture mapping state is incorrect\n");
        failed = 1;
        goto cleanup;
    }
    if (!read_text(events_path, events, sizeof(events)) ||
            strcmp(events, expected_init) != 0) {
        printf("unexpected init events: %s\n", events);
        failed = 1;
        goto cleanup;
    }

    api = osmw_loader_api();
    /* struct_size tracks the newest table the core publishes, so it moves with
     * each minor release.  What must not move is where the 1.0 fields sit, and
     * seal_region landing beyond the 1.0 size is what proves the append was an
     * append - test_api_layout.c pins those offsets. */
    if (api == NULL || api->struct_size != OSMW_CORE_API_V1_1_SIZE ||
            api->struct_size <= OSMW_CORE_API_V1_0_SIZE ||
            api->seal_region == NULL ||
            api->abi_version != OSMW_CORE_ABI_CURRENT ||
            (api->capabilities & OSMW_CAP_MODULE_STATUS) == 0 ||
            (api->capabilities &
             (OSMW_CAP_UI_TICK | OSMW_CAP_GAME_PACKETS |
              OSMW_CAP_SEND_QUEUE)) != 0) {
        printf("versioned API table is invalid\n");
        failed = 1;
        goto cleanup;
    }
    ZeroMemory(&short_status, sizeof(short_status));
    short_status.struct_size = sizeof(short_status) - 1;
    if (api->module_status("good", &short_status) !=
            OSMW_E_BUFFER_TOO_SMALL) {
        printf("short status buffer was not rejected\n");
        failed = 1;
        goto cleanup;
    }
    short_status.struct_size = sizeof(short_status);
    if (api->module_status("unknown", &short_status) != OSMW_E_NOT_FOUND) {
        printf("unknown status lookup was not rejected\n");
        failed = 1;
        goto cleanup;
    }

    if (!osmw_loader_shutdown()) {
        printf("managed fixture shutdown was unsafe\n");
        failed = 1;
        goto cleanup;
    }
    if (!expect_status("good", OSMW_MODULE_UNLOADED, OSMW_OK, "good") ||
            !expect_status("disabled", OSMW_MODULE_UNLOADED,
                           OSMW_OK, "disabled") ||
            !read_text(events_path, events, sizeof(events)) ||
            strcmp(events, expected_shutdown) != 0) {
        printf("shutdown order or final status is incorrect: %s\n", events);
        failed = 1;
        goto cleanup;
    }
    printf("loader manifest, preflight, ABI and lifecycle fixtures passed\n");

cleanup:
    if (services_started) {
        osmw_core_shutdown();
    }
    SetEnvironmentVariableA("OSMW_FIXTURE_EVENTS", NULL);
    SetEnvironmentVariableA("OSMW_LEGACY_MARKER", NULL);
    if (events_path[0] != '\0') {
        DeleteFileA(events_path);
    }
    if (legacy_marker[0] != '\0') {
        DeleteFileA(legacy_marker);
    }
    if (config_path[0] != '\0') {
        DeleteFileA(config_path);
    }
    for (index = 0; index < FIXTURE_COUNT; ++index) {
        if (make_path(destination, sizeof(destination), temp_dir,
                      g_fixture_names[index])) {
            DeleteFileA(destination);
        }
    }
    RemoveDirectoryA(temp_dir);
    return failed;
}
