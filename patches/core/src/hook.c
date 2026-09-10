/* Shared-address resolution and EIP-safe inline-hook writer.
 *
 * The generic writer rejects any target whose live prologue no longer equals
 * the pristine image.  The concrete UiTick hook is owned here so no feature
 * DLL can race or chain another writer at that shared entry point.
 */
#include "core_internal.h"
#include "sigs.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <tlhelp32.h>

#define OSMW_MAX_FROZEN_THREADS 64u
#define OSMW_PATCH_RETRIES 100u
#define OSMW_UI_DETOUR_GUARD     256u
#define OSMW_PACKET_DETOUR_GUARD 256u

typedef struct OSMW_FROZEN_SET {
    HANDLE handles[OSMW_MAX_FROZEN_THREADS];
    DWORD count;
    DWORD error;
} OSMW_FROZEN_SET;

static BOOL g_hook_ready;
static CRITICAL_SECTION g_patch_lock;
static void *g_shared_addresses[OSMW_SHARED_ADDRESS_COUNT];
static OSMW_HOOK g_ui_tick_hook;
static PVOID volatile g_ui_tick_original_address;
static volatile LONG g_ui_tick_entries;
static OSMW_HOOK g_packet_hook;
static PVOID volatile g_packet_original_address;
static volatile LONG g_packet_entries;
typedef char osmw_shared_signature_count_must_match[
    OSMW_CORE_SIGNATURE_COUNT == OSMW_SHARED_ADDRESS_COUNT ? 1 : -1];

static OSMW_RESULT osmw_hook_install_with_slot(
    OSMW_OWNER owner, OSMW_HOOK *hook, void *target, void *detour,
    const BYTE *expected, DWORD length, const char *label,
    PVOID volatile *trampoline_slot);

static void __cdecl ui_tick_detour(LONG list_selector)
{
    OSMW_UI_ORIGINAL_FN original = (OSMW_UI_ORIGINAL_FN)
        InterlockedCompareExchangePointer(
            &g_ui_tick_original_address, NULL, NULL);

    InterlockedIncrement(&g_ui_tick_entries);
    osmw_dispatch_ui_invoke(original, list_selector);
    InterlockedDecrement(&g_ui_tick_entries);
}

static void __stdcall handle_packet_detour(
    DWORD type, DWORD field12, const void *extra, DWORD extra_size)
{
    OSMW_PACKET_ORIGINAL_FN original = (OSMW_PACKET_ORIGINAL_FN)
        InterlockedCompareExchangePointer(
            &g_packet_original_address, NULL, NULL);

    InterlockedIncrement(&g_packet_entries);
    osmw_dispatch_packet_invoke(
        original, type, field12, extra, extra_size);
    InterlockedDecrement(&g_packet_entries);
}

static void hook_log(
    OSMW_OWNER owner, OSMW_LOG_LEVEL level, const char *format, ...)
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
    osmw_api_log(owner, level, message);
}

static BOOL read_exact(const void *address, void *output, DWORD length)
{
    SIZE_T read_size = 0;
    return ReadProcessMemory(GetCurrentProcess(), address, output, length,
                             &read_size) && read_size == length;
}

static void thaw_threads(OSMW_FROZEN_SET *set)
{
    while (set->count != 0) {
        HANDLE thread = set->handles[--set->count];
        (void)ResumeThread(thread);
        CloseHandle(thread);
    }
}

static BOOL freeze_other_threads(OSMW_FROZEN_SET *set)
{
    HANDLE snapshot;
    THREADENTRY32 entry;
    DWORD process_id = GetCurrentProcessId();
    DWORD current_thread = GetCurrentThreadId();
    BOOL have_entry;

    ZeroMemory(set, sizeof(*set));
    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        set->error = GetLastError();
        return FALSE;
    }
    ZeroMemory(&entry, sizeof(entry));
    entry.dwSize = sizeof(entry);
    have_entry = Thread32First(snapshot, &entry);
    if (!have_entry) {
        set->error = GetLastError();
        CloseHandle(snapshot);
        return FALSE;
    }
    do {
        HANDLE thread;
        DWORD error;
        if (entry.th32OwnerProcessID != process_id ||
                entry.th32ThreadID == current_thread) {
            continue;
        }
        if (set->count >= OSMW_MAX_FROZEN_THREADS) {
            set->error = ERROR_INSUFFICIENT_BUFFER;
            CloseHandle(snapshot);
            thaw_threads(set);
            return FALSE;
        }
        thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT,
                            FALSE, entry.th32ThreadID);
        if (thread == NULL) {
            error = GetLastError();
            /* A thread can disappear between the snapshot and OpenThread. */
            if (error == ERROR_INVALID_PARAMETER) {
                continue;
            }
            set->error = error;
            CloseHandle(snapshot);
            thaw_threads(set);
            return FALSE;
        }
        if (SuspendThread(thread) == (DWORD)-1) {
            set->error = GetLastError();
            CloseHandle(thread);
            CloseHandle(snapshot);
            thaw_threads(set);
            return FALSE;
        }
        set->handles[set->count++] = thread;
    } while (Thread32Next(snapshot, &entry));
    CloseHandle(snapshot);
    return TRUE;
}

