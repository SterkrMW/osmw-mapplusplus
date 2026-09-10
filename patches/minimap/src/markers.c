/* Persistent map markers: the on-disk store.
 *
 * Spec: mw-tools/tools/client-dll/map-markers.md section 4.
 *
 * Records live in map-markers.tsv beside main.exe, keyed by base map file. The
 * file is shared by every client running from that folder, so a mutation is
 * never a write-back of a model read earlier: it takes a named mutex, reloads
 * whatever is on disk now, applies itself by stable id, and writes the result
 * through a temporary file and an atomic replace. That is what stops five
 * clients erasing each other's markers.
 *
 * A file that fails validation is never overwritten automatically. Individual
 * bad records are dropped and counted; a bad header or an unknown version
 * fails the whole load, and the caller is expected to go read-only rather than
 * truncate someone's data.
 *
 * Parsing and serialising are deliberately free of Win32 and of the core API
 * so tests/test_markers.c can compile this file directly.
 */
#include "osmw_minimap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MM_MARKER_HEADER   "OSMW-MARKERS"
#define MM_MARKER_VERSION  1
#define MM_MARKER_FILE     "map-markers.tsv"
#define MM_MARKER_TEMP     "map-markers.tsv.tmp"
#define MM_MARKER_MUTEX    "Global\\osmw_map_markers_"

/* ---- small helpers ------------------------------------------------------- */

