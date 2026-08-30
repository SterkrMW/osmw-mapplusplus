/* Standalone Win32 roster editor. Passwords are write-only: an empty box on
 * save keeps the existing DPAPI blob. Never shown after load.
 */
#include "roster.h"

#include <stdio.h>
#include <string.h>

#define APP_TITLE "osMW login roster"

#define ID_LIST     100
#define ID_ACCOUNT  101
#define ID_PASSWORD 102
#define ID_CHAR     103
#define ID_ENABLED  104
#define ID_ADD      105
#define ID_APPLY    106
#define ID_DEL      107
#define ID_UP       108
#define ID_DOWN     109
#define ID_SAVE     110
#define ID_CLOSE    111

static lg_roster_t g_roster;
static char        g_path[MAX_PATH];
static HWND        g_list, g_account, g_password, g_character, g_enabled;
static int         g_loading;

static void set_font(HWND hwnd, HFONT font)
{
    HWND child;
    SendMessageA(hwnd, WM_SETFONT, (WPARAM)font, FALSE);
    child = GetWindow(hwnd, GW_CHILD);
    while (child) {
        SendMessageA(child, WM_SETFONT, (WPARAM)font, FALSE);
        child = GetWindow(child, GW_HWNDNEXT);
    }
}

static void refresh_list(int keep)
{
    int i, sel;
    char line[128];

    sel = keep;
    g_loading = 1;
    SendMessageA(g_list, LB_RESETCONTENT, 0, 0);
    for (i = 0; i < g_roster.count; i++) {
        const lg_row_t *row = &g_roster.rows[i];
        _snprintf(line, sizeof(line), "%s  %s  %s",
                  row->account,
                  row->character[0] ? row->character : "#1",
                  row->enabled ? "on" : "off");
        line[sizeof(line) - 1] = '\0';
        SendMessageA(g_list, LB_ADDSTRING, 0, (LPARAM)line);
    }
    if (sel < 0 || sel >= g_roster.count)
        sel = g_roster.count ? 0 : -1;
    if (sel >= 0)
        SendMessageA(g_list, LB_SETCURSEL, (WPARAM)sel, 0);
    g_loading = 0;
}

static int selected_index(void)
{
    int i = (int)SendMessageA(g_list, LB_GETCURSEL, 0, 0);
    if (i < 0 || i >= g_roster.count)
        return -1;
    return i;
}

