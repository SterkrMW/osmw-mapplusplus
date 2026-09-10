/* Host-side lifecycle and exact-claim fixture for real osmw_fightspeed.dll. */
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "osmw_core_api.h"

#define TEST_OWNER ((OSMW_OWNER)0x8100u)
#define TEST_SIGNATURE_COUNT 10
#define TEST_MAX_CLAIMS 32

typedef struct {
    OSMW_TOKEN token;
    BYTE *address;
    BYTE saved[24];
    DWORD length;
    OSMW_REGION_KIND kind;
    int active;
} test_claim_t;

static int g_failures, g_service_calls, g_resolve_calls;
static int g_gate_enabled = 1;
static int g_claim_calls, g_fail_claim_call, g_live_claims, g_release_calls;
static const char *g_missing_signature;
static test_claim_t g_claims[TEST_MAX_CLAIMS];
static BYTE *g_floor_anchor, *g_vfx_anchor;
static BYTE g_anim_flag, g_keys[0x59];
static volatile LONG g_anchor_calls;
static int g_camera_decoy;   /* resolve CameraPan to a callee with the wrong site count */
static int g_pan_claims;     /* CALL_SITE claims on a pan site */
static int g_centre_claims;  /* CALL_SITE claims on a keyframe-camera site */
static int g_step_claims;    /* CALL_SITE claims on the camera-ease site */

static void check(int condition, const char *message)
{
    if (condition) printf("PASS: %s\n", message);
    else { printf("FAIL: %s\n", message); ++g_failures; }
}

static void __declspec(noinline) __cdecl fake_framewait(int milliseconds)
{ InterlockedExchangeAdd(&g_anchor_calls, milliseconds); }
static void __declspec(noinline) __cdecl fake_chatline(
    int channel, int value, const char *name, const char *text)
{
    (void)channel; (void)value; (void)name; (void)text;
    InterlockedIncrement(&g_anchor_calls);
}

/* Two AnimFlag writers bound two fight-local FrameWait call sites exactly as
 * the production module's feature-specific scanner expects. */
static volatile LONG g_pan_calls;
static int g_fake_out_x, g_fake_out_y;
static int g_fake_scene;   /* the three globals fake_camerapan's */
static int g_fake_cam_x;   /* prologue names, at the same offsets */
static int g_fake_cam_y;   /* the reference build uses */

/* Naked, because camera.c derives the three camera globals out of CameraPan's
 * own first bytes rather than hardcoding their addresses. The prologue below
 * reproduces the reference build's byte layout exactly so that derivation --
 * and its opcode check -- is what the fixture exercises:
 *
 *   +0x00  83 EC 14        sub esp, 0x14
 *   +0x03  55              push ebp
 *   +0x04  DB 44 24 28     fild dword ptr [esp+0x28]
 *   +0x08  56              push esi
 *   +0x09  57              push edi
 *   +0x0A  8B 3D <abs32>   mov edi, [scene global]      <-- the operand read
 */
#pragma warning(push)
#pragma warning(disable: 4100)   /* naked: the parameters are read from the stack */
/* Naked, because camera.c derives the three camera globals out of CameraPan's
 * own first bytes rather than hardcoding their addresses. The prologue below
 * reproduces the reference build's byte layout exactly, so that derivation --
 * and its opcode check -- is what the fixture exercises:
 *
 *   +0x00  83 EC 14        sub esp, 0x14
 *   +0x03  55              push ebp
 *   +0x04  DB 44 24 28     fild dword ptr [esp+0x28]
 *   +0x08  56              push esi
 *   +0x09  57              push edi
 *   +0x0A  8B 3D <abs32>   mov edi, [scene]     <-- read by camera.c
 *   +0x2A  8B 15 <abs32>   mov edx, [camera Y]  <-- read by camera.c
 *   +0x3E  8B 2D <abs32>   mov ebp, [camera X]  <-- read by camera.c
 *
 * The nop runs are the reference build's arithmetic, which is irrelevant here;
 * only the three load sites and their offsets matter. Change one and the mod
 * stands the feature down, which is exactly the behaviour under test.
 */
