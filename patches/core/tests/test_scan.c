#include "core_internal.h"
#include "test_pe_fixture.h"

#include <stdio.h>
#include <string.h>

struct representative_signature {
    const char *module;
    const BYTE *pattern;
    const char *mask;
    DWORD length;
    DWORD reference_rva;
    DWORD fixture_offset;
};

/* Byte-for-byte representatives from each current generated sigs.h. */
static const BYTE sig_chat_wndproc[] =
    {0x56, 0x8B, 0x74, 0x24, 0x0C, 0x57, 0x8B, 0x7C, 0x24, 0x14, 0x83, 0xFE};
static const BYTE sig_login_tick[] =
    {0x39, 0x1D, 0, 0, 0, 0, 0x89, 0x5C, 0x24, 0x1C, 0x89, 0x5C, 0x24, 0x18};
static const BYTE sig_minimap_open[] =
    {0xA0, 0, 0, 0, 0, 0x84, 0xC0, 0x56, 0x57, 0x74, 0, 0xA0};
static const BYTE sig_fightspeed_framesyncfloor[] =
    {0x83, 0xFA, 0x1E, 0x73, 0, 0x2B, 0xC8, 0x83, 0xC1,
     0x1E, 0x51, 0xFF, 0x15, 0, 0, 0, 0};
static const BYTE sig_weather_setcode[] =
    {0x8B, 0x44, 0x24, 0x04, 0x85, 0xC0, 0x75, 0x0E,
     0x50, 0x6A, 0x02, 0x6A, 0x0F, 0xE8};
static const BYTE sig_settings_textwidth[] =
    {0x56, 0x8B, 0x74, 0x24, 0x08, 0x57, 0x33, 0xC9,
     0x33, 0xFF, 0x33, 0xC0, 0x8D, 0x64};
static const BYTE sig_timers_handlepacket[] =
    {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x81, 0xEC, 0x54, 0x05,
     0x00, 0x00, 0xA0, 0, 0, 0, 0, 0x53, 0x8B, 0x5D, 0x0C, 0x56,
     0x8B, 0x75, 0x10, 0xB9, 0x01, 0x00, 0x00, 0x00, 0x57, 0x8B,
     0x7D, 0x08, 0x84, 0xC0, 0x8B, 0x45, 0x14};
static const BYTE sig_inventory_drawinfobox[] =
    {0x8B, 0x44, 0x24, 0x0C, 0x83, 0xC0, 0xF0, 0x83,
     0xEC, 0x08, 0x99, 0x53, 0x83, 0xE2};
static const BYTE sig_fishing_loadmda[] =
    {0x83, 0xEC, 0x28, 0x56, 0x8B, 0x74, 0x24, 0x30,
     0x57, 0x8B, 0x7C, 0x24, 0x38, 0xC7, 0x07, 0xFF};

static const struct representative_signature representatives[] = {
    {"chatmod", sig_chat_wndproc, "xxxxxxxxxxxx", 12, 0x00079170u, 0x010u},
    {"login", sig_login_tick, "xx????xxxxxxxx", 14, 0x00001A62u, 0x040u},
    {"minimap", sig_minimap_open, "x????xxxxx?x", 12, 0x00042370u, 0x070u},
    {"fightspeed", sig_fightspeed_framesyncfloor,
     "xxxx?xxxxxxxx????", 17, 0x0005B8BAu, 0x0A0u},
    {"weather", sig_weather_setcode, "xxxxxxxxxxxxxx", 14,
     0x0006F950u, 0x0D0u},
    {"settings", sig_settings_textwidth, "xxxxxxxxxxxxxx", 14,
     0x00006560u, 0x100u},
    {"timers", sig_timers_handlepacket,
     "xxxxxxxxxxxxx????xxxxxxxxxxxxxxxxxxxxxx", 39,
     0x000542C0u, 0x140u},
    {"inventory", sig_inventory_drawinfobox, "xxxxxxxxxxxxxx", 14,
     0x000615B0u, 0x180u},
    {"fishing", sig_fishing_loadmda, "xxxxxxxxxxxxxxxx", 16,
     0x0006B550u, 0x1B0u},
};

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

static void place_signature(BYTE *text,
                            const struct representative_signature *signature,
                            BYTE wildcard_seed)
{
    DWORD index;
    BYTE *destination = text + signature->fixture_offset;
    memcpy(destination, signature->pattern, signature->length);
    for (index = 0; index < signature->length; ++index) {
        if (signature->mask[index] == '?') {
            destination[index] = (BYTE)(wildcard_seed + index);
        }
    }
}