static BOOL address_in_range(DWORD eip, const BYTE *address, DWORD length)
{
    ULONG_PTR value = (ULONG_PTR)eip;
    ULONG_PTR start = (ULONG_PTR)address;
    return address != NULL && length != 0 &&
        value >= start && value < start + length;
}

static BOOL frozen_thread_inside(
    OSMW_FROZEN_SET *set, const BYTE *first, DWORD first_length,
    const BYTE *second, DWORD second_length,
    const BYTE *third, DWORD third_length)
{
    DWORD index;
    CONTEXT context;

    for (index = 0; index < set->count; ++index) {
        ZeroMemory(&context, sizeof(context));
        context.ContextFlags = CONTEXT_CONTROL;
        if (!GetThreadContext(set->handles[index], &context)) {
            set->error = GetLastError();
            return TRUE;
        }
        if (address_in_range(context.Eip, first, first_length) ||
                address_in_range(context.Eip, second, second_length) ||
                address_in_range(context.Eip, third, third_length)) {
            return TRUE;
        }
    }
    return FALSE;
}

static OSMW_RESULT safe_write(
    BYTE *target, const BYTE *expected, const BYTE *replacement, DWORD length,
    const BYTE *second_busy_range, DWORD second_busy_length,
    const BYTE *third_busy_range, DWORD third_busy_length,
    volatile LONG *busy_count,
    OSMW_RESULT mismatch_result)
{
    BYTE live[OSMW_HOOK_MAX_PROLOGUE];
    DWORD old_protect;
    DWORD ignored_protect;
    DWORD attempt;
    OSMW_RESULT result = OSMW_E_BUSY;

    if (!g_hook_ready || target == NULL || expected == NULL ||
            replacement == NULL || length == 0 ||
            length > OSMW_HOOK_MAX_PROLOGUE) {
        return OSMW_E_INVALID_ARGUMENT;
    }
    EnterCriticalSection(&g_patch_lock);
    if (!read_exact(target, live, length)) {
        LeaveCriticalSection(&g_patch_lock);
        return OSMW_E_INVALID_ARGUMENT;
    }
    if (memcmp(live, expected, length) != 0) {
        LeaveCriticalSection(&g_patch_lock);
        return mismatch_result;
    }
    if (!VirtualProtect(target, length, PAGE_EXECUTE_READWRITE,
                        &old_protect)) {
        LeaveCriticalSection(&g_patch_lock);
        return OSMW_E_INTERNAL;
    }

    for (attempt = 0; attempt < OSMW_PATCH_RETRIES; ++attempt) {
        OSMW_FROZEN_SET frozen;
        if (freeze_other_threads(&frozen) &&
                (busy_count == NULL ||
                 InterlockedCompareExchange(busy_count, 0, 0) == 0) &&
                !frozen_thread_inside(&frozen, target, length,
                                      second_busy_range,
                                      second_busy_length,
                                      third_busy_range,
                                      third_busy_length)) {
            if (memcmp(target, expected, length) != 0) {
                result = mismatch_result;
            } else {
                memcpy(target, replacement, length);
                FlushInstructionCache(GetCurrentProcess(), target, length);
                result = OSMW_OK;
            }
            thaw_threads(&frozen);
            break;
        }
        thaw_threads(&frozen);
        Sleep(1);
    }
    (void)VirtualProtect(target, length, old_protect, &ignored_protect);
    LeaveCriticalSection(&g_patch_lock);
    return result;
}

static BOOL starts_with_relative_branch(const BYTE *bytes, DWORD length)
{
    if (length == 0) {
        return FALSE;
    }
    return bytes[0] == 0xE8 || bytes[0] == 0xE9 || bytes[0] == 0xEB ||
        (bytes[0] >= 0x70 && bytes[0] <= 0x7F) ||
        (length > 1 && bytes[0] == 0x0F &&
         bytes[1] >= 0x80 && bytes[1] <= 0x8F);
}

