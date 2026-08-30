#include "roster.h"

#include <wincrypt.h>
#include <stdio.h>
#include <string.h>

#define MAGIC "OSMWROS1"

int lg_protect(const char *plain, unsigned char **blob, DWORD *len)
{
    DATA_BLOB in, out;

    if (!plain || !blob || !len)
        return 0;
    *blob = NULL;
    *len = 0;
    in.cbData = (DWORD)strlen(plain) + 1;
    in.pbData = (BYTE *)plain;
    if (!CryptProtectData(&in, L"osmw_login", NULL, NULL, NULL,
                          CRYPTPROTECT_UI_FORBIDDEN, &out))
        return 0;
    *blob = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, out.cbData);
    if (!*blob) {
        LocalFree(out.pbData);
        return 0;
    }
    memcpy(*blob, out.pbData, out.cbData);
    *len = out.cbData;
    LocalFree(out.pbData);
    return 1;
}

int lg_unprotect(const unsigned char *blob, DWORD len, char *plain, int cap)
{
    DATA_BLOB in, out;

    if (!blob || !len || !plain || cap <= 0)
        return 0;
    in.cbData = len;
    in.pbData = (BYTE *)blob;
    if (!CryptUnprotectData(&in, NULL, NULL, NULL, NULL,
                            CRYPTPROTECT_UI_FORBIDDEN, &out))
        return 0;
    {
        DWORD n = out.cbData;
        if (n >= (DWORD)cap)
            n = (DWORD)cap - 1;
        memcpy(plain, out.pbData, n);
        plain[n] = '\0';
    }
    LocalFree(out.pbData);
    return 1;
}

void lg_roster_free(lg_roster_t *r)
{
    int i;
    if (!r)
        return;
    for (i = 0; i < r->count; i++) {
        if (r->rows[i].pw_blob)
            HeapFree(GetProcessHeap(), 0, r->rows[i].pw_blob);
        r->rows[i].pw_blob = NULL;
        r->rows[i].pw_blob_len = 0;
    }
    r->count = 0;
}

static void make_id(char *out, int cap)
{
    static volatile LONG seq;
    LONG n = InterlockedIncrement(&seq);
    _snprintf(out, cap, "%08lX-%08lX-%08lX",
              (unsigned long)GetTickCount(),
              (unsigned long)GetCurrentProcessId(),
              (unsigned long)n);
    out[cap - 1] = '\0';
}

int lg_roster_add(lg_roster_t *r, const char *account, const char *password,
                  const char *character)
{
    lg_row_t *row;

    if (!r || !account || !password || !character)
        return 0;
    if (r->count >= LG_ROWS_MAX)
        return 0;
    if (!account[0] || strlen(account) >= LG_ACC_MAX)
        return 0;
    if (strlen(character) >= LG_CHAR_MAX)
        return 0;

    row = &r->rows[r->count];
    memset(row, 0, sizeof(*row));
    make_id(row->id, sizeof(row->id));
    strncpy(row->account, account, LG_ACC_MAX - 1);
    strncpy(row->character, character, LG_CHAR_MAX - 1);
    row->enabled = 1;
    if (!lg_protect(password, &row->pw_blob, &row->pw_blob_len))
        return 0;
    r->count++;
    return 1;
}

int lg_row_password(const lg_row_t *row, char *out, int cap)
{
    if (!row || !out)
        return 0;
    return lg_unprotect(row->pw_blob, row->pw_blob_len, out, cap);
}

int lg_row_keep_password(lg_row_t *row, const lg_row_t *from)
{
    if (!row || !from || !from->pw_blob || !from->pw_blob_len)
        return 0;
    if (row->pw_blob)
        HeapFree(GetProcessHeap(), 0, row->pw_blob);
    row->pw_blob = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, from->pw_blob_len);
    if (!row->pw_blob)
        return 0;
    memcpy(row->pw_blob, from->pw_blob, from->pw_blob_len);
    row->pw_blob_len = from->pw_blob_len;
    return 1;
}

int lg_roster_save(const char *path, const lg_roster_t *r)
{
    HANDLE f;
    DWORD n, i, count;
    char magic[8];

    if (!path || !r)
        return 0;
    f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    memcpy(magic, MAGIC, 8);
    count = (DWORD)r->count;
    if (!WriteFile(f, magic, 8, &n, NULL) ||
        !WriteFile(f, &count, 4, &n, NULL)) {
        CloseHandle(f);
        return 0;
    }
    for (i = 0; i < count; i++) {
        const lg_row_t *row = &r->rows[i];
        DWORD enabled = row->enabled ? 1u : 0u;
        if (!WriteFile(f, row->id, LG_ID_MAX, &n, NULL) ||
            !WriteFile(f, row->account, LG_ACC_MAX, &n, NULL) ||
            !WriteFile(f, row->character, LG_CHAR_MAX, &n, NULL) ||
            !WriteFile(f, &enabled, 4, &n, NULL) ||
            !WriteFile(f, &row->pw_blob_len, 4, &n, NULL) ||
            (row->pw_blob_len &&
             !WriteFile(f, row->pw_blob, row->pw_blob_len, &n, NULL))) {
            CloseHandle(f);
            return 0;
        }
    }
    CloseHandle(f);
    return 1;
}

int lg_roster_load(const char *path, lg_roster_t *r)
{
    HANDLE f;
    DWORD n, i, count = 0;
    char magic[8];

    if (!path || !r)
        return 0;
    memset(r, 0, sizeof(*r));
    f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    if (!ReadFile(f, magic, 8, &n, NULL) || n != 8 || memcmp(magic, MAGIC, 8) ||
        !ReadFile(f, &count, 4, &n, NULL) || n != 4 || count > LG_ROWS_MAX) {
        CloseHandle(f);
        return 0;
    }
    for (i = 0; i < count; i++) {
        lg_row_t *row = &r->rows[i];
        DWORD enabled = 0;
        memset(row, 0, sizeof(*row));
        if (!ReadFile(f, row->id, LG_ID_MAX, &n, NULL) || n != LG_ID_MAX ||
            !ReadFile(f, row->account, LG_ACC_MAX, &n, NULL) || n != LG_ACC_MAX ||
            !ReadFile(f, row->character, LG_CHAR_MAX, &n, NULL) || n != LG_CHAR_MAX ||
            !ReadFile(f, &enabled, 4, &n, NULL) || n != 4 ||
            !ReadFile(f, &row->pw_blob_len, 4, &n, NULL) || n != 4) {
            lg_roster_free(r);
            CloseHandle(f);
            return 0;
        }
        row->enabled = enabled ? 1 : 0;
        row->id[LG_ID_MAX - 1] = '\0';
        row->account[LG_ACC_MAX - 1] = '\0';
        row->character[LG_CHAR_MAX - 1] = '\0';
        if (row->pw_blob_len) {
            if (row->pw_blob_len > 4096) {
                lg_roster_free(r);
                CloseHandle(f);
                return 0;
            }
            row->pw_blob = (unsigned char *)HeapAlloc(GetProcessHeap(), 0,
                                                      row->pw_blob_len);
            if (!row->pw_blob ||
                !ReadFile(f, row->pw_blob, row->pw_blob_len, &n, NULL) ||
                n != row->pw_blob_len) {
                lg_roster_free(r);
                CloseHandle(f);
                return 0;
            }
        }
        r->count++;
    }
    CloseHandle(f);
    return 1;
}
