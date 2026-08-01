/* proxy.c -- dinput8.dll proxy.
 *
 * WHY dinput8 AND NOT SOMETHING ELSE
 * ----------------------------------
 * Proton 10 and 11 ship a builtin/native DLL policy that already lists
 * dinput8.dll as prefer-native, precisely because mod and ASI loaders have
 * hijacked that name for a decade. Naming ourselves dinput8.dll therefore needs
 * zero WINEDLLOVERRIDES configuration: dropping the file next to BlackOps.exe
 * is the entire install procedure.
 *
 * If a DLL override ever does become necessary for some other module, it must
 * be spelled "name=n,b" and never bare "name=n". With a bare "n" Wine will
 * return DLL_NOT_FOUND at the moment our proxy tries to LoadLibrary the real
 * system DLL, because the override applies to the load we ourselves are
 * performing. The ",b" fallback lets the builtin satisfy that second load.
 *
 * FORWARDING STRATEGY
 * -------------------
 * We cannot use .def-file forwarders ("DirectInput8Create = dinput8.DirectInput8Create")
 * because the forward target would be our own module name -- an infinite loop.
 * So we resolve the real exports by hand out of %SystemRoot%\system32\dinput8.dll.
 * In a 32-bit process under WoW64, GetSystemDirectoryA already yields the
 * syswow64 path, so no Wow64DisableWow64FsRedirection dance is needed.
 *
 * Each thunk is a plain __stdcall C function with the correct arity. Getting the
 * arity wrong corrupts the stack on return, and on x86 stdcall that is an
 * immediate, hard-to-attribute crash -- so the prototypes below are transcribed
 * from dinput.h / objbase.h deliberately rather than being guessed.
 */

#include "proxy.h"
#include "log.h"

typedef HRESULT(WINAPI *pfnDirectInput8Create)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);
typedef HRESULT(WINAPI *pfnDllCanUnloadNow)(void);
typedef HRESULT(WINAPI *pfnDllGetClassObject)(REFCLSID, REFIID, LPVOID *);
typedef HRESULT(WINAPI *pfnDllRegisterServer)(void);
typedef HRESULT(WINAPI *pfnDllUnregisterServer)(void);

static HMODULE g_real;
static pfnDirectInput8Create   g_DirectInput8Create;
static pfnDllCanUnloadNow      g_DllCanUnloadNow;
static pfnDllGetClassObject    g_DllGetClassObject;
static pfnDllRegisterServer    g_DllRegisterServer;
static pfnDllUnregisterServer  g_DllUnregisterServer;

BOOL proxy_init(void)
{
    char path[MAX_PATH];
    UINT n;

    if (g_real)
        return TRUE;

    n = GetSystemDirectoryA(path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH - 16) {
        LOGE("proxy: GetSystemDirectoryA failed (n=%u, err=%lu)", n, GetLastError());
        return FALSE;
    }
    lstrcatA(path, "\\dinput8.dll");

    g_real = LoadLibraryA(path);
    if (!g_real) {
        /* If this ever fires with error 126 (MOD_NOT_FOUND) check for a bare
         * "n" DLL override -- that is the classic cause, not a missing file. */
        LOGE("proxy: LoadLibraryA(\"%s\") failed, err=%lu", path, GetLastError());
        return FALSE;
    }

    g_DirectInput8Create  = (pfnDirectInput8Create)  GetProcAddress(g_real, "DirectInput8Create");
    g_DllCanUnloadNow     = (pfnDllCanUnloadNow)     GetProcAddress(g_real, "DllCanUnloadNow");
    g_DllGetClassObject   = (pfnDllGetClassObject)   GetProcAddress(g_real, "DllGetClassObject");
    g_DllRegisterServer   = (pfnDllRegisterServer)   GetProcAddress(g_real, "DllRegisterServer");
    g_DllUnregisterServer = (pfnDllUnregisterServer) GetProcAddress(g_real, "DllUnregisterServer");

    LOGI("proxy: real dinput8 at %p (DirectInput8Create=%p)",
         (void *)g_real, (void *)g_DirectInput8Create);

    if (!g_DirectInput8Create)
        LOGE("proxy: DirectInput8Create missing from real dinput8 -- input will break");

    return TRUE;
}

void proxy_shutdown(void)
{
    /* Deliberately does NOT FreeLibrary. We are being unloaded at process
     * teardown; dropping the reference here risks the loader lock and buys
     * nothing. */
    g_real = NULL;
}

/* ---- forwarded exports (names come from dinput8.def) ---- */

HRESULT WINAPI DirectInput8Create(HINSTANCE hinst, DWORD version, REFIID riid,
                                  LPVOID *out, LPUNKNOWN outer)
{
    if (!proxy_init() || !g_DirectInput8Create)
        return E_FAIL;
    return g_DirectInput8Create(hinst, version, riid, out, outer);
}

HRESULT WINAPI DllCanUnloadNow(void)
{
    if (!proxy_init() || !g_DllCanUnloadNow)
        return S_FALSE; /* "do not unload" is the safe answer */
    return g_DllCanUnloadNow();
}

HRESULT WINAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID *out)
{
    if (!proxy_init() || !g_DllGetClassObject)
        return CLASS_E_CLASSNOTAVAILABLE;
    return g_DllGetClassObject(rclsid, riid, out);
}

HRESULT WINAPI DllRegisterServer(void)
{
    if (!proxy_init() || !g_DllRegisterServer)
        return E_FAIL;
    return g_DllRegisterServer();
}

HRESULT WINAPI DllUnregisterServer(void)
{
    if (!proxy_init() || !g_DllUnregisterServer)
        return E_FAIL;
    return g_DllUnregisterServer();
}