__declspec(naked) static void __cdecl fake_camerapan(int scene, int x, int y,
                                                     int frames)
{
    __asm {
        sub  esp, 0x14
        push ebp
        fild dword ptr [esp + 0x28]
        push esi
        push edi
        mov  edi, dword ptr [g_fake_scene]
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        mov  edx, dword ptr [g_fake_cam_y]
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        mov  ebp, dword ptr [g_fake_cam_x]
        fstp st(0)
        inc  dword ptr [g_pan_calls]
        pop  edi
        pop  esi
        pop  ebp
        add  esp, 0x14
        ret
    }
}
#pragma warning(pop)

static void __cdecl fake_cameracentre(int scene, int wx, int wy,
                                      int *out_x, int *out_y)
{
    (void)scene;
    if (out_x) *out_x = wx;
    if (out_y) *out_y = wy;
}

/* Counts, so identical-COMDAT folding cannot merge it with another empty
 * function and make the single-call-site check see three. */
static volatile LONG g_step_calls;
static void __cdecl fake_camerastep(void)
{
    InterlockedIncrement(&g_step_calls);
}

/* The fight home block the mod resolves its home globals out of. Only the
 * three instructions camera.c reads are reproduced, at the offsets it uses:
 *   +0x0F  A3 <world X>
 *   +0x23  C7 05 <scene> 2E 00 00 00
 *   +0x41  A3 <world Y>
 * The mod cross-checks that world X and world Y sit one and two ints above the
 * scene, so the three targets below must keep that layout. */
static struct { int scene, wx, wy; } g_fake_home = { 46, 1000, 900 };

__declspec(naked) static void fake_fighthome(void)
{
    __asm {
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        mov  dword ptr [g_fake_home.wx], eax          /* +0x0F  A3, 5 bytes */
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        mov  dword ptr [g_fake_home.scene], 46        /* +0x23  C7 05, 10 bytes */
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        mov  dword ptr [g_fake_home.wy], eax          /* +0x41  A3, 5 bytes */
        ret
    }
}

/* The synthetic fight animation module.
 *
 * derive_module_bounds() takes the lowest and highest `mov byte [AnimFlag],
 * imm8` in .text, so the two writes below are what make this function the
 * module as far as the DLL is concerned. Everything the mod discovers by
 * scanning must therefore live between them.
 *
 * Thirteen pan call sites, matching the reference build: ten at 12 frames
 * (the pan home) and three at 3 (the pan away). camera.c requires exactly
 * that count before it redirects anything. */
__declspec(naked) static void fight_module_anchor(void)
{
    __asm {
        mov byte ptr [g_anim_flag], 1
        push 5
        call fake_framewait
        add esp, 4
        push 9
        call fake_framewait
        add esp, 4
        push 12
        push 300
        push 200
        push 100
        call fake_camerapan
        add esp, 16
        push 12
        push 300
        push 200
        push 100
        call fake_camerapan
        add esp, 16
        push 12
        push 300
        push 200
        push 100
        call fake_camerapan
        add esp, 16
        push 12
        push 300
        push 200
        push 100
        call fake_camerapan
        add esp, 16
        push 12
        push 300
        push 200
        push 100
        call fake_camerapan
        add esp, 16
        push 12
        push 300
        push 200
        push 100
        call fake_camerapan
        add esp, 16
        push 12
        push 300
        push 200
        push 100
        call fake_camerapan
        add esp, 16
        push 12
        push 300
        push 200
        push 100
        call fake_camerapan
        add esp, 16
        push 12
        push 300
        push 200
        push 100
        call fake_camerapan
        add esp, 16
        push 12
        push 300
        push 200
        push 100
        call fake_camerapan
        add esp, 16
        push 3
        push 300
        push 200
        push 100
        call fake_camerapan
        add esp, 16
        push 3
        push 300
        push 200
        push 100
        call fake_camerapan
        add esp, 16
        push 3
        push 300
        push 200
        push 100
        call fake_camerapan
        add esp, 16
        push offset g_fake_out_y
        push offset g_fake_out_x
        push 300
        push 200
        push 100
        call fake_cameracentre
        add esp, 20
        push offset g_fake_out_y
        push offset g_fake_out_x
        push 300
        push 200
        push 100
        call fake_cameracentre
        add esp, 20
        push offset g_fake_out_y
        push offset g_fake_out_x
        push 300
        push 200
        push 100
        call fake_cameracentre
        add esp, 20
        call fake_camerastep
        mov byte ptr [g_anim_flag], 0
        ret
    }
}

