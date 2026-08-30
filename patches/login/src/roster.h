#ifndef OSMW_LOGIN_ROSTER_H
#define OSMW_LOGIN_ROSTER_H

#include <windows.h>

#define LG_ACC_MAX    32
#define LG_CHAR_MAX   32
#define LG_ID_MAX     40
#define LG_ROWS_MAX   16
#define LG_ROSTER_NAME "login_roster.dat"

typedef struct {
    char           id[LG_ID_MAX];
    char           account[LG_ACC_MAX];
    char           character[LG_CHAR_MAX];
    unsigned char *pw_blob;
    DWORD          pw_blob_len;
    int            enabled;
} lg_row_t;

typedef struct {
    lg_row_t rows[LG_ROWS_MAX];
    int      count;
} lg_roster_t;

int  lg_protect(const char *plain, unsigned char **blob, DWORD *len);
int  lg_unprotect(const unsigned char *blob, DWORD len, char *plain, int cap);

void lg_roster_free(lg_roster_t *r);
int  lg_roster_add(lg_roster_t *r, const char *account, const char *password,
                   const char *character);
int  lg_roster_save(const char *path, const lg_roster_t *r);
int  lg_roster_load(const char *path, lg_roster_t *r);
int  lg_row_password(const lg_row_t *row, char *out, int cap);
int  lg_row_keep_password(lg_row_t *row, const lg_row_t *from);

#endif
