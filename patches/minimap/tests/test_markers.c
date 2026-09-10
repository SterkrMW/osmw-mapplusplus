/* Host fixture for the marker store.
 *
 * The store is two halves and they are tested differently. markers.c is pure
 * text and model code, so it is compiled straight into this fixture. The file
 * half needs a game directory, a real mutex and real atomic replaces, so it
 * runs against a scratch folder with mm_game_dir() and mm_log() stubbed out.
 *
 * Build and run:
 *     python patches/minimap/tools/build.py --test
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Stubs for the two core adapters markers_io.c uses. Declared before the
 * sources are pulled in so the real prototypes still apply. */
static char g_test_dir[MAX_PATH];
static int  g_log_lines;

#define mm_game_dir mm_test_game_dir
#define mm_log      mm_test_log

static const char *mm_test_game_dir(void);
static void mm_test_log(const char *fmt, ...);

#include "../src/markers.c"
#include "../src/markers_io.c"

#undef mm_game_dir
#undef mm_log

static const char *mm_test_game_dir(void) { return g_test_dir; }

static void mm_test_log(const char *fmt, ...)
{
    va_list ap;
    char line[512];
    va_start(ap, fmt);
    _vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    line[sizeof(line) - 1] = '\0';
    g_log_lines++;
    printf("      log: %s\n", line);
}

/* ---- harness ------------------------------------------------------------- */

static int g_fail;

