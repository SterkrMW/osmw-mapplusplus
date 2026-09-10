#include "osmw_timers.h"

#include <string.h>

/* Protocol-v1 bounds. A level-5/special SSP lasts at most 16 hours and the
 * duration ring can double it. Rejecting larger values keeps wrap-safe DWORD
 * deadline arithmetic inside its documented half-range. */
#define TM_MAX_DEVIL_TEARS_SECONDS 1800U
#define TM_MAX_SHAPESHIFT_SECONDS  115200U
/* The stock EXP potions run an hour. The ceiling is generous for event durations
 * and shares the shapeshift bound, which is what keeps the wrap-safe DWORD
 * deadline arithmetic inside its documented half-range. */
#define TM_MAX_EXP_SECONDS         115200U
#define TM_MAX_EXP_MULTIPLIER      99U
#define TM_SEND_RETRY_MS           500U

static volatile LONG g_synced;
static volatile LONG g_devil_tears_active;
static volatile LONG g_shapeshift_active;
static volatile LONG g_devil_tears_deadline;
static volatile LONG g_shapeshift_deadline;
static volatile LONG g_exp_active;
static volatile LONG g_exp_deadline;
static volatile LONG g_exp_multiplier;
static volatile LONG g_refresh_pending;

/* Core tick and packet callbacks run on the client thread. Interlocked stores
 * make publication order explicit; multi-field consistency still deliberately
 * relies on that single-threaded dispatch contract. */
static int g_was_in_world;
static int g_have_sent;
static DWORD g_last_send_tick;

static unsigned int seconds_until(DWORD deadline, DWORD now)
{
    LONG remaining_ms = (LONG)(deadline - now);
    if (remaining_ms <= 0)
        return 0;
    return ((unsigned int)remaining_ms + 999U) / 1000U;
}

static void queue_refresh(void)
{
    InterlockedExchange(&g_refresh_pending, 1);
}

int tm_timers_install(void)
{
    tm_timers_reset();
    tm_log("timers: protocol v%u ready through core send queue",
           TM_TIMER_PROTOCOL);
    return 1;
}

void tm_timers_reset(void)
{
    InterlockedExchange(&g_synced, 0);
    InterlockedExchange(&g_devil_tears_active, 0);
    InterlockedExchange(&g_shapeshift_active, 0);
    InterlockedExchange(&g_devil_tears_deadline, 0);
    InterlockedExchange(&g_shapeshift_deadline, 0);
    InterlockedExchange(&g_exp_active, 0);
    InterlockedExchange(&g_exp_deadline, 0);
    InterlockedExchange(&g_exp_multiplier, 0);
    InterlockedExchange(&g_refresh_pending, 0);
    g_was_in_world = 0;
    g_have_sent = 0;
    g_last_send_tick = 0;
}

void tm_timers_on_map_data(void)
{
    /* A complete replacement is cheap and prevents a previous character's
     * state leaking through a fast character switch. Ordinary map changes
     * display Synchronising for at most one round trip. */
    InterlockedExchange(&g_synced, 0);
    InterlockedExchange(&g_devil_tears_active, 0);
    InterlockedExchange(&g_shapeshift_active, 0);
    InterlockedExchange(&g_exp_active, 0);
    queue_refresh();
}

