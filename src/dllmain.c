/* dllmain.c -- entry point for the bo1-vr ASI loader (ships as dinput8.dll).
 *
 * Responsibilities, in order:
 *   1. Announce ourselves on stderr so ~/steam-42700.log proves we were loaded.
 *   2. Install the vectored exception handler before anything else can fault.
 *   3. Initialise MinHook so plugins can hook without each re-initialising.
 *   4. Load every *.asi beside us.
 * Export forwarding to the real dinput8.dll lives in proxy.c and is lazy -- it
 * happens on first call, not here, so a broken system dinput8 cannot stop the
 * loader from coming up and logging why.
 */

#include <windows.h>
#include <stdio.h>

#include "log.h"
#include "proxy.h"
#include "veh.h"
#include "asi_loader.h"

#include "MinHook.h"

static void banner(HMODULE self)
{
    char path[MAX_PATH] = {0};
    GetModuleFileNameA(self, path, MAX_PATH);

    LOGI("=====================================================");
    LOGI("bo1-vr ASI loader (dinput8.dll proxy)");
    LOGI("  built " __DATE__ " " __TIME__ " with GCC " __VERSION__);
    LOGI("  module: %s", path);
    LOGI("  pid=%lu", GetCurrentProcessId());
    LOGI("=====================================================");
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;

    switch (reason) {
    case DLL_PROCESS_ATTACH:
        /* We do NOT DisableThreadLibraryCalls: a future hook may want
         * DLL_THREAD_ATTACH to install per-thread state. It is cheap. */
        banner(inst);
        veh_install();

        {
            MH_STATUS st = MH_Initialize();
            if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
                LOGE("MinHook: MH_Initialize failed: %s", MH_StatusToString(st));
            else
                LOGI("MinHook: initialised (%s)", MH_StatusToString(st));
        }

        /* Loading plugins from inside DllMain runs LoadLibrary under the loader
         * lock. This is formally unsafe, but it is what Ultimate ASI Loader and
         * every other ASI loader does, it is what plugins expect (they want to
         * be up before the game's own init runs), and it is proven under Proton.
         * The practical hazard is a plugin whose DllMain itself blocks on
         * another thread -- that deadlocks. Keep plugin DllMains trivial. */
        asi_load_all(inst);

        LOGI("loader: init complete");
        break;

    case DLL_PROCESS_DETACH:
        /* If reserved != NULL the process is exiting and the loader will not run
         * other DLLs' cleanup; doing real work here is pointless and risky. */
        if (reserved == NULL) {
            MH_Uninitialize();
            veh_remove();
            proxy_shutdown();
        }
        break;

    default:
        break;
    }

    return TRUE;
}
