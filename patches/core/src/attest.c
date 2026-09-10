/* Optional client self-attestation.
 *
 * Reports what core already knows about its own process - which build of main.exe it
 * mapped, whether the image base moved, whether anything wrote into a range a managed
 * module claimed, and which modules are loaded - as one periodic packet on the mod-only
 * type range.
 *
 * This lives in core rather than a feature module because every input is core-owned state:
 * a feature DLL would have to ask core for all of it, and core does not expose the pristine
 * image or the claim snapshots across the ABI.
 *
 * ## What it is worth
 *
 * Little, and the server weights it accordingly. There is no signing anywhere in this
 * ecosystem, so anyone who captures one of these packets can replay it, and anyone who
 * patches core can send whatever they like. It raises the cost of casual automation and
 * nothing beyond that. It must never be the reason a player is accused of anything.
 *
 * ## What it deliberately does not do
 *
 * It reads only this process's own image and the game's own directory. No process
 * enumeration, no filesystem scan outside the game folder, no hardware fingerprint, no
 * screen capture, no input content. Everything on the wire is a hash, a version number or
 * a count. Anything beyond that is a different feature and needs its own decision.
 *
 * Wire format is specified in mw-tools/tools/client-dll/attestation.md and parsed by
 * mw-tools/src/game-server/Services/AutoMod/ClientAttestation.ts. Keep all three in step.
 */
#include "core_internal.h"

#include <stdio.h>
#include <string.h>

#define OSMW_ATTEST_PACKET_TYPE   0x7F000030u
#define OSMW_ATTEST_PROTOCOL      1u
#define OSMW_ATTEST_HEADER_BYTES  32u   /* body only; see the prefix note on build_payload */
#define OSMW_ATTEST_MODULE_BYTES  8u
#define OSMW_ATTEST_MAX_MODULES   32u

/* Flag bits at wire offset 28.  Higher bits are reserved; the server ignores what it does
 * not know, so a newer core may add bits without breaking an older server. */
#define OSMW_ATTEST_FLAG_BASE_MISMATCH   0x00000001u
#define OSMW_ATTEST_FLAG_MODHASH         0x00000002u
/* Set only when BOTH input counters are real.  Until then the server must not judge
 * the action/input ratio at all: zero input against real actions is the bot signature
 * itself, so an ungated half-implementation accuses every honest client. */
#define OSMW_ATTEST_FLAG_INPUT_PROVENANCE 0x00000004u
/* Set when the tampered count is a count of FOREIGN writes rather than of the patches the
 * client applied itself.
 *
 * A claim snapshots the bytes a module is about to overwrite, so that snapshot is the
 * pre-patch original and comparing it live differs for every patch that is applied.  Read as
 * intrusion it accused the first live client of 92 of them, all its own modules working.
 *
 * seal_region (ABI 1.1) is what separates the two: an owner declares the bytes it just wrote,
 * and only sealed claims are compared afterwards.  So the bit means "at least one claim is
 * sealed, and the count only ever includes sealed ones".  With nothing sealed the count is
 * structurally zero and the bit stays clear, which keeps "nothing was written" distinct from
 * "nothing is being watched". */
#define OSMW_ATTEST_FLAG_CLAIMS_SEALED   0x00000008u

/* First report shortly after startup, then at a slow interval.  The server rate-limits to a
 * 5s minimum and 4 per minute, so this is far inside what it will accept. */
#define OSMW_ATTEST_FIRST_DELAY_MS  20000u
#define OSMW_ATTEST_INTERVAL_MS    300000u

/* Map data. The client receives this on entering the world and on every map change, so
 * the first one is the earliest moment a report can actually be accepted. */
#define OSMW_ATTEST_MAPDATA_TYPE 0x00030100u

/* How long to wait for the map-data observer before reporting anyway. */
#define OSMW_ATTEST_WORLD_FALLBACK_MS 180000u

