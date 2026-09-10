#ifndef OSMW_TIMERS_H
#define OSMW_TIMERS_H

#include <windows.h>
#include "osmw_core_api.h"
#include "sigs.h"

extern unsigned char *tm_addr[TM_ADDR_COUNT];

const OSMW_CORE_API_V1 *tm_core_api(void);
OSMW_OWNER tm_core_owner(void);

void tm_log(const char *fmt, ...);

int tm_cfg_int(const char *key, int fallback);
int tm_cfg_int_in(const char *section, const char *key, int fallback);
const char *tm_cfg_str(const char *key, const char *fallback);
int tm_cfg_copy(const char *key, const char *fallback, char *out, int size);
void tm_cfg_set_int(const char *key, int value);
void tm_cfg_set_int_in(const char *section, const char *key, int value);
void tm_cfg_set_str(const char *key, const char *value);

OSMW_RESULT tm_resolve_all(void);
void tm_resolve_reset(void);

int tm_key_scancode_for(const char *name);
void tm_key_init(void);
int tm_key_pressed(int scancode);

typedef struct {
    int left, top, right, bottom;
} tm_rect_t;

int tm_node_rect(int handle, tm_rect_t *out_rect, int *out_flag);
int tm_node_set_rect(int handle, const tm_rect_t *rect, int flag);
int tm_client_in_world(void);
int tm_panel_install(void);
void tm_panel_remove(void);
void tm_panel_tick(void);
int tm_panel_is_open(void);

#define TM_PACKET_MAP_DATA        0x00030100U
#define TM_PACKET_TIMER_HELLO     0x7F000001U
#define TM_PACKET_TIMER_SNAPSHOT  0x7F000002U
#define TM_TIMER_PROTOCOL         2U

typedef struct {
    int synced;
    unsigned int devil_tears_seconds;
    unsigned int shapeshift_seconds;
    unsigned int exp_seconds;
    unsigned int exp_multiplier;   /* 2 or 3; 0 when no EXP effect is running */
} tm_timer_view_t;

int tm_timers_install(void);
void tm_timers_reset(void);
void tm_timers_on_map_data(void);
void tm_timers_accept_snapshot(unsigned int protocol, const void *extra,
                               unsigned int extra_size);
void tm_timers_tick(void);
void tm_timers_view(tm_timer_view_t *out);

#endif /* OSMW_TIMERS_H */
