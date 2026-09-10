/* Scale the channel chips with the message font.
 *
 * The client draws ten chips into a vertical atlas in the composite DC at
 * chatobj+0x4C -- 38x18 cells on a 19px pitch -- and records each channel's Y
 * offset in chatobj+0x58 + channel*4. Three places then blit a 38x18 cell out
 * of that atlas onto each line.
 *
 * Those offsets being DATA is what makes this tractable: the pitch needs no
 * code patch, only the three blit heights do. Width is deliberately left at
 * 38, because changing it would also mean moving the text origin, which is
 * hardcoded at nine further sites for a gain nobody would notice.
 *
 * Rather than reimplement the client's chip drawing -- which would mean
 * hardcoding ten colours and ten labels, and getting them wrong on any
 * non-English build -- the original atlas is snapshotted and stretched. The
 * chips keep whatever the client drew, at a taller cell.
 */
#include "osmw_chat.h"

#include <stdio.h>

#define OBJ_CHIP_DC     0x4C
#define OBJ_CHIP_Y      0x58        /* ten dwords, one per channel */

#define CHANNEL_COUNT   10
#define CHIP_W          38          /* fixed; see the header comment */
#define CHIP_H_BASE     18
#define CHIP_PITCH_BASE 19
#define CHIP_H_MIN      14
#define CHIP_H_MAX      40          /* the blit height is a push imm8 */

typedef void (__fastcall *chipatlas_fn)(void *ecx, void *edx);

/* Each blit site, anchored to a signature-resolved function so a moved
 * client cannot leave us patching arbitrary bytes. Every one is `push imm8`,
 * so the height sits one byte in. */
typedef struct {
    int  base;                      /* MW_* index of the anchor */
    int  offset;                    /* bytes from the anchor */
    unsigned char expect[2];        /* what must be there: push 0x12 */
} blit_site_t;

static const blit_site_t BLIT_SITES[] = {
    { MW_RENDER,    0x367, { 0x6A, CHIP_H_BASE } },
    { MW_RENDER,    0x58D, { 0x6A, CHIP_H_BASE } },
    { MW_DRAWLINES, 0x062, { 0x6A, CHIP_H_BASE } },
};
#define BLIT_SITE_COUNT ((int)(sizeof(BLIT_SITES) / sizeof(BLIT_SITES[0])))

static mw_hook_t     g_atlas_hook;
static OSMW_TOKEN    g_blit_claims[BLIT_SITE_COUNT];
static chipatlas_fn  g_atlas_orig;
static int           g_installed;
static int           g_applied_height;

/* A pristine copy of the atlas as the client draws it, so every rescale
 * stretches from the original rather than from the last scaled version. */
static HDC     g_snapshot_dc;
static HBITMAP g_snapshot_bmp;

/* Desired chip height: tall enough to sit comfortably beside the message text,
 * clamped to what the atlas and a push imm8 can carry. */
static int wanted_height(void)
{
    int configured = mw_cfg_int("ChipHeight", 0);
    int size;

    if (configured > 0)
        return configured < CHIP_H_MIN ? CHIP_H_MIN
             : configured > CHIP_H_MAX ? CHIP_H_MAX : configured;

    /* Auto: follow the message font, since that is what the chip sits next
     * to. FontSize 0 means the client's own 16, which is the stock look. */
    size = mw_cfg_int("FontSize", 0);
    if (size <= 0)
        return CHIP_H_BASE;
    size = size + 2;
    if (size < CHIP_H_BASE)
        size = CHIP_H_BASE;         /* never smaller than the client's own */
    if (size > CHIP_H_MAX)
        size = CHIP_H_MAX;
    return size;
}

static unsigned char *site_address(const blit_site_t *site)
{
    unsigned char *anchor = mw_addr[site->base];
    return anchor ? anchor + site->offset : NULL;
}

/* Patch the three blit heights. Every site is verified against the byte it is
 * expected to hold, so a build we do not recognise is left alone. */
static int patch_blit_height(int height)
{
    unsigned char patch[2];
    int i;

    for (i = 0; i < BLIT_SITE_COUNT; i++) {
        unsigned char *at = site_address(&BLIT_SITES[i]);
        if (!at || at[0] != BLIT_SITES[i].expect[0]) {
            mw_log("chips: site %d does not look like `push imm8`, standing down", i);
            return 0;
        }
    }

    patch[0] = 0x6A;
    patch[1] = (unsigned char)height;
    for (i = 0; i < BLIT_SITE_COUNT; i++)
        if (!mw_write_bytes(site_address(&BLIT_SITES[i]), patch, 2))
            return 0;
    return 1;
}

static int claim_blit_sites(void)
{
    static const char *labels[BLIT_SITE_COUNT] = {
        "chat render chip-height immediate 1",
        "chat render chip-height immediate 2",
        "chat draw-lines chip-height immediate"
    };
    int i;

    for (i = 0; i < BLIT_SITE_COUNT; ++i) {
        unsigned char *at = site_address(&BLIT_SITES[i]);
        OSMW_RESULT result;
        if (at == NULL || memcmp(at, BLIT_SITES[i].expect, 2) != 0) {
            mw_log("chips: site %d bytes differ; refusing claims", i);
            break;
        }
        result = mw_claim_region(at, 2, OSMW_REGION_CODE, labels[i],
                                 &g_blit_claims[i]);
        if (result != OSMW_OK) {
            mw_log("chips: site %d claim failed (%ld)", i, (long)result);
            break;
        }
    }
    if (i == BLIT_SITE_COUNT)
        return 1;
    while (i-- > 0)
        (void)mw_release_region(&g_blit_claims[i]);
    return 0;
}

