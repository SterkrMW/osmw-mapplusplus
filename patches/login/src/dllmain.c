/* Entry point. Resolves signatures, claims a roster row, installs hooks, then
 * returns -- a thread parked inside this DLL is a thread whose code disappears
 * on unload.
 */
#include "osmw_login.h"

#include <stdio.h>

static HMODULE g_self;
static HANDLE  g_worker;

/* Per-launch consent, set by whoever started this process.
 *
 * [Addons] AutoLogin cannot answer "did the user ask for auto-login THIS time":
 * it is a persistent value, so whatever Maps++ last wrote there also applied to
 * a client the player started by hand afterwards, which is how running
 * osmw_chat_launch.exe on its own ended up logging in by itself. That key also
 * doubles as Maps++'s own addon-enable flag, so it cannot simply be parked at 0
 * between launches either.
 *
 * An environment variable is the natural carrier: it belongs to the process
 * tree rather than to the disk. Maps++ sets it immediately before Run() and
 * clears it after, and both launchers pass NULL for lpEnvironment in
 * CreateProcess, so main.exe inherits it through the injection chain with no
 * launcher change. A client the player starts from Explorer inherits Explorer's
 * environment, which has no such variable, and so logs in normally.
 *
 * Absent means no. Present-but-"0" also means no, so a stray leftover in a
 * shell cannot turn it on.
 */
#define LG_CONSENT_VAR "OSMW_AUTOLOGIN"

static int lg_launch_consent(void)
{
    char  buf[16];
    DWORD n = GetEnvironmentVariableA(LG_CONSENT_VAR, buf, sizeof(buf));

    /* 0 = not set (or unreadable); >= sizeof(buf) = far longer than "1", which
     * is not something we wrote, so treat it as absent rather than guessing. */
    if (n == 0 || n >= sizeof(buf))
        return 0;
    return buf[0] != '0' && buf[0] != '\0';
}

static DWORD WINAPI worker(LPVOID unused)
{
    char logpath[MAX_PATH];
    int  failed;

    (void)unused;

    lg_config_load();
    _snprintf(logpath, sizeof(logpath), "%s\\osmw_login.%lu.log",
              lg_game_dir(), (unsigned long)GetCurrentProcessId());
    logpath[sizeof(logpath) - 1] = '\0';
    lg_log_prune(lg_game_dir(), 5);
    lg_log_open(logpath);

    lg_log("osmw_login starting");

    if (!lg_cfg_int_in("Addons", "AutoLogin", 1)) {
        lg_log("[Addons] AutoLogin=0 -- standing down, client untouched");
        return 0;
    }

    /* Both gates must agree: the ini says the feature is enabled at all, the
     * environment says this particular launch asked for it. */
    if (!lg_launch_consent()) {
        lg_log("%s not set -- this client was not launched with auto-login, "
               "standing down, client untouched", LG_CONSENT_VAR);
        return 0;
    }

    failed = lg_resolve_all();
    if (failed) {
        lg_log("resolve: %d signature(s) failed -- standing down, client untouched",
               failed);
        return 0;
    }
    lg_log("resolve: all %d signatures OK", LG_SIG_COUNT);

    if (lg_timestamp != 0x45650696U) {
        lg_log("note: build 0x%08X is not the reference build 0x45650696; "
               "signatures still matched, so continuing", lg_timestamp);
    }

    if (!lg_autologin_start())
        lg_log("autologin: start failed -- client left untouched where possible");

    lg_log("startup complete");
    return 0;
}

__declspec(dllexport) DWORD WINAPI osmw_prepare_unload(LPVOID unused)
{
    (void)unused;
    if (g_worker) {
        WaitForSingleObject(g_worker, 5000);
        CloseHandle(g_worker);
        g_worker = NULL;
    }
    lg_log("prepare_unload: worker stopped, safe to FreeLibrary");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;

    switch (reason) {
    case DLL_PROCESS_ATTACH:
        g_self = inst;
        (void)g_self;
        DisableThreadLibraryCalls(inst);
        g_worker = CreateThread(NULL, 0, worker, NULL, 0, NULL);
        break;

    case DLL_PROCESS_DETACH:
        lg_autologin_stop();
        lg_log("osmw_login unloading");
        lg_log_close();
        break;
    }
    return TRUE;
}
