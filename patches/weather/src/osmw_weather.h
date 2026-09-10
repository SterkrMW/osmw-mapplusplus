/* osmw_weather -- two extra weather types for the MythWar (osMW) client.
 *
 * The client draws rain, snow, clouds and thunder from code, one particle
 * group per bit of the weather code the server sends. Bits 4 and 5 of that
 * code are unused and a stock client masks them off, so they are free for us:
 * this DLL adds Sandstorm (bit 4) and Ash fall (bit 5), for the lava and sand
 * maps that make up most of the world.
 *
 * Loaded and initialised through osmw_core.dll's ABI 1 manifest. Generated
 * signatures resolve through core's pristine image and every dynamically
 * discovered call site is exclusively claimed before this module's existing
 * safe patch transaction writes it.
 *
 * Spec: mw-tools/tools/client-dll/weather-effects.md
 * RE:   mw-tools/docs/client-packet-re.md, 0x00030015
 */
#ifndef OSMW_WEATHER_H
#define OSMW_WEATHER_H

#include <windows.h>
#include "osmw_core_api.h"
#include "sigs.h"

const OSMW_CORE_API_V1 *wx_core_api(void);
OSMW_OWNER wx_core_owner(void);

/* ---- resolved target addresses ------------------------------------------ */

/* Absolute addresses in the host process, indexed by the WX_* enum in sigs.h.
 * NULL means that signature did not resolve. */
extern unsigned char *wx_addr[WX_ADDR_COUNT];
extern unsigned char *wx_base;        /* main.exe image base */
extern unsigned int   wx_timestamp;   /* PE TimeDateStamp, identifies the build */

/* The reference build these signatures were generated against. A different
 * timestamp is a warning, not a failure -- the signatures are what decide. */
#define WX_REFERENCE_BUILD 0x45650696U

/* ---- the wire code ------------------------------------------------------- */

/* Bits 0-3 are the client's own groups. SetWeather masks the code to those
 * four before it stores anything, which is exactly why 4 and 5 are safe to
 * use: an unpatched client discards them instead of misbehaving. */
#define WX_BIT_RAIN    0x01
#define WX_BIT_SNOW    0x02
#define WX_BIT_CLOUDS  0x04
#define WX_BIT_THUNDER 0x08
#define WX_BIT_SAND    0x10
#define WX_BIT_ASH     0x20
#define WX_BIT_BLIZZARD  0x40
#define WX_BIT_EMBER     0x80
#define WX_BIT_FOG       0x100
#define WX_BIT_FIREFLY   0x200
#define WX_BIT_LEAVES    0x400
#define WX_BIT_BLOOD     0x800
#define WX_BIT_DARKNESS  0x1000
#define WX_BIT_OURS      (WX_BIT_SAND | WX_BIT_ASH | WX_BIT_BLIZZARD | WX_BIT_EMBER | \
                          WX_BIT_FOG | WX_BIT_FIREFLY | WX_BIT_LEAVES | WX_BIT_BLOOD | \
                          WX_BIT_DARKNESS)

/* Bits 13-23 are still free: eleven more groups before the intensity field at
 * 24/25 is reached. */

/* Intensity rides in bits 24/25 of the same dword, 25 beating 24. */
#define WX_INTENSITY_MEDIUM 0x01000000U
#define WX_INTENSITY_HEAVY  0x02000000U

/* Our two groups are held HERE, never in the client's own mask word at
 * 0x7decb0. Putting them there would feed the HUD weather icon picker a bit
 * it has no case for, and a sandstorm would relabel the HUD as thunder and
 * clouds. See "The HUD weather icon" in the spec. */

/* ---- effect shape -------------------------------------------------------- */

/* Two engines, four particle groups. A "streak" is a fast near-horizontal
 * line (sand, blizzard); a "mote" is a slow drifting point that may fall or
 * rise (ash, embers). Each group owns its own pool, because any pair of them
 * can be running at once -- the wire code is a bitmask.
 *
 * Static arrays: nothing here allocates after install, so there is no path
 * from a weather packet to the heap. Four pools is about 36 KB of .bss. */
