/* osmw_fightspeed -- selectable battle animation speed for the MythWar (osMW) client.
 *
 * Loaded and initialised through osmw_core.dll's ABI 1 manifest. Generated
 * signatures resolve through core's pristine image; every code, call-site and
 * IAT range is exclusively claimed before this module's specialised safe
 * patch transaction writes it.
 *
 * Spec: mw-tools/tools/client-dll/fight-speed.md
 * RE:   mw-tools/docs/client-packet-re.md, "the fight animation clock"
 */
#ifndef OSMW_FIGHTSPEED_H
#define OSMW_FIGHTSPEED_H

#include <windows.h>
#include "osmw_core_api.h"
#include "sigs.h"

const OSMW_CORE_API_V1 *fs_core_api(void);
OSMW_OWNER fs_core_owner(void);

/* ---- resolved target addresses ------------------------------------------ */

/* Absolute addresses in the host process, indexed by the FS_* enum in sigs.h.
 * NULL means that signature did not resolve. */
extern unsigned char *fs_addr[FS_ADDR_COUNT];
extern unsigned char *fs_base;        /* main.exe image base */
extern unsigned int   fs_timestamp;   /* PE TimeDateStamp, identifies the build */

/* The reference build these signatures were generated against. A different
 * timestamp is a warning, not a failure -- the signatures are what decide. */
#define FS_REFERENCE_BUILD 0x45650696U

/* ---- the constants the client ships with -------------------------------- */

/* FrameSync's sleep floor, the `cmp edx, 0x1e` at the patch site. Everything
 * in a fight animation is a count of these. */
#define FS_STOCK_FLOOR 30

/* FrameWait(ms) at 0x494F10 spins until frame-begin + ms. It is what actually
 * paces skill animations, and 27 of its 28 call sites are inside the fight
 * animation module. Its prologue is `push ebp; mov ebp, esp; call [abs32]`. */
/* FrameWait is NOT prologue-hookable. 0x494F2B is `jmp 0x494F13`, a backward
 * branch into the middle of its own first nine bytes, so any prologue detour
 * leaves the spin loop jumping into the replaced instruction stream. Its call
 * sites are redirected instead. */

/* 0x4661E0: mov edx,[abs32]; push esi -> 7 bytes, position independent.
 * cdecl, bare `ret`. Returns the MDA slot index for a handle, or -2 when the
 * handle is not in the table. -2 is what the fight code treats as "no
 * animation to play". */
#define FS_HANDLE_NOT_FOUND (-2)

/* Off redirects individual CALL SITES, never the shared function - see the
 * "Off must not hook a shared function" note in speed.c. At most this many. */
#define FS_MAX_CALL_SITES 40


/* The fight VFX overlay's wall-clock step, the `add eax, 0x28` imm8 at
 * VfxStep + FS_VFXSTEP_IMM. */
#define FS_STOCK_VFX_STEP 40
#define FS_VFXSTEP_IMM    19

/* How long the cycle key is ignored after it fires. A cycle key that can only
 * step once per third of a second is correct behaviour on its own - nobody
 * legitimately cycles faster - and it also makes the mod immune to whatever is
 * producing repeat edges from the key array. See NOTES.txt. */
#define FS_CYCLE_COOLDOWN_MS 350

/* Chat channel 7 is the client's own non-blocking notice line, drawn in the
 * chat area on whichever tab is open. It is what the muted notice uses. */
#define FS_CHAT_NOTICE_CHANNEL 7

/* Bytes of FrameSync we take over: cmp / jae / sub / add / push / call Sleep.
 * Both 30 ms immediates are inside it, which is the point. */
#define FS_FLOOR_PATCH_LEN 17

/* THREE MODES, NOT A SPEED DIAL.
 *
 * The first build offered 1x/2x/3x/instant. In game 2x, 3x and 10x turned out
 * to be indistinguishable from each other while all three were clearly faster
 * than stock, which is what you would expect once the wall-clock waits are
 * scaled away and the only cost left is the render itself. A dial whose
 * positions nobody can tell apart is a worse control than a switch, so the
 * three positions are now modes:
 *
 *   NORMAL  stock timing, nothing scaled
 *   FAST    waits scaled away - as quick as the animation can be drawn
 *   OFF     FAST, plus the skill ANIMATION files are made unreadable so the
 *           client skips the cast by its own logic - what players get today by
 *           deleting them, and what the original patch does by scrambling the
 *           path string. Toggleable, exactly as the original is.
 */
#define FS_MODE_NORMAL 0
#define FS_MODE_FAST   1
#define FS_MODE_OFF    2
#define FS_MODE_COUNT  3

/* What FAST and OFF scale the wall-clock waits by, as a percentage. Kept as a
 * number rather than "as fast as possible" so it stays tunable from the ini
 * without another build; 300 and 1000 were indistinguishable in testing. */
#define FS_FAST_PERCENT 300

