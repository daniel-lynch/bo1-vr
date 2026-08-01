/* winmm_shim.c -- the 40 lines that get dist/dinput8.dll into BlackOps.exe.
 *
 * WHY THIS FILE EXISTS AT ALL
 * ---------------------------
 * README Decision 2 assumes that naming the ASI loader "dinput8.dll" is enough,
 * because Proton lists that name prefer-native and games generally import it.
 * BlackOps.exe does not. Measured from its own import table -- 16 DLLs, and
 * dinput8 is not among them:
 *
 *   steam_api.dll  WINMM.dll  WSOCK32.dll  binkw32.dll  d3d9.dll  d3dx9_43.dll
 *   DSOUND.dll  KERNEL32.dll  USER32.dll  GDI32.dll  ADVAPI32.dll  SHELL32.dll
 *   ole32.dll  XINPUT1_3.dll  PSAPI.DLL  WS2_32.dll
 *
 * The delay-import directory is empty and the only DLL-name strings in the file
 * are DBGHELP/nvapi/ddraw/PunkBuster -- there is no runtime LoadLibrary of
 * dinput8 either. A dinput8.dll sitting beside BlackOps.exe is simply never
 * opened. So something the game *does* import has to bring the loader in.
 *
 * WHY WINMM AND NOT SOMETHING ELSE
 * --------------------------------
 *   - XINPUT1_3.dll and DSOUND.dll are imported by ORDINAL only (2,3,4 and
 *     11,6), so a proxy would have to pin ordinals as well as names.
 *   - d3d9.dll is only 3 exports and is tempting, but it puts us in the
 *     rendering path before we have proven anything, and DXVK is the module we
 *     would be shadowing. Save that for BAC-281 when we actually want it.
 *   - binkw32.dll and steam_api.dll live in the game install, which we do not
 *     touch.
 *   - WINMM.dll is imported by NAME, all 11 of them, is loaded at process start
 *     because it is a static import, and does nothing that can break rendering.
 *
 * This shim deliberately contains no logic of its own. It forwards the eleven
 * exports BlackOps.exe imports to the real winmm and LoadLibrary's the
 * repository's own, unmodified dist/dinput8.dll. Everything the README says
 * about the loader therefore still holds; only the way it gets loaded changed.
 *
 * FORWARDING
 * ----------
 * Same reasoning as src/proxy.c: .def forwarders cannot be used because the
 * forward target would resolve back to us. Unlike proxy.c we do not know (and
 * do not want to hand-transcribe) eleven mmsystem.h prototypes -- getting a
 * __stdcall arity wrong is an immediate stack-corrupting crash. So each export
 * is a one-instruction indirect tail jump emitted as inline asm: the arguments
 * and the callee's own "ret N" are left completely untouched, which makes the
 * thunk signature-agnostic and arity-proof.
 *
 * REQUIRES "winmm=n,b" IN WINEDLLOVERRIDES.
 * Experiment 0, Finding 2: under Wine a builtin silently wins over a native DLL
 * in the application directory unless the name is prefer-native. dinput8 is on
 * Proton's prefer-native list; winmm is not. Bare "winmm=n" would then break
 * our own LoadLibrary of the real winmm (Exp. 0, Finding 1) -- hence ",b".
 */

#include <windows.h>
#include <stdio.h>

static void shimlog(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 20, fmt, ap);   /* 20 = room for prefix + \n */
    va_end(ap);
    buf[sizeof(buf) - 20] = '\0';
    /* The prefix must be inside the buffer, not just in the fprintf: under
     * `proton run` the Windows process's stdio is swallowed entirely (measured
     * -- even the host exe's own printf never reaches the launching shell), and
     * the only channel that survives is OutputDebugStringA read back with
     * WINEDEBUG=+debugstr. Anything not in `buf` is invisible where it counts. */
    memmove(buf + 13, buf, strlen(buf) + 1);
    memcpy(buf, "[winmm-shim] ", 13);
    fprintf(stderr, "%s\n", buf);
    fflush(stderr);
    strcat(buf, "\n");
    OutputDebugStringA(buf);
}

