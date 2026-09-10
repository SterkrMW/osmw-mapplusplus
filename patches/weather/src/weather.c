/* Sandstorm, ash fall, blizzard, ember updraft and fog.
 *
 * Every hook is a call-site patch, so no prologue is relocated and no
 * executable memory is allocated. Two of them carry the effects:
 *
 *   1. Every `call SetWeather` site is repointed at wx_setweather_detour.
 *      SetWeather (0x46f950) masks the code to bits 0-3 before it stores
 *      anything, so the detour is the only place the full dword is still
 *      visible. It records our bits and the intensity, then calls the real
 *      function, which is left completely untouched.
 *
 *   2. The single `call WeatherDraw` site inside the render pass (0x4491b9)
 *      is repointed at wx_draw_detour, which calls the real draw first and
 *      then layers our own groups over the stock ones.
 *
 * Three more are cosmetic and all optional: the HUD icon picker's single call
 * site, and the single call site of each of the two weather-name builders
 * (the hover tooltip and the chat status line), which otherwise say "Sunny"
 * through every one of our groups. Each stands itself down on its own without
 * touching the effects.
 *
 * Everything after install runs on the client's own render thread, called
 * from those two detours. There is no worker thread and no timer: the draw
 * detour is the tick, which is exactly the rate the stock groups animate at.
 *
 * Why the draw site and not the function: the site sits inside the block
 * guarded by `cmp byte [0x5e1540], bl` at 0x4491aa, the client's own "draw
 * weather at all" gate. Hooking there inherits that gate for free, and the
 * three low-spec refusal flags are upstream of us as well -- they zero the
 * code in the packet buffer before SetWeather ever sees it, so on a slow
 * machine our groups stand down with the client's.
 *
 * Two particle engines cover four of the five groups. A STREAK is a fast
 * near-horizontal line (sandstorm, blizzard); a MOTE is a slow drifting point
 * that falls or rises (ash, embers). Sand and blizzard differ only in their
 * parameter block, and so do ash and embers -- which is the whole reason
 * adding a weather is cheap. FOG and DARKNESS are neither: they are
 * full-screen alpha blends we do ourselves, because the client has no alpha
 * to borrow, and they are two instances of one tint pass rather than two
 * copies of it.
 */
#include "osmw_weather.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* ---- the client's own routines ------------------------------------------ */

typedef void           (__cdecl *wx_setweather_fn)(unsigned int code);
typedef void           (__cdecl *wx_draw_fn)(void *surface);
typedef void           (__cdecl *wx_drawline_fn)(int x1, int y1, int x2, int y2,
                                                 int colour, void *surface);
typedef void           (__cdecl *wx_drawpixel_fn)(int x, int y, int colour,
                                                  void *surface);
typedef unsigned short (__cdecl *wx_colourconv_fn)(unsigned int rgb);
typedef int            (__cdecl *wx_iconpick_fn)(void);
/* find(name, bank) -- NOT (bank, name). The bank is arg2: 0x466b38 reads it
 * from [esp+0x14] after three pushes and scales it by 0x144 into the table
 * at 0x743b1c, while arg1 is what reaches _strcmpi. It is an INDEX, not a
 * pointer, which is why AniLoad writes -1 into the global on failure. */
typedef int            (__cdecl *wx_animfind_fn)(const char *name, int bank);

static wx_setweather_fn   g_setweather;
static wx_draw_fn         g_draw;
static wx_drawline_fn     g_drawline;
static wx_drawpixel_fn    g_drawpixel;

/* ColourConv resolves to the ADDRESS OF a function pointer, not to a
 * function: the client keeps its RGB888 -> native16 converter in a global at
 * 0x7e6920 and calls it indirectly. The slot is filled during video init, so
 * it can still be null when we install; every use null-checks it. */
static wx_colourconv_fn  *g_colourconv;

/* The stock icon picker, called when none of our groups is running. */
static wx_iconpick_fn     g_iconpick;

/* Both weather-name builders: void __cdecl(char *dest). Called when none of
 * our groups is running, and called into a scratch buffer when one is, so the
 * client's own wind word, map name and markup all come from the client. */
typedef void           (__cdecl *wx_namebuild_fn)(char *dest);
static wx_namebuild_fn    g_tooltip_orig;
static wx_namebuild_fn    g_status_orig;
/* The client's own weather mask, read out of the picker's first instruction
 * rather than hardcoded. Only the icon path needs it. */
static unsigned int      *g_client_mask;

/* ---- state --------------------------------------------------------------- */

/* Our groups, as WX_BIT_*. Deliberately NOT stored in the client's mask word
 * at 0x7decb0: the HUD weather-icon picker at 0x46f9b0 is an equality chain
 * over that word with no default, so a bit it has no case for falls through
 * to icon 7 and would relabel our weather as thunder and clouds. Keeping our
 * bits here means the stock icon stays correct. */
static unsigned int g_mask;
static int          g_level;        /* 0 light, 1 medium, 2 heavy */

typedef struct {
    int            x, y;            /* position, in pixels */
    int            vx, vy;          /* velocity, in pixels per frame */
    int            len;             /* streak stretch percent; 0 = a point */
    unsigned char  phase;           /* sway / pulse position, wraps at 256 */
    unsigned char  rate;            /* how fast this particle's phase moves */
    unsigned short colour;
    unsigned short colour_alt;      /* pulse swaps between the two */
} wx_particle_t;

/* ---- a private RNG -------------------------------------------------------
 *
 * xorshift32 rather than rand(). The client seeds and consumes the CRT
 * generator on its own schedule -- the thunder reschedule is `rand()%50+600`
 * -- and a mod that pulls from the same stream perturbs client behaviour that
 * has nothing to do with weather. This costs four bytes of state.
 */
static unsigned int g_rng = 0x9E3779B9u;

