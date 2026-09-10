/* osmw_chat_launch.exe -- compatibility launcher for the managed core.
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
 * The launcher loads only osmw_core.dll. Core owns module order, gates and
 * failure handling, with chat first in its default manifest.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define APP_TITLE "osMW managed mod launcher"

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

/* Does this main.exe load the mods by itself?
 *
 * mw-tools' tools/client-patch/mod_autoload.py replaces the 7-byte CRT entry
 * prologue with a jmp into a cave that LoadLibraryA's the mod DLLs, so a
 * patched client already has them before we could inject. A second LoadLibraryA
 * would only bump the refcount -- DllMain does not run twice -- so injecting
 * anyway is harmless, but it makes the log claim something that did not happen
 * and leaks a reference.
 *
 * Probed on disk rather than in the process: we start the client suspended, and
 * a suspended process has no module list to look at yet.
 *
 * Deliberately strict. Rather than "these bytes are not stock", which any other
 * build would also satisfy, this requires the exact shape the patch writes -- a
 * rel32 jmp, two nops, and a target inside the cave that patch claims. Anything
 * else reads as "not self-loading", so an exe we do not recognise keeps the old
 * behaviour instead of quietly losing its mods.
 */
#define ENTRY_FILE_OFFSET 0x000CD360UL   /* VA 0x004CD360, .text is mapped 1:1 */
#define ENTRY_VA          0x004CD360UL
#define AUTOLOAD_CAVE_LO  0x004CED0CUL
#define AUTOLOAD_CAVE_HI  0x004CED8CUL

static int client_self_loads(const char *exe)
{
    HANDLE f;
    unsigned char got[7];
    DWORD  read = 0, target;
    LONG   rel;

    f = CreateFileA(exe, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    if (SetFilePointer(f, ENTRY_FILE_OFFSET, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER ||
        !ReadFile(f, got, sizeof(got), &read, NULL) || read != sizeof(got)) {
        CloseHandle(f);
        return 0;
    }
    CloseHandle(f);

    if (got[0] != 0xE9 || got[5] != 0x90 || got[6] != 0x90)
        return 0;
    rel = (LONG)((DWORD)got[1] | ((DWORD)got[2] << 8) |
                 ((DWORD)got[3] << 16) | ((DWORD)got[4] << 24));
    target = ENTRY_VA + 5 + (DWORD)rel;
    return target >= AUTOLOAD_CAVE_LO && target < AUTOLOAD_CAVE_HI;
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
    char dir[MAX_PATH], exe[MAX_PATH], dll[MAX_PATH];
    char cmd[MAX_PATH * 2];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    int self_load;

    (void)inst; (void)prev; (void)show;

    if (!game_dir(dir, sizeof(dir))) {
        fail("Locating the game directory", GetLastError());
        return 1;
    }
    _snprintf(exe, sizeof(exe), "%s\\main.exe", dir);
    _snprintf(dll, sizeof(dll), "%s\\osmw_core.dll", dir);
    exe[sizeof(exe) - 1] = dll[sizeof(dll) - 1] = '\0';

    if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxA(NULL, "main.exe is not next to this launcher.\n\n"
                    "Put osmw_chat_launch.exe in the game folder.",
                    APP_TITLE, MB_ICONERROR | MB_OK);
        return 1;
    }
    /* A self-loading client owns the whole question: it loads what is there and
     * ignores what is not, so neither the missing-DLL refusal below nor the
     * injection afterwards is ours to make. */
    self_load = client_self_loads(exe);

    if (!self_load && GetFileAttributesA(dll) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxA(NULL, "osmw_core.dll is not next to this launcher.\n\n"
                    "Build it with patches\\core\\tools\\build.py, or run\n"
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

    if (!self_load && !inject_dll(pi.hProcess, dll)) {
        DWORD err = GetLastError();
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        fail("Injecting osmw_core.dll", err);
        return 1;
    }

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
