/* Inventory sorting protocol state. The client computes a permutation from a
 * server-issued metadata snapshot; it never edits the client's inventory
 * memory. The server validates the one-time proposal and answers with the
 * ordinary authoritative InventoryData packet on either acceptance or reject.
 */
#include "osmw_inventory.h"

#include <stdlib.h>
#include <string.h>

#define IV_SEND_RETRY_MS 500U
#define IV_OPERATION_TIMEOUT_MS 5000U

typedef enum {
    IV_SORT_IDLE,
    IV_SORT_PREPARE_PENDING,
    IV_SORT_WAIT_SNAPSHOT,
    IV_SORT_APPLY_PENDING,
    IV_SORT_WAIT_RESULT
} iv_sort_state_t;

typedef struct {
    unsigned char source_slot;
    unsigned char category;
    unsigned char locked;
    unsigned int base_id;
    unsigned int count;
} iv_sort_entry_t;

static iv_sort_state_t g_state;
static iv_sort_entry_t g_entries[IV_MAX_ITEMS];
static unsigned int g_entry_count;
static unsigned int g_nonce;
static DWORD g_started_at;
static DWORD g_last_send_at;
static int g_have_sent;
/* Deliberately NOT cleared by iv_sort_reset: the whole point of the
 * cooldown is to survive the operation completing, timing out, or being
 * cancelled. iv_sort_install clears it. */
static DWORD g_cooldown_until;
static DWORD g_cooldown_len;

static int __cdecl compare_entries(const void *left, const void *right)
{
    const iv_sort_entry_t *a = (const iv_sort_entry_t *)left;
    const iv_sort_entry_t *b = (const iv_sort_entry_t *)right;

    if (a->category != b->category)
        return a->category < b->category ? -1 : 1;
    if (a->base_id != b->base_id)
        return a->base_id < b->base_id ? -1 : 1;
    if (a->count != b->count)
        return a->count > b->count ? -1 : 1;
    if (a->source_slot != b->source_slot)
        return a->source_slot < b->source_slot ? -1 : 1;
    return 0;
}

/* Mirror of the server's buildCanonicalPlacement (see
 * src/game-server/Services/Inventory/InventorySortAuthority.ts).
 *
 * A locked entry keeps the slot it is already in; everything else is sorted
 * canonically and packed into the lowest slots the pinned entries left free.
 * The result is ordered by destination, which is the order the apply packet
 * lists source slots in. The server recomputes this and rejects the apply
 * outright if it disagrees, so the two must stay identical.
 */
static void build_canonical_placement(unsigned int count)
{
    iv_sort_entry_t free_entries[IV_MAX_ITEMS];
    iv_sort_entry_t ordered[IV_MAX_ITEMS];
    unsigned char pinned[IV_MAX_ITEMS];
    unsigned int destination[IV_MAX_ITEMS];
    unsigned int free_count = 0;
    unsigned int index, next, slot;

    ZeroMemory(pinned, sizeof(pinned));
    for (index = 0; index < count; ++index) {
        if (g_entries[index].locked)
            pinned[g_entries[index].source_slot] = 1;
        else
            free_entries[free_count++] = g_entries[index];
    }

    if (free_count > 1)
        qsort(free_entries, free_count, sizeof(free_entries[0]),
              compare_entries);

    /* Destination for every entry: its own slot when pinned, otherwise the
     * next slot no pinned entry claimed. */
    next = 0;
    free_count = 0;
    for (index = 0; index < count; ++index) {
        if (g_entries[index].locked)
            continue;
        ++free_count;
    }
    for (index = 0; index < free_count; ++index) {
        while (next < IV_MAX_ITEMS && pinned[next])
            ++next;
        destination[index] = next;
        ++next;
    }

    /* Emit by ascending destination. Slot numbers are below IV_MAX_ITEMS, so a
     * simple sweep is both correct and cheaper than another sort. */
    {
        unsigned int emitted = 0;
        unsigned int free_index = 0;

        for (slot = 0; slot < IV_MAX_ITEMS && emitted < count; ++slot) {
            if (pinned[slot]) {
                for (index = 0; index < count; ++index) {
                    if (g_entries[index].locked &&
                            g_entries[index].source_slot == slot) {
                        ordered[emitted++] = g_entries[index];
                        break;
                    }
                }
                continue;
            }
            if (free_index < free_count && destination[free_index] == slot)
                ordered[emitted++] = free_entries[free_index++];
        }

        for (index = 0; index < emitted; ++index)
            g_entries[index] = ordered[index];
    }
}