static BOOL g_attest_ready;
static OSMW_OWNER g_attest_owner;
static OSMW_TOKEN g_attest_tick_token;
static DWORD g_attest_started_at;
static DWORD g_attest_last_sent;
static BOOL g_attest_sent_once;
static BOOL g_attest_in_world;
static OSMW_RESULT g_attest_last_result;
static BOOL g_attest_logged_once;
static OSMW_TOKEN g_attest_mapdata_token;

/* SendPacket counter.  Written from whatever thread the client sends on and read on
 * the UiTick thread, so it is interlocked rather than a plain long: a torn read here
 * would put a fabricated number on the wire. */
static OSMW_HOOK g_attest_send_hook;
static volatile LONG g_attest_actions_sent;
static PVOID volatile g_attest_send_original;

/* Input provenance. See attest_hook_input for why this redirects a call site. */
static volatile LONG g_attest_input_events;
static BOOL g_attest_input_ready;
static BYTE *g_attest_input_byte;      /* client global: raw WndProc message code */
static BYTE *g_attest_input_call_site; /* the five-byte call we redirect */
static BYTE g_attest_input_saved[5];
static OSMW_TOKEN g_attest_input_claim;
typedef BYTE(__cdecl *OSMW_INPUT_SAMPLE_FN)(void);
static OSMW_INPUT_SAMPLE_FN g_attest_input_original;

/* FNV-1a over the mapped file bytes.  Not a security hash and does not need to be: the
 * packet is unsigned anyway, so this only has to distinguish builds, not resist forgery. */
static DWORD hash_bytes(const BYTE *data, DWORD length)
{
    DWORD hash = 2166136261u;
    DWORD index;

    for (index = 0; index < length; ++index) {
        hash ^= (DWORD)data[index];
        hash *= 16777619u;
    }
    return hash;
}

/* Stable hash of a module id, so no variable-length strings ride the wire. */
static DWORD hash_module_id(const char *text)
{
    DWORD hash = 2166136261u;

    while (text != NULL && *text != '\0') {
        hash ^= (DWORD)(BYTE)*text++;
        hash *= 16777619u;
    }
    return hash;
}

static void write_dword(BYTE *buffer, DWORD offset, DWORD value)
{
    buffer[offset + 0] = (BYTE)(value & 0xFFu);
    buffer[offset + 1] = (BYTE)((value >> 8) & 0xFFu);
    buffer[offset + 2] = (BYTE)((value >> 16) & 0xFFu);
    buffer[offset + 3] = (BYTE)((value >> 24) & 0xFFu);
}

static void write_word(BYTE *buffer, DWORD offset, WORD value)
{
    buffer[offset + 0] = (BYTE)(value & 0xFFu);
    buffer[offset + 1] = (BYTE)((value >> 8) & 0xFFu);
}

/*
 * Per-frame input sampler, wrapped to count real messages.
 *
 * The client samples one input event per frame. The function it calls returns the raw
 * WndProc message code when there is one, and otherwise synthesises a held-button code
 * from the last message's wParam. Only the first is evidence a person did something: the
 * synthesised one is regenerated every frame a button stays down, so counting it would
 * make an idle client with a resting finger look like a fast typist.
 *
 * The count therefore keys on the raw byte being non-zero on entry, which is exactly the
 * test the client itself uses to tell a real message from a manufactured one.
 *
 * If sampling ever happens more than once a frame this overcounts, which is the safe
 * direction: undercounting would inflate the actions-per-input ratio and push honest
 * players toward an accusation.
 */
static BYTE __cdecl attest_input_detour(void)
{
    BYTE *raw = g_attest_input_byte;
    OSMW_INPUT_SAMPLE_FN original = g_attest_input_original;

    if (raw != NULL && *(volatile BYTE *)raw != 0) {
        InterlockedIncrement(&g_attest_input_events);
    }
    if (original == NULL) {
        return 0;
    }
    return original();
}

