/* Fill the stock login widgets, submit, then pick a roster character.
 *
 * Login tick is hooked at the `cmp [NextFlag], ebx` after the input pump
 * (0x401A62 on the reference build). The control that sets NextFlag=1 and
 * Result=-2 is leave/back: the parent returns to the start task. Credential
 * submit is password vtable+0xA0 (0x401350): it copies the account, the
 * CUtlString at widget+0x138, send4(0x00020000), Result=0.
 *
 * Password characters go through vtable+0x7C (0x4023D0), not the shared
 * +0x18 glyph insert. +0x18 leaves password +0xD8 at 0; +0x7C stores the
 * real char in the CUtlString and draws '*'. 0x401350 refuses an empty
 * glyph buffer ("Please enter the correct username and password").
 *
 * Character select sends 0x00030008. The send helper is resolved from the
 * relative call after `push 0x00030008`.
 *
 * Passwords are never written to the log.
 */
#include "osmw_login.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHAR_STRIDE  0x1C
#define LG_SLOT_MAX  5
#define WIDGET_PAIR  0x150

typedef int (__cdecl *lg_send4_fn)(unsigned op, unsigned a, unsigned b, unsigned c);

static lg_hook_t      g_login_hook;
static lg_hook_t      g_char_hook;
static void          *g_login_tramp;
static void          *g_char_tramp;
static lg_send4_fn    g_send4;
static void          *g_pw_append; /* CUtlString append, resolved from +0x7C */

static lg_roster_t    g_roster;
static lg_row_t      *g_row;
static char           g_claimed_id[LG_ID_MAX];
static char           g_account[LG_ACC_MAX];
static char           g_password[64];
static char           g_character[LG_CHAR_MAX];

static volatile LONG  g_filled;
static volatile LONG  g_selected;
static volatile LONG  g_select_logged;

static void *as_widget(void *p, unsigned vt)
{
    if (!p || IsBadReadPtr(p, 4))
        return NULL;
    if (*(unsigned *)p == vt)
        return p;
    return NULL;
}

static int find_widgets(void **account, void **password)
{
    unsigned acc_vt, pw_vt;
    unsigned char *focus;

    *account = *password = NULL;
    if (!lg_addr[LG_FOCUS] || !lg_addr[LG_VTACCOUNT] || !lg_addr[LG_VTPASSWORD])
        return 0;

    acc_vt = (DWORD)(UINT_PTR)lg_addr[LG_VTACCOUNT];
    pw_vt  = (DWORD)(UINT_PTR)lg_addr[LG_VTPASSWORD];
    if (IsBadReadPtr(lg_addr[LG_FOCUS], 4))
        return 0;
    focus = *(unsigned char **)lg_addr[LG_FOCUS];

    *account  = as_widget(focus, acc_vt);
    *password = as_widget(focus, pw_vt);
    if (!*account)
        *account = as_widget(focus - WIDGET_PAIR, acc_vt);
    if (!*account)
        *account = as_widget(focus + WIDGET_PAIR, acc_vt);
    if (!*password)
        *password = as_widget(focus - WIDGET_PAIR, pw_vt);
    if (!*password)
        *password = as_widget(focus + WIDGET_PAIR, pw_vt);
    return *account && *password;
}

static int dword_at(void *p, int off)
{
    char *c = (char *)p + off;
    if (!p || IsBadReadPtr(c, 4))
        return -1;
    return *(int *)c;
}

/* 0x47B740 writes the char to [ime_index + 0x7E6890] but MultiByteToWideChar
 * always reads 0x7E6890. If the index is not 0, the new char never lands in
 * the conversion buffer and insert returns E_FAIL -- so '*' glyphs and the
 * CUtlString append are both skipped. Rewind before each password char. */
static void lg_ime_rewind(void)
{
    unsigned char *fn;
    int           *slot;

    if (!lg_addr[LG_VTACCOUNT])
        return;
    fn = ((unsigned char **)lg_addr[LG_VTACCOUNT])[0x7C / 4];
    if (!fn || IsBadReadPtr(fn, 10))
        return;
    if (fn[0] != 0x83 || fn[1] != 0xEC || fn[3] != 0x8B || fn[4] != 0x15)
        return;
    slot = *(int **)(fn + 5);
    if (!slot || IsBadWritePtr(slot, 4))
        return;
    *slot = 0;
}

