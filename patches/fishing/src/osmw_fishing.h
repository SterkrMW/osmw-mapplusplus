#ifndef OSMW_FISHING_H
#define OSMW_FISHING_H

#include <windows.h>
#include "osmw_core_api.h"
#include "sigs.h"

extern unsigned char *fg_addr[FG_ADDR_COUNT];

const OSMW_CORE_API_V1 *fg_core_api(void);
OSMW_OWNER fg_core_owner(void);

void fg_log(const char *fmt, ...);

int fg_cfg_int(const char *key, int fallback);
int fg_cfg_int_in(const char *section, const char *key, int fallback);

OSMW_RESULT fg_resolve_all(void);
void fg_resolve_reset(void);

typedef struct {
    int left, top, right, bottom;
} fg_rect_t;

int fg_node_rect(int handle, fg_rect_t *out_rect, int *out_flag);
int fg_client_in_world(void);
int fg_panel_install(void);
void fg_panel_remove(void);
void fg_panel_tick(void);
int fg_panel_is_open(void);

#define FG_PACKET_MAP_DATA   0x00030100U
#define FG_PACKET_HELLO      0x7F000040U
#define FG_PACKET_SNAPSHOT   0x7F000041U
#define FG_PACKET_ACTION     0x7F000042U
#define FG_PROTOCOL          1U
#define FG_SNAPSHOT_EXTRA    112U
#define FG_ACTION_EXTRA      12U
#define FG_MESSAGE_BYTES     80
#define FG_SEND_RETRY_MS     500U

#define FG_PHASE_CLOSED      0
#define FG_PHASE_READY       1
#define FG_PHASE_HOOKED      2
#define FG_PHASE_NOBITE      3
#define FG_PHASE_SNAP        4
#define FG_PHASE_ESCAPE      5
#define FG_PHASE_LAND        6
#define FG_PHASE_NOBAIT      7
#define FG_PHASE_SLACK       8

#define FG_HINT_NONE         0
#define FG_HINT_LEFT         1
#define FG_HINT_RIGHT        2

#define FG_CMD_CAST          1U
#define FG_CMD_REEL_LEFT     2U
#define FG_CMD_REEL_RIGHT    3U
#define FG_CMD_HOLD          4U
#define FG_CMD_SLACK         5U
#define FG_CMD_PACK_UP       6U

#define FG_FLAG_CAN_CAST     1
#define FG_FLAG_OVER_CAP     2
#define FG_FLAG_NEW_LOG      4

typedef struct {
    int valid;
    unsigned int phase;
    unsigned int hint;
    unsigned int progress;
    unsigned int tension;
    unsigned int session;
    unsigned int revision;
    unsigned int rounds_remaining;
    unsigned int max_rounds;
    int species_index;
    unsigned int flags;
    unsigned int bait_count;
    unsigned int fishing_level;
    unsigned int fishing_xp;
    unsigned int next_level_xp;
    unsigned int collection_mask;
    char message[FG_MESSAGE_BYTES];
} fg_view_t;

int fg_fishing_install(void);
void fg_fishing_reset(void);
void fg_fishing_on_map_data(void);
void fg_fishing_accept_snapshot(unsigned int protocol, const void *extra,
                                unsigned int extra_size);
void fg_fishing_tick(void);
void fg_fishing_view(fg_view_t *out);
int fg_fishing_want_open(void);
int fg_fishing_input_locked(void);
void fg_fishing_lock_input(void);
int fg_fishing_queue_action(unsigned int command);

#endif /* OSMW_FISHING_H */