/*
 * Redirect the sampler call rather than detouring the sampler itself.
 *
 * The sampler body is duplicated verbatim at a second address in the reference build, so
 * no signature over it can resolve to exactly one match. The call site is unique because
 * of what surrounds it, and rewriting a call is a five-byte write on an instruction
 * boundary rather than a prologue splice that would have to straddle the movzx the
 * function opens with.
 *
 * Both addresses are derived from the resolved site rather than hardcoded: the sampler is
 * read out of the call displacement, and the global out of the sampler's first
 * instruction. Every step is verified before anything is written, and any failure leaves
 * the client untouched - the server simply never judges the ratio.
 */
static BOOL attest_hook_input(OSMW_OWNER owner, const OSMW_CORE_API_V1 *api)
{
    static const BYTE pattern[26] = {
        0x89, 0x0D, 0x00, 0x00, 0x00, 0x00, 0xE8, 0x00, 0x00, 0x00, 0x00,
        0xA2, 0x00, 0x00, 0x00, 0x00, 0xE8, 0x00, 0x00, 0x00, 0x00,
        0xA2, 0x00, 0x00, 0x00, 0x00
    };
    OSMW_SIGNATURE_V1 request;
    OSMW_RESOLVED_ADDRESS_V1 resolved;
    OSMW_REGION_CLAIM_V1 claim;
    BYTE *site;
    BYTE *target;
    DWORD protect = 0;
    LONG displacement;

    if (api == NULL || api->resolve_signature == NULL || api->claim_region == NULL) {
        return FALSE;
    }

    ZeroMemory(&request, sizeof(request));
    request.struct_size = sizeof(request);
    request.name = "AttestInputSampleCall";
    request.pattern = pattern;
    request.mask = "xx????x????x????x????x????";
    request.length = (DWORD)sizeof(pattern);
    request.operand_offset = -1;
    request.reference_rva = 0x0005BA2Fu;

    ZeroMemory(&resolved, sizeof(resolved));
    resolved.struct_size = sizeof(resolved);
    if (api->resolve_signature(owner, &request, &resolved) != OSMW_OK) {
        return FALSE;
    }

    /* The pattern starts six bytes before the call it exists to locate. */
    site = (BYTE *)(DWORD_PTR)resolved.address + 6;
    if (site[0] != 0xE8) {
        return FALSE;
    }

    memcpy(&displacement, site + 1, sizeof(displacement));
    target = site + 5 + displacement;

    /* push ebp; mov ebp,esp; movzx eax, byte [imm32] - refuse anything else. */
    if (target[0] != 0x55 || target[1] != 0x8B || target[2] != 0xEC ||
            target[3] != 0x0F || target[4] != 0xB6 || target[5] != 0x05) {
        return FALSE;
    }
    memcpy(&g_attest_input_byte, target + 6, sizeof(g_attest_input_byte));
    if (g_attest_input_byte == NULL) {
        return FALSE;
    }
    g_attest_input_original = (OSMW_INPUT_SAMPLE_FN)target;

    ZeroMemory(&claim, sizeof(claim));
    claim.struct_size = sizeof(claim);
    claim.address = site;
    claim.length = 5;
    claim.kind = OSMW_REGION_CALL_SITE;
    claim.label = "attest-input-sample";
    if (api->claim_region(owner, &claim, &g_attest_input_claim) != OSMW_OK) {
        g_attest_input_byte = NULL;
        g_attest_input_original = NULL;
        return FALSE;
    }

    memcpy(g_attest_input_saved, site, sizeof(g_attest_input_saved));
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &protect)) {
        (void)api->release_region(owner, g_attest_input_claim);
        g_attest_input_claim = OSMW_INVALID_TOKEN;
        g_attest_input_byte = NULL;
        g_attest_input_original = NULL;
        return FALSE;
    }
    displacement = (LONG)((BYTE *)attest_input_detour - (site + 5));
    memcpy(site + 1, &displacement, sizeof(displacement));
    (void)VirtualProtect(site, 5, protect, &protect);
    (void)FlushInstructionCache(GetCurrentProcess(), site, 5);

    g_attest_input_call_site = site;
    g_attest_input_ready = TRUE;
    return TRUE;
}

