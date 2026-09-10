/* The fight camera: stop the view lurching once per action.
 *
 * Players call this "combat screen shake". It is not a shake, and the client
 * has no whole-screen shake at all - see the header, and the "the fight
 * camera" worked example in mw-tools/docs/client-packet-re.md.
 *
 * THREE drivers move the fight view. Each live trial found another one, so the
 * order they are listed in is the order they were learned, worst first:
 *
 *   CameraStep (0x40CE80) is THE fight camera, and it was found last.
 *       Exactly one caller, 0x40F049, inside the fight scene renderer and
 *       gated on AnimFlag, so it runs on EVERY RENDERED FRAME of EVERY fight
 *       animation - melee very much included. It eases the view one pixel per
 *       axis per frame toward a goal held in [0x673B18] (scene), [0x673B10]
 *       (world centre X) and [0x673B14] (world centre Y), then clamps.
 *       The anim functions do not move the camera; they set that goal and let
 *       this ease to it. Melee tail 0x40FECF is exactly that - it writes the
 *       home triple into the goal and returns.
 *       This is why melee still moved with the other two handled, and it is
 *       the "slow smooth drift": it is literally 1px per frame.
 *   CameraCentre (0x40D560), 3 call sites. Called EVERY FRAME from the
 *       keyframe loops inside the three cinematic anims (0x412000, 0x412AE0,
 *       0x413790), which step [0x7430B0] through 0x44-byte records at
 *       [0x7430A8]. The scripted camera the big skills are built around; it
 *       writes the camera directly, so it overrides the ease.
 *   CameraPan (0x40F1C0), 13 call sites. The explicit blocking glide. Ten are
 *       the 12-frame restore at the end of an action, over a zero delta on the
 *       ordinary anims - dead time more than motion.
 *
 * Fixed has to stop all three. Stopping any subset just hands the view to
 * whichever one is left, which is what each trial reported.
 *
 * WHY CALL SITES, NOT THE FUNCTION.
 *
 * Two independent reasons, and either alone would settle it:
 *
 *  1. 0x40F1C0 carries `mov byte [AnimFlag], 0` at 0x40F235, which is the
 *     LOWEST AnimFlag write in .text and therefore the low bound
 *     derive_module_bounds() measures. Patching bytes in its body moves the
 *     bound and silently takes the speed feature's caller gate with it.
 *  2. Repointing `call rel32` leaves the callee completely intact, so a caller
 *     we did not classify still reaches the real function. That is the bar
 *     fight-speed.md sets for this feature.
 *
 * The whole unit is optional. Every failure path here logs once and returns,
 * leaving Normal / Fast / Off exactly as they were.
 */
#include "osmw_fightspeed.h"

#include <string.h>


/* How many pan call sites the reference build has, and what they are:
 *
 *   pan home, 12 frames, args from the saved home triple - ten sites
 *       0x40FEB9 0x4109DA 0x410E33 0x41149B 0x4117E3
 *       0x4119B7 0x411FAD 0x412A41 0x4136F1 0x413DE6
 *   pan away, 3 frames, args from the keyframe table - three sites
 *       0x4121DC 0x412BE3 0x4139A3
 *
 * A different count means the fight module is not the one this feature was
 * classified against, so the release bar ("the caller set has been
 * classified") no longer holds and the subfeature stands down. It does not
 * stand the mod down: this is checked after the speed patches are in.
 */
#define FS_CAM_EXPECTED_SITES 13
#define FS_CAM_MAX_SITES      16

/* CameraCentre call sites, the per-frame keyframe camera: 0x4122D3, 0x412F43
 * and 0x413A95, one in each cinematic anim. Same all-or-nothing rule. */
#define FS_CTR_EXPECTED_SITES 3
#define FS_CTR_MAX_SITES      8

static void (__cdecl *g_pan_orig)(int, int, int, int);
static fs_patch_t     g_pan_sites[FS_CAM_MAX_SITES];
static int            g_pan_site_count;

