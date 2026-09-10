/* THE GENERIC LOADER'S MESSAGE IS DELIBERATELY LEFT ALONE.
 *
 * "File %s open error" is printed from 0x44B57C, inside the generic file
 * loader - code that runs constantly while the client loads assets and logs
 * in. NOPing it crashed the client at the login screen, which is the THIRD
 * time in this mod's history that patching startup-hot code has broken it
 * (the 0x4661E0 prologue detour, and now this).
 *
 * The rule that came out of it, and it is not negotiable: NEVER patch
 * instructions in code the client executes during startup or login. Only the
 * three in-module sites are silenced - they are cold, they run exclusively
 * inside a fight animation, and they carry the message that actually repeats
 * per cast.
 *
 * If "File %s open error" still shows in Off, the answer is NOT to NOP it
 * here. Either the message is acceptable, or it gets handled the way
 * mw-tools/tools/client-patch/mda_error_silence.py handles that family: as a
 * deliberate, opt-in STATIC patch applied to main.exe on disk, where no
 * running thread can be mid-instruction when it changes. */

/* Battle animation speed: the two knobs, the hotkey and the toast.
 *
 * The whole feature rests on three facts about the client, decoded in
 * mw-tools/docs/client-packet-re.md ("the fight animation clock"):
 *
 *  1. Fight animations are BLOCKING inline render loops with hardcoded trip
 *     counts. Motion comes from a table indexed by the loop counter, so the
 *     duration of an animation is `trip count x whatever one FrameSync costs`.
 *     Shorten FrameSync and the animation shortens with it.
 *
 *  2. FrameSync's return value is a monotonic frame counter, not a timestamp,
 *     and the render divisor it is taken modulo is 1. So a shorter sleep does
 *     not skip draws -- every in-between frame still renders, just sooner.
 *     That is why this mod scales the sleep instead of cutting trip counts.
 *
 *  3. FrameSync is GLOBAL. 54 call sites, only 32 of them fight animations;
 *     the rest are the map loop, loading screens and modal UI. Speeding it up
 *     unconditionally speeds up the whole client, so every change is gated on
 *     the byte at AnimFlag, which the client sets while a fight animation is
 *     playing and which gates the fight scene renderer's first instruction.
 *
 * Knob 1 replaces the 17-byte sleep block inside FrameSync -- both 30 ms
 * immediates included -- with a jump to the stub below.
 *
 * Knob 2 pokes the fight VFX overlay's 40 ms wall-clock step. That one is
 * already fight-only, so it needs no gate. It is NOT optional decoration: the
 * overlay advances on wall time, so speeding up the loop without scaling this
 * makes effects play TRUNCATED rather than faster.
 *
 * Silencing the client's MDA load errors is deliberately not here. It ships as
 * mw-tools/tools/client-patch/mda_error_silence.py so that players who delete
 * effect files instead of installing this mod still benefit -- and so that two
 * things are not writing the same bytes from two places.
 */
#include "osmw_fightspeed.h"

#pragma intrinsic(_ReturnAddress)

#include <intrin.h>
#include <stdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- state --------------------------------------------------------------- */

static fs_patch_t g_floor_patch;   /* the 17 bytes inside FrameSync */
static fs_patch_t g_vfx_patch;     /* the one imm8 byte */

static int  g_mode = FS_MODE_NORMAL;
static int  g_scale = 100;            /* percent; 100 = stock timing */
static int  g_anim_floor = FS_STOCK_FLOOR;

/* OFF mode: swallow the effect overlay draw. */
static int g_skip_files = 1;   /* [Fight] SkipAnimationFiles */
static int g_patch_vfx = 1;
static int g_patch_framesync = 1;
static int g_patch_framewait = 1;
static int g_hc_fires;      /* how many animations Off has actually skipped */

/* Read by the naked stub every frame. C keeps it correct; see fs_frame_tick. */
static volatile DWORD g_floor = FS_STOCK_FLOOR;

/* Where the stub rejoins the stock function: patch site + the block we took. */
static void *g_resume;

/* Sleep through our own import rather than the client's IAT slot -- we have
 * kernel32 anyway, and it keeps the stub independent of the client's imports. */
static void (WINAPI *g_sleep)(DWORD) = NULL;

/* Not read as a live gate - see fs_frame_tick. Kept because the SET of
 * instructions that write it is what defines the fight animation module. */
static const unsigned char *g_anim_flag;

/* Bounds of the fight animation module, derived at install time (see
 * derive_module_bounds). A FrameSync/FrameWait call whose RETURN ADDRESS falls
 * inside this range is part of a fight animation; anything else is the map,
 * a loading screen or a modal window and must be left at stock speed. */
static unsigned char *g_mod_lo;
static unsigned char *g_mod_hi;

/* Return address of the FrameSync caller, captured by the stub before it
 * disturbs esp. */
static volatile unsigned char *g_caller_ra;

/* FrameWait(ms), the real pacer for skill animations. */
static void (__cdecl *g_framewait_orig)(int);
static fs_patch_t g_fw_sites[FS_MAX_CALL_SITES];
static int        g_fw_site_count;
static const unsigned char *g_keys_plain;  /* 0x7E6BA0, non-extended scancodes */
static void (__cdecl *g_chat_line)(int, int, const char *, const char *);

