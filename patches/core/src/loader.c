/* Deterministic manifest loader for managed feature DLLs.
 *
 * Every configured file is inspected as an inert PE image before LoadLibrary.
 * A DLL without the exact osmw_mod_query export is therefore classified as
 * legacy without allowing its DllMain to execute.  Managed DLLs are loaded and
 * initialised serially in manifest order through the versioned core API.
 */
#include "core_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define OSMW_LOADER_MODULE_CAP 32u
#define OSMW_LOADER_MANIFEST_CAP 2048u
#define OSMW_LOADER_FILE_CAP (64u * 1024u * 1024u)

#define OSMW_DEFAULT_MODULES \
    "osmw_chat.dll,osmw_login.dll,osmw_minimap.dll," \
    "osmw_fightspeed.dll,osmw_weather.dll,osmw_settings.dll," \
    "osmw_timers.dll,osmw_inventory.dll,osmw_fishing.dll"

typedef struct OSMW_LOADER_RECORD {
    HMODULE module;
    OSMW_OWNER owner;
    OSMW_MODULE_STATE state;
    DWORD module_version;
    DWORD required_core_abi;
    OSMW_RESULT last_result;
    OSMW_MOD_INIT_V1 init;
    OSMW_MOD_SHUTDOWN_V1 shutdown;
    char module_id[OSMW_MODULE_ID_CAP];
    char filename[OSMW_MODULE_FILENAME_CAP];
    char display_name[OSMW_MODULE_DISPLAY_NAME_CAP];
} OSMW_LOADER_RECORD;

typedef struct OSMW_PE_FILE_VIEW {
    const BYTE *data;
    DWORD size;
    DWORD size_of_headers;
    const IMAGE_SECTION_HEADER *sections;
    WORD section_count;
    DWORD export_rva;
    DWORD export_size;
} OSMW_PE_FILE_VIEW;

typedef DWORD OSMW_PREFLIGHT_RESULT;
#define OSMW_PREFLIGHT_OK        ((OSMW_PREFLIGHT_RESULT)0u)
#define OSMW_PREFLIGHT_NO_EXPORT ((OSMW_PREFLIGHT_RESULT)1u)
#define OSMW_PREFLIGHT_INVALID   ((OSMW_PREFLIGHT_RESULT)2u)

static BOOL g_loader_ready;
static BOOL g_loader_lock_ready;
static CRITICAL_SECTION g_loader_lock;
static OSMW_OWNER g_loader_core_owner;
static OSMW_LOADER_RECORD g_modules[OSMW_LOADER_MODULE_CAP];
static DWORD g_module_count;
static OSMW_CORE_API_V1 g_api;

static BOOL range_valid(DWORD offset, DWORD length, DWORD size)
{
    return offset <= size && length <= size - offset;
}

static void loader_log(OSMW_LOG_LEVEL level, const char *format, ...)
{
    char message[384];
    int written;
    va_list arguments;

    va_start(arguments, format);
    written = _vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    if (written < 0 || (DWORD)written >= sizeof(message)) {
        message[sizeof(message) - 1] = '\0';
    }
    osmw_api_log(g_loader_core_owner, level, message);
}

static BOOL owner_is_valid(OSMW_OWNER owner)
{
    char module_id[OSMW_MODULE_ID_CAP];
    return osmw_core_owner_copy_id(owner, module_id, sizeof(module_id));
}

static OSMW_RESULT unavailable_with_token(
    OSMW_OWNER owner, OSMW_TOKEN *token)
{
    if (token != NULL) {
        *token = OSMW_INVALID_TOKEN;
    }
    return owner_is_valid(owner) ? OSMW_E_UNAVAILABLE : OSMW_E_WRONG_OWNER;
}

static OSMW_RESULT __cdecl api_subscribe_post_ui_tick(
    OSMW_OWNER owner, LONG priority, OSMW_UI_TICK_CALLBACK_V1 callback,
    void *context, OSMW_TOKEN *token)
{
    if (!osmw_hook_ui_active()) {
        return unavailable_with_token(owner, token);
    }
    return osmw_dispatch_subscribe_ui(
        owner, priority, callback, context, token);
}

static OSMW_RESULT __cdecl api_unsubscribe(
    OSMW_OWNER owner, OSMW_TOKEN token)
{
    if (!osmw_hook_ui_active() && !osmw_hook_packet_active()) {
        return owner_is_valid(owner)
            ? OSMW_E_UNAVAILABLE : OSMW_E_WRONG_OWNER;
    }
    return osmw_dispatch_unsubscribe(owner, token);
}

