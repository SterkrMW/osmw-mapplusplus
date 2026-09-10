/* Per-tab chat backgrounds.
 *
 * The stock chat WndProc cycles one global background on every
 * WM_LBUTTONDBLCLK. We keep using that loader -- including its 32x32, 24-bit
 * BMP validation and tiling -- but choose the wanted slot before calling it
 * and persist that slot against the active tab name.
 *
 * No hook is needed. window.c already owns the one chat-window subclass, and
 * tabs.c tells us when its active identity changes.
 */
#include "osmw_chat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windowsx.h>

#define BACKGROUND_COUNT 5
#define CONFIG_MAX       256
#define MAPPING_MAX      14
#define TAB_NAME_MAX     16

/* Keep the gesture away from the stock channel/scroll chrome. Composed text
 * rows are excluded separately through links.c's exact DrawLines walk. */
#define LEFT_GUARD_W   40
#define RIGHT_GUARD_W  24
#define BOTTOM_GUARD_H 24

typedef HRESULT (__fastcall *background_cycle_fn)(void *self, void *edx);

typedef struct {
    char name[TAB_NAME_MAX];
    int  background;
} background_mapping_t;

static background_cycle_fn g_cycle;
static int                 *g_index;
static OSMW_TOKEN           g_index_claim = OSMW_INVALID_TOKEN;
static background_mapping_t g_mappings[MAPPING_MAX];
static int                  g_mapping_count;
static int                  g_startup_index;
static unsigned             g_failed_mask;
static char                 g_config_spec[CONFIG_MAX];
static int                  g_config_loaded;
static int                  g_installed;

static int mapping_index(const char *name)
{
    int i;
    if (!name || !name[0])
        return -1;
    for (i = 0; i < g_mapping_count; i++) {
        if (!_stricmp(g_mappings[i].name, name))
            return i;
    }
    return -1;
}

static int wanted_for(const char *name)
{
    int index = mapping_index(name);
    return index >= 0 ? g_mappings[index].background : 0;
}

static void parse_mappings(const char *spec)
{
    char work[CONFIG_MAX];
    char *part, *next;

    g_mapping_count = 0;
    _snprintf(work, sizeof(work), "%s", spec ? spec : "");
    work[sizeof(work) - 1] = '\0';

    for (part = work; part && *part; part = next) {
        char *colon;
        int slot, existing;
        size_t name_len;

        next = strchr(part, '|');
        if (next)
            *next++ = '\0';
        colon = strrchr(part, ':');
        if (!colon || colon == part || colon[1] < '0' || colon[1] > '4' || colon[2]) {
            mw_log("backgrounds: invalid mapping \"%s\" ignored", part);
            continue;
        }
        *colon = '\0';
        name_len = strlen(part);
        if (name_len == 0 || name_len >= TAB_NAME_MAX) {
            mw_log("backgrounds: invalid tab name \"%s\" ignored", part);
            continue;
        }

        slot = colon[1] - '0';
        existing = mapping_index(part);
        if (existing >= 0) {
            g_mappings[existing].background = slot;
            continue;
        }
        if (g_mapping_count >= MAPPING_MAX) {
            mw_log("backgrounds: mapping capacity reached; \"%s\" ignored", part);
            continue;
        }
        _snprintf(g_mappings[g_mapping_count].name, TAB_NAME_MAX, "%s", part);
        g_mappings[g_mapping_count].name[TAB_NAME_MAX - 1] = '\0';
        g_mappings[g_mapping_count].background = slot;
        g_mapping_count++;
    }
}

/* The routine increments before loading, so seed it with target-1. It returns
 * E_FAIL for a missing/wrong-format BMP. Restore the old index on every
 * failure; the routine does not replace the old pixels until validation has
 * succeeded. */
static int apply_background(int target, const char *why)
{
    int current, seed;
    HRESULT hr;

    if (!g_installed || !g_cycle || !g_index || target < 0 || target >= BACKGROUND_COUNT)
        return 0;
    current = *g_index;
    if (current < 0 || current >= BACKGROUND_COUNT) {
        mw_log("backgrounds: live index %d is invalid; refusing %s", current, why);
        return 0;
    }
    if (current == target)
        return 1;

    seed = (target + BACKGROUND_COUNT - 1) % BACKGROUND_COUNT;
    if (!mw_write_bytes(g_index, &seed, sizeof(seed))) {
        mw_log("backgrounds: could not seed index %d (%s)", target, why);
        return 0;
    }
    hr = g_cycle(mw_addr[MW_OBJECT], NULL);
    if (FAILED(hr) || *g_index != target) {
        (void)mw_write_bytes(g_index, &current, sizeof(current));
        if (!(g_failed_mask & (1u << target))) {
            g_failed_mask |= 1u << target;
            mw_log("backgrounds: slot %d failed to load (HRESULT 0x%08lX); "
                   "kept slot %d", target, (unsigned long)hr, current);
        }
        return 0;
    }

    mw_log("backgrounds: slot %d applied for \"%s\" (%s)", target,
           mw_tabs_active_name(), why);
    return 1;
}

static void serialize_mappings(char *output, size_t size)
{
    size_t used = 0;
    int i;

    if (!output || size == 0)
        return;
    output[0] = '\0';
    for (i = 0; i < g_mapping_count; i++) {
        int wrote = _snprintf(output + used, size - used, "%s%s:%d",
                              used ? "|" : "", g_mappings[i].name,
                              g_mappings[i].background);
        if (wrote < 0 || (size_t)wrote >= size - used) {
            output[used] = '\0';
            mw_log("backgrounds: persisted mapping reached the config limit");
            return;
        }
        used += (size_t)wrote;
    }
}