/* hotkey */
static int   g_cycle_sc;                   /* scancode, 0 = disabled */
static int   g_key_was_down;
static DWORD g_cycle_at;                   /* last fire, for the cooldown */
static int   g_cycle_fires;                /* diagnostic: total fires this run */
static int   g_cycle_suppressed;           /* diagnostic: edges eaten by cooldown */

/* deferred config writeback */
static int   g_persist;
static int   g_dirty;
static DWORD g_dirty_at;
#define FS_PERSIST_DELAY_MS 3000

/* The last Mode value seen in config.ini. The settings poll triggers on the
 * FILE changing, not on the file disagreeing with the live mode; -1 until
 * install reads it. See settings_poll for why that distinction is the whole
 * point. */
static int   g_seen_mode = -1;

/* Same edge-on-file rule for CameraMotion. -1 until install has read it. */
static int   g_seen_cam = -1;


/* ---- knob derivation ------------------------------------------------------ */

/* Both knobs are the stock constant divided by the speed multiplier.
 *
 * The floor is clamped to 1 ms, never 0: a 0 ms floor turns FrameSync into a
 * spin, and the Sleep(0) earlier in the function is not enough of a yield on
 * its own. At the 1000 cap this gives a 3 ms floor, which puts a five-step
 * melee phase at ~15 ms -- instantaneous as far as anyone can see. */
static int floor_for(int percent)
{
    int ms = (FS_STOCK_FLOOR * 100) / percent;
    return ms < 1 ? 1 : ms;
}

static int vfx_step_for(int percent)
{
    int step = (FS_STOCK_VFX_STEP * 100) / percent;
    if (step < 1)
        step = 1;
    if (step > 255)
        step = 255;
    return step;
}

int fs_mode_get(void)
{
    return g_mode;
}

const char *fs_mode_name(int mode)
{
    switch (mode) {
    case FS_MODE_FAST: return "Fast";
    case FS_MODE_OFF:  return "Off";
    default:           return "Normal";
    }
}

void fs_mode_set(int mode)
{
    unsigned char step;

    if (mode < 0 || mode >= FS_MODE_COUNT)
        mode = FS_MODE_NORMAL;

    g_mode  = mode;
    g_scale = (mode == FS_MODE_NORMAL) ? 100 : FS_FAST_PERCENT;
    g_anim_floor = floor_for(g_scale);

    /* Knob 2. Written straight over the imm8; g_vfx_patch already holds the
     * stock byte from install time, so undo still restores 0x28. */
    step = (unsigned char)vfx_step_for(g_scale);
    if (g_vfx_patch.active) {
        DWORD old, dummy;
        unsigned char *at = (unsigned char *)g_vfx_patch.at;
        if (VirtualProtect(at, 1, PAGE_EXECUTE_READWRITE, &old)) {
            *at = step;
            VirtualProtect(at, 1, old, &dummy);
            FlushInstructionCache(GetCurrentProcess(), at, 1);
        }
    }

    fs_log("mode: %s -> scale %d%%, frame floor %d ms, vfx step %d ms, "
           "effect overlay %s",
           fs_mode_name(mode), g_scale, g_anim_floor, step,
           (mode == FS_MODE_OFF) ? "SUPPRESSED" : "drawn");
}

/* ---- the toast ------------------------------------------------------------ */

/* Feedback is not optional. A speed change with no visible confirmation is
 * indistinguishable from a dropped keypress, and players mash it.
 *
 * This goes through the client's CHAT line adder (0x4620C0), NOT the on-screen
 * printer at 0x45ED90. The first build used the latter and it was wrong:
 * 0x45ED90 is the modal error-box printer, and 87 of its 89 call sites pass
 * the same second argument, so that argument is not a style selector and there
 * is no quiet mode to ask it for. It drew the red box with the dashed border.
 *
 * 0x4620C0 is AddChatLine(channel, value, name, text): it appends to the
 * 0x400-entry ring at [0x70BFC0] in 0xC0-byte records. Channel 7 with a NULL
 * name is the client's own non-blocking notice line - the same call shape the
 * canned block at 0x406D73 uses, and the one the server-side muted notice
 * relies on. Cyan, in the chat area, no box, no input block.
 *
 * Called once per keypress, never per frame. The signature is optional, so a
 * client update that moves it costs the notice and nothing else. */
static void toast_line(const char *label, const char *value)
{
    char line[64];

    if (!g_chat_line)
        return;
    _snprintf(line, sizeof(line), "%s: %s", label, value);
    line[sizeof(line) - 1] = 0;
    g_chat_line(FS_CHAT_NOTICE_CHANNEL, 0, NULL, line);
}

static void toast(int mode)
{
    toast_line("Battle animations", fs_mode_name(mode));
}

/* ---- the hotkey ----------------------------------------------------------- */

/* Scancodes, not virtual keys: the client's key state is scancode-indexed and
 * the arrays are 0x59 entries, a range that ends exactly at F12. */
static const struct { const char *name; int sc; } FS_KEYS[] = {
    {"F1", 0x3B}, {"F2", 0x3C}, {"F3", 0x3D}, {"F4",  0x3E},
    {"F5", 0x3F}, {"F6", 0x40}, {"F7", 0x41}, {"F8",  0x42},
    {"F9", 0x43}, {"F10", 0x44}, {"F11", 0x57}, {"F12", 0x58},
};
#define FS_KEY_COUNT ((int)(sizeof(FS_KEYS) / sizeof(FS_KEYS[0])))
#define FS_KEY_ARRAY_LEN 0x59