static void take_snapshot(HDC atlas_dc)
{
    if (g_snapshot_dc)
        return;
    g_snapshot_dc = CreateCompatibleDC(atlas_dc);
    if (!g_snapshot_dc)
        return;
    g_snapshot_bmp = CreateCompatibleBitmap(atlas_dc, CHIP_W,
                                            CHIP_PITCH_BASE * CHANNEL_COUNT);
    if (!g_snapshot_bmp) {
        DeleteDC(g_snapshot_dc);
        g_snapshot_dc = NULL;
        return;
    }
    SelectObject(g_snapshot_dc, g_snapshot_bmp);
}

/* Stretch the snapshotted chips back into the atlas at a taller cell, and
 * republish each channel's Y offset. */
static void rescale(unsigned char *obj, HDC atlas_dc, int height)
{
    int pitch = height + 1;
    int old_mode, ch;

    if (!g_snapshot_dc)
        return;

    /* COLORONCOLOR keeps the flat chip fills clean; the default would drop
     * rows and leave the frames broken. */
    old_mode = SetStretchBltMode(atlas_dc, COLORONCOLOR);

    for (ch = 0; ch < CHANNEL_COUNT; ch++) {
        StretchBlt(atlas_dc, 0, ch * pitch, CHIP_W, height,
                   g_snapshot_dc, 0, ch * CHIP_PITCH_BASE, CHIP_W, CHIP_H_BASE,
                   SRCCOPY);
        *(int *)(obj + OBJ_CHIP_Y + ch * 4) = ch * pitch;
    }

    SetStretchBltMode(atlas_dc, old_mode);
}

/* Runs right after the client has rebuilt its atlas, so the snapshot is taken
 * from a freshly drawn, unscaled one. */
static void __fastcall atlas_detour(void *self, void *edx)
{
    unsigned char *obj = (unsigned char *)self;
    HDC   atlas_dc;
    int   height;

    g_atlas_orig(self, edx);

    atlas_dc = *(HDC *)(obj + OBJ_CHIP_DC);
    if (!atlas_dc)
        return;

    take_snapshot(atlas_dc);
    if (g_snapshot_dc)
        BitBlt(g_snapshot_dc, 0, 0, CHIP_W, CHIP_PITCH_BASE * CHANNEL_COUNT,
               atlas_dc, 0, 0, SRCCOPY);

    height = wanted_height();
    if (height == CHIP_H_BASE) {
        if (g_applied_height && g_applied_height != CHIP_H_BASE) {
            patch_blit_height(CHIP_H_BASE);
            mw_log("chips: back to the client's own %dpx cell", CHIP_H_BASE);
        }
        g_applied_height = CHIP_H_BASE;
        return;                     /* nothing to scale */
    }

    rescale(obj, atlas_dc, height);
    if (height != g_applied_height && patch_blit_height(height)) {
        mw_log("chips: cell scaled to %dx%d (pitch %d)", CHIP_W, height, height + 1);
        g_applied_height = height;
    }
}

int mw_chips_install(void)
{
    if (g_installed)
        return 1;
    if (!mw_addr[MW_CHIPATLAS] || !mw_addr[MW_RENDER] || !mw_addr[MW_DRAWLINES]) {
        mw_log("chips: anchors unresolved, not installing");
        return 0;
    }

    if (!claim_blit_sites())
        return 0;

    /* prologue is `sub esp,0x6c / push ebx / push ebp` -- exactly 5 bytes */
    if (!mw_hook_install(&g_atlas_hook, mw_addr[MW_CHIPATLAS],
                         (void *)(UINT_PTR)atlas_detour, 5,
                         "chat chip-atlas prologue")) {
        mw_log("chips: could not hook the atlas builder");
        {
            int i;
            for (i = BLIT_SITE_COUNT - 1; i >= 0; --i)
                (void)mw_release_region(&g_blit_claims[i]);
        }
        return 0;
    }
    g_atlas_orig = (chipatlas_fn)g_atlas_hook.trampoline;

    g_installed = 1;
    g_applied_height = CHIP_H_BASE;
    mw_log("chips: installed (ChipHeight, 0 = follow the message font)");
    return 1;
}

OSMW_RESULT mw_chips_remove(void)
{
    OSMW_RESULT result;
    int i;

    if (!g_installed)
        return OSMW_OK;

    /* Put the blit heights back before the hook goes, or the client keeps
     * blitting a cell size whose atlas we are about to stop maintaining. */
    if (g_applied_height != CHIP_H_BASE &&
            !patch_blit_height(CHIP_H_BASE))
        return OSMW_E_INTERNAL;

    result = mw_hook_remove(&g_atlas_hook);
    if (result != OSMW_OK)
        return result;

    for (i = BLIT_SITE_COUNT - 1; i >= 0; --i) {
        result = mw_release_region(&g_blit_claims[i]);
        if (result != OSMW_OK)
            return result;
    }

    if (g_snapshot_bmp) {
        DeleteObject(g_snapshot_bmp);
        g_snapshot_bmp = NULL;
    }
    if (g_snapshot_dc) {
        DeleteDC(g_snapshot_dc);
        g_snapshot_dc = NULL;
    }
    g_installed = 0;
    mw_log("chips: removed");
    return OSMW_OK;
}
