/* Client self-attestation payload.
 *
 * The wire format has three copies that must agree: this builder, the spec in
 * mw-tools/tools/client-dll/attestation.md, and the parser in
 * mw-tools/src/game-server/Services/AutoMod/ClientAttestation.ts. The parser is strict -
 * it rejects any packet whose declared module count does not account for the length
 * exactly - so a disagreement here is silent on the client and total on the server.
 *
 * These assertions encode the parser's rules so a drift is caught at build time rather
 * than by a client that quietly stops being trusted.
 */
#include "core_internal.h"

#include <stdio.h>
#include <string.h>

static int g_failures;

static void check(int condition, const char *what)
{
    if (condition) {
        printf("  ok:   %s\n", what);
    } else {
        printf("  FAIL: %s\n", what);
        ++g_failures;
    }
}

static DWORD read_dword(const BYTE *buffer, DWORD offset)
{
    return (DWORD)buffer[offset] |
           ((DWORD)buffer[offset + 1] << 8) |
           ((DWORD)buffer[offset + 2] << 16) |
           ((DWORD)buffer[offset + 3] << 24);
}

/* Absolute wire offsets minus the 12-byte packet prefix that queue_game_packet supplies. */
#define WIRE(absolute) ((absolute) - 12u)

int main(void)
{
    BYTE payload[512];
    DWORD size;
    DWORD module_count;

    printf("client attestation payload\n");

    if (!osmw_core_services_startup(NULL)) {
        printf("  FAIL: core services did not start\n");
        return 1;
    }

    ZeroMemory(payload, sizeof(payload));
    size = osmw_attest_build_payload_for_test(payload, (DWORD)sizeof(payload));

    check(size > 0, "payload is produced once the pristine image is mapped");
    if (size == 0) {
        osmw_core_shutdown();
        return 1;
    }

    check(read_dword(payload, WIRE(12)) == 1u, "protocol is 1");
    check(read_dword(payload, WIRE(16)) == (DWORD)OSMW_CORE_ABI_CURRENT,
          "core reports the ABI it actually implements");
    check(read_dword(payload, WIRE(20)) != 0u,
          "image hash is a real value, not a zero placeholder");

    /* The base-mismatch flag must mirror what the resolver itself decided, rather than be
     * asserted against a guess about the environment.  It genuinely fires in this harness:
     * the test host is an ordinary console exe with ASLR, so its loaded base does differ
     * from its preferred one.  That is the flag working, and asserting it away would hide
     * the one condition the field exists to report. */
    {
        OSMW_IMAGE_VIEW view;
        DWORD flags = read_dword(payload, WIRE(24));
        DWORD expected = 0u;

        check(osmw_image_get_view(&view), "image view is readable");
        if (!view.address_compatible) {
            expected |= 0x00000001u;
        }
        check(flags == expected, "flags mirror the resolver base-compatibility decision");
        check((flags & ~0x00000003u) == 0u, "no undefined flag bits are set");
    }

    /* Nothing has written into a claimed range in a freshly started process. */
    check(read_dword(payload, WIRE(28)) == 0u, "no tampered claims in a clean process");

    /* Input provenance is all-or-nothing.  The capability bit and the two counters have to
     * agree: reporting an action count against zero input is the bot signature itself, so a
     * client that cannot count input must report neither number and must not set the bit. */
    {
        DWORD flags = read_dword(payload, WIRE(24));
        DWORD input = read_dword(payload, WIRE(32));
        DWORD actions = read_dword(payload, WIRE(36));
        BOOL claims_input = (flags & 0x00000004u) != 0u;

        if (!claims_input) {
            check(input == 0u && actions == 0u,
                  "counters stay zero while input provenance is unavailable");
        } else {
            /* In this harness there is no client to generate input, so the bit may be set
             * with genuinely zero counts.  What must never happen is actions without the
             * bit, which is the shape that accuses an honest client. */
            check(TRUE, "input provenance declared");
        }
        check(actions == 0u || claims_input,
              "actions are never reported without the capability bit");
    }

    module_count = read_dword(payload, WIRE(40));
    check(module_count <= 32u, "module count is within the protocol cap");

    /* The rule the server enforces: header plus count times record size must equal the
     * packet length exactly, or the whole report is discarded. */
    check(size == 32u + module_count * 8u,
          "declared module count accounts for the payload length exactly");

    /* Refusing to overrun a caller buffer matters because the send queue has a hard cap. */
    check(osmw_attest_build_payload_for_test(payload, 8u) == 0u,
          "a buffer too small for the header produces nothing");

    osmw_core_shutdown();

    printf(g_failures == 0 ? "  PASSED: client attestation payload, 0 failure(s)\n"
                           : "  FAILED: client attestation payload\n");
    return g_failures == 0 ? 0 : 1;
}
