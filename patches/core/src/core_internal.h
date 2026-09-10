#ifndef OSMW_CORE_INTERNAL_H
#define OSMW_CORE_INTERNAL_H

#include "osmw_core_api.h"

#include <windows.h>

/* Core state/owner helpers.  The loader will use this registry when it begins
 * issuing module owners in Task 5; keeping validation here means services
 * never need to trust a caller-supplied tag. */
BOOL osmw_core_owners_init(void);
void osmw_core_owners_shutdown(void);
OSMW_OWNER osmw_core_owner_register(const char *module_id);
BOOL osmw_core_owner_unregister(OSMW_OWNER owner);
BOOL osmw_core_owner_copy_id(OSMW_OWNER owner, char *output, DWORD output_size);
BOOL osmw_core_module_id_valid(const char *module_id);
DWORD osmw_core_owner_resource_count(OSMW_OWNER owner);

/* Exclusive runtime-memory claim registry.  The same entry points are used by
 * feature owners and the core owner so shared hook sites cannot be bypassed. */
BOOL osmw_claims_init(void);
void osmw_claims_reset(void);
DWORD osmw_claims_owner_count(OSMW_OWNER owner);
DWORD osmw_claims_total_count(void);
void osmw_claims_log_owner(OSMW_OWNER owner);
DWORD osmw_claims_tampered_count(void);
DWORD osmw_claims_sealed_count(void);
OSMW_RESULT __cdecl osmw_api_claim_region(
    OSMW_OWNER owner, const OSMW_REGION_CLAIM_V1 *claim,
    OSMW_TOKEN *token);
OSMW_RESULT __cdecl osmw_api_release_region(
    OSMW_OWNER owner, OSMW_TOKEN token);
OSMW_RESULT __cdecl osmw_api_seal_region(
    OSMW_OWNER owner, OSMW_TOKEN token);

/* Core hook writer and the three shared addresses resolved from the pristine
 * image.  The core owns the concrete UiTick and HandlePacket detours. */
#define OSMW_HOOK_MAX_PROLOGUE 32u
typedef struct OSMW_HOOK {
    BYTE *target;
    BYTE *trampoline;
    DWORD trampoline_size;
    DWORD length;
    OSMW_OWNER owner;
    OSMW_TOKEN claim_token;
    BOOL patch_installed;
    BYTE saved[OSMW_HOOK_MAX_PROLOGUE];
    BYTE patch[OSMW_HOOK_MAX_PROLOGUE];
} OSMW_HOOK;

typedef DWORD OSMW_SHARED_ADDRESS;
#define OSMW_SHARED_UI_TICK       ((OSMW_SHARED_ADDRESS)0u)
#define OSMW_SHARED_HANDLE_PACKET ((OSMW_SHARED_ADDRESS)1u)
#define OSMW_SHARED_SEND_PACKET   ((OSMW_SHARED_ADDRESS)2u)
#define OSMW_SHARED_ADDRESS_COUNT 3u

BOOL osmw_hook_init(void);
void osmw_hook_reset(void);
BOOL osmw_hook_resolve_shared(OSMW_OWNER core_owner);
void *osmw_hook_shared_address(OSMW_SHARED_ADDRESS which);
DWORD osmw_hook_shared_length(OSMW_SHARED_ADDRESS which);
OSMW_RESULT osmw_hook_install(
    OSMW_OWNER owner, OSMW_HOOK *hook, void *target, void *detour,
    const BYTE *expected, DWORD length, const char *label);
OSMW_RESULT osmw_hook_install_shared(
    OSMW_OWNER owner, OSMW_SHARED_ADDRESS which, OSMW_HOOK *hook,
    void *detour, const char *label);
OSMW_RESULT osmw_hook_remove(OSMW_HOOK *hook);
BOOL osmw_hook_is_active(const OSMW_HOOK *hook);
OSMW_RESULT osmw_hook_install_ui_target(
    OSMW_OWNER owner, void *target, const BYTE *expected, DWORD length);
OSMW_RESULT osmw_hook_start_ui(OSMW_OWNER owner);
OSMW_RESULT osmw_hook_stop_ui(void);
BOOL osmw_hook_ui_active(void);
OSMW_RESULT osmw_hook_install_packet_target(
    OSMW_OWNER owner, void *target, const BYTE *expected, DWORD length);
OSMW_RESULT osmw_hook_start_packet(OSMW_OWNER owner);
OSMW_RESULT osmw_hook_stop_packet(void);
BOOL osmw_hook_packet_active(void);

/* Post-UiTick callback registry.  The concrete hook above calls the stock
 * trampoline once and then enters this dispatcher. */
typedef void (__cdecl *OSMW_UI_ORIGINAL_FN)(LONG list_selector);
typedef void (__stdcall *OSMW_PACKET_ORIGINAL_FN)(
    DWORD type, DWORD field12, const void *extra, DWORD extra_size);
typedef int (__cdecl *OSMW_SEND_PACKET_FN)(
    DWORD type, DWORD field12, const void *extra, DWORD extra_size);
BOOL osmw_dispatch_init(void);
void osmw_dispatch_reset(void);
OSMW_RESULT osmw_dispatch_subscribe_ui(
    OSMW_OWNER owner, LONG priority, OSMW_UI_TICK_CALLBACK_V1 callback,
    void *context, OSMW_TOKEN *token);
