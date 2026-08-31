#include "claim.h"

#include <string.h>

static HANDLE          g_mutex;
static HANDLE          g_map;
static lg_claim_shm_t *g_shm;

static int pid_live(DWORD pid)
{
    HANDLE h;
    DWORD  code = 0;

    if (!pid)
        return 0;
    h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!h)
        return 0;
    if (!GetExitCodeProcess(h, &code)) {
        CloseHandle(h);
        return 0;
    }
    CloseHandle(h);
    return code == STILL_ACTIVE;
}

static int ensure(void)
{
    DWORD created = 0;

    if (g_shm)
        return 1;
    g_mutex = CreateMutexA(NULL, FALSE, LG_CLAIM_MUTEX);
    if (!g_mutex)
        return 0;
    g_map = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                               0, sizeof(lg_claim_shm_t), LG_CLAIM_MAPPING);
    if (!g_map) {
        CloseHandle(g_mutex);
        g_mutex = NULL;
        return 0;
    }
    created = (GetLastError() != ERROR_ALREADY_EXISTS);
    g_shm = (lg_claim_shm_t *)MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0,
                                            sizeof(lg_claim_shm_t));
    if (!g_shm) {
        CloseHandle(g_map);
        CloseHandle(g_mutex);
        g_map = NULL;
        g_mutex = NULL;
        return 0;
    }
    if (created || g_shm->magic != LG_CLAIM_MAGIC) {
        memset(g_shm, 0, sizeof(*g_shm));
        g_shm->magic = LG_CLAIM_MAGIC;
    }
    return 1;
}

static int row_claimed_live(const char *id)
{
    int i;
    for (i = 0; i < LG_ROWS_MAX; i++) {
        if (g_shm->slots[i].id[0] == '\0')
            continue;
        if (strcmp(g_shm->slots[i].id, id) != 0)
            continue;
        if (pid_live(g_shm->slots[i].pid))
            return 1;
        memset(&g_shm->slots[i], 0, sizeof(g_shm->slots[i]));
    }
    return 0;
}

int lg_claim_row(lg_roster_t *r, lg_row_t **out)
{
    int i, s;

    if (!r || !out)
        return 0;
    *out = NULL;
    if (!ensure())
        return 0;
    WaitForSingleObject(g_mutex, INFINITE);
    for (i = 0; i < r->count; i++) {
        if (!r->rows[i].enabled || !r->rows[i].id[0])
            continue;
        if (row_claimed_live(r->rows[i].id))
            continue;
        for (s = 0; s < LG_ROWS_MAX; s++) {
            if (g_shm->slots[s].id[0] == '\0' || !pid_live(g_shm->slots[s].pid)) {
                memset(&g_shm->slots[s], 0, sizeof(g_shm->slots[s]));
                g_shm->slots[s].pid = GetCurrentProcessId();
                strncpy(g_shm->slots[s].id, r->rows[i].id, LG_ID_MAX - 1);
                *out = &r->rows[i];
                ReleaseMutex(g_mutex);
                return 1;
            }
        }
        break;
    }
    ReleaseMutex(g_mutex);
    return 0;
}

int lg_claim_row_id(lg_roster_t *r, const char *id, lg_row_t **out)
{
    int i, s;

    if (!r || !id || !id[0] || !out)
        return 0;
    *out = NULL;
    if (!ensure())
        return 0;
    WaitForSingleObject(g_mutex, INFINITE);
    for (i = 0; i < r->count; i++) {
        if (strcmp(r->rows[i].id, id) != 0)
            continue;
        /* enabled is deliberately not tested here. Naming a row is the player's
         * explicit act; the flag only governs which rows the next-free path is
         * allowed to wander into. */
        if (row_claimed_live(id))
            break;
        for (s = 0; s < LG_ROWS_MAX; s++) {
            if (g_shm->slots[s].id[0] == '\0' || !pid_live(g_shm->slots[s].pid)) {
                memset(&g_shm->slots[s], 0, sizeof(g_shm->slots[s]));
                g_shm->slots[s].pid = GetCurrentProcessId();
                strncpy(g_shm->slots[s].id, r->rows[i].id, LG_ID_MAX - 1);
                *out = &r->rows[i];
                ReleaseMutex(g_mutex);
                return 1;
            }
        }
        break;
    }
    ReleaseMutex(g_mutex);
    return 0;
}

void lg_claim_release(const char *id)
{
    int i;
    if (!id || !ensure())
        return;
    WaitForSingleObject(g_mutex, INFINITE);
    for (i = 0; i < LG_ROWS_MAX; i++) {
        if (strcmp(g_shm->slots[i].id, id) == 0)
            memset(&g_shm->slots[i], 0, sizeof(g_shm->slots[i]));
    }
    ReleaseMutex(g_mutex);
}

void lg_claim_clear(void)
{
    if (!ensure())
        return;
    WaitForSingleObject(g_mutex, INFINITE);
    memset(g_shm->slots, 0, sizeof(g_shm->slots));
    ReleaseMutex(g_mutex);
}
