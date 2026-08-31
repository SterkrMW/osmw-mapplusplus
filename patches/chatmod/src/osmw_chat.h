/* osmw_chat -- chat window QoL mod for the MythWar (osMW) client.
 *
 * Injected into main.exe. Resolves its hook points by pattern scan at runtime
 * (patterns generated from signatures.ini into sigs.h) so a client update that
 * moves code disables the mod instead of corrupting the process.
 *
 * Phase 1 is scaffolding only: resolve, verify, log. No behaviour is changed.
 */
#ifndef OSMW_CHAT_H
#define OSMW_CHAT_H

#include <windows.h>
#include "sigs.h"

/* ---- resolved target addresses ------------------------------------------ */

/* Absolute addresses in the host process, indexed by the MW_* enum in sigs.h.
 * NULL means that signature did not resolve. */
extern unsigned char *mw_addr[MW_ADDR_COUNT];
extern unsigned char *mw_base;        /* main.exe image base */
extern unsigned int   mw_timestamp;   /* PE TimeDateStamp, identifies the build */

/* ---- log.c --------------------------------------------------------------- */

void mw_log_open(const char *path);
void mw_log_prune(const char *dir, int keep);
void mw_log_close(void);
void mw_log(const char *fmt, ...);

/* ---- scan.c -------------------------------------------------------------- */

/* Scan main.exe's .text for every signature. Returns the number that failed;
 * 0 means everything resolved and mw_addr[] is fully populated. */
int  mw_resolve_all(void);

/* ---- hook.c -------------------------------------------------------------- */

#define MW_MAX_PROLOGUE 24

typedef struct {
    void          *target;                    /* patched address */
    void          *trampoline;                /* call this to reach the original */
    unsigned char  saved[MW_MAX_PROLOGUE];    /* original bytes, for removal */
    int            len;                       /* how many bytes we replaced */
    int            active;
} mw_hook_t;

/* Redirect `target` to `detour`, relocating `prologue_len` bytes into a
 * trampoline. `prologue_len` must be >= 5 and land on an instruction boundary
 * -- callers pass a value verified against the signature, so there is no
 * length disassembler here. Returns 1 on success. */
int  mw_hook_install(mw_hook_t *h, void *target, void *detour, int prologue_len);
void mw_hook_remove(mw_hook_t *h);

/* Rewrite an indirect `call [iat]` site (6 bytes) into a direct `call detour`
 * plus a nop. Used for the CreateFontA call sites, where the arguments are
 * already on the stack. Returns 1 on success. */
int  mw_patch_call(mw_hook_t *h, void *site, void *detour);

/* Repoint a direct `call rel32` (5 bytes) at `detour`, returning the address it
 * previously called in *original. The callee itself is left alone, so other
 * callers of it are unaffected -- necessary when the callee is shared, as the
 * CRT sprintf behind the line composer is. */
int  mw_patch_direct_call(mw_hook_t *h, void *site, void *detour, void **original);

/* ---- font.c -------------------------------------------------------------- */

/* Redirect the client's two CreateFontA call sites. The fonts are built once,
 * during the client's startup init, so this must be installed before then --
 * i.e. the mod has to be injected at launch, not into a running client. */
int  mw_font_install(void);
void mw_font_remove(void);

/* Apply a font settings change without restarting the client. MUST be called
 * on the game thread; the settings poll in the window procedure is that. */
void mw_font_poll(void);

/* ---- window.c ------------------------------------------------------------ */

/* The single owner of the chat window subclass. Features register behaviour by
 * exposing a *_on_message handler, which this dispatches to; nothing else may
 * call SetWindowLongA(GWL_WNDPROC) on the chat window. */
int     mw_window_install(HWND hwnd);
void    mw_window_remove(void);
LRESULT mw_window_call_original(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
HWND    mw_window_hwnd(void);

/* Re-read the settings the message handlers consult. */
void mw_settings_refresh(void);

/* Throttled settings poll. Driven from both the window procedure and the
 * render so an edit takes effect whether or not you touch the window. */
void mw_window_poll_settings(void);

/* ---- geometry.c ---------------------------------------------------------- */

/* Intercepts the reposition routine so the window stays where the player puts
 * it, and widens the size limits the client pins shut. */
int  mw_geometry_install(HWND hwnd);
void mw_geometry_remove(void);
int  mw_geometry_on_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT *result);

/* ---- clear.c ------------------------------------------------------------- */

int  mw_clear_install(HWND hwnd);
void mw_clear_remove(void);
int  mw_clear_on_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT *result);
void mw_clear_sync_hotkey(HWND hwnd);
void mw_clear_now(const char *why);

/* "CTRL+SHIFT+C", "ALT+F1", "CTRL+DELETE"; returns 0 for empty or unparseable. */
int  mw_parse_hotkey(const char *spec, UINT *mods, UINT *vk);

/* ---- chips.c -------------------------------------------------------------- */

/* Scales the channel chips to sit beside a larger message font. Hooks the
 * atlas builder, so it must be installed before any font re-apply triggers
 * one. */
int  mw_chips_install(void);
void mw_chips_remove(void);

/* ---- mirror.c ------------------------------------------------------------ */

/* Redirects the chat window's own references to the global message ring onto a
 * private copy, so the window can be given a filtered view without disturbing
 * the several other subsystems that read the real one. */
int            mw_mirror_install(void);
void           mw_mirror_remove(void);
unsigned char *mw_mirror_base(void);   /* NULL when not installed */