static void resolve_pw_append(void *pw)
{
    void          **vt;
    unsigned char  *p;
    int             i, j;

    if (g_pw_append || !pw)
        return;
    vt = *(void ***)pw;
    if (!vt || IsBadReadPtr(vt, 0x80))
        return;
    p = (unsigned char *)vt[0x7C / 4];
    if (!p || IsBadReadPtr(p, 0x50))
        return;
    for (i = 0; i < 0x48; i++) {
        if (p[i] != 0x8D || p[i + 2] != 0x38 || p[i + 3] != 0x01 ||
            p[i + 4] != 0x00 || p[i + 5] != 0x00)
            continue;
        if (p[i + 1] != 0x8F && p[i + 1] != 0x8E)
            continue;
        for (j = 6; j < 16; j++) {
            if (p[i + j] == 0xE8) {
                g_pw_append = p + i + j + 5 + *(int *)(p + i + j + 1);
                return;
            }
        }
    }
}

/* vtable+0x18 = insert-char, __thiscall, 3 extra args (ch, 0, 0), ret 0x0C. */
#pragma optimize("", off)
static int lg_insert_char(void *widget, int glyph)
{
    void **vt;
    void  *fn;
    int    r = 0;

    if (!widget || IsBadReadPtr(widget, 4))
        return 0;
    vt = *(void ***)widget;
    if (!vt || IsBadReadPtr(vt, 0x1C))
        return 0;
    fn = vt[6];
    if (!fn)
        return 0;

    __asm {
        push 0
        push 0
        push glyph
        mov  ecx, widget
        mov  eax, fn
        call eax
        mov  r, eax
    }
    return r;
}

/* Password vtable+0x7C: thiscall, 1 stack arg, ret 4. */
static int lg_insert_pw_char(void *widget, int glyph)
{
    void **vt;
    void  *fn;
    int    r = 0;

    if (!widget || IsBadReadPtr(widget, 4))
        return 0;
    vt = *(void ***)widget;
    if (!vt || IsBadReadPtr(vt, 0x80))
        return 0;
    fn = vt[0x7C / 4];
    if (!fn)
        return 0;

    lg_ime_rewind();
    __asm {
        mov  ecx, widget
        push glyph
        mov  eax, fn
        call eax
        mov  r, eax
    }
    return r;
}

static void lg_cutl_append(void *widget, int glyph)
{
    unsigned char  ch, *pch;
    void          *cutl, *fn;
    int            index;

    if (!g_pw_append || !widget)
        return;
    ch    = (unsigned char)glyph;
    pch   = &ch;
    cutl  = (char *)widget + 0x138;
    fn    = g_pw_append;
    index = dword_at(widget, 0x144);
    if (index < 0)
        return;
    __asm {
        push pch
        push index
        mov  ecx, cutl
        mov  eax, fn
        call eax
    }
}

/* Password vtable+0xA0: thiscall, no stack args. Sends 0x00020000. */
static int lg_submit_password(void *widget)
{
    void **vt;
    void  *fn;
    int    r = 0;

    if (!widget || IsBadReadPtr(widget, 4))
        return -1;
    vt = *(void ***)widget;
    if (!vt || IsBadReadPtr(vt, 0xA4))
        return -1;
    fn = vt[0xA0 / 4];
    if (!fn)
        return -1;
    __asm {
        mov  ecx, widget
        mov  eax, fn
        call eax
        mov  r, eax
    }
    return r;
}
#pragma optimize("", on)

static void type_into(void *widget, const char *text)
{
    const unsigned char *p;
    if (!widget || !text)
        return;
    *(void **)lg_addr[LG_FOCUS] = widget;
    for (p = (const unsigned char *)text; *p; p++)
        lg_insert_char(widget, *p);
}

static void type_password(void *widget, const char *text)
{
    const unsigned char *p;
    if (!widget || !text)
        return;
    *(void **)lg_addr[LG_FOCUS] = widget;
    for (p = (const unsigned char *)text; *p; p++)
        lg_insert_pw_char(widget, *p);
}

