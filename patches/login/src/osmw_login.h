#ifndef OSMW_LOGIN_H
#define OSMW_LOGIN_H

#include <windows.h>
#include "osmw_core_api.h"
#include "sigs.h"
#include "roster.h"
#include "claim.h"

extern unsigned char *lg_addr[LG_ADDR_COUNT];

const OSMW_CORE_API_V1 *lg_core_api(void);
OSMW_OWNER lg_core_owner(void);

void lg_log_open(const char *path);
void lg_log_close(void);
void lg_log_prune(const char *dir, int keep);
void lg_log(const char *fmt, ...);

OSMW_RESULT lg_resolve_all(void);
void lg_resolve_reset(void);

#define LG_MAX_PROLOGUE 24

typedef struct {
    void          *target;
    void          *trampoline;
    unsigned char  saved[LG_MAX_PROLOGUE];
    int            len;
    int            active;
    OSMW_TOKEN     claim_token;
} lg_hook_t;

int  lg_hook_install(lg_hook_t *h, void *target, void *detour, int prologue_len,
                    void **jmp_to, const char *label);
OSMW_RESULT lg_hook_remove(lg_hook_t *h);

void        lg_config_load(void);
int         lg_cfg_int_in(const char *section, const char *key, int fallback);
const char *lg_game_dir(void);

/* Non-zero when this launch named a specific roster row; fills out with its id. */
int  lg_launch_row(char *out, int cap);

int  lg_autologin_start(void);
OSMW_RESULT lg_autologin_stop(void);

#endif