/* Which slot of the CLIENT's ring one of our slots came from, or -1. The line
 * composer sees only our ring but the timestamp is keyed on the client's index,
 * which is the one that survives a rebuild. */
int mw_mirror_real_slot(int mirror_slot);

/* Bring the mirror up to date with newly arrived chat. Append-only, so slot
 * indices stay stable and the window's cached line list stays valid. */
void mw_mirror_sync(unsigned mask);

/* Rebuild from scratch under a new mask. Renumbers, so this also invalidates
 * the window's cached line list. Call on a filter change or after a clear. */
void mw_mirror_rebuild(unsigned mask);

/* Diagnostics: put a synthetic message into the window's ring only. Never
 * reaches the server or the client's own ring; gone on the next rebuild. */
void mw_mirror_inject(unsigned channel, const char *name, const char *text);

/* Drop the window's cached line list, keeping the ring. Needed after anything
 * that changes text metrics, since those records cache wrap points and line
 * heights measured with the old font. */
void mw_mirror_invalidate_view(void);

/* ---- probe.c ------------------------------------------------------------- */

/* Render colour ramps into the pane so the engine's 16-bit colour
 * quantisation can be read off a screenshot. Diagnostic only. */
void mw_probe_colours(void);

/* ---- mention.c ----------------------------------------------------------- */

/* Re-read settings and, once you are in the world, learn your character name
 * from the main window caption. Safe to call repeatedly. */
void        mw_mention_refresh(void);
const char *mw_mention_name(void);

/* Does this message record mention you? PMs always count. */
int  mw_mention_matches(const unsigned char *rec);

/* Prefix the mention marker onto a MIRRORED record's text, in place. Never
 * call this on the client's own ring. */
void mw_mention_decorate(unsigned char *rec);

/* ---- cooldown.c ----------------------------------------------------------- */

/* Replace the client's generic "Please do not flood !" with the seconds
 * actually remaining. Reads the client's own gate and per-channel cooldown
 * table; changes nothing about what is allowed, which the server enforces. */
int  mw_cooldown_install(void);
void mw_cooldown_remove(void);
void mw_cooldown_refresh(void);

/* ---- scroll.c ------------------------------------------------------------ */

/* Mouse-wheel scrolling. The window has scroll buttons but no wheel handling;
 * this calls the client's own scroll routine so the wheel behaves exactly like
 * pressing a button, overlay included. */
int  mw_scroll_install(void);
void mw_scroll_remove(void);
void mw_scroll_refresh(void);
int  mw_scroll_on_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT *result);

/* ---- timestamps.c -------------------------------------------------------- */

/* Inline arrival times. The client stores no time on a message, so one is
 * observed as the message arrives and kept against the REAL ring's slot index
 * -- stable for the message's life, and therefore unaffected by the mirror
 * being rebuilt on every filter change. */
/* Hook the client's line composer so the stamp lands at the START of the line,
 * before the sender name, uniformly on every channel. Composing is also where
 * wrapping is measured and where the ~1KB line buffer lives, so the stamp
 * neither overflows the pane nor runs out of room on a long message.
 * Returns 0 if the site could not be hooked; the mod then falls back to
 * stamping the record's text, which lands after the name. */
int  mw_time_install(void);
void mw_time_remove(void);

/* Re-read the settings; on a change, rebuild the ring so the toggle applies to
 * the messages already on screen and not just to the next arrival. */
void mw_time_poll(void);
void mw_time_reset(void);
int  mw_time_enabled(void);

/* Record that this slot of the CLIENT's ring was just filled. Called for every
 * arrival, even when the feature is off, so switching it on stamps history
 * correctly instead of starting blank. */
void mw_time_observe(int real_slot);

/* Prefix the stamp onto a MIRRORED record's text, in place. Never call this on
 * the client's own ring. */
void mw_time_decorate(unsigned char *rec, int real_slot);

/* Count one record as the mirror takes it in, for the rebuild's report. Called
 * from the mirror rather than from where the stamp is written, because the
 * composer runs per line per frame. */
void mw_time_note(int real_slot);

/* Read and clear the stamped / no-known-time counts. A record that predates
 * injection legitimately has no time, so a rebuild reports both numbers rather
 * than leaving "nothing appeared" ambiguous. */
void mw_time_stats(int *stamped, int *no_time);

/* ---- tabs.c -------------------------------------------------------------- */

int  mw_tabs_install(HWND hwnd);
void mw_tabs_remove(void);
int  mw_tabs_on_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT *result);
void mw_tabs_sync_hotkey(HWND hwnd);
void mw_tabs_cycle(const char *why);
void mw_tabs_set(int index, const char *why);
unsigned mw_tabs_mask(void);    /* channel bitmask for the active tab */

/* Pixels the chat window's client rect is currently displaced by to make room
 * for the tab strip; 0 when the strip is off or not mid-render. Anything that
 * re-reads that rect from the window must re-apply this. */
int mw_tabs_rect_shift(void);

/* ---- config.c ------------------------------------------------------------ */

/* Reads the game root's config.ini, section [Chat]. */
void        mw_config_load(void);
int         mw_cfg_int(const char *key, int fallback);
int         mw_cfg_int_in(const char *section, const char *key, int fallback);
void        mw_cfg_set_int(const char *key, int value);
void        mw_cfg_clear(const char *key);
const char *mw_cfg_str(const char *key, const char *fallback);
const char *mw_game_dir(void);

#endif /* OSMW_CHAT_H */