static int scancode_for(const char *name)
{
    int i;
    if (!name || !name[0])
        return 0;
    for (i = 0; i < FS_KEY_COUNT; i++) {
        if (_stricmp(name, FS_KEYS[i].name) == 0)
            return FS_KEYS[i].sc;
    }
    return -1;
}

/* The client's own array, never GetAsyncKeyState.
 *
 * Players run up to five clients at once. GetAsyncKeyState is global and would
 * flip the speed in all five on one keypress; this array is only fed while
 * that window has input focus, so reading it is focus-correct for free.
 *
 * ONE array, not two. The first version ORed 0x7E6BA0 and 0x7E6C00 on the
 * assumption that one was "held" and the other "pressed this frame". That was
 * wrong: the driver at 0x494F30 is a prefix state machine, and the two arrays
 * are non-extended vs EXTENDED (0xE0/0xE1-prefixed) scancodes. F1-F12 are
 * plain, so they only ever appear in 0x7E6BA0, and ORing the extended array in
 * could only ever contribute a false positive from an unrelated key that
 * happens to share the index. Both halves are proper make/break. */
static int key_down(int sc)
{
    if (sc <= 0 || sc >= FS_KEY_ARRAY_LEN)
        return 0;
    return g_keys_plain && g_keys_plain[sc];
}

static void cycle_mode(void)
{
    fs_mode_set((g_mode + 1) % FS_MODE_COUNT);
    toast(g_mode);
    fs_log("hotkey: cycle -> %s (fire %d, %d suppressed by cooldown; "
           "Off has skipped %d animation(s) so far)",
           fs_mode_name(g_mode), g_cycle_fires, g_cycle_suppressed, g_hc_fires);

    if (g_persist) {
        g_dirty = 1;
        g_dirty_at = GetTickCount();
    }
}

static void hotkey_poll(void)
{
    int down;

    if (!g_cycle_sc)
        return;

    down = key_down(g_cycle_sc);

    /* Edge, then cooldown. The array is level state, so the edge is what stops
     * a held key cycling once per frame.
     *
     * The cooldown is a second guard, and it is here because the first build
     * DID step several times on one press and the reason is not yet proven --
     * see NOTES.txt. A cycle key that steps at most once per 350 ms is right
     * on its own terms, so this is a real behaviour rule rather than a patch
     * over a mystery, but the counters below exist so the log says whether
     * something underneath is still firing repeatedly. */
    if (down && !g_key_was_down) {
        DWORD now = GetTickCount();
        if (g_cycle_at && (now - g_cycle_at) < FS_CYCLE_COOLDOWN_MS) {
            g_cycle_suppressed++;
        } else {
            g_cycle_at = now;
            g_cycle_fires++;
            cycle_mode();
        }
    }
    g_key_was_down = down;
}

/* Debounced writeback: cycling through four speeds in a second must not write
 * config.ini four times. */
static void persist_poll(void)
{
    if (!g_dirty)
        return;
    if (GetTickCount() - g_dirty_at < FS_PERSIST_DELAY_MS)
        return;
    g_dirty = 0;
    g_seen_mode = g_mode;   /* we just wrote it; do not read it back as news */
    fs_cfg_set_int("Mode", g_mode);
    fs_log("config: persisted Mode=%d (%s)", g_mode, fs_mode_name(g_mode));
}

/* ---- the settings poll ---------------------------------------------------- */

/* Pick up a Mode changed outside this mod -- the in-engine settings panel, or
 * someone editing config.ini in Notepad -- without a restart.
 *
 * Same shape and the same reasoning as chatmod's SETTINGS_POLL_MS: on the game
 * thread, throttled, never from a worker. A thread parked inside this DLL is a
 * thread whose code disappears on unload.
 *
 * Only Mode and CameraMotion are polled. Every other key here decides which
 * hooks get installed (PatchFrameSync, PatchVfx, PatchCamera,
 * SkipAnimationFiles, CycleKey), and re-reading those would report a change
 * this mod cannot actually make at runtime.
 */
#define FS_SETTINGS_POLL_MS 2000

/* CameraMotion, on the same edge-on-file rule as Mode. It is polled from its
 * own function rather than inline because Mode's checks return early, and a
 * camera change must not be swallowed by a Mode value that has not moved.
 *
 * There is no hotkey for this one, so there is no PersistMode equivalent and
 * no dirty-window race: the file is the only writer. */
static void camera_poll(void)
{
    char text[32];
    int  mode;

    if (!fs_cfg_copy("CameraMotion", "", text, sizeof(text)) || !text[0])
        return;
    mode = fs_camera_mode_parse(text);
    if (mode == g_seen_cam)
        return;
    g_seen_cam = mode;
    if (mode == fs_camera_mode())
        return;

    fs_log("config: CameraMotion changed externally to %s",
           fs_camera_mode_name(mode));
    fs_camera_mode_set(mode);
    toast_line("Battle camera", fs_camera_mode_name(mode));
}

