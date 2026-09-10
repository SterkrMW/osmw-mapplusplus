/* Compile this file as both C (/TC) and C++ (/TP).  The typedef assertions
 * freeze ABI 1.0's x86 sizes and offsets before any real module consumes it. */
#include <stdio.h>
#include <string.h>
#include "osmw_core_api.h"

#define ABI_ASSERT(name, expression) \
    typedef char abi_assert_##name[(expression) ? 1 : -1]

ABI_ASSERT(pointer_size, sizeof(void *) == 4);
ABI_ASSERT(dword_size, sizeof(DWORD) == 4);
ABI_ASSERT(long_size, sizeof(LONG) == 4);
ABI_ASSERT(owner_size, sizeof(OSMW_OWNER) == 4);
ABI_ASSERT(token_size, sizeof(OSMW_TOKEN) == 4);
ABI_ASSERT(result_size, sizeof(OSMW_RESULT) == 4);
ABI_ASSERT(function_pointer_size, sizeof(OSMW_GAME_DIR_FN) == 4);

ABI_ASSERT(signature_size, sizeof(OSMW_SIGNATURE_V1) == 32);
ABI_ASSERT(signature_name_offset,
           offsetof(OSMW_SIGNATURE_V1, name) == 4);
ABI_ASSERT(signature_operand_offset,
           offsetof(OSMW_SIGNATURE_V1, operand_offset) == 20);
ABI_ASSERT(signature_flags_offset,
           offsetof(OSMW_SIGNATURE_V1, flags) == 28);

ABI_ASSERT(resolved_size, sizeof(OSMW_RESOLVED_ADDRESS_V1) == 24);
ABI_ASSERT(resolved_address_offset,
           offsetof(OSMW_RESOLVED_ADDRESS_V1, address) == 8);
ABI_ASSERT(resolved_delta_offset,
           offsetof(OSMW_RESOLVED_ADDRESS_V1, rva_delta) == 20);

ABI_ASSERT(region_claim_size, sizeof(OSMW_REGION_CLAIM_V1) == 24);
ABI_ASSERT(region_claim_address_offset,
           offsetof(OSMW_REGION_CLAIM_V1, address) == 4);
ABI_ASSERT(region_claim_label_offset,
           offsetof(OSMW_REGION_CLAIM_V1, label) == 16);

ABI_ASSERT(packet_size, sizeof(OSMW_GAME_PACKET_V1) == 20);
ABI_ASSERT(packet_extra_offset,
           offsetof(OSMW_GAME_PACKET_V1, extra) == 12);
ABI_ASSERT(packet_extra_size_offset,
           offsetof(OSMW_GAME_PACKET_V1, extra_size) == 16);

ABI_ASSERT(module_status_size, sizeof(OSMW_MODULE_STATUS_V1) == 192);
ABI_ASSERT(module_status_last_result_offset,
           offsetof(OSMW_MODULE_STATUS_V1, last_result) == 24);
ABI_ASSERT(module_status_id_offset,
           offsetof(OSMW_MODULE_STATUS_V1, module_id) == 32);
ABI_ASSERT(module_status_filename_offset,
           offsetof(OSMW_MODULE_STATUS_V1, filename) == 64);
ABI_ASSERT(module_status_display_offset,
           offsetof(OSMW_MODULE_STATUS_V1, display_name) == 128);

ABI_ASSERT(descriptor_size, sizeof(OSMW_MOD_DESCRIPTOR_V1) == 32);
ABI_ASSERT(descriptor_id_offset,
           offsetof(OSMW_MOD_DESCRIPTOR_V1, module_id) == 16);
ABI_ASSERT(descriptor_init_offset,
           offsetof(OSMW_MOD_DESCRIPTOR_V1, init) == 24);
ABI_ASSERT(descriptor_shutdown_offset,
           offsetof(OSMW_MOD_DESCRIPTOR_V1, shutdown) == 28);
ABI_ASSERT(descriptor_min_size,
           OSMW_MOD_DESCRIPTOR_V1_MIN_SIZE == 32);

ABI_ASSERT(api_size, sizeof(OSMW_CORE_API_V1) == 76);
ABI_ASSERT(api_game_dir_offset,
           offsetof(OSMW_CORE_API_V1, game_dir) == 16);
ABI_ASSERT(api_resolve_offset,
           offsetof(OSMW_CORE_API_V1, resolve_signature) == 36);
ABI_ASSERT(api_tick_offset,
           offsetof(OSMW_CORE_API_V1, subscribe_post_ui_tick) == 48);
ABI_ASSERT(api_module_status_offset,
           offsetof(OSMW_CORE_API_V1, module_status) == 68);
/* The 1.0 size must stay 72 forever.  It is the promise a 1.0 module compiled
 * against the old header relies on: everything it knows about sits below this
 * mark, so appending past it cannot move anything under its feet. */
ABI_ASSERT(api_v1_0_size, OSMW_CORE_API_V1_0_SIZE == 72);

/* 1.1 appends seal_region and nothing else. */
ABI_ASSERT(api_seal_offset,
           offsetof(OSMW_CORE_API_V1, seal_region) == 72);
ABI_ASSERT(api_v1_1_size, OSMW_CORE_API_V1_1_SIZE == 76);

ABI_ASSERT(v1_major, OSMW_ABI_MAJOR(OSMW_CORE_ABI_V1_0) == 1);
ABI_ASSERT(v1_minor, OSMW_ABI_MINOR(OSMW_CORE_ABI_V1_0) == 0);
ABI_ASSERT(v1_1_major, OSMW_ABI_MAJOR(OSMW_CORE_ABI_V1_1) == 1);
ABI_ASSERT(v1_1_minor, OSMW_ABI_MINOR(OSMW_CORE_ABI_V1_1) == 1);
ABI_ASSERT(current_is_v1_1, OSMW_CORE_ABI_CURRENT == OSMW_CORE_ABI_V1_1);
ABI_ASSERT(ok_is_zero, OSMW_OK == 0);
ABI_ASSERT(disabled_is_positive, OSMW_MOD_DISABLED > 0);
ABI_ASSERT(errors_are_negative, OSMW_E_INVALID_ARGUMENT < 0);

static int expect(int condition, const char *message)
{
    if (!condition) {
        printf("FAIL: %s\n", message);
        return 1;
    }
    printf("ok:   %s\n", message);
    return 0;
}

int main(void)
{
    OSMW_SIGNATURE_V1 signature;
    int failures = 0;

    memset(&signature, 0, sizeof(signature));
    signature.struct_size = sizeof(signature);

    failures += expect(OSMW_STRUCT_HAS(&signature, OSMW_SIGNATURE_V1, flags),
                       "full v1 signature includes flags");
    signature.struct_size = OSMW_FIELD_END(OSMW_SIGNATURE_V1, length);
    failures += expect(OSMW_STRUCT_HAS(&signature, OSMW_SIGNATURE_V1, length),
                       "size prefix includes its final field");
    failures += expect(!OSMW_STRUCT_HAS(&signature, OSMW_SIGNATURE_V1, flags),
                       "short v1 prefix excludes a tail field");
    failures += expect(strcmp(OSMW_MOD_QUERY_EXPORT_NAME, "osmw_mod_query") == 0,
                       "query export name is undecorated and stable");

    printf("%s: ABI 1.0 layout, %d failure(s)\n",
           failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
