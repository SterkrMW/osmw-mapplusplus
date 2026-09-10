#include "osmw_fishing.h"

#include <string.h>

static fg_view_t g_view;
static int g_was_in_world;
static int g_have_sent;
static int g_hello_pending;
static int g_input_locked;
static unsigned int g_locked_revision;
static DWORD g_last_send_tick;

static unsigned int read_u32(const unsigned char *bytes, unsigned int offset)
{
    unsigned int value;
    memcpy(&value, bytes + offset, sizeof(value));
    return value;
}

static unsigned int read_u16(const unsigned char *bytes, unsigned int offset)
{
    unsigned short value;
    memcpy(&value, bytes + offset, sizeof(value));
    return value;
}

static void copy_message(char *dest, const unsigned char *src)
{
    unsigned int index;
    for (index = 0; index < FG_MESSAGE_BYTES - 1; ++index) {
        char ch = (char)src[index];
        if (ch == '\0')
            break;
        dest[index] = ch;
    }
    dest[index] = '\0';
}

static void queue_hello(void)
{
    g_hello_pending = 1;
}

int fg_fishing_install(void)
{
    fg_fishing_reset();
    fg_log("fishing: protocol v%u ready through core send queue", FG_PROTOCOL);
    return 1;
}

void fg_fishing_reset(void)
{
    ZeroMemory(&g_view, sizeof(g_view));
    g_was_in_world = 0;
    g_have_sent = 0;
    g_hello_pending = 0;
    g_input_locked = 0;
    g_locked_revision = 0;
    g_last_send_tick = 0;
}

void fg_fishing_on_map_data(void)
{
    ZeroMemory(&g_view, sizeof(g_view));
    g_input_locked = 0;
    g_have_sent = 0;
    queue_hello();
}

void fg_fishing_accept_snapshot(unsigned int protocol, const void *extra,
                                unsigned int extra_size)
{
    const unsigned char *bytes;
    unsigned int revision;
    fg_view_t view;

    if (protocol != FG_PROTOCOL || extra == NULL ||
            extra_size != FG_SNAPSHOT_EXTRA) {
        fg_log("fishing: ignored malformed snapshot (v%u, extra %p, size %u)",
               protocol, extra, extra_size);
        return;
    }

    bytes = (const unsigned char *)extra;
    ZeroMemory(&view, sizeof(view));
    view.phase = bytes[0];
    view.hint = bytes[1];
    view.progress = bytes[2];
    view.tension = bytes[3];
    view.session = read_u32(bytes, 4);
    view.revision = read_u32(bytes, 8);
    view.rounds_remaining = bytes[12];
    view.max_rounds = bytes[13];
    view.species_index = (signed char)bytes[14];
    view.flags = bytes[15];
    view.bait_count = read_u16(bytes, 16);
    view.fishing_level = bytes[18];
    view.fishing_xp = read_u32(bytes, 20);
    view.next_level_xp = read_u32(bytes, 24);
    view.collection_mask = read_u32(bytes, 28);
    copy_message(view.message, bytes + 32);

    if (view.phase > FG_PHASE_SLACK || view.hint > FG_HINT_RIGHT ||
            view.progress > 100 || view.tension > 100) {
        fg_log("fishing: ignored out-of-range snapshot (phase %u hint %u)",
               view.phase, view.hint);
        return;
    }

    revision = view.revision;
    view.valid = 1;
    g_view = view;
    if (revision != g_locked_revision)
        g_input_locked = 0;
    fg_log("fishing: snapshot phase=%u rev=%u session=%u",
           view.phase, view.revision, view.session);
}

void fg_fishing_view(fg_view_t *out)
{
    if (!out)
        return;
    *out = g_view;
}

int fg_fishing_want_open(void)
{
    return g_view.valid && g_view.phase != FG_PHASE_CLOSED;
}

int fg_fishing_input_locked(void)
{
    return g_input_locked;
}

void fg_fishing_lock_input(void)
{
    g_input_locked = 1;
    g_locked_revision = g_view.revision;
}

int fg_fishing_queue_action(unsigned int command)
{
    const OSMW_CORE_API_V1 *api = fg_core_api();
    unsigned char extra[FG_ACTION_EXTRA];
    OSMW_RESULT result;

    if (!api || !api->queue_game_packet || !g_view.valid ||
            g_view.session == 0 || g_input_locked)
        return 0;

    memcpy(extra, &command, 4);
    memcpy(extra + 4, &g_view.session, 4);
    memcpy(extra + 8, &g_view.revision, 4);
    result = api->queue_game_packet(
        fg_core_owner(), FG_PACKET_ACTION, FG_PROTOCOL, extra, FG_ACTION_EXTRA);
    if (result != OSMW_OK) {
        fg_log("fishing: action queue failed (%ld)", (long)result);
        return 0;
    }
    fg_fishing_lock_input();
    return 1;
}

void fg_fishing_tick(void)
{
    const OSMW_CORE_API_V1 *api = fg_core_api();
    OSMW_RESULT result;
    DWORD now;
    int in_world = fg_client_in_world();

    if (!in_world) {
        if (g_was_in_world)
            fg_fishing_reset();
        g_was_in_world = 0;
        return;
    }

    if (!g_was_in_world) {
        g_was_in_world = 1;
        g_have_sent = 0;
        queue_hello();
    }

    if (!g_hello_pending && g_have_sent)
        return;
    if (!api || !api->queue_game_packet)
        return;

    now = GetTickCount();
    if (g_have_sent && (LONG)(now - g_last_send_tick) < (LONG)FG_SEND_RETRY_MS)
        return;

    result = api->queue_game_packet(
        fg_core_owner(), FG_PACKET_HELLO, FG_PROTOCOL, NULL, 0);
    g_last_send_tick = now;
    if (result == OSMW_OK) {
        g_hello_pending = 0;
        g_have_sent = 1;
        return;
    }
    g_hello_pending = 1;
    g_have_sent = 1;
}