static void settings_poll(void)
{
    static DWORD last;
    DWORD now = GetTickCount();
    int mode;

    if (last && (now - last) < FS_SETTINGS_POLL_MS)
        return;
    last = now;

    camera_poll();

    mode = fs_cfg_int("Mode", -1);
    if (mode < 0 || mode >= FS_MODE_COUNT)
        return;             /* absent, or garbage: leave the live mode alone */

    /* Trigger on the FILE changing, not on the file disagreeing with g_mode.
     *
     * The level comparison is the obvious version and it is wrong in a way
     * that only shows up with PersistMode=0. There, the hotkey changes g_mode
     * but nothing ever writes it out, so the file keeps its old value forever
     * and a level check would "correct" the live mode back on the very next
     * poll -- the keypress would undo itself about two seconds later, which
     * reads as the hotkey being broken rather than as a config race.
     *
     * Edge-on-file also makes the g_dirty window a non-issue: between a
     * keypress and the debounced write the file still holds the old value, but
     * that value is not new, so there is nothing to adopt.
     */
    if (mode == g_seen_mode)
        return;
    g_seen_mode = mode;

    if (mode == g_mode)
        return;             /* file caught up with where we already were */

    fs_log("config: Mode changed externally to %d (%s)", mode,
           fs_mode_name(mode));
    fs_mode_set(mode);
    toast(mode);
}

/* ---- the per-frame hook --------------------------------------------------- */

/* Called by the stub on every FrameSync, which includes every iteration of a
 * blocking fight animation loop. That is what makes the hotkey work
 * mid-animation without a second hook.
 *
 * Must stay cheap: this runs at the client's frame rate. */
static int in_anim_module(const void *ra)
{
    const unsigned char *p = (const unsigned char *)ra;
    return g_mod_lo && p >= g_mod_lo && p <= g_mod_hi;
}


static void __cdecl fs_frame_tick(void)
{

    /* The gate is WHO CALLED US, not a global flag.
     *
     * The first build gated on [0x673B0D] and that was wrong in the worst way:
     * of its 18 writers 15 set it and only 3 clear it, so after the player's
     * first fight animation it stays 1 for the rest of the session and the
     * whole client - world map walking included - runs at the fast floor.
     * It is a latch that 0x40F040 consumes, not an "in animation" state. */
    g_floor = (DWORD)(in_anim_module((const void *)g_caller_ra)
                      ? g_anim_floor : FS_STOCK_FLOOR);

    hotkey_poll();
    persist_poll();
    settings_poll();
}

/* FrameWait(ms) spins until frame-begin + ms, so it sets a per-frame WALL
 * CLOCK duration and is completely independent of FrameSync's sleep floor.
 *
 * This is what actually paces skill animations, which is why the first build
 * barely changed them: the melee loop calls FrameWait(0) and is paced by the
 * FrameSync floor, but the skill loops call FrameWait(<duration>) and each
 * iteration costs roughly max(floor, duration). With a duration above 30 ms
 * the floor change was invisible.
 *
 * 27 of the 28 call sites are inside the fight animation module, but the gate
 * is applied anyway - the one outsider at 0x437886 is not ours to speed up. */
static void __cdecl fs_framewait_detour(int ms)
{
    /* Belt and braces for the race above: if the trampoline is somehow not
     * published yet, skipping the wait is a dropped frame. Calling through a
     * null pointer is a dead client. */

    if (!g_framewait_orig)
        return;

    /* Only fight-module call sites are redirected here, so there is nothing
     * to test: if we are running, the caller is an animation loop. */
    if (ms > 0) {
        int scaled = (ms * 100) / g_scale;
        ms = scaled < 1 ? 1 : scaled;
    }
    g_framewait_orig(ms);
}

/* The replacement for FrameSync's sleep block.
 *
 * Register state on entry, straight from the instructions above the patch:
 *     eax = now, ecx = the previous timestamp, edx = now - previous
 * edx is the only one we need. eax, ecx and edx are all dead after the block
 * (the stock code reloads every one of them past the resume point), so the
 * stub may clobber them freely.
 *
 * edi is NOT dead: it holds the timeGetTime function pointer and the very
 * instruction we resume to is `call edi`. pushad/popad around the C call is
 * what protects it.
 *
 * Sleep is stdcall, so it pops its own argument and the stack stays balanced
 * on both paths.
 */
static __declspec(naked) void fs_framesync_stub(void)
{
    __asm {
        /* [esp+12] is the caller's return address: FrameSync pushed ebx, esi
         * and edi before reaching this point, so three dwords sit above esp
         * and the fourth is the return address. Captured BEFORE pushad, which
         * would otherwise move esp out from under the offset. */
        mov  eax, [esp+12]
        mov  [g_caller_ra], eax
        pushad
        pushfd
        call fs_frame_tick
        popfd
        popad

        mov  eax, [g_floor]
        cmp  edx, eax
        jae  resume
        sub  eax, edx
        push eax
        call [g_sleep]
    resume:
        jmp  [g_resume]
    }
}

