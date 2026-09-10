#include "core_internal.h"

#include <stdio.h>
#include <string.h>

static BOOL make_path(
    char *output, DWORD output_size, const char *directory, const char *name)
{
    int length = _snprintf(output, output_size, "%s\\%s", directory, name);
    return length >= 0 && (DWORD)length < output_size;
}

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

    file = CreateFileA(path, GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE || output_size == 0) {
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

static OSMW_RESULT claim_range(
    OSMW_OWNER owner, BYTE *address, DWORD length, OSMW_REGION_KIND kind,
    const char *label, OSMW_TOKEN *token)
{
    OSMW_REGION_CLAIM_V1 claim;
    ZeroMemory(&claim, sizeof(claim));
    claim.struct_size = sizeof(claim);
    claim.address = address;
    claim.length = length;
    claim.kind = kind;
    claim.label = label;
    return osmw_api_claim_region(owner, &claim, token);
}

static BOOL expect_status(
    const char *module_id, OSMW_MODULE_STATE state,
    OSMW_RESULT last_result, DWORD claim_count)
{
    OSMW_MODULE_STATUS_V1 status;
    OSMW_RESULT result;

    ZeroMemory(&status, sizeof(status));
    status.struct_size = sizeof(status);
    result = osmw_api_module_status(module_id, &status);
    if (result != OSMW_OK || status.state != state ||
            status.last_result != last_result ||
            status.owned_claim_count != claim_count) {
        printf("%s status mismatch: call=%ld state=%lu last=%ld "
               "claims=%lu\n", module_id, (long)result,
               (unsigned long)status.state,
               (long)status.last_result,
               (unsigned long)status.owned_claim_count);
        return FALSE;
    }
    return TRUE;
}

int main(int argc, char **argv)
{
    BYTE arena[64];
    BYTE original;
    BYTE original_next;
    char temp_base[MAX_PATH] = "";
    char temp_dir[MAX_PATH] = "";
    char log_path[MAX_PATH] = "";
    char dll_path[MAX_PATH] = "";
    char good_dll_path[MAX_PATH] = "";
    char config_path[MAX_PATH] = "";
    char events_path[MAX_PATH] = "";
    char log_content[4096];
    char events[256];
    char long_label[65];
    OSMW_REGION_CLAIM_V1 invalid;
    OSMW_OWNER core_owner = OSMW_INVALID_OWNER;
    OSMW_OWNER alpha_owner = OSMW_INVALID_OWNER;
    OSMW_OWNER beta_owner = OSMW_INVALID_OWNER;
    OSMW_TOKEN alpha_token = OSMW_INVALID_TOKEN;
    OSMW_TOKEN token = OSMW_INVALID_TOKEN;
    DWORD index;
    BOOL direct_ready = FALSE;
    BOOL services_started = FALSE;
    int failed = 0;

#define CHECK(condition, message) \
    do { \
        if (!(condition)) { \
            printf("FAILED: %s\n", (message)); \
            failed = 1; \
            goto cleanup; \
        } \
    } while (0)

    if (argc != 3) {
        printf("usage: test_claims <fixture_unrestored.dll> "
               "<fixture_good.dll>\n");
        return 2;
    }
    CHECK(GetTempPathA(sizeof(temp_base), temp_base) != 0,
          "temporary root available");
    CHECK(_snprintf(temp_dir, sizeof(temp_dir), "%sosmw-claims-%lu-%lu",
                    temp_base, (unsigned long)GetCurrentProcessId(),
                    (unsigned long)GetTickCount()) >= 0,
          "temporary claim path fits");
    CHECK(CreateDirectoryA(temp_dir, NULL), "temporary claim directory made");
    CHECK(_snprintf(log_path, sizeof(log_path), "%s\\osmw_core.%lu.log",
                    temp_dir, (unsigned long)GetCurrentProcessId()) >= 0,
          "claim log path fits");

    for (index = 0; index < sizeof(arena); ++index) {
        arena[index] = (BYTE)index;
    }
    CHECK(osmw_core_owners_init(), "owner registry starts");
    direct_ready = TRUE;
    core_owner = osmw_core_owner_register("core");
    alpha_owner = osmw_core_owner_register("alpha");
    beta_owner = osmw_core_owner_register("beta");
    CHECK(core_owner != OSMW_INVALID_OWNER &&
          alpha_owner != OSMW_INVALID_OWNER &&
          beta_owner != OSMW_INVALID_OWNER, "three owners issued");
    CHECK(osmw_claims_init(), "claim registry starts");
    CHECK(osmw_log_start(temp_dir, TRUE), "diagnostic log starts");

    CHECK(claim_range(alpha_owner, arena + 16, 8, OSMW_REGION_CODE,
                      "alpha-main", &alpha_token) == OSMW_OK &&
          alpha_token != OSMW_INVALID_TOKEN, "first claim granted");
    CHECK(osmw_claims_owner_count(alpha_owner) == 1 &&
          osmw_claims_total_count() == 1, "first claim counted");

    CHECK(claim_range(beta_owner, arena + 16, 8, OSMW_REGION_DATA,
                      "beta-exact", &token) == OSMW_E_ALREADY_CLAIMED,
          "exact overlap rejected across kinds");
    CHECK(claim_range(beta_owner, arena + 18, 2, OSMW_REGION_DATA,
                      "beta-contained", &token) == OSMW_E_ALREADY_CLAIMED,
          "contained overlap rejected");
    CHECK(claim_range(beta_owner, arena + 12, 16, OSMW_REGION_DATA,
                      "beta-containing", &token) == OSMW_E_ALREADY_CLAIMED,
          "containing overlap rejected");
    CHECK(claim_range(beta_owner, arena + 12, 8, OSMW_REGION_CALL_SITE,
                      "beta-left", &token) == OSMW_E_ALREADY_CLAIMED,
          "left-edge overlap rejected");
    CHECK(claim_range(beta_owner, arena + 20, 8, OSMW_REGION_IAT,
                      "beta-right", &token) == OSMW_E_ALREADY_CLAIMED,
          "right-edge overlap rejected");

    CHECK(claim_range(beta_owner, arena + 8, 8, OSMW_REGION_DATA,
                      "beta-adjacent-left", &token) == OSMW_OK,
          "left-adjacent range allowed");
    CHECK(osmw_api_release_region(beta_owner, token) == OSMW_OK,
          "left-adjacent claim released");
    CHECK(claim_range(beta_owner, arena + 24, 8, OSMW_REGION_DATA,
                      "beta-adjacent-right", &token) == OSMW_OK,
          "right-adjacent range allowed");
    CHECK(osmw_api_release_region(beta_owner, token) == OSMW_OK,
          "right-adjacent claim released");

    CHECK(osmw_api_release_region(beta_owner, alpha_token) ==
              OSMW_E_WRONG_OWNER,
          "wrong-owner release rejected");

    /* Sealing.  The whole reason it exists is the first block below: a claim
     * snapshots the bytes a module is about to overwrite, so an applied patch
     * always differs from the snapshot.  Counting that as tampering accused a
     * live client of 92 intrusions, every one of them its own modules working. */
    original = arena[16];
    original_next = arena[17];
    arena[16] = 0x90;
    CHECK(osmw_claims_tampered_count() == 0,
          "an applied patch on an unsealed claim is not tampering");
    CHECK(osmw_claims_sealed_count() == 0,
          "nothing is sealed until an owner says so");

    CHECK(osmw_api_seal_region(beta_owner, alpha_token) == OSMW_E_WRONG_OWNER,
          "sealing somebody else claim rejected");
    CHECK(osmw_api_seal_region(alpha_owner, alpha_token) == OSMW_OK,
          "owner seals its own patched claim");
    CHECK(osmw_claims_sealed_count() == 1, "seal counted");
    CHECK(osmw_claims_tampered_count() == 0,
          "the sealed bytes are the patch itself, so nothing differs");

    arena[17] = 0xCC;
    CHECK(osmw_claims_tampered_count() == 1,
          "a write after sealing is reported");
    CHECK(osmw_api_seal_region(alpha_owner, alpha_token) == OSMW_OK,
          "re-sealing accepts the new bytes");
    CHECK(osmw_claims_tampered_count() == 0, "re-seal clears the report");

    /* The bulk form, which is what modules actually call: they seal from the
     * helper they write memory through, which knows the address but not the
     * token covering it. */
    arena[17] = 0xCD;
    CHECK(osmw_claims_tampered_count() == 1, "bulk fixture is dirty");
    CHECK(osmw_api_seal_region(beta_owner, OSMW_INVALID_TOKEN) ==
              OSMW_E_NOT_FOUND,
          "bulk seal reaches nothing when the caller owns no claims");
    CHECK(osmw_claims_tampered_count() == 1,
          "another owner bulk seal does not launder this claim");
    CHECK(osmw_api_seal_region(alpha_owner, OSMW_INVALID_TOKEN) == OSMW_OK,
          "bulk seal covers the caller own claims");
    CHECK(osmw_claims_tampered_count() == 0, "bulk seal clears the report");

    arena[16] = original;
    arena[17] = original_next;
    CHECK(osmw_api_seal_region(alpha_owner, alpha_token) == OSMW_OK,
          "seal follows the restore too");
    CHECK(osmw_claims_tampered_count() == 0,
          "a restored region is not tampering");

    original = arena[16];
    arena[16] = 0x90;
    CHECK(osmw_api_release_region(alpha_owner, alpha_token) ==
              OSMW_E_PATCH_STILL_ACTIVE,
          "changed live bytes block release");
    CHECK(osmw_claims_owner_count(alpha_owner) == 1,
          "blocked release retains ownership");
    osmw_claims_log_owner(alpha_owner);
    arena[16] = original;
    CHECK(osmw_api_release_region(alpha_owner, alpha_token) == OSMW_OK,
          "restored bytes release successfully");
    CHECK(osmw_api_release_region(alpha_owner, alpha_token) ==
              OSMW_E_NOT_FOUND,
          "duplicate release rejected");

    CHECK(claim_range(core_owner, arena + 40, 4, OSMW_REGION_CODE,
                      "core-shared-site", &token) == OSMW_OK,
          "core uses the same claim registry");
    CHECK(claim_range(beta_owner, arena + 41, 1, OSMW_REGION_DATA,
                      "module-vs-core", &alpha_token) ==
              OSMW_E_ALREADY_CLAIMED,
          "module cannot overlap a core-owned site");
    CHECK(osmw_api_release_region(core_owner, token) == OSMW_OK,
          "core-owned claim released");

    ZeroMemory(&invalid, sizeof(invalid));
    invalid.struct_size = sizeof(invalid) - 1;
    invalid.address = arena;
    invalid.length = 1;
    invalid.kind = OSMW_REGION_DATA;
    invalid.label = "invalid";
    token = 123u;
    CHECK(osmw_api_claim_region(alpha_owner, &invalid, &token) ==
              OSMW_E_INVALID_ARGUMENT && token == OSMW_INVALID_TOKEN,
          "short claim and output token rejected safely");
    invalid.struct_size = sizeof(invalid);
    invalid.flags = 1;
    CHECK(osmw_api_claim_region(alpha_owner, &invalid, &token) ==
              OSMW_E_INVALID_ARGUMENT, "unknown flags rejected");
    invalid.flags = 0;
    invalid.kind = 99;
    CHECK(osmw_api_claim_region(alpha_owner, &invalid, &token) ==
              OSMW_E_INVALID_ARGUMENT, "unknown kind rejected");
    invalid.kind = OSMW_REGION_DATA;
    invalid.address = NULL;
    CHECK(osmw_api_claim_region(alpha_owner, &invalid, &token) ==
              OSMW_E_INVALID_ARGUMENT, "null address rejected");
    invalid.address = arena;
    invalid.length = 0;
    CHECK(osmw_api_claim_region(alpha_owner, &invalid, &token) ==
              OSMW_E_INVALID_ARGUMENT, "zero length rejected");
    invalid.length = 1;
    invalid.label = "";
    CHECK(osmw_api_claim_region(alpha_owner, &invalid, &token) ==
              OSMW_E_INVALID_ARGUMENT, "empty label rejected");
    memset(long_label, 'x', sizeof(long_label) - 1);
    long_label[sizeof(long_label) - 1] = '\0';
    invalid.label = long_label;
    CHECK(osmw_api_claim_region(alpha_owner, &invalid, &token) ==
              OSMW_E_INVALID_ARGUMENT, "overlong label rejected");
    invalid.label = "unreadable";
    invalid.address = (void *)1;
    CHECK(osmw_api_claim_region(alpha_owner, &invalid, &token) ==
              OSMW_E_INVALID_ARGUMENT, "unreadable range rejected");
    invalid.address = arena;
    CHECK(osmw_api_claim_region(OSMW_INVALID_OWNER, &invalid, &token) ==
              OSMW_E_WRONG_OWNER, "invalid owner rejected");
    CHECK(osmw_claims_total_count() == 0,
          "direct claim fixture leaves no resources");

    osmw_log_close();
    CHECK(read_text(log_path, log_content, sizeof(log_content)),
          "overlap diagnostic log readable");
    CHECK(strstr(log_content, "beta/beta-contained") != NULL &&
          strstr(log_content, "alpha/alpha-main") != NULL &&
          strstr(log_content, "beta/module-vs-core") != NULL &&
          strstr(log_content, "core/core-shared-site") != NULL &&
          strstr(log_content, "claim still active: alpha-main") != NULL,
          "overlap diagnostics name both owners and labels");
    osmw_claims_reset();
    (void)osmw_core_owner_unregister(beta_owner);
    (void)osmw_core_owner_unregister(alpha_owner);
    (void)osmw_core_owner_unregister(core_owner);
    osmw_core_owners_shutdown();
    direct_ready = FALSE;

    CHECK(make_path(dll_path, sizeof(dll_path), temp_dir,
                    "fixture_unrestored.dll") &&
          CopyFileA(argv[1], dll_path, FALSE),
          "unrestored module copied");
    CHECK(make_path(good_dll_path, sizeof(good_dll_path), temp_dir,
                    "fixture_good.dll") &&
          CopyFileA(argv[2], good_dll_path, FALSE),
          "clean companion module copied");
    CHECK(make_path(config_path, sizeof(config_path), temp_dir, "config.ini") &&
          make_path(events_path, sizeof(events_path), temp_dir, "events.txt") &&
          write_text(config_path,
                     "[Debug]\r\nModLogging=0\r\n"
                     "[Core]\r\nModules=fixture_good.dll,"
                     "fixture_unrestored.dll\r\n"),
          "unrestored module manifest written");
    SetEnvironmentVariableA("OSMW_FIXTURE_EVENTS", events_path);
    CHECK(osmw_core_services_startup(temp_dir),
          "core starts with unrestored fixture");
    services_started = TRUE;
    CHECK(expect_status("unrestored", OSMW_MODULE_ACTIVE, OSMW_OK, 1),
          "active module status exposes one claim");
    CHECK((osmw_loader_api()->capabilities & OSMW_CAP_REGION_CLAIMS) != 0,
          "claim capability advertised");
    CHECK(!osmw_core_shutdown(),
          "unrestored module blocks whole-core unload");
    CHECK(expect_status("unrestored", OSMW_MODULE_UNSAFE_TO_UNLOAD,
                        OSMW_E_PATCH_STILL_ACTIVE, 1),
          "unsafe module status retains its claim");
    CHECK(expect_status("good", OSMW_MODULE_STOPPING, OSMW_OK, 0),
          "clean companion is stopped but retained after refusal");
    CHECK(GetModuleHandleA("fixture_unrestored.dll") != NULL &&
          GetModuleHandleA("fixture_good.dll") != NULL,
          "refusal retains the complete managed mapping set");
    CHECK(osmw_core_shutdown(),
          "whole-core retry succeeds after fixture restores its bytes");
    services_started = FALSE;
    CHECK(GetModuleHandleA("fixture_unrestored.dll") == NULL &&
          GetModuleHandleA("fixture_good.dll") == NULL,
          "successful retry unloads the complete managed mapping set");
    CHECK(read_text(events_path, events, sizeof(events)) &&
          strcmp(events,
                 "good.init\r\n"
                 "unrestored.init\r\n"
                 "unrestored.shutdown.blocked\r\n"
                 "good.shutdown\r\n"
                 "unrestored.shutdown.restored\r\n") == 0,
          "whole-core refusal and retry order is deterministic");
    printf("claim overlap, all-or-none refusal and retry fixtures passed\n");

cleanup:
    if (services_started) {
        (void)osmw_loader_shutdown();
        (void)osmw_core_shutdown();
    }
    if (direct_ready) {
        osmw_log_close();
        osmw_claims_reset();
        if (beta_owner != OSMW_INVALID_OWNER) {
            (void)osmw_core_owner_unregister(beta_owner);
        }
        if (alpha_owner != OSMW_INVALID_OWNER) {
            (void)osmw_core_owner_unregister(alpha_owner);
        }
        if (core_owner != OSMW_INVALID_OWNER) {
            (void)osmw_core_owner_unregister(core_owner);
        }
        osmw_core_owners_shutdown();
    }
    SetEnvironmentVariableA("OSMW_FIXTURE_EVENTS", NULL);
    if (events_path[0] != '\0') {
        DeleteFileA(events_path);
    }
    if (config_path[0] != '\0') {
        DeleteFileA(config_path);
    }
    if (dll_path[0] != '\0') {
        DeleteFileA(dll_path);
    }
    if (good_dll_path[0] != '\0') {
        DeleteFileA(good_dll_path);
    }
    if (log_path[0] != '\0') {
        DeleteFileA(log_path);
    }
    if (temp_dir[0] != '\0') {
        RemoveDirectoryA(temp_dir);
    }
    return failed;
}