#define WX_MAX_STREAK 512
#define WX_MAX_MOTE   384
#define WX_STREAK_GROUPS 3          /* sandstorm, blizzard, blood rain */
#define WX_MOTE_GROUPS   4          /* ash, embers, fireflies, leaves */

/* Per-intensity live counts, indexed by 0 light / 1 medium / 2 heavy. */
#define WX_STREAK_LIGHT  120
#define WX_STREAK_MEDIUM 260
#define WX_STREAK_HEAVY  512
#define WX_MOTE_LIGHT     60
#define WX_MOTE_MEDIUM   150
#define WX_MOTE_HEAVY    384

/* Colours as 0xRRGGBB. The client converts these to the surface's own 16-bit
 * format through the function pointer ColourConv resolves, so the mod never
 * assumes 565 or 555. */
#define WX_COLOUR_SAND_PALE  0xD8C89Cu
#define WX_COLOUR_SAND_DEEP  0xB49A66u
#define WX_COLOUR_ASH_GREY   0x9C9C98u
#define WX_COLOUR_ASH_DARK   0x6E6E6Au
#define WX_COLOUR_EMBER      0xE86A28u
#define WX_COLOUR_SNOW_WHITE 0xFFFFFFu
#define WX_COLOUR_SNOW_BLUE  0xC8D8F0u
#define WX_COLOUR_EMBER_HOT  0xFFC864u
#define WX_COLOUR_EMBER_DIM  0x963C14u
/* Near-white with a slight blue, NOT mid-grey. A blend toward 0xC8C8C8
 * darkens every highlight it touches, so on a bright map it reads as the
 * colour draining out of the scene rather than as haze. Real distance fog
 * ADDS light: it lifts shadows and leaves highlights nearly alone, which is
 * what a value this close to white does. */
#define WX_COLOUR_FOG        0xE0E4ECu
#define WX_COLOUR_BLOOD      0xA01818u
#define WX_COLOUR_BLOOD_DARK 0x701010u
#define WX_COLOUR_FIREFLY    0xFFE060u
#define WX_COLOUR_FIREFLY_D  0x8A6A18u
#define WX_COLOUR_FIREFLY_H  0xFFFFC8u
/* Leaves are mostly GREEN, on purpose. The first version was gold, brown and
 * red, which is the same warm family as ash and embers -- at a glance, and
 * especially in motion, three of the nine groups looked like the same effect.
 * Four greens against one gold reads as foliage; the red stays as a rare
 * accent because a few turned leaves are what makes the rest look green. */
#define WX_COLOUR_LEAF_GREEN 0x5C8A2Eu
#define WX_COLOUR_LEAF_LIME  0x8CA83Au
#define WX_COLOUR_LEAF_OLIVE 0x6E7A28u
#define WX_COLOUR_LEAF_DEEP  0x3E6B24u
#define WX_COLOUR_LEAF_GOLD  0xC8781Eu
#define WX_COLOUR_LEAF_RED   0xB03020u

/* Darkness tints toward almost-black with a little blue left in it. A pure
 * 0x000000 tint just crushes the scene; the trace of blue reads as night
 * rather than as the brightness slider going down. */
#define WX_COLOUR_DARK       0x101822u

/* The fog layer is a REAL ALPHA BLEND, done by us.
 *
 * The first build dithered instead -- every Nth pixel overwritten with a flat
 * grey -- on the grounds that the client has no alpha. That was wrong, and it
 * looked it: see "Fog: the dither that failed" in NOTES.txt. The client's
 * FillRect has no alpha, but we write the row table ourselves, so nothing
 * stops us reading each pixel and blending it.
 *
 * Blending needs the pixel format, which is the one thing the mod had been
 * careful never to assume. It does not have to: feeding the client's own
 * RGB888 -> native16 converter pure red, green and blue hands back the three
 * channel masks directly, so the format is measured rather than guessed and
 * 565 and 555 both work. */
#define WX_FOG_ALPHA_LIGHT  25   /* percent of the fog colour mixed in */
#define WX_FOG_ALPHA_MEDIUM 40
#define WX_FOG_ALPHA_HEAVY  55

