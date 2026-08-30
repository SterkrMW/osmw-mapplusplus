/* osmw_login_launch.exe -- start the client with the login mod, or edit the roster.

 *   osmw_login_launch.exe           inject osmw_login.dll (and chat, if present)
 *   osmw_login_launch.exe --roster  Win32 roster editor, no game launch
 *
 * Same-bitness 32-bit so LoadLibraryA can be used as a remote thread start.
 * Whether the mod does anything is [Addons] AutoLogin; the launcher stays dumb.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define APP_TITLE "osMW login launcher"

int lg_roster_ui(const char *game_dir);

static void fail(const char *what, DWORD err)
{
    char msg[512];
    _snprintf(msg, sizeof(msg),
              "%s failed (error %lu).\n\n"
              "The client was not started. Run main.exe directly for an\n"
              "unmodified client.", what, err);
    msg[sizeof(msg) - 1] = '\0';
    MessageBoxA(NULL, msg, APP_TITLE, MB_ICONERROR | MB_OK);
}

static int game_dir(char *out, DWORD size)
{
    char *slash;
    if (!GetModuleFileNameA(NULL, out, size))
        return 0;
    slash = strrchr(out, '\\');
    if (!slash)
        return 0;
    *slash = '\0';
    return 1;
}

static int has_flag(const char *cmd, const char *flag)
{
    const char *p;
    int n;

    if (!cmd || !flag)
        return 0;
    n = (int)strlen(flag);
    for (p = cmd; *p; p++) {
        if (strncmp(p, flag, n) == 0 && (p[n] == '\0' || p[n] == ' ') &&
            (p == cmd || p[-1] == ' '))
            return 1;
    }
    return 0;
}

static int inject_dll(HANDLE proc, const char *dll)
{
    LPTHREAD_START_ROUTINE load_library;
    void  *remote;
    SIZE_T written = 0;
    HANDLE thread;
    DWORD  code = 0;

    load_library = (LPTHREAD_START_ROUTINE)(UINT_PTR)
        GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
    remote = VirtualAllocEx(proc, NULL, MAX_PATH, MEM_COMMIT | MEM_RESERVE,
                            PAGE_READWRITE);
    if (!load_library || !remote ||
        !WriteProcessMemory(proc, remote, dll, strlen(dll) + 1, &written)) {
        if (remote)
            VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        return 0;
    }

    thread = CreateRemoteThread(proc, NULL, 0, load_library, remote, 0, NULL);
    if (!thread) {
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        return 0;
    }
    WaitForSingleObject(thread, 10000);
    GetExitCodeThread(thread, &code);
    CloseHandle(thread);
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    return code != 0;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    char dir[MAX_PATH], exe[MAX_PATH], login_dll[MAX_PATH], chat_dll[MAX_PATH];
    char cmd[MAX_PATH * 2];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;

    (void)inst; (void)prev; (void)show;

    if (!game_dir(dir, sizeof(dir))) {
        fail("Locating the game directory", GetLastError());
        return 1;
    }

    if (has_flag(cmdline, "--roster"))
        return lg_roster_ui(dir);

    _snprintf(exe, sizeof(exe), "%s\\main.exe", dir);
    _snprintf(login_dll, sizeof(login_dll), "%s\\osmw_login.dll", dir);
    _snprintf(chat_dll, sizeof(chat_dll), "%s\\osmw_chat.dll", dir);
    exe[sizeof(exe) - 1] = login_dll[sizeof(login_dll) - 1] =
        chat_dll[sizeof(chat_dll) - 1] = '\0';

    if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxA(NULL, "main.exe is not next to this launcher.\n\n"
                    "Put osmw_login_launch.exe in the game folder.",
                    APP_TITLE, MB_ICONERROR | MB_OK);
        return 1;
    }
    if (GetFileAttributesA(login_dll) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxA(NULL, "osmw_login.dll is not next to this launcher.\n\n"
                    "Build it with patches\\login\\tools\\build.py, or run\n"
                    "main.exe directly for an unmodified client.",
                    APP_TITLE, MB_ICONERROR | MB_OK);
        return 1;
    }

    _snprintf(cmd, sizeof(cmd), "\"%s\" %s", exe, cmdline ? cmdline : "");
    cmd[sizeof(cmd) - 1] = '\0';

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessA(exe, cmd, NULL, NULL, FALSE, CREATE_SUSPENDED,
                        NULL, dir, &si, &pi)) {
        fail("Starting main.exe", GetLastError());
        return 1;
    }

    if (!inject_dll(pi.hProcess, login_dll)) {
        DWORD err = GetLastError();
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        fail("Injecting osmw_login.dll", err);
        return 1;
    }

    if (GetFileAttributesA(chat_dll) != INVALID_FILE_ATTRIBUTES &&
        !inject_dll(pi.hProcess, chat_dll)) {
        DWORD err = GetLastError();
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        fail("Injecting osmw_chat.dll", err);
        return 1;
    }

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