static void (__cdecl *g_centre_orig)(int, int, int, int *, int *);
static fs_patch_t     g_centre_sites[FS_CTR_MAX_SITES];
static int            g_centre_site_count;

/* CameraStep has ONE call site in the whole binary, 0x40F049, and it sits at
 * 0x40F049 - BELOW the derived module low bound (0x40F235 on the reference
 * build), because that bound is the lowest AnimFlag write and the renderer is
 * upstream of it. So this one is found by scanning all of .text and requiring
 * exactly one match, not by scanning the module range. It is fight-only by
 * construction anyway: its only caller is guarded by AnimFlag. */
#define FS_STEP_EXPECTED_SITES 1
#define FS_STEP_MAX_SITES      2

/* Bound on the converge loop in Snap. The ease moves one pixel per axis per
 * call, and the scene is clamped to 1600x900, so convergence cannot need more
 * steps than that; the loop also breaks as soon as a call changes nothing. The
 * bound exists so a client whose goal is unreachable cannot spin a frame. */
#define FS_STEP_CONVERGE_MAX 2048

static void (__cdecl *g_step_orig)(void);
static fs_patch_t     g_step_sites[FS_STEP_MAX_SITES];
static int            g_step_site_count;

/* The three camera globals, derived from CameraPan rather than hardcoded: it
 * opens by loading all three, and the signature already pins those bytes. Each
 * load has its opcode checked before its operand is trusted.
 *
 *   pan+0x0A  8B 3D <scene>    mov edi, [0x743140]
 *   pan+0x2A  8B 15 <camY>     mov edx, [0x743148]
 *   pan+0x3E  8B 2D <camX>     mov ebp, [0x743144]
 */
#define FS_CAM_SCENE_OP 0x0A
#define FS_CAM_CAMY_OP  0x2A
#define FS_CAM_CAMX_OP  0x3E

/* And the home block, resolved at its first instruction:
 *
 *   home+0x0F  A3 <world X>          mov [0x73BFDC], eax
 *   home+0x23  C7 05 <scene> 2E..    mov [0x73BFD8], 0x2E
 *   home+0x41  A3 <world Y>          mov [0x73BFE0], eax
 *
 * camera_global checks the opcode; the A3 form is one byte, so its second
 * check byte is passed as 0 and skipped. */
#define FS_HOME_WX_OP    0x0F
#define FS_HOME_SCENE_OP 0x23
#define FS_HOME_WY_OP    0x41

static int *g_cam_scene, *g_cam_x, *g_cam_y;

/* The fight home triple, read straight out of the client rather than inferred.
 * 0x40B5F0 writes it once per fight and it is constant for the duration; there
 * are two variants because there are two battle backdrops, so the ADDRESSES
 * are resolved once and the VALUES are read live at each use.
 *
 * This replaces the learn-by-repetition fallback below as the primary source,
 * and is why the first fight of a session now behaves like every later one:
 * the old rule needed two consecutive pan-home calls before it knew anything,
 * so the opening actions of a session ran unheld. */
static int *g_home_scene, *g_home_wx, *g_home_wy;

/* Where the client keeps putting the camera back to.
 *
 * The ten pan-home sites all pass the client's saved home triple - scene, and
 * a WORLD-CENTRE x/y, not a camera origin - written once per fight at
 * 0x40B5F0 and constant for its duration. One of them runs at the end of
 * every action, so the same (scene, x, y) recurs constantly. The
 * three pan-away sites pass a keyframe, which does not. Taking a target as
 * home once it has been asked for TWICE IN A ROW therefore identifies the home
 * shot with no build-specific constant and no second signature, and it
 * re-learns by itself after a mode change or a new fight.
 *
 * Learning runs in every mode, Stock included, so switching into Fixed
 * mid-fight needs no warm-up. Until home is known, Fixed leaves the keyframe
 * camera alone - which is the behaviour that shipped first, so an unlearned
 * home is a fallback and never a regression. */
