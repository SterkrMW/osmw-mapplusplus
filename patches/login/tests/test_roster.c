/* Roster file + DPAPI tests. Compiled by build.py --test. */
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "roster.h"
#include "claim.h"

static int g_fail;

static void expect(int cond, const char *msg)
{
    if (!cond) {
        printf("FAIL: %s\n", msg);
        g_fail++;
    } else {
        printf("ok:   %s\n", msg);
    }
}

static void test_protect_roundtrip(void)
{
    unsigned char *blob = NULL;
    DWORD blob_len = 0;
    char plain[64];

    expect(lg_protect("secret-password", &blob, &blob_len) == 1,
           "protect a password");
    expect(blob != NULL && blob_len > 0, "blob is non-empty");
    expect(lg_unprotect(blob, blob_len, plain, sizeof(plain)) == 1,
           "unprotect the blob");
    expect(strcmp(plain, "secret-password") == 0, "plaintext matches");
    HeapFree(GetProcessHeap(), 0, blob);
}

static void test_roster_file(const char *path)
{
    lg_roster_t r, loaded;
    char pw[64];

    DeleteFileA(path);
    memset(&r, 0, sizeof(r));
    expect(lg_roster_add(&r, "acc1", "pw1", "#1") == 1, "add row 1");
    expect(lg_roster_add(&r, "acc2", "pw2", "#2") == 1, "add row 2");
    expect(r.count == 2, "count is 2");
    expect(lg_roster_save(path, &r) == 1, "save roster");
    lg_roster_free(&r);

    memset(&loaded, 0, sizeof(loaded));
    expect(lg_roster_load(path, &loaded) == 1, "load roster");
    expect(loaded.count == 2, "loaded count is 2");
    expect(strcmp(loaded.rows[0].account, "acc1") == 0, "account 1");
    expect(strcmp(loaded.rows[0].character, "#1") == 0, "character 1");
    expect(strcmp(loaded.rows[1].account, "acc2") == 0, "account 2");
    expect(lg_row_password(&loaded.rows[0], pw, sizeof(pw)) == 1, "decrypt row 1");
    expect(strcmp(pw, "pw1") == 0, "password 1");
    expect(lg_row_password(&loaded.rows[1], pw, sizeof(pw)) == 1, "decrypt row 2");
    expect(strcmp(pw, "pw2") == 0, "password 2");
    lg_roster_free(&loaded);
    DeleteFileA(path);
}

static void test_claim(void)
{
    lg_roster_t r;
    lg_row_t *a, *b, *c;
    HANDLE mapping;
    lg_claim_shm_t *shm;

    memset(&r, 0, sizeof(r));
    lg_roster_add(&r, "a", "p", "#1");
    lg_roster_add(&r, "b", "p", "#2");
    lg_roster_add(&r, "c", "p", "#3");
    r.rows[0].enabled = r.rows[1].enabled = r.rows[2].enabled = 1;

    lg_claim_clear();

    expect(lg_claim_row(&r, &a) == 1, "claim first row");
    expect(a == &r.rows[0], "first claim is row 0");
    expect(lg_claim_row(&r, &b) == 1, "claim second row");
    expect(b == &r.rows[1], "second claim is row 1");

    /* Stale PID: poke a dead pid into slot 0 so a third claim can reuse row 0
     * after we release nothing — we overwrite the live pid with 1. */
    mapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, LG_CLAIM_MAPPING);
    expect(mapping != NULL, "open claim mapping");
    shm = (lg_claim_shm_t *)MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(*shm));
    expect(shm != NULL, "map claim view");
    if (shm) {
        shm->slots[0].pid = 1; /* PID 1 is not a live MythWar client */
        UnmapViewOfFile(shm);
    }
    if (mapping)
        CloseHandle(mapping);

    expect(lg_claim_row(&r, &c) == 1, "claim reuses stale slot");
    expect(c == &r.rows[0], "stale pid released row 0");

    lg_claim_release(r.rows[1].id);
    expect(lg_claim_row(&r, &a) == 1, "claim after explicit release");
    expect(a == &r.rows[1], "released row 1 is next");

    lg_roster_free(&r);
    lg_claim_clear();
}

int main(void)
{
    char path[MAX_PATH];
    GetTempPathA(sizeof(path), path);
    strcat(path, "osmw_login_roster_test.dat");

    test_protect_roundtrip();
    test_roster_file(path);
    test_claim();

    printf("%s: %d failure(s)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