void tm_timers_accept_snapshot(unsigned int protocol, const void *extra,
                               unsigned int extra_size)
{
    unsigned int devil_tears;
    unsigned int shapeshift;
    unsigned int exp_seconds = 0;
    unsigned int exp_multiplier = 0;
    DWORD now;

    /* v2 added the EXP pair. The server answers in whatever version we announced,
     * so a mismatch here is a malformed packet rather than an old server. */
    if (protocol != TM_TIMER_PROTOCOL || !extra || extra_size != 16U) {
        tm_log("timers: ignored malformed snapshot (v%u, extra %p, size %u)",
               protocol, extra, extra_size);
        return;
    }

    memcpy(&devil_tears, extra, sizeof(devil_tears));
    memcpy(&shapeshift, (const unsigned char *)extra + 4, sizeof(shapeshift));
    memcpy(&exp_seconds, (const unsigned char *)extra + 8, sizeof(exp_seconds));
    memcpy(&exp_multiplier, (const unsigned char *)extra + 12,
           sizeof(exp_multiplier));
    if (devil_tears > TM_MAX_DEVIL_TEARS_SECONDS ||
        shapeshift > TM_MAX_SHAPESHIFT_SECONDS ||
        exp_seconds > TM_MAX_EXP_SECONDS ||
        exp_multiplier > TM_MAX_EXP_MULTIPLIER) {
        tm_log("timers: ignored out-of-range snapshot (%u, %u, %u x%u)",
               devil_tears, shapeshift, exp_seconds, exp_multiplier);
        return;
    }

    /* A duration without a multiplier has no sprite to draw and a multiplier
     * without a duration has nothing to count down, so require both or neither. */
    if ((exp_seconds != 0U) != (exp_multiplier > 1U)) {
        tm_log("timers: ignored inconsistent EXP pair (%u s, x%u)",
               exp_seconds, exp_multiplier);
        return;
    }

    now = GetTickCount();
    InterlockedExchange(&g_synced, 0);
    InterlockedExchange(&g_devil_tears_deadline,
                        (LONG)(now + devil_tears * 1000U));
    InterlockedExchange(&g_shapeshift_deadline,
                        (LONG)(now + shapeshift * 1000U));
    InterlockedExchange(&g_exp_deadline, (LONG)(now + exp_seconds * 1000U));
    InterlockedExchange(&g_exp_multiplier, (LONG)exp_multiplier);
    InterlockedExchange(&g_devil_tears_active, devil_tears != 0U);
    InterlockedExchange(&g_shapeshift_active, shapeshift != 0U);
    InterlockedExchange(&g_exp_active, exp_seconds != 0U);
    InterlockedExchange(&g_refresh_pending, 0);
    InterlockedExchange(&g_synced, 1);

    tm_log("timers: snapshot Devil's Tears=%u s, Shapeshift=%u s, EXP=%u s x%u",
           devil_tears, shapeshift, exp_seconds, exp_multiplier);
}

void tm_timers_tick(void)
{
    const OSMW_CORE_API_V1 *api;
    OSMW_RESULT result;
    DWORD now;
    int in_world = tm_client_in_world();

    if (!in_world) {
        if (g_was_in_world)
            tm_timers_reset();
        g_was_in_world = 0;
        return;
    }

    if (!g_was_in_world) {
        g_was_in_world = 1;
        InterlockedExchange(&g_synced, 0);
        queue_refresh();
    }

    now = GetTickCount();
    if (g_synced) {
        if (g_devil_tears_active &&
            seconds_until((DWORD)g_devil_tears_deadline, now) == 0U) {
            InterlockedExchange(&g_devil_tears_active, 0);
            queue_refresh();
        }
        if (g_shapeshift_active &&
            seconds_until((DWORD)g_shapeshift_deadline, now) == 0U) {
            InterlockedExchange(&g_shapeshift_active, 0);
            queue_refresh();
        }
        if (g_exp_active && seconds_until((DWORD)g_exp_deadline, now) == 0U) {
            InterlockedExchange(&g_exp_active, 0);
            queue_refresh();
        }
    }

    if (!g_refresh_pending)
        return;
    if (g_have_sent && (DWORD)(now - g_last_send_tick) < TM_SEND_RETRY_MS)
        return;

    api = tm_core_api();
    if (api == NULL || api->queue_game_packet == NULL)
        return;
    g_have_sent = 1;
    g_last_send_tick = now;
    result = api->queue_game_packet(
        tm_core_owner(), TM_PACKET_TIMER_HELLO, TM_TIMER_PROTOCOL, NULL, 0);
    if (result == OSMW_OK) {
        InterlockedExchange(&g_refresh_pending, 0);
    } else {
        tm_log("timers: hello queue failed (%ld); retry remains pending",
               (long)result);
    }
}

void tm_timers_view(tm_timer_view_t *out)
{
    DWORD now;

    if (!out)
        return;
    ZeroMemory(out, sizeof(*out));
    if (!g_synced)
        return;

    now = GetTickCount();
    out->synced = 1;
    if (g_devil_tears_active)
        out->devil_tears_seconds =
            seconds_until((DWORD)g_devil_tears_deadline, now);
    if (g_shapeshift_active)
        out->shapeshift_seconds =
            seconds_until((DWORD)g_shapeshift_deadline, now);
    if (g_exp_active) {
        out->exp_seconds = seconds_until((DWORD)g_exp_deadline, now);
        out->exp_multiplier = (unsigned int)g_exp_multiplier;
    }
}