/* OFF mode: make the skill effect files unreadable, exactly as deleting them
 * would.
 *
 * THIS IS THE FOURTH ATTEMPT AND THE FIRST ONE BUILT ON EVIDENCE RATHER THAN A
 * GUESS. The first three tried to intervene inside the animation code -
 * suppress 0x44ADD0, suppress 0x4684F0, force the handle check at 0x4661E0 to
 * fail - and every one of them left OFF indistinguishable from FAST.
 *
 * The reason they could not work: the anim functions DO NOT SHARE a skip
 * mechanism. Type 3 (0x410E80) resolves a handle through 0x4661E0 and takes
 * `jl 0x4114E1`; type 2 (0x410A40) instead reads a per-unit frame count at
 * [edi+edx*4+0x13c] and takes `jle 0x410BEA`. Seven effect-load sites exist
 * against only three handle checks, so at most three of ten anim functions
 * could ever be reached that way.
 *
 * But deleting ani/skl*.mda demonstrably skips EVERY skill - players rely on
 * it and the third-party patch for the official release automates it by
 * scrambling the path strings. So a universal point does exist; it is simply
 * further upstream than any of those guesses. It is the FILE OPEN. Every
 * guard above is downstream of the same load: no file, no frames, no
 * animation, whatever the individual function then checks.
 *
 * So OFF fails the open for skill effect MDAs and lets each anim function
 * reach its own skip branch by itself. No per-function knowledge, and it is
 * equivalent to deletion by construction rather than by argument.
 *
 * The client has three ways in - fopen and _open from MSVCR71, CreateFileA
 * from KERNEL32 - and all three are hooked through the IMPORT TABLE. That is
 * deliberately the safest patch in this mod: one aligned pointer write, no
 * instruction bytes touched, so it cannot split an instruction and cannot race
 * a thread that is executing the code being patched. Both of those are
 * mistakes this mod has already made once.
 *
 * THE COST IS EXPECTED: damage floaters are drawn inside the skipped loops, so
 * OFF loses them - the same downside the deletion trick has, and structural
 * rather than incidental. FAST is the mode that keeps damage numbers.
 * SuppressOverlay=0 disables the whole thing and leaves OFF as a faster FAST. */

static FILE  *(__cdecl  *g_fopen_orig)(const char *, const char *);
static int    (__cdecl  *g_open_orig)(const char *, int, int);
static HANDLE (WINAPI   *g_createfile_orig)(LPCSTR, DWORD, DWORD,
                                            LPSECURITY_ATTRIBUTES, DWORD,
                                            DWORD, HANDLE);

/* "...skl<something>.mda", case-insensitive - the skill effect artwork, and
 * nothing else. ani/Obj*.mda, the fight background and every non-fight asset
 * are left alone. */
static int g_off_seen_outside;   /* diagnostic: opens refused nowhere */

/* Is fight animation code anywhere on this thread's stack?
 *
 * The immediate return address is not enough. The fight code does not open the
 * file itself - it goes through the shared asset loader, so fopen's caller is
 * always something like LoadMDA (0x46B573) or 0x45FAF3, never the module. The
 * log said so directly:
 *
 *   off: allowing 'ANI/skl237.MDA' (caller 0046B573 is outside the fight module)
 *
 * So the real question is whether the module is anywhere in the call chain.
 * Walking a proper frame-pointer chain is not safe here - this client is built
 * with optimisations and not every frame keeps ebp - so instead the thread's
 * own stack is scanned for any value that lands inside the module. A stale
 * word that merely looks like a module return address can only cause a skill
 * effect to be skipped one time too many, which is the benign direction.
 *
 * Bounded by the TEB's stack base and a fixed word count so it can neither run
 * off the mapping nor become expensive. */
#define FS_STACK_SCAN_WORDS 2048

static int module_on_stack(void)
{
    unsigned char **sp, **base;
    int n = 0;

    if (!g_mod_lo)
        return 0;

    /* NtTib.StackBase sits at fs:[4]; the current frame is just below us. */
    base = (unsigned char **)__readfsdword(0x04);
    sp   = (unsigned char **)&sp;
    if (!base || sp >= base)
        return 0;

    for (; sp < base && n < FS_STACK_SCAN_WORDS; sp++, n++) {
        unsigned char *v = *sp;
        if (v >= g_mod_lo && v <= g_mod_hi)
            return 1;
    }
    return 0;
}