static void createfile_import_anchor(void)
{
    HANDLE file = CreateFileA("NUL", GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING, 0, NULL);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
}

static int g_echo_camera_log;

static void __cdecl fake_log(OSMW_OWNER owner, OSMW_LOG_LEVEL level,
                             const char *message)
{
    ++g_service_calls;
    if (owner != TEST_OWNER || level > OSMW_LOG_ERROR || !message) ++g_failures;
    /* The camera feature stands itself down on several distinct conditions and
     * the claim counters cannot tell them apart. Echo its lines so a failure
     * says which one fired. */
    if (g_echo_camera_log && message && strstr(message, "camera"))
        printf("      [mod] %s" "\n", message);
}

static int __cdecl fake_config_read_int(OSMW_OWNER owner, const char *section,
                                        const char *key, int fallback)
{
    ++g_service_calls;
    if (owner != TEST_OWNER) ++g_failures;
    if (!strcmp(section, "Addons") && !strcmp(key, "FightSpeed"))
        return g_gate_enabled;
    return fallback;
}

static OSMW_RESULT __cdecl fake_config_read_string(
    OSMW_OWNER owner, const char *section, const char *key,
    const char *fallback, char *output, DWORD output_size)
{
    const char *value = fallback ? fallback : "";
    size_t length = strlen(value);
    (void)section; (void)key; ++g_service_calls;
    if (owner != TEST_OWNER || !output || !output_size)
        return OSMW_E_INVALID_ARGUMENT;
    if (length >= output_size) return OSMW_E_BUFFER_TOO_SMALL;
    memcpy(output, value, length + 1);
    return OSMW_OK;
}

static OSMW_RESULT __cdecl fake_config_write_string(
    OSMW_OWNER owner, const char *section, const char *key, const char *value)
{
    ++g_service_calls;
    return owner == TEST_OWNER && section && key && value
        ? OSMW_OK : OSMW_E_INVALID_ARGUMENT;
}

static OSMW_RESULT __cdecl fake_resolve_signature(
    OSMW_OWNER owner, const OSMW_SIGNATURE_V1 *signature,
    OSMW_RESOLVED_ADDRESS_V1 *output)
{
    union { void (__cdecl *wait)(int); void *address; } wait_address;
    union { void (__cdecl *chat)(int,int,const char *,const char *); void *address; } chat_address;
    ++g_service_calls; ++g_resolve_calls;
    if (owner != TEST_OWNER || !signature || !output ||
        signature->struct_size < sizeof(*signature) ||
        output->struct_size < sizeof(*output) || signature->flags ||
        !signature->name) return OSMW_E_INVALID_ARGUMENT;
    ZeroMemory(output, sizeof(*output));
    output->struct_size = sizeof(*output);
    if (g_missing_signature && !strcmp(signature->name, g_missing_signature))
        return OSMW_E_NOT_FOUND;
    wait_address.wait = fake_framewait;
    chat_address.chat = fake_chatline;
    if (!strcmp(signature->name, "FrameSyncFloor")) output->address = g_floor_anchor;
    else if (!strcmp(signature->name, "VfxStep")) output->address = g_vfx_anchor;
    else if (!strcmp(signature->name, "AnimFlag")) output->address = &g_anim_flag;
    else if (!strcmp(signature->name, "KeyArrayA")) output->address = g_keys;
    else if (!strcmp(signature->name, "ChatLine")) output->address = chat_address.address;
    else if (!strcmp(signature->name, "FrameWait")) output->address = wait_address.address;
    else if (!strcmp(signature->name, "CameraPan")) {
        union { void (__cdecl *pan)(int,int,int,int); void *address; } pan_address;
        pan_address.pan = fake_camerapan;
        /* The decoy resolves to a callee the anchor calls twice, so discovery
         * finds a plausible but wrong number of sites. */
        output->address = g_camera_decoy ? wait_address.address
                                         : pan_address.address;
    }
    else if (!strcmp(signature->name, "CameraCentre")) {
        union { void (__cdecl *ctr)(int,int,int,int *,int *); void *address; } ctr_address;
        ctr_address.ctr = fake_cameracentre;
        output->address = ctr_address.address;
    }
    else if (!strcmp(signature->name, "CameraStep")) {
        union { void (__cdecl *st)(void); void *address; } st_address;
        st_address.st = fake_camerastep;
        output->address = st_address.address;
    }
    else if (!strcmp(signature->name, "FightHome")) {
        union { void (__cdecl *fh)(void); void *address; } fh_address;
        fh_address.fh = fake_fighthome;
        output->address = fh_address.address;
    }
    else return OSMW_E_NOT_FOUND;
    output->match_count = 1;
    output->reference_rva = signature->reference_rva;
    output->match_rva = signature->reference_rva;
    return OSMW_OK;
}

