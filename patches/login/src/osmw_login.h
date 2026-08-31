#ifndef OSMW_LOGIN_H
#define OSMW_LOGIN_H

#include <windows.h>
#include "sigs.h"
#include "roster.h"
#include "claim.h"

extern unsigned char *lg_addr[LG_ADDR_COUNT];
extern unsigned char *lg_base;
extern unsigned int   lg_timestamp;

void lg_log_open(const char *path);
void lg_log_close(void);
void lg_log_prune(const char *dir, int keep);
void lg_log(const char *fmt, ...);

int  lg_resolve_all(void);

#define LG_MAX_PROLOGUE 24

typedef struct {
    void          *target;
    void          *trampoline;
    unsigned char  saved[LG_MAX_PROLOGUE];
    int            len;
    int            active;
} lg_hook_t;

int  lg_hook_install(lg_hook_t *h, void *target, void *detour, int prologue_len,
                    void **jmp_to);
void lg_hook_remove(lg_hook_t *h);

void        lg_config_load(void);
int         lg_cfg_int_in(const char *section, const char *key, int fallback);
const char *lg_game_dir(void);

/* Non-zero when this launch named a specific roster row; fills out with its id. */
int  lg_launch_row(char *out, int cap);

int  lg_autologin_start(void);
void lg_autologin_stop(void);

#endif