static OSMW_RESULT __cdecl api_claim_game_packet(
    OSMW_OWNER owner, DWORD packet_type, OSMW_PACKET_HANDLER_V1 callback,
    void *context, OSMW_TOKEN *token)
{
    if (!osmw_hook_packet_active()) {
        return unavailable_with_token(owner, token);
    }
    return osmw_dispatch_claim_packet(
        owner, packet_type, callback, context, token);
}

static OSMW_RESULT __cdecl api_observe_game_packet_after(
    OSMW_OWNER owner, DWORD packet_type, LONG priority,
    OSMW_PACKET_OBSERVER_V1 callback, void *context, OSMW_TOKEN *token)
{
    if (!osmw_hook_packet_active()) {
        return unavailable_with_token(owner, token);
    }
    return osmw_dispatch_observe_packet(
        owner, packet_type, priority, callback, context, token);
}

static OSMW_RESULT __cdecl api_queue_game_packet(
    OSMW_OWNER owner, DWORD packet_type, DWORD field12,
    const void *extra, DWORD extra_size)
{
    if (!osmw_hook_ui_active() || !osmw_dispatch_send_available()) {
        return owner_is_valid(owner)
            ? OSMW_E_UNAVAILABLE : OSMW_E_WRONG_OWNER;
    }
    return osmw_dispatch_queue_packet(
        owner, packet_type, field12, extra, extra_size);
}

static void initialise_api_table(void)
{
    OSMW_IMAGE_VIEW image;

    ZeroMemory(&g_api, sizeof(g_api));
    g_api.struct_size = sizeof(g_api);
    g_api.abi_version = OSMW_CORE_ABI_CURRENT;
    g_api.capabilities = OSMW_CAP_CONFIG | OSMW_CAP_REGION_CLAIMS |
        OSMW_CAP_MODULE_STATUS;
    if (osmw_image_get_view(&image) && image.address_compatible) {
        g_api.capabilities |= OSMW_CAP_SIGNATURES;
    }
    if (osmw_hook_ui_active()) {
        g_api.capabilities |= OSMW_CAP_UI_TICK;
    }
    if (osmw_hook_packet_active()) {
        g_api.capabilities |= OSMW_CAP_GAME_PACKETS;
    }
    if (osmw_hook_ui_active() && osmw_dispatch_send_available()) {
        g_api.capabilities |= OSMW_CAP_SEND_QUEUE;
    }
    g_api.game_dir = osmw_api_game_dir;
    g_api.log = osmw_api_log;
    g_api.config_read_int = osmw_api_config_read_int;
    g_api.config_read_string = osmw_api_config_read_string;
    g_api.config_write_string = osmw_api_config_write_string;
    g_api.resolve_signature = osmw_api_resolve_signature;
    g_api.claim_region = osmw_api_claim_region;
    g_api.release_region = osmw_api_release_region;
    g_api.seal_region = osmw_api_seal_region;
    g_api.subscribe_post_ui_tick = api_subscribe_post_ui_tick;
    g_api.unsubscribe = api_unsubscribe;
    g_api.claim_game_packet = api_claim_game_packet;
    g_api.observe_game_packet_after = api_observe_game_packet_after;
    g_api.queue_game_packet = api_queue_game_packet;
    g_api.module_status = osmw_api_module_status;
}

const OSMW_CORE_API_V1 *osmw_loader_api(void)
{
    return g_loader_ready ? &g_api : NULL;
}

static BOOL read_file(const char *path, BYTE **data_out, DWORD *size_out)
{
    HANDLE file;
    DWORD size;
    DWORD read_size;
    BYTE *data;
    BOOL ok;

    *data_out = NULL;
    *size_out = 0;
    file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return FALSE;
    }
    SetLastError(NO_ERROR);
    size = GetFileSize(file, NULL);
    if (size == INVALID_FILE_SIZE && GetLastError() != NO_ERROR) {
        CloseHandle(file);
        return FALSE;
    }
    if (size == 0 || size > OSMW_LOADER_FILE_CAP) {
        CloseHandle(file);
        return FALSE;
    }
    data = (BYTE *)HeapAlloc(GetProcessHeap(), 0, size);
    if (data == NULL) {
        CloseHandle(file);
        return FALSE;
    }
    ok = ReadFile(file, data, size, &read_size, NULL);
    CloseHandle(file);
    if (!ok || read_size != size) {
        HeapFree(GetProcessHeap(), 0, data);
        return FALSE;
    }
    *data_out = data;
    *size_out = size;
    return TRUE;
}