static void load_fields(void)
{
    int i = selected_index();
    const lg_row_t *row;

    g_loading = 1;
    if (i < 0) {
        SetWindowTextA(g_account, "");
        SetWindowTextA(g_password, "");
        SetWindowTextA(g_character, "");
        SendMessageA(g_enabled, BM_SETCHECK, BST_UNCHECKED, 0);
        g_loading = 0;
        return;
    }
    row = &g_roster.rows[i];
    SetWindowTextA(g_account, row->account);
    SetWindowTextA(g_password, "");
    SetWindowTextA(g_character, row->character);
    SendMessageA(g_enabled, BM_SETCHECK, row->enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    g_loading = 0;
}

static int apply_fields(void)
{
    int i = selected_index();
    lg_row_t *row;
    char account[LG_ACC_MAX], character[LG_CHAR_MAX], password[64];
    unsigned char *blob;
    DWORD len;

    if (i < 0)
        return 0;
    row = &g_roster.rows[i];
    GetWindowTextA(g_account, account, sizeof(account));
    GetWindowTextA(g_character, character, sizeof(character));
    GetWindowTextA(g_password, password, sizeof(password));
    if (!account[0]) {
        MessageBoxA(NULL, "Account cannot be empty.", APP_TITLE, MB_ICONWARNING | MB_OK);
        return 0;
    }
    strncpy(row->account, account, LG_ACC_MAX - 1);
    row->account[LG_ACC_MAX - 1] = '\0';
    strncpy(row->character, character, LG_CHAR_MAX - 1);
    row->character[LG_CHAR_MAX - 1] = '\0';
    row->enabled = SendMessageA(g_enabled, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (password[0]) {
        blob = NULL;
        len = 0;
        if (!lg_protect(password, &blob, &len)) {
            SecureZeroMemory(password, sizeof(password));
            MessageBoxA(NULL, "Could not protect the password (DPAPI).",
                        APP_TITLE, MB_ICONERROR | MB_OK);
            return 0;
        }
        if (row->pw_blob)
            HeapFree(GetProcessHeap(), 0, row->pw_blob);
        row->pw_blob = blob;
        row->pw_blob_len = len;
        SetWindowTextA(g_password, "");
    }
    SecureZeroMemory(password, sizeof(password));
    refresh_list(i);
    return 1;
}

static void on_add(void)
{
    char account[LG_ACC_MAX], character[LG_CHAR_MAX], password[64];

    GetWindowTextA(g_account, account, sizeof(account));
    GetWindowTextA(g_character, character, sizeof(character));
    GetWindowTextA(g_password, password, sizeof(password));
    if (!account[0] || !password[0]) {
        SecureZeroMemory(password, sizeof(password));
        MessageBoxA(NULL, "New rows need an account and a password.",
                    APP_TITLE, MB_ICONWARNING | MB_OK);
        return;
    }
    if (!character[0])
        strncpy(character, "#1", sizeof(character) - 1);
    if (!lg_roster_add(&g_roster, account, password, character)) {
        SecureZeroMemory(password, sizeof(password));
        MessageBoxA(NULL, "Could not add the row (full roster, or DPAPI failed).",
                    APP_TITLE, MB_ICONERROR | MB_OK);
        return;
    }
    SecureZeroMemory(password, sizeof(password));
    g_roster.rows[g_roster.count - 1].enabled =
        SendMessageA(g_enabled, BM_GETCHECK, 0, 0) != BST_UNCHECKED;
    refresh_list(g_roster.count - 1);
    load_fields();
}

static void on_delete(void)
{
    int i = selected_index();
    lg_row_t *row;

    if (i < 0)
        return;
    row = &g_roster.rows[i];
    if (row->pw_blob)
        HeapFree(GetProcessHeap(), 0, row->pw_blob);
    if (i < g_roster.count - 1)
        memmove(&g_roster.rows[i], &g_roster.rows[i + 1],
                sizeof(lg_row_t) * (g_roster.count - 1 - i));
    memset(&g_roster.rows[g_roster.count - 1], 0, sizeof(lg_row_t));
    g_roster.count--;
    refresh_list(i);
    load_fields();
}

static void swap_rows(int a, int b)
{
    lg_row_t tmp;
    if (a < 0 || b < 0 || a >= g_roster.count || b >= g_roster.count)
        return;
    tmp = g_roster.rows[a];
    g_roster.rows[a] = g_roster.rows[b];
    g_roster.rows[b] = tmp;
    refresh_list(b);
    load_fields();
}

static void on_save(void)
{
    if (g_roster.count && selected_index() >= 0 && !apply_fields())
        return;
    if (!lg_roster_save(g_path, &g_roster)) {
        MessageBoxA(NULL, "Could not write login_roster.dat.",
                    APP_TITLE, MB_ICONERROR | MB_OK);
        return;
    }
    MessageBoxA(NULL, "Roster saved. Passwords stay on this Windows user.",
                APP_TITLE, MB_ICONINFORMATION | MB_OK);
}

static HWND add_static(HWND parent, const char *text, int x, int y, int w, int h)
{
    return CreateWindowExA(0, "STATIC", text, WS_CHILD | WS_VISIBLE,
                           x, y, w, h, parent, NULL, NULL, NULL);
}

static HWND add_edit(HWND parent, int id, int x, int y, int w, int h, DWORD extra)
{
    return CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | extra,
                           x, y, w, h, parent, (HMENU)(UINT_PTR)id, NULL, NULL);
}

static HWND add_btn(HWND parent, int id, const char *text, int x, int y, int w, int h)
{
    return CreateWindowExA(0, "BUTTON", text,
                           WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                           x, y, w, h, parent, (HMENU)(UINT_PTR)id, NULL, NULL);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l)
{
    switch (msg) {
    case WM_CREATE:
        add_static(hwnd, "Launch order (first free row is claimed):", 12, 10, 340, 18);
        g_list = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", "",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                                 LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                                 12, 32, 340, 200, hwnd, (HMENU)ID_LIST, NULL, NULL);
        add_btn(hwnd, ID_UP, "Up", 12, 240, 50, 24);
        add_btn(hwnd, ID_DOWN, "Down", 68, 240, 50, 24);
        add_btn(hwnd, ID_DEL, "Remove", 124, 240, 70, 24);

        add_static(hwnd, "Account", 370, 32, 170, 16);
        g_account = add_edit(hwnd, ID_ACCOUNT, 370, 50, 170, 22, 0);
        add_static(hwnd, "Password (blank = keep)", 370, 80, 170, 16);
        g_password = add_edit(hwnd, ID_PASSWORD, 370, 98, 170, 22, ES_PASSWORD);
        add_static(hwnd, "Character (#1-#5 or ID)", 370, 128, 170, 16);
        g_character = add_edit(hwnd, ID_CHAR, 370, 146, 170, 22, 0);
        g_enabled = CreateWindowExA(0, "BUTTON", "Enabled",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                    370, 176, 170, 20, hwnd, (HMENU)ID_ENABLED, NULL, NULL);
        SendMessageA(g_enabled, BM_SETCHECK, BST_CHECKED, 0);
        add_btn(hwnd, ID_ADD, "Add", 370, 206, 80, 24);
        add_btn(hwnd, ID_APPLY, "Apply", 460, 206, 80, 24);

        add_static(hwnd,
                   "Each launched client claims the next enabled row. "
                   "Same account, different #slot is the usual 5-box setup.",
                   12, 276, 530, 36);
        add_btn(hwnd, ID_SAVE, "Save", 370, 320, 80, 28);
        add_btn(hwnd, ID_CLOSE, "Close", 460, 320, 80, 28);
        refresh_list(0);
        load_fields();
        return 0;

    case WM_COMMAND:
        switch (LOWORD(w)) {
        case ID_LIST:
            if (HIWORD(w) == LBN_SELCHANGE && !g_loading)
                load_fields();
            break;
        case ID_ADD:
            on_add();
            break;
        case ID_APPLY:
            apply_fields();
            break;
        case ID_DEL:
            on_delete();
            break;
        case ID_UP: {
            int i = selected_index();
            swap_rows(i, i - 1);
            break;
        }
        case ID_DOWN: {
            int i = selected_index();
            swap_rows(i, i + 1);
            break;
        }
        case ID_SAVE:
            on_save();
            break;
        case ID_CLOSE:
            DestroyWindow(hwnd);
            break;
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, w, l);
}

int lg_roster_ui(const char *game_dir)
{
    WNDCLASSA wc;
    HWND hwnd;
    MSG msg;
    HFONT font;
    NONCLIENTMETRICSA ncm;

    if (!game_dir || !game_dir[0])
        return 1;
    _snprintf(g_path, sizeof(g_path), "%s\\%s", game_dir, LG_ROSTER_NAME);
    g_path[sizeof(g_path) - 1] = '\0';

    memset(&g_roster, 0, sizeof(g_roster));
    if (GetFileAttributesA(g_path) != INVALID_FILE_ATTRIBUTES &&
        !lg_roster_load(g_path, &g_roster)) {
        MessageBoxA(NULL, "login_roster.dat exists but could not be read.",
                    APP_TITLE, MB_ICONERROR | MB_OK);
        return 1;
    }

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "osmw_login_roster";
    if (!RegisterClassA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        MessageBoxA(NULL, "Could not register the roster window class.",
                    APP_TITLE, MB_ICONERROR | MB_OK);
        return 1;
    }

    hwnd = CreateWindowExA(0, wc.lpszClassName, APP_TITLE,
                           WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                           CW_USEDEFAULT, CW_USEDEFAULT, 568, 400,
                           NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) {
        lg_roster_free(&g_roster);
        MessageBoxA(NULL, "Could not create the roster window.",
                    APP_TITLE, MB_ICONERROR | MB_OK);
        return 1;
    }

    memset(&ncm, 0, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
        font = CreateFontIndirectA(&ncm.lfMessageFont);
    else
        font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    set_font(hwnd, font);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageA(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }
    if (font && font != GetStockObject(DEFAULT_GUI_FONT))
        DeleteObject(font);
    lg_roster_free(&g_roster);
    return 0;
}
