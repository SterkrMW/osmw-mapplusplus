#ifndef OSMW_LOGIN_CLAIM_H
#define OSMW_LOGIN_CLAIM_H

#include <windows.h>
#include "roster.h"

#define LG_CLAIM_MUTEX   "Local\\osmw_login_claim"
#define LG_CLAIM_MAPPING "Local\\osmw_login_claims"
#define LG_CLAIM_MAGIC   0x4C47434Du /* 'LGCM' */

typedef struct {
    DWORD pid;
    char  id[LG_ID_MAX];
} lg_claim_slot_t;

typedef struct {
    DWORD           magic;
    lg_claim_slot_t slots[LG_ROWS_MAX];
} lg_claim_shm_t;

/* First enabled roster row whose id is not claimed by a live PID. */
int  lg_claim_row(lg_roster_t *r, lg_row_t **out);
/* One named row, whether or not it is enabled; fails when a live PID holds it. */
int  lg_claim_row_id(lg_roster_t *r, const char *id, lg_row_t **out);
void lg_claim_release(const char *id);
void lg_claim_clear(void);

#endif
