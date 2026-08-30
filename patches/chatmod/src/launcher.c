/* osmw_chat_launch.exe -- start the MythWar client with the chat mod loaded.
 *
 * The chat window and its fonts are built during the client's startup init, so
 * the mod has to be in the process before that runs. Injecting into an already
 * running client is too late for anything font related, which is why this
 * launcher exists rather than an "attach" button.
 *
 * This is built as a 32-bit exe on purpose. Same-bitness means kernel32 sits
 * at the same base in this process and in the client, so LoadLibraryA can be
 * resolved locally and used as a remote thread start address -- no export
 * table parsing, and it works even though the suspended client has no module
 * list yet.
 *
 * Whether the mod actually does anything is decided by [Addons] ChatMod, which
 * the DLL itself reads. The launcher always injects and stays dumb.
 *
 * If osmw_login.dll sits next to this exe it is injected too (before chat),
 * so Maps++ can keep pointing GamePath at this launcher for both mods.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define APP_TITLE "osMW chat mod launcher"

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

/* Directory this exe lives in, which is also the game root. */
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

    /* The remote thread runs loader init before reaching LoadLibraryA, so
     * kernel32 is mapped -- at the address we resolved -- by the time it is
     * called, even though the client's main thread is still suspended. */
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
    char dir[MAX_PATH], exe[MAX_PATH], dll[MAX_PATH], login_dll[MAX_PATH];
    char cmd[MAX_PATH * 2];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;

    (void)inst; (void)prev; (void)show;

    if (!game_dir(dir, sizeof(dir))) {
        fail("Locating the game directory", GetLastError());
        return 1;
    }
    _snprintf(exe, sizeof(exe), "%s\\main.exe", dir);
    _snprintf(dll, sizeof(dll), "%s\\osmw_chat.dll", dir);
    _snprintf(login_dll, sizeof(login_dll), "%s\\osmw_login.dll", dir);
    exe[sizeof(exe) - 1] = dll[sizeof(dll) - 1] =
        login_dll[sizeof(login_dll) - 1] = '\0';

    if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxA(NULL, "main.exe is not next to this launcher.\n\n"
                    "Put osmw_chat_launch.exe in the game folder.",
                    APP_TITLE, MB_ICONERROR | MB_OK);
        return 1;
    }
    if (GetFileAttributesA(dll) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxA(NULL, "osmw_chat.dll is not next to this launcher.\n\n"
                    "Build it with patches\\chatmod\\tools\\build.py, or run\n"
                    "main.exe directly for an unmodified client.",
                    APP_TITLE, MB_ICONERROR | MB_OK);
        return 1;
    }

    /* Pass our own arguments through to the client. */
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

    if (GetFileAttributesA(login_dll) != INVALID_FILE_ATTRIBUTES &&
        !inject_dll(pi.hProcess, login_dll)) {
        DWORD err = GetLastError();
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        fail("Injecting osmw_login.dll", err);
        return 1;
    }

    if (!inject_dll(pi.hProcess, dll)) {
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