static BOOL parse_pe_file(
    const BYTE *data, DWORD size, OSMW_PE_FILE_VIEW *view)
{
    const IMAGE_DOS_HEADER *dos;
    const IMAGE_NT_HEADERS32 *nt;
    const IMAGE_SECTION_HEADER *sections;
    DWORD nt_offset;
    DWORD optional_offset;
    DWORD section_offset;
    DWORD directory_end;
    WORD index;

    ZeroMemory(view, sizeof(*view));
    if (!range_valid(0, sizeof(IMAGE_DOS_HEADER), size)) {
        return FALSE;
    }
    dos = (const IMAGE_DOS_HEADER *)data;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0) {
        return FALSE;
    }
    nt_offset = (DWORD)dos->e_lfanew;
    if (!range_valid(nt_offset, sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER), size)) {
        return FALSE;
    }
    nt = (const IMAGE_NT_HEADERS32 *)(data + nt_offset);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
            (nt->FileHeader.Characteristics & IMAGE_FILE_DLL) == 0 ||
            nt->FileHeader.NumberOfSections == 0 ||
            nt->FileHeader.NumberOfSections > 96) {
        return FALSE;
    }
    optional_offset = nt_offset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER);
    if (!range_valid(optional_offset, nt->FileHeader.SizeOfOptionalHeader, size) ||
            nt->FileHeader.SizeOfOptionalHeader <
                offsetof(IMAGE_OPTIONAL_HEADER32, DataDirectory)) {
        return FALSE;
    }
    if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        return FALSE;
    }
    directory_end = (DWORD)offsetof(IMAGE_OPTIONAL_HEADER32, DataDirectory) +
        (IMAGE_DIRECTORY_ENTRY_EXPORT + 1u) * sizeof(IMAGE_DATA_DIRECTORY);
    if (nt->FileHeader.SizeOfOptionalHeader < directory_end ||
            nt->OptionalHeader.NumberOfRvaAndSizes <=
                IMAGE_DIRECTORY_ENTRY_EXPORT) {
        return FALSE;
    }
    section_offset = optional_offset + nt->FileHeader.SizeOfOptionalHeader;
    if (!range_valid(section_offset,
                     (DWORD)nt->FileHeader.NumberOfSections *
                         sizeof(IMAGE_SECTION_HEADER), size)) {
        return FALSE;
    }
    sections = (const IMAGE_SECTION_HEADER *)(data + section_offset);
    for (index = 0; index < nt->FileHeader.NumberOfSections; ++index) {
        if (sections[index].SizeOfRawData != 0 &&
                !range_valid(sections[index].PointerToRawData,
                             sections[index].SizeOfRawData, size)) {
            return FALSE;
        }
    }
    view->data = data;
    view->size = size;
    view->size_of_headers = nt->OptionalHeader.SizeOfHeaders;
    view->sections = sections;
    view->section_count = nt->FileHeader.NumberOfSections;
    view->export_rva = nt->OptionalHeader
        .DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
    view->export_size = nt->OptionalHeader
        .DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size;
    return TRUE;
}

static BOOL pe_rva_span(
    const OSMW_PE_FILE_VIEW *view, DWORD rva,
    const BYTE **pointer_out, DWORD *available_out)
{
    WORD index;

    if (rva < view->size_of_headers && rva < view->size) {
        DWORD available = view->size_of_headers - rva;
        if (available > view->size - rva) {
            available = view->size - rva;
        }
        *pointer_out = view->data + rva;
        *available_out = available;
        return TRUE;
    }
    for (index = 0; index < view->section_count; ++index) {
        const IMAGE_SECTION_HEADER *section = &view->sections[index];
        DWORD span = section->Misc.VirtualSize;
        DWORD delta;
        DWORD offset;
        if (span < section->SizeOfRawData) {
            span = section->SizeOfRawData;
        }
        if (rva < section->VirtualAddress) {
            continue;
        }
        delta = rva - section->VirtualAddress;
        if (delta >= span || delta >= section->SizeOfRawData) {
            continue;
        }
        offset = section->PointerToRawData + delta;
        if (offset < section->PointerToRawData || offset >= view->size) {
            return FALSE;
        }
        *pointer_out = view->data + offset;
        *available_out = section->SizeOfRawData - delta;
        if (*available_out > view->size - offset) {
            *available_out = view->size - offset;
        }
        return TRUE;
    }
    return FALSE;
}