static int g_home[3], g_home_valid;
static int g_last[3], g_last_valid;

/* One log line the first time the hold actually engages, so a live run can be
 * checked from the log instead of by eye. Reset whenever home is relearned. */
static int g_held_logged;

/* Read on every pan, written from the settings poll on the same game thread.
 * A plain aligned int on x86 is atomic enough for this: the detour only ever
 * reads it, and the worst a stale read could do is pick the previous mode for
 * one animation. Nothing is rewritten in code when it changes, which is the
 * property that makes toggling mid-animation safe. */
static volatile int g_cam_mode = FS_CAM_STOCK;

/* AnimFlag, so FIXED can reproduce the clear the real function does. */
static unsigned char *g_anim_flag_byte;


int fs_camera_mode(void)
{
    return g_cam_mode;
}

const char *fs_camera_mode_name(int mode)
{
    switch (mode) {
    case FS_CAM_SNAP:  return "Snap";
    case FS_CAM_FIXED: return "Fixed";
    default:           return "Stock";
    }
}

void fs_camera_mode_set(int mode)
{
    if (mode < 0 || mode >= FS_CAM_COUNT)
        mode = FS_CAM_STOCK;
    g_cam_mode = mode;
    fs_log("camera: motion %s", fs_camera_mode_name(mode));
}

int fs_camera_mode_parse(const char *text)
{
    if (!text)
        return FS_CAM_STOCK;
    while (*text == ' ' || *text == '\t')
        ++text;
    if (_stricmp(text, "snap") == 0 || _stricmp(text, "1") == 0)
        return FS_CAM_SNAP;
    if (_stricmp(text, "fixed") == 0 || _stricmp(text, "2") == 0)
        return FS_CAM_FIXED;
    return FS_CAM_STOCK;
}


/* The home shot, preferring the client's own globals and falling back to what
 * the pan targets taught us. Returns 0 if neither is available yet. */
static int home_triple(int *scene, int *wx, int *wy)
{
    if (g_home_scene && g_home_wx && g_home_wy) {
        *scene = *g_home_scene; *wx = *g_home_wx; *wy = *g_home_wy;
        return 1;
    }
    if (g_home_valid) {
        *scene = g_home[0]; *wx = g_home[1]; *wy = g_home[2];
        return 1;
    }
    return 0;
}


/* ---- the detour ----------------------------------------------------------- */

/* __cdecl, matching 0x40F1C0: bare `ret`, callers clean their own four
 * arguments. Verified at all thirteen sites. */
static void __cdecl fs_camera_pan_detour(int scene, int x, int y, int frames)
{
    if (g_last_valid && g_last[0] == scene && g_last[1] == x && g_last[2] == y &&
        (!g_home_valid || g_home[0] != scene || g_home[1] != x || g_home[2] != y)) {
        g_home[0] = scene; g_home[1] = x; g_home[2] = y;
        g_home_valid  = 1;
        g_held_logged = 0;
        fs_log("camera: home is scene %d, world centre (%d, %d)", scene, x, y);
    }
    g_last[0] = scene; g_last[1] = x; g_last[2] = y;
    g_last_valid = 1;

    switch (g_cam_mode) {
    case FS_CAM_SNAP:
        /* One iteration lands exactly on the target, because the step is
         * delta/frames. The client still does its own interpolation, clamping,
         * render and present - we never write a camera global ourselves and
         * never duplicate its clamp arithmetic. */
        g_pan_orig(scene, x, y, 1);
        return;

    case FS_CAM_FIXED:
        /* Skip it. Ten of the thirteen sites are restores TO the home shot,
         * so a camera that never leaves home never needs restoring, and a
         * mid-fight switch into this mode cannot strand the view.
         *
         * The real function clears AnimFlag before its early-out, so do the
         * same: it costs one byte and keeps observable state identical. */
        if (g_anim_flag_byte)
            *(volatile unsigned char *)g_anim_flag_byte = 0;
        return;

    default:
        g_pan_orig(scene, x, y, frames);
        return;
    }
}