static unsigned int wx_rand(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

static int wx_rand_range(int lo, int hi)
{
    if (hi <= lo)
        return lo;
    return lo + (int)(wx_rand() % (unsigned int)(hi - lo + 1));
}

/* sin scaled to +/-100, 32 steps. Integer only: the client is a 2006 binary
 * and this runs inside its render loop, so nothing here touches the FPU. */
static const signed char g_sin32[32] = {
       0,   20,   38,   56,   71,   83,   92,   98,
     100,   98,   92,   83,   71,   56,   38,   20,
       0,  -20,  -38,  -56,  -71,  -83,  -92,  -98,
    -100,  -98,  -92,  -83,  -71,  -56,  -38,  -20,
};

static int wx_sin(unsigned int phase)
{
    return g_sin32[(phase >> 3) & 31];      /* 256 phase steps -> 32 entries */
}

/* ---- the streak engine ---------------------------------------------------
 *
 * Fast, near-horizontal, driven by a wind speed. `sag` is how much vertical
 * drift a streak may pick up: sand blows flat, snow in a blizzard falls while
 * it blows, and that one number is most of the difference between them.
 */
typedef struct {
    unsigned int    bit;
    wx_particle_t  *pool;
    int             live;
    int             wind;           /* horizontal px/frame, negative = left */
    int             wind_var;       /* random spread on wind */
    int             fall_min;       /* vertical px/frame */
    int             fall_max;
    int             stretch;        /* streak length as a percent of one
                                     * frame's travel; 100 = motion blur */
    unsigned short *colour_a;
    unsigned short *colour_b;
} wx_streak_t;

/* ---- the mote engine -----------------------------------------------------
 *
 * Slow, wandering, and signed: a positive vy range falls (ash), a negative
 * one rises (embers off lava). `special` is the one-in-N accent colour that
 * stops a field of grey reading as snow.
 */
typedef struct {
    unsigned int    bit;
    wx_particle_t  *pool;
    int             live;
    int             vy_min, vy_max; /* may both be negative to rise */
    int             drift;          /* sideways wander */
    int             sway;           /* horizontal oscillation amplitude */
    int             pulse;          /* 1 = swap colours on the particle phase */
    int             special_odds;   /* one in N particles takes colour_s */
    /* Base colours, picked from uniformly. Pointers rather than values
     * because the conversion to the surface's format only happens once the
     * client's video is up, long after these tables are built. */
    unsigned short **palette;
    int              palette_n;
    unsigned short  *colour_s;      /* rare accent */
    unsigned short  *colour_dim;    /* pulse swaps to this; NULL if no pulse */
} wx_mote_t;

static wx_particle_t g_pool_streak[WX_STREAK_GROUPS][WX_MAX_STREAK];
static wx_particle_t g_pool_mote[WX_MOTE_GROUPS][WX_MAX_MOTE];

/* Converted once, on the first draw that has a usable converter. */
static unsigned short g_c_sand_pale, g_c_sand_deep;
static unsigned short g_c_ash_grey, g_c_ash_dark, g_c_ember;
static unsigned short g_c_snow_white, g_c_snow_blue;
static unsigned short g_c_ember_hot, g_c_ember_dim;
static unsigned short g_c_fog, g_c_dark;
static unsigned short g_c_blood, g_c_blood_dark;
static unsigned short g_c_fly, g_c_fly_dim, g_c_fly_hot;
static unsigned short g_c_leaf_green, g_c_leaf_lime, g_c_leaf_olive;
static unsigned short g_c_leaf_deep, g_c_leaf_gold, g_c_leaf_red;
static int            g_colours_ready;

/*                bit              pool               live wind var fall     stretch */
static wx_streak_t g_streaks[WX_STREAK_GROUPS] = {
    { WX_BIT_SAND,     g_pool_streak[0], 0, -14, 4,  0,  2, 100,
      &g_c_sand_pale,  &g_c_sand_deep },
    { WX_BIT_BLIZZARD, g_pool_streak[1], 0, -18, 4,  2,  6, 100,
      &g_c_snow_white, &g_c_snow_blue },
    /* Blood rain is the same engine turned upright: a small sideways
     * component and a large fall, so the streak the motion blur draws is
     * near-vertical instead of near-horizontal. */
    { WX_BIT_BLOOD,    g_pool_streak[2], 0,  -3, 2, 14, 22, 110,
      &g_c_blood,      &g_c_blood_dark },
};

/* Base palettes. Four greens and one gold for leaves; the others keep the two
 * they had, which is all a field of one material needs. */
static unsigned short *g_pal_ash[]   = { &g_c_ash_grey, &g_c_ash_dark };
static unsigned short *g_pal_ember[] = { &g_c_ember_dim, &g_c_ember };
static unsigned short *g_pal_fly[]   = { &g_c_fly };
static unsigned short *g_pal_leaf[]  = { &g_c_leaf_green, &g_c_leaf_lime,
                                         &g_c_leaf_olive, &g_c_leaf_deep,
                                         &g_c_leaf_gold };

/*              bit            pool            live vymin vymax drift sway pulse odds */
static wx_mote_t g_motes[WX_MOTE_GROUPS] = {
    { WX_BIT_ASH,     g_pool_mote[0], 0,  1,  3, 1,  0, 0, 40,
      g_pal_ash,   2, &g_c_ember,     NULL },
    { WX_BIT_EMBER,   g_pool_mote[1], 0, -3, -1, 2,  0, 0, 12,
      g_pal_ember, 2, &g_c_ember_hot, NULL },
    /* Fireflies hover: the vy range straddles zero, which the spawn code
     * treats as permission to sit still rather than a value to correct. */
    { WX_BIT_FIREFLY, g_pool_mote[2], 0, -1,  1, 1, 20, 1,  9,
      g_pal_fly,   1, &g_c_fly_hot,   &g_c_fly_dim },
    /* Leaves fall slowly and swing hard. The sway is what separates a leaf
     * from a flake, and the green palette is what separates it from an ember:
     * the red accent is rare (1 in 16) because a few turned leaves make the
     * rest look green, while many make the whole field look like fire. */
    { WX_BIT_LEAVES,  g_pool_mote[3], 0,  1,  2, 1, 46, 0, 16,
      g_pal_leaf,  5, &g_c_leaf_red,  NULL },
};

/* Tunables, read once at install. */
static int g_sand_wind = 14;
static int g_ash_drift = 1;
static int g_gust_depth = WX_GUST_DEPTH_DEFAULT;

/* Gust phase, advanced once per drawn frame. Shared by every streak group so
 * a sandstorm and a blood rain surge together rather than beating against
 * each other. */
static unsigned int g_gust_frame;

/* Density multiplier for this frame, as a percent. 100 with gusting off. */
static int wx_gust_percent(void)
{
    int swing;

    if (g_gust_depth <= 0)
        return 100;
    /* One full cycle every WX_GUST_PERIOD frames. */
    swing = wx_sin((g_gust_frame * 256u) / WX_GUST_PERIOD);
    return 100 + (g_gust_depth * swing) / 100;
}

/* The surface's pixel layout, MEASURED from the client's own colour converter
 * rather than assumed. Derived once, alongside the colours. */
static unsigned int g_ch_mask[3];   /* red, green, blue */
static int          g_ch_shift[3];
/* A screen tint: one full-screen alpha blend toward a colour. Fog and
 * darkness are the same machinery with different colours and gradients, so
 * they are two instances rather than two copies of the pass. */
#define WX_TINT_COUNT 2

/* The tables live OUTSIDE the struct on purpose. A struct with any non-zero
 * initialiser goes to .data whole, so folding 48 KB of zeroed lookup into
 * each tint put 96 KB of nothing in the DLL's image and grew the file from
 * 125 KB to 225 KB. Held separately they are .bss, which costs nothing on
 * disk. */
static unsigned short g_tint_lut[WX_TINT_COUNT][WX_FOG_BANDS][3][WX_FOG_LUT_SIZE];

typedef struct {
    unsigned int    bit;
    unsigned short *colour;         /* converted, filled in by colour init */
    int             a_light, a_medium, a_heavy;
    int             gradient;       /* 1 = graded top to bottom */
    int             top_pct, bottom_pct;
    int             alpha_override; /* 0 = follow intensity */
    unsigned short (*lut)[3][WX_FOG_LUT_SIZE];
    /* built state */
    int             lut_ready, lut_alpha, lut_gradient;
} wx_tint_t;

static wx_tint_t g_tints[WX_TINT_COUNT] = {
    { WX_BIT_FOG, &g_c_fog,
      WX_FOG_ALPHA_LIGHT, WX_FOG_ALPHA_MEDIUM, WX_FOG_ALPHA_HEAVY,
      1, WX_FOG_TOP_PERCENT, WX_FOG_BOTTOM_PERCENT, 0, g_tint_lut[0], 0, 0, 0 },
    /* Darkness is flat: night does not thicken toward the horizon, and a
     * graded darkness reads as a vignette, which is a camera artefact. */
    { WX_BIT_DARKNESS, &g_c_dark,
      WX_DARK_ALPHA_LIGHT, WX_DARK_ALPHA_MEDIUM, WX_DARK_ALPHA_HEAVY,
      0, 100, 100, 0, g_tint_lut[1], 0, 0, 0 },
};

/* Which icon a group claims, in PRIORITY order: the first running group in
 * this list names the weather. Precipitation and storms come first because
 * that is what a player would call the weather they are standing in; the
 * tints and the ambient motes are modifiers and come last, so
 * fireflies+darkness shows fireflies and sandstorm+fog shows the sandstorm.
 *
 * Ours outrank the client's four outright. A sandstorm blowing through cloud
 * is a sandstorm, and the stock chain would have called it clouds.
 *
 * The name rides in the same row as the icon on purpose. The tooltip and the
 * status line pick the weather's name with this same first-match walk, so the
 * glyph and the words can never end up naming different groups. */
static const struct { unsigned int bit; int icon; const char *name; } g_icon_map[] = {
    { WX_BIT_BLOOD,    WX_ICON_BLOOD,    "Blood Rain"     },
    { WX_BIT_SAND,     WX_ICON_SAND,     "Sandstorm"      },
    { WX_BIT_BLIZZARD, WX_ICON_BLIZZARD, "Blizzard"       },
    { WX_BIT_ASH,      WX_ICON_ASH,      "Ash Fall"       },
    { WX_BIT_EMBER,    WX_ICON_EMBER,    "Embers"         },
    { WX_BIT_LEAVES,   WX_ICON_LEAVES,   "Falling Leaves" },
    { WX_BIT_FOG,      WX_ICON_FOG,      "Fog"            },
    { WX_BIT_FIREFLY,  WX_ICON_FIREFLY,  "Fireflies"      },
    { WX_BIT_DARKNESS, WX_ICON_DARKNESS, "Darkness"       },
};
#define WX_ICON_MAP_COUNT ((int)(sizeof(g_icon_map) / sizeof(g_icon_map[0])))

static wx_patch_t g_icon_patch[WX_MAX_SET_SITES];
static int        g_icon_patches;

/* The two name-builder call sites, patched together or not at all. */
static wx_patch_t g_name_patch[2];
static int        g_name_patches;

/* "Light ", "Medium ", "Heavy ", indexed by g_level. Read out of the client
 * at install; these literals are the fallback if any read does not check
 * out, so the feature degrades to slightly-wrong wording rather than off. */
static const char *g_level_word[3] = { "Light ", "Medium ", "Heavy " };

/* Where the client keeps the weather bank's INDEX, read out of the
 * instruction after the picker call site. */
static int       *g_icon_bank;
/* Lazy probe result: 0 not answered yet, 1 artwork present, -1 absent.
 *
 * This cannot be answered at install time. The autoload cave calls
 * LoadLibraryA from the CRT entry point, so the mod installs long before the
 * client loads system\weather.mda at 0x45cd1c -- an install-time probe asks an
 * empty bank, is told no, and latches the feature off for the session. That is
 * exactly the bug that made the new icons never appear while the stock ones
 * kept working. Same reason the colour converter is resolved on the first
 * frame rather than at install. */
static int        g_icons_probed;

static wx_patch_t g_set_patch[WX_MAX_SET_SITES];
static int        g_set_patches;
static wx_patch_t g_draw_patch;
static int        g_installed;

/* ---- surface ------------------------------------------------------------- */

/* The back buffer is { int width; int height; ... ; void *rows[]; } -- the
 * drawing primitives read width at +0 and height at +4 and index a row table
 * at +0xC. Streaks and motes only need the dimensions; the fog pass walks the
 * row table itself. */
static int surface_w(void *s) { return *(int *)s; }
static int surface_h(void *s) { return *(int *)((char *)s + 4); }

static unsigned short **surface_rows(void *s)
{
    return (unsigned short **)((char *)s + 0x0C);
}

static int surface_sane(void *s)
{
    int w, h;
    if (!s)
        return 0;
    w = surface_w(s);
    h = surface_h(s);
    return w > 16 && h > 16 && w <= WX_SURFACE_MAX_DIM && h <= WX_SURFACE_MAX_DIM;
}

/* ---- colours -------------------------------------------------------------- */

static void wx_colours_init(void)
{
    wx_colourconv_fn conv;
    int i;

    if (g_colours_ready || !g_colourconv)
        return;
    conv = *g_colourconv;
    if (!conv)
        return;                     /* video not up yet; try again next frame */

    g_c_sand_pale  = conv(WX_COLOUR_SAND_PALE);
    g_c_sand_deep  = conv(WX_COLOUR_SAND_DEEP);
    g_c_ash_grey   = conv(WX_COLOUR_ASH_GREY);
    g_c_ash_dark   = conv(WX_COLOUR_ASH_DARK);
    g_c_ember      = conv(WX_COLOUR_EMBER);
    g_c_snow_white = conv(WX_COLOUR_SNOW_WHITE);
    g_c_snow_blue  = conv(WX_COLOUR_SNOW_BLUE);
    g_c_ember_hot  = conv(WX_COLOUR_EMBER_HOT);
    g_c_ember_dim  = conv(WX_COLOUR_EMBER_DIM);
    g_c_fog        = conv(WX_COLOUR_FOG);
    g_c_dark       = conv(WX_COLOUR_DARK);
    g_c_blood      = conv(WX_COLOUR_BLOOD);
    g_c_blood_dark = conv(WX_COLOUR_BLOOD_DARK);
    g_c_fly        = conv(WX_COLOUR_FIREFLY);
    g_c_fly_dim    = conv(WX_COLOUR_FIREFLY_D);
    g_c_fly_hot    = conv(WX_COLOUR_FIREFLY_H);
    g_c_leaf_green = conv(WX_COLOUR_LEAF_GREEN);
    g_c_leaf_lime  = conv(WX_COLOUR_LEAF_LIME);
    g_c_leaf_olive = conv(WX_COLOUR_LEAF_OLIVE);
    g_c_leaf_deep  = conv(WX_COLOUR_LEAF_DEEP);
    g_c_leaf_gold  = conv(WX_COLOUR_LEAF_GOLD);
    g_c_leaf_red   = conv(WX_COLOUR_LEAF_RED);

    /* Measure the pixel layout. A pure channel converts to exactly that
     * channel's mask, so red/green/blue hand back the three masks with no
     * assumption about 565 vs 555 vs anything else. */
    g_ch_mask[0] = conv(0xFF0000u);
    g_ch_mask[1] = conv(0x00FF00u);
    g_ch_mask[2] = conv(0x0000FFu);
    for (i = 0; i < 3; i++) {
        int shift = 0;
        unsigned int m = g_ch_mask[i];
        if (m == 0)
            break;
        while ((m & 1u) == 0u) {
            m >>= 1;
            shift++;
        }
        g_ch_shift[i] = shift;
        /* A channel wider than the lookup table would index off the end. No
         * 16-bit layout does this, but the fog pass reads memory, so the
         * check is not optional. */
        if ((g_ch_mask[i] >> shift) >= WX_FOG_LUT_SIZE)
            g_ch_mask[i] = 0;
    }

    g_colours_ready = 1;
    wx_log("colours ready: sand %04X/%04X  ash %04X/%04X  ember %04X/%04X/%04X  "
           "snow %04X/%04X  fog %04X",
           g_c_sand_pale, g_c_sand_deep, g_c_ash_grey, g_c_ash_dark,
           g_c_ember_dim, g_c_ember, g_c_ember_hot,
           g_c_snow_white, g_c_snow_blue, g_c_fog);
    wx_log("pixel layout: R %04X>>%d  G %04X>>%d  B %04X>>%d",
           g_ch_mask[0], g_ch_shift[0], g_ch_mask[1], g_ch_shift[1],
           g_ch_mask[2], g_ch_shift[2]);
}

/* ---- counts --------------------------------------------------------------- */

static int streak_target(const wx_streak_t *g)
{
    int base;

    if (!(g_mask & g->bit))
        return 0;
    base = g_level >= 2 ? WX_STREAK_HEAVY
         : g_level == 1 ? WX_STREAK_MEDIUM
                        : WX_STREAK_LIGHT;

    /* Gusting rides on the density rather than on the wind speed. Changing
     * the speed of particles already in flight makes the whole field jerk;
     * changing how many there are makes the storm breathe. */
    base = (base * wx_gust_percent()) / 100;
    if (base < 0)
        base = 0;
    if (base > WX_MAX_STREAK)
        base = WX_MAX_STREAK;
    return base;
}

static int mote_target(const wx_mote_t *g)
{
    if (!(g_mask & g->bit))
        return 0;
    return g_level >= 2 ? WX_MOTE_HEAVY
         : g_level == 1 ? WX_MOTE_MEDIUM
                        : WX_MOTE_LIGHT;
}

/* ---- streaks -------------------------------------------------------------- */

static void spawn_streak(const wx_streak_t *g, wx_particle_t *p,
                         int w, int h, int anywhere)
{
    int wind = g->wind + wx_rand_range(-g->wind_var, g->wind_var);
    int fall = wx_rand_range(g->fall_min, g->fall_max);
    int steep;

    /* Something must be moving, or the particle sits on screen forever. */
    if (wind == 0 && fall == 0)
        fall = 1;

    steep = (fall > (wind < 0 ? -wind : wind));

    if (anywhere) {
        p->x = wx_rand_range(0, w - 1);
        p->y = wx_rand_range(0, h - 1);
    } else if (steep) {
        /* Falls faster than it blows: enter from above, like rain. */
        p->x = wx_rand_range(-40, w + 40);
        p->y = -wx_rand_range(0, 40);
    } else {
        /* Blows faster than it falls: enter from the windward edge. */
        p->x = wind < 0 ? w + wx_rand_range(0, 60) : -wx_rand_range(0, 60);
        p->y = wx_rand_range(-20, h - 1);
    }

    p->vx  = wind;
    p->vy  = fall;
    p->len = g->stretch;
    p->phase = 0;
    p->rate  = 0;
    p->colour = (wx_rand() & 1) ? *g->colour_a : *g->colour_b;
    p->colour_alt = p->colour;
}

static void draw_streaks(wx_streak_t *g, void *surface, int w, int h)
{
    int target = streak_target(g);
    int i;

    if (g->live < target) {
        /* Grow a few per frame rather than all at once, so a storm rolls in
         * instead of appearing whole -- and so a gust ramps rather than
         * popping a hundred particles into existence. */
        int add = target - g->live;
        if (add > 8)
            add = 8;
        for (i = 0; i < add; i++)
            spawn_streak(g, &g->pool[g->live + i], w, h, g->live == 0);
        g->live += add;
    } else if (g->live > target) {
        g->live = target;
    }

    for (i = 0; i < g->live; i++) {
        wx_particle_t *p = &g->pool[i];
        int x2, y2;

        p->x += p->vx;
        p->y += p->vy;
        if (p->x < -80 || p->x > w + 80 || p->y > h + 8) {
            spawn_streak(g, p, w, h, 0);
            continue;
        }

        /* The streak is one frame of travel, scaled: that makes it motion
         * blur rather than a fixed-length dash, so the same code draws a flat
         * sand streak and a near-vertical fall of blood without knowing which
         * it is. */
        x2 = p->x + (p->vx * p->len) / 100;
        y2 = p->y + (p->vy * p->len) / 100;

        /* DrawLine clips against the surface itself, but the client's own
         * particle code range-checks first and so do we: it keeps the call
         * count down when a storm is mostly off-screen. */
        if (p->y < WX_EDGE_MARGIN && y2 < WX_EDGE_MARGIN)
            continue;
        if (p->y >= h - WX_EDGE_MARGIN && y2 >= h - WX_EDGE_MARGIN)
            continue;
        g_drawline(p->x, p->y, x2, y2, p->colour, surface);
    }
}

/* ---- motes ---------------------------------------------------------------- */

static void spawn_mote(const wx_mote_t *g, wx_particle_t *p,
                       int w, int h, int anywhere)
{
    unsigned int roll   = wx_rand();
    int          rising = g->vy_max < 0;
    /* A vy range that straddles zero is permission to hover, not a value to
     * correct: it is what lets fireflies sit in the air. */
    int          hovers = (g->vy_min <= 0 && g->vy_max >= 0);

    p->x  = wx_rand_range(0, w - 1);
    /* Falling motes enter at the top, rising ones at the bottom, hovering
     * ones simply appear where they are. */
    p->y  = (anywhere || hovers) ? wx_rand_range(0, h - 1)
          : (rising ? h + wx_rand_range(0, 40) : -wx_rand_range(0, 40));
    p->vx = wx_rand_range(-g->drift, g->drift);
    p->vy = wx_rand_range(g->vy_min, g->vy_max);
    if (p->vy == 0 && !hovers)
        p->vy = rising ? -1 : 1;
    p->len   = 0;
    p->phase = (unsigned char)(roll & 0xFF);
    /* Each particle runs its own phase speed, or a field of leaves swings in
     * unison and reads as one object. */
    p->rate  = (unsigned char)(2 + (roll >> 8) % 5u);

    if (g->special_odds > 0 && (roll % (unsigned int)g->special_odds) == 0) {
        p->colour = *g->colour_s;
    } else if (g->palette_n > 0) {
        p->colour = *g->palette[(roll >> 16) % (unsigned int)g->palette_n];
    } else {
        p->colour = *g->colour_s;
    }
    /* Pulsing swaps between the particle's colour and the group's dim one. */
    p->colour_alt = (g->pulse && g->colour_dim) ? *g->colour_dim : p->colour;
}

static void draw_motes(wx_mote_t *g, void *surface, int w, int h)
{
    int target = mote_target(g);
    int i;

    if (g->live < target) {
        int add = target - g->live;
        if (add > 6)
            add = 6;
        for (i = 0; i < add; i++)
            spawn_mote(g, &g->pool[g->live + i], w, h, g->live == 0);
        g->live += add;
    } else if (g->live > target) {
        g->live = target;
    }

    for (i = 0; i < g->live; i++) {
        wx_particle_t *p = &g->pool[i];
        unsigned short c;
        int px;

        p->phase = (unsigned char)(p->phase + p->rate);
        p->y += p->vy;

        if (g->sway) {
            /* Swing around the drift path rather than replacing it: the
             * particle still travels, it just does not travel straight. */
            p->x += p->vx;
            px = p->x + (g->sway * wx_sin(p->phase)) / (100 * 2);
        } else {
            p->x += p->vx;
            px = p->x;
            /* A slow sideways wander, re-rolled rarely so motes do not
             * shimmer. Groups that sway do not need it. */
            if ((wx_rand() & 0x3F) == 0)
                p->vx = wx_rand_range(-g->drift, g->drift);
        }

        if (p->y > h || p->y < -8 || px < -8 || px > w + 8) {
            spawn_mote(g, p, w, h, 0);
            continue;
        }

        /* Pulsing is a phase test, not a second timer: a firefly is lit for
         * half its own cycle and dark for the other half, and because every
         * particle has its own rate they blink independently. */
        c = (g->pulse && (p->phase & 0x80)) ? p->colour_alt : p->colour;

        /* The clipped pixel writer, never the raw one at 0x48c320: that one
         * indexes the row table with no bounds check at all, so a single
         * stray coordinate is a memory write outside the surface. */
        g_drawpixel(px, p->y, c, surface);
        if (c == *g->colour_s)
            g_drawpixel(px, p->y - (g->vy_max < 0 ? -1 : 1), c, surface);
    }
}

/* ---- fog ------------------------------------------------------------------
 *
 * A real alpha blend, written by us straight into the surface's row table.
 *
 * The first version dithered -- every Nth pixel overwritten with a flat grey,
 * on the reasoning that the client has no alpha. In game it drew as hard
 * diagonal lines crawling sideways, which is exactly what the code asks for:
 * a start offset of (y + phase) %% step increments once per row, and a set of
 * points one apart on successive rows IS a diagonal line. The crawl was the
 * phase advance. And even with the geometry fixed it could not have worked --
 * an opaque mid-grey over a bright scene reads as dark speckle, not haze,
 * because overwriting is not mixing.
 *
 * The client having no alpha was true of FillRect and irrelevant here: we own
 * the write, so we can read the pixel first and mix it ourselves.
 *
 * Cost is three lookups and two ORs per pixel. The lookup tables collapse the
 * whole blend -- unpack channel, mix toward fog, repack -- into one indexed
 * read per channel, rebuilt only when the alpha changes.
 *
 * The fog is graded top to bottom rather than flat, because in an isometric
 * view up is away and a uniform wash reads as a screen filter rather than as
 * distance. One blend table per horizontal band, chosen once per row, so the
 * inner loop is unchanged.
 */
static void tint_build_lut(wx_tint_t *t, int alpha)
{
    int band, ch, v;

    for (band = 0; band < WX_FOG_BANDS; band++) {
        int a = alpha;

        if (t->gradient) {
            /* Band 0 is the top of the screen and the far distance. */
            int pct = t->top_pct +
                      ((t->bottom_pct - t->top_pct) * band) / (WX_FOG_BANDS - 1);
            a = (alpha * pct) / 100;
        }
        if (a < 0)
            a = 0;
        if (a > 90)
            a = 90;

        for (ch = 0; ch < 3; ch++) {
            unsigned int mask  = g_ch_mask[ch];
            int          shift = g_ch_shift[ch];
            int          top   = mask ? (int)(mask >> shift) : 0;
            int          tgt   = (int)((*t->colour & mask) >> shift);

            for (v = 0; v < WX_FOG_LUT_SIZE; v++) {
                int src = v > top ? top : v;
                int out = src + (((tgt - src) * a) / 100);

                if (out < 0)
                    out = 0;
                if (out > top)
                    out = top;
                t->lut[band][ch][v] = (unsigned short)((unsigned int)out << shift);
            }
        }
    }
    t->lut_alpha    = alpha;
    t->lut_gradient = t->gradient;
    t->lut_ready    = 1;
    wx_log("tint 0x%03X: %d table(s) at %d%%%s", t->bit, WX_FOG_BANDS, alpha,
           t->gradient ? " graded" : " flat");
}

static void draw_tint(wx_tint_t *t, void *surface, int w, int h)
{
    unsigned short **rows = surface_rows(surface);
    const unsigned short *lr, *lg, *lb;
    unsigned int rm, gm, bm;
    int rs, gs, bs;
    int alpha = t->alpha_override;
    int y;

    /* Without a measured layout there is no safe way to take a pixel apart,
     * and guessing 565 is exactly the assumption this mod exists without. */
    if (!g_ch_mask[0] || !g_ch_mask[1] || !g_ch_mask[2])
        return;

    if (alpha <= 0)
        alpha = g_level >= 2 ? t->a_heavy
              : g_level == 1 ? t->a_medium
                             : t->a_light;
    if (!t->lut_ready || t->lut_alpha != alpha || t->lut_gradient != t->gradient)
        tint_build_lut(t, alpha);

    rm = g_ch_mask[0]; gm = g_ch_mask[1]; bm = g_ch_mask[2];
    rs = g_ch_shift[0]; gs = g_ch_shift[1]; bs = g_ch_shift[2];

    for (y = 0; y < h; y++) {
        unsigned short *row = rows[y];
        int band = (y * WX_FOG_BANDS) / h;
        int x;

        if (!row)
            continue;
        if (band >= WX_FOG_BANDS)
            band = WX_FOG_BANDS - 1;

        /* Picked once per row, so the inner loop is exactly what it was
         * before the gradient existed. */
        lr = t->lut[band][0];
        lg = t->lut[band][1];
        lb = t->lut[band][2];

        for (x = 0; x < w; x++) {
            unsigned int d = row[x];
            row[x] = (unsigned short)(lr[(d & rm) >> rs] |
                                      lg[(d & gm) >> gs] |
                                      lb[(d & bm) >> bs]);
        }
    }
}

/* ---- the draw ------------------------------------------------------------- */

static void wx_draw_ours(void *surface)
{
    int w, h, i;

    if (!g_mask)
        return;
    if (!surface_sane(surface))
        return;

    wx_colours_init();
    if (!g_colours_ready)
        return;                     /* no converter yet; draw nothing */

    w = surface_w(surface);
    h = surface_h(surface);

    /* Tints first: they are a wash over the scene, and the particles belong
     * in front or a heavy fog would swallow them. Fog before darkness, so
     * night falls on a fogged scene rather than the fog glowing through it. */
    for (i = 0; i < WX_TINT_COUNT; i++) {
        if (g_mask & g_tints[i].bit)
            draw_tint(&g_tints[i], surface, w, h);
    }

    g_gust_frame++;

    for (i = 0; i < WX_STREAK_GROUPS; i++) {
        if (g_mask & g_streaks[i].bit)
            draw_streaks(&g_streaks[i], surface, w, h);
    }
    for (i = 0; i < WX_MOTE_GROUPS; i++) {
        if (g_mask & g_motes[i].bit)
            draw_motes(&g_motes[i], surface, w, h);
    }
}

/* ---- detours -------------------------------------------------------------- */

static void wx_clear_particles(void)
{
    int i;
    for (i = 0; i < WX_STREAK_GROUPS; i++)
        g_streaks[i].live = 0;
    for (i = 0; i < WX_MOTE_GROUPS; i++)
        g_motes[i].live = 0;
}

/* Mirrors WeatherApplyGroups' own semantics: code 0 is the only thing that
 * stands groups down, and any other code ORs its bits into what is already
 * running. The server sends a change as stop-then-set for exactly this
 * reason, so in practice the second packet defines the new state. */
static void wx_note_code(unsigned int code)
{
    unsigned int ours;

    if (code == 0) {
        if (g_mask)
            wx_log("weather: stop (was 0x%03X)", g_mask);
        g_mask = 0;
        wx_clear_particles();
        return;
    }

    g_level = (code & WX_INTENSITY_HEAVY)  ? 2
            : (code & WX_INTENSITY_MEDIUM) ? 1
                                           : 0;

    ours = code & WX_BIT_OURS;
    if (ours && ours != (g_mask & ours))
        wx_log("weather: code 0x%08X -> ours 0x%03X level %d", code, ours, g_level);
    g_mask |= ours;
}

/* Replaces the stock icon picker at its single call site. Returns the index
 * the client then formats into "MainCommand%d".
 *
 * When none of our groups is running this calls the original, so the stock
 * four keep the icons they always had and a clear sky still gets MainCommand0.
 * That is also what makes the hook safe to leave installed: with our mask
 * empty it is a pure pass-through. */
static int icons_available(void)
{
    wx_animfind_fn find;
    char name[24];
    int  bank;

    if (g_icons_probed)
        return g_icons_probed > 0;

    if (!g_icon_bank || !wx_addr[WX_ANIMFIND])
        return 0;

    /* A bank index below zero is what AniLoad writes on failure, and it is
     * also what the global still holds before the load happens. Either way it
     * must never reach the lookup: that scales the index by 0x144 into the
     * table at 0x743b1c, so a negative one reads outside it. Return without
     * latching, so the next frame asks again. */
    bank = *g_icon_bank;
    if (bank < 0)
        return 0;

    find = (wx_animfind_fn)wx_addr[WX_ANIMFIND];
    _snprintf(name, sizeof(name), "MainCommand%d", WX_ICON_HIGHEST);
    name[sizeof(name) - 1] = '\0';

    if (find(name, bank) == WX_ANIM_NOT_FOUND) {
        g_icons_probed = -1;
        wx_log("icons: %s is not in the weather bank -- artwork not packed, "
               "falling through to the client's own picker", name);
        return 0;
    }

    g_icons_probed = 1;
    wx_log("icons: %s found in bank %d -- new icons active", name, bank);
    return 1;
}

/* Replaces the stock icon picker at its single call site. Returns the index
 * the client then formats into "MainCommand%d".
 *
 * Falls through to the original whenever it has nothing better to say: none of
 * our groups running, or the new artwork not present. That is what makes the
 * hook safe to install unconditionally at startup, which it has to be --
 * see g_icons_probed. */
static int __cdecl wx_icon_detour(void)
{
    int i;

    if (icons_available()) {
        for (i = 0; i < WX_ICON_MAP_COUNT; i++) {
            if (g_mask & g_icon_map[i].bit)
                return g_icon_map[i].icon;
        }
    }
    return g_iconpick();
}

/* ---- the weather's name --------------------------------------------------
 *
 * The client says "Sunny" for every one of our groups, and it is right to:
 * both of its name builders assemble the word from its own mask at 0x7decb0,
 * our bits are deliberately not in that word, and an empty word becomes
 * "Sunny" in both builders' last few instructions.
 *
 * So each detour calls the real builder into a scratch buffer and replaces
 * only the weather field, splicing on the literal middle of the client's own
 * format string. Everything else in the line -- the wind word, the map name,
 * the colour markup -- is the client's, untranslated and unassumed.
 *
 * Anything unexpected copies the original's own output through unchanged. A
 * stale "Sunny" is exactly today's behaviour and is a much better failure
 * than a mangled line.
 */

/* First running group in g_icon_map order, which is the same walk the icon
 * picker does -- so the words and the glyph always name the same weather.
 * NULL means "nothing of ours is running", i.e. leave the client alone. */
static const char *wx_group_name(void)
{
    int i;

    for (i = 0; i < WX_ICON_MAP_COUNT; i++) {
        if (g_mask & g_icon_map[i].bit)
            return g_icon_map[i].name;
    }
    return NULL;
}

static const char *wx_level_word(void)
{
    int level = g_level;

    if (level < 0)
        level = 0;
    if (level > 2)
        level = 2;
    return g_level_word[level] ? g_level_word[level] : "";
}

static void wx_append(char *out, int cap, int *len, const char *s)
{
    if (!s)
        return;
    while (*s && *len < cap - 1)
        out[(*len)++] = *s++;
    out[*len] = '\0';
}

/* Copy back exactly what the original wrote. Used on every path that decides
 * not to substitute, so the caller sees stock behaviour byte for byte. */
static void wx_passthrough(char *dest, const char *built)
{
    while ((*dest++ = *built++) != '\0')
        ;
}

/* "<weather> - <wind> Wind" */
static void __cdecl wx_tooltip_detour(char *dest)
{
    char        scratch[WX_NAME_SCRATCH];
    char        out[WX_NAME_SCRATCH];
    const char *name;
    const char *tail;
    int         len = 0;

    name = wx_group_name();
    if (!name) {
        g_tooltip_orig(dest);
        return;
    }

    scratch[0] = '\0';
    g_tooltip_orig(scratch);
    scratch[sizeof(scratch) - 1] = '\0';

    tail = strstr(scratch, WX_TOOLTIP_SEPARATOR);
    if (!tail) {
        wx_passthrough(dest, scratch);
        return;
    }

    out[0] = '\0';
    wx_append(out, (int)sizeof(out), &len, wx_level_word());
    wx_append(out, (int)sizeof(out), &len, name);
    wx_append(out, (int)sizeof(out), &len, tail);
    wx_passthrough(dest, out);
}

/* "#Y<map>#N: #Y<weather>#N and #Y<wind>#N Wind " -- three fields, and only
 * the middle one is ours. The head is copied verbatim rather than rebuilt,
 * so the map name never passes through us. */
static void __cdecl wx_status_detour(char *dest)
{
    char        scratch[WX_NAME_SCRATCH];
    char        out[WX_NAME_SCRATCH];
    const char *name;
    const char *field;
    const char *tail;
    int         len = 0;

    name = wx_group_name();
    if (!name) {
        g_status_orig(dest);
        return;
    }

    scratch[0] = '\0';
    g_status_orig(scratch);
    scratch[sizeof(scratch) - 1] = '\0';

    field = strstr(scratch, WX_STATUS_FIELD_OPEN);
    if (field)
        field += sizeof(WX_STATUS_FIELD_OPEN) - 1;
    tail = field ? strstr(field, WX_STATUS_FIELD_CLOSE) : NULL;
    if (!tail) {
        wx_passthrough(dest, scratch);
        return;
    }

    out[0] = '\0';
    while (len < (int)sizeof(out) - 1 && scratch + len < field)
        out[len] = scratch[len], len++;
    out[len] = '\0';
    wx_append(out, (int)sizeof(out), &len, wx_level_word());
    wx_append(out, (int)sizeof(out), &len, name);
    wx_append(out, (int)sizeof(out), &len, tail);
    wx_passthrough(dest, out);
}

static void __cdecl wx_setweather_detour(unsigned int code)
{
    wx_note_code(code);
    g_setweather(code);
}

/* ---- settings ------------------------------------------------------------ */

/* Read every tunable out of config.ini and clamp it into the live parameter
 * blocks.
 *
 * Split out of wx_weather_install so it can also be called from the poll
 * below. Everything here is a pure assignment into g_streaks / g_motes /
 * g_tints and can therefore run at any point on the render thread; nothing
 * here installs, removes or inspects a hook.
 *
 * [Weather] Icons and Names are deliberately NOT here. They decide whether
 * install_icon_hook() and install_name_hooks() run at all, so they are
 * startup decisions rather than tunables, and re-reading either would report
 * a change this mod cannot make.
 *
 * Every read passes "Weather" explicitly: WX_SECTION is "Fight", a leftover
 * from the mod this file was cloned from, so plain wx_cfg_int would land in
 * the wrong section.
 */
static void apply_config(void)
{
    g_sand_wind = wx_cfg_int_in("Weather", "SandWind", 14);
    g_ash_drift = wx_cfg_int_in("Weather", "AshDrift", 1);
    if (g_ash_drift < 0)
        g_ash_drift = 0;
    if (g_ash_drift > 4)
        g_ash_drift = 4;
    g_streaks[0].wind = -g_sand_wind;
    g_motes[0].drift  = g_ash_drift;

    g_streaks[1].wind = -wx_cfg_int_in("Weather", "BlizzardWind", 18);
    g_motes[1].drift  = wx_cfg_int_in("Weather", "EmberDrift", 2);
    if (g_motes[1].drift < 0)
        g_motes[1].drift = 0;
    if (g_motes[1].drift > 4)
        g_motes[1].drift = 4;

    /* 0 follows intensity; 1-90 pins the mix. Above 90 the scene is gone
     * rather than fogged, so it is clamped rather than trusted. */
    g_tints[0].alpha_override = wx_cfg_int_in("Weather", "FogAlpha", 0);
    if (g_tints[0].alpha_override < 0 || g_tints[0].alpha_override > 90)
        g_tints[0].alpha_override = 0;
    g_tints[0].gradient = wx_cfg_int_in("Weather", "FogGradient", 1) ? 1 : 0;

    g_tints[1].alpha_override = wx_cfg_int_in("Weather", "DarknessAlpha", 0);
    if (g_tints[1].alpha_override < 0 || g_tints[1].alpha_override > 90)
        g_tints[1].alpha_override = 0;

    g_gust_depth = wx_cfg_int_in("Weather", "Gust", WX_GUST_DEPTH_DEFAULT);
    if (g_gust_depth < 0)
        g_gust_depth = 0;
    if (g_gust_depth > 90)
        g_gust_depth = 90;

    g_streaks[2].wind = -wx_cfg_int_in("Weather", "BloodWind", 3);
    g_motes[3].sway   = wx_cfg_int_in("Weather", "LeafSway", 46);
    if (g_motes[3].sway < 0)
        g_motes[3].sway = 0;
    if (g_motes[3].sway > 200)
        g_motes[3].sway = 200;
}

/* Re-read the tunables periodically so an edit -- from the in-engine settings
 * panel, or from Notepad -- takes effect without a restart.
 *
 * Same pattern and the same reasoning as chatmod's SETTINGS_POLL_MS: on the
 * client's own render thread, throttled, never from a worker. A thread parked
 * inside this DLL is a thread whose code disappears on unload.
 *
 * Unlike osmw_fightspeed this mod never writes config.ini, so there is no
 * writer to race and no need to track what was last seen in the file. A plain
 * re-read is correct here.
 */
#define WX_SETTINGS_POLL_MS 2000

static void settings_poll(void)
{
    static DWORD last;
    DWORD now = GetTickCount();

    if (last && (now - last) < WX_SETTINGS_POLL_MS)
        return;
    last = now;
    apply_config();
}

static void __cdecl wx_draw_detour(void *surface)
{
    /* The draw site is the tick. It sits inside the client's own "draw weather
     * at all" gate, so the poll only runs when these values are about to be
     * used -- which is the only time they matter. */
    settings_poll();

    /* Stock groups first: ours are meant to sit over rain and under nothing,
     * and the client draws its own in one pass. */
    g_draw(surface);
    wx_draw_ours(surface);
}

unsigned int wx_active_mask(void)
{
    return g_mask;
}

/* ---- install -------------------------------------------------------------- */

/* Find every `call rel32` in .text whose target is `fn`. This is how the mod
 * avoids a prologue detour: SetWeather has four callers in the stock client
 * and patching each of their five bytes leaves the function itself byte for
 * byte untouched. */
static int patch_all_call_sites(unsigned char *fn, void *detour)
{
    unsigned char *lo, *hi, *p;

    if (!wx_text_span(&lo, &hi))
        return 0;

    g_set_patches = 0;
    for (p = lo; p <= hi - 5 && g_set_patches < WX_MAX_SET_SITES; p++) {
        if (p[0] != 0xE8)
            continue;
        if (p + 5 + *(int *)(p + 1) != fn)
            continue;
        if (!wx_patch_call_site(&g_set_patch[g_set_patches], p, detour,
                                "SetWeather caller")) {
            wx_log("install: call site %p refused", p);
            return -1;
        }
        wx_log("install: SetWeather call site %p -> detour", p);
        g_set_patches++;
    }
    return g_set_patches;
}

/* Patch the picker's single call site. The artwork probe is NOT done here --
 * at install time the client has not loaded the bank yet - only the address of
 * the bank global is captured, and the detour probes on its first call.
 * Everything here fails soft: any check that does not hold leaves the client's
 * own picker in place. */
static void install_icon_hook(void)
{
    unsigned char *site;
    unsigned char *lo, *hi, *p;

    g_iconpick    = (wx_iconpick_fn)wx_addr[WX_ICONPICKER];
    g_client_mask = *(unsigned int **)(wx_addr[WX_ICONPICKER] + 1);

    if (!wx_text_span(&lo, &hi))
        return;

    /* One caller in the stock client. Found the same way as SetWeather's,
     * rather than by another signature. */
    site = NULL;
    for (p = lo; p <= hi - 5; p++) {
        if (p[0] != 0xE8)
            continue;
        if (p + 5 + *(int *)(p + 1) != wx_addr[WX_ICONPICKER])
            continue;
        if (site) {
            wx_log("icons: picker has more than one call site -- standing down");
            return;
        }
        site = p;
    }
    if (!site) {
        wx_log("icons: no picker call site found -- standing down");
        return;
    }

    /* The bank global is loaded a fixed distance past the call. Verify the
     * opcode before believing the operand. */
    if (site[WX_ICON_BANK_OFFSET] != 0xA1) {
        wx_log("icons: no `mov eax, [abs32]` at %p+0x%02X (%02X) -- standing down",
               site, WX_ICON_BANK_OFFSET, site[WX_ICON_BANK_OFFSET]);
        return;
    }
    g_icon_bank = *(int **)(site + WX_ICON_BANK_OFFSET + 1);

    if (!wx_patch_call_site(&g_icon_patch[0], site, (void *)wx_icon_detour,
                            "weather icon-picker caller")) {
        wx_log("icons: call site %p refused", site);
        return;
    }
    g_icon_patches = 1;
    wx_log("icons: picker call site %p -> detour, bank global %p "
           "(artwork probed on first draw)", site, g_icon_bank);
}

/* Resolve one name-builder call site: check the E8 is still there, follow it,
 * and check the target still opens the way both builders do. Fails soft --
 * a NULL out means leave the client's own text alone. */
static int resolve_name_site(int sig, int call_offset, const char *what,
                             unsigned char **out_site, unsigned char **out_fn)
{
    unsigned char *site = wx_addr[sig] + call_offset;
    unsigned char *fn;

    if (site[0] != 0xE8) {
        wx_log("names: %s site %p is not `call rel32` (%02X) -- standing down",
               what, site, site[0]);
        return 0;
    }
    fn = site + 5 + *(int *)(site + 1);
    if (fn[0] != WX_NAMEFN_PROLOGUE_A || fn[5] != WX_NAMEFN_PROLOGUE_B ||
        fn[6] != WX_NAMEFN_PROLOGUE_C) {
        wx_log("names: %s target %p does not open like a name builder "
               "(%02X %02X %02X) -- standing down", what, fn, fn[0], fn[5], fn[6]);
        return 0;
    }
    *out_site = site;
    *out_fn   = fn;
    return 1;
}

/* Is `len` bytes at `p` committed and readable? Only used at install, and
 * only to decide whether an operand that looks like an address may be
 * dereferenced. */
static int wx_readable(const void *p, int len)
{
    MEMORY_BASIC_INFORMATION mbi;
    const unsigned char *base = (const unsigned char *)p;
    DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                     PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                     PAGE_EXECUTE_WRITECOPY;

    if (!p || len <= 0)
        return 0;
    if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi))
        return 0;
    if (mbi.State != MEM_COMMIT || !(mbi.Protect & readable) ||
        (mbi.Protect & PAGE_GUARD))
        return 0;
    /* Do not walk into the next region: one page is far more than a word. */
    return base + len <= (const unsigned char *)mbi.BaseAddress + mbi.RegionSize;
}