BOOL osmw_hook_init(void)
{
    if (g_hook_ready) {
        return TRUE;
    }
    ZeroMemory(g_shared_addresses, sizeof(g_shared_addresses));
    ZeroMemory(&g_ui_tick_hook, sizeof(g_ui_tick_hook));
    ZeroMemory(&g_packet_hook, sizeof(g_packet_hook));
    InterlockedExchangePointer(&g_ui_tick_original_address, NULL);
    InterlockedExchangePointer(&g_packet_original_address, NULL);
    InterlockedExchange(&g_ui_tick_entries, 0);
    InterlockedExchange(&g_packet_entries, 0);
    InitializeCriticalSection(&g_patch_lock);
    g_hook_ready = TRUE;
    return TRUE;
}

void osmw_hook_reset(void)
{
    if (!g_hook_ready) {
        return;
    }
    if (osmw_hook_is_active(&g_ui_tick_hook) ||
            osmw_hook_is_active(&g_packet_hook)) {
        return;
    }
    ZeroMemory(g_shared_addresses, sizeof(g_shared_addresses));
    ZeroMemory(&g_ui_tick_hook, sizeof(g_ui_tick_hook));
    ZeroMemory(&g_packet_hook, sizeof(g_packet_hook));
    InterlockedExchangePointer(&g_ui_tick_original_address, NULL);
    InterlockedExchangePointer(&g_packet_original_address, NULL);
    InterlockedExchange(&g_ui_tick_entries, 0);
    InterlockedExchange(&g_packet_entries, 0);
    DeleteCriticalSection(&g_patch_lock);
    g_hook_ready = FALSE;
}

BOOL osmw_hook_resolve_shared(OSMW_OWNER core_owner)
{
    DWORD index;
    DWORD resolved_count = 0;

    if (!g_hook_ready) {
        return FALSE;
    }
    ZeroMemory(g_shared_addresses, sizeof(g_shared_addresses));
    for (index = 0; index < OSMW_CORE_SIGNATURE_COUNT; ++index) {
        const OSMW_CORE_SIGNATURE_DEF *definition =
            &OSMW_CORE_SIGNATURES[index];
        OSMW_SIGNATURE_V1 request;
        OSMW_RESOLVED_ADDRESS_V1 output;
        OSMW_RESULT result;

        ZeroMemory(&request, sizeof(request));
        request.struct_size = sizeof(request);
        request.name = definition->name;
        request.pattern = definition->pattern;
        request.mask = definition->mask;
        request.length = definition->length;
        request.operand_offset = definition->operand_offset;
        request.reference_rva = definition->reference_rva;
        ZeroMemory(&output, sizeof(output));
        output.struct_size = sizeof(output);
        result = osmw_api_resolve_signature(core_owner, &request, &output);
        if (result == OSMW_OK) {
            g_shared_addresses[index] = output.address;
            ++resolved_count;
            hook_log(core_owner, OSMW_LOG_INFO,
                     "shared signature ready: %s at %p",
                     definition->name, output.address);
        } else {
            hook_log(core_owner, OSMW_LOG_ERROR,
                     "shared signature unavailable: %s (%ld)",
                     definition->name, (long)result);
        }
    }
    return resolved_count == OSMW_CORE_SIGNATURE_COUNT;
}

void *osmw_hook_shared_address(OSMW_SHARED_ADDRESS which)
{
    if (!g_hook_ready || which >= OSMW_SHARED_ADDRESS_COUNT) {
        return NULL;
    }
    return g_shared_addresses[which];
}

DWORD osmw_hook_shared_length(OSMW_SHARED_ADDRESS which)
{
    if (which >= OSMW_SHARED_ADDRESS_COUNT) {
        return 0;
    }
    return OSMW_CORE_SIGNATURES[which].hook_length;
}

OSMW_RESULT osmw_hook_install(
    OSMW_OWNER owner, OSMW_HOOK *hook, void *target, void *detour,
    const BYTE *expected, DWORD length, const char *label)
{
    return osmw_hook_install_with_slot(
        owner, hook, target, detour, expected, length, label, NULL);
}

