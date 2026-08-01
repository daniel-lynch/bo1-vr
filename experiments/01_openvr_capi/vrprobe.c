/* experiments/01_openvr_capi/vrprobe.c
 *
 * EXPERIMENT 1
 * Can a mingw-built 32-bit DLL call
 *     VR_GetGenericInterface("FnTable:IVRCompositor_029", &err)
 * against Valve's shipped openvr_api.dll, and get a sane answer instead of a
 * crash?
 *
 * What this proves if it passes:
 *   - the 32-bit openvr_api.dll loads under Wine/Proton at all;
 *   - the undecorated cdecl entry points resolve from mingw;
 *   - the "FnTable:" string protocol is understood by the shipped DLL;
 *   - failure without a runtime is a returned error code, not an access
 *     violation -- which is what lets us write a graceful no-HMD path.
 *
 * NOTE ON THE HEADER: openvr_capi.h cannot be included from C as-is. For
 * _WIN32 without OPENVR_API_EXPORTS/OPENVR_API_NODLL it expands S_API to
 *     extern "C" __declspec(dllimport)
 * which is a C++ construct and will not compile in C. Defining
 * OPENVR_API_NODLL suppresses the import declarations, which is what we want
 * anyway: we resolve everything through GetProcAddress so that a missing
 * openvr_api.dll is a logged error rather than a failed process load.
 */

#define OPENVR_API_NODLL 1

#include <windows.h>
#include <stdio.h>
#include <stdint.h>

#include "openvr_capi.h"

#define P(...) do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); fflush(stderr); } while (0)

/* Free functions in openvr_api.dll are __cdecl (S_API carries no calling
 * convention on win32, so the platform default applies). The FnTable members
 * are __stdcall via OPENVR_FNTABLE_CALLTYPE -- that asymmetry is deliberate in
 * Valve's design and is the whole reason the C API is safe for us. */
typedef intptr_t    (__cdecl *pfn_VR_GetGenericInterface)(const char *, EVRInitError *);
typedef intptr_t    (__cdecl *pfn_VR_InitInternal)(EVRInitError *, EVRApplicationType);
typedef void        (__cdecl *pfn_VR_ShutdownInternal)(void);
typedef bool        (__cdecl *pfn_VR_IsHmdPresent)(void);
typedef bool        (__cdecl *pfn_VR_IsRuntimeInstalled)(void);
typedef const char *(__cdecl *pfn_VR_GetVRInitErrorAsSymbol)(EVRInitError);
typedef const char *(__cdecl *pfn_VR_GetVRInitErrorAsEnglishDescription)(EVRInitError);
typedef bool        (__cdecl *pfn_VR_IsInterfaceVersionValid)(const char *);
typedef const char *(__cdecl *pfn_VR_GetRuntimePath)(void);

static pfn_VR_GetGenericInterface                 fnGetGenericInterface;
static pfn_VR_InitInternal                        fnInitInternal;
static pfn_VR_ShutdownInternal                    fnShutdownInternal;
static pfn_VR_IsHmdPresent                        fnIsHmdPresent;
static pfn_VR_IsRuntimeInstalled                  fnIsRuntimeInstalled;
static pfn_VR_GetVRInitErrorAsSymbol              fnErrSymbol;
static pfn_VR_GetVRInitErrorAsEnglishDescription  fnErrDesc;
static pfn_VR_IsInterfaceVersionValid             fnIsIfaceValid;

static const char *errsym(EVRInitError e)
{
    return fnErrSymbol ? fnErrSymbol(e) : "<no symbol fn>";
}

#define RESOLVE(var, name) do { \
        (var) = (void *)GetProcAddress(h, name); \
        P("  %-45s %s (%p)", name, (var) ? "resolved" : "*** MISSING ***", (void *)(var)); \
        if (!(var)) missing++; \
    } while (0)

