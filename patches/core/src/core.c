/* Managed core service bootstrap.
 *
 * Config, logging, pristine signatures, exclusive runtime claims and the
 * core-owned UiTick/packet brokers are operational.
 */
#include "core_internal.h"

#include <stdio.h>
#include <string.h>

#define OSMW_CORE_OWNER_CAP 32

struct osmw_core_owner_record {
    OSMW_OWNER handle;
    char module_id[OSMW_MODULE_ID_CAP];
};

static volatile LONG g_stop_requested;
static volatile LONG g_services_started;
static volatile LONG g_next_owner;
static BOOL g_owners_ready;
static CRITICAL_SECTION g_owner_lock;
static struct osmw_core_owner_record g_owners[OSMW_CORE_OWNER_CAP];
static OSMW_OWNER g_core_owner;

BOOL osmw_core_module_id_valid(const char *module_id)
{
    DWORD length = 0;

    if (module_id == NULL || module_id[0] == '\0') {
        return FALSE;
    }
    while (module_id[length] != '\0') {
        char ch = module_id[length];
        if (!((ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')) {
            return FALSE;
        }
        ++length;
        if (length >= OSMW_MODULE_ID_CAP) {
            return FALSE;
        }
    }
    return TRUE;
}

BOOL osmw_core_owners_init(void)
{
    if (g_owners_ready) {
        return TRUE;
    }
    ZeroMemory(g_owners, sizeof(g_owners));
    InitializeCriticalSection(&g_owner_lock);
    g_owners_ready = TRUE;
    return TRUE;
}

void osmw_core_owners_shutdown(void)
{
    if (!g_owners_ready) {
        return;
    }
    EnterCriticalSection(&g_owner_lock);
    ZeroMemory(g_owners, sizeof(g_owners));
    LeaveCriticalSection(&g_owner_lock);
    DeleteCriticalSection(&g_owner_lock);
    g_owners_ready = FALSE;
    InterlockedExchange(&g_next_owner, 0);
}

OSMW_OWNER osmw_core_owner_register(const char *module_id)
{
    DWORD index;
    OSMW_OWNER handle = OSMW_INVALID_OWNER;

    if (!g_owners_ready || !osmw_core_module_id_valid(module_id)) {
        return OSMW_INVALID_OWNER;
    }

    EnterCriticalSection(&g_owner_lock);
    for (index = 0; index < OSMW_CORE_OWNER_CAP; ++index) {
        if (g_owners[index].handle != OSMW_INVALID_OWNER &&
                strcmp(g_owners[index].module_id, module_id) == 0) {
            LeaveCriticalSection(&g_owner_lock);
            return OSMW_INVALID_OWNER;
        }
    }
    for (index = 0; index < OSMW_CORE_OWNER_CAP; ++index) {
        if (g_owners[index].handle == OSMW_INVALID_OWNER) {
            do {
                handle = (OSMW_OWNER)InterlockedIncrement(&g_next_owner);
            } while (handle == OSMW_INVALID_OWNER);
            g_owners[index].handle = handle;
            strcpy(g_owners[index].module_id, module_id);
            break;
        }
    }
    LeaveCriticalSection(&g_owner_lock);
    return handle;
}

BOOL osmw_core_owner_unregister(OSMW_OWNER owner)
{
    DWORD index;

    if (!g_owners_ready || owner == OSMW_INVALID_OWNER) {
        return FALSE;
    }
    EnterCriticalSection(&g_owner_lock);
    for (index = 0; index < OSMW_CORE_OWNER_CAP; ++index) {
        if (g_owners[index].handle == owner) {
            ZeroMemory(&g_owners[index], sizeof(g_owners[index]));
            LeaveCriticalSection(&g_owner_lock);
            return TRUE;
        }
    }
    LeaveCriticalSection(&g_owner_lock);
    return FALSE;
}

BOOL osmw_core_owner_copy_id(OSMW_OWNER owner, char *output, DWORD output_size)
{
    DWORD index;
    BOOL found = FALSE;

    if (!g_owners_ready || owner == OSMW_INVALID_OWNER ||
            output == NULL || output_size == 0) {
        return FALSE;
    }
    output[0] = '\0';
    EnterCriticalSection(&g_owner_lock);
    for (index = 0; index < OSMW_CORE_OWNER_CAP; ++index) {
        if (g_owners[index].handle == owner) {
            size_t length = strlen(g_owners[index].module_id);
            if (length + 1 <= output_size) {
                memcpy(output, g_owners[index].module_id, length + 1);
                found = TRUE;
            }
            break;
        }
    }
    LeaveCriticalSection(&g_owner_lock);
    return found;
}

DWORD osmw_core_owner_resource_count(OSMW_OWNER owner)
{
    return osmw_claims_owner_count(owner) +
        osmw_dispatch_owner_count(owner);
}

void osmw_core_request_stop(void)
{
    InterlockedExchange(&g_stop_requested, 1);
}

BOOL osmw_core_stop_requested(void)
{
    return InterlockedCompareExchange(&g_stop_requested, 0, 0) != 0;
}

BOOL osmw_core_services_startup(const char *game_dir_override)
{
    OSMW_IMAGE_VIEW image;
    BOOL have_config;
    BOOL logging_enabled;
    OSMW_RESULT hook_result;
    char message[320];
    int length;

    if (InterlockedCompareExchange(&g_services_started, 1, 0) != 0) {
        return TRUE;
    }
    if (!osmw_core_owners_init()) {
        InterlockedExchange(&g_services_started, 0);
        return FALSE;
    }
    g_core_owner = osmw_core_owner_register("core");
    if (g_core_owner == OSMW_INVALID_OWNER) {
        osmw_core_owners_shutdown();
        InterlockedExchange(&g_services_started, 0);
        return FALSE;
    }
    if (!osmw_claims_init()) {
        (void)osmw_core_owner_unregister(g_core_owner);
        g_core_owner = OSMW_INVALID_OWNER;
        osmw_core_owners_shutdown();
        InterlockedExchange(&g_services_started, 0);
        return FALSE;
    }
    if (!osmw_hook_init() || !osmw_dispatch_init()) {
        osmw_dispatch_reset();
        osmw_hook_reset();
        osmw_claims_reset();
        (void)osmw_core_owner_unregister(g_core_owner);
        g_core_owner = OSMW_INVALID_OWNER;
        osmw_core_owners_shutdown();
        InterlockedExchange(&g_services_started, 0);
        return FALSE;
    }

    have_config = game_dir_override != NULL
        ? osmw_config_init_for_dir(game_dir_override)
        : osmw_config_init();
    if (!have_config) {
        OutputDebugStringA("osmw_core: cannot resolve the process directory\n");
        osmw_dispatch_reset();
        osmw_hook_reset();
        osmw_claims_reset();
        (void)osmw_core_owner_unregister(g_core_owner);
        g_core_owner = OSMW_INVALID_OWNER;
        osmw_core_owners_shutdown();
        InterlockedExchange(&g_services_started, 0);
        return FALSE;
    }

    logging_enabled = osmw_api_config_read_int(
        g_core_owner, "Debug", "ModLogging", 0) != 0;
    if (!osmw_log_start(osmw_api_game_dir(), logging_enabled) &&
            logging_enabled) {
        OutputDebugStringA("osmw_core: configured log could not be opened\n");
    }

    if (!osmw_image_load_process()) {
        length = _snprintf(message, sizeof(message),
                           "pristine image unavailable: %s",
                           osmw_image_last_error());
        if (length < 0) {
            message[sizeof(message) - 1] = '\0';
        }
        osmw_api_log(g_core_owner, OSMW_LOG_ERROR, message);
    } else if (!osmw_scan_start() || !osmw_image_get_view(&image)) {
        osmw_api_log(g_core_owner, OSMW_LOG_ERROR,
                     "signature resolver could not start");
    } else {
        length = _snprintf(
            message, sizeof(message),
            "pristine image ready: .text RVA 0x%08lX, %lu bytes, build 0x%08lX",
            (unsigned long)image.text_rva, (unsigned long)image.text_size,
            (unsigned long)image.timestamp);
        if (length < 0) {
            message[sizeof(message) - 1] = '\0';
        }
        osmw_api_log(g_core_owner, OSMW_LOG_INFO, message);
        if (!image.address_compatible) {
            length = _snprintf(
                message, sizeof(message),
                "address services unavailable: loaded base 0x%08lX differs "
                "from preferred base 0x%08lX",
                (unsigned long)image.loaded_base,
                (unsigned long)image.preferred_base);
            if (length < 0) {
                message[sizeof(message) - 1] = '\0';
            }
            osmw_api_log(g_core_owner, OSMW_LOG_ERROR, message);
        }
    }
    (void)osmw_hook_resolve_shared(g_core_owner);
    hook_result = osmw_hook_start_ui(g_core_owner);
    if (hook_result == OSMW_OK) {
        osmw_api_log(g_core_owner, OSMW_LOG_INFO,
                     "post-UiTick broker active");
    } else {
        length = _snprintf(
            message, sizeof(message),
            "post-UiTick broker unavailable; stock client retained (%ld)",
            (long)hook_result);
        if (length < 0) {
            message[sizeof(message) - 1] = '\0';
        }
        osmw_api_log(g_core_owner, OSMW_LOG_WARN, message);
    }
    hook_result = osmw_hook_start_packet(g_core_owner);
    if (hook_result == OSMW_OK) {
        osmw_api_log(g_core_owner, OSMW_LOG_INFO,
                     "game-packet broker active");
    } else {
        length = _snprintf(
            message, sizeof(message),
            "game-packet broker unavailable; stock client retained (%ld)",
            (long)hook_result);
        if (length < 0) {
            message[sizeof(message) - 1] = '\0';
        }
        osmw_api_log(g_core_owner, OSMW_LOG_WARN, message);
    }
    osmw_dispatch_set_send_target(
        osmw_hook_shared_address(OSMW_SHARED_SEND_PACKET));
    osmw_api_log(
        g_core_owner,
        osmw_hook_ui_active() && osmw_dispatch_send_available()
            ? OSMW_LOG_INFO : OSMW_LOG_WARN,
        osmw_hook_ui_active() && osmw_dispatch_send_available()
            ? "game-packet send queue active"
            : "game-packet send queue unavailable");
    if (!osmw_loader_start(g_core_owner)) {
        osmw_api_log(g_core_owner, OSMW_LOG_ERROR,
                     "module manifest loader could not start");
        osmw_dispatch_begin_stop();
        if (!osmw_dispatch_wait_quiescent(5000u)) {
            osmw_api_log(g_core_owner, OSMW_LOG_ERROR,
                         "startup cleanup refused: hooks did not quiesce");
            return FALSE;
        }
        hook_result = osmw_hook_stop_packet();
        if (hook_result != OSMW_OK) {
            osmw_api_log(g_core_owner, OSMW_LOG_ERROR,
                         "startup cleanup refused: packet hook remains");
            return FALSE;
        }
        hook_result = osmw_hook_stop_ui();
        if (hook_result != OSMW_OK) {
            osmw_api_log(g_core_owner, OSMW_LOG_ERROR,
                         "startup cleanup refused: UiTick hook remains");
            return FALSE;
        }
        osmw_dispatch_set_send_target(NULL);
        osmw_loader_reset();
        osmw_scan_reset();
        osmw_image_reset();
        osmw_log_close();
        osmw_config_reset();
        osmw_dispatch_reset();
        osmw_hook_reset();
        osmw_claims_reset();
        (void)osmw_core_owner_unregister(g_core_owner);
        g_core_owner = OSMW_INVALID_OWNER;
        osmw_core_owners_shutdown();
        InterlockedExchange(&g_services_started, 0);
        return FALSE;
    }
    /* Last, so the first report describes a fully-loaded manifest rather than a
     * half-populated one.  Optional: a core that cannot subscribe still works. */
    osmw_api_log(g_core_owner, OSMW_LOG_INFO,
                 osmw_attest_start(g_core_owner)
                     ? "client attestation active"
                     : "client attestation unavailable");
    osmw_api_log(g_core_owner, OSMW_LOG_INFO,
                 "startup complete; manifest processed; capabilities published");
    return TRUE;
}

DWORD WINAPI osmw_core_startup_worker(LPVOID unused)
{
    (void)unused;

    if (!osmw_core_services_startup(NULL)) {
        return 0;
    }

    if (osmw_core_stop_requested()) {
        osmw_api_log(g_core_owner, OSMW_LOG_INFO,
                     "stop observed during bounded startup");
    }
    return 0;
}

BOOL osmw_core_shutdown(void)
{
    OSMW_RESULT hook_result;

    if (InterlockedCompareExchange(&g_services_started, 1, 1) == 0) {
        return TRUE;
    }
    if (g_core_owner != OSMW_INVALID_OWNER) {
        osmw_api_log(g_core_owner, OSMW_LOG_INFO,
                     "managed module shutdown starting");
    }
    /* Before the dispatcher stops: releasing the subscription first means the tick
     * callback cannot be mid-flight while its own state is torn down. */
    osmw_attest_stop();
    osmw_dispatch_begin_stop();
    if (!osmw_dispatch_wait_quiescent(5000u)) {
        osmw_api_log(g_core_owner, OSMW_LOG_ERROR,
                     "shutdown refused: callbacks did not quiesce");
        return FALSE;
    }
    if (!osmw_loader_shutdown()) {
        osmw_api_log(g_core_owner, OSMW_LOG_ERROR,
                     "shutdown refused: a module still owns live resources");
        return FALSE;
    }
    if (osmw_dispatch_total_count() != 0) {
        osmw_api_log(g_core_owner, OSMW_LOG_ERROR,
                     "shutdown refused: orphaned callbacks remain");
        return FALSE;
    }
    hook_result = osmw_hook_stop_packet();
    if (hook_result != OSMW_OK) {
        osmw_api_log(g_core_owner, OSMW_LOG_ERROR,
                     "shutdown refused: packet hook could not be removed");
        return FALSE;
    }
    hook_result = osmw_hook_stop_ui();
    if (hook_result != OSMW_OK) {
        osmw_api_log(g_core_owner, OSMW_LOG_ERROR,
                     "shutdown refused: UiTick hook could not be removed");
        return FALSE;
    }
    if (osmw_claims_total_count() != 0) {
        osmw_api_log(g_core_owner, OSMW_LOG_ERROR,
                     "shutdown refused: orphaned runtime claims remain");
        return FALSE;
    }
    osmw_dispatch_set_send_target(NULL);
    InterlockedExchange(&g_services_started, 0);
    osmw_loader_reset();
    osmw_dispatch_reset();
    osmw_hook_reset();
    if (g_core_owner != OSMW_INVALID_OWNER) {
        osmw_api_log(g_core_owner, OSMW_LOG_INFO, "shutdown complete");
    }
    osmw_scan_reset();
    osmw_image_reset();
    osmw_claims_reset();
    osmw_log_close();
    osmw_config_reset();
    if (g_core_owner != OSMW_INVALID_OWNER) {
        (void)osmw_core_owner_unregister(g_core_owner);
        g_core_owner = OSMW_INVALID_OWNER;
    }
    osmw_core_owners_shutdown();
    return TRUE;
}