static void attest_unhook_input(OSMW_OWNER owner, const OSMW_CORE_API_V1 *api)
{
    DWORD protect = 0;

    if (g_attest_input_call_site != NULL) {
        /* Restore before releasing: the claim compares the live bytes against its snapshot
         * and refuses the release while they still differ. */
        if (VirtualProtect(g_attest_input_call_site, 5, PAGE_EXECUTE_READWRITE, &protect)) {
            memcpy(g_attest_input_call_site, g_attest_input_saved,
                   sizeof(g_attest_input_saved));
            (void)VirtualProtect(g_attest_input_call_site, 5, protect, &protect);
            (void)FlushInstructionCache(GetCurrentProcess(), g_attest_input_call_site, 5);
        }
        g_attest_input_call_site = NULL;
    }
    if (g_attest_input_claim != OSMW_INVALID_TOKEN && api != NULL &&
            api->release_region != NULL) {
        (void)api->release_region(owner, g_attest_input_claim);
        g_attest_input_claim = OSMW_INVALID_TOKEN;
    }
    g_attest_input_ready = FALSE;
    g_attest_input_byte = NULL;
    g_attest_input_original = NULL;
    InterlockedExchange(&g_attest_input_events, 0);
}

/* Count one client-originated send and forward unchanged.
 *
 * Deliberately does nothing but increment and tail-call.  This sits on the client's
 * own send path, so any work here is paid on every packet the game sends, and any
 * mistake here breaks the game rather than the telemetry.  It never inspects, filters
 * or alters a packet.
 *
 * Note this counts core's own queued sends too.  That is correct for the ratio the
 * server computes: a module sending on the player's behalf is still an action that no
 * human input produced, which is exactly what the measure is meant to expose. */
static int __cdecl attest_send_detour(
    DWORD type, DWORD field12, const void *extra, DWORD extra_size)
{
    OSMW_SEND_PACKET_FN original =
        (OSMW_SEND_PACKET_FN)g_attest_send_original;

    InterlockedIncrement(&g_attest_actions_sent);
    if (original == NULL) {
        return 0;
    }
    return original(type, field12, extra, extra_size);
}

/* Install the counter.  Optional: a core that cannot hook SendPacket still attests,
 * it just reports zero actions, which the server ignores below its minimum sample. */
static BOOL attest_hook_send(OSMW_OWNER owner)
{
    /* The installer compares the live bytes against the pristine image before
     * patching, so it needs those bytes and refuses without them. They are only
     * reachable inside hook.c, which is why this goes through the shared helper
     * rather than calling the installer directly. */
    if (osmw_hook_install_shared(owner, OSMW_SHARED_SEND_PACKET, &g_attest_send_hook,
                                 (void *)attest_send_detour,
                                 "attest-send-count") != OSMW_OK) {
        return FALSE;
    }
    g_attest_send_original = g_attest_send_hook.trampoline;
    return TRUE;
}

/* Build the extra payload.  Returns its size, or 0 if the report cannot be produced.
 *
 * The prefix ahead of this payload is SIXTEEN bytes, not twelve.  queue_game_packet writes
 * its field12 argument at wire offset 12 - the parameter is named for the offset - and the
 * extra begins immediately after it, at 16.  So buffer[0] here is wire offset 16, and the
 * protocol goes out twice: once as field12, once as the first dword of this payload.
 *
 * That off-by-four was believed to be twelve for this protocol first outing, and it cost a
 * debugging session on the server side.  Nothing failed loudly: the server read every field
 * four bytes early, got a garbage module count, failed its strict length check and dropped
 * the report in silence - which is indistinguishable from a player running no mod at all.
 * Both logs already held the answer, 96 bytes queued against 112 bytes received.
 *
 * Offsets in the comments below are absolute wire offsets, so they can be read straight
 * against the spec and the TypeScript parser with no arithmetic.
 */