/* One intensity word out of the tooltip builder's body. `oplen` is the
 * instruction's opcode length, so the absolute address sits at fn+off+oplen.
 * Returns NULL unless the opcode is what it must be and the address holds a
 * short printable NUL-terminated string. */
static const char *read_level_word(unsigned char *fn, int off, int oplen)
{
    const char *s;
    int         i;

    if (oplen == WX_LEVEL_LIGHT_OPLEN) {
        if (fn[off] != 0x8B || fn[off + 1] != 0x35)
            return NULL;
    } else if (fn[off] != WX_NAMEFN_PROLOGUE_A) {
        return NULL;
    }

    s = *(const char **)(fn + off + oplen);
    /* The opcode matching is not proof the operand is an address: on a client
     * we have never seen, those bytes could be anything. Ask the OS before
     * dereferencing, rather than reading whatever the operand points at. */
    if (!wx_readable(s, WX_LEVEL_WORD_MAX))
        return NULL;
    for (i = 0; i < WX_LEVEL_WORD_MAX; i++) {
        if (s[i] == '\0')
            return i > 0 ? s : NULL;
        if (s[i] < 0x20 || s[i] > 0x7E)
            return NULL;
    }
    return NULL;
}

static void read_level_words(unsigned char *fn)
{
    static const struct { int off; int oplen; } where[3] = {
        { WX_LEVEL_LIGHT_OFFSET,  WX_LEVEL_LIGHT_OPLEN  },
        { WX_LEVEL_MEDIUM_OFFSET, WX_LEVEL_MEDIUM_OPLEN },
        { WX_LEVEL_HEAVY_OFFSET,  WX_LEVEL_HEAVY_OPLEN  },
    };
    int i;

    for (i = 0; i < 3; i++) {
        const char *word = read_level_word(fn, where[i].off, where[i].oplen);
        if (word)
            g_level_word[i] = word;
        else
            wx_log("names: intensity word %d not readable at %p+0x%02X -- "
                   "keeping \"%s\"", i, fn, where[i].off, g_level_word[i]);
    }
    wx_log("names: intensity words \"%s\" \"%s\" \"%s\"",
           g_level_word[0], g_level_word[1], g_level_word[2]);
}