static int is_skill_effect(const char *path, const void *ra)
{
    const char *dot, *base;
    size_t      n;

    if (g_mode != FS_MODE_OFF || !g_skip_files || !path)
        return 0;

    n = strlen(path);
    if (n < 8)
        return 0;
    /* ".ani", NOT ".mda".
     *
     * This is the whole correction. The original patch scrambles the path
     * string the client builds from "ani/skl%03dr.ani" and "ani/skl%03dl.ani"
     * (0x50F8E0 / 0x50F8F4), loaded at three sites in three functions -
     * 0x41208B, 0x412B6E, 0x41381E - and the three "Unable to open %s file!"
     * prints that follow, at 0x4121A5, 0x412BAD and 0x41396C, are exactly the
     * "3 calls inside 3 different functions that need to be NOP'd out".
     *
     * Refusing "ani/skl%03d.mda" instead - the effect artwork - is a different
     * file family on a different code path. It skipped the animation, but it
     * produced error boxes the original never shows and left the client
     * crashing on the next cast after a toggle back to Normal. The .ani path
     * is the one 15 years of use have proven safe to fail. */
    dot = path + n - 4;
    if (_stricmp(dot, ".ani") != 0)
        return 0;

    base = path + n;
    while (base > path && base[-1] != '\\' && base[-1] != '/')
        base--;
    {
        /* Stop two characters early: q[1] and q[2] must stay inside the
         * string. Scanning to *q read up to two bytes past the terminator on
         * every .mda the client opens, which is a page fault waiting for a
         * name that happens to end a page. */
        const char *q, *last = base + strlen(base);
        int matched = 0;
        for (q = base; q + 2 < last + 1 && q[0] && q[1] && q[2]; q++) {
            if ((q[0] == 's' || q[0] == 'S') &&
                (q[1] == 'k' || q[1] == 'K') &&
                (q[2] == 'l' || q[2] == 'L')) { matched = 1; break; }
        }
        if (!matched)
            return 0;
    }

    /* THE CALLER DECIDES, NOT JUST THE NAME.
     *
     * The client PRELOADS skill artwork at startup - refusing "ani/skl001.mda"
     * there produced "Unable to open ani/skl001.mda file!" on the launch
     * screen and a cascading "Read system image error!" behind it. Off is
     * supposed to skip a fight animation, not sabotage asset loading, so the
     * refusal is confined to opens that come from the fight animation module. */
    if (in_anim_module(ra) || module_on_stack()) {
        if (g_hc_fires < 4)
            fs_log("off: refusing '%s' (caller %p, module on stack)", path, ra);
        g_hc_fires++;
        return 1;
    }

    /* Not from the module. Allowed - but logged the first few times, because
     * if EVERY skill open lands here then the loader is a shared function and
     * the caller test is looking at the wrong frame. That would show up as
     * "Off does nothing" with a pile of these lines to explain why. */
    if (g_off_seen_outside < 6) {
        fs_log("off: allowing '%s' (caller %p is outside the fight module)",
               path, ra);
        g_off_seen_outside++;
    }
    return 0;
}

static FILE * __cdecl fs_fopen_detour(const char *path, const char *mode)
{
    if (is_skill_effect(path, _ReturnAddress()))
        return NULL;
    return g_fopen_orig(path, mode);
}

static int __cdecl fs_open_detour(const char *path, int flags, int pmode)
{
    if (is_skill_effect(path, _ReturnAddress()))
        return -1;
    return g_open_orig(path, flags, pmode);
}