static DWORD build_payload(BYTE *buffer, DWORD capacity)
{
    OSMW_IMAGE_VIEW image;
    DWORD module_total;
    DWORD written = 0;
    DWORD flags = 0;
    DWORD index;
    DWORD offset;

    if (capacity < OSMW_ATTEST_HEADER_BYTES) {
        return 0;
    }
    if (!osmw_image_get_view(&image)) {
        /* No pristine image means core could not read its own executable.  Reporting a
         * hash of nothing would be worse than staying quiet. */
        return 0;
    }

    if (osmw_claims_sealed_count() > 0) {
        /* Read from the registry rather than assumed from the build: a core that supports
         * sealing but whose modules never seal is watching nothing, and must not claim
         * otherwise. */
        flags |= OSMW_ATTEST_FLAG_CLAIMS_SEALED;
    }

    if (!image.address_compatible) {
        /* Core refuses all address resolution in this state because the reference client
         * has no DYNAMICBASE, so something rebased the process. */
        flags |= OSMW_ATTEST_FLAG_BASE_MISMATCH;
    }

    /* Wire 16 protocol, 20 core ABI, 24 image hash, 28 flags, 32 tampered claims. */
    write_dword(buffer, 0, OSMW_ATTEST_PROTOCOL);
    write_dword(buffer, 4, (DWORD)OSMW_CORE_ABI_CURRENT);
    write_dword(buffer, 8, hash_bytes(image.file_data, image.file_size));
    write_dword(buffer, 16, osmw_claims_tampered_count());

    /* Wire 36 input events, 40 actions sent.
     *
     * Both counters are reported together or not at all. Zero input against a real
     * action count is exactly the bot signature the server looks for, so reporting one
     * half would make every honest client accuse itself. The capability bit says the
     * pair can be trusted; without it the server does not judge the ratio at all.
     *
     * Read-and-reset, so each report covers the interval since the last one. */
    {
        LONG input = InterlockedExchange(&g_attest_input_events, 0);
        LONG actions = InterlockedExchange(&g_attest_actions_sent, 0);

        if (g_attest_input_ready) {
            flags |= OSMW_ATTEST_FLAG_INPUT_PROVENANCE;
        } else {
            input = 0;
            actions = 0;
        }
        write_dword(buffer, 20, (DWORD)input);
        write_dword(buffer, 24, (DWORD)actions);
    }

    /* Written last: the capability bit above is part of it. */
    write_dword(buffer, 12, flags);

    module_total = osmw_loader_module_count();
    if (module_total > OSMW_ATTEST_MAX_MODULES) {
        module_total = OSMW_ATTEST_MAX_MODULES;
    }

    offset = OSMW_ATTEST_HEADER_BYTES;
    written = 0;
    for (index = 0; index < module_total; ++index) {
        OSMW_MODULE_STATUS_V1 status;

        if (offset + OSMW_ATTEST_MODULE_BYTES > capacity) {
            break;
        }
        ZeroMemory(&status, sizeof(status));
        status.struct_size = sizeof(status);
        if (osmw_loader_module_at(index, &status) != OSMW_OK) {
            continue;
        }

        write_dword(buffer, offset, hash_module_id(status.module_id));
        write_word(buffer, offset + 4, (WORD)(status.module_version & 0xFFFFu));
        buffer[offset + 6] = (BYTE)(status.state & 0xFFu);
        buffer[offset + 7] = 0; /* reserved */
        offset += OSMW_ATTEST_MODULE_BYTES;
        ++written;
    }

    /* Wire 44 module count.  Written last because entries that failed to read are skipped,
     * and the server requires the declared count to account for the payload exactly. */
    write_dword(buffer, 28, written);
    return offset;
}