__declspec(dllexport) int vrprobe_run(void)
{
    HMODULE h;
    int missing = 0;
    EVRInitError err;
    intptr_t iface;

    P("");
    P("=== EXPERIMENT 1: OpenVR C API from a mingw 32-bit DLL ===");
    P("built with GCC %s, target %s", __VERSION__, sizeof(void *) == 4 ? "i686 (32-bit)" : "x86_64");
    P("");

    /* --- step 1: load the DLL ------------------------------------------- */
    P("[1] LoadLibraryA(\"openvr_api.dll\")");
    h = LoadLibraryA("openvr_api.dll");
    if (!h) {
        P("    FAILED, err=%lu -- cannot continue", GetLastError());
        return 1;
    }
    P("    loaded at %p", (void *)h);
    P("");

    /* --- step 2: resolve exports ---------------------------------------- */
    P("[2] GetProcAddress for the C entry points");
    RESOLVE(fnGetGenericInterface, "VR_GetGenericInterface");
    RESOLVE(fnInitInternal,        "VR_InitInternal");
    RESOLVE(fnShutdownInternal,    "VR_ShutdownInternal");
    RESOLVE(fnIsHmdPresent,        "VR_IsHmdPresent");
    RESOLVE(fnIsRuntimeInstalled,  "VR_IsRuntimeInstalled");
    RESOLVE(fnErrSymbol,           "VR_GetVRInitErrorAsSymbol");
    RESOLVE(fnErrDesc,             "VR_GetVRInitErrorAsEnglishDescription");
    RESOLVE(fnIsIfaceValid,        "VR_IsInterfaceVersionValid");
    P("    %d missing", missing);
    P("");
    if (!fnGetGenericInterface) {
        P("    VR_GetGenericInterface missing -- experiment cannot proceed");
        return 2;
    }

    /* --- step 3: environment probes (must not crash without a runtime) --- */
    P("[3] environment probes");
    if (fnIsRuntimeInstalled) P("    VR_IsRuntimeInstalled() = %d", (int)fnIsRuntimeInstalled());
    if (fnIsHmdPresent)       P("    VR_IsHmdPresent()       = %d", (int)fnIsHmdPresent());
    if (fnIsIfaceValid) {
        P("    VR_IsInterfaceVersionValid(\"%s\") = %d",
          IVRCompositor_Version, (int)fnIsIfaceValid(IVRCompositor_Version));
        P("    VR_IsInterfaceVersionValid(\"%s\") = %d",
          IVRSystem_Version, (int)fnIsIfaceValid(IVRSystem_Version));
    }
    P("");

    /* --- step 4: THE call, before any VR_InitInternal -------------------- */
    P("[4] VR_GetGenericInterface(\"FnTable:%s\", &err)  [uninitialised]",
      IVRCompositor_Version);
    err = (EVRInitError)0xDEADBEEF;   /* poison, so we can tell if it was written */
    iface = fnGetGenericInterface("FnTable:" "IVRCompositor_029", &err);
    P("    returned ptr = %p", (void *)iface);
    P("    err          = %d (%s)", (int)err, errsym(err));
    P("    -> survived the call without faulting");
    P("");

    /* --- step 5: after attempting init ----------------------------------- */
    P("[5] VR_InitInternal(&err, VRApplication_Background), then retry");
    if (fnInitInternal) {
        intptr_t token;
        err = (EVRInitError)0xDEADBEEF;
        token = fnInitInternal(&err, EVRApplicationType_VRApplication_Background);
        P("    VR_InitInternal token = %p", (void *)token);
        P("    err = %d (%s)", (int)err, errsym(err));
        if (fnErrDesc) P("    desc: %s", fnErrDesc(err));

        err = (EVRInitError)0xDEADBEEF;
        iface = fnGetGenericInterface("FnTable:" "IVRCompositor_029", &err);
        P("    retry FnTable:IVRCompositor_029 -> ptr=%p err=%d (%s)",
          (void *)iface, (int)err, errsym(err));

        if (iface) {
            /* If we ever get here we have a live runtime. Read one field of the
             * table so we prove the __stdcall layout is walkable, but do NOT
             * call into the compositor -- that needs a real session. */
            struct VR_IVRCompositor_FnTable *t = (struct VR_IVRCompositor_FnTable *)iface;
            P("    FnTable non-NULL. first slots:");
            P("      SetTrackingSpace   = %p", (void *)t->SetTrackingSpace);
            P("      WaitGetPoses       = %p", (void *)t->WaitGetPoses);
            P("      Submit             = %p", (void *)t->Submit);
        } else {
            P("    FnTable NULL, as expected without a running compositor");
        }

        if (fnShutdownInternal) {
            fnShutdownInternal();
            P("    VR_ShutdownInternal() returned cleanly");
        }
    }
    P("");
    P("=== EXPERIMENT 1 END (reached the end without crashing) ===");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE i, DWORD r, LPVOID v) { (void)i;(void)r;(void)v; return TRUE; }
