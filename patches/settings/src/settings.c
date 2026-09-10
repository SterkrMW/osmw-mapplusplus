/* The row table: what the panel shows, and what each row reads and writes.
 *
 * This is the only place the settings themselves are described. panel.c knows
 * how to draw a heading, a toggle and a cycle, and nothing about which keys
 * exist -- so adding a setting is a row here and no other change.
 *
 * Values are never cached. Every draw reads the ini through st_cfg_int_in and
 * every click writes it straight back, which means the panel cannot drift out
 * of sync with a file someone edited in Notepad while it was open, and there
 * is no "apply" step to get wrong. GetPrivateProfileString goes through the
 * Windows file cache, so reading ~15 keys a frame is cheaper than it sounds --
 * and the panel is only up while the player is looking at it.
 */
#include "osmw_settings.h"

/* ---- section names --------------------------------------------------------
 *
 * TRAP, and it has bitten once already: osmw_fightspeed reads [Fight], NOT
 * [FightSpeed], even though its [Addons] gate key IS "FightSpeed". Do not
 * "correct" either one to match the other -- both names are already in shipped
 * config.ini files, and changing one silently orphans every existing setting.
 */
#define SEC_CHAT    "Chat"
#define SEC_MINIMAP "Minimap"
#define SEC_FIGHT   "Fight"
#define SEC_WEATHER "Weather"

#define DLL_CHAT    "osmw_chat.dll"
#define DLL_MINIMAP "osmw_minimap.dll"
#define DLL_FIGHT   "osmw_fightspeed.dll"
#define DLL_WEATHER "osmw_weather.dll"

#define HEADING(text) \
    {ST_ROW_HEADING, text, 0, 0, 0, ST_APPLY_NOW, {0}, 0}
#define TOGGLE(text, sec, k, def, ap, dll) \
    {ST_ROW_TOGGLE, text, sec, k, def, ap, {0}, dll}

/* The release panel is deliberately narrower than the full config surface.
 * Module gates, event-NPC visibility and diagnostic logging are deployment or
 * support switches, not everyday player preferences, so they remain available
 * in config.ini without being advertised in-game.
 *
 * The visible surface is toggles and small cycles only. Numeric tuning -- SandWind, LeafSway,
 * FogAlpha, font sizes, window geometry -- is deliberately out: most of it is
 * a developer knob, and it needs steppers plus a lot more panel than this.
 */
const st_row_t ST_ROWS[] = {
    HEADING("Chat"),
    TOGGLE("Timestamps",         SEC_CHAT, "Timestamps",        0, ST_APPLY_NOW, DLL_CHAT),
    TOGGLE("Channel tabs",       SEC_CHAT, "TabStrip",          1, ST_APPLY_NOW, DLL_CHAT),
    TOGGLE("Remember active tab",SEC_CHAT, "RememberActiveTab", 1, ST_APPLY_NOW, DLL_CHAT),
    TOGGLE("Highlight my name",  SEC_CHAT, "HighlightMentions", 1, ST_APPLY_NOW, DLL_CHAT),
    TOGGLE("Mouse wheel scroll", SEC_CHAT, "WheelScroll",       1, ST_APPLY_NOW, DLL_CHAT),

    HEADING("Minimap"),
    TOGGLE("Walk to NPC",        SEC_MINIMAP, "AutoWalk",      1, ST_APPLY_NOW, DLL_MINIMAP),

    HEADING("Battle"),
    /* Values are FS_MODE_NORMAL / FAST / OFF in that order, which is what
     * osmw_fightspeed's own enum uses and what its F11 hotkey cycles through.
     * Keep the order: the mod stores the integer, not the name. */
    {ST_ROW_CYCLE, "Animation speed", SEC_FIGHT, "Mode", 0, ST_APPLY_NOW,
     {"Normal", "Fast", "Off", 0}, DLL_FIGHT},
    /* The fight view pans to the acting unit and back once per action; this
     * is what players call combat screen shake, and it is orthogonal to the
     * speed above so Normal+Fixed and Fast+Fixed are both reachable.
     * 0/1/2 are FS_CAM_STOCK / SNAP / FIXED. osmw_fightspeed also accepts the
     * words "stock", "snap" and "fixed" in the file, but this panel reads the
     * key as an integer, so it writes and shows the numbers. */
    {ST_ROW_CYCLE, "Camera motion", SEC_FIGHT, "CameraMotion", 0, ST_APPLY_NOW,
     {"Stock", "Snap", "Fixed", 0}, DLL_FIGHT},

    HEADING("Weather"),
    TOGGLE("Weather icons",      SEC_WEATHER, "Icons", 1, ST_APPLY_RELAUNCH, DLL_WEATHER),
};

const int ST_ROW_COUNT = (int)(sizeof(ST_ROWS) / sizeof(ST_ROWS[0]));

static int choice_count(const st_row_t *row)
{
    int n = 0;
    while (n < ST_MAX_CHOICES && row->choices[n])
        n++;
    return n;
}

int st_row_value(const st_row_t *row)
{
    int v;

    if (!row || row->kind == ST_ROW_HEADING || !row->section || !row->key)
        return 0;

    v = st_cfg_int_in(row->section, row->key, row->fallback);

    /* Clamp rather than trust. The file is user-editable and a cycle index out
     * of range would draw off the end of the choices array; a toggle is
     * normalised so any non-zero reads as on, which is what every mod that
     * reads these keys does with `!= 0`. */
    if (row->kind == ST_ROW_CYCLE) {
        int n = choice_count(row);
        if (n <= 0)
            return 0;
        if (v < 0 || v >= n)
            v = 0;
        return v;
    }
    return v != 0;
}

void st_row_advance(const st_row_t *row)
{
    int v;

    if (!row || row->kind == ST_ROW_HEADING || !row->section || !row->key)
        return;

    v = st_row_value(row);
    if (row->kind == ST_ROW_CYCLE) {
        int n = choice_count(row);
        v = (n > 0) ? (v + 1) % n : 0;
    } else {
        v = !v;
    }

    st_cfg_set_int_in(row->section, row->key, v);
    st_log("set: [%s] %s = %d (%s)", row->section, row->key, v,
           row->applies == ST_APPLY_RELAUNCH ? "next launch" : "live");
}

int st_row_present(const st_row_t *row)
{
    const OSMW_CORE_API_V1 *api;
    OSMW_MODULE_STATUS_V1 status;

    if (!row || !row->dll)
        return 1;
    api = st_core_api();
    if (api == NULL || api->module_status == NULL)
        return 0;

    ZeroMemory(&status, sizeof(status));
    status.struct_size = sizeof(status);
    if (api->module_status(row->dll, &status) != OSMW_OK)
        return 0;

    /* Disabled modules remain installed and can be enabled for the next
     * launch. Failed, missing, incompatible and unloaded modules do not. */
    return status.state == OSMW_MODULE_LOADED ||
           status.state == OSMW_MODULE_ACTIVE ||
           status.state == OSMW_MODULE_DISABLED ||
           status.state == OSMW_MODULE_STOPPING ||
           status.state == OSMW_MODULE_UNSAFE_TO_UNLOAD;
}
