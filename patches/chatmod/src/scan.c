/* Resolve every signature in sigs.h against the host image's .text.
 *
 * The DLL never trusts offsets_cache.ini -- that file is for the analysis
 * tooling. Here we rescan, and require exactly one match per required
 * pattern. A required signature that matches zero times (client updated) or
 * more than once (pattern no longer specific) is a hard failure for that
 * address, which leaves mw_addr[] NULL and stops the corresponding feature
 * from installing. Optional signatures (mw_sig_t.optional) may miss without
 * counting toward the stand-down total.
 */
#include "osmw_chat.h"

unsigned char *mw_addr[MW_ADDR_COUNT];
unsigned char *mw_base;
unsigned int   mw_timestamp;

static int match_at(const unsigned char *p, const mw_sig_t *s)
{
    int i;
    for (i = 0; i < s->length; i++) {
        if (s->mask[i] == 'x' && p[i] != s->pattern[i])
            return 0;
    }
    return 1;
}

/* Locate .text so we scan code only -- scanning .data invites false hits. */
static int find_text(unsigned char *base, unsigned char **out_lo, unsigned char **out_hi)
{
    IMAGE_DOS_HEADER     *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS32   *nt;
    IMAGE_SECTION_HEADER *sec;
    int i;

    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return 0;
    nt = (IMAGE_NT_HEADERS32 *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return 0;

    mw_timestamp = nt->FileHeader.TimeDateStamp;

    sec = IMAGE_FIRST_SECTION(nt);
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        if (sec->Name[0] == '.' && sec->Name[1] == 't' &&
            sec->Name[2] == 'e' && sec->Name[3] == 'x' && sec->Name[4] == 't') {
            DWORD size = sec->Misc.VirtualSize;
            if (size == 0 || size > sec->SizeOfRawData)
                size = sec->SizeOfRawData;
            *out_lo = base + sec->VirtualAddress;
            *out_hi = base + sec->VirtualAddress + size;
            return 1;
        }
    }
    return 0;
}

int mw_resolve_all(void)
{
    unsigned char *lo, *hi, *p;
    int failed = 0;
    int i;

    mw_base = (unsigned char *)GetModuleHandleA(NULL);
    if (!mw_base || !find_text(mw_base, &lo, &hi)) {
        mw_log("scan: cannot locate .text in the host image");
        return MW_ADDR_COUNT;
    }
    mw_log("scan: base %p  .text %p..%p  build 0x%08X",
           mw_base, lo, hi, mw_timestamp);

    for (i = 0; i < MW_SIG_COUNT; i++) {
        const mw_sig_t *s = &MW_SIGS[i];
        unsigned char  *hit = NULL;
        int             count = 0;

        for (p = lo; p <= hi - s->length; p++) {
            if (match_at(p, s)) {
                if (++count == 1)
                    hit = p;
                else
                    break;      /* ambiguous; no point counting further */
            }
        }

        if (count != 1) {
            mw_log("scan: %-12s FAILED (%d matches, expected 1)%s",
                   s->name, count, s->optional ? "  (optional)" : "");
            mw_addr[i] = NULL;
            if (!s->optional)
                failed++;
            continue;
        }

        if (s->op_offset >= 0) {
            /* the pattern points at an instruction that encodes the address */
            mw_addr[i] = (unsigned char *)(*(DWORD *)(hit + s->op_offset));
        } else {
            mw_addr[i] = hit;
        }

        {
            DWORD rva = (DWORD)(mw_addr[i] - mw_base);
            mw_log("scan: %-12s %p  rva 0x%08X%s", s->name, mw_addr[i], rva,
                   rva == s->reference_rva ? "" : "  (differs from reference build)");
        }
    }

    return failed;
}