/* Patch both name builders' single call sites, or neither: a tooltip that
 * says Sandstorm while the status line says Sunny is worse than both saying
 * Sunny. Everything fails soft, exactly like the icon hook. */
static void install_name_hooks(void)
{
    unsigned char *tip_site, *tip_fn, *status_site, *status_fn;

    if (!resolve_name_site(WX_TOOLTIPCALLSITE, WX_TOOLTIP_CALL_OFFSET,
                           "tooltip", &tip_site, &tip_fn))
        return;
    if (!resolve_name_site(WX_STATUSCALLSITE, WX_STATUS_CALL_OFFSET,
                           "status line", &status_site, &status_fn))
        return;

    /* The two builders are near-clones. If a signature has started matching
     * the wrong one, both sites resolve to the same function and one of the
     * two lines would be built with the other's format string. */
    if (tip_fn == status_fn) {
        wx_log("names: both sites call %p -- standing down", tip_fn);
        return;
    }

    read_level_words(tip_fn);

    g_tooltip_orig = (wx_namebuild_fn)tip_fn;
    g_status_orig  = (wx_namebuild_fn)status_fn;

    if (!wx_patch_call_site(&g_name_patch[0], tip_site, (void *)wx_tooltip_detour,
                            "weather tooltip caller")) {
        wx_log("names: tooltip call site %p refused", tip_site);
        g_tooltip_orig = NULL;
        g_status_orig  = NULL;
        return;
    }
    g_name_patches = 1;

    if (!wx_patch_call_site(&g_name_patch[1], status_site, (void *)wx_status_detour,
                            "weather status-line caller")) {
        wx_log("names: status-line call site %p refused -- rolling the tooltip back",
               status_site);
        if (wx_patch_undo(&g_name_patch[0]) == OSMW_OK)
            g_name_patches = 0;
        g_tooltip_orig = NULL;
        g_status_orig  = NULL;
        return;
    }
    g_name_patches = 2;

    wx_log("names: tooltip site %p -> %p, status site %p -> %p",
           tip_site, tip_fn, status_site, status_fn);
}