int iv_sort_install(void)
{
    const OSMW_CORE_API_V1 *api = iv_core_api();
    int seconds = IV_SORT_COOLDOWN_SECONDS_DEFAULT;

    iv_sort_reset();
    g_cooldown_until = 0;
    if (api && api->config_read_int)
        seconds = api->config_read_int(iv_core_owner(), "Inventory",
                                       "SortCooldownSeconds",
                                       IV_SORT_COOLDOWN_SECONDS_DEFAULT);
    if (seconds < 0)
        seconds = 0;
    if (seconds > IV_SORT_COOLDOWN_SECONDS_MAX)
        seconds = IV_SORT_COOLDOWN_SECONDS_MAX;
    g_cooldown_len = (DWORD)seconds * 1000U;
    iv_log("sort: cooldown %ds", seconds);
    iv_log("sort: protocol v%u ready; server remains authoritative",
           IV_SORT_PROTOCOL);
    return 1;
}

void iv_sort_reset(void)
{
    g_state = IV_SORT_IDLE;
    g_entry_count = 0;
    g_nonce = 0;
    g_started_at = 0;
    g_last_send_at = 0;
    g_have_sent = 0;
    ZeroMemory(g_entries, sizeof(g_entries));
}

int iv_sort_busy(void)
{
    return g_state != IV_SORT_IDLE;
}

unsigned int iv_sort_cooldown_ms(void)
{
    DWORD now = GetTickCount();

    if (g_cooldown_until == 0U || g_cooldown_len == 0U)
        return 0U;
    /* Unsigned wrap makes this correct across the 49-day GetTickCount roll:
     * once now has passed the deadline the difference goes negative and its
     * unsigned form exceeds the cooldown length. */
    if ((DWORD)(g_cooldown_until - now) > g_cooldown_len)
        return 0U;
    return (unsigned int)(g_cooldown_until - now);
}

void iv_sort_request(void)
{
    unsigned int remaining;

    if (g_state != IV_SORT_IDLE || !iv_client_in_world())
        return;
    remaining = iv_sort_cooldown_ms();
    if (remaining > 0U) {
        iv_log("sort: refused, %us of cooldown left",
               (remaining + 999U) / 1000U);
        return;
    }
    /* The cooldown runs from the request, not from completion, so a slow or
     * lost round trip cannot extend it. */
    if (g_cooldown_len > 0U)
        g_cooldown_until = GetTickCount() + g_cooldown_len;
    g_state = IV_SORT_PREPARE_PENDING;
    g_started_at = GetTickCount();
    g_have_sent = 0;
    iv_log("sort: inventory snapshot requested");
}

