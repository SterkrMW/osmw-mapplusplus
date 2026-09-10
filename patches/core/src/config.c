/* Game-root configuration service.
 *
 * Every read follows config.ini -> defaults.ini -> caller literal.  Writes
 * target config.ini only, preserving the existing file-backed settings
 * contract shared with Maps++ and the feature DLLs.
 */
#include "core_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OSMW_CONFIG_VALUE_CAP 4096
#define OSMW_CONFIG_ABSENT "\x01__osmw_core_absent__"

static char g_game_dir[MAX_PATH];
static char g_config_path[MAX_PATH];
static char g_defaults_path[MAX_PATH];

static BOOL make_profile_paths(const char *game_dir)
{
    size_t length;
    int written;

    if (game_dir == NULL || game_dir[0] == '\0') {
        return FALSE;
    }
    length = strlen(game_dir);
    if (length >= sizeof(g_game_dir)) {
        return FALSE;
    }
    memcpy(g_game_dir, game_dir, length + 1);
    while (length > 3 &&
            (g_game_dir[length - 1] == '\\' ||
             g_game_dir[length - 1] == '/')) {
        g_game_dir[--length] = '\0';
    }

    written = _snprintf(g_config_path, sizeof(g_config_path),
                        "%s%sconfig.ini", g_game_dir,
                        g_game_dir[length - 1] == '\\' ? "" : "\\");
    if (written < 0 || written >= (int)sizeof(g_config_path)) {
        osmw_config_reset();
        return FALSE;
    }
    written = _snprintf(g_defaults_path, sizeof(g_defaults_path),
                        "%s%sdefaults.ini", g_game_dir,
                        g_game_dir[length - 1] == '\\' ? "" : "\\");
    if (written < 0 || written >= (int)sizeof(g_defaults_path)) {
        osmw_config_reset();
        return FALSE;
    }
    return TRUE;
}

BOOL osmw_config_init_for_dir(const char *game_dir)
{
    osmw_config_reset();
    return make_profile_paths(game_dir);
}

BOOL osmw_config_init(void)
{
    char process_path[MAX_PATH];
    char *backslash;
    char *slash;
    char *last;
    DWORD length;

    osmw_config_reset();
    length = GetModuleFileNameA(NULL, process_path, sizeof(process_path));
    if (length == 0 || length >= sizeof(process_path)) {
        return FALSE;
    }
    backslash = strrchr(process_path, '\\');
    slash = strrchr(process_path, '/');
    last = backslash;
    if (slash != NULL && (last == NULL || slash > last)) {
        last = slash;
    }
    if (last == NULL) {
        return FALSE;
    }
    if (last == process_path + 2 && process_path[1] == ':') {
        last[1] = '\0';
    } else {
        *last = '\0';
    }
    return make_profile_paths(process_path);
}

void osmw_config_reset(void)
{
    g_game_dir[0] = '\0';
    g_config_path[0] = '\0';
    g_defaults_path[0] = '\0';
}

const char *__cdecl osmw_api_game_dir(void)
{
    return g_game_dir;
}

/* 0 absent, 1 found, 2 found but longer than the service scratch buffer. */
static int read_from(const char *path, const char *section, const char *key,
                     char *output, DWORD output_size)
{
    DWORD length;

    if (path[0] == '\0') {
        return 0;
    }
    length = GetPrivateProfileStringA(section, key, OSMW_CONFIG_ABSENT,
                                      output, output_size, path);
    if (strcmp(output, OSMW_CONFIG_ABSENT) == 0) {
        return 0;
    }
    return length >= output_size - 1 ? 2 : 1;
}

static int read_effective(const char *section, const char *key,
                          char *output, DWORD output_size)
{
    int result = read_from(g_config_path, section, key, output, output_size);
    if (result != 0) {
        return result;
    }
    return read_from(g_defaults_path, section, key, output, output_size);
}

static BOOL valid_owner(OSMW_OWNER owner)
{
    char module_id[OSMW_MODULE_ID_CAP];
    return osmw_core_owner_copy_id(
        owner, module_id, (DWORD)sizeof(module_id));
}

int __cdecl osmw_api_config_read_int(OSMW_OWNER owner,
                                     const char *section,
                                     const char *key,
                                     int fallback)
{
    char buffer[128];
    char *end;
    long value;
    int result;

    if (!valid_owner(owner) || section == NULL || section[0] == '\0' ||
            key == NULL || key[0] == '\0') {
        return fallback;
    }
    result = read_effective(section, key, buffer, (DWORD)sizeof(buffer));
    if (result != 1) {
        return fallback;
    }
    end = buffer;
    value = strtol(buffer, &end, 0);
    return end != buffer ? (int)value : fallback;
}

static OSMW_RESULT copy_result(const char *value, BOOL source_truncated,
                               char *output, DWORD output_size)
{
    size_t length = strlen(value);
    size_t copied = length;

    if (copied >= output_size) {
        copied = output_size - 1;
    }
    if (copied != 0) {
        memcpy(output, value, copied);
    }
    output[copied] = '\0';
    return source_truncated || length >= output_size
        ? OSMW_E_BUFFER_TOO_SMALL
        : OSMW_OK;
}

OSMW_RESULT __cdecl osmw_api_config_read_string(
    OSMW_OWNER owner, const char *section, const char *key,
    const char *fallback, char *output, DWORD output_size)
{
    char value[OSMW_CONFIG_VALUE_CAP];
    int result;

    if (output == NULL || output_size == 0 || section == NULL ||
            section[0] == '\0' || key == NULL || key[0] == '\0') {
        return OSMW_E_INVALID_ARGUMENT;
    }
    output[0] = '\0';
    if (!valid_owner(owner)) {
        return OSMW_E_WRONG_OWNER;
    }
    result = read_effective(section, key, value, (DWORD)sizeof(value));
    if (result != 0) {
        return copy_result(value, result == 2, output, output_size);
    }
    return copy_result(fallback != NULL ? fallback : "", FALSE,
                       output, output_size);
}

OSMW_RESULT __cdecl osmw_api_config_write_string(
    OSMW_OWNER owner, const char *section, const char *key, const char *value)
{
    if (section == NULL || section[0] == '\0' || key == NULL ||
            key[0] == '\0' || value == NULL) {
        return OSMW_E_INVALID_ARGUMENT;
    }
    if (!valid_owner(owner)) {
        return OSMW_E_WRONG_OWNER;
    }
    if (g_config_path[0] == '\0') {
        return OSMW_E_UNAVAILABLE;
    }
    return WritePrivateProfileStringA(section, key, value, g_config_path)
        ? OSMW_OK
        : OSMW_E_INTERNAL;
}