/* ---- the fight camera ----------------------------------------------------

 * The fight view is three globals: a scene/background index, and a scroll
 * origin into that scene. Exactly two functions write them during a battle,
 * and every call site of both is inside the fight animation module:
 *
 *   CameraPan (0x40F1C0, __cdecl (scene, x, y, frames))
 *       interpolates all three over `frames` FrameSync iterations, clamped to
 *       the scene bounds, rendering and presenting each step. 13 call sites:
 *       ten pan back to the saved home shot at 12 frames, three pan away to a
 *       keyframe at 3 frames.
 *
 *   CameraCentre (0x40D560, __cdecl (scene, x, y, &outX, &outY))
 *       centres the viewport on a world point. 3 call sites, the cinematic
 *       anims, which hard-cut the camera before their pan. NOT hooked - see
 *       fight-speed.md, "Phase B".
 *
 * The lurch players ask us to stop is the 12-frame pan home that runs at the
 * end of every single action - 12 x the 30 ms floor, about 360 ms, and on the
 * ten ordinary anims the camera has not moved, so it is a pan over a zero
 * delta: pure dead time that also drags the view about.
 *
 * CAMERA MODES, orthogonal to FS_MODE_*. A fourth speed value would have made
 * Normal+fixed and Fast+fixed unreachable.
 *
 *   STOCK  call through untouched
 *   SNAP   force frames = 1. The client still interpolates, clamps, renders
 *          and presents exactly once, so we never write a camera global
 *          ourselves and never duplicate its clamping. Lowest-risk mode.
 *   FIXED  skip the pan. Because ten of the thirteen are restores TO home,
 *          the camera simply never leaves the home shot and nothing is left
 *          stranded when the mode changes mid-fight.
 *
 * 0x40F1C0 CONTAINS `mov byte [AnimFlag], 0` AT 0x40F235 - one of the 18
 * AnimFlag writes, and the LOW BOUND derive_module_bounds() measures. So:
 * FIXED must reproduce that clear itself, and nothing here may ever patch
 * bytes inside 0x40F1C0's body, or the module bounds move and the speed
 * feature's caller gate goes with them. Call sites only.
 */
#define FS_CAM_STOCK 0
#define FS_CAM_SNAP  1
#define FS_CAM_FIXED 2
#define FS_CAM_COUNT 3

/* ---- log.c --------------------------------------------------------------- */

void fs_log_open(const char *path);
void fs_log_prune(const char *dir, int keep);
void fs_log_close(void);
void fs_log(const char *fmt, ...);

/* ---- config.c ------------------------------------------------------------ */

void        fs_config_load(void);
const char *fs_game_dir(void);
int         fs_cfg_int(const char *key, int fallback);
int         fs_cfg_int_in(const char *section, const char *key, int fallback);
int         fs_cfg_copy(const char *key, const char *fallback, char *out, int size);
void        fs_cfg_set_int(const char *key, int value);

/* ---- scan.c -------------------------------------------------------------- */

/* Scan main.exe's .text for every signature. Returns the number of required
 * signatures that failed (0 = proceed). Optional misses leave fs_addr[] NULL
 * for that slot and do not count. */
OSMW_RESULT fs_resolve_all(void);
void fs_resolve_reset(void);

/* The host image's .text span. Returns 0 before fs_resolve_all has run. */
int fs_text_span(unsigned char **out_lo, unsigned char **out_hi);

/* ---- patch.c ------------------------------------------------------------- */

/* Raw byte patching with an undo record. Deliberately not the trampoline
 * hooking the other mods use: this mod replaces a block of a function outright
 * and jumps back past it, so there is no prologue to relocate and no original
 * to call through to. */
#define FS_MAX_PATCH 24

typedef struct {
    void          *at;
    unsigned char  saved[FS_MAX_PATCH];
    int            len;
    int            active;
    OSMW_TOKEN     claim_token;
} fs_patch_t;

int  fs_patch_write(fs_patch_t *p, void *at, const void *bytes, int len,
                    OSMW_REGION_KIND kind, const char *label);
OSMW_RESULT fs_patch_undo(fs_patch_t *p);



/* Repoint a direct `call rel32` at `site` to `detour`. Five bytes, no
 * trampoline, and the callee is untouched - every other caller of it still
 * goes straight there. That is the whole point: it is how a hook can be
 * confined to chosen call sites instead of a whole function. */
int fs_patch_call_site(fs_patch_t *p, void *site, void *detour,
                       const char *label);

/* Swap one import-table entry. This is the SAFEST patch this mod makes: an
 * aligned pointer write, no instruction bytes touched, so it cannot split an
 * instruction and cannot race a thread executing the code being patched -
 * which is how a previous build killed the client at startup. Returns 0 if
 * the import is not found. */
int  fs_iat_hook(const char *dll, const char *func, void *detour, void **orig,
                 const char *label);
OSMW_RESULT fs_iat_unhook(void);

/* ---- speed.c ------------------------------------------------------------- */

/* Install the frame-floor detour and take ownership of the VFX step byte.
 * Returns 0 and changes nothing if a required address is missing. */
int  fs_speed_install(void);
OSMW_RESULT fs_speed_remove(void);

/* Current mode, one of FS_MODE_*. */
int  fs_mode_get(void);

/* Set the mode and re-derive every knob. Out-of-range values clamp to NORMAL. */
void fs_mode_set(int mode);

/* Human-readable mode name, for the notice line and the log. */
const char *fs_mode_name(int mode);

/* The fight animation module's derived bounds, or 0 before install has run.
 * camera.c uses them to prove every call site it redirects is fight-local. */
int fs_anim_module_bounds(unsigned char **lo, unsigned char **hi);

/* ---- camera.c ------------------------------------------------------------ */

/* Redirect the fight camera's pan call sites. Optional: a miss logs once and
 * leaves Normal/Fast/Off completely unaffected. Returns the number of sites
 * taken, or -1 if a site could not be claimed. */
int fs_camera_install(void);
OSMW_RESULT fs_camera_remove(void);

/* Current camera mode, one of FS_CAM_*. */
int  fs_camera_mode(void);
void fs_camera_mode_set(int mode);
const char *fs_camera_mode_name(int mode);

/* Parse a CameraMotion value: "stock" | "snap" | "fixed", or the legacy
 * ScreenShake 0/1. Returns FS_CAM_STOCK for anything unrecognised. */
int fs_camera_mode_parse(const char *text);

#endif /* OSMW_FIGHTSPEED_H */
