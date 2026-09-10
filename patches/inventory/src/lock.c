/* Per-slot item lock.
 *
 * The module's whole job is to name a slot. The server owns the flag and
 * answers a toggle with the ordinary authoritative ItemChange, which the stock
 * client already knows how to render: byte +20 of an item record drives the
 * padlock in the tooltip and the client's own refusal on drop, trade and give.
 * So there is no local lock state, no optimistic update and nothing to draw.
 *
 * The button acts on the slot the client itself treats as active - the one its
 * inventory click handler publishes at InvSlotIndex - exactly the way Sort acts
 * on the whole inventory. It deliberately does NOT intercept grid clicks:
 * anything covering the grid also takes the hover events, which kills the stock
 * tooltip, which is the only place the padlock is drawn.
 */
#include "osmw_inventory.h"

/* The visible page holds 24 cells; a wire slot is global across the pages. */
#define IV_LOCK_PAGE_SLOTS 24U
#define IV_LOCK_MAX_PAGE 3U

/* The active cell, or -1 when the client has not published a usable one. */
static int active_cell(void)
{
    int cell;

    if (!iv_addr[IV_INVSLOTINDEX])
        return -1;
    cell = *(int *)iv_addr[IV_INVSLOTINDEX];
    if (cell < 0 || cell >= (int)IV_LOCK_PAGE_SLOTS)
        return -1;
    return cell;
}

static unsigned int current_page(void)
{
    unsigned int page;

    if (!iv_addr[IV_INVPAGE])
        return 0U;
    page = (unsigned int)*(int *)iv_addr[IV_INVPAGE];
    return page > IV_LOCK_MAX_PAGE ? 0U : page;
}

unsigned int iv_lock_active_slot(void)
{
    int cell = active_cell();

    /* Nothing selected yet reads as the first slot, which is what the stock
     * inventory shows as active when it opens. */
    if (cell < 0)
        cell = 0;
    return current_page() * IV_LOCK_PAGE_SLOTS + (unsigned int)cell;
}

void iv_lock_request(unsigned int slot)
{
    const OSMW_CORE_API_V1 *api = iv_core_api();
    unsigned char payload[4];
    OSMW_RESULT result;

    if (!api || !api->queue_game_packet || !iv_client_in_world())
        return;
    if (slot >= IV_MAX_ITEMS)
        return;

    /* field12 carries the protocol version and the extra carries the slot,
     * which puts the version at offset 12 and the slot at 16 - the 20-byte
     * layout InventoryLockPacketHandler expects. */
    memcpy(payload, &slot, sizeof(payload));
    result = api->queue_game_packet(iv_core_owner(), IV_PACKET_LOCK_TOGGLE,
                                    IV_LOCK_PROTOCOL, payload,
                                    sizeof(payload));
    if (result == OSMW_OK) {
        iv_log("lock: toggle requested for slot %u", slot);
    } else {
        /* Dropped rather than retried: a toggle is not idempotent, and the
         * player can see the result and click again. */
        iv_log("lock: toggle for slot %u not queued (%ld)", slot,
               (long)result);
    }
}

void iv_lock_request_active(void)
{
    iv_lock_request(iv_lock_active_slot());
}