static void init_request(OSMW_SIGNATURE_V1 *request,
                         const char *name, const BYTE *pattern,
                         const char *mask, DWORD length,
                         LONG operand_offset, DWORD reference_rva)
{
    ZeroMemory(request, sizeof(*request));
    request->struct_size = sizeof(*request);
    request->name = name;
    request->pattern = pattern;
    request->mask = mask;
    request->length = length;
    request->operand_offset = operand_offset;
    request->reference_rva = reference_rva;
}

static void init_output(OSMW_RESOLVED_ADDRESS_V1 *output)
{
    ZeroMemory(output, sizeof(*output));
    output->struct_size = sizeof(*output);
}

int main(void)
{
    BYTE file[TEST_PE_FILE_SIZE];
    BYTE text[0x300];
    BYTE operand_pattern[] = {0xA1, 0, 0, 0, 0, 0x85};
    const char operand_mask[] = "x????x";
    const BYTE missing_pattern[] = {0xF1, 0xF2, 0xF3, 0xF4, 0xF5};
    const BYTE repeated_pattern[] = {0xDE, 0xAD, 0xBE, 0xEF};
    char bad_mask[] = "xxzxxx";
    char path[MAX_PATH];
    char name[96];
    OSMW_SIGNATURE_V1 request;
    OSMW_RESOLVED_ADDRESS_V1 output;
    OSMW_OWNER owner;
    DWORD target = TEST_PE_IMAGE_BASE + 0x1800u;
    DWORD index;
    DWORD before;
    OSMW_RESULT result;

    memset(text, 0xCC, sizeof(text));
    for (index = 0; index <
            (DWORD)(sizeof(representatives) / sizeof(representatives[0]));
            ++index) {
        place_signature(text, &representatives[index], (BYTE)(0x30 + index));
    }
    memcpy(text + 0x200, operand_pattern, sizeof(operand_pattern));
    memcpy(text + 0x201, &target, sizeof(target));
    memcpy(text + 0x240, repeated_pattern, sizeof(repeated_pattern));
    memcpy(text + 0x260, repeated_pattern, sizeof(repeated_pattern));

    check(test_pe_temp_path(path, sizeof(path)), "temporary scan PE path created");
    check(test_pe_build(file, text, sizeof(text)) == TEST_PE_FILE_SIZE &&
              test_pe_write(path, file, sizeof(file)),
          "signature fixture PE written");
    check(osmw_core_owners_init(), "owner registry initialized");
    owner = osmw_core_owner_register("scan-fixture");
    check(owner != OSMW_INVALID_OWNER, "scan fixture owner issued");
    check(osmw_image_load_file(path, TEST_PE_IMAGE_BASE),
          "pristine signature image loaded");
    check(osmw_scan_start(), "signature cache initialized");

    for (index = 0; index <
            (DWORD)(sizeof(representatives) / sizeof(representatives[0]));
            ++index) {
        const struct representative_signature *representative =
            &representatives[index];
        DWORD resolved_rva = TEST_PE_TEXT_RVA + representative->fixture_offset;

        _snprintf(name, sizeof(name), "%s representative",
                  representative->module);
        name[sizeof(name) - 1] = '\0';
        init_request(&request, name, representative->pattern,
                     representative->mask, representative->length, -1,
                     representative->reference_rva);
        init_output(&output);
        result = osmw_api_resolve_signature(owner, &request, &output);
        _snprintf(name, sizeof(name), "%s generated shape resolves",
                  representative->module);
        name[sizeof(name) - 1] = '\0';
        check(result == OSMW_OK && output.match_count == 1 &&
                  output.match_rva == resolved_rva &&
                  (DWORD)(ULONG_PTR)output.address ==
                      TEST_PE_IMAGE_BASE + resolved_rva &&
                  output.rva_delta ==
                      (LONG)(resolved_rva - representative->reference_rva),
              name);
    }
    check(osmw_scan_cache_entries() ==
              (DWORD)(sizeof(representatives) / sizeof(representatives[0])),
          "one content-cache entry stored per representative");

    before = osmw_scan_cache_entries();
    init_request(&request, "same bytes, new diagnostic",
                 representatives[0].pattern, representatives[0].mask,
                 representatives[0].length, -1,
                 TEST_PE_TEXT_RVA + representatives[0].fixture_offset);
    init_output(&output);
    check(osmw_api_resolve_signature(owner, &request, &output) == OSMW_OK &&
              output.rva_delta == 0 &&
              osmw_scan_cache_entries() == before,
          "cache key ignores name/reference diagnostics");

    init_request(&request, "missing", missing_pattern, "xxxxx",
                 sizeof(missing_pattern), -1, 0);
    init_output(&output);
    check(osmw_api_resolve_signature(owner, &request, &output) ==
              OSMW_E_NOT_FOUND && output.match_count == 0 &&
              output.address == NULL,
          "zero matches returns OSMW_E_NOT_FOUND");

    init_request(&request, "multiple", repeated_pattern, "xxxx",
                 sizeof(repeated_pattern), -1, 0);
    init_output(&output);
    check(osmw_api_resolve_signature(owner, &request, &output) ==
              OSMW_E_MULTIPLE_MATCHES && output.match_count == 2 &&
              output.address == NULL,
          "two matches returns OSMW_E_MULTIPLE_MATCHES");

    init_request(&request, "absolute operand", operand_pattern, operand_mask,
                 sizeof(operand_pattern), 1, 0x1800u);
    init_output(&output);
    check(osmw_api_resolve_signature(owner, &request, &output) == OSMW_OK &&
              (DWORD)(ULONG_PTR)output.address == target &&
              output.match_rva == TEST_PE_TEXT_RVA + 0x200u &&
              output.rva_delta == 0,
          "absolute DWORD operand resolves from pristine bytes");
    before = osmw_scan_cache_entries();
    request.reference_rva = 0x17F0u;
    init_output(&output);
    check(osmw_api_resolve_signature(owner, &request, &output) == OSMW_OK &&
              output.rva_delta == 0x10 &&
              osmw_scan_cache_entries() == before,
          "operand target drift is computed after a cache hit");

    request.operand_offset = 3;
    init_output(&output);
    check(osmw_api_resolve_signature(owner, &request, &output) ==
              OSMW_E_INVALID_ARGUMENT,
          "out-of-pattern DWORD operand rejected");
    request.operand_offset = -2;
    init_output(&output);
    check(osmw_api_resolve_signature(owner, &request, &output) ==
              OSMW_E_INVALID_ARGUMENT,
          "operand offset below -1 rejected");

    init_request(&request, "bad mask", operand_pattern, bad_mask,
                 sizeof(operand_pattern), -1, 0);
    init_output(&output);
    check(osmw_api_resolve_signature(owner, &request, &output) ==
              OSMW_E_INVALID_ARGUMENT,
          "mask characters outside x/question-mark rejected");
    request.flags = 1;
    init_output(&output);
    check(osmw_api_resolve_signature(owner, &request, &output) ==
              OSMW_E_INVALID_ARGUMENT,
          "unknown signature flags rejected");
    request.flags = 0;
    request.struct_size = sizeof(request) - 1;
    init_output(&output);
    check(osmw_api_resolve_signature(owner, &request, &output) ==
              OSMW_E_INVALID_ARGUMENT,
          "short signature structure rejected");
    request.struct_size = sizeof(request);
    init_output(&output);
    check(osmw_api_resolve_signature(0xFFFFFFFFu, &request, &output) ==
              OSMW_E_WRONG_OWNER,
          "invalid resolver owner rejected");
    output.struct_size = sizeof(output) - 1;
    check(osmw_api_resolve_signature(owner, &request, &output) ==
              OSMW_E_INVALID_ARGUMENT,
          "short result structure rejected");

    osmw_scan_reset();
    osmw_image_reset();
    check(osmw_image_load_file(path, 0x00500000u),
          "relocated fixture image loaded");
    check(osmw_scan_start(), "resolver starts with relocated metadata");
    init_request(&request, "relocated", representatives[0].pattern,
                 representatives[0].mask, representatives[0].length,
                 -1, representatives[0].reference_rva);
    init_output(&output);
    check(osmw_api_resolve_signature(owner, &request, &output) ==
              OSMW_E_UNAVAILABLE && output.address == NULL,
          "loaded/preferred base mismatch refuses address results");

    osmw_scan_reset();
    osmw_image_reset();
    check(osmw_core_owner_unregister(owner), "scan fixture owner released");
    osmw_core_owners_shutdown();
    (void)DeleteFileA(path);

    printf("%s: core signature resolver, %d failure(s)\n",
           g_failures == 0 ? "PASSED" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