/* Channel values are at most 6 bits on any 16-bit layout, so a 64-entry
 * lookup per channel covers every possible source value. Three lookups and
 * two ORs per pixel is what keeps a full-screen blend affordable. */
#define WX_FOG_LUT_SIZE 64

/* Fog is denser toward the top of the screen, because in an isometric view up
 * IS away: the top of the frame is the far distance. A flat wash over the
 * whole screen is the thing that reads as a filter rather than as weather.
 *
 * One band of the gradient per horizontal strip, each with its own blend
 * table, so the per-pixel cost is unchanged -- the row picks a table pointer
 * and the inner loop is identical.
 *
 * THE BAND COUNT IS NOT A FREE CHOICE. It shipped at 8 and the seams were
 * visible in game, worst while moving: the bands are fixed in screen space
 * while the world scrolls under them, so a static horizontal edge is exactly
 * what the eye is best at finding. What matters is the largest change in the
 * final 5/6-bit channel value between two vertically adjacent pixels --
 * anything above 1 is a contour. Measured on the 6-bit green channel, worst
 * case over every source value and every screen row:
 *
 *     bands    a=25  a=40  a=55  a=90     tables
 *         8       2     3     4     7       3 KB
 *        32       1     2     2     2      12 KB
 *        64       1     1     2     2      24 KB
 *       128       1     1     1     1      48 KB
 *
 * 128 is the first count where every alpha is down to a single unit, which is
 * the client's own quantisation and therefore invisible. Do not lower it
 * without re-running patches/weather/tools/bands.py. */
#define WX_FOG_BANDS 128

/* Band alpha as a percentage OF the base alpha: strongest at the top of the
 * screen, weakest at the bottom, interpolated between. */
#define WX_FOG_TOP_PERCENT    135
#define WX_FOG_BOTTOM_PERCENT  55

/* Darkness ships FLAT. Night does not get thicker toward the horizon, and a
 * graded darkness reads as a vignette, which is a camera artefact rather than
 * weather. The gradient fields exist on the tint struct so it CAN be turned
 * on, not because it should be. */
#define WX_DARK_ALPHA_LIGHT  30
#define WX_DARK_ALPHA_MEDIUM 45
#define WX_DARK_ALPHA_HEAVY  62

/* ---- gusting -------------------------------------------------------------
 *
 * Not a weather and not a bit: it is how wind behaves, so it is a property of
 * every streak group rather than something the server switches on. Density
 * and streak length swing around their base value on a slow cycle, which is
 * most of the difference between a storm and a screen full of moving lines.
 *
 * Depth is a percentage of the base density. The period is in frames, and it
 * is long on purpose -- a gust that repeats quickly reads as a pulsing effect
 * rather than as wind. */
#define WX_GUST_DEPTH_DEFAULT 35
#define WX_GUST_PERIOD        520

/* ---- sway and pulse ------------------------------------------------------
 *
 * Two mote parameters that exist because of what the first four groups
 * taught: ash and embers differ only in the sign of vy, so the engine is
 * mostly parameters, and the cheapest way to make a new group feel different
 * is to give it a motion the others do not have.
 *
 * SWAY is a horizontal oscillation. A leaf that falls straight down is a grey
 * flake painted orange; a leaf that swings as it falls is a leaf.
 * PULSE swaps the particle between its two colours on its own phase, which is
 * what makes fireflies blink instead of hover as steady dots. */
#define WX_SWAY_SCALE 32            /* divisor on sway * sin, keeps it gentle */

/* The most base colours a mote group can carry. Two was the original limit
 * and it is what made leaves hard to tell from embers: a group needs enough
 * hues to read as a material rather than as a colour. */
#define WX_MAX_PALETTE 6

/* ---- the HUD weather icon ------------------------------------------------
 *
 * A 32x32 sprite blitted at (741, 4), top right, from system\weather.mda.
 * The client picks its index in 0x46f9b0 -- an equality chain over its own
 * mask at 0x7decb0 with no default -- and 0x4411c6 formats the result into
 * "MainCommand%d" and draws it.
 *
 * Note the stock chain returns 0 for a mask of 0, and MainCommand0 IS drawn:
 * it is the clear-sky sun, not "nothing". Index 0 is therefore taken.
 *
 *   0 clear   1 thunder   2 clouds   3 rain   4 snow
 *   5 any combination with snow    6 any other with rain    7 thunder+clouds
 *
 * Our nine groups take 8 upward, in bit order, so the index for a group is
 * simply 4 + its bit number. Nothing enforces that -- g_icon_map is the
 * authority -- but keeping it mechanical means a tenth group's icon number is
 * not a decision.
 */