#pragma optimize("", off)
static void lg_try_fill(void)
{
    void *acc, *pw;
    int   hr, cutl, i;

    if (g_filled)
        return;
    if (!g_account[0] || !g_password[0])
        return;
    if (!find_widgets(&acc, &pw))
        return;

    type_into(acc, g_account);
    resolve_pw_append(pw);
    type_password(pw, g_password);
    cutl = dword_at(pw, 0x144);
    if (cutl == 0 && g_pw_append) {
        for (i = 0; g_password[i]; i++)
            lg_cutl_append(pw, (unsigned char)g_password[i]);
    }
    hr = lg_submit_password(pw);
    if (hr == 0)
        lg_log("login: submitted account '%s'", g_account);
    else
        lg_log("login: submit failed hr=0x%08X account '%s'",
               (unsigned)hr, g_account);
    SecureZeroMemory(g_password, sizeof(g_password));
    InterlockedExchange(&g_filled, 1);
}

static int match_row(int slot, const unsigned char *rec)
{
    unsigned id0, id8;
    const char *c = g_character;
    char *end;
    unsigned long v;
    char buf[CHAR_STRIDE + 1];

    id0 = *(unsigned *)rec;
    id8 = *(unsigned *)(rec + 8);

    if (!c[0] || (c[0] == '#' && c[1] == '\0'))
        return slot == 0;
    if (c[0] == '#')
        return atoi(c + 1) == slot + 1;

    if (c[0] >= '0' && c[0] <= '9') {
        v = strtoul(c, &end, 0);
        if (end != c && *end == '\0')
            return v == id8 || v == id0;
    }

    memcpy(buf, rec, CHAR_STRIDE);
    buf[CHAR_STRIDE] = '\0';
    return strstr(buf, c) != NULL;
}

static void lg_try_select(void)
{
    int            count, i;
    unsigned char *list, *rec;

    if (g_selected || !g_row || !g_send4)
        return;
    if (!lg_addr[LG_CHARCOUNT] || !lg_addr[LG_CHARLIST])
        return;
    if (IsBadReadPtr(lg_addr[LG_CHARCOUNT], 4) || IsBadReadPtr(lg_addr[LG_CHARLIST], 4))
        return;

    count = *(int *)lg_addr[LG_CHARCOUNT];
    list  = *(unsigned char **)lg_addr[LG_CHARLIST];
    if (count <= 0 || count > LG_SLOT_MAX || !list)
        return;
    if (IsBadReadPtr(list, count * CHAR_STRIDE))
        return;

    for (i = 0; i < count; i++) {
        rec = list + i * CHAR_STRIDE;
        if (!match_row(i, rec))
            continue;
        lg_log("select: slot %d account %s character '%s'",
               i + 1, g_account, g_character);
        g_send4(0x00030008u, *(unsigned *)rec, 0, 0);
        InterlockedExchange(&g_selected, 1);
        return;
    }

    if (!g_select_logged) {
        InterlockedExchange(&g_select_logged, 1);
        lg_log("select: no match for '%s' in %d slot(s) -- leaving the stock screen",
               g_character, count);
    }
}

static void __declspec(naked) detour_login_tick(void)
{
    __asm {
        pushad
        call lg_try_fill
        popad
        jmp dword ptr [g_login_tramp]
    }
}

static void __declspec(naked) detour_char_tick(void)
{
    __asm {
        pushad
        call lg_try_select
        popad
        jmp dword ptr [g_char_tramp]
    }
}
#pragma optimize("", on)

static int resolve_send4(void)
{
    unsigned char *p, *end, *q;

    p = lg_addr[LG_CHARSELECTNEXT];
    if (!p)
        return 0;
    end = p + 0x40;
    for (; p + 5 < end; p++) {
        if (p[0] != 0x68 || p[1] != 0x08 || p[2] != 0x00 ||
            p[3] != 0x03 || p[4] != 0x00)
            continue;
        q = p + 5;
        if (q[0] == 0xC7 && q[1] == 0x44)
            q += 8;
        if (q[0] != 0xE8)
            continue;
        g_send4 = (lg_send4_fn)(q + 5 + *(int *)(q + 1));
        return 1;
    }
    lg_log("select: could not resolve send4 from CharSelectNext %p",
           lg_addr[LG_CHARSELECTNEXT]);
    return 0;
}