/* The per-frame keyframe camera.
 *
 * __cdecl (scene, worldX, worldY, &outX, &outY). The caller pushes the two
 * camera globals as the out-parameters, and writes the keyframe scene index
 * into the scene global inline just before the call.
 *
 * In Fixed we answer with the home shot and put the scene index back, so the
 * cinematic plays out under the wide shot the fight opened on.
 *
 * IT IS A CALL-THROUGH WITH SUBSTITUTED ARGUMENTS, NOT A DIRECT WRITE, AND
 * THAT DISTINCTION IS THE WHOLE BUG THE FIRST BUILD SHIPPED.
 *
 * CameraPan takes WORLD-CENTRE coordinates; CameraCentre hands back a camera
 * ORIGIN. Pan converts between them itself:
 *
 *   pan+0x1F   ecx = dims[cur].w >> 1 + [camX]     current world centre X
 *   pan+0x30   eax = dims[cur].h >> 1 + [camY]     current world centre Y
 *   pan+0x44   edx = arg1 - ecx                    delta, in WORLD space
 *   0x40F31C   [camX] = interpolated - dims[scene].w >> 1    back to an origin
 *
 * g_home is learned from Pan, so it is a world centre. Writing it straight
 * into Centre's out-parameters put the camera about half a scene width off,
 * which tripped the caller's `camX + w >= 1600` clamp and pinned the view to
 * the far edge - the reported "flicks the opposite direction" - and then slid
 * as each keyframe's scene dimensions changed what that clamp produced, which
 * is the reported "slowly drifts back". Handing the same triple to the real
 * function converts it exactly, with no duplicated dimension arithmetic here
 * and no second signature for the dimensions table.
 *
 * The client itself keeps both forms side by side, which is the confirmation:
 * 0x40B5F0 writes the origin (0x15E, 0x16D) to [0x73BFD0/D4] and the world
 * centre, origin + dim/2, to [0x73BFDC/E0].
 *
 * The caller still clamps our answer against the KEYFRAME scene's dimensions,
 * which cannot be reached from in here. With a correct origin that is a
 * no-op unless a keyframe scene is wide enough to exclude it, and it cannot
 * accumulate: every frame comes back through here and rewrites all three. */
static void __cdecl fs_camera_centre_detour(int scene, int wx, int wy,
                                            int *out_x, int *out_y)
{
    int hs, hx, hy;

    if (g_cam_mode == FS_CAM_FIXED && out_x && out_y &&
        home_triple(&hs, &hx, &hy)) {
        if (g_cam_scene)
            *g_cam_scene = hs;
        g_centre_orig(hs, hx, hy, out_x, out_y);
        if (!g_held_logged) {
            g_held_logged = 1;
            fs_log("camera: holding the keyframe camera at home scene %d, "
                   "world (%d, %d) -> origin (%d, %d)",
                   hs, hx, hy, *out_x, *out_y);
        }
        return;
    }
    g_centre_orig(scene, wx, wy, out_x, out_y);
}


/* The per-frame camera ease, and the one that makes melee move.
 *
 * Stock is one pixel per axis per rendered frame toward [0x673B18/10/14].
 *
 *   Fixed  do not step at all. The camera is wherever the fight setup put it
 *          and stays there; nothing else in Fixed moves it either.
 *   Snap   step until it stops changing. That is the same destination the
 *          client was easing to, reached immediately instead of over a second
 *          or two, which is exactly what Snap means everywhere else here. The
 *          real function is pure arithmetic over three globals - no drawing,
 *          no allocation - so iterating it is safe and cheap.
 */
