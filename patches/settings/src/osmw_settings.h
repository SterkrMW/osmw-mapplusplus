/* osmw_settings -- a core-managed in-engine settings panel for osMW mods.
 *
 * The core loads this DLL, owns its lifecycle and shared UiTick hook, and
 * resolves the generated signatures against one pristine executable image.
 *
 * Spec: mw-tools/tools/client-dll/settings-panel.md
 *
 * WHAT THIS MOD IS FOR. Five native mods ship with the client and expose about
 * sixty config.ini keys between them, and the only ways to change any of them
 * are a text editor or Maps++ -- which the mods stopped requiring when the
 * autoload patch landed. This is the in-client way, drawn with the client's
 * own tooltip frame and bitmap font so it looks like part of the game.
 *
 * HOW IT REACHES THE OTHER MODS: it writes config.ini, and they notice.
 * That is the whole contract. There are no cross-DLL calls, no shared header,
 * no exported settings struct and no init-order dependency. osmw_minimap reads
 * its keys on demand; osmw_chat, osmw_fightspeed and osmw_weather each poll the
 * file every couple of seconds on the game thread. A mod that is not loaded is
 * simply a mod whose keys nothing reads.
 */
#ifndef OSMW_SETTINGS_H
#define OSMW_SETTINGS_H

#include <windows.h>
#include "osmw_core_api.h"
#include "sigs.h"

/* ---- managed core binding ---------------------------------------------- */

/* Valid from module init until shutdown returns. */
const OSMW_CORE_API_V1 *st_core_api(void);
OSMW_OWNER st_core_owner(void);

/* ---- resolved target addresses ------------------------------------------ */

/* Borrowed process addresses returned by core, indexed by the ST_* enum in
 * sigs.h. NULL means that signature did not resolve. */
extern unsigned char *st_addr[ST_ADDR_COUNT];

/* ---- log.c --------------------------------------------------------------- */

void st_log(const char *fmt, ...);

/* ---- config.c ------------------------------------------------------------ */

int         st_cfg_int(const char *key, int fallback);
int         st_cfg_int_in(const char *section, const char *key, int fallback);
const char *st_cfg_str(const char *key, const char *fallback);
int         st_cfg_copy(const char *key, const char *fallback, char *out, int size);
void        st_cfg_set_int(const char *key, int value);
void        st_cfg_set_int_in(const char *section, const char *key, int value);
void        st_cfg_set_str(const char *key, const char *value);

/* ---- scan.c -------------------------------------------------------------- */

/* Resolve every module-owned signature through the core's pristine image.
 * Optional misses leave st_addr[] NULL. */
OSMW_RESULT st_resolve_all(void);
void st_resolve_reset(void);

/* ---- the settings model (settings.c) ------------------------------------- */

/* When a change actually reaches the player.
 *
 * This is shown per row, and it is not decoration. An [Addons] gate that was
 * off at startup means the module never registered its services, so no amount of
 * re-reading the ini can bring it up -- a panel that let that look instant
 * would be lying, and the player would conclude the mod is broken.
 */
typedef enum {
    ST_APPLY_NOW = 0,      /* within the reading mod's poll, ~2 s worst case */
    ST_APPLY_RELAUNCH      /* needs the client restarted */
} st_apply_t;

typedef enum {
    ST_ROW_HEADING = 0,    /* a section label; not clickable */
    ST_ROW_TOGGLE,         /* [X] / [ ] */
    ST_ROW_CYCLE           /* [ Normal ] -> [ Fast ] -> [ Off ] */
} st_rowkind_t;

#define ST_MAX_CHOICES 4

typedef struct {
    st_rowkind_t kind;
    const char  *label;
    const char  *section;      /* ini section, NULL for a heading */
    const char  *key;
    int          fallback;     /* value when the key is absent everywhere */
    st_apply_t   applies;
    /* ST_ROW_CYCLE only: the names, in value order. NULL-terminated. */
    const char  *choices[ST_MAX_CHOICES];
    /* The DLL this row belongs to, for the "is it even loaded" check. NULL
     * means the row is not tied to one module (headings, [Debug]). */
    const char  *dll;
} st_row_t;

extern const st_row_t ST_ROWS[];
extern const int      ST_ROW_COUNT;

/* Current value of a row, read straight from the ini. */
int  st_row_value(const st_row_t *row);
/* Advance a row: toggles flip, cycles step and wrap. Writes config.ini. */
void st_row_advance(const st_row_t *row);
/* Whether the module behind a row is loaded in this process. Headings and
 * rows with no dll always report present. */
int  st_row_present(const st_row_t *row);

/* ---- keys.c -------------------------------------------------------------- */

/* Read the client's own scancode array, never GetAsyncKeyState: players run up
 * to five clients at once and GetAsyncKeyState is global, so one press would
 * open the panel in all five. The client's array is only fed while that window
 * has input focus, which makes this focus-correct for free. */
int  st_key_scancode_for(const char *name);   /* "F12" -> 0x58; -1 unknown, 0 empty */
void st_key_init(void);
/* 1 on the frame the key goes down, 0 otherwise. Edge-triggered: the array is
 * level state, so without this a held key fires every frame. */
int  st_key_pressed(int scancode);

/* ---- panel.c ------------------------------------------------------------- */

typedef struct {
    int left, top, right, bottom;
} st_rect_t;

/* 0x45E6F0. Returns 0 on success, -2 when no node has that handle.
 *
 * `out_rect` MUST be a full 16-byte rect: the callee writes four dwords
 * through it unconditionally. Passing anything smaller overruns the caller's
 * frame. */
int st_node_rect(int handle, st_rect_t *out_rect, int *out_flag);
/* 0x45E750. Assigns the rect and flag in place; relinks nothing, so it is the
 * one geometry change that is safe from inside a draw or input proc. */
int st_node_set_rect(int handle, const st_rect_t *rect, int flag);

int  st_panel_install(void);
void st_panel_remove(void);
/* Called from the core's post-UiTick callback after the client's own pass.
 * This is the only normal runtime point where a node is created or destroyed. */
void st_panel_tick(void);
int  st_panel_is_open(void);

/* Whether the client has a world up. Keys off the grid scales, which the .map
 * loader writes and which are never legitimately zero once a map is loaded.
 *
 * This also keeps early core startup from treating zeroed client globals as a
 * live world. */
int st_client_in_world(void);

#endif /* OSMW_SETTINGS_H */