static OSMW_RESULT osmw_hook_install_with_slot(
    OSMW_OWNER owner, OSMW_HOOK *hook, void *target, void *detour,
    const BYTE *expected, DWORD length, const char *label,
    PVOID volatile *trampoline_slot)
{
    OSMW_REGION_CLAIM_V1 claim;
    OSMW_TOKEN claim_token = OSMW_INVALID_TOKEN;
    BYTE live[OSMW_HOOK_MAX_PROLOGUE];
    BYTE patch[OSMW_HOOK_MAX_PROLOGUE];
    BYTE *trampoline;
    DWORD index;
    OSMW_RESULT result;

    if (!g_hook_ready || hook == NULL || target == NULL || detour == NULL ||
            expected == NULL || label == NULL || length < 5 ||
            length > OSMW_HOOK_MAX_PROLOGUE) {
        return OSMW_E_INVALID_ARGUMENT;
    }
    if (osmw_hook_is_active(hook)) {
        return OSMW_E_BUSY;
    }
    ZeroMemory(hook, sizeof(*hook));
    if (starts_with_relative_branch(expected, length)) {
        return OSMW_E_INCOMPATIBLE_ABI;
    }
    if (!read_exact(target, live, length)) {
        return OSMW_E_INVALID_ARGUMENT;
    }
    if (memcmp(live, expected, length) != 0) {
        hook_log(owner, OSMW_LOG_ERROR,
                 "hook target already differs from pristine bytes: %s at %p",
                 label, target);
        return OSMW_E_ALREADY_CLAIMED;
    }

    ZeroMemory(&claim, sizeof(claim));
    claim.struct_size = sizeof(claim);
    claim.address = target;
    claim.length = length;
    claim.kind = OSMW_REGION_CODE;
    claim.label = label;
    result = osmw_api_claim_region(owner, &claim, &claim_token);
    if (result != OSMW_OK) {
        return result;
    }

    trampoline = (BYTE *)VirtualAlloc(
        NULL, length + 5u, MEM_COMMIT | MEM_RESERVE,
        PAGE_EXECUTE_READWRITE);
    if (trampoline == NULL) {
        (void)osmw_api_release_region(owner, claim_token);
        return OSMW_E_INTERNAL;
    }
    memcpy(trampoline, expected, length);
    trampoline[length] = 0xE9;
    *(LONG *)(trampoline + length + 1u) = (LONG)(
        ((ULONG_PTR)target + length) -
        ((ULONG_PTR)trampoline + length + 5u));

    if (trampoline_slot != NULL) {
        InterlockedExchangePointer(trampoline_slot, trampoline);
    }

    patch[0] = 0xE9;
    *(LONG *)(patch + 1) = (LONG)(
        (ULONG_PTR)detour - ((ULONG_PTR)target + 5u));
    for (index = 5; index < length; ++index) {
        patch[index] = 0x90;
    }

    result = safe_write((BYTE *)target, expected, patch, length,
                        NULL, 0, NULL, 0, NULL,
                        OSMW_E_ALREADY_CLAIMED);
    if (result != OSMW_OK) {
        OSMW_RESULT release_result =
            osmw_api_release_region(owner, claim_token);
        if (trampoline_slot != NULL) {
            InterlockedExchangePointer(trampoline_slot, NULL);
        }
        VirtualFree(trampoline, 0, MEM_RELEASE);
        if (release_result != OSMW_OK) {
            hook->target = (BYTE *)target;
            hook->length = length;
            hook->owner = owner;
            hook->claim_token = claim_token;
            memcpy(hook->saved, expected, length);
        }
        hook_log(owner, OSMW_LOG_ERROR,
                 "hook write refused: %s at %p (%ld)",
                 label, target, (long)result);
        return result;
    }

    hook->target = (BYTE *)target;
    hook->trampoline = trampoline;
    hook->trampoline_size = length + 5u;
    hook->length = length;
    hook->owner = owner;
    hook->claim_token = claim_token;
    hook->patch_installed = TRUE;
    memcpy(hook->saved, expected, length);
    memcpy(hook->patch, patch, length);
    /* The trampoline jump is now the intended content of this range, so record it
     * as such.  Without this the claim is only ever compared against the original
     * bytes, which every installed hook differs from by construction.
     *
     * Sealing is advisory: a failure means this range goes unwatched, which is
     * the same position every unsealed claim is in and is not a reason to refuse
     * a hook that is already installed and working. */
    if (osmw_api_seal_region(owner, claim_token) != OSMW_OK) {
        hook_log(owner, OSMW_LOG_WARN,
                 "hook seal failed: %s at %p, region will not be monitored",
                 label, target);
    }
    hook_log(owner, OSMW_LOG_INFO,
             "hook installed: %s at %p -> %p", label, target, detour);
    return OSMW_OK;
}