static void roster_path(char *out, int cap)
{
    _snprintf(out, cap, "%s\\%s", lg_game_dir(), LG_ROSTER_NAME);
    out[cap - 1] = '\0';
}

int lg_autologin_start(void)
{
    char path[MAX_PATH];
    char want[LG_ID_MAX];
    int  i, enabled, wanted;

    memset(&g_roster, 0, sizeof(g_roster));
    g_row = NULL;
    g_claimed_id[0] = '\0';
    g_account[0] = '\0';
    g_character[0] = '\0';
    SecureZeroMemory(g_password, sizeof(g_password));

    roster_path(path, sizeof(path));
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        lg_log("roster: %s not found -- standing down", path);
        return 1;
    }
    if (!lg_roster_load(path, &g_roster)) {
        lg_log("roster: failed to load %s -- standing down", path);
        return 0;
    }

    wanted = lg_launch_row(want, sizeof(want));

    enabled = 0;
    for (i = 0; i < g_roster.count; i++) {
        if (g_roster.rows[i].enabled)
            enabled++;
    }
    lg_log("roster: %d row(s), %d enabled", g_roster.count, enabled);
    /* A named row does not need the enabled count: it says which row to take. */
    if (!enabled && !wanted) {
        lg_roster_free(&g_roster);
        lg_log("roster: nothing enabled -- standing down");
        return 1;
    }

    if (wanted) {
        /* Deliberately no fall-back to the next free row. The player asked for
         * this account; if a live client already holds it, logging into some
         * other account is a worse answer than leaving this client at the stock
         * login screen -- and the server rejects a duplicate account login
         * anyway. */
        if (!lg_claim_row_id(&g_roster, want, &g_row) || !g_row) {
            lg_log("roster: requested row %s unavailable -- standing down", want);
            lg_roster_free(&g_roster);
            g_row = NULL;
            return 1;
        }
    } else if (!lg_claim_row(&g_roster, &g_row) || !g_row) {
        lg_log("roster: no free row to claim -- standing down");
        lg_roster_free(&g_roster);
        return 1;
    }
    strncpy(g_claimed_id, g_row->id, LG_ID_MAX - 1);
    strncpy(g_account, g_row->account, LG_ACC_MAX - 1);
    strncpy(g_character, g_row->character, LG_CHAR_MAX - 1);
    if (!lg_row_password(g_row, g_password, sizeof(g_password))) {
        lg_log("roster: DPAPI unprotect failed for account '%s' -- standing down",
               g_account);
        lg_claim_release(g_claimed_id);
        g_claimed_id[0] = '\0';
        lg_roster_free(&g_roster);
        g_row = NULL;
        return 0;
    }
    lg_log("claim: account '%s' character '%s' id %s",
           g_account, g_character[0] ? g_character : "#1", g_claimed_id);

    resolve_send4();

    if (!lg_hook_install(&g_login_hook, lg_addr[LG_TICK],
                         (void *)(UINT_PTR)detour_login_tick, 6, &g_login_tramp,
                         "login tick prologue"))
        return 0;

    if (lg_addr[LG_CHARTICK] && g_send4) {
        if (!lg_hook_install(&g_char_hook, lg_addr[LG_CHARTICK],
                             (void *)(UINT_PTR)detour_char_tick, 6, &g_char_tramp,
                             "character-select tick prologue"))
            lg_log("select: char-tick hook failed -- will stop at character select");
    }
    return 1;
}

OSMW_RESULT lg_autologin_stop(void)
{
    OSMW_RESULT result;

    result = lg_hook_remove(&g_char_hook);
    if (result != OSMW_OK)
        return result;
    result = lg_hook_remove(&g_login_hook);
    if (result != OSMW_OK)
        return result;
    g_login_tramp = NULL;
    g_char_tramp  = NULL;
    g_send4       = NULL;
    g_pw_append   = NULL;
    if (g_claimed_id[0]) {
        lg_claim_release(g_claimed_id);
        lg_log("claim: released %s", g_claimed_id);
        g_claimed_id[0] = '\0';
    }
    SecureZeroMemory(g_password, sizeof(g_password));
    lg_roster_free(&g_roster);
    g_row = NULL;
    return OSMW_OK;
}