static BOOL pe_rva_object(
    const OSMW_PE_FILE_VIEW *view, DWORD rva, DWORD length,
    const BYTE **pointer_out)
{
    DWORD available;
    if (!pe_rva_span(view, rva, pointer_out, &available)) {
        return FALSE;
    }
    return length <= available;
}

static BOOL pe_name_equals(
    const OSMW_PE_FILE_VIEW *view, DWORD rva, const char *expected)
{
    const BYTE *pointer;
    DWORD available;
    DWORD length = (DWORD)strlen(expected);

    if (!pe_rva_span(view, rva, &pointer, &available) ||
            available <= length) {
        return FALSE;
    }
    return memcmp(pointer, expected, length) == 0 && pointer[length] == '\0';
}

static OSMW_PREFLIGHT_RESULT preflight_query_export(const char *path)
{
    BYTE *data;
    DWORD size;
    OSMW_PE_FILE_VIEW view;
    const BYTE *directory_bytes;
    const BYTE *name_bytes;
    const IMAGE_EXPORT_DIRECTORY *directory;
    const DWORD *names;
    DWORD names_size;
    DWORD index;
    OSMW_PREFLIGHT_RESULT result = OSMW_PREFLIGHT_INVALID;

    if (!read_file(path, &data, &size)) {
        return OSMW_PREFLIGHT_INVALID;
    }
    if (!parse_pe_file(data, size, &view)) {
        goto done;
    }
    if (view.export_rva == 0 ||
            view.export_size < sizeof(IMAGE_EXPORT_DIRECTORY)) {
        result = OSMW_PREFLIGHT_NO_EXPORT;
        goto done;
    }
    if (!pe_rva_object(&view, view.export_rva,
                       sizeof(IMAGE_EXPORT_DIRECTORY), &directory_bytes)) {
        goto done;
    }
    directory = (const IMAGE_EXPORT_DIRECTORY *)directory_bytes;
    if (directory->NumberOfNames == 0) {
        result = OSMW_PREFLIGHT_NO_EXPORT;
        goto done;
    }
    if (directory->NumberOfNames > view.size / sizeof(DWORD)) {
        goto done;
    }
    names_size = directory->NumberOfNames * sizeof(DWORD);
    if (!pe_rva_object(&view, directory->AddressOfNames,
                       names_size, &name_bytes)) {
        goto done;
    }
    names = (const DWORD *)name_bytes;
    result = OSMW_PREFLIGHT_NO_EXPORT;
    for (index = 0; index < directory->NumberOfNames; ++index) {
        if (pe_name_equals(&view, names[index], OSMW_MOD_QUERY_EXPORT_NAME)) {
            result = OSMW_PREFLIGHT_OK;
            break;
        }
    }

done:
    HeapFree(GetProcessHeap(), 0, data);
    return result;
}