static OSMW_RESULT __cdecl fake_claim_region(
    OSMW_OWNER owner, const OSMW_REGION_CLAIM_V1 *claim, OSMW_TOKEN *token)
{
    test_claim_t *record;
    ++g_service_calls; ++g_claim_calls;
    if (owner != TEST_OWNER || !claim || !token ||
        claim->struct_size < sizeof(*claim) || claim->flags ||
        !claim->address || !claim->length || claim->length > 24 || !claim->label)
        return OSMW_E_INVALID_ARGUMENT;
    if (claim->kind == OSMW_REGION_CODE)
        check(claim->length == 1 || claim->length == 17,
              "fight code claim is exactly the VFX byte or FrameSync block");
    else if (claim->kind == OSMW_REGION_CALL_SITE) {
        check(claim->length == 5 && *(BYTE *)claim->address == 0xE8,
              "call-site claim is a five-byte E8 taken before the rewrite");
        if (strstr(claim->label, "keyframe"))
            ++g_centre_claims;
        else if (strstr(claim->label, "ease"))
            ++g_step_claims;
        else if (strstr(claim->label, "camera"))
            ++g_pan_claims;
    }
    else if (claim->kind == OSMW_REGION_IAT)
        check(claim->length == sizeof(void *), "IAT claim is one x86 pointer");
    else
        return OSMW_E_INVALID_ARGUMENT;
    if (g_fail_claim_call == g_claim_calls) return OSMW_E_ALREADY_CLAIMED;
    if (g_live_claims >= TEST_MAX_CLAIMS) return OSMW_E_INTERNAL;
    record = &g_claims[g_live_claims];
    record->token = (OSMW_TOKEN)(0x8110u + (DWORD)g_live_claims);
    record->address = (BYTE *)claim->address;
    record->length = claim->length;
    record->kind = claim->kind;
    memcpy(record->saved, record->address, record->length);
    record->active = 1;
    *token = record->token;
    ++g_live_claims;
    return OSMW_OK;
}

static OSMW_RESULT __cdecl fake_release_region(OSMW_OWNER owner, OSMW_TOKEN token)
{
    int index;
    ++g_service_calls;
    if (owner != TEST_OWNER) return OSMW_E_WRONG_OWNER;
    for (index = 0; index < TEST_MAX_CLAIMS; ++index) {
        test_claim_t *record = &g_claims[index];
        if (!record->active || record->token != token) continue;
        check(!memcmp(record->address, record->saved, record->length),
              "fight bytes/pointer are restored before claim release");
        record->active = 0;
        --g_live_claims; ++g_release_calls;
        return OSMW_OK;
    }
    return OSMW_E_WRONG_OWNER;
}

static void fill_api(OSMW_CORE_API_V1 *api)
{
    ZeroMemory(api, sizeof(*api));
    api->struct_size = sizeof(*api);
    api->abi_version = OSMW_CORE_ABI_CURRENT;
    api->capabilities = OSMW_CAP_CONFIG | OSMW_CAP_SIGNATURES |
                        OSMW_CAP_REGION_CLAIMS;
    api->log = fake_log;
    api->config_read_int = fake_config_read_int;
    api->config_read_string = fake_config_read_string;
    api->config_write_string = fake_config_write_string;
    api->resolve_signature = fake_resolve_signature;
    api->claim_region = fake_claim_region;
    api->release_region = fake_release_region;
}

