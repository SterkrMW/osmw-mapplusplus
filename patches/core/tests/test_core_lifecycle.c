#include <stdio.h>
#include <windows.h>

#define LOAD_CYCLES 25

typedef DWORD (WINAPI *OSMW_PREPARE_UNLOAD_FN)(LPVOID unused);

static OSMW_PREPARE_UNLOAD_FN find_prepare(HMODULE module)
{
    FARPROC raw = GetProcAddress(module, "osmw_prepare_unload");
    OSMW_PREPARE_UNLOAD_FN typed = NULL;

    /* Function-pointer casts are intentionally isolated so /W4 remains clean
     * in both old and current MSVC x86 toolchains. */
    if (raw != NULL) {
        *(FARPROC *)&typed = raw;
    }
    return typed;
}

int main(int argc, char **argv)
{
    int cycle;

    if (argc != 2) {
        fprintf(stderr, "usage: test_core_lifecycle path-to-osmw_core.dll\n");
        return 2;
    }

    for (cycle = 0; cycle < LOAD_CYCLES; ++cycle) {
        HMODULE module = LoadLibraryA(argv[1]);
        OSMW_PREPARE_UNLOAD_FN prepare;
        DWORD result;

        if (module == NULL) {
            fprintf(stderr, "cycle %d: LoadLibraryA failed: %lu\n",
                    cycle, (unsigned long)GetLastError());
            return 3;
        }

        prepare = find_prepare(module);
        if (prepare == NULL) {
            fprintf(stderr, "cycle %d: exact unload export missing\n", cycle);
            FreeLibrary(module);
            return 4;
        }

        result = prepare(NULL);
        if (result != 0) {
            fprintf(stderr, "cycle %d: prepare_unload returned %lu\n",
                    cycle, (unsigned long)result);
            /* The contract forbids FreeLibrary after refusal. */
            return 5;
        }
        result = prepare(NULL);
        if (result != 0) {
            fprintf(stderr,
                    "cycle %d: repeated prepare_unload returned %lu\n",
                    cycle, (unsigned long)result);
            return 6;
        }
        if (!FreeLibrary(module)) {
            fprintf(stderr, "cycle %d: FreeLibrary failed: %lu\n",
                    cycle, (unsigned long)GetLastError());
            return 7;
        }
    }

    printf("core lifecycle fixture passed (%d load/prepare/unload cycles)\n",
           LOAD_CYCLES);
    return 0;
}