static HANDLE WINAPI fs_createfile_detour(LPCSTR path, DWORD access, DWORD share,
                                          LPSECURITY_ATTRIBUTES sa, DWORD disp,
                                          DWORD flags, HANDLE tmpl)
{
    if (is_skill_effect(path, _ReturnAddress())) {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    return g_createfile_orig(path, access, share, sa, disp, flags, tmpl);
}

static int install_off_hooks(void)
{
    int n = 0;
    int result;
    result = fs_iat_hook("MSVCR71.dll", "fopen", (void *)fs_fopen_detour,
                         (void **)&g_fopen_orig, "MSVCR71 fopen IAT slot");
    if (result < 0) return -1;
    n += result;
    result = fs_iat_hook("MSVCR71.dll", "_open", (void *)fs_open_detour,
                         (void **)&g_open_orig, "MSVCR71 _open IAT slot");
    if (result < 0) return -1;
    n += result;
    result = fs_iat_hook("KERNEL32.dll", "CreateFileA",
                         (void *)fs_createfile_detour,
                         (void **)&g_createfile_orig,
                         "KERNEL32 CreateFileA IAT slot");
    if (result < 0) return -1;
    n += result;
    if (n > 0) {
        /* NOTHING IS SILENCED FROM HERE ANY MORE.
         *
         * The three in-module calls to 0x464EA0 that used to be NOP'd
         * (0x4121A5, 0x412BAD, 0x41396C) are simply not on the path a skill
         * effect takes. The log proved it: the load goes through the shared
         * asset loader, and the two messages a player actually sees come from
         * 0x44B57C and 0x46B5C4 - both inside that loader, both startup-hot,
         * both unsafe to patch from a worker thread.
         *
         * Those two are handled the way the original 15-year-old mod handled
         * it and the way this should have been done from the start: as a
         * STATIC patch to main.exe on disk, in
         * mw-tools/tools/client-patch/mda_error_silence.py, where no thread
         * can be mid-instruction when the bytes change. */
        fs_log("off: message suppression is not done here - apply "
               "tools/client-patch/mda_error_silence.py to main.exe");
    }
    return n;
}

/* ---- install / remove ----------------------------------------------------- */

static void load_hotkey_config(void)
{
    char buf[64];
    int  sc;

    fs_cfg_copy("CycleKey", "F11", buf, sizeof(buf));
    sc = scancode_for(buf);
    if (sc < 0) {
        fs_log("hotkey: '%s' is not a key this mod knows (F1..F12) -- disabled", buf);
        g_cycle_sc = 0;
    } else if (sc == 0) {
        fs_log("hotkey: CycleKey is empty -- disabled");
        g_cycle_sc = 0;
    } else if (!g_keys_plain) {
        fs_log("hotkey: the plain-scancode array did not resolve -- disabled, "
               "ini speed still applies");
        g_cycle_sc = 0;
    } else {
        g_cycle_sc = sc;
        fs_log("hotkey: %s (scancode 0x%02X) cycles Normal/Fast/Off, "
               "%d ms cooldown, notice line %s",
               buf, sc, FS_CYCLE_COOLDOWN_MS,
               g_chat_line ? "available" : "UNAVAILABLE (ChatLine missed)");
    }
}

/* The fight animation module, found from the AnimFlag writers.
 *
 * [0x673B0D] is useless as a live gate, but the SET of instructions that write
 * it is an excellent description of where the fight animation code lives: 18
 * sites, all of them inside it. Taking the lowest and highest gives bounds
 * derived entirely from a signature-resolved address, with no hardcoded VA and
 * no magic constant, and they self-adjust if a client update moves the code.
 *
 * Measured on the reference build: [0x40F235, 0x413E12]. That range contains
 * 27 of the 28 FrameWait sites and 28 of the 54 FrameSync sites, and excludes
 * every map, loading-screen and modal-UI caller - which is the property that
 * matters, since those are what leaked in the first build. */
static int derive_module_bounds(void)
{
    unsigned char *lo, *hi, *p;
    unsigned char  want[6];
    unsigned char *first = NULL, *last = NULL;

    if (!g_anim_flag || !fs_text_span(&lo, &hi))
        return 0;

    want[0] = 0xC6;                       /* mov byte ptr [abs32], imm8 */
    want[1] = 0x05;
    *(DWORD *)(want + 2) = (DWORD)(ULONG_PTR)g_anim_flag;

    for (p = lo; p <= hi - 7; p++) {
        if (memcmp(p, want, sizeof(want)) == 0) {
            if (!first)
                first = p;
            last = p;
        }
    }
    if (!first || first == last)
        return 0;

    g_mod_lo = first;
    g_mod_hi = last;
    fs_log("module: fight animation code spans %p..%p (%u bytes)",
           g_mod_lo, g_mod_hi, (unsigned)(g_mod_hi - g_mod_lo));
    return 1;
}

/* camera.c needs the same bounds to prove the call sites it redirects are
 * fight-local. Exposed rather than duplicated: two independent derivations of
 * the same range is two things to keep in step. */
int fs_anim_module_bounds(unsigned char **lo, unsigned char **hi)
{
    if (!g_mod_lo || !g_mod_hi)
        return 0;
    if (lo) *lo = g_mod_lo;
    if (hi) *hi = g_mod_hi;
    return 1;
}

int fs_speed_install(void)
{
    unsigned char *site = fs_addr[FS_FRAMESYNCFLOOR];
    unsigned char *vfx  = fs_addr[FS_VFXSTEP];
    unsigned char  jmp_block[FS_FLOOR_PATCH_LEN];
    int i;

    fs_log("install: resolving state");

    if (!site || !vfx) {
        fs_log("install: a required address is missing -- standing down");
        return 0;
    }

    g_sleep       = (void (WINAPI *)(DWORD))Sleep;
    g_anim_flag  = fs_addr[FS_ANIMFLAG];
    g_keys_plain = fs_addr[FS_KEYARRAYA];   /* non-extended scancodes */
    g_chat_line  = (void (__cdecl *)(int, int, const char *, const char *))
                   fs_addr[FS_CHATLINE];

    if (!g_anim_flag || !derive_module_bounds()) {
        /* Without the gate the floor change reaches the map loop, loading
         * screens and every modal window - which is exactly what the first
         * build shipped. Refuse rather than do that again. */
        fs_log("install: could not establish the fight animation module bounds "
               "-- refusing to patch a global frame limiter with no gate");
        return 0;
    }

    /* Each code-byte patch has its own switch so a bad one can be found in a
     * single launch instead of a rebuild. Added after a login crash that
     * survived turning Off's whole file layer off, leaving three suspects and
     * no way to tell them apart. */
    g_patch_vfx       = fs_cfg_int("PatchVfx", 1) != 0;
    g_patch_framesync = fs_cfg_int("PatchFrameSync", 1) != 0;
    g_patch_framewait = fs_cfg_int("PatchFrameWait", 1) != 0;
    fs_log("install: switches - PatchVfx=%d PatchFrameSync=%d PatchFrameWait=%d",
           g_patch_vfx, g_patch_framesync, g_patch_framewait);

    fs_log("install: taking the vfx step byte");

    /* Knob 2 first: take a copy of the stock imm8 so undo can restore it even
     * if the floor patch below fails. */
    if (g_patch_vfx &&
        !fs_patch_write(&g_vfx_patch, vfx + FS_VFXSTEP_IMM,
                        vfx + FS_VFXSTEP_IMM, 1, OSMW_REGION_CODE,
                        "fight VFX step immediate")) {
        fs_log("install: could not take the VFX step byte");
        return 0;
    }
    if (!g_patch_vfx)
        fs_log("install: PatchVfx=0 - vfx step left at stock");

    fs_log("install: patching the frame floor");

    /* Knob 1: jmp rel32 to the stub, nop-padded across the rest of the block. */
    g_resume = site + FS_FLOOR_PATCH_LEN;
    jmp_block[0] = 0xE9;
    *(DWORD *)(jmp_block + 1) =
        (DWORD)((unsigned char *)fs_framesync_stub - (site + 5));
    for (i = 5; i < FS_FLOOR_PATCH_LEN; i++)
        jmp_block[i] = 0x90;

    if (g_patch_framesync) {
        if (!fs_patch_write(&g_floor_patch, site, jmp_block,
                            FS_FLOOR_PATCH_LEN, OSMW_REGION_CODE,
                            "FrameSync floor block")) {
            fs_patch_undo(&g_vfx_patch);
            fs_log("install: could not patch the frame floor");
            return 0;
        }
    } else {
        fs_log("install: PatchFrameSync=0 - frame floor left at stock 30 ms");
    }

    /* Knob 3: FrameWait, the pacer skill animations actually use. Failing
     * here is not fatal - melee still scales off the frame floor - but it is
     * the difference between "skills are faster" and "skills are unchanged",
     * so say so loudly. */
    fs_log("install: redirecting FrameWait call sites");

    if (!g_patch_framewait) {
        fs_log("install: PatchFrameWait=0 - skill pacing left at stock");
    } else if (fs_addr[FS_FRAMEWAIT]) {
        /* REDIRECT THE CALL SITES, DO NOT HOOK THE FUNCTION.
         *
         * 0x494F10 ends with `jmp 0x494F13` at 0x494F2B - its spin loop
         * branches back INTO its own first nine bytes. A prologue detour
         * therefore sends that loop into the middle of the replacement jump
         * the moment a wait does not elapse immediately, which is what killed
         * the client on the first FrameWait(5) from the login loop.
         *
         * Redirecting the callers leaves 0x494F10 completely intact, and only
         * the fight module's own calls are taken - the login loop's call at
         * 0x437886 keeps going straight to the real function. */
        unsigned char *target = fs_addr[FS_FRAMEWAIT];
        unsigned char *q;

        g_framewait_orig = (void (__cdecl *)(int))target;
        for (q = g_mod_lo; q <= g_mod_hi && g_fw_site_count < FS_MAX_CALL_SITES; q++) {
            if (q[0] != 0xE8)
                continue;
            if (q + 5 + *(LONG *)(q + 1) != target)
                continue;
            if (!fs_patch_call_site(&g_fw_sites[g_fw_site_count], q,
                                    (void *)fs_framewait_detour,
                                    "fight-local FrameWait caller")) {
                fs_log("install: FrameWait caller %p could not be claimed", q);
                return 0;
            }
            g_fw_site_count++;
        }
        if (g_fw_site_count > 0) {
            fs_log("install: FrameWait - %d in-module call site(s) redirected "
                   "(0x494F10 itself left untouched)", g_fw_site_count);
        } else {
            g_framewait_orig = NULL;
            fs_log("install: FrameWait - no in-module call sites found, skill "
                   "pacing left at stock");
        }
    } else {
        fs_log("install: FrameWait did not resolve -- skill animations will "
               "not scale, only melee");
    }

    fs_log("install: Off file layer");

    /* Old name SuppressOverlay is still honoured: it shipped in test configs
     * and describes nothing the mod does any more. */
    g_skip_files = fs_cfg_int("SkipAnimationFiles",
                                    fs_cfg_int("SuppressOverlay", 1)) != 0;
    if (g_skip_files) {
        int taken = install_off_hooks();
        if (taken < 0) {
            fs_log("off: an import slot could not be claimed");
            return 0;
        } else if (taken > 0) {
            fs_log("off: %d file-open import(s) hooked", taken);
        } else {
            g_skip_files = 0;
            fs_log("off: no file-open imports hooked -- Off will behave as Fast");
        }
    }

    fs_log("install: hotkey and mode");

    load_hotkey_config();
    g_seen_mode = fs_cfg_int("Mode", FS_MODE_NORMAL);
    fs_mode_set(g_seen_mode);

    g_persist = fs_cfg_int("PersistMode", 1) != 0;

    /* Optional and last: the fight camera. It is a separate feature on the
     * same module bounds, and a miss here must not cost anyone Fast or Off,
     * so its result is logged and otherwise ignored. */
    fs_log("install: fight camera");
    if (fs_camera_install() < 0)
        fs_log("install: camera rolled itself back - battle speed unaffected");
    g_seen_cam = fs_camera_mode();

    fs_log("install: complete - frame floor at %p -> stub %p, resume %p",
           site, (void *)fs_framesync_stub, g_resume);
    return 1;
}

OSMW_RESULT fs_speed_remove(void)
{
    OSMW_RESULT result;

    /* Camera first: its detour is reached only from the fight module, but it
     * is still a pointer into this DLL, so it goes before anything else. */
    result = fs_camera_remove();
    if (result != OSMW_OK)
        return result;

    /* Floor next: once it is back, nothing calls into this DLL any more. */
    result = fs_patch_undo(&g_floor_patch);
    if (result != OSMW_OK)
        return result;
    while (g_fw_site_count > 0) {
        result = fs_patch_undo(&g_fw_sites[g_fw_site_count - 1]);
        if (result != OSMW_OK)
            return result;
        --g_fw_site_count;
    }
    result = fs_iat_unhook();
    if (result != OSMW_OK)
        return result;
    result = fs_patch_undo(&g_vfx_patch);
    if (result != OSMW_OK)
        return result;
    g_framewait_orig = NULL;

    if (g_dirty) {
        g_dirty = 0;
        fs_cfg_set_int("Mode", g_mode);
    }
    return OSMW_OK;
}