static void __cdecl fs_camera_step_detour(void)
{
    int i, sx, sy, ss;

    switch (g_cam_mode) {
    case FS_CAM_FIXED:
        return;

    case FS_CAM_SNAP:
        if (!g_cam_scene || !g_cam_x || !g_cam_y) {
            g_step_orig();          /* cannot detect convergence; stay stock */
            return;
        }
        for (i = 0; i < FS_STEP_CONVERGE_MAX; i++) {
            ss = *g_cam_scene; sx = *g_cam_x; sy = *g_cam_y;
            g_step_orig();
            if (*g_cam_scene == ss && *g_cam_x == sx && *g_cam_y == sy)
                break;
        }
        return;

    default:
        g_step_orig();
        return;
    }
}


/* ---- install / remove ----------------------------------------------------- */

/* Pull one absolute operand out of CameraPan, checking the opcode first. */
static int *camera_global(unsigned char *at, int op, unsigned char b0,
                          unsigned char b1, const char *what)
{
    /* b1 == 0 means a one-byte opcode (A3 = mov [abs32], eax), so the operand
     * follows immediately; otherwise it is opcode + modrm and the operand is
     * two bytes in. */
    int operand = (b1 == 0) ? op + 1 : op + 2;

    if (at[op] != b0 || (b1 != 0 && at[op + 1] != b1)) {
        fs_log("camera: +0x%02X is %02X %02X, not %02X %02X - cannot derive "
               "the %s global", op, at[op], at[op + 1], b0, b1, what);
        return NULL;
    }
    return *(int **)(at + operand);
}

/* Redirect every in-module `call rel32` landing on `target`, all or nothing.
 * Returns the number taken, 0 if the count is not `expected`, or -1 on a claim
 * failure with the caller left to roll the whole feature back. */
static int redirect_sites(unsigned char *target, void *detour, int expected,
                          fs_patch_t *slots, int max_slots, const char *label)
{
    unsigned char *lo, *hi, *q;
    int found = 0, taken = 0;

    if (!fs_anim_module_bounds(&lo, &hi))
        return 0;

    /* Count first, commit second. Standing down after redirecting some of the
     * sites would leave the camera behaving differently depending on which
     * animation happened to play. */
    for (q = lo; q <= hi - 5; q++)
        if (q[0] == 0xE8 && q + 5 + *(LONG *)(q + 1) == target)
            found++;
    if (found != expected) {
        fs_log("camera: found %d in-module %s call site(s), expected %d - the "
               "caller set is not the one this feature was classified against",
               found, label, expected);
        return 0;
    }
    for (q = lo; q <= hi - 5 && taken < max_slots; q++) {
        if (q[0] != 0xE8 || q + 5 + *(LONG *)(q + 1) != target)
            continue;
        if (!fs_patch_call_site(&slots[taken], q, detour, label)) {
            fs_log("camera: %s site %p could not be claimed", label, q);
            return -1;
        }
        taken++;
    }
    return taken;
}

/* Same contract as redirect_sites, but over the whole of .text.
 *
 * CameraStep's only caller is at 0x40F049, below the derived module low bound,
 * so the module-range scan cannot see it. Requiring EXACTLY one match across
 * .text is the stronger check anyway: it proves the callee has the single
 * caller this feature was classified against. */
static int redirect_sites_text(unsigned char *target, void *detour, int expected,
                               fs_patch_t *slots, int max_slots,
                               const char *label)
{
    unsigned char *lo, *hi, *q;
    int found = 0, taken = 0;

    if (!fs_text_span(&lo, &hi))
        return 0;
    for (q = lo; q <= hi - 5; q++)
        if (q[0] == 0xE8 && q + 5 + *(LONG *)(q + 1) == target)
            found++;
    if (found != expected) {
        fs_log("camera: found %d %s call site(s) in .text, expected %d - the "
               "caller set is not the one this feature was classified against",
               found, label, expected);
        return 0;
    }
    for (q = lo; q <= hi - 5 && taken < max_slots; q++) {
        if (q[0] != 0xE8 || q + 5 + *(LONG *)(q + 1) != target)
            continue;
        if (!fs_patch_call_site(&slots[taken], q, detour, label)) {
            fs_log("camera: %s site %p could not be claimed", label, q);
            return -1;
        }
        taken++;
    }
    return taken;
}