static int mm_hex_value(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int mm_id_valid(const char *id)
{
    int i;
    for (i = 0; i < MM_MARKER_ID_LEN; i++) {
        int c = (unsigned char)id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return 0;
    }
    return id[MM_MARKER_ID_LEN] == '\0';
}

/* Percent-decode in place. Returns the decoded length, or -1 on a bad escape
 * or an embedded control character. */
static int mm_label_decode(const char *src, int src_len, char *dst, int dst_cap)
{
    int i = 0, n = 0;

    while (i < src_len) {
        int c = (unsigned char)src[i];
        if (c == '%') {
            int hi, lo;
            if (i + 2 >= src_len)
                return -1;
            hi = mm_hex_value((unsigned char)src[i + 1]);
            lo = mm_hex_value((unsigned char)src[i + 2]);
            if (hi < 0 || lo < 0)
                return -1;
            c = (hi << 4) | lo;
            i += 3;
        } else {
            i += 1;
        }
        if (c < 0x20 || c == 0x7F)
            return -1;
        if (n >= dst_cap - 1)
            return -1;
        dst[n++] = (char)c;
    }
    dst[n] = '\0';
    return n;
}

/* Percent-encode everything that would break the record grammar or the file:
 * the escape character itself, the field separator, the line separators, and
 * anything outside printable ASCII (which includes the client's high-byte
 * code page, so a localised label survives a round trip byte for byte). */
static int mm_label_encode(const char *src, char *dst, int dst_cap)
{
    int n = 0;
    const unsigned char *p = (const unsigned char *)src;

    for (; *p != '\0'; p++) {
        unsigned c = *p;
        if (c == '%' || c == '\t' || c == '\r' || c == '\n' ||
            c < 0x20 || c > 0x7E) {
            if (n + 3 >= dst_cap)
                return -1;
            dst[n++] = '%';
            dst[n++] = "0123456789ABCDEF"[(c >> 4) & 0xF];
            dst[n++] = "0123456789ABCDEF"[c & 0xF];
        } else {
            if (n + 1 >= dst_cap)
                return -1;
            dst[n++] = (char)c;
        }
    }
    dst[n] = '\0';
    return n;
}

/* strtoul with none of its leniency: the whole field must be digits. */
static int mm_field_uint(const char *s, int len, unsigned *out)
{
    unsigned value = 0;
    int i;

    if (len <= 0 || len > 10)
        return 0;
    for (i = 0; i < len; i++) {
        int c = (unsigned char)s[i];
        if (c < '0' || c > '9')
            return 0;
        value = value * 10u + (unsigned)(c - '0');
        if (value > 0x7FFFFFFFu)
            return 0;
    }
    *out = value;
    return 1;
}

/* ---- model --------------------------------------------------------------- */

int mm_markers_find(const mm_marker_set_t *set, const char *id)
{
    int i;
    if (set == NULL || id == NULL)
        return -1;
    for (i = 0; i < set->count; i++) {
        if (strcmp(set->items[i].id, id) == 0)
            return i;
    }
    return -1;
}

int mm_markers_count_on_map(const mm_marker_set_t *set, unsigned map_file)
{
    int i, n = 0;
    if (set == NULL)
        return 0;
    for (i = 0; i < set->count; i++) {
        if (set->items[i].map_file == map_file)
            n++;
    }
    return n;
}

/* Replace by id, else append. Caps are enforced here rather than at the call
 * sites so every mutation path gets them. */
int mm_markers_upsert(mm_marker_set_t *set, const mm_marker_t *m)
{
    int at;

    if (set == NULL || m == NULL || !mm_id_valid(m->id))
        return MM_MARKER_ERR_INVALID;
    if (m->kind >= MM_MARKER_KIND_COUNT)
        return MM_MARKER_ERR_INVALID;
    if ((m->flags & ~MM_MARKER_FLAG_MASK) != 0)
        return MM_MARKER_ERR_INVALID;
    if (strlen(m->label) > MM_MARKER_LABEL_MAX)
        return MM_MARKER_ERR_INVALID;
    /* Control characters are rejected on the way in, not just on the way out.
     * The parser refuses them, so a label carrying one would be written and
     * then silently dropped on the next load. */
    {
        const unsigned char *p = (const unsigned char *)m->label;
        for (; *p != '\0'; p++) {
            if (*p < 0x20 || *p == 0x7F)
                return MM_MARKER_ERR_INVALID;
        }
    }

    at = mm_markers_find(set, m->id);
    if (at >= 0) {
        /* A move between maps still has to respect the destination's cap. */
        if (set->items[at].map_file != m->map_file &&
            mm_markers_count_on_map(set, m->map_file) >= MM_MARKERS_PER_MAP)
            return MM_MARKER_ERR_MAP_FULL;
        set->items[at] = *m;
        return MM_MARKER_OK;
    }

    if (set->count >= MM_MARKERS_MAX)
        return MM_MARKER_ERR_FULL;
    if (mm_markers_count_on_map(set, m->map_file) >= MM_MARKERS_PER_MAP)
        return MM_MARKER_ERR_MAP_FULL;

    set->items[set->count++] = *m;
    return MM_MARKER_OK;
}

int mm_markers_erase(mm_marker_set_t *set, const char *id)
{
    int at = mm_markers_find(set, id);
    if (at < 0)
        return MM_MARKER_ERR_MISSING;
    if (at != set->count - 1)
        set->items[at] = set->items[set->count - 1];
    set->count--;
    return MM_MARKER_OK;
}

/* ---- parse --------------------------------------------------------------- */

static const char *mm_line_end(const char *p, const char *end)
{
    while (p < end && *p != '\n' && *p != '\r')
        p++;
    return p;
}

static int mm_split_fields(const char *line, int len,
                           const char *field[MM_MARKER_FIELDS],
                           int field_len[MM_MARKER_FIELDS])
{
    int n = 0, start = 0, i;

    for (i = 0; i <= len; i++) {
        if (i == len || line[i] == '\t') {
            if (n >= MM_MARKER_FIELDS)
                return -1;              /* too many fields */
            field[n] = line + start;
            field_len[n] = i - start;
            n++;
            start = i + 1;
        }
    }
    return n;
}

int mm_markers_parse(const char *text, int len, mm_marker_set_t *set,
                     mm_marker_stats_t *stats)
{
    const char *p = text;
    const char *end = text + len;
    const char *line_end;
    int header_len;

    if (set == NULL || text == NULL)
        return MM_MARKER_ERR_INVALID;

    set->count = 0;
    if (stats != NULL)
        memset(stats, 0, sizeof(*stats));

    /* Header: "OSMW-MARKERS\t<version>". Anything else is not our file, and
     * an unknown version is a newer writer we must not clobber. */
    line_end = mm_line_end(p, end);
    header_len = (int)(line_end - p);
    {
        const char *field[MM_MARKER_FIELDS];
        int field_len[MM_MARKER_FIELDS];
        unsigned version = 0;
        int n = mm_split_fields(p, header_len, field, field_len);

        if (n != 2)
            return MM_MARKER_ERR_HEADER;
        if (field_len[0] != (int)strlen(MM_MARKER_HEADER) ||
            memcmp(field[0], MM_MARKER_HEADER, field_len[0]) != 0)
            return MM_MARKER_ERR_HEADER;
        if (!mm_field_uint(field[1], field_len[1], &version))
            return MM_MARKER_ERR_HEADER;
        if (version != MM_MARKER_VERSION)
            return MM_MARKER_ERR_VERSION;
    }

    p = line_end;
    while (p < end) {
        const char *field[MM_MARKER_FIELDS];
        int field_len[MM_MARKER_FIELDS];
        mm_marker_t m;
        unsigned map_file = 0, x = 0, y = 0, kind = 0, flags = 0;
        int n, line_len, decoded;

        while (p < end && (*p == '\n' || *p == '\r'))
            p++;
        if (p >= end)
            break;

        line_end = mm_line_end(p, end);
        line_len = (int)(line_end - p);

        n = mm_split_fields(p, line_len, field, field_len);
        p = line_end;
        if (n != MM_MARKER_FIELDS) {
            if (stats) stats->rejected++;
            continue;
        }

        memset(&m, 0, sizeof(m));
        if (field_len[0] != MM_MARKER_ID_LEN) {
            if (stats) stats->rejected++;
            continue;
        }
        memcpy(m.id, field[0], MM_MARKER_ID_LEN);
        m.id[MM_MARKER_ID_LEN] = '\0';
        if (!mm_id_valid(m.id)) {
            if (stats) stats->rejected++;
            continue;
        }
        if (!mm_field_uint(field[1], field_len[1], &map_file) ||
            !mm_field_uint(field[2], field_len[2], &x) ||
            !mm_field_uint(field[3], field_len[3], &y) ||
            !mm_field_uint(field[4], field_len[4], &kind) ||
            !mm_field_uint(field[5], field_len[5], &flags)) {
            if (stats) stats->rejected++;
            continue;
        }
        if (map_file > 0xFFFFu || kind >= MM_MARKER_KIND_COUNT ||
            (flags & ~MM_MARKER_FLAG_MASK) != 0) {
            if (stats) stats->rejected++;
            continue;
        }

        decoded = mm_label_decode(field[6], field_len[6], m.label,
                                  (int)sizeof(m.label));
        if (decoded < 0) {
            if (stats) stats->rejected++;
            continue;
        }

        m.map_file = (unsigned short)map_file;
        m.x = x;
        m.y = y;
        m.kind = (unsigned char)kind;
        m.flags = (unsigned char)flags;

        /* Coordinates are not range checked here: the map they belong to is
         * usually not the loaded one, so its extent is unknown. The draw path
         * re-checks against [MapUnitsW]/[MapUnitsH] instead, which is also
         * what the stock banner does. */

        if (mm_markers_find(set, m.id) >= 0) {
            if (stats) stats->duplicates++;
            continue;
        }
        if (set->count >= MM_MARKERS_MAX ||
            mm_markers_count_on_map(set, m.map_file) >= MM_MARKERS_PER_MAP) {
            if (stats) stats->overflow++;
            continue;
        }
        set->items[set->count++] = m;
        if (stats) stats->accepted++;
    }

    return MM_MARKER_OK;
}

/* ---- serialise ----------------------------------------------------------- */

int mm_markers_serialise(const mm_marker_set_t *set, char *out, int cap)
{
    int used, i;

    if (set == NULL || out == NULL || cap <= 0)
        return -1;

    used = _snprintf(out, (size_t)cap, "%s\t%d\r\n",
                     MM_MARKER_HEADER, MM_MARKER_VERSION);
    if (used < 0 || used >= cap)
        return -1;

    for (i = 0; i < set->count; i++) {
        const mm_marker_t *m = &set->items[i];
        char label[MM_MARKER_LABEL_MAX * 3 + 1];
        int written;

        if (mm_label_encode(m->label, label, (int)sizeof(label)) < 0)
            return -1;

        written = _snprintf(out + used, (size_t)(cap - used),
                            "%s\t%u\t%u\t%u\t%u\t%u\t%s\r\n",
                            m->id, (unsigned)m->map_file, m->x, m->y,
                            (unsigned)m->kind, (unsigned)m->flags, label);
        if (written < 0 || written >= cap - used)
            return -1;
        used += written;
    }
    return used;
}

/* Worst case: header plus every record at its maximum encoded width. */
int mm_markers_serialised_bound(int count)
{
    return 32 + count * (MM_MARKER_ID_LEN + 6 + 11 + 11 + 3 + 3 +
                         MM_MARKER_LABEL_MAX * 3 + 8);
}

/* ---- id generation ------------------------------------------------------- */

/* Ids only have to be unique across the clients sharing one file, and they are
 * never a security boundary. Process id, a monotonic counter and the tick
 * count give that without dragging in a PRNG. */
void mm_markers_make_id(char out[MM_MARKER_ID_LEN + 1])
{
    static unsigned counter;
    unsigned hi, lo;

    counter++;
    hi = (GetCurrentProcessId() << 16) ^ (counter * 2654435761u);
    lo = GetTickCount() ^ (counter * 40503u);
    _snprintf(out, MM_MARKER_ID_LEN + 1, "%08x%08x", hi, lo);
    out[MM_MARKER_ID_LEN] = '\0';
}