static void reset_fixture(void)
{
    static const BYTE stock_floor[17] =
        {0x83,0xFA,0x1E,0x73,0x0C,0x2B,0xC8,0x83,0xC1,0x1E,0x51,0xFF,0x15,0,0,0,0};
    g_service_calls = g_resolve_calls = g_claim_calls = 0;
    g_fail_claim_call = g_live_claims = g_release_calls = 0;
    g_pan_claims = g_centre_claims = g_step_claims = 0;
    g_camera_decoy = 0;
    g_gate_enabled = 1; g_missing_signature = NULL;
    ZeroMemory(g_claims, sizeof(g_claims));
    ZeroMemory(g_keys, sizeof(g_keys));
    memcpy(g_floor_anchor, stock_floor, sizeof(stock_floor));
    memset(g_vfx_anchor, 0x90, 0x40);
    g_vfx_anchor[19] = 0x28;
}

int main(int argc, char **argv)
{
    HMODULE module;
    union { FARPROC raw; OSMW_MOD_QUERY_V1 query; } query_address;
    OSMW_MOD_DESCRIPTOR_V1 descriptor;
    OSMW_CORE_API_V1 api;
    OSMW_RESULT result;
    if (argc != 2) { fprintf(stderr, "usage: test_lifecycle.exe dll\n"); return 2; }
    g_floor_anchor = (BYTE *)VirtualAlloc(NULL, 0x40, MEM_COMMIT | MEM_RESERVE,
                                          PAGE_EXECUTE_READWRITE);
    g_vfx_anchor = (BYTE *)VirtualAlloc(NULL, 0x40, MEM_COMMIT | MEM_RESERVE,
                                        PAGE_EXECUTE_READWRITE);
    check(g_floor_anchor && g_vfx_anchor, "fight patch fixtures allocate");
    fight_module_anchor(); createfile_import_anchor(); reset_fixture();
    module = LoadLibraryA(argv[1]);
    check(module != NULL, "real fight-speed DLL loads");
    check(g_service_calls == 0, "DllMain is passive");
    if (!module) return 1;
    query_address.raw = GetProcAddress(module, OSMW_MOD_QUERY_EXPORT_NAME);
    check(query_address.raw != NULL, "undecorated osmw_mod_query resolves");
    ZeroMemory(&descriptor, sizeof(descriptor));
    result = query_address.query(OSMW_ABI_MAKE(2,0), sizeof(descriptor), &descriptor);
    check(result == OSMW_E_INCOMPATIBLE_ABI, "query rejects another ABI major");
    result = query_address.query(OSMW_CORE_ABI_CURRENT, sizeof(descriptor), NULL);
    check(result == OSMW_E_INVALID_ARGUMENT, "query rejects null output");
    result = query_address.query(OSMW_CORE_ABI_CURRENT,
        OSMW_MOD_DESCRIPTOR_V1_MIN_SIZE - 1, &descriptor);
    check(result == OSMW_E_BUFFER_TOO_SMALL, "query rejects a short buffer");
    ZeroMemory(&descriptor, sizeof(descriptor));
    result = query_address.query(OSMW_CORE_ABI_CURRENT, sizeof(descriptor), &descriptor);
    check(result == OSMW_OK && !strcmp(descriptor.module_id, "fightspeed") &&
          descriptor.init && descriptor.shutdown,
          "query publishes the fight-speed ABI 1 descriptor");
    check(g_service_calls == 0, "query is side-effect free");

    fill_api(&api); g_gate_enabled = 0;
    result = descriptor.init(&api, TEST_OWNER);
    check(result == OSMW_MOD_DISABLED && !g_resolve_calls && !g_live_claims,
          "disabled fight-speed resolves and claims nothing");
    check(descriptor.shutdown(&api, TEST_OWNER) == OSMW_OK,
          "disabled instance shuts down cleanly");

    reset_fixture(); fill_api(&api); g_missing_signature = "FrameWait";
    result = descriptor.init(&api, TEST_OWNER);
    check(result == OSMW_E_NOT_FOUND && !g_live_claims,
          "required signature miss occurs before claims");

    reset_fixture(); fill_api(&api); g_fail_claim_call = 5;
    result = descriptor.init(&api, TEST_OWNER);
    check(result == OSMW_E_INTERNAL && !g_live_claims && g_release_calls == 4,
          "IAT claim conflict rolls back every code/call-site claim");

    /* CameraPan resolves but its caller set is the wrong shape: the camera
     * feature must stand down on its own and leave the five speed claims. */
    reset_fixture(); fill_api(&api); g_camera_decoy = 1;
    result = descriptor.init(&api, TEST_OWNER);
    check(result == OSMW_OK && g_claim_calls == 5 && g_live_claims == 5 &&
          g_pan_claims == 0 && g_centre_claims == 0 && g_step_claims == 0,
          "a wrong CameraPan caller count stands the camera down, not the mod");
    check(descriptor.shutdown(&api, TEST_OWNER) == OSMW_OK,
          "the stood-down camera still shuts down cleanly");

    /* CameraPan missing entirely: same outcome, the optional path. */
    /* CameraCentre missing: the pans are still taken, only the cinematic
     * keyframe camera is left at stock. */
    reset_fixture(); fill_api(&api); g_missing_signature = "CameraCentre";
    result = descriptor.init(&api, TEST_OWNER);
    check(result == OSMW_OK && g_pan_claims == 13 && g_centre_claims == 0 &&
          g_step_claims == 1,
          "a missing CameraCentre still leaves the pan and ease sites taken");
    check(descriptor.shutdown(&api, TEST_OWNER) == OSMW_OK,
          "the keyframe-camera-less instance shuts down cleanly");

    /* CameraStep missing: pans and keyframe camera still handled, only the
     * per-frame ease is left at stock. */
    reset_fixture(); fill_api(&api); g_missing_signature = "CameraStep";
    result = descriptor.init(&api, TEST_OWNER);
    check(result == OSMW_OK && g_pan_claims == 13 && g_centre_claims == 3 &&
          g_step_claims == 0,
          "a missing CameraStep leaves the pan and keyframe sites redirected");
    check(descriptor.shutdown(&api, TEST_OWNER) == OSMW_OK,
          "the ease-less instance shuts down cleanly");

    reset_fixture(); fill_api(&api); g_missing_signature = "CameraPan";
    result = descriptor.init(&api, TEST_OWNER);
    check(result == OSMW_OK && g_claim_calls == 5 && g_live_claims == 5,
          "an unresolved CameraPan leaves battle speed fully installed");
    check(descriptor.shutdown(&api, TEST_OWNER) == OSMW_OK,
          "the camera-less instance shuts down cleanly");

    reset_fixture(); fill_api(&api); g_echo_camera_log = 1;
    result = descriptor.init(&api, TEST_OWNER);
    g_echo_camera_log = 0;
    check(result == OSMW_OK && g_resolve_calls == TEST_SIGNATURE_COUNT &&
          g_claim_calls == 22 && g_live_claims == 22 && g_pan_claims == 13 &&
          g_centre_claims == 3 && g_step_claims == 1,
          "active init claims VFX, FrameSync, two FrameWait callers, IAT, all "
          "thirteen pan sites, three keyframe sites and the camera ease");
    check(descriptor.shutdown(&api, TEST_OWNER + 1) == OSMW_E_WRONG_OWNER &&
          g_live_claims == 22, "wrong-owner shutdown preserves fight patches");
    result = descriptor.shutdown(&api, TEST_OWNER);
    check(result == OSMW_OK && !g_live_claims && g_release_calls == 22,
          "shutdown restores and releases every fight-speed range");
    FreeLibrary(module);
    if (g_floor_anchor) VirtualFree(g_floor_anchor, 0, MEM_RELEASE);
    if (g_vfx_anchor) VirtualFree(g_vfx_anchor, 0, MEM_RELEASE);
    if (g_failures) { printf("FAILED: %d fight-speed checks\n", g_failures); return 1; }
    printf("PASSED: managed fight-speed lifecycle\n");
    return 0;
}