static void ok(int cond, const char *what)
{
    printf("  %s %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond)
        g_fail++;
}

static mm_marker_t g_backing[MM_MARKERS_MAX];

static mm_marker_set_t make_set(void)
{
    mm_marker_set_t s;
    s.items = g_backing;
    s.count = 0;
    return s;
}

static mm_marker_t make_marker(const char *id, unsigned map, unsigned x,
                               unsigned y, unsigned kind, const char *label)
{
    mm_marker_t m;
    memset(&m, 0, sizeof(m));
    _snprintf(m.id, sizeof(m.id), "%s", id);
    m.map_file = (unsigned short)map;
    m.x = x;
    m.y = y;
    m.kind = (unsigned char)kind;
    m.flags = MM_MARKER_FLAG_VISIBLE;
    _snprintf(m.label, sizeof(m.label), "%s", label ? label : "");
    return m;
}

/* ---- parse and serialise ------------------------------------------------- */

static void test_roundtrip(void)
{
    mm_marker_set_t set = make_set();
    mm_marker_set_t back = make_set();
    static mm_marker_t back_items[MM_MARKERS_MAX];
    mm_marker_stats_t stats;
    char buf[4096];
    int len;
    mm_marker_t m;

    back.items = back_items;

    m = make_marker("00112233445566aa", 37, 120, 240, MM_MARKER_KIND_SHOP,
                    "Blacksmith");
    ok(mm_markers_upsert(&set, &m) == MM_MARKER_OK, "upsert accepts a marker");

    /* A percent and a high byte from the client's code page both have to
     * survive byte for byte. */
    m = make_marker("ffeeddccbbaa9988", 37, 5, 6, MM_MARKER_KIND_NOTE,
                    "50% done \xE9");
    ok(mm_markers_upsert(&set, &m) == MM_MARKER_OK, "upsert accepts an odd label");

    /* A tab would encode fine and then be dropped by the parser as a control
     * character, so it must never reach the model in the first place. */
    m = make_marker("ffeeddccbbaa9977", 37, 5, 6, MM_MARKER_KIND_NOTE,
                    "tab\there");
    ok(mm_markers_upsert(&set, &m) == MM_MARKER_ERR_INVALID,
       "upsert refuses a control character in a label");

    len = mm_markers_serialise(&set, buf, (int)sizeof(buf));
    ok(len > 0, "serialise succeeds");
    ok(strstr(buf, "OSMW-MARKERS\t1\r\n") == buf, "header is first");
    ok(strstr(buf, "50%25 done %E9") != NULL, "label is percent-encoded");

    ok(mm_markers_parse(buf, len, &back, &stats) == MM_MARKER_OK, "reparse ok");
    ok(back.count == 2 && stats.accepted == 2, "both records return");
    ok(strcmp(back.items[0].label, "Blacksmith") == 0, "label 1 intact");
    ok(strcmp(back.items[1].label, "50% done \xE9") == 0, "label 2 intact");
    ok(back.items[0].map_file == 37 && back.items[0].x == 120 &&
       back.items[0].y == 240 && back.items[0].kind == MM_MARKER_KIND_SHOP,
       "fields intact");
}

static void test_bad_header(void)
{
    mm_marker_set_t set = make_set();
    const char *not_ours = "# something else\r\nabc\r\n";
    const char *newer = "OSMW-MARKERS\t2\r\n";

    ok(mm_markers_parse(not_ours, (int)strlen(not_ours), &set, NULL)
       == MM_MARKER_ERR_HEADER, "foreign file rejected");
    ok(mm_markers_parse(newer, (int)strlen(newer), &set, NULL)
       == MM_MARKER_ERR_VERSION, "newer version rejected");
}

static void test_bad_records(void)
{
    mm_marker_set_t set = make_set();
    mm_marker_stats_t stats;
    /* good, short id, non-hex id, bad kind, bad escape, missing field,
     * duplicate of the good one, bad flags */
    const char *text =
        "OSMW-MARKERS\t1\n"
        "00112233445566aa\t37\t1\t2\t0\t1\tGood\n"
        "0011\t37\t1\t2\t0\t1\tShortId\n"
        "00112233445566zz\t37\t1\t2\t0\t1\tNotHex\n"
        "00112233445566ab\t37\t1\t2\t9\t1\tBadKind\n"
        "00112233445566ac\t37\t1\t2\t0\t1\tBad%ZZ\n"
        "00112233445566ad\t37\t1\t2\t0\n"
        "00112233445566aa\t37\t9\t9\t0\t1\tDupe\n"
        "00112233445566ae\t37\t1\t2\t0\t255\tBadFlags\n";

    ok(mm_markers_parse(text, (int)strlen(text), &set, &stats) == MM_MARKER_OK,
       "a file of mostly rubbish still parses");
    ok(set.count == 1 && stats.accepted == 1, "only the good record survives");
    ok(stats.rejected == 6, "six malformed records counted");
    ok(stats.duplicates == 1, "the duplicate id is counted separately");
    ok(strcmp(set.items[0].label, "Good") == 0, "the survivor is the good one");
}

static void test_bare_lf_and_crlf(void)
{
    mm_marker_set_t set = make_set();
    const char *lf = "OSMW-MARKERS\t1\n00112233445566aa\t1\t2\t3\t0\t1\tA\n";
    const char *crlf = "OSMW-MARKERS\t1\r\n00112233445566aa\t1\t2\t3\t0\t1\tA\r\n";

    ok(mm_markers_parse(lf, (int)strlen(lf), &set, NULL) == MM_MARKER_OK &&
       set.count == 1, "bare LF accepted");
    ok(mm_markers_parse(crlf, (int)strlen(crlf), &set, NULL) == MM_MARKER_OK &&
       set.count == 1, "CRLF accepted");
}

static void test_caps(void)
{
    mm_marker_set_t set = make_set();
    mm_marker_t m;
    char id[MM_MARKER_ID_LEN + 1];
    int i, rc = MM_MARKER_OK;

    for (i = 0; i < MM_MARKERS_PER_MAP; i++) {
        _snprintf(id, sizeof(id), "%016x", i);
        m = make_marker(id, 12, (unsigned)i, 0, MM_MARKER_KIND_NOTE, "x");
        rc = mm_markers_upsert(&set, &m);
        if (rc != MM_MARKER_OK)
            break;
    }
    ok(rc == MM_MARKER_OK && set.count == MM_MARKERS_PER_MAP,
       "64 markers fit on one map");

    _snprintf(id, sizeof(id), "%016x", 0x1000);
    m = make_marker(id, 12, 1, 1, MM_MARKER_KIND_NOTE, "over");
    ok(mm_markers_upsert(&set, &m) == MM_MARKER_ERR_MAP_FULL,
       "the 65th on that map is refused");

    m = make_marker(id, 13, 1, 1, MM_MARKER_KIND_NOTE, "other map");
    ok(mm_markers_upsert(&set, &m) == MM_MARKER_OK,
       "the same marker fits on a different map");

    /* Moving it back onto the full map must be refused too. */
    m.map_file = 12;
    ok(mm_markers_upsert(&set, &m) == MM_MARKER_ERR_MAP_FULL,
       "moving onto a full map is refused");
}

static void test_label_too_long(void)
{
    mm_marker_set_t set = make_set();
    char text[256];
    char label[MM_MARKER_LABEL_MAX + 8];
    mm_marker_stats_t stats;

    memset(label, 'a', sizeof(label) - 1);
    label[sizeof(label) - 1] = '\0';
    _snprintf(text, sizeof(text),
              "OSMW-MARKERS\t1\n00112233445566aa\t1\t2\t3\t0\t1\t%s\n", label);

    ok(mm_markers_parse(text, (int)strlen(text), &set, &stats) == MM_MARKER_OK,
       "an over-long label does not kill the file");
    ok(set.count == 0 && stats.rejected == 1, "the over-long record is dropped");
}

/* ---- file layer ---------------------------------------------------------- */

static void write_raw(const char *text)
{
    char path[MAX_PATH];
    HANDLE h;
    DWORD wrote = 0;
    _snprintf(path, sizeof(path), "%s\\%s", g_test_dir, MM_STORE_FILE);
    h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    WriteFile(h, text, (DWORD)strlen(text), &wrote, NULL);
    CloseHandle(h);
}

static int file_exists(const char *name)
{
    char path[MAX_PATH];
    _snprintf(path, sizeof(path), "%s\\%s", g_test_dir, name);
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

static void test_store_file(void)
{
    mm_marker_t m;
    char id[MM_MARKER_ID_LEN + 1];
    const mm_marker_set_t *live;

    ok(mm_store_init() == 1, "store init");
    ok(mm_store_load() == 1, "loading a missing file is not an error");
    ok(mm_store_get()->count == 0, "and yields an empty model");
    ok(mm_store_writable() == 1, "a missing file is writable");

    mm_markers_make_id(id);
    m = make_marker(id, 37, 100, 200, MM_MARKER_KIND_QUEST, "Elder");
    ok(mm_store_put(&m) == MM_MARKER_OK, "put writes a marker");
    ok(file_exists(MM_STORE_FILE), "the file now exists");
    ok(!file_exists(MM_STORE_TEMP), "no temp file is left behind");

    live = mm_store_get();
    ok(live->count == 1 && strcmp(live->items[0].label, "Elder") == 0,
       "the model reflects the write");

    /* A second client appends behind our back. Our next mutation must keep
     * that record: this is the merge-by-id rule. */
    {
        char path[MAX_PATH], buf[1024];
        HANDLE h;
        DWORD wrote = 0, got = 0;
        _snprintf(path, sizeof(path), "%s\\%s", g_test_dir, MM_STORE_FILE);
        h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        ReadFile(h, buf, sizeof(buf) - 1, &got, NULL);
        CloseHandle(h);
        buf[got] = '\0';
        strcat(buf, "aaaabbbbccccdddd\t37\t9\t9\t0\t1\tOtherClient\r\n");
        h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL);
        WriteFile(h, buf, (DWORD)strlen(buf), &wrote, NULL);
        CloseHandle(h);
    }

    mm_markers_make_id(id);
    m = make_marker(id, 37, 300, 400, MM_MARKER_KIND_SHOP, "Mine");
    ok(mm_store_put(&m) == MM_MARKER_OK, "put after a foreign edit");
    live = mm_store_get();
    ok(live->count == 3, "our two and the other client's one all survive");
    ok(mm_markers_find(live, "aaaabbbbccccdddd") >= 0,
       "the foreign record was not erased");

    ok(mm_store_remove("aaaabbbbccccdddd") == MM_MARKER_OK, "remove by id");
    ok(mm_store_get()->count == 2, "and it is gone");
    ok(mm_store_remove("aaaabbbbccccdddd") == MM_MARKER_ERR_MISSING,
       "removing it twice reports missing");

    mm_store_shutdown();
}

static void test_store_refuses_bad_file(void)
{
    mm_marker_t m;
    char path[MAX_PATH], before[512], after[512];
    HANDLE h;
    DWORD got = 0;

    write_raw("OSMW-MARKERS\t99\r\nnot going to parse\r\n");
    _snprintf(path, sizeof(path), "%s\\%s", g_test_dir, MM_STORE_FILE);
    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    ReadFile(h, before, sizeof(before) - 1, &got, NULL);
    CloseHandle(h);
    before[got] = '\0';

    ok(mm_store_init() == 1, "store init over an unreadable file");
    ok(mm_store_load() == 0, "load reports failure");
    ok(mm_store_writable() == 0, "and the store goes read-only");

    mm_markers_make_id(m.id);
    m = make_marker("1111222233334444", 1, 1, 1, MM_MARKER_KIND_NOTE, "nope");
    ok(mm_store_put(&m) == MM_MARKER_ERR_READONLY, "put is refused");

    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    got = 0;
    ReadFile(h, after, sizeof(after) - 1, &got, NULL);
    CloseHandle(h);
    after[got] = '\0';
    ok(strcmp(before, after) == 0, "the unreadable file is byte-identical");

    mm_store_shutdown();
    DeleteFileA(path);
}

/* ---- main ---------------------------------------------------------------- */

int main(void)
{
    char temp[MAX_PATH];

    GetTempPathA(sizeof(temp), temp);
    _snprintf(g_test_dir, sizeof(g_test_dir), "%sosmw_markers_test_%lu",
              temp, GetCurrentProcessId());
    CreateDirectoryA(g_test_dir, NULL);
    printf("markers fixture in %s\n", g_test_dir);

    printf("\nformat:\n");
    test_roundtrip();
    test_bad_header();
    test_bad_records();
    test_bare_lf_and_crlf();
    test_label_too_long();

    printf("\ncaps:\n");
    test_caps();

    printf("\nfile:\n");
    test_store_file();
    test_store_refuses_bad_file();

    {
        char path[MAX_PATH];
        _snprintf(path, sizeof(path), "%s\\%s", g_test_dir, MM_STORE_FILE);
        DeleteFileA(path);
        _snprintf(path, sizeof(path), "%s\\%s", g_test_dir, MM_STORE_TEMP);
        DeleteFileA(path);
        RemoveDirectoryA(g_test_dir);
    }

    printf("\n%s\n", g_fail ? "FAILED" : "all marker tests passed");
    return g_fail ? 1 : 0;
}