static void remember_active(int background)
{
    const char *name = mw_tabs_active_name();
    int index = mapping_index(name);
    char spec[CONFIG_MAX];

    if (!name[0])
        return;
    if (index < 0) {
        if (g_mapping_count >= MAPPING_MAX) {
            /* Stale names are optional. Preserve the current tab by replacing
             * the oldest mapping when the bounded value is full. */
            memmove(&g_mappings[0], &g_mappings[1],
                    sizeof(g_mappings[0]) * (MAPPING_MAX - 1));
            g_mapping_count = MAPPING_MAX - 1;
        }
        index = g_mapping_count++;
        _snprintf(g_mappings[index].name, TAB_NAME_MAX, "%s", name);
        g_mappings[index].name[TAB_NAME_MAX - 1] = '\0';
    }
    g_mappings[index].background = background;

    serialize_mappings(spec, sizeof(spec));
    _snprintf(g_config_spec, sizeof(g_config_spec), "%s", spec);
    g_config_spec[sizeof(g_config_spec) - 1] = '\0';
    g_config_loaded = 1;
    mw_cfg_set_str("TabBackgrounds", spec);
}

void mw_backgrounds_poll(void)
{
    char spec[CONFIG_MAX];

    if (!g_installed)
        return;
    (void)mw_cfg_copy("TabBackgrounds", "", spec, sizeof(spec));
    if (g_config_loaded && !strcmp(spec, g_config_spec))
        return;

    _snprintf(g_config_spec, sizeof(g_config_spec), "%s", spec);
    g_config_spec[sizeof(g_config_spec) - 1] = '\0';
    g_config_loaded = 1;
    g_failed_mask = 0;             /* an external edit may accompany fixed assets */
    parse_mappings(g_config_spec);
    (void)apply_background(wanted_for(mw_tabs_active_name()), "config refresh");
}

void mw_backgrounds_on_tab_changed(void)
{
    if (g_installed)
        (void)apply_background(wanted_for(mw_tabs_active_name()), "tab changed");
}

static void cycle_active(void)
{
    int current, step;

    if (!g_index)
        return;
    current = *g_index;
    if (current < 0 || current >= BACKGROUND_COUNT)
        return;

    for (step = 1; step < BACKGROUND_COUNT; step++) {
        int target = (current + step) % BACKGROUND_COUNT;
        if (apply_background(target, "background double-click")) {
            remember_active(target);
            return;
        }
    }
}

int mw_backgrounds_on_message(HWND hwnd, UINT msg, WPARAM wp,
                              LPARAM lp, LRESULT *result)
{
    RECT client;
    int x, y;

    (void)wp;
    if (msg != WM_LBUTTONDBLCLK || !g_installed)
        return 0;

    x = GET_X_LPARAM(lp);
    y = GET_Y_LPARAM(lp);
    if (!GetClientRect(hwnd, &client) || x < LEFT_GUARD_W ||
            x >= client.right - RIGHT_GUARD_W || y < 0 ||
            y >= client.bottom - BOTTOM_GUARD_H ||
            mw_tabs_point_is_strip(y) || mw_links_line_occupied(y)) {
        *result = 0;
        return 1;                   /* do not let stock perform a global cycle */
    }

    cycle_active();
    *result = 0;
    return 1;
}

int mw_backgrounds_install(void)
{
    OSMW_RESULT result;
    int live;

    if (g_installed)
        return 1;
    if (!mw_addr[MW_BACKGROUNDCYCLE] || !mw_addr[MW_BACKGROUNDINDEX] ||
            !mw_addr[MW_OBJECT]) {
        mw_log("backgrounds: stock cycle/index unavailable; preserving global stock mode");
        return 0;
    }

    g_cycle = (background_cycle_fn)(UINT_PTR)mw_addr[MW_BACKGROUNDCYCLE];
    g_index = (int *)mw_addr[MW_BACKGROUNDINDEX];
    live = *g_index;
    if (live < 0 || live >= BACKGROUND_COUNT) {
        mw_log("backgrounds: startup index %d is invalid; not installing", live);
        g_cycle = NULL;
        g_index = NULL;
        return 0;
    }

    result = mw_claim_region(g_index, sizeof(*g_index), OSMW_REGION_DATA,
                             "chat background index", &g_index_claim);
    if (result != OSMW_OK) {
        mw_log("backgrounds: index claim refused (%ld)", (long)result);
        g_cycle = NULL;
        g_index = NULL;
        return 0;
    }

    g_startup_index = live;
    g_failed_mask = 0;
    g_config_loaded = 0;
    g_installed = 1;
    mw_backgrounds_poll();
    mw_log("backgrounds: installed (startup slot %d, active \"%s\")",
           g_startup_index, mw_tabs_active_name());
    return 1;
}

OSMW_RESULT mw_backgrounds_remove(void)
{
    OSMW_RESULT result;

    if (!g_installed)
        return OSMW_OK;

    /* Restore through the stock loader so both the index and tiled backing
     * pixels match startup before ownership is released. */
    if (!apply_background(g_startup_index, "shutdown restore"))
        (void)mw_write_bytes(g_index, &g_startup_index, sizeof(g_startup_index));
    result = mw_release_region(&g_index_claim);
    if (result != OSMW_OK)
        return result;

    g_installed = 0;
    g_cycle = NULL;
    g_index = NULL;
    g_mapping_count = 0;
    g_config_loaded = 0;
    mw_log("backgrounds: removed");
    return OSMW_OK;
}
