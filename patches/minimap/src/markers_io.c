/* Persistent map markers: file access, sharing and change detection.
 *
 * Spec: mw-tools/tools/client-dll/map-markers.md section 4.
 *
 * The model in memory is a cache of map-markers.tsv. Reads are cheap and
 * rate-limited by the file's write time; writes are the careful part, because
 * five clients can run from one game folder:
 *
 *   take the named mutex -> reload from disk -> apply this one mutation by
 *   stable id -> write a temporary file -> atomic replace -> release
 *
 * Nothing writes back a model that was read before the mutex was taken, so a
 * client cannot erase a marker another client added while it had the panel
 * open. A file that failed to parse is never overwritten: mm_store_writable()
 * goes false and stays false until the file is repaired or replaced by hand.
 */
#include "osmw_minimap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MM_STORE_POLL_MS 2000

static mm_marker_set_t  g_set;
static mm_marker_t     *g_items;
static HANDLE           g_mutex;
static char             g_path[MAX_PATH];
static char             g_temp[MAX_PATH];
static FILETIME         g_seen;
static DWORD            g_last_poll;
static int              g_loaded;
static int              g_readonly;      /* parse failed; refuse to overwrite */

/* ---- paths and the cross-process lock ------------------------------------ */

/* One mutex per game folder, so two installs do not serialise against each
 * other. Backslashes cannot appear in a mutex name after the prefix. */
static void mm_store_mutex_name(const char *dir, char *out, int cap)
{
    int n, i;

    n = _snprintf(out, (size_t)cap, "%s%s", MM_STORE_MUTEX_PREFIX, dir);
    if (n < 0 || n >= cap) {
        _snprintf(out, (size_t)cap, "%s", MM_STORE_MUTEX_PREFIX);
        out[cap - 1] = '\0';
        return;
    }
    for (i = (int)strlen(MM_STORE_MUTEX_PREFIX); out[i] != '\0'; i++) {
        int c = (unsigned char)out[i];
        if (c == '\\' || c == '/' || c == ':')
            out[i] = '_';
        else if (c >= 'A' && c <= 'Z')
            out[i] = (char)(c - 'A' + 'a');
    }
}

int mm_store_init(void)
{
    const char *dir = mm_game_dir();
    char name[MAX_PATH + 64];

    if (dir == NULL || dir[0] == '\0') {
        mm_log("markers: no game directory; store disabled");
        return 0;
    }

    g_items = (mm_marker_t *)calloc(MM_MARKERS_MAX, sizeof(mm_marker_t));
    if (g_items == NULL) {
        mm_log("markers: out of memory for %d records", MM_MARKERS_MAX);
        return 0;
    }
    g_set.items = g_items;
    g_set.count = 0;

    _snprintf(g_path, sizeof(g_path), "%s\\%s", dir, MM_STORE_FILE);
    _snprintf(g_temp, sizeof(g_temp), "%s\\%s", dir, MM_STORE_TEMP);
    g_path[sizeof(g_path) - 1] = '\0';
    g_temp[sizeof(g_temp) - 1] = '\0';

    mm_store_mutex_name(dir, name, (int)sizeof(name));
    g_mutex = CreateMutexA(NULL, FALSE, name);
    if (g_mutex == NULL)
        mm_log("markers: no shared mutex (%lu); concurrent edits unprotected",
               GetLastError());

    g_loaded = 0;
    g_readonly = 0;
    memset(&g_seen, 0, sizeof(g_seen));
    g_last_poll = 0;
    return 1;
}

void mm_store_shutdown(void)
{
    if (g_mutex != NULL) {
        CloseHandle(g_mutex);
        g_mutex = NULL;
    }
    free(g_items);
    g_items = NULL;
    g_set.items = NULL;
    g_set.count = 0;
    g_loaded = 0;
}

static int mm_store_lock(void)
{
    if (g_mutex == NULL)
        return 1;                        /* degraded, but still usable alone */
    /* A holder that died mid-write abandons the mutex; we still get it. */
    switch (WaitForSingleObject(g_mutex, 3000)) {
    case WAIT_OBJECT_0:
    case WAIT_ABANDONED:
        return 1;
    default:
        mm_log("markers: timed out waiting for the store lock");
        return 0;
    }
}

static void mm_store_unlock(void)
{
    if (g_mutex != NULL)
        ReleaseMutex(g_mutex);
}

/* ---- raw file access ----------------------------------------------------- */

/* Reads the whole file. A missing file is success with an empty buffer: an
 * install that has never placed a marker is not an error. */
static int mm_store_read_file(char **out, int *out_len, FILETIME *written)
{
    HANDLE h;
    DWORD size, got = 0;
    char *buf;

    *out = NULL;
    *out_len = 0;

    h = CreateFileA(g_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return (GetLastError() == ERROR_FILE_NOT_FOUND) ? 1 : 0;

    if (written != NULL)
        GetFileTime(h, NULL, NULL, written);

    size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size > MM_STORE_MAX_BYTES) {
        CloseHandle(h);
        return 0;
    }

    buf = (char *)malloc((size_t)size + 1);
    if (buf == NULL) {
        CloseHandle(h);
        return 0;
    }
    if (!ReadFile(h, buf, size, &got, NULL) || got != size) {
        free(buf);
        CloseHandle(h);
        return 0;
    }
    CloseHandle(h);

    buf[size] = '\0';
    *out = buf;
    *out_len = (int)size;
    return 1;
}

/* Temp file, flushed, then replaced over the target. MoveFileEx with
 * WRITE_THROUGH is the atomic step: readers see the old file or the new one,
 * never a half-written one. */