#define WX_ICON_SAND      8
#define WX_ICON_ASH       9
#define WX_ICON_BLIZZARD 10
#define WX_ICON_EMBER    11
#define WX_ICON_FOG      12
#define WX_ICON_FIREFLY  13
#define WX_ICON_LEAVES   14
#define WX_ICON_BLOOD    15
#define WX_ICON_DARKNESS 16

/* The highest index we would ever ask for. Install probes the bank for this
 * one by name: if the artwork has not been packed, the icon hook is left out
 * and the client keeps its own picker, rather than asking for a sprite that
 * is not there and drawing nothing. */
#define WX_ICON_HIGHEST  WX_ICON_DARKNESS

/* The animation lookup returns this when a name is not in the bank. */
#define WX_ANIM_NOT_FOUND (-2)

/* The bank global is loaded 0x1B bytes past the picker call site
 * (`mov eax, [abs32]`), which is where the mod reads it from -- guarded by a
 * check that the opcode is still A1. */
#define WX_ICON_BANK_OFFSET 0x1B

/* ---- the weather NAME, in the tooltip and the chat status line -----------
 *
 * The client builds its weather name twice, in two near-clones:
 *
 *   0x46eae0  void __cdecl(char *dest)  sprintf(dest, "%s - %s Wind", ...)
 *             the HUD tooltip, drawn by 0x441090 after a 1000 ms hover dwell.
 *             One call site, 0x441b86.
 *   0x46e820  void __cdecl(char *dest)  the chat status line printed on every
 *             wind re-roll, "#Y<map>#N: #Y<weather>#N and #Y<wind>#N Wind ".
 *             One call site, 0x46ee14, inside WeatherTick.
 *
 * Both assemble the weather word by testing the CLIENT's mask at 0x7decb0,
 * and both end with "if what I built is <= 1 char, write Sunny instead". Our
 * bits are deliberately not in that word (see g_mask), so for all nine of our
 * groups that Sunny branch is the one that always runs. Hence the hook.
 *
 * The spec used to call these two byte-identical clones. They are not: they
 * share their first 0xb4 bytes and then diverge, so no prologue pattern can
 * separate them and the operand offsets below are valid for the TOOLTIP
 * builder only. Both are reached through their call sites instead, which is
 * also where the E8 we rewrite lives.
 */

/* The E8 inside each call-site signature. */
#define WX_TOOLTIP_CALL_OFFSET 0x12
#define WX_STATUS_CALL_OFFSET  0x05

/* Both builders open `mov eax, [abs32]` / `sub esp, 0xa0`. Checked before a
 * resolved call target is believed, the way the icon hook checks for A1. */
#define WX_NAMEFN_PROLOGUE_A   0xA1
#define WX_NAMEFN_PROLOGUE_B   0x81
#define WX_NAMEFN_PROLOGUE_C   0xEC

/* "Light ", "Heavy " and "Medium " -- with their trailing spaces -- loaded
 * into registers at these offsets in the TOOLTIP builder. Reading them out of
 * the client means our prefix always matches what its own rain and snow say.
 * Each is guarded by the opcode that must be there. */
#define WX_LEVEL_LIGHT_OFFSET  0xA6   /* 8B 35    mov esi, [abs32] */
#define WX_LEVEL_HEAVY_OFFSET  0xD8   /* A1       mov eax, [abs32] */
#define WX_LEVEL_MEDIUM_OFFSET 0xF8   /* A1       mov eax, [abs32] */
#define WX_LEVEL_LIGHT_OPLEN   2
#define WX_LEVEL_HEAVY_OPLEN   1
#define WX_LEVEL_MEDIUM_OPLEN  1