int fs_camera_install(void)
{
    unsigned char *pan    = fs_addr[FS_CAMERAPAN];
    unsigned char *centre = fs_addr[FS_CAMERACENTRE];
    unsigned char *step   = fs_addr[FS_CAMERASTEP];
    unsigned char *home   = fs_addr[FS_FIGHTHOME];
    unsigned char *lo, *hi;
    char           text[32];
    int            taken;

    if (!fs_cfg_int("PatchCamera", 1)) {
        fs_log("camera: PatchCamera=0 - fight camera left at stock");
        return 0;
    }
    if (!pan) {
        fs_log("camera: CameraPan did not resolve - fight camera left at "
               "stock, battle speed unaffected");
        return 0;
    }
    if (!fs_anim_module_bounds(&lo, &hi)) {
        fs_log("camera: no fight animation module bounds - refusing to "
               "redirect a call site we cannot prove is fight-local");
        return 0;
    }

    g_pan_orig       = (void (__cdecl *)(int, int, int, int))pan;
    g_anim_flag_byte = fs_addr[FS_ANIMFLAG];
    g_cam_scene      = camera_global(pan, FS_CAM_SCENE_OP, 0x8B, 0x3D, "scene");
    g_cam_y          = camera_global(pan, FS_CAM_CAMY_OP,  0x8B, 0x15, "camera Y");
    g_cam_x          = camera_global(pan, FS_CAM_CAMX_OP,  0x8B, 0x2D, "camera X");

    /* The home triple, read out of the fight setup block. Optional: without it
     * the learn-by-repetition fallback still works, it just needs two actions
     * of the session before it knows anything. */
    if (home) {
        int *scene = camera_global(home, FS_HOME_SCENE_OP, 0xC7, 0x05, "home scene");
        int *wy    = camera_global(home, FS_HOME_WY_OP,    0xA3, 0x00, "home world Y");
        int *wx    = camera_global(home, FS_HOME_WX_OP,    0xA3, 0x00, "home world X");
        /* Cross-check the layout rather than assuming the three are adjacent:
         * the block writes scene, then origin X/Y, then world Y, and world X
         * four bytes below the scene. If that does not hold, this is not the
         * block we decoded and the fallback takes over. */
        if (scene && wx && wy && wx == scene + 1 && wy == scene + 2) {
            g_home_scene = scene; g_home_wx = wx; g_home_wy = wy;
            fs_log("camera: home globals at %p/%p/%p", scene, wx, wy);
        } else {
            fs_log("camera: home block did not decode as expected - falling "
                   "back to learning home from the pan targets");
        }
    } else {
        fs_log("camera: FightHome did not resolve - learning home from the "
               "pan targets instead, which costs the first two actions of a "
               "session");
    }

    taken = redirect_sites(pan, (void *)fs_camera_pan_detour,
                           FS_CAM_EXPECTED_SITES, g_pan_sites,
                           FS_CAM_MAX_SITES, "fight camera pan caller");
    if (taken < 0) {
        fs_camera_remove();
        return -1;
    }
    g_pan_site_count = taken;
    if (taken == 0) {
        fs_log("camera: standing down, battle speed unaffected");
        g_pan_orig = NULL;
        return 0;
    }

    /* The per-frame keyframe camera, and the reason Fixed did not hold still
     * on its first live trial. Optional on top of an optional feature: without
     * it Fixed still suppresses the pans, which is what shipped first. */
    if (!centre) {
        fs_log("camera: CameraCentre did not resolve - the cinematic keyframe "
               "camera will still move in Fixed");
    } else if (!g_cam_scene) {
        fs_log("camera: no scene global - not touching the keyframe camera, "
               "since holding it without the scene index would draw the wrong "
               "backdrop");
    } else {
        g_centre_orig = (void (__cdecl *)(int, int, int, int *, int *))centre;
        taken = redirect_sites(centre, (void *)fs_camera_centre_detour,
                               FS_CTR_EXPECTED_SITES, g_centre_sites,
                               FS_CTR_MAX_SITES, "fight keyframe camera caller");
        if (taken < 0) {
            fs_camera_remove();
            return -1;
        }
        g_centre_site_count = taken;
        if (taken == 0) {
            g_centre_orig = NULL;
            fs_log("camera: keyframe camera left at stock; the pans are still "
                   "handled");
        }
    }

    /* CameraStep, the per-frame ease. This is the one that moves the camera on
     * an ordinary melee, so without it Fixed is not "the view does not move".
     * Still optional: a miss leaves the pans and the keyframe camera handled
     * and says so. */
    if (!step) {
        fs_log("camera: CameraStep did not resolve - the per-frame camera ease "
               "will still run, so melee will still drift");
    } else {
        g_step_orig = (void (__cdecl *)(void))step;
        taken = redirect_sites_text(step, (void *)fs_camera_step_detour,
                                    FS_STEP_EXPECTED_SITES, g_step_sites,
                                    FS_STEP_MAX_SITES, "fight camera ease caller");
        if (taken < 0) {
            fs_camera_remove();
            return -1;
        }
        g_step_site_count = taken;
        if (taken == 0) {
            g_step_orig = NULL;
            fs_log("camera: per-frame camera ease left at stock");
        }
    }

    /* Old name from the QoL spec, which called this a screen shake before the
     * RE was done. ScreenShake=0 meant "stop it moving", so it maps to FIXED.
     * Kept only because that name was published; nothing ships it. */
    if (fs_cfg_copy("CameraMotion", "", text, sizeof(text)) && text[0]) {
        fs_camera_mode_set(fs_camera_mode_parse(text));
    } else {
        int shake = fs_cfg_int("ScreenShake", 1);
        fs_camera_mode_set(shake ? FS_CAM_STOCK : FS_CAM_FIXED);
    }

    fs_log("camera: %d pan, %d keyframe and %d ease call site(s) redirected "
           "(the callees themselves left untouched)",
           g_pan_site_count, g_centre_site_count, g_step_site_count);
    return g_pan_site_count + g_centre_site_count + g_step_site_count;
}