OSMW_RESULT osmw_dispatch_unsubscribe(
    OSMW_OWNER owner, OSMW_TOKEN token);
OSMW_RESULT osmw_dispatch_claim_packet(
    OSMW_OWNER owner, DWORD packet_type, OSMW_PACKET_HANDLER_V1 callback,
    void *context, OSMW_TOKEN *token);
OSMW_RESULT osmw_dispatch_observe_packet(
    OSMW_OWNER owner, DWORD packet_type, LONG priority,
    OSMW_PACKET_OBSERVER_V1 callback, void *context, OSMW_TOKEN *token);
OSMW_RESULT osmw_dispatch_queue_packet(
    OSMW_OWNER owner, DWORD packet_type, DWORD field12,
    const void *extra, DWORD extra_size);
DWORD osmw_dispatch_owner_count(OSMW_OWNER owner);
DWORD osmw_dispatch_total_count(void);
void osmw_dispatch_ui_invoke(
    OSMW_UI_ORIGINAL_FN original, LONG list_selector);
void osmw_dispatch_packet_invoke(
    OSMW_PACKET_ORIGINAL_FN original, DWORD type, DWORD field12,
    const void *extra, DWORD extra_size);
void osmw_dispatch_set_send_target(void *target);
BOOL osmw_dispatch_send_available(void);
void osmw_dispatch_begin_stop(void);
void osmw_dispatch_resume(void);
BOOL osmw_dispatch_wait_quiescent(DWORD timeout_ms);

/* Config service and ABI-shaped entry points. */
BOOL osmw_config_init(void);
BOOL osmw_config_init_for_dir(const char *game_dir);
void osmw_config_reset(void);
const char *__cdecl osmw_api_game_dir(void);
int __cdecl osmw_api_config_read_int(
    OSMW_OWNER owner, const char *section, const char *key, int fallback);
OSMW_RESULT __cdecl osmw_api_config_read_string(
    OSMW_OWNER owner, const char *section, const char *key,
    const char *fallback, char *output, DWORD output_size);
OSMW_RESULT __cdecl osmw_api_config_write_string(
    OSMW_OWNER owner, const char *section, const char *key,
    const char *value);

/* Tagged logging service and ABI-shaped entry point. */
BOOL osmw_log_start(const char *game_dir, BOOL enabled);
void osmw_log_close(void);
void __cdecl osmw_api_log(
    OSMW_OWNER owner, OSMW_LOG_LEVEL level, const char *message);

/* Immutable on-disk process image.  Pointers in a view remain valid until
 * osmw_image_reset, which must run after the scan cache is stopped. */
typedef struct OSMW_IMAGE_VIEW {
    const BYTE *file_data;
    DWORD file_size;
    const BYTE *text;
    DWORD text_size;
    DWORD text_rva;
    DWORD preferred_base;
    DWORD loaded_base;
    DWORD timestamp;
    DWORD image_size;
    BOOL address_compatible;
} OSMW_IMAGE_VIEW;

BOOL osmw_image_load_process(void);
BOOL osmw_image_load_file(const char *path, DWORD loaded_base);
BOOL osmw_image_get_view(OSMW_IMAGE_VIEW *output);
const char *osmw_image_last_error(void);
void osmw_image_reset(void);

/* Signature resolver and its process-lifetime content cache. */
BOOL osmw_scan_start(void);
void osmw_scan_reset(void);
DWORD osmw_scan_cache_entries(void);
OSMW_RESULT __cdecl osmw_api_resolve_signature(
    OSMW_OWNER owner, const OSMW_SIGNATURE_V1 *signature,
    OSMW_RESOLVED_ADDRESS_V1 *output);

/* Deterministic managed-module loader.  Loader records remain queryable after
 * shutdown until reset, which lets tests and future diagnostics inspect final
 * states without retaining module-owned strings. */
BOOL osmw_loader_start(OSMW_OWNER core_owner);
BOOL osmw_loader_shutdown(void);
void osmw_loader_reset(void);
DWORD osmw_loader_module_count(void);
const OSMW_CORE_API_V1 *osmw_loader_api(void);
OSMW_RESULT osmw_loader_module_at(DWORD index, OSMW_MODULE_STATUS_V1 *output);
OSMW_RESULT __cdecl osmw_api_module_status(
    const char *module_id_or_filename, OSMW_MODULE_STATUS_V1 *output);

/* Service orchestration.  The explicit-directory form is an internal test
 * seam; production always derives the directory from the process image. */
/* Optional client self-attestation.  Reports core-owned state on a slow interval;
 * see attest.c for what it does and does not look at. */
BOOL osmw_attest_start(OSMW_OWNER core_owner);
void osmw_attest_stop(void);
DWORD osmw_attest_build_payload_for_test(BYTE *buffer, DWORD capacity);

BOOL osmw_core_services_startup(const char *game_dir_override);

DWORD WINAPI osmw_core_startup_worker(LPVOID unused);
void osmw_core_request_stop(void);
BOOL osmw_core_stop_requested(void);
BOOL osmw_core_shutdown(void);

#endif /* OSMW_CORE_INTERNAL_H */