static BOOL copy_bounded_text(
    const char *source, char *destination, DWORD capacity, BOOL allow_empty)
{
    DWORD length;

    if (source == NULL || destination == NULL || capacity == 0) {
        return FALSE;
    }
    for (length = 0; length < capacity; ++length) {
        if (source[length] == '\0') {
            if (length == 0 && !allow_empty) {
                return FALSE;
            }
            memcpy(destination, source, length + 1);
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL valid_filename(const char *filename)
{
    DWORD length;
    const char *cursor;

    if (filename == NULL || filename[0] == '\0' ||
            strcmp(filename, ".") == 0 || strcmp(filename, "..") == 0 ||
            _stricmp(filename, "osmw_core.dll") == 0) {
        return FALSE;
    }
    for (cursor = filename; *cursor != '\0'; ++cursor) {
        if (*cursor == '\\' || *cursor == '/' || *cursor == ':') {
            return FALSE;
        }
    }
    length = (DWORD)(cursor - filename);
    if (length < 5 || length >= OSMW_MODULE_FILENAME_CAP) {
        return FALSE;
    }
    return _stricmp(filename + length - 4, ".dll") == 0;
}

static BOOL filename_already_listed(const char *filename)
{
    DWORD index;
    for (index = 0; index < g_module_count; ++index) {
        if (_stricmp(g_modules[index].filename, filename) == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

static OSMW_LOADER_RECORD *add_record(const char *filename)
{
    OSMW_LOADER_RECORD *record;

    if (g_module_count >= OSMW_LOADER_MODULE_CAP) {
        return NULL;
    }
    record = &g_modules[g_module_count++];
    ZeroMemory(record, sizeof(*record));
    record->last_result = OSMW_OK;
    strcpy(record->filename, filename);
    return record;
}

static void set_state(
    OSMW_LOADER_RECORD *record, OSMW_MODULE_STATE state,
    OSMW_RESULT result)
{
    EnterCriticalSection(&g_loader_lock);
    record->state = state;
    record->last_result = result;
    LeaveCriticalSection(&g_loader_lock);
}

static BOOL id_already_listed(
    const char *module_id, const OSMW_LOADER_RECORD *current)
{
    DWORD index;
    for (index = 0; index < g_module_count; ++index) {
        const OSMW_LOADER_RECORD *record = &g_modules[index];
        if (record != current && record->module_id[0] != '\0' &&
                strcmp(record->module_id, module_id) == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL descriptor_is_compatible(
    const OSMW_MOD_DESCRIPTOR_V1 *descriptor,
    char *module_id, char *display_name)
{
    DWORD required_major;
    DWORD required_minor;

    if (descriptor->struct_size < OSMW_MOD_DESCRIPTOR_V1_MIN_SIZE ||
            descriptor->flags != 0 || descriptor->init == NULL ||
            descriptor->shutdown == NULL ||
            !copy_bounded_text(descriptor->module_id, module_id,
                               OSMW_MODULE_ID_CAP, FALSE) ||
            !osmw_core_module_id_valid(module_id) ||
            !copy_bounded_text(descriptor->display_name, display_name,
                               OSMW_MODULE_DISPLAY_NAME_CAP, FALSE)) {
        return FALSE;
    }
    required_major = OSMW_ABI_MAJOR(descriptor->required_core_abi);
    required_minor = OSMW_ABI_MINOR(descriptor->required_core_abi);
    return required_major == OSMW_ABI_MAJOR(OSMW_CORE_ABI_CURRENT) &&
        required_minor <= OSMW_ABI_MINOR(OSMW_CORE_ABI_CURRENT);
}

static void unload_rejected(OSMW_LOADER_RECORD *record)
{
    if (record->module != NULL) {
        FreeLibrary(record->module);
        record->module = NULL;
    }
    record->init = NULL;
    record->shutdown = NULL;
}

static void process_module(OSMW_LOADER_RECORD *record)
{
    char path[MAX_PATH];
    int path_length;
    DWORD attributes;
    OSMW_PREFLIGHT_RESULT preflight;
    FARPROC query_address;
    OSMW_MOD_QUERY_V1 query;
    OSMW_MOD_DESCRIPTOR_V1 descriptor;
    OSMW_RESULT result;
    char module_id[OSMW_MODULE_ID_CAP];
    char display_name[OSMW_MODULE_DISPLAY_NAME_CAP];

    path_length = _snprintf(path, sizeof(path), "%s\\%s",
                            osmw_api_game_dir(), record->filename);
    if (path_length < 0 || (DWORD)path_length >= sizeof(path)) {
        set_state(record, OSMW_MODULE_INCOMPATIBLE,
                  OSMW_E_BUFFER_TOO_SMALL);
        loader_log(OSMW_LOG_ERROR, "module path is too long: %s",
                   record->filename);
        return;
    }
    attributes = GetFileAttributesA(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        set_state(record, OSMW_MODULE_MISSING, OSMW_E_NOT_FOUND);
        loader_log(OSMW_LOG_WARN, "module is missing: %s", record->filename);
        return;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        set_state(record, OSMW_MODULE_INCOMPATIBLE,
                  OSMW_E_INVALID_ARGUMENT);
        loader_log(OSMW_LOG_ERROR, "module path is not a file: %s",
                   record->filename);
        return;
    }

    preflight = preflight_query_export(path);
    if (preflight == OSMW_PREFLIGHT_NO_EXPORT) {
        set_state(record, OSMW_MODULE_LEGACY, OSMW_E_INCOMPATIBLE_ABI);
        loader_log(OSMW_LOG_WARN,
                   "legacy module rejected before LoadLibrary: %s",
                   record->filename);
        return;
    }
    if (preflight != OSMW_PREFLIGHT_OK) {
        set_state(record, OSMW_MODULE_INCOMPATIBLE, OSMW_E_INTERNAL);
        loader_log(OSMW_LOG_ERROR, "invalid PE module rejected: %s",
                   record->filename);
        return;
    }

    record->module = LoadLibraryA(path);
    if (record->module == NULL) {
        set_state(record, OSMW_MODULE_FAILED, OSMW_E_INTERNAL);
        loader_log(OSMW_LOG_ERROR, "LoadLibrary failed for %s (error %lu)",
                   record->filename, (unsigned long)GetLastError());
        return;
    }
    query_address = GetProcAddress(record->module, OSMW_MOD_QUERY_EXPORT_NAME);
    if (query_address == NULL) {
        set_state(record, OSMW_MODULE_INCOMPATIBLE,
                  OSMW_E_INCOMPATIBLE_ABI);
        unload_rejected(record);
        loader_log(OSMW_LOG_ERROR,
                   "preflighted query export was not loadable: %s",
                   record->filename);
        return;
    }
    query = (OSMW_MOD_QUERY_V1)query_address;
    ZeroMemory(&descriptor, sizeof(descriptor));
    result = query(OSMW_CORE_ABI_CURRENT, sizeof(descriptor), &descriptor);
    if (result != OSMW_OK ||
            !descriptor_is_compatible(&descriptor, module_id, display_name)) {
        set_state(record, OSMW_MODULE_INCOMPATIBLE,
                  result != OSMW_OK ? result : OSMW_E_INCOMPATIBLE_ABI);
        unload_rejected(record);
        loader_log(OSMW_LOG_WARN, "incompatible module rejected: %s",
                   record->filename);
        return;
    }

    strcpy(record->module_id, module_id);
    strcpy(record->display_name, display_name);
    record->module_version = descriptor.module_version;
    record->required_core_abi = descriptor.required_core_abi;
    if (id_already_listed(module_id, record)) {
        set_state(record, OSMW_MODULE_INCOMPATIBLE, OSMW_E_DUPLICATE_ID);
        unload_rejected(record);
        loader_log(OSMW_LOG_ERROR,
                   "duplicate module id '%s' rejected from %s",
                   module_id, record->filename);
        return;
    }

    record->owner = osmw_core_owner_register(module_id);
    if (record->owner == OSMW_INVALID_OWNER) {
        set_state(record, OSMW_MODULE_FAILED, OSMW_E_INTERNAL);
        unload_rejected(record);
        loader_log(OSMW_LOG_ERROR, "could not issue owner for module %s",
                   module_id);
        return;
    }
    record->init = descriptor.init;
    record->shutdown = descriptor.shutdown;
    set_state(record, OSMW_MODULE_LOADED, OSMW_OK);
    result = record->init(&g_api, record->owner);
    if (result == OSMW_OK) {
        set_state(record, OSMW_MODULE_ACTIVE, OSMW_OK);
        loader_log(OSMW_LOG_INFO, "module active: %s (%s)",
                   module_id, record->filename);
        return;
    }
    if (result == OSMW_MOD_DISABLED &&
            osmw_core_owner_resource_count(record->owner) == 0) {
        set_state(record, OSMW_MODULE_DISABLED, OSMW_MOD_DISABLED);
        loader_log(OSMW_LOG_INFO, "module disabled by configuration: %s",
                   module_id);
        return;
    }

    if (osmw_core_owner_resource_count(record->owner) != 0) {
        set_state(record, OSMW_MODULE_UNSAFE_TO_UNLOAD,
                  result != OSMW_MOD_DISABLED
                      ? result : OSMW_E_PATCH_STILL_ACTIVE);
        loader_log(OSMW_LOG_ERROR,
                   "failed module retained because resources remain: %s",
                   module_id);
        return;
    }
    set_state(record, OSMW_MODULE_FAILED, result);
    (void)osmw_core_owner_unregister(record->owner);
    record->owner = OSMW_INVALID_OWNER;
    unload_rejected(record);
    loader_log(OSMW_LOG_ERROR, "module init failed cleanly: %s (%ld)",
               module_id, (long)result);
}

static char *trim_token(char *token)
{
    char *end;
    while (*token == ' ' || *token == '\t') {
        ++token;
    }
    end = token + strlen(token);
    while (end > token && (end[-1] == ' ' || end[-1] == '\t')) {
        --end;
    }
    *end = '\0';
    return token;
}

BOOL osmw_loader_start(OSMW_OWNER core_owner)
{
    char manifest[OSMW_LOADER_MANIFEST_CAP];
    char *token;
    char *next;
    OSMW_RESULT result;

    if (g_loader_ready) {
        return TRUE;
    }
    if (!owner_is_valid(core_owner)) {
        return FALSE;
    }
    ZeroMemory(g_modules, sizeof(g_modules));
    g_module_count = 0;
    g_loader_core_owner = core_owner;
    InitializeCriticalSection(&g_loader_lock);
    g_loader_lock_ready = TRUE;
    initialise_api_table();
    g_loader_ready = TRUE;

    result = osmw_api_config_read_string(
        core_owner, "Core", "Modules", OSMW_DEFAULT_MODULES,
        manifest, sizeof(manifest));
    if (result != OSMW_OK) {
        loader_log(OSMW_LOG_ERROR,
                   "[Core] Modules is too long or could not be read");
        osmw_loader_reset();
        return FALSE;
    }

    token = manifest;
    for (;;) {
        OSMW_LOADER_RECORD *record;
        next = strchr(token, ',');
        if (next != NULL) {
            *next = '\0';
        }
        token = trim_token(token);
        if (token[0] != '\0') {
            if (!valid_filename(token)) {
                loader_log(OSMW_LOG_ERROR,
                           "invalid module manifest entry rejected: %s",
                           token);
            } else if (filename_already_listed(token)) {
                loader_log(OSMW_LOG_ERROR,
                           "duplicate module filename rejected: %s", token);
            } else {
                record = add_record(token);
                if (record == NULL) {
                    loader_log(OSMW_LOG_ERROR,
                               "module manifest exceeds %lu entries",
                               (unsigned long)OSMW_LOADER_MODULE_CAP);
                    break;
                }
                process_module(record);
            }
        }
        if (next == NULL) {
            break;
        }
        token = next + 1;
    }
    loader_log(OSMW_LOG_INFO, "module manifest complete: %lu entr%s",
               (unsigned long)g_module_count,
               g_module_count == 1 ? "y" : "ies");
    return TRUE;
}

BOOL osmw_loader_shutdown(void)
{
    LONG index;
    BOOL safe = TRUE;

    if (!g_loader_ready) {
        return TRUE;
    }
    for (index = (LONG)g_module_count - 1; index >= 0; --index) {
        OSMW_LOADER_RECORD *record = &g_modules[index];
        OSMW_RESULT result;
        if (record->state != OSMW_MODULE_ACTIVE &&
                record->state != OSMW_MODULE_DISABLED &&
                record->state != OSMW_MODULE_UNSAFE_TO_UNLOAD) {
            continue;
        }
        set_state(record, OSMW_MODULE_STOPPING, OSMW_OK);
        result = record->shutdown(&g_api, record->owner);
        if (result != OSMW_OK ||
                osmw_core_owner_resource_count(record->owner) != 0) {
            set_state(record, OSMW_MODULE_UNSAFE_TO_UNLOAD,
                      result != OSMW_OK
                          ? result : OSMW_E_PATCH_STILL_ACTIVE);
            loader_log(OSMW_LOG_ERROR,
                       "module cannot be unloaded safely: %s (%ld)",
                       record->module_id, (long)record->last_result);
            osmw_claims_log_owner(record->owner);
            safe = FALSE;
            continue;
        }
        /* STOPPING also means "shutdown completed, mapping retained".  Do not
         * unregister an owner or unload one clean module until every module
         * has proved clean.  A later refusal must leave the complete managed
         * dependency chain mapped so the caller can safely retry. */
        set_state(record, OSMW_MODULE_STOPPING, OSMW_OK);
    }
    if (!safe) {
        return FALSE;
    }

    /* Validate the whole commit set before the first irreversible
     * FreeLibrary.  Records already in STOPPING are successful work from an
     * earlier refused attempt and deliberately do not receive shutdown twice. */
    for (index = (LONG)g_module_count - 1; index >= 0; --index) {
        OSMW_LOADER_RECORD *record = &g_modules[index];
        if (record->state != OSMW_MODULE_STOPPING) {
            continue;
        }
        if (record->module == NULL ||
                record->owner == OSMW_INVALID_OWNER ||
                !owner_is_valid(record->owner) ||
                osmw_core_owner_resource_count(record->owner) != 0) {
            set_state(record, OSMW_MODULE_UNSAFE_TO_UNLOAD,
                      OSMW_E_PATCH_STILL_ACTIVE);
            loader_log(OSMW_LOG_ERROR,
                       "module failed final unload audit: %s",
                       record->module_id);
            osmw_claims_log_owner(record->owner);
            return FALSE;
        }
    }
    if (osmw_dispatch_total_count() != 0 ||
            osmw_claims_total_count() !=
                osmw_claims_owner_count(g_loader_core_owner)) {
        loader_log(OSMW_LOG_ERROR,
                   "managed unload audit found orphaned resources");
        return FALSE;
    }

    /* Every managed mapping is now known clean.  Commit the unload in reverse
     * initialisation order. */
    for (index = (LONG)g_module_count - 1; index >= 0; --index) {
        OSMW_LOADER_RECORD *record = &g_modules[index];
        if (record->state != OSMW_MODULE_STOPPING) {
            continue;
        }
        (void)osmw_core_owner_unregister(record->owner);
        record->owner = OSMW_INVALID_OWNER;
        FreeLibrary(record->module);
        record->module = NULL;
        record->init = NULL;
        record->shutdown = NULL;
        set_state(record, OSMW_MODULE_UNLOADED, OSMW_OK);
    }
    return TRUE;
}

void osmw_loader_reset(void)
{
    if (!g_loader_lock_ready) {
        return;
    }
    ZeroMemory(g_modules, sizeof(g_modules));
    g_module_count = 0;
    g_loader_ready = FALSE;
    ZeroMemory(&g_api, sizeof(g_api));
    g_loader_core_owner = OSMW_INVALID_OWNER;
    DeleteCriticalSection(&g_loader_lock);
    g_loader_lock_ready = FALSE;
}

DWORD osmw_loader_module_count(void)
{
    DWORD count;
    if (!g_loader_lock_ready) {
        return 0;
    }
    EnterCriticalSection(&g_loader_lock);
    count = g_module_count;
    LeaveCriticalSection(&g_loader_lock);
    return count;
}

/* Read one loader record by position, for callers that need the whole manifest rather
 * than a single named module.  Same snapshot shape as module_status so there is one
 * struct to reason about. */
OSMW_RESULT osmw_loader_module_at(DWORD index, OSMW_MODULE_STATUS_V1 *output)
{
    const OSMW_LOADER_RECORD *record;

    if (output == NULL || output->struct_size < sizeof(OSMW_MODULE_STATUS_V1)) {
        return OSMW_E_INVALID_ARGUMENT;
    }
    if (index >= g_module_count) {
        return OSMW_E_NOT_FOUND;
    }
    record = &g_modules[index];

    output->state = record->state;
    output->module_version = record->module_version;
    output->required_core_abi = record->required_core_abi;
    output->owned_claim_count = osmw_claims_owner_count(record->owner);
    output->subscription_count = osmw_dispatch_owner_count(record->owner);
    output->last_result = record->last_result;
    output->reserved = 0;
    lstrcpynA(output->module_id, record->module_id, OSMW_MODULE_ID_CAP);
    lstrcpynA(output->filename, record->filename, OSMW_MODULE_FILENAME_CAP);
    lstrcpynA(output->display_name, record->display_name,
              OSMW_MODULE_DISPLAY_NAME_CAP);
    return OSMW_OK;
}
OSMW_RESULT __cdecl osmw_api_module_status(
    const char *module_id_or_filename, OSMW_MODULE_STATUS_V1 *output)
{
    DWORD index;
    DWORD supplied_size;

    if (!g_loader_ready || module_id_or_filename == NULL ||
            module_id_or_filename[0] == '\0' || output == NULL) {
        return OSMW_E_INVALID_ARGUMENT;
    }
    supplied_size = output->struct_size;
    if (supplied_size < sizeof(*output)) {
        return OSMW_E_BUFFER_TOO_SMALL;
    }
    EnterCriticalSection(&g_loader_lock);
    for (index = 0; index < g_module_count; ++index) {
        OSMW_LOADER_RECORD *record = &g_modules[index];
        if ((record->module_id[0] != '\0' &&
                strcmp(record->module_id, module_id_or_filename) == 0) ||
                _stricmp(record->filename, module_id_or_filename) == 0) {
            ZeroMemory(output, sizeof(*output));
            output->struct_size = sizeof(*output);
            output->state = record->state;
            output->module_version = record->module_version;
            output->required_core_abi = record->required_core_abi;
            output->owned_claim_count =
                osmw_claims_owner_count(record->owner);
            output->subscription_count =
                osmw_dispatch_owner_count(record->owner);
            output->last_result = record->last_result;
            strcpy(output->module_id, record->module_id);
            strcpy(output->filename, record->filename);
            strcpy(output->display_name, record->display_name);
            LeaveCriticalSection(&g_loader_lock);
            return OSMW_OK;
        }
    }
    LeaveCriticalSection(&g_loader_lock);
    return OSMW_E_NOT_FOUND;
}
