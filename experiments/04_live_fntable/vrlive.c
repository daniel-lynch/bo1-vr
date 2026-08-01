/* experiments/04_live_fntable/vrlive.c
 *
 * EXPERIMENT 4 -- THE GATING EXPERIMENT
 *
 * From a 32-bit mingw-built DLL loaded inside a Proton prefix:
 *   1. VR_InitInternal2(&err, VRApplication_Scene, "")
 *   2. VR_GetGenericInterface("FnTable:IVRCompositor_029", &err)
 *   3. get a NON-NULL table and CALL THROUGH IT.
 *
 * A NULL table plus a plausible error code is NOT a pass. The pass condition is
 * a non-NULL pointer plus at least one successfully invoked __stdcall method
 * whose observable effect proves the call really happened -- here
 * SetTrackingSpace() followed by GetTrackingSpace() returning the value we set.
 *
 * We log to a file as well as stderr: winedbg detaches the inferior's stdio and
 * `proton run` does not reliably forward a child's stderr to the launching
 * shell, so stderr alone is not a dependable channel (see README, Logging).
 */

#define OPENVR_API_NODLL 1

#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "openvr_capi.h"
#include "ivrsystem_023.h"

static FILE *g_log;

static void P(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr); fflush(stderr);
    if (g_log) {
        va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
        fputc('\n', g_log); fflush(g_log);
    }
}

typedef intptr_t    (__cdecl *pfn_GetGenericInterface)(const char *, EVRInitError *);
typedef intptr_t    (__cdecl *pfn_InitInternal2)(EVRInitError *, EVRApplicationType, const char *);
typedef void        (__cdecl *pfn_ShutdownInternal)(void);
typedef const char *(__cdecl *pfn_ErrSymbol)(EVRInitError);
typedef bool        (__cdecl *pfn_IsHmdPresent)(void);

static pfn_GetGenericInterface fnGGI;
static pfn_InitInternal2       fnInit2;
static pfn_ShutdownInternal    fnShutdown;
static pfn_ErrSymbol           fnErrSym;
static pfn_IsHmdPresent        fnHmdPresent;

static const char *esym(EVRInitError e) { return fnErrSym ? fnErrSym(e) : "?"; }

/* set by the caller so a fault leaves a breadcrumb in the log */
#define STEP(s) P("--- step: %s", s)