static OSMW_RESULT osmw_hook_remove_with_guard(
    OSMW_HOOK *hook, const BYTE *detour, DWORD detour_length,
    volatile LONG *busy_count)
{
    OSMW_RESULT result;

    if (hook == NULL) {
        return OSMW_E_INVALID_ARGUMENT;
    }
    if (!osmw_hook_is_active(hook)) {
        return OSMW_E_NOT_FOUND;
    }
    if (hook->patch_installed) {
        result = safe_write(
            hook->target, hook->patch, hook->saved, hook->length,
            hook->trampoline, hook->trampoline_size,
            detour, detour_length, busy_count,
            OSMW_E_PATCH_STILL_ACTIVE);
        if (result != OSMW_OK) {
            hook_log(hook->owner, OSMW_LOG_ERROR,
                     "hook removal refused at %p (%ld)",
                     hook->target, (long)result);
            return result;
        }
        hook->patch_installed = FALSE;
        /* Re-seal on the original bytes.  Release usually follows immediately,
         * but a release that fails leaves the claim standing, and a seal still
         * describing the removed patch would report the restored original as an
         * intrusion for the rest of the session. */
        (void)osmw_api_seal_region(hook->owner, hook->claim_token);
    }
    result = osmw_api_release_region(hook->owner, hook->claim_token);
    if (result != OSMW_OK) {
        hook_log(hook->owner, OSMW_LOG_ERROR,
                 "hook claim release refused at %p (%ld)",
                 hook->target, (long)result);
        return result;
    }
    if (hook->trampoline != NULL) {
        VirtualFree(hook->trampoline, 0, MEM_RELEASE);
    }
    hook_log(hook->owner, OSMW_LOG_INFO,
             "hook removed at %p", hook->target);
    ZeroMemory(hook, sizeof(*hook));
    return OSMW_OK;
}

OSMW_RESULT osmw_hook_remove(OSMW_HOOK *hook)
{
    return osmw_hook_remove_with_guard(hook, NULL, 0, NULL);
}

BOOL osmw_hook_is_active(const OSMW_HOOK *hook)
{
    return hook != NULL &&
        (hook->claim_token != OSMW_INVALID_TOKEN ||
         hook->patch_installed || hook->trampoline != NULL);
}

static BOOL copy_shared_pristine_bytes(
    OSMW_SHARED_ADDRESS which, BYTE *output, DWORD length)
{
    OSMW_IMAGE_VIEW image;
    ULONG_PTR address;
    ULONG_PTR rva_value;
    DWORD offset;

    if (!g_hook_ready || which >= OSMW_SHARED_ADDRESS_COUNT ||
            output == NULL || length == 0 ||
            g_shared_addresses[which] == NULL ||
            !osmw_image_get_view(&image)) {
        return FALSE;
    }
    address = (ULONG_PTR)g_shared_addresses[which];
    if (address < (ULONG_PTR)image.loaded_base) {
        return FALSE;
    }
    rva_value = address - (ULONG_PTR)image.loaded_base;
    if (rva_value > MAXDWORD || (DWORD)rva_value < image.text_rva) {
        return FALSE;
    }
    offset = (DWORD)rva_value - image.text_rva;
    if (offset > image.text_size || length > image.text_size - offset) {
        return FALSE;
    }
    memcpy(output, image.text + offset, length);
    return TRUE;
}

OSMW_RESULT osmw_hook_install_ui_target(
    OSMW_OWNER owner, void *target, const BYTE *expected, DWORD length)
{
    if (length != osmw_hook_shared_length(OSMW_SHARED_UI_TICK)) {
        return OSMW_E_INVALID_ARGUMENT;
    }
    if (osmw_hook_is_active(&g_ui_tick_hook)) {
        return OSMW_E_BUSY;
    }
    return osmw_hook_install_with_slot(
        owner, &g_ui_tick_hook, target, (void *)ui_tick_detour,
        expected, length, "core-ui-tick", &g_ui_tick_original_address);
}

/*
 * Install a caller-owned hook on one of the shared targets.
 *
 * The pristine prologue lives here, next to the resolver that produced it, so callers
 * outside this file cannot install without it. Passing NULL for the expected bytes is
 * rejected by the installer, which is the correct refusal but an opaque one: the whole
 * point of comparing against the pristine image is to notice when something else got
 * there first, so there is no sensible way to hook without it.
 */