/* An intensity word we accept from the client: short, printable, terminated.
 * Anything else and we keep our own literal rather than print rubbish. */
#define WX_LEVEL_WORD_MAX 16

/* The literal middles of the two format strings. A map name and a weather
 * word can contain neither, so they split the built line unambiguously. */
#define WX_TOOLTIP_SEPARATOR  " - "
#define WX_STATUS_FIELD_OPEN  "#N: #Y"
#define WX_STATUS_FIELD_CLOSE "#N and #Y"

/* Room for a whole built line. The client writes these into 0x400 and 0x464
 * byte stack frames with no bound of its own; ours is generous against a
 * stock worst case of about 105 bytes.
 *
 * Nothing here widens what a caller's buffer must hold: the field we
 * substitute is at most "Medium " + "Falling Leaves" = 21 characters, against
 * the stock builder's own 40-character worst case of
 * "Medium Rain" + "Medium Snow" + "Clouds" + "Thunderstorm". */
#define WX_NAME_SCRATCH 256

/* Writing the row table ourselves bypasses the client's clipping, so the pass
 * clamps against these before it touches anything. */
#define WX_SURFACE_MAX_DIM 4096

/* The client hand-clips its own particles to this box before calling DrawLine
 * (`cmp edi, 0x31f` / `cmp edx, 0x257` in the rain draw). We use the surface's
 * own dimensions instead, but keep the same one-pixel margin. */
#define WX_EDGE_MARGIN 1

/* ---- log.c --------------------------------------------------------------- */

void wx_log_open(const char *path);
void wx_log_prune(const char *dir, int keep);
void wx_log_close(void);
void wx_log(const char *fmt, ...);

/* ---- config.c ------------------------------------------------------------ */

void        wx_config_load(void);
const char *wx_game_dir(void);
int         wx_cfg_int(const char *key, int fallback);
int         wx_cfg_int_in(const char *section, const char *key, int fallback);
int         wx_cfg_copy(const char *key, const char *fallback, char *out, int size);
void        wx_cfg_set_int(const char *key, int value);

/* ---- scan.c -------------------------------------------------------------- */

/* Scan main.exe's .text for every signature. Returns the number of required
 * signatures that failed (0 = proceed). */
OSMW_RESULT wx_resolve_all(void);
void wx_resolve_reset(void);

/* The host image's .text span. Returns 0 before wx_resolve_all has run. */
int wx_text_span(unsigned char **out_lo, unsigned char **out_hi);

/* ---- patch.c ------------------------------------------------------------- */

#define WX_MAX_PATCH 24

typedef struct {
    void          *at;
    unsigned char  saved[WX_MAX_PATCH];
    int            len;
    int            active;
    OSMW_TOKEN     claim_token;
} wx_patch_t;

int  wx_patch_write(wx_patch_t *p, void *at, const void *bytes, int len,
                    OSMW_REGION_KIND kind, const char *label);
OSMW_RESULT wx_patch_undo(wx_patch_t *p);

/* Repoint a direct `call rel32` at `site` to `detour`. Five bytes, no
 * trampoline, and the callee is untouched -- every other caller of it still
 * goes straight there. Both of this mod's hooks are call-site patches, so it
 * never relocates a prologue and never allocates executable memory. */
int wx_patch_call_site(wx_patch_t *p, void *site, void *detour,
                       const char *label);

int  wx_iat_hook(const char *dll, const char *func, void *detour, void **orig,
                 const char *label);
OSMW_RESULT wx_iat_unhook(void);

/* ---- weather.c ----------------------------------------------------------- */

/* The most call sites of SetWeather we will patch. The stock client has four;
 * the cap is a guard against a pattern that has started matching data. */
#define WX_MAX_SET_SITES 8

/* Install the hooks. Returns 0 and changes nothing if a REQUIRED address is
 * missing or a required site does not look the way it must; the icon and
 * weather-name hooks are optional and stand down on their own. */
int  wx_weather_install(void);
OSMW_RESULT wx_weather_remove(void);

/* The groups currently running, as WX_BIT_* -- for the log and for tests. */
unsigned int wx_active_mask(void);

#endif /* OSMW_WEATHER_H */