void iv_sort_accept_snapshot(unsigned int protocol, const void *extra,
                             unsigned int extra_size)
{
    const unsigned char *bytes = (const unsigned char *)extra;
    unsigned char seen[IV_MAX_ITEMS];
    unsigned int count, index;

    if (g_state != IV_SORT_WAIT_SNAPSHOT)
        return;
    if (protocol != IV_SORT_PROTOCOL || !extra || extra_size < 4U ||
            (extra_size - 4U) % 12U != 0U) {
        iv_log("sort: ignored malformed snapshot (v%u, size %u)",
               protocol, extra_size);
        return;
    }
    count = (extra_size - 4U) / 12U;
    if (count > IV_MAX_ITEMS) {
        iv_log("sort: ignored oversized snapshot (%u entries)", count);
        return;
    }

    ZeroMemory(seen, sizeof(seen));
    memcpy(&g_nonce, bytes, sizeof(g_nonce));
    if (g_nonce == 0U) {
        iv_log("sort: ignored zero-nonce snapshot");
        return;
    }
    for (index = 0; index < count; ++index) {
        unsigned int packed, base_id, item_count;
        unsigned int source, category;

        memcpy(&packed, bytes + 4U + index * 12U, sizeof(packed));
        memcpy(&base_id, bytes + 8U + index * 12U, sizeof(base_id));
        memcpy(&item_count, bytes + 12U + index * 12U,
               sizeof(item_count));
        source = packed & 0xFFU;
        category = (packed >> 8) & 0xFFU;
        /* Bit 16 is the lock flag as of protocol 2; everything above it is
         * still reserved and must be zero. */
        if ((packed & 0xFFFE0000U) != 0U || source >= IV_MAX_ITEMS ||
                category > 4U || seen[source]) {
            iv_log("sort: ignored invalid entry %u (packed 0x%08X)",
                   index, packed);
            return;
        }
        seen[source] = 1;
        g_entries[index].source_slot = (unsigned char)source;
        g_entries[index].category = (unsigned char)category;
        g_entries[index].locked = (unsigned char)((packed >> 16) & 1U);
        g_entries[index].base_id = base_id;
        g_entries[index].count = item_count;
    }

    g_entry_count = count;
    build_canonical_placement(count);
    g_state = IV_SORT_APPLY_PENDING;
    g_have_sent = 0;
    iv_log("sort: computed proposal for %u occupied slot%s", count,
           count == 1U ? "" : "s");
}

void iv_sort_on_inventory_data(void)
{
    if (g_state == IV_SORT_IDLE)
        return;
    iv_log("sort: authoritative inventory response received");
    iv_sort_reset();
}

static void queue_prepare(DWORD now)
{
    const OSMW_CORE_API_V1 *api = iv_core_api();
    OSMW_RESULT result;

    if (!api || !api->queue_game_packet)
        return;
    g_have_sent = 1;
    g_last_send_at = now;
    result = api->queue_game_packet(iv_core_owner(), IV_PACKET_SORT_PREPARE,
                                    IV_SORT_PROTOCOL, NULL, 0);
    if (result == OSMW_OK) {
        g_state = IV_SORT_WAIT_SNAPSHOT;
    } else {
        iv_log("sort: prepare queue failed (%ld); retry remains pending",
               (long)result);
    }
}

static void queue_apply(DWORD now)
{
    const OSMW_CORE_API_V1 *api = iv_core_api();
    unsigned char payload[4 + IV_MAX_ITEMS];
    OSMW_RESULT result;
    unsigned int index;

    if (!api || !api->queue_game_packet)
        return;
    memcpy(payload, &g_nonce, sizeof(g_nonce));
    for (index = 0; index < g_entry_count; ++index)
        payload[4U + index] = g_entries[index].source_slot;

    g_have_sent = 1;
    g_last_send_at = now;
    result = api->queue_game_packet(
        iv_core_owner(), IV_PACKET_SORT_APPLY, IV_SORT_PROTOCOL, payload,
        4U + g_entry_count);
    if (result == OSMW_OK) {
        g_state = IV_SORT_WAIT_RESULT;
    } else {
        iv_log("sort: apply queue failed (%ld); retry remains pending",
               (long)result);
    }
}

void iv_sort_tick(void)
{
    DWORD now;

    if (g_state == IV_SORT_IDLE)
        return;
    if (!iv_client_in_world()) {
        iv_log("sort: cancelled on leaving the world");
        iv_sort_reset();
        return;
    }

    now = GetTickCount();
    if ((DWORD)(now - g_started_at) >= IV_OPERATION_TIMEOUT_MS) {
        iv_log("sort: timed out waiting for the server");
        iv_sort_reset();
        return;
    }
    if (g_state != IV_SORT_PREPARE_PENDING &&
            g_state != IV_SORT_APPLY_PENDING)
        return;
    if (g_have_sent && (DWORD)(now - g_last_send_at) < IV_SEND_RETRY_MS)
        return;

    if (g_state == IV_SORT_PREPARE_PENDING)
        queue_prepare(now);
    else
        queue_apply(now);
}