int wx_weather_install(void)
{
    unsigned char *site;
    int i;

    if (g_installed)
        return 1;

    for (i = 0; i < WX_ADDR_COUNT; i++) {
        /* An optional signature that missed costs its own feature and nothing
         * else. The two name-builder call sites are the only ones: losing the
         * weather's NAME must never cost the weather. */
        if (WX_SIGS[i].optional)
            continue;
        if (!wx_addr[i]) {
            wx_log("install: %s did not resolve -- standing down", WX_SIGS[i].name);
            return 0;
        }
    }

    g_setweather = (wx_setweather_fn)wx_addr[WX_SETCODE];
    g_draw       = (wx_draw_fn)wx_addr[WX_DRAW];
    g_drawline   = (wx_drawline_fn)wx_addr[WX_DRAWLINE];
    g_drawpixel  = (wx_drawpixel_fn)wx_addr[WX_DRAWPIXEL];
    g_colourconv = (wx_colourconv_fn *)wx_addr[WX_COLOURCONV];

    apply_config();

    /* The draw call site is RenderCallSite + 0x23, which is past the end of
     * the matched pattern, so verify rather than assume: it must be an E8 and
     * it must currently target the WeatherDraw we resolved separately. If the
     * client ever grows a second weather draw, this is what catches it. */
    site = wx_addr[WX_RENDERCALLSITE] + 0x23;
    if (site[0] != 0xE8) {
        wx_log("install: %p is not `call rel32` (%02X) -- standing down",
               site, site[0]);
        return 0;
    }
    if (site + 5 + *(int *)(site + 1) != wx_addr[WX_DRAW]) {
        wx_log("install: %p does not call WeatherDraw -- standing down", site);
        return 0;
    }

    /* The icon hook is optional and self-checking: without the new artwork in
     * system\weather.mda it is left out entirely, and the client keeps its
     * own picker. Asking for a MainCommand that is not in the bank would make
     * the icon silently disappear, which is worse than showing the wrong one. */
    if (wx_cfg_int_in("Weather", "Icons", 1))
        install_icon_hook();
    else
        wx_log("icons: [Weather] Icons=0 -- stock picker left in place");

    /* And the words. Independent of Icons on purpose: they are two separate
     * client surfaces, and being able to turn one off without the other is
     * what makes a client update bisectable. */
    if (!wx_addr[WX_TOOLTIPCALLSITE] || !wx_addr[WX_STATUSCALLSITE])
        wx_log("names: a call-site signature did not resolve -- stock weather "
               "text left in place, the effects are unaffected");
    else if (wx_cfg_int_in("Weather", "Names", 1))
        install_name_hooks();
    else
        wx_log("names: [Weather] Names=0 -- stock weather text left in place");

    if (patch_all_call_sites(wx_addr[WX_SETCODE],
                             (void *)wx_setweather_detour) <= 0) {
        wx_log("install: no SetWeather call sites found -- standing down");
        return 0;
    }

    if (!wx_patch_call_site(&g_draw_patch, site, (void *)wx_draw_detour,
                            "WeatherDraw caller")) {
        wx_log("install: draw site patch failed -- rolling back");
        return 0;
    }

    g_installed = 1;
    wx_log("install: %d SetWeather site(s) + draw site %p; groups 0x%03X",
           g_set_patches, site, (unsigned int)WX_BIT_OURS);
    return 1;
}

OSMW_RESULT wx_weather_remove(void)
{
    OSMW_RESULT result;

    /* Draw site first: after this the client stops calling into us at all,
     * which is the ordering that matters if a frame is in flight. */
    result = wx_patch_undo(&g_draw_patch);
    if (result != OSMW_OK)
        return result;
    while (g_icon_patches > 0) {
        result = wx_patch_undo(&g_icon_patch[g_icon_patches - 1]);
        if (result != OSMW_OK)
            return result;
        --g_icon_patches;
    }
    g_icons_probed = 0;
    while (g_name_patches > 0) {
        result = wx_patch_undo(&g_name_patch[g_name_patches - 1]);
        if (result != OSMW_OK)
            return result;
        --g_name_patches;
    }
    g_tooltip_orig = NULL;
    g_status_orig  = NULL;
    while (g_set_patches > 0) {
        result = wx_patch_undo(&g_set_patch[g_set_patches - 1]);
        if (result != OSMW_OK)
            return result;
        --g_set_patches;
    }
    result = wx_iat_unhook();
    if (result != OSMW_OK)
        return result;

    g_mask = 0;
    wx_clear_particles();
    g_installed = 0;
    wx_log("removed: client bytes restored");
    return OSMW_OK;
}