static void attest_send(void)
{
    BYTE payload[OSMW_ATTEST_HEADER_BYTES +
                 OSMW_ATTEST_MAX_MODULES * OSMW_ATTEST_MODULE_BYTES];
    const OSMW_CORE_API_V1 *api;
    DWORD size;

    ZeroMemory(payload, sizeof(payload));
    size = build_payload(payload, (DWORD)sizeof(payload));
    if (size == 0) {
        return;
    }

    api = osmw_loader_api();
    if (api == NULL || api->queue_game_packet == NULL) {
        return;
    }
    /* field12 carries the protocol as well, matching the timer hello shape, so a server can
     * version-gate before it parses anything. */
    {
        OSMW_RESULT queued = api->queue_game_packet(
            g_attest_owner, OSMW_ATTEST_PACKET_TYPE, OSMW_ATTEST_PROTOCOL, payload, size);
        char message[128];
        int written;

        /* Logged once per outcome change, because a report that is built but never leaves
         * the client is indistinguishable at the server from a client that is not attesting
         * at all - which is exactly the confusion this spent a session in. */
        if (queued != g_attest_last_result || !g_attest_logged_once) {
            written = _snprintf(message, sizeof(message),
                                queued == OSMW_OK
                                    ? "attestation queued: %lu bytes, %lu module(s)"
                                    : "attestation could not be queued: result %ld, %lu bytes",
                                queued == OSMW_OK ? (unsigned long)size : (unsigned long)queued,
                                (unsigned long)((size - OSMW_ATTEST_HEADER_BYTES) /
                                                OSMW_ATTEST_MODULE_BYTES));
            if (written < 0) {
                message[sizeof(message) - 1] = 0;
            }
            api->log(g_attest_owner, OSMW_LOG_INFO, message);
            g_attest_last_result = queued;
            g_attest_logged_once = TRUE;
        }
    }
}

/*
 * Start the reporting clock when the player actually enters the world.
 *
 * The clock used to run from core startup, which is wrong by a whole login: the first
 * report fired about twenty seconds in, while the client was still on the character
 * screen, and the server drops anything that arrives without a player attached. The
 * next attempt was five minutes later, so a short session could finish having never
 * delivered a single report - which looked exactly like a client that was not attesting
 * at all.
 *
 * Only the first map data matters. Re-arming on every map change would put a report
 * behind every zone transition and push at the rate limit for no benefit.
 */
static void __cdecl attest_mapdata_observer(
    OSMW_OWNER owner, void *context, const OSMW_GAME_PACKET_V1 *packet)
{
    (void)owner;
    (void)context;
    (void)packet;

    if (g_attest_in_world) {
        return;
    }
    g_attest_in_world = TRUE;
    g_attest_started_at = GetTickCount();
    g_attest_sent_once = FALSE;
}

static void __cdecl attest_tick(OSMW_OWNER owner, void *context, LONG list_selector)
{
    DWORD now;
    DWORD elapsed;

    (void)owner;
    (void)context;
    (void)list_selector;

    if (!g_attest_ready) {
        return;
    }
    if (!g_attest_in_world) {
        /* Normally nothing is said until the player is in the world, because a report sent
         * from the character screen has no session attached and is discarded.
         *
         * The fallback matters more than the optimisation: gating solely on the map-data
         * observer would make that one subscription a single point of failure, and a client
         * that silently never reported would be indistinguishable from one with no core at
         * all. After a few minutes, report regardless and let the server decide. */
        if (GetTickCount() - g_attest_started_at < OSMW_ATTEST_WORLD_FALLBACK_MS) {
            return;
        }
    }

    /* GetTickCount wraps after ~49 days; unsigned subtraction stays correct across the
     * wrap, which is why the deltas are computed rather than compared directly. */
    now = GetTickCount();
    if (!g_attest_sent_once) {
        elapsed = now - g_attest_started_at;
        if (elapsed < OSMW_ATTEST_FIRST_DELAY_MS) {
            return;
        }
    } else {
        elapsed = now - g_attest_last_sent;
        if (elapsed < OSMW_ATTEST_INTERVAL_MS) {
            return;
        }
    }

    g_attest_last_sent = now;
    g_attest_sent_once = TRUE;
    attest_send();
}