OSMW_RESULT osmw_hook_install_shared(
    OSMW_OWNER owner, OSMW_SHARED_ADDRESS which, OSMW_HOOK *hook,
    void *detour, const char *label)
{
    BYTE expected[OSMW_HOOK_MAX_PROLOGUE];
    void *target = osmw_hook_shared_address(which);
    DWORD length = osmw_hook_shared_length(which);

    if (target == NULL || length < 5u || length > OSMW_HOOK_MAX_PROLOGUE ||
            !copy_shared_pristine_bytes(which, expected, length)) {
        return OSMW_E_UNAVAILABLE;
    }
    return osmw_hook_install(owner, hook, target, detour, expected, length, label);
}

OSMW_RESULT osmw_hook_start_ui(OSMW_OWNER owner)
{
    BYTE expected[OSMW_HOOK_MAX_PROLOGUE];
    void *target = osmw_hook_shared_address(OSMW_SHARED_UI_TICK);
    DWORD length = osmw_hook_shared_length(OSMW_SHARED_UI_TICK);

    if (target == NULL || length != 7u ||
            !copy_shared_pristine_bytes(
                OSMW_SHARED_UI_TICK, expected, length)) {
        return OSMW_E_UNAVAILABLE;
    }
    return osmw_hook_install_ui_target(owner, target, expected, length);
}

OSMW_RESULT osmw_hook_stop_ui(void)
{
    OSMW_RESULT result;

    if (!osmw_hook_is_active(&g_ui_tick_hook)) {
        InterlockedExchangePointer(&g_ui_tick_original_address, NULL);
        return OSMW_OK;
    }
    result = osmw_hook_remove_with_guard(
        &g_ui_tick_hook, (const BYTE *)ui_tick_detour,
        OSMW_UI_DETOUR_GUARD, &g_ui_tick_entries);
    if (result == OSMW_OK) {
        InterlockedExchangePointer(&g_ui_tick_original_address, NULL);
    }
    return result;
}

BOOL osmw_hook_ui_active(void)
{
    return g_hook_ready && g_ui_tick_hook.patch_installed &&
        InterlockedCompareExchangePointer(
            &g_ui_tick_original_address, NULL, NULL) != NULL;
}

OSMW_RESULT osmw_hook_install_packet_target(
    OSMW_OWNER owner, void *target, const BYTE *expected, DWORD length)
{
    if (length != osmw_hook_shared_length(OSMW_SHARED_HANDLE_PACKET)) {
        return OSMW_E_INVALID_ARGUMENT;
    }
    if (osmw_hook_is_active(&g_packet_hook)) {
        return OSMW_E_BUSY;
    }
    return osmw_hook_install_with_slot(
        owner, &g_packet_hook, target, (void *)handle_packet_detour,
        expected, length, "core-handle-packet",
        &g_packet_original_address);
}

OSMW_RESULT osmw_hook_start_packet(OSMW_OWNER owner)
{
    BYTE expected[OSMW_HOOK_MAX_PROLOGUE];
    void *target = osmw_hook_shared_address(OSMW_SHARED_HANDLE_PACKET);
    DWORD length = osmw_hook_shared_length(OSMW_SHARED_HANDLE_PACKET);

    if (target == NULL || length != 12u ||
            !copy_shared_pristine_bytes(
                OSMW_SHARED_HANDLE_PACKET, expected, length)) {
        return OSMW_E_UNAVAILABLE;
    }
    return osmw_hook_install_packet_target(owner, target, expected, length);
}

OSMW_RESULT osmw_hook_stop_packet(void)
{
    OSMW_RESULT result;

    if (!osmw_hook_is_active(&g_packet_hook)) {
        InterlockedExchangePointer(&g_packet_original_address, NULL);
        return OSMW_OK;
    }
    result = osmw_hook_remove_with_guard(
        &g_packet_hook, (const BYTE *)handle_packet_detour,
        OSMW_PACKET_DETOUR_GUARD, &g_packet_entries);
    if (result == OSMW_OK) {
        InterlockedExchangePointer(&g_packet_original_address, NULL);
    }
    return result;
}

BOOL osmw_hook_packet_active(void)
{
    return g_hook_ready && g_packet_hook.patch_installed &&
        InterlockedCompareExchangePointer(
            &g_packet_original_address, NULL, NULL) != NULL;
}