__declspec(dllexport) int vrlive_run(void)
{
    HMODULE h;
    EVRInitError err;
    intptr_t token, iface;
    struct VR_IVRCompositor_FnTable *comp;
    struct IVRSystem_023_FnTable *sys;
    int fail = 0;

    {
        const char *lp = getenv("BO1VR_LOG");
        g_log = fopen(lp ? lp : "vrlive.log", "w");
    }

    P("=== EXPERIMENT 4: live OpenVR FnTable from a 32-bit mingw DLL ===");
    P("built with GCC %s, pointer size %d, pid %lu",
      __VERSION__, (int)sizeof(void *), GetCurrentProcessId());

    STEP("LoadLibraryA(openvr_api.dll)");
    h = LoadLibraryA("openvr_api.dll");
    if (!h) { P("FAIL: LoadLibraryA err=%lu", GetLastError()); return 1; }
    P("openvr_api.dll @ %p", (void *)h);

    fnGGI        = (pfn_GetGenericInterface)(void *)GetProcAddress(h, "VR_GetGenericInterface");
    fnInit2      = (pfn_InitInternal2)      (void *)GetProcAddress(h, "VR_InitInternal2");
    fnShutdown   = (pfn_ShutdownInternal)   (void *)GetProcAddress(h, "VR_ShutdownInternal");
    fnErrSym     = (pfn_ErrSymbol)          (void *)GetProcAddress(h, "VR_GetVRInitErrorAsSymbol");
    fnHmdPresent = (pfn_IsHmdPresent)       (void *)GetProcAddress(h, "VR_IsHmdPresent");
    if (!fnGGI || !fnInit2) { P("FAIL: missing entry points"); return 2; }

    if (fnHmdPresent) P("VR_IsHmdPresent() = %d", (int)fnHmdPresent());

    STEP("VR_InitInternal2(&err, VRApplication_Scene, \"\")");
    err = (EVRInitError)0xDEADBEEF;
    token = fnInit2(&err, EVRApplicationType_VRApplication_Scene, "");
    P("token = %p, err = %d (%s)", (void *)token, (int)err, esym(err));
    if (err != EVRInitError_VRInitError_None) {
        P("FAIL: VR_InitInternal2 did not succeed");
        return 3;
    }

    STEP("VR_GetGenericInterface(\"FnTable:IVRCompositor_029\", &err)");
    err = (EVRInitError)0xDEADBEEF;
    iface = fnGGI("FnTable:" "IVRCompositor_029", &err);
    P("returned ptr = %p, err = %d (%s)", (void *)iface, (int)err, esym(err));
    if (!iface) {
        P("FAIL: FnTable is NULL -- this is the failure the whole experiment exists to rule out");
        if (fnShutdown) fnShutdown();
        return 4;
    }
    comp = (struct VR_IVRCompositor_FnTable *)iface;
    P("PASS-1: non-NULL IVRCompositor_029 FnTable");
    P("  SetTrackingSpace      = %p", (void *)comp->SetTrackingSpace);
    P("  GetTrackingSpace      = %p", (void *)comp->GetTrackingSpace);
    P("  WaitGetPoses          = %p", (void *)comp->WaitGetPoses);
    P("  Submit                = %p", (void *)comp->Submit);

    /* ---- THE MILESTONE: call through the __stdcall table --------------- */
    STEP("comp->GetTrackingSpace()");
    {
        ETrackingUniverseOrigin a, b, c;
        a = comp->GetTrackingSpace();
        P("GetTrackingSpace() = %d", (int)a);

        STEP("comp->SetTrackingSpace(Standing) then GetTrackingSpace()");
        comp->SetTrackingSpace(ETrackingUniverseOrigin_TrackingUniverseStanding);
        b = comp->GetTrackingSpace();
        P("after Set(Standing): GetTrackingSpace() = %d", (int)b);

        comp->SetTrackingSpace(ETrackingUniverseOrigin_TrackingUniverseSeated);
        c = comp->GetTrackingSpace();
        P("after Set(Seated):   GetTrackingSpace() = %d", (int)c);

        if (b == ETrackingUniverseOrigin_TrackingUniverseStanding &&
            c == ETrackingUniverseOrigin_TrackingUniverseSeated) {
            P("PASS-2: round-tripped state through two __stdcall FnTable methods");
        } else {
            P("FAIL: state did not round-trip (%d,%d) -- the calls did not take effect", (int)b, (int)c);
            fail = 1;
        }
        comp->SetTrackingSpace(a);
    }

    /* Nullary methods, to check callee stack cleanup across several calls.
     * NOTE: xrizer implements only part of IVRCompositor. Anything it left as
     * todo!() panics in Rust; the panic unwinds into a frame Wine cannot
     * dispatch ("Exception frame is not in stack limits") and kills the
     * process. GetCurrentSceneFocusProcess() and GetLastFrameRenderer() are two
     * such methods and are deliberately NOT called -- see RESULTS.md. That is
     * an xrizer coverage gap, not a fault in the chain. */
    STEP("more nullary compositor methods (stack discipline check)");
    P("GetFrameTimeRemaining()        = %f", (double)comp->GetFrameTimeRemaining());
    P("IsFullscreen()                 = %d", (int)comp->IsFullscreen());
    P("CanRenderScene()               = %d", (int)comp->CanRenderScene());
    P("ShouldAppRenderWithLowResources() = %d", (int)comp->ShouldAppRenderWithLowResources());

    /* ---- IVRSystem --------------------------------------------------
     * Ask for _023, not the _026 that third_party/openvr's header names:
     * neither Proton's vrclient nor xrizer knows anything past _023, and
     * _026's table is not a prefix of _023's. See ivrsystem_023.h. */
    STEP("VR_GetGenericInterface(\"FnTable:IVRSystem_026\", &err)  [expected NULL]");
    err = (EVRInitError)0xDEADBEEF;
    iface = fnGGI("FnTable:" "IVRSystem_026", &err);
    P("IVRSystem_026 ptr = %p, err = %d (%s)", (void *)iface, (int)err, esym(err));

    STEP("VR_GetGenericInterface(\"FnTable:IVRSystem_023\", &err)");
    err = (EVRInitError)0xDEADBEEF;
    iface = fnGGI("FnTable:" "IVRSystem_023", &err);
    P("IVRSystem_023 ptr = %p, err = %d (%s)", (void *)iface, (int)err, esym(err));
    sys = (struct IVRSystem_023_FnTable *)iface;
    if (sys) {
        uint32_t w = 0, hgt = 0;
        float l = 0, r = 0, t = 0, b = 0;

        STEP("sys->GetRecommendedRenderTargetSize(&w,&h)");
        sys->GetRecommendedRenderTargetSize(&w, &hgt);
        P("recommended render target = %u x %u", w, hgt);
        if (!w || !hgt) { P("FAIL: zero render target size"); fail = 1; }
        else P("PASS-3: real per-eye data came back from the runtime through the table");

        STEP("sys->GetProjectionRaw(Eye_Left, ...)");
        sys->GetProjectionRaw(EVREye_Eye_Left, &l, &r, &t, &b);
        P("projection raw L=%f R=%f T=%f B=%f", (double)l, (double)r, (double)t, (double)b);

        P("IsTrackedDeviceConnected(0) = %d", (int)sys->IsTrackedDeviceConnected(0));
        P("GetTrackedDeviceClass(0)    = %d", (int)sys->GetTrackedDeviceClass(0));
    } else {
        P("FAIL: no IVRSystem at all");
        fail = 1;
    }

    /* ---- WaitGetPoses: the first thing a real frame loop does --------- */
    STEP("comp->WaitGetPoses(render[64], 64, game[64], 64)");
    {
        static struct TrackedDevicePose_t render[64], game[64];
        EVRCompositorError ce = comp->WaitGetPoses(render, 64, game, 64);
        P("WaitGetPoses -> %d", (int)ce);
        if (ce == EVRCompositorError_VRCompositorError_None) {
            P("PASS-4: WaitGetPoses returned None");
            P("  hmd pose valid=%d connected=%d",
              (int)render[0].bPoseIsValid, (int)render[0].bDeviceIsConnected);
            P("  hmd m[0][3]=%f m[1][3]=%f m[2][3]=%f",
              (double)render[0].mDeviceToAbsoluteTracking.m[0][3],
              (double)render[0].mDeviceToAbsoluteTracking.m[1][3],
              (double)render[0].mDeviceToAbsoluteTracking.m[2][3]);
        } else {
            P("NOTE: WaitGetPoses non-None (%d)", (int)ce);
        }
    }

    /* ---- deliberately last: the two struct-by-value ABI questions -----
     * These are last so that if either one corrupts the stack, everything
     * above is already in the log. See README "Corrections", item A. */
    if (sys) {
        STEP("ABI: sys->GetProjectionMatrix (64-byte struct return, hidden pointer)");
        {
            struct HmdMatrix44_t m = sys->GetProjectionMatrix(EVREye_Eye_Left, 0.1f, 100.0f);
            P("proj [0][0]=%f [1][1]=%f [2][2]=%f [2][3]=%f",
              (double)m.m[0][0], (double)m.m[1][1], (double)m.m[2][2], (double)m.m[2][3]);
            P("survived the 64-byte struct return");
        }
        STEP("ABI: sys->GetEyeToHeadTransform (48-byte struct return)");
        {
            struct HmdMatrix34_t e = sys->GetEyeToHeadTransform(EVREye_Eye_Left);
            P("eye->head translation = (%f, %f, %f)",
              (double)e.m[0][3], (double)e.m[1][3], (double)e.m[2][3]);
            P("survived the 48-byte struct return");
        }
    }

    STEP("VR_ShutdownInternal()");
    if (fnShutdown) fnShutdown();
    P("=== EXPERIMENT 4 END: %s ===", fail ? "FAIL" : "PASS");

    /* GetHiddenAreaMesh is the ONE OpenVR method that returns, by value, an
     * 8-byte struct containing a pointer -- README Correction A's open
     * question. It is measured LAST and behind an opt-in because calling it
     * KILLS THE PROCESS: Proton's own wow64 thunk faults,
     *
     *   warn:vrclient:winIVRSystem_IVRSystem_023_GetHiddenAreaMesh
     *        IVRSystem_IVRSystem_023_GetHiddenAreaMesh failed, status 0xc0000005
     *   err:msvcrt:_wassert (L"!status",L"../src-vrclient/winIVRSystem.c",12973)
     *
     * i.e. the unix call itself returns STATUS_ACCESS_VIOLATION and Proton's
     * generated wrapper asserts. Our frame is intact when it happens, so this
     * is NOT the mingw-vs-MSVC struct-return divergence the research warned
     * about -- by-value struct returns per se work here: GetProjectionMatrix
     * (64 bytes) and GetEyeToHeadTransform (48 bytes) both succeeded above.
     * See RESULTS.md. Set BO1VR_ABI_HAM=1 to reproduce the fault. */
    if (sys && getenv("BO1VR_ABI_HAM")) {
        STEP("ABI: sys->GetHiddenAreaMesh (8-byte struct return -- expect a fault)");
        {
            struct HiddenAreaMesh_t ham =
                sys->GetHiddenAreaMesh(EVREye_Eye_Left, EHiddenAreaMeshType_k_eHiddenAreaMesh_Standard);
            P("HiddenAreaMesh: pVertexData=%p unTriangleCount=%u",
              (void *)ham.pVertexData, ham.unTriangleCount);
            P("survived the 8-byte struct return");
        }
    }

    if (g_log) fclose(g_log);
    return fail ? 10 : 0;
}

BOOL WINAPI DllMain(HINSTANCE i, DWORD r, LPVOID v) { (void)i;(void)r;(void)v; return TRUE; }