BOOL osmw_attest_start(OSMW_OWNER core_owner)
{
    const OSMW_CORE_API_V1 *api;
    OSMW_RESULT result;

    if (g_attest_ready) {
        return TRUE;
    }
    if (core_owner == OSMW_INVALID_OWNER) {
        return FALSE;
    }

    g_attest_owner = core_owner;
    g_attest_started_at = GetTickCount();
    g_attest_last_sent = g_attest_started_at;
    g_attest_sent_once = FALSE;

    /* Priority 0 alongside the feature modules: nothing here has to run before or after
     * any of them, and the work is a couple of comparisons on most ticks. */
    api = osmw_loader_api();
    if (api == NULL || api->subscribe_post_ui_tick == NULL) {
        return FALSE;
    }
    if (api->observe_game_packet_after != NULL) {
        (void)api->observe_game_packet_after(
            core_owner, OSMW_ATTEST_MAPDATA_TYPE, 0, attest_mapdata_observer, NULL,
            &g_attest_mapdata_token);
    }

    result = api->subscribe_post_ui_tick(
        core_owner, 0, attest_tick, NULL, &g_attest_tick_token);
    if (result != OSMW_OK) {
        /* A core without a UiTick hook still loads modules and serves signatures.
         * Attestation is optional by design, so its absence is not an error. */
        g_attest_tick_token = OSMW_INVALID_TOKEN;
        return FALSE;
    }

    /* Both optional. Losing either only costs the ratio, which the server then skips.
     *
     * Logged either way, because from the server a silent miss and a working hook look
     * identical - both end with nothing flagged - so without these lines there is no way
     * to tell whether input provenance is actually running. */
    api->log(core_owner, OSMW_LOG_INFO,
             attest_hook_send(core_owner)
                 ? "action counting active: SendPacket hooked"
                 : "action counting unavailable: SendPacket not hooked");
    if (attest_hook_input(core_owner, api)) {
        api->log(core_owner, OSMW_LOG_INFO,
                 "input provenance active: sampler call redirected");
    } else {
        g_attest_input_ready = FALSE;
        api->log(core_owner, OSMW_LOG_INFO,
                 "input provenance unavailable; action counts will not be reported");
    }

    g_attest_ready = TRUE;
    return TRUE;
}

void osmw_attest_stop(void)
{
    if (!g_attest_ready) {
        return;
    }
    g_attest_ready = FALSE;
    if (g_attest_tick_token != OSMW_INVALID_TOKEN) {
        const OSMW_CORE_API_V1 *api = osmw_loader_api();
        if (api != NULL && api->unsubscribe != NULL) {
            (void)api->unsubscribe(g_attest_owner, g_attest_tick_token);
        }
        g_attest_tick_token = OSMW_INVALID_TOKEN;
    }
    if (g_attest_mapdata_token != OSMW_INVALID_TOKEN) {
        const OSMW_CORE_API_V1 *stop_api = osmw_loader_api();
        if (stop_api != NULL && stop_api->unsubscribe != NULL) {
            (void)stop_api->unsubscribe(g_attest_owner, g_attest_mapdata_token);
        }
        g_attest_mapdata_token = OSMW_INVALID_TOKEN;
    }
    g_attest_in_world = FALSE;

    attest_unhook_input(g_attest_owner, osmw_loader_api());

    if (osmw_hook_is_active(&g_attest_send_hook)) {
        /* Restore before the owner goes away: leaving a detour pointing into a core
         * that is about to unmap is how an eject turns into a crash. */
        (void)osmw_hook_remove(&g_attest_send_hook);
    }
    g_attest_send_original = NULL;
    InterlockedExchange(&g_attest_actions_sent, 0);
    g_attest_owner = OSMW_INVALID_OWNER;
    g_attest_sent_once = FALSE;
}

/* Test seam: build a payload without a live UiTick or send queue. */
DWORD osmw_attest_build_payload_for_test(BYTE *buffer, DWORD capacity)
{
    return build_payload(buffer, capacity);
}
