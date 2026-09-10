#ifndef OSMW_INVENTORY_H
#define OSMW_INVENTORY_H

#include <windows.h>
#include "osmw_core_api.h"
#include "sigs.h"

extern unsigned char *iv_addr[IV_ADDR_COUNT];

const OSMW_CORE_API_V1 *iv_core_api(void);
OSMW_OWNER iv_core_owner(void);
void iv_log(const char *format, ...);

OSMW_RESULT iv_resolve_all(void);
void iv_resolve_reset(void);

typedef struct {
    int left, top, right, bottom;
} iv_rect_t;

int iv_node_rect(int handle, iv_rect_t *rect, int *flag);
int iv_node_set_rect(int handle, const iv_rect_t *rect, int flag);
int iv_client_in_world(void);
int iv_panel_install(void);
void iv_panel_remove(void);
void iv_panel_tick(void);
/* Sprite mode paints from the post-UiTick callback, after the manager has
 * walked and drawn every stock window, so the buttons cannot be buried by
 * the inventory window being raised. Call it after iv_panel_tick. */
void iv_panel_overlay(void);

#define IV_PACKET_INVENTORY_DATA          0x00070001U
#define IV_PACKET_SORT_PREPARE            0x7F000010U
#define IV_PACKET_SORT_SNAPSHOT           0x7F000011U
#define IV_PACKET_SORT_APPLY              0x7F000012U
#define IV_PACKET_LOCK_TOGGLE             0x7F000020U
#define IV_LOCK_PROTOCOL                  1U
#define IV_SORT_PROTOCOL                  2U
#define IV_MAX_ITEMS                     96U
/* One sort per player per 30s, overridable with [Inventory]
 * SortCooldownSeconds. The server rate limiter is the authority (see
 * ActionRateLimiter's InventorySort row); this only stops the button offering
 * what would be refused, and a tampered client still gets nothing. */
#define IV_SORT_COOLDOWN_SECONDS_DEFAULT 30
#define IV_SORT_COOLDOWN_SECONDS_MAX   3600

int iv_sort_install(void);
void iv_sort_reset(void);
void iv_sort_request(void);
void iv_sort_accept_snapshot(unsigned int protocol, const void *extra,
                             unsigned int extra_size);
void iv_sort_on_inventory_data(void);
void iv_sort_tick(void);
int iv_sort_busy(void);

/* ---- item lock ----------------------------------------------------------
 * The Lock button toggles the lock on the slot the client already treats as
 * active, the same way Sort acts on the whole inventory. Nothing covers the
 * grid: a node over it would take the hover events too, and the stock tooltip
 * is where the padlock is drawn.
 */
/* Global slot (page * 24 + active cell) the button would act on. */
unsigned int iv_lock_active_slot(void);
void iv_lock_request(unsigned int slot);
void iv_lock_request_active(void);
/* Milliseconds left before another sort is allowed, 0 when ready. */
unsigned int iv_sort_cooldown_ms(void);

#endif /* OSMW_INVENTORY_H */
