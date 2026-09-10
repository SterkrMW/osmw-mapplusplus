/* Reading the client's live map-character array, and the pixel/grid scales.
 *
 * All of this hangs off one idiom in the client, at 0x43A6A0, which is also
 * where the three signatures came from:
 *
 *     mov  eax, [count]        ; 0x743098
 *     test eax, eax
 *     jle  nothing_here
 *     mov  eax, [local_slot]   ; 0x702988
 *     mov  ecx, [base]         ; 0x743B30  <- a POINTER, dereferenced
 *     imul eax, eax, 0xCC      ; stride
 *     add  eax, ecx
 *
 * The load through 0x743B30 is the part worth spelling out: that address holds
 * a pointer to the array, it is not the array. Indexing from it directly reads
 * garbage. The client also reallocates the array, which is why nothing here
 * caches the base across calls.
 */
#include "osmw_minimap.h"

int mm_map_char_count(void)
{
    if (!mm_addr[MM_MAPCHARCOUNT])
        return 0;
    return *(int *)mm_addr[MM_MAPCHARCOUNT];
}

int mm_local_slot_index(void)
{
    if (!mm_addr[MM_LOCALSLOT])
        return -1;
    return *(int *)mm_addr[MM_LOCALSLOT];
}

unsigned char *mm_map_char_slot(int index)
{
    unsigned char *base;
    int count;

    if (!mm_addr[MM_MAPCHARBASE])
        return NULL;

    /* Deref every time: the array moves. */
    base = *(unsigned char **)mm_addr[MM_MAPCHARBASE];
    if (!base)
        return NULL;

    count = mm_map_char_count();
    if (index < 0 || index >= count)
        return NULL;

    return base + (size_t)index * MM_SLOT_STRIDE;
}

int mm_pixel_to_grid(int pixel_x, int pixel_y, int *grid_x, int *grid_y)
{
    int sx, sy;

    if (!mm_addr[MM_GRIDSCALEX] || !mm_addr[MM_GRIDSCALEY])
        return 0;

    sx = *(int *)mm_addr[MM_GRIDSCALEX];
    sy = *(int *)mm_addr[MM_GRIDSCALEY];
    /* The client idivs by these. Zero would fault it too, but it reaches that
     * code only after a map is loaded; we can be called before one is. */
    if (sx <= 0 || sy <= 0)
        return 0;

    if (grid_x)
        *grid_x = pixel_x / sx;
    if (grid_y)
        *grid_y = pixel_y / sy;
    return 1;
}

int mm_minimap_handle(void)
{
    if (!mm_addr[MM_HANDLE])
        return -1;
    return *(int *)mm_addr[MM_HANDLE];
}

/* Has the client actually loaded a map yet?
 *
 * Every global this mod reads lives in BSS, and the cave bootstrap loads us at
 * the CRT entry point - before a single line of client code has run. At that
 * moment they are all plain zero: not -1, not "closed", just zero.
 *
 * That matters most for the minimap handle. Its closed sentinel is -1, but the
 * client only writes that when it resets the UI handle block on map load
 * (0x45B320, and 0x46051E where `or eax,0xFFFFFFFF` feeds the store). Testing
 * `handle >= 0` before then reads 0 and looks open, which is exactly how the
 * first phase 1 run reported "minimap: opened" 60 ms in with every field zero.
 *
 * The grid scales are the honest signal: the .map loader writes them, the
 * client itself divides by them, and they are never legitimately zero once a
 * map is up.
 */
int mm_client_in_world(void)
{
    int sx, sy;

    if (!mm_addr[MM_GRIDSCALEX] || !mm_addr[MM_GRIDSCALEY])
        return 0;

    sx = *(int *)mm_addr[MM_GRIDSCALEX];
    sy = *(int *)mm_addr[MM_GRIDSCALEY];
    if (sx <= 0 || sy <= 0)
        return 0;

    /* The local player is always in the array once the map is populated, so a
     * count of zero means the load has not finished. */
    return mm_map_char_count() > 0;
}

/* The three gates OpenMinimap checks before it will create the window. Kept in
 * the same order the client tests them, so a map without minimap art reports
 * the same way here as it behaves there. */
int mm_minimap_enabled(void)
{
    if (!mm_addr[MM_ENABLETIER] || !mm_addr[MM_ENABLEFLAG])
        return 0;

    /* 0x7BAE25 is the smallmap02/03 tier byte; when it is zero the client
     * falls through to the plain enable flag at 0x7BAE24. */
    if (*(unsigned char *)mm_addr[MM_ENABLETIER])
        return 1;
    return *(unsigned char *)mm_addr[MM_ENABLEFLAG] != 0;
}

/* 0x45E6F0: look a node up by handle and copy its geometry out.
 *
 * The out-rect really is sixteen bytes. The client always passes a stack RECT;
 * we once passed a lone int, and the twelve bytes past it took out the return
 * address of the draw proc that made the call. Hence the typed parameter.
 */
int mm_node_rect(int handle, mm_rect_t *out_rect, int *out_flag)
{
    typedef int(__cdecl * geom_fn)(int handle, mm_rect_t *rect, int *flag, int list_sel);
    geom_fn geom;
    int scratch = 0;

    if (!mm_addr[MM_NODEGEOMETRY] || !out_rect)
        return -1;

    geom = (geom_fn)mm_addr[MM_NODEGEOMETRY];
    /* out_flag is also written unconditionally, so it always gets somewhere
     * real to land even when the caller does not want it. */
    return geom(handle, out_rect, out_flag ? out_flag : &scratch, 0);
}

/* 0x45E750: set a node's rect and flag in place.
 *
 * The client checks the rect pointer for NULL before copying, so passing NULL
 * updates only the flag.
 */
int mm_node_set_rect(int handle, const mm_rect_t *rect, int flag)
{
    typedef void(__cdecl * setgeom_fn)(int handle, const mm_rect_t *rect,
                                       int flag, int list_sel);
    if (!mm_addr[MM_NODESETGEOMETRY])
        return -1;
    ((setgeom_fn)mm_addr[MM_NODESETGEOMETRY])(handle, rect, flag, 0);
    return 0;
}