OSMW_RESULT fs_camera_remove(void)
{
    /* Reverse of installation: ease, then keyframe, then pans. */
    while (g_step_site_count > 0) {
        OSMW_RESULT result = fs_patch_undo(&g_step_sites[g_step_site_count - 1]);
        if (result != OSMW_OK)
            return result;
        --g_step_site_count;
    }
    while (g_centre_site_count > 0) {
        OSMW_RESULT result =
            fs_patch_undo(&g_centre_sites[g_centre_site_count - 1]);
        if (result != OSMW_OK)
            return result;
        --g_centre_site_count;
    }
    while (g_pan_site_count > 0) {
        OSMW_RESULT result = fs_patch_undo(&g_pan_sites[g_pan_site_count - 1]);
        if (result != OSMW_OK)
            return result;
        --g_pan_site_count;
    }
    g_pan_orig       = NULL;
    g_centre_orig    = NULL;
    g_step_orig      = NULL;
    g_cam_x          = NULL;
    g_cam_y          = NULL;
    g_home_scene     = NULL;
    g_home_wx        = NULL;
    g_home_wy        = NULL;
    g_anim_flag_byte = NULL;
    g_cam_scene      = NULL;
    g_home_valid     = 0;
    g_last_valid     = 0;
    g_held_logged    = 0;
    return OSMW_OK;
}
