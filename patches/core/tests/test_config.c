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
            !GetTempFileNameA(base, "ocf", 0, output)) {
        return FALSE;
    }
    if (!DeleteFileA(output) || !CreateDirectoryA(output, NULL)) {
        return FALSE;
    }
    return TRUE;
}

static BOOL join_path(char *output, size_t output_size,
                      const char *dir, const char *name)
{
    int length = _snprintf(output, output_size, "%s\\%s", dir, name);
    if (length < 0 || length >= (int)output_size) {
        output[0] = '\0';
        return FALSE;
    }
    return TRUE;
}

static BOOL direct_value(const char *path, const char *section,
                         const char *key, char *output, DWORD output_size)
{
    static const char absent[] = "__absent__";
    GetPrivateProfileStringA(section, key, absent,
                             output, output_size, path);
    return strcmp(output, absent) != 0;
}

int main(void)
{
    char dir[MAX_PATH];
    char config_path[MAX_PATH];
    char defaults_path[MAX_PATH];
    char output[64];
    char short_output[5];
    OSMW_OWNER owner;
    OSMW_RESULT result;

    check(make_temp_dir(dir, sizeof(dir)), "temporary game directory created");
    if (g_failures != 0) {
        return 1;
    }
    check(join_path(config_path, sizeof(config_path), dir, "config.ini"),
          "config path fits MAX_PATH");
    check(join_path(defaults_path, sizeof(defaults_path), dir, "defaults.ini"),
          "defaults path fits MAX_PATH");

    check(WritePrivateProfileStringA("Test", "IntValue", "11",
                                     defaults_path),
          "default integer written");
    check(WritePrivateProfileStringA("Test", "OnlyDefault", "33",
                                     defaults_path),
          "default-only integer written");
    check(WritePrivateProfileStringA("Test", "Text", "default-value",
                                     defaults_path),
          "default string written");
    check(WritePrivateProfileStringA("Test", "IntValue", "22", config_path),
          "user integer written");
    check(WritePrivateProfileStringA("Test", "Text", "config-value",
                                     config_path),
          "user string written");
    check(WritePrivateProfileStringA("Test", "BadInt", "not-a-number",
                                     config_path),
          "invalid user integer written");
    check(WritePrivateProfileStringA("Test", "BadInt", "77",
                                     defaults_path),
          "shadowed default integer written");

    check(osmw_core_owners_init(), "owner registry initialized");
    owner = osmw_core_owner_register("fixture");
    check(owner != OSMW_INVALID_OWNER, "fixture owner issued");
    check(osmw_core_owner_register("fixture") == OSMW_INVALID_OWNER,
          "duplicate module id rejected");
    check(osmw_config_init_for_dir(dir), "explicit game directory accepted");
    check(strcmp(osmw_api_game_dir(), dir) == 0,
          "game_dir returns configured directory");

    check(osmw_api_config_read_int(owner, "Test", "IntValue", -1) == 22,
          "config.ini overrides defaults.ini for integers");
    check(osmw_api_config_read_int(owner, "Test", "OnlyDefault", -1) == 33,
          "defaults.ini supplies a missing integer");
    check(osmw_api_config_read_int(owner, "Test", "Missing", 44) == 44,
          "caller literal supplies a missing integer");
    check(osmw_api_config_read_int(owner, "Test", "BadInt", 55) == 55,
          "present invalid user value falls back to caller literal");
    check(osmw_api_config_read_int(0xFFFFFFFFu, "Test", "IntValue", 66) == 66,
          "invalid owner cannot read an integer");

    result = osmw_api_config_read_string(
        owner, "Test", "Text", "literal", output, sizeof(output));
    check(result == OSMW_OK && strcmp(output, "config-value") == 0,
          "config.ini overrides defaults.ini for strings");
    result = osmw_api_config_read_string(
        owner, "Test", "Missing", "literal", output, sizeof(output));
    check(result == OSMW_OK && strcmp(output, "literal") == 0,
          "string read reaches caller literal");
    result = osmw_api_config_read_string(
        owner, "Test", "Text", "literal", short_output,
        sizeof(short_output));
    check(result == OSMW_E_BUFFER_TOO_SMALL &&
              strcmp(short_output, "conf") == 0,
          "truncated string is NUL-terminated and reported");
    check(osmw_api_config_read_string(
              owner, "Test", "Text", "literal", NULL, 0) ==
              OSMW_E_INVALID_ARGUMENT,
          "invalid output buffer rejected");
    check(osmw_api_config_read_string(
              0xFFFFFFFFu, "Test", "Text", "literal",
              output, sizeof(output)) == OSMW_E_WRONG_OWNER,
          "invalid owner cannot read a string");

    check(osmw_api_config_write_string(
              owner, "Test", "Written", "user-only") == OSMW_OK,
          "string write succeeds");
    check(direct_value(config_path, "Test", "Written",
                       output, sizeof(output)) &&
              strcmp(output, "user-only") == 0,
          "write targets config.ini");
    check(!direct_value(defaults_path, "Test", "Written",
                        output, sizeof(output)),
          "write never targets defaults.ini");
    check(osmw_api_config_write_string(
              0xFFFFFFFFu, "Test", "Denied", "value") ==
              OSMW_E_WRONG_OWNER,
          "invalid owner write rejected");
    check(!direct_value(config_path, "Test", "Denied",
                        output, sizeof(output)),
          "rejected write leaves config.ini unchanged");

    osmw_config_reset();
    check(osmw_core_owner_unregister(owner), "fixture owner released");
    osmw_core_owners_shutdown();

    (void)DeleteFileA(config_path);
    (void)DeleteFileA(defaults_path);
    (void)RemoveDirectoryA(dir);

    printf("%s: core config service, %d failure(s)\n",
           g_failures == 0 ? "PASSED" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