/* One indirect tail jump per export. mingw32 prefixes C symbols with '_'. */
#define FWD(name)                                                   \
    void *g_p_##name;                                               \
    __asm__(".text\n"                                               \
            ".globl _" #name "\n"                                   \
            "_" #name ":\n"                                         \
            "\tjmp *_g_p_" #name "\n")

FWD(mixerClose);
FWD(mixerGetControlDetailsA);
FWD(mixerGetLineControlsA);
FWD(mixerGetLineInfoA);
FWD(mixerGetNumDevs);
FWD(mixerOpen);
FWD(mixerSetControlDetails);
FWD(timeBeginPeriod);
FWD(timeEndPeriod);
FWD(timeGetTime);
FWD(waveInGetNumDevs);

struct fwd_entry { const char *name; void **slot; };
static const struct fwd_entry g_fwd[] = {
    { "mixerClose",              &g_p_mixerClose              },
    { "mixerGetControlDetailsA", &g_p_mixerGetControlDetailsA },
    { "mixerGetLineControlsA",   &g_p_mixerGetLineControlsA   },
    { "mixerGetLineInfoA",       &g_p_mixerGetLineInfoA       },
    { "mixerGetNumDevs",         &g_p_mixerGetNumDevs         },
    { "mixerOpen",               &g_p_mixerOpen               },
    { "mixerSetControlDetails",  &g_p_mixerSetControlDetails  },
    { "timeBeginPeriod",         &g_p_timeBeginPeriod         },
    { "timeEndPeriod",           &g_p_timeEndPeriod           },
    { "timeGetTime",             &g_p_timeGetTime             },
    { "waveInGetNumDevs",        &g_p_waveInGetNumDevs        },
};

static int resolve_real_winmm(void)
{
    char path[MAX_PATH];
    HMODULE real;
    UINT n;
    size_t i;
    int missing = 0;

    /* In a 32-bit process GetSystemDirectoryA already yields syswow64. */
    n = GetSystemDirectoryA(path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH - 16) {
        shimlog("GetSystemDirectoryA failed (n=%u err=%lu)", n, GetLastError());
        return 0;
    }
    lstrcatA(path, "\\winmm.dll");

    real = LoadLibraryA(path);
    if (!real) {
        /* err 126 here means a bare "winmm=n" override: see the header comment. */
        shimlog("LoadLibraryA(\"%s\") FAILED err=%lu", path, GetLastError());
        return 0;
    }

    for (i = 0; i < sizeof(g_fwd) / sizeof(g_fwd[0]); i++) {
        *g_fwd[i].slot = (void *)GetProcAddress(real, g_fwd[i].name);
        if (!*g_fwd[i].slot) {
            shimlog("MISSING export %s in %s", g_fwd[i].name, path);
            missing++;
        }
    }
    shimlog("real winmm at %p, %u/%u exports resolved", (void *)real,
            (unsigned)(sizeof(g_fwd) / sizeof(g_fwd[0])) - missing,
            (unsigned)(sizeof(g_fwd) / sizeof(g_fwd[0])));
    return missing == 0;
}

static void load_the_asi_loader(HMODULE self)
{
    char path[MAX_PATH];
    char *slash;
    HMODULE m;
    DWORD n;

    n = GetModuleFileNameA(self, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        shimlog("GetModuleFileNameA failed err=%lu", GetLastError());
        return;
    }
    slash = strrchr(path, '\\');
    if (!slash) { shimlog("no backslash in \"%s\"", path); return; }
    lstrcpyA(slash + 1, "dinput8.dll");

    SetLastError(0);
    m = LoadLibraryA(path);
    if (m)
        shimlog("loaded ASI loader %s at %p", path, (void *)m);
    else
        shimlog("FAILED to load %s err=%lu", path, GetLastError());
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        shimlog("attach, pid=%lu", GetCurrentProcessId());
        /* Forwarding must be live before the game's first timeGetTime, which
         * happens as soon as the game's own entry point runs, so this cannot be
         * deferred to first call the way src/proxy.c defers dinput8's. */
        resolve_real_winmm();
        /* Nested LoadLibrary under the loader lock -- the same thing
         * asi_load_all() already does, and proven under Proton in Exp. 4/5. */
        load_the_asi_loader(inst);
    }
    return TRUE;
}