static int mm_store_write_file(const mm_marker_set_t *set)
{
    HANDLE h;
    char *buf;
    int cap, len;
    DWORD wrote = 0;

    cap = mm_markers_serialised_bound(set->count);
    buf = (char *)malloc((size_t)cap);
    if (buf == NULL)
        return 0;

    len = mm_markers_serialise(set, buf, cap);
    if (len < 0) {
        free(buf);
        mm_log("markers: refusing to write, serialisation failed");
        return 0;
    }

    h = CreateFileA(g_temp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        free(buf);
        mm_log("markers: cannot create %s (%lu)", MM_STORE_TEMP, GetLastError());
        return 0;
    }
    if (!WriteFile(h, buf, (DWORD)len, &wrote, NULL) || wrote != (DWORD)len) {
        CloseHandle(h);
        DeleteFileA(g_temp);
        free(buf);
        mm_log("markers: short write to %s", MM_STORE_TEMP);
        return 0;
    }
    FlushFileBuffers(h);
    CloseHandle(h);
    free(buf);

    if (!MoveFileExA(g_temp, g_path,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileA(g_temp);
        mm_log("markers: atomic replace failed (%lu)", GetLastError());
        return 0;
    }
    return 1;
}

/* ---- load, poll, mutate -------------------------------------------------- */

/* Parse into the live model. On a header/version failure the model is left
 * empty and the store goes read-only, so nothing overwrites a file we did not
 * understand. */
static int mm_store_reload_locked(void)
{
    mm_marker_stats_t stats;
    char *buf = NULL;
    int len = 0, rc;

    if (!mm_store_read_file(&buf, &len, &g_seen)) {
        mm_log("markers: cannot read %s", MM_STORE_FILE);
        return 0;
    }
    if (buf == NULL) {                   /* no file yet */
        g_set.count = 0;
        g_loaded = 1;
        g_readonly = 0;
        return 1;
    }

    rc = mm_markers_parse(buf, len, &g_set, &stats);
    free(buf);

    if (rc == MM_MARKER_ERR_HEADER || rc == MM_MARKER_ERR_VERSION) {
        g_set.count = 0;
        g_readonly = 1;
        g_loaded = 1;
        mm_log("markers: %s is %s; store is read-only until it is repaired",
               MM_STORE_FILE,
               rc == MM_MARKER_ERR_VERSION ? "a newer version" : "not a marker file");
        return 0;
    }
    if (rc != MM_MARKER_OK) {
        g_readonly = 1;
        g_loaded = 1;
        return 0;
    }

    g_readonly = 0;
    g_loaded = 1;
    if (stats.rejected || stats.duplicates || stats.overflow)
        mm_log("markers: loaded %d, dropped %d bad, %d duplicate, %d over cap",
               stats.accepted, stats.rejected, stats.duplicates, stats.overflow);
    return 1;
}

int mm_store_load(void)
{
    int ok;

    if (g_items == NULL)
        return 0;
    if (!mm_store_lock())
        return 0;
    ok = mm_store_reload_locked();
    mm_store_unlock();
    g_last_poll = GetTickCount();
    return ok;
}

/* Cheap freshness check for the draw path: stat the file at most every two
 * seconds, and only reparse when its write time actually moved. */
void mm_store_poll(void)
{
    WIN32_FILE_ATTRIBUTE_DATA info;
    DWORD now;

    if (g_items == NULL)
        return;
    if (!g_loaded) {
        mm_store_load();
        return;
    }

    now = GetTickCount();
    if ((DWORD)(now - g_last_poll) < MM_STORE_POLL_MS)
        return;
    g_last_poll = now;

    if (!GetFileAttributesExA(g_path, GetFileExInfoStandard, &info))
        return;
    if (CompareFileTime(&info.ftLastWriteTime, &g_seen) == 0)
        return;

    mm_store_load();
}

const mm_marker_set_t *mm_store_get(void)
{
    return &g_set;
}

int mm_store_writable(void)
{
    return g_items != NULL && !g_readonly;
}

/* The one mutation entry point. `remove` non-zero deletes by id; otherwise the
 * record is upserted. Everything is done under the lock against a freshly
 * reloaded model, so a concurrent client's additions survive. */
static int mm_store_mutate(const mm_marker_t *m, const char *id, int remove)
{
    int rc;

    if (g_items == NULL)
        return MM_MARKER_ERR_INVALID;
    if (!mm_store_lock())
        return MM_MARKER_ERR_LOCKED;

    if (!mm_store_reload_locked()) {
        mm_store_unlock();
        return MM_MARKER_ERR_READONLY;
    }

    rc = remove ? mm_markers_erase(&g_set, id)
                : mm_markers_upsert(&g_set, m);
    if (rc != MM_MARKER_OK) {
        mm_store_unlock();
        return rc;
    }

    if (!mm_store_write_file(&g_set)) {
        mm_store_unlock();
        /* The model now reflects a write that did not land. Force a reload on
         * the next poll rather than leaving the caller with a phantom. */
        g_loaded = 0;
        return MM_MARKER_ERR_IO;
    }

    /* Our own write must not look like someone else's change. */
    {
        WIN32_FILE_ATTRIBUTE_DATA info;
        if (GetFileAttributesExA(g_path, GetFileExInfoStandard, &info))
            g_seen = info.ftLastWriteTime;
    }
    g_last_poll = GetTickCount();
    mm_store_unlock();
    return MM_MARKER_OK;
}

int mm_store_put(const mm_marker_t *m)
{
    return mm_store_mutate(m, NULL, 0);
}

int mm_store_remove(const char *id)
{
    return mm_store_mutate(NULL, id, 1);
}
