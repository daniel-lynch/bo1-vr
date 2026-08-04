/* gameframe.c -- the game's own frames, on the compositor.
 *
 * BAC-280, second half. Exp. 5 submitted a texture WE drew; Exp. 10 got hold of
 * the device and swap chain the GAME draws with. This joins them: every frame
 * the game presents is resolved, handed to the compositor as a Vulkan image,
 * and submitted to both eyes.
 *
 * It is deliberately MONO -- the same picture to each eye. Nothing here knows
 * about the HMD pose yet; that is BAC-281, and it needs the camera hook. What
 * this proves is the pipe: game -> resolve -> VkImage -> Submit -> xrizer ->
 * monado, at the game's own frame rate, without disturbing what the game draws
 * on the monitor.
 *
 * THREE THINGS THAT SHAPE THE CODE, all measured, not assumed:
 *
 * 1. The game presents through IDirect3DSwapChain9::Present (slot 3), NOT
 *    IDirect3DDevice9::Present (slot 17). Exp. 10 hooked slot 17 successfully
 *    and counted zero frames in 50 s while the game was visibly running.
 *
 * 2. The back buffer is 4x MULTISAMPLED (Exp. 10: mult=4). A multisampled
 *    surface cannot be given to the compositor and cannot be read with
 *    GetRenderTargetData. StretchRect from a multisampled source to a
 *    single-sampled destination of the same size IS the resolve, and is the
 *    only legal way to get at those pixels.
 *
 * 3. xrizer ignores Submit's pBounds (Exp. 6). Packing both eyes into one
 *    texture and slicing with bounds silently sends the WHOLE texture to both
 *    eyes. Hence one render target per eye, always -- even here, where the two
 *    happen to contain the same thing.
 *
 * WHY Vulkan AND NOT TextureType_DirectX: Proton's vrclient does translate a
 * DirectX texture, but only via IID_IDXGIVkInteropSurface, which is DXVK's
 * D3D11 interop interface. D3D9 objects do not implement it, the QueryInterface
 * fails, and the raw IDirect3DSurface9 is forwarded untranslated for xrizer to
 * reject. Exp. 5 §4b measured this; the app must do the interop itself.
 */

#define OPENVR_API_NODLL 1
#define COBJMACROS

#include <windows.h>
#include <tlhelp32.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include "openvr_capi.h"
#include "ivrsystem_023.h"
#include "d3d9_dxvk.h"
#include "MinHook.h"

/* ---------------------------------------------------------------- logging */

static void glog(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    DWORD n;
    char path[MAX_PATH];
    HANDLE h;

    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 32, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 32] = '\0';
    memmove(buf + 12, buf, strlen(buf) + 1);
    memcpy(buf, "[gameframe] ", 12);
    strcat(buf, "\n");

    OutputDebugStringA(buf);
    /* The only channel that survives a Steam launch (Exp. 9 §4). NOTE
     * GetTempPathA here yields AppData\Local\Temp, not steamuser\Temp. */
    n = GetTempPathA(MAX_PATH - 24, path);
    if (n && n < MAX_PATH - 24) {
        lstrcatA(path, "bo1vr_gameframe.log");
        h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD w;
            WriteFile(h, buf, (DWORD)strlen(buf), &w, NULL);
            CloseHandle(h);
        }
    }
}

/* ------------------------------------------------------------ openvr glue */

typedef intptr_t (__cdecl *pfn_GGI)(const char *, EVRInitError *);
typedef intptr_t (__cdecl *pfn_Init2)(EVRInitError *, EVRApplicationType, const char *);
typedef void     (__cdecl *pfn_Shutdown)(void);

static pfn_GGI      fnGGI;
static pfn_Init2    fnInit2;
static pfn_Shutdown fnShutdown;

static struct VR_IVRCompositor_FnTable *g_comp;
static struct IVRSystem_023_FnTable    *g_sys;

/* ------------------------------------------------------------------ state */

struct eye_target {
    IDirect3DTexture9     *tex;
    IDirect3DSurface9     *surf;
    ID3D9VkInteropTexture *vktex;
    bo1vr_VkImage          image;
    bo1vr_VkEnum           layout;
    bo1vr_VkImageCreateInfo info;
    IDirect3DQuery9        *q;      /* EVENT query: has our copy actually landed? */
};

static IDirect3DDevice9   *g_dev;
static ID3D9VkInteropDevice *g_vkdev;
static bo1vr_VkInstance    g_vkinst;
static bo1vr_VkPhysicalDevice g_vkphys;
static bo1vr_VkDevice      g_vkdevice;
static bo1vr_VkQueue       g_vkqueue;
static uint32_t            g_qindex, g_qfamily;
/* TWO SETS OF EYE TEXTURES, ALTERNATING PER FRAME.
 *
 * With novr.on (submission disabled, every hook still active and the scene
 * still rendered twice per eye) the game is stable. So the rendering work is
 * innocent and the fault is in what we hand the compositor.
 *
 * The remaining suspect is this: we wrote into the SAME two textures every
 * frame. Submit is asynchronous -- the compositor may still be reading last
 * frame's image when the next StretchRect overwrites it. That is consistent
 * with everything observed: the runtime failing to acquire a swapchain image,
 * xrizer panicking on the unwrap, and the flashing seen before that.
 *
 * Alternating between two sets means nothing is overwritten until at least a
 * full frame after it was submitted. Costs four textures instead of two. */
static struct eye_target   g_eyes[2][2];   /* [eye][buffer] */
static int                 g_buf;          /* which buffer THIS frame uses */
static uint32_t            g_rw, g_rh;

static LONG g_frames;
static LONG g_submitted;
static int  g_cur_eye;              /* AER fallback: which eye THIS frame is */
static int  g_dual;                 /* 1 once camera.asi supplies both eyes */
static float g_fov[2][2];           /* what the GAME should render (crop-compensated) */
static float g_fov_hmd[2][2];       /* what the HEADSET actually wants */
static int   g_fov_ok;
static int   g_flat_logged;
static LONG  g_flat_frames;
static LONG  g_submit_fails;
static LONG  g_gate_timeouts;
static LONG  g_gate_max_spins;

/* Bisect switches, declared here because interop_init (above the definitions)
 * now consults them too. */
static int g_opts_read, g_nolock, g_notrans, g_nosubmit, g_nogate, g_nowait, g_nocap, g_novk, g_notex, g_noviq, g_noq, g_probe, g_probe2;
static int g_nowaitlock;            /* nowaitlock.on: restore the racy order */
static LONG g_waitlocks;            /* proof the lock around WaitGetPoses ran */
static volatile LONG g_gd_calls;
static volatile LONG g_gd_last;
static volatile LONG g_gd_sfalse, g_gd_sok, g_gd_other;
static void read_opts(void);
static HRESULT resolve_cropped(IDirect3DSurface9 *src, IDirect3DSurface9 *dst);
static int  opt(const char *name);
static IDirect3DSurface9 *g_probe_rt;   /* probe.on / probe2.on target */

static void (*g_set_eye)(int);      /* camera.asi's bo1vr_camera_set_eye */
static int  g_state;          /* 0 = not tried, 1 = live, -1 = failed, do not retry */
static int  g_dev_hooked;

/* No __try/__except: 32-bit mingw has no SEH and this toolchain's DWARF-2
 * unwinder cannot walk BlackOps.exe's CFI-less MSVC frames (README Decision 6).
 * The loader's vectored handler traces; the defence here is to null-check every
 * pointer and to latch g_state = -1 on any failure so a broken frame path is
 * attempted exactly once rather than every frame forever. */

/* ------------------------------------------------------------------ setup */

static int vr_init(void)
{
    HMODULE hovr;
    EVRInitError err;
    intptr_t iface;

    /* KILL SWITCH. Drop a file named novr.on next to the plugins and we do not
     * touch the compositor at all -- the hooks still load and the game still
     * renders normally to the monitor.
     *
     * This is the discriminating test for "is the freeze even ours": if it
     * still freezes with this file present, the cause is not our submission
     * path, and every hypothesis about Submit, the frame pairing and DXVK's
     * queue lock is wrong. It also means a bad build never leaves the game
     * unplayable -- no rebuild, no uninstall, just a file. */
    if (GetFileAttributesA("C:\\bo1vr\\novr.on") != INVALID_FILE_ATTRIBUTES) {
        glog("novr.on present -- VR submission DISABLED, game runs flat");
        return 0;
    }

    /* C:\bo1vr first. The bare name would be searched in the exe's directory --
     * the game install, which stays untouched (Exp. 9) -- and then the system
     * directories, where only Proton's openvr_api_DXVK.dll lives, which is a
     * different DLL that does no interop at all (README Correction). Staging
     * our own copy in the prefix's plugin directory is the only placement that
     * needs neither an install change nor an environment variable. */
    hovr = LoadLibraryA("C:\\bo1vr\\openvr_api.dll");
    if (!hovr) hovr = LoadLibraryA("openvr_api.dll");
    if (!hovr) { glog("no openvr_api.dll (err=%lu)", GetLastError()); return 0; }

    fnGGI      = (pfn_GGI)     (void *)GetProcAddress(hovr, "VR_GetGenericInterface");
    fnInit2    = (pfn_Init2)   (void *)GetProcAddress(hovr, "VR_InitInternal2");
    fnShutdown = (pfn_Shutdown)(void *)GetProcAddress(hovr, "VR_ShutdownInternal");
    if (!fnGGI || !fnInit2) { glog("openvr_api.dll lacks the entry points"); return 0; }

    err = (EVRInitError)0xDEADBEEF;
    fnInit2(&err, EVRApplicationType_VRApplication_Scene, "");
    if (err != EVRInitError_VRInitError_None) {
        glog("VR_InitInternal2 err=%d", (int)err);
        /* WHAT CAN THIS PROCESS ACTUALLY SEE?
         *
         * The game runs inside the Steam Linux Runtime pressure-vessel
         * container, whose /usr is the container's, not the host's -- so a path
         * that resolves on the bench may simply not exist here, and err=105
         * InterfaceNotFound looks identical whether the runtime is missing, the
         * OpenXR manifest is missing, or monado's socket is unreachable.
         *
         * We cannot enter the container from outside to look (bwrap refuses to
         * set up a uid map under an unprivileged shell). But we are already
         * INSIDE it, and Wine maps the container's filesystem root at Z:. So
         * ask from here. This is the only vantage point that can answer it. */
        {
            static const char *paths[] = {
                "Z:\\usr\\share\\openxr\\1\\openxr_monado.json",
                "Z:\\run\\host\\usr\\share\\openxr\\1\\openxr_monado.json",
                "Z:\\usr\\lib\\x86_64-linux-gnu\\libopenxr_monado.so",
                "Z:\\run\\host\\usr\\lib\\x86_64-linux-gnu\\libopenxr_monado.so",
                "Z:\\run\\user\\1000\\monado_comp_ipc",
                "Z:\\home\\dlynch\\.config\\openxr\\1\\active_runtime.json",
                "Z:\\home\\dlynch\\.local\\share\\bo1vr-xrizer\\bin\\vrclient.so",
                "Z:\\home\\dlynch\\.local\\share\\bo1vr-xrizer\\bin\\linux64\\vrclient.so",
            };
            size_t k;
            for (k = 0; k < sizeof(paths) / sizeof(paths[0]); k++)
                glog("  see %-62s : %s", paths[k],
                     GetFileAttributesA(paths[k]) == INVALID_FILE_ATTRIBUTES ? "NO" : "yes");
            {
                char buf[512];
                DWORD n2 = GetEnvironmentVariableA("XR_RUNTIME_JSON", buf, sizeof buf);
                glog("  XR_RUNTIME_JSON = %s", n2 ? buf : "(unset)");
                n2 = GetEnvironmentVariableA("PROTON_VR_RUNTIME", buf, sizeof buf);
                glog("  PROTON_VR_RUNTIME = %s", n2 ? buf : "(unset)");
            }
        }
        return 0;
    }

    iface = fnGGI("FnTable:" "IVRCompositor_029", &err);
    g_comp = (struct VR_IVRCompositor_FnTable *)iface;
    if (!g_comp) { glog("no IVRCompositor_029 (err=%d)", (int)err); return 0; }

    iface = fnGGI("FnTable:" "IVRSystem_023", &err);
    g_sys = (struct IVRSystem_023_FnTable *)iface;
    if (g_sys) g_sys->GetRecommendedRenderTargetSize(&g_rw, &g_rh);
    if (!g_rw || !g_rh) { g_rw = 896; g_rh = 1007; glog("no recommended size; using %ux%u", g_rw, g_rh); }

    /* THE HEADSET'S FIELD OF VIEW, not the game's.
     *
     * The game runs a ~60 degree vertical FOV (exp 13 measured tanHalfFovY =
     * 0.57735 = tan(30deg)). A headset wants roughly double that, and rendering
     * the narrow one into a wide display is exactly what "super zoomed in"
     * looks like -- the world appears magnified and head movement feels wrong
     * because the angular scale is off.
     *
     * GetProjectionRaw returns the four TANGENTS of the eye frustum, which is
     * precisely what refdef's tanHalfFov fields want. The eye frustum is
     * asymmetric (l and r differ in magnitude), and the honest fix is a sheared
     * projection matrix -- camera-hook-plan 4.2. Here we take the half-width
     * and half-height instead and let the ENGINE build its own symmetric
     * matrix from them: it is one assignment rather than a hand-built matrix
     * with a handedness convention to get wrong, it cannot desync from whatever
     * else the engine does to the projection, and it fixes the magnification,
     * which is the part that actually hurts. The residual asymmetry is a small
     * off-centre error, not a scale error. */
    if (g_sys) {
        int e;
        for (e = 0; e < 2; e++) {
            float l = 0, r = 0, t = 0, b = 0;
            g_sys->GetProjectionRaw((EVREye)e, &l, &r, &t, &b);
            /* Tangents; t/b come back negative-up in OpenVR's convention, so
             * use magnitudes and average the two sides. */
            g_fov_hmd[e][0] = (float)((fabs((double)l) + fabs((double)r)) * 0.5);
            g_fov_hmd[e][1] = (float)((fabs((double)t) + fabs((double)b)) * 0.5);
            g_fov[e][0] = g_fov_hmd[e][0];   /* until setup_crop widens them */
            g_fov[e][1] = g_fov_hmd[e][1];
            glog("eye %d projection raw l=%.4f r=%.4f t=%.4f b=%.4f -> tanHalfFov %.4f %.4f",
                 e, l, r, t, b, g_fov_hmd[e][0], g_fov_hmd[e][1]);
            if (g_fov_hmd[e][0] > 0.05f && g_fov_hmd[e][1] > 0.05f) g_fov_ok = 1;
        }
    }

    glog("OpenVR up: compositor=%p system=%p per-eye %ux%u", (void *)g_comp, (void *)g_sys, g_rw, g_rh);
    return 1;
}

static int interop_init(IDirect3DDevice9 *dev)
{
    HRESULT hr;
    int i;

    hr = IDirect3DDevice9_QueryInterface(dev, &IID_ID3D9VkInteropDevice, (void **)&g_vkdev);
    if (FAILED(hr) || !g_vkdev) {
        glog("no ID3D9VkInteropDevice (hr=0x%08lx) -- d3d9.dll is not DXVK?", (unsigned long)hr);
        return 0;
    }
    /* GetVulkanHandles yields the VkInstance too, so unlike Exp. 5 we never need
     * ID3D9VkInteropInterface off the IDirect3D9 -- one fewer object to hold. */
    g_vkdev->lpVtbl->GetVulkanHandles(g_vkdev, &g_vkinst, &g_vkphys, &g_vkdevice);
    g_vkdev->lpVtbl->GetSubmissionQueue(g_vkdev, &g_vkqueue, &g_qindex, &g_qfamily);
    glog("vk: instance=%p phys=%p device=%p queue=%p family=%u",
         (void *)g_vkinst, (void *)g_vkphys, (void *)g_vkdevice, (void *)g_vkqueue, g_qfamily);
    if (!g_vkphys || !g_vkdevice || !g_vkqueue) {
        glog("a NULL Vulkan handle came back -- a plausible-looking NULL is exactly the "
             "false pass this project has been burned by before");
        return 0;
    }

    /* notex.on splits interop_init in half. Above this line: the interop
     * device, GetVulkanHandles, GetSubmissionQueue. Below it: four
     * D3DUSAGE_RENDERTARGET textures, their ID3D9VkInteropTexture exports and
     * an event query each. novk.on showed the freeze needs SOMETHING in this
     * function; this says which half. */
    if (g_notex) { glog("notex.on: interop handles only, no eye targets"); return 1; }

    for (i = 0; i < 4; i++) {
        struct eye_target *E = &g_eyes[i >> 1][i & 1];
        /* X8R8G8B8, not A8R8G8B8: the back buffer has an alpha channel whose
         * contents are whatever the game left there, and the compositor would
         * honour it. Forcing opaque here costs nothing and cannot produce a
         * see-through eye. */
        hr = IDirect3DDevice9_CreateTexture(dev, g_rw, g_rh, 1, D3DUSAGE_RENDERTARGET,
                                            D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT,
                                            &E->tex, NULL);
        if (FAILED(hr)) { glog("CreateTexture eye %d hr=0x%08lx", i, (unsigned long)hr); return 0; }
        hr = IDirect3DTexture9_GetSurfaceLevel(E->tex, 0, &E->surf);
        if (FAILED(hr)) { glog("GetSurfaceLevel eye %d", i); return 0; }

        /* noviq.on: the D3D9 side stays -- the render target exists, capture
         * resolves into it, the event query is created and polled. Only the
         * ID3D9VkInteropTexture export and GetVulkanImageInfo are skipped, so
         * DXVK is never asked to hand out a VkImage for this resource. */
        if (g_noviq) {
            if (FAILED(IDirect3DDevice9_CreateQuery(dev, D3DQUERYTYPE_EVENT, &E->q)))
                E->q = NULL;
            glog("noviq.on: eye %d target created, no Vulkan export", i);
            continue;
        }

        hr = IDirect3DTexture9_QueryInterface(E->tex, &IID_ID3D9VkInteropTexture,
                                              (void **)&E->vktex);
        if (FAILED(hr) || !E->vktex) {
            hr = IDirect3DSurface9_QueryInterface(E->surf, &IID_ID3D9VkInteropTexture,
                                                  (void **)&E->vktex);
        }
        if (FAILED(hr) || !E->vktex) {
            glog("no ID3D9VkInteropTexture for eye %d (0x%08lx)", i, (unsigned long)hr);
            return 0;
        }

        memset(&E->info, 0, sizeof(E->info));
        E->info.sType = BO1VR_VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        hr = E->vktex->lpVtbl->GetVulkanImageInfo(E->vktex, &E->image,
                                                        &E->layout, &E->info);
        if (FAILED(hr) || !E->image) { glog("no VkImage for eye %d", i); return 0; }
        if (E->info.extent.width != g_rw || E->info.extent.height != g_rh) {
            glog("eye %d VkImage extent %ux%u != requested %ux%u -- the interop struct "
                 "layout is wrong, not just the values", i,
                 E->info.extent.width, E->info.extent.height, g_rw, g_rh);
            return 0;
        }
        /* One event query per target. D3D9's event query completes when every
         * command issued before it has been consumed by the GPU -- the D3D9
         * expression of "the work is really submitted", which is what
         * render-submit-sync-RE.md says must be verified rather than assumed. */
        if (g_noq || FAILED(IDirect3DDevice9_CreateQuery(dev, D3DQUERYTYPE_EVENT, &E->q)))
            E->q = NULL;
        glog("eye %d: VkImage=0x%016llx layout=%d %ux%u fmt=%d", i,
             (unsigned long long)E->image, (int)E->layout,
             E->info.extent.width, E->info.extent.height,
             (int)E->info.format);
    }
    return 1;
}

static void release_eyes(void)
{
    int i;
    for (i = 0; i < 4; i++) {
        struct eye_target *E = &g_eyes[i >> 1][i & 1];
        if (E->vktex) { E->vktex->lpVtbl->Release(E->vktex); }
        if (E->surf)  IDirect3DSurface9_Release(E->surf);
        if (E->q)     IDirect3DQuery9_Release(E->q);
        if (E->tex)   IDirect3DTexture9_Release(E->tex);
        memset(E, 0, sizeof(*E));
    }
}

/* ------------------------------------------------------- the frame clock */

/* WaitGetPoses, on a thread WE own.
 *
 * It has to be called by somebody: it is what makes an application the focused
 * scene app, and without it every Submit is rejected with
 * VRCompositorError_DoNotHaveFocus (101). Measured the hard way -- moving it
 * off the render thread stopped the hang and turned the headset black, with
 * "Submit -> 101" on every frame.
 *
 * But it must NOT be called on the game's render thread, because it blocks
 * until the runtime wants the next frame, and when the compositor session goes
 * away it never returns: the game's main thread parked in futex_wait_multiple
 * with the audio thread still playing.
 *
 * Both facts are satisfied by giving it a thread of its own. If the compositor
 * stalls, this thread stalls, and the game carries on rendering to the monitor.
 * The poses it collects are also exactly what the head tracking will read.
 */
static struct TrackedDevicePose_t g_rposes[64], g_gposes[64];
static HANDLE g_pose_thread;
static HANDLE g_ev_ready, g_ev_consumed;
static volatile LONG g_pose_ticks;
static LONG g_skipped;

/* WHY THESE TWO EVENTS EXIST.
 *
 * OpenXR requires a strict per-frame sequence: xrWaitFrame, then xrBeginFrame,
 * then xrEndFrame. xrizer maps WaitGetPoses onto the first and
 * Submit/PostPresentHandoff onto the last.
 *
 * The first version of this pose thread called WaitGetPoses in a bare loop at
 * its own pace -- measured at 14 ticks by the render thread's frame 1 and 26 by
 * frame 2, i.e. free-running. The two halves of the frame loop then belonged to
 * different clocks, the runtime's frame state machine desynced, and OpenXR
 * started returning ERROR_RUNTIME_FAILURE. xrizer unwraps that:
 *
 *   panicked at src/compositor.rs:1185: called `Result::unwrap()` on an `Err`
 *   value: ERROR_RUNTIME_FAILURE
 *   panicked at src/compositor.rs:1155: Failed to acquire swapchain image
 *
 * and a Rust panic here unwinds into a frame Wine cannot dispatch, so the
 * process dies -- the "xrizer has crashed and the game froze" report.
 *
 * So the calls must be 1:1. They are paired with events rather than by moving
 * WaitGetPoses back onto the render thread, because that is what caused the
 * original hang: the render thread must never block on the compositor. The
 * render thread polls g_ev_ready with a ZERO timeout -- if a frame is not
 * ready it simply does not submit this time and carries on. If the compositor
 * stalls, the pose thread stalls alone and the game keeps rendering to the
 * monitor. */

static DWORD WINAPI pose_thread(LPVOID p)
{
    (void)p;
    glog("pose thread up: WaitGetPoses here, paired 1:1 with the render thread");
    for (;;) {
        if (!g_comp) break;
        g_comp->WaitGetPoses(g_rposes, 64, g_gposes, 64);
        InterlockedIncrement(&g_pose_ticks);
        SetEvent(g_ev_ready);
        /* Wait for the render thread to consume this frame before asking for
         * another. Without this the loop free-runs and OpenXR's frame state
         * machine desyncs -- see the comment on g_ev_ready. The timeout means a
         * render thread that stops submitting (menu, stand-down) cannot wedge
         * us permanently. */
        WaitForSingleObject(g_ev_consumed, 500);
    }
    return 0;
}

/* WHERE IS THE RENDER THREAD RIGHT NOW?
 *
 * Three freezes in a row have been diagnosed after the fact from thread states
 * and third-party logs, which is slow and has produced one wrong guess already.
 * g_stage is set as the render thread moves through the frame, and a watchdog
 * thread prints it if the frame counter stops advancing. A freeze then names
 * its own location in our log instead of needing /proc archaeology.
 *
 * The prime suspect this exists to confirm or kill: submit_eye holds DXVK's
 * SUBMISSION QUEUE LOCK across IVRCompositor::Submit. If Submit blocks -- and
 * it can, because xrizer acquires and waits on a swapchain image inside it --
 * then every DXVK thread that needs to submit blocks behind us, which would
 * freeze the game exactly like this. Stage 3/4 stuck would confirm it. */
static volatile LONG g_stage;      /* 0 idle 1 capture/resolve 2 pre-submit
                                      3 in Submit eye0 4 in Submit eye1
                                      5 PostPresentHandoff */
static volatile LONG g_watch_frame;
static DWORD g_render_tid;

/* THE AUTOPSY.
 *
 * g_stage answers "where are WE when it freezes" and has now answered it four
 * times running: stage 0, idle, our code not on the stack at all. That is a
 * real result -- it is what killed the Submit, the handoff, the texture-reuse
 * and the queue-lock theories -- but it cannot say anything about where the
 * GAME is, and that is now the whole question.
 *
 * Bisecting further costs one playtest per bit, and there is no reason to pay
 * that: the freeze happens in-process, with a live watchdog thread sitting
 * right there. It can suspend every other thread and read its instruction
 * pointer directly. Four suspects become one named module and offset, in one
 * run instead of two more rounds.
 *
 * ORDER MATTERS AND IS NOT A STYLE CHOICE. Between SuspendThread and
 * ResumeThread we call nothing that can take a lock the suspended thread might
 * hold -- no logging, no GetModuleFileName (loader lock), no CRT. We copy the
 * raw register context and a slab of stack with ReadProcessMemory (a syscall,
 * lock-free against our own process) and resume immediately. Symbolisation and
 * logging happen afterwards, with every thread running again. Getting this
 * backwards would deadlock the process while diagnosing a deadlock, and the
 * result would look exactly like the bug.
 */
#define AUT_MAXT   48
#define AUT_STACK  256                   /* dwords of stack copied per thread */

struct aut_thread {
    DWORD tid;
    DWORD eip, esp, ebp;
    int   ok;
    DWORD stack[AUT_STACK];
    int   stack_n;
};

/* Name an address: which module owns it, and at what offset. Returns 0 if the
 * address is not in a committed executable page -- that filter is what makes
 * the stack scan below produce return addresses rather than noise. */
static int aut_symbolise(DWORD addr, char *out, size_t outsz)
{
    MEMORY_BASIC_INFORMATION mbi;
    char path[MAX_PATH], *base;
    DWORD prot;

    if (!addr) return 0;
    if (!VirtualQuery((LPCVOID)(uintptr_t)addr, &mbi, sizeof(mbi))) return 0;
    if (mbi.State != MEM_COMMIT || !mbi.AllocationBase) return 0;
    prot = mbi.Protect & 0xff;
    if (prot != PAGE_EXECUTE && prot != PAGE_EXECUTE_READ &&
        prot != PAGE_EXECUTE_READWRITE && prot != PAGE_EXECUTE_WRITECOPY)
        return 0;

    if (!GetModuleFileNameA((HMODULE)mbi.AllocationBase, path, sizeof(path)))
        return 0;
    base = strrchr(path, '\\');
    base = base ? base + 1 : path;
    _snprintf(out, outsz, "%s+0x%lx", base,
              (unsigned long)(addr - (DWORD)(uintptr_t)mbi.AllocationBase));
    out[outsz - 1] = '\0';
    return 1;
}


/* THE GAME'S FENCE, READ AT THE MOMENT IT IS STUCK.
 *
 * 0x6ebb40 spins on ring[(index + size - 1) % size]->GetData(D3DGETDATA_FLUSH).
 * Those three globals are all we need to see the fence from outside: how big
 * the ring is, where the game is in it, and which query object it is waiting
 * on. Pure reads -- no D3D9 calls from this thread, because the render thread
 * is inside GetData on that very object and D3D9 objects are not safe to touch
 * concurrently unless the device was created MULTITHREADED.
 *
 * A vtable pointer landing inside d3d9.dll is the check that these addresses
 * still mean what the disassembly said they mean; a plausible-looking number
 * read from the wrong place is the failure mode this project keeps hitting. */
#define GAME_RING_BASE  0x3966134
#define GAME_RING_SIZE  0x39660b4
#define GAME_RING_INDEX 0x396a4cc

/* Like aut_symbolise but for DATA addresses (a vtable is in .rdata, which is
 * not executable -- the code filter rejected it and printed "unresolved"). */
static int sym_data(DWORD addr, char *out, size_t outsz)
{
    MEMORY_BASIC_INFORMATION mbi;
    char path[MAX_PATH], *base;
    if (!addr || !VirtualQuery((LPCVOID)(uintptr_t)addr, &mbi, sizeof(mbi))) return 0;
    if (mbi.State != MEM_COMMIT || !mbi.AllocationBase) return 0;
    if (!GetModuleFileNameA((HMODULE)mbi.AllocationBase, path, sizeof(path))) return 0;
    base = strrchr(path, '\\'); base = base ? base + 1 : path;
    _snprintf(out, outsz, "%s+0x%lx", base,
              (unsigned long)(addr - (DWORD)(uintptr_t)mbi.AllocationBase));
    out[outsz - 1] = '\0';
    return 1;
}

static int addr_ok(const void *p, SIZE_T n)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || !VirtualQuery(p, &mbi, sizeof(mbi))) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    return (SIZE_T)((uintptr_t)mbi.BaseAddress + mbi.RegionSize - (uintptr_t)p) >= n;
}

static void dump_game_fence(void)
{
    DWORD size, index, waited;
    DWORD *ring;
    char sym[160];
    unsigned i;

    if (!addr_ok((void *)GAME_RING_SIZE, 4) || !addr_ok((void *)GAME_RING_INDEX, 4)) {
        glog("FENCE: ring globals not mapped -- addresses stale?");
        return;
    }
    size  = *(volatile DWORD *)GAME_RING_SIZE;
    index = *(volatile DWORD *)GAME_RING_INDEX;
    if (!size || size > 64) { glog("FENCE: implausible ring size %lu", (unsigned long)size); return; }

    waited = (index + size - 1) % size;
    glog("FENCE: GetData calls=%ld  last hr=0x%08lx  (S_FALSE=%ld S_OK=%ld other=%ld)",
         g_gd_calls, (unsigned long)(DWORD)g_gd_last, g_gd_sfalse, g_gd_sok, g_gd_other);
    glog("FENCE: ring size=%lu index=%lu -> game is polling slot %lu",
         (unsigned long)size, (unsigned long)index, (unsigned long)waited);

    ring = (DWORD *)GAME_RING_BASE;
    if (!addr_ok(ring, size * 4)) { glog("FENCE: ring array not mapped"); return; }
    for (i = 0; i < size; i++) {
        DWORD q = ring[i];
        if (!q || !addr_ok((void *)q, 4)) {
            glog("FENCE:   slot %u = %08lx%s", i, (unsigned long)q,
                 i == waited ? "   <== POLLED" : "");
            continue;
        }
        /* first dword of a COM object is its vtable */
        if (sym_data(*(DWORD *)q, sym, sizeof(sym)))
            glog("FENCE:   slot %u = %08lx vtbl->%s%s", i, (unsigned long)q, sym,
                 i == waited ? "   <== POLLED" : "");
        else
            glog("FENCE:   slot %u = %08lx vtbl=%08lx (unresolved)%s", i,
                 (unsigned long)q, (unsigned long)*(DWORD *)q,
                 i == waited ? "   <== POLLED" : "");
    }
}

/* IS THE WHOLE PIPELINE STALLED, OR ONLY THE GAME'S QUERY?
 *
 * The game is spinning on a single D3D9 EVENT query that will not retire. Two
 * very different worlds produce that: either nothing is completing on the GPU
 * at all, or that particular query is stuck while the device is otherwise
 * fine. A fresh query of our own, issued and polled right now, separates them
 * in one measurement -- and the answer points at completely different fixes.
 *
 * Behind a switch, because it calls D3D9 from the watchdog thread while the
 * render thread sits inside GetData on its own object. The game is already
 * dead when this runs, so the downside is bounded; running it in normal
 * operation would not be. */
static void probe_own_fence(void)
{
    IDirect3DQuery9 *q = NULL;
    HRESULT hr;
    int i;

    if (!g_dev) { glog("FENCE: no device to probe with"); return; }
    hr = IDirect3DDevice9_CreateQuery(g_dev, D3DQUERYTYPE_EVENT, &q);
    if (FAILED(hr) || !q) {
        glog("FENCE: our CreateQuery failed hr=0x%08lx -- the device itself is gone",
             (unsigned long)hr);
        return;
    }
    IDirect3DQuery9_Issue(q, D3DISSUE_END);
    for (i = 0; i < 3000; i++) {
        hr = IDirect3DQuery9_GetData(q, NULL, 0, D3DGETDATA_FLUSH);
        if (hr != S_FALSE) break;
        Sleep(1);
    }
    if (hr == S_OK)
        glog("FENCE: OUR fresh query RETIRED after %d ms -- the GPU and the "
             "submission path are alive; only the game's query is stuck", i);
    else
        glog("FENCE: OUR fresh query did NOT retire in %d ms (hr=0x%08lx) -- "
             "nothing is completing; the stall is below D3D9", i, (unsigned long)hr);
    IDirect3DQuery9_Release(q);
}


/* WHAT IS THE GAME'S GetData ACTUALLY RETURNING?
 *
 * The game's loop at 0x6ebb40 exits only on S_OK or D3DERR_DEVICELOST. DXVK's
 * D3D9Query::GetData has a THIRD outcome: d3d9_query.cpp returns
 * D3DERR_INVALIDCALL when the underlying vkGetEventStatus reports neither SET
 * nor RESET. That value is not in the game's exit set, so it would spin
 * forever on it -- a completely different bug from "the fence never signals",
 * with a completely different fix, and the two are indistinguishable from
 * outside.
 *
 * So hook GetData itself. The vtable belongs to DXVK and is shared by every
 * D3D9 query including our own, which is fine: we record the last HRESULT and
 * a call count, and the watchdog prints them when it fires. Cheap, on the
 * right thread, and it settles the question in one run. */
typedef HRESULT (WINAPI *pfn_getdata)(IDirect3DQuery9 *, void *, DWORD, DWORD);
static pfn_getdata real_getdata;

static HRESULT WINAPI my_getdata(IDirect3DQuery9 *q, void *data, DWORD size, DWORD flags)
{
    HRESULT hr = real_getdata(q, data, size, flags);
    InterlockedIncrement(&g_gd_calls);
    g_gd_last = (LONG)hr;
    if (hr == S_FALSE)      InterlockedIncrement(&g_gd_sfalse);
    else if (hr == S_OK)    InterlockedIncrement(&g_gd_sok);
    else                    InterlockedIncrement(&g_gd_other);
    return hr;
}

static void hook_query_getdata(void)
{
    static int done;
    DWORD size, q, *ring;
    void **vt;

    if (done) return;
    if (!addr_ok((void *)GAME_RING_SIZE, 4)) return;
    size = *(volatile DWORD *)GAME_RING_SIZE;
    if (!size || size > 64) return;
    ring = (DWORD *)GAME_RING_BASE;
    if (!addr_ok(ring, size * 4)) return;
    q = ring[0];
    if (!q || !addr_ok((void *)q, 4)) return;      /* not created yet */

    done = 1;
    vt = *(void ***)q;
    if (!addr_ok(vt, 8 * sizeof(void *))) { glog("GETDATA: query vtable unreadable"); return; }
    if (MH_CreateHook(vt[7], (void *)my_getdata, (void **)&real_getdata) == MH_OK &&
        MH_EnableHook(vt[7]) == MH_OK)
        glog("GETDATA: hooked IDirect3DQuery9::GetData (vt[7]=%p) on the game's fence",
             vt[7]);
    else
        glog("GETDATA: failed to hook vt[7]=%p", vt[7]);
}

static void freeze_autopsy(void)
{
    static struct aut_thread t[AUT_MAXT];
    HANDLE snap, th;
    THREADENTRY32 te;
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    int n = 0, i, j, shown;
    char sym[160];
    CONTEXT ctx;
    SIZE_T got;

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) { glog("AUTOPSY: no thread snapshot"); return; }

    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
            if (n >= AUT_MAXT) break;
            t[n].tid = te.th32ThreadID;
            t[n].ok = 0;
            t[n].stack_n = 0;

            th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT,
                            FALSE, te.th32ThreadID);
            if (th) {
                if (SuspendThread(th) != (DWORD)-1) {
                    memset(&ctx, 0, sizeof(ctx));
                    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                    if (GetThreadContext(th, &ctx)) {
                        t[n].eip = ctx.Eip;
                        t[n].esp = ctx.Esp;
                        t[n].ebp = ctx.Ebp;
                        t[n].ok  = 1;
                        /* CLAMP TO THE END OF THE STACK REGION.
                         *
                         * The first version read a flat 1 KB from ESP, and
                         * ReadProcessMemory fails the WHOLE call if any part of
                         * the range is unreadable. A thread parked near the top
                         * of its stack has less than 1 KB left before the guard
                         * page, so it got zero bytes and printed no frames at
                         * all -- and that is exactly the state an idle thread
                         * waiting for work is in. It silently blanked the one
                         * thread the autopsy exists to look at. */
                        {
                            MEMORY_BASIC_INFORMATION smbi;
                            SIZE_T want = sizeof(t[n].stack);
                            if (VirtualQuery((LPCVOID)(uintptr_t)ctx.Esp, &smbi,
                                             sizeof(smbi))) {
                                SIZE_T avail = (SIZE_T)((uintptr_t)smbi.BaseAddress
                                             + smbi.RegionSize - ctx.Esp);
                                if (avail < want) want = avail;
                            }
                            got = 0;
                            if (want >= sizeof(DWORD) &&
                                ReadProcessMemory(GetCurrentProcess(),
                                                  (LPCVOID)(uintptr_t)ctx.Esp,
                                                  t[n].stack, want, &got))
                                t[n].stack_n = (int)(got / sizeof(DWORD));
                        }
                    }
                    ResumeThread(th);        /* before ANY logging. see above. */
                }
                CloseHandle(th);
            }
            n++;
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);

    glog("AUTOPSY: %d other threads; render thread is %lu", n,
         (unsigned long)g_render_tid);

    for (i = 0; i < n; i++) {
        if (!t[i].ok) { glog("AUTOPSY: tid %lu -- no context", (unsigned long)t[i].tid); continue; }
        if (!aut_symbolise(t[i].eip, sym, sizeof(sym)))
            _snprintf(sym, sizeof(sym), "<unmapped>");
        glog("AUTOPSY: tid %lu%s eip=%08lx %s esp=%08lx ebp=%08lx",
             (unsigned long)t[i].tid,
             t[i].tid == g_render_tid ? " [RENDER]" : "",
             (unsigned long)t[i].eip, sym,
             (unsigned long)t[i].esp, (unsigned long)t[i].ebp);

        /* A scan, not a real unwind: the frames here are MSVC's and mingw's
         * DWARF unwinder cannot walk them (docs/, exp 13). Every stack slot
         * that points into an executable page is printed; some are stale, but
         * the callers are all in there and the module names are what matter. */
        for (j = 0, shown = 0; j < t[i].stack_n && shown < 12; j++) {
            if (aut_symbolise(t[i].stack[j], sym, sizeof(sym))) {
                glog("AUTOPSY:   [%lu] +%03d %08lx %s",
                     (unsigned long)t[i].tid, j * 4,
                     (unsigned long)t[i].stack[j], sym);
                shown++;
            }
        }
    }
    dump_game_fence();
    if (opt("fenceprobe.on")) probe_own_fence();
    glog("AUTOPSY: end");
}

static DWORD WINAPI watchdog_thread(LPVOID p)
{
    LONG last = -1, same = 0;
    int autopsied = 0;
    (void)p;
    for (;;) {
        Sleep(1000);
        if (g_watch_frame == last) {
            if (++same == 3 || same == 10 || (same % 30) == 0)
                glog("WATCHDOG: no frame for %ld s, stuck at stage %ld "
                     "(1=capture 2=pre-submit 3=Submit-eye0 4=Submit-eye1 5=handoff), "
                     "pose ticks %ld", same, g_stage, g_pose_ticks);
            /* Twice, and no more: at 5 s to catch it fresh, at 20 s to show
             * what is still stuck versus what merely happened to be idle. A
             * repeating autopsy would suspend threads every tick forever. */
            if (same == 5 || same == 20) freeze_autopsy();
            if (same == 20) autopsied = 1;
            (void)autopsied;
        } else {
            last = g_watch_frame;
            same = 0;
        }
    }
    return 0;
}

static void start_watchdog(void)
{
    static LONG started;
    DWORD tid;
    if (InterlockedExchange(&started, 1)) return;
    /* Never from DllMain: creating a thread under the loader lock is the
     * deadlock src/dllmain.c warns about. The first Present is safe. */
    CreateThread(NULL, 0, watchdog_thread, NULL, 0, &tid);
}

static void start_pose_thread(void)
{
    DWORD tid;
    if (g_pose_thread || !g_comp) return;
    g_ev_ready    = CreateEventA(NULL, FALSE, FALSE, NULL);   /* auto-reset */
    g_ev_consumed = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!g_ev_ready || !g_ev_consumed) { glog("CreateEvent failed"); return; }
    /* Started from the render thread at first frame, never from DllMain:
     * creating a thread under the loader lock is the deadlock src/dllmain.c
     * warns about. */
    /* The pose thread is gone: WaitGetPoses now runs on the render thread with
     * Submit, because splitting them across threads is what xrizer could not
     * survive. Kept as dead code rather than deleted so the reasoning above
     * stays attached to the thing it is about. */
    (void)pose_thread;
    (void)tid;             /* the watchdog now starts unconditionally, earlier */
}

/* --------------------------------------------------- dual-view capture */

static LONG g_captured;      /* eyes captured during THIS frame by camera.asi */
static int  g_rt_logged;
static RECT g_crop;
static int  g_have_crop;
static IDirect3DSurface9 *g_resolve;   /* non-MSAA intermediate, source-sized */

/* THE ASPECT FIX.
 *
 * The game renders 2560x1440 (1.778, landscape). The per-eye target is
 * 1832x2015 (0.909, portrait). Copying one into the other full-frame is a 1.96x
 * NON-UNIFORM squeeze -- circles become ellipses, and it reads as "still a bit
 * zoomed in" even after the field of view itself was corrected.
 *
 * The fix is to take a centred sub-rectangle of the source whose aspect already
 * matches the eye, so the copy is a UNIFORM scale, and to widen the FOV we ask
 * the game to render by exactly the amount the crop throws away. The player
 * then sees the headset's true field of view with correct geometry.
 *
 * Worked for this case: crop height 1440 (all of it), width 1440 * 0.909 = 1309
 * of 2560. We keep 51% of the width, so the game must render 1/0.51 = 1.96x
 * wider horizontally for the visible part to come out at the headset's FOV.
 *
 * WHY NOT RENDER STRAIGHT INTO THE EYE TARGET, which wastes nothing? Because
 * D3D9 requires the depth-stencil surface to be at least as large as the render
 * target, and the eye is 2015 tall against the game's 1440 depth buffer -- so
 * it needs our own depth surface, which must also match multisample type, which
 * means dropping the game's 4x MSAA and hoping the engine does not rebind its
 * own targets mid-scene. That is a much bigger change with several ways to fail
 * silently. This one is arithmetic. */
static void setup_crop(UINT sw, UINT sh)
{
    double eye_aspect, cw, ch;

    if (!sw || !sh || !g_rw || !g_rh) return;
    eye_aspect = (double)g_rw / (double)g_rh;

    ch = (double)sh;
    cw = ch * eye_aspect;
    if (cw > (double)sw) { cw = (double)sw; ch = cw / eye_aspect; }

    g_crop.left   = (LONG)((sw - cw) * 0.5);
    g_crop.top    = (LONG)((sh - ch) * 0.5);
    g_crop.right  = g_crop.left + (LONG)cw;
    g_crop.bottom = g_crop.top  + (LONG)ch;
    g_have_crop   = 1;

    /* The intermediate the MSAA resolve lands in. Same size as the source and
     * single-sampled, which is what makes the resolve legal with no rect. */
    if (!g_resolve) {
        if (FAILED(IDirect3DDevice9_CreateRenderTarget(g_dev, sw, sh, D3DFMT_X8R8G8B8,
                                                       D3DMULTISAMPLE_NONE, 0, FALSE,
                                                       &g_resolve, NULL))) {
            g_resolve = NULL;
            g_have_crop = 0;         /* no intermediate -> no crop; stretch as before */
            glog("could not create the %ux%u resolve target -- falling back to full-frame "
                 "stretch (aspect will be wrong, but it will not be black)", sw, sh);
        }
    }

    /* Widen the render FOV by exactly what the crop discards, so the visible
     * part subtends the headset's angles. */
    if (g_fov_ok) {
        int e;
        for (e = 0; e < 2; e++) {
            g_fov[e][0] = g_fov_hmd[e][0] * (float)((double)sw / cw);
            g_fov[e][1] = g_fov_hmd[e][1] * (float)((double)sh / ch);
        }
    }
    /* Log unconditionally. The first version logged only when g_fov_ok, and
     * when the screen came up black there was no crop line at all -- so the
     * geometry could not be checked against the symptom. Same lesson as the
     * slot watch: an instrument that is silent in the failing case is worse
     * than none, because it reads as "that code did not run". */
    glog("crop %ldx%ld at (%ld,%ld) of %ux%u -> render tanHalfFov %.4f %.4f (headset %.4f %.4f) fov_ok=%d",
         g_crop.right - g_crop.left, g_crop.bottom - g_crop.top,
         g_crop.left, g_crop.top, sw, sh,
         g_fov[0][0], g_fov[0][1], g_fov_hmd[0][0], g_fov_hmd[0][1], g_fov_ok);
}

/* Called by camera.asi immediately after each of its two R_RenderScene calls.
 *
 * WHY THE CURRENT RENDER TARGET AND NOT THE SWAP CHAIN'S BACK BUFFER.
 * We are mid-frame here: the game has drawn the 3D scene but has not finished
 * the frame, and it may well render the scene into an offscreen target and
 * composite later (this engine has post-processing). Reading the swap chain's
 * back buffer at this moment would then capture whatever was left there --
 * plausibly last frame's image, which would look like working stereo with a
 * one-frame lag rather than like a bug. GetRenderTarget(0) is whatever the game
 * is actually drawing into right now, offscreen or not, so it is correct in
 * both cases. Its description is logged once so we find out which.
 *
 * StretchRect does the MSAA resolve and the downscale to the per-eye size in
 * one call, exactly as the AER path did. */

__declspec(dllexport) void bo1vr_capture_eye(int eye)
{
    IDirect3DSurface9 *rt = NULL;
    HRESULT hr;

    read_opts();

    /* probe2.on: the SAME single StretchRect as probe.on, moved to here --
     * mid-scene, from the camera hook, with GetRenderTarget(0) as the source
     * instead of the back buffer. probe.on survived; if this one freezes, the
     * trigger is resolving the CURRENTLY BOUND render target in the middle of
     * the game's own scene render, not the existence of extra work. */
    if (g_probe2) {
        static LONG n2; static int logged2;
        IDirect3DSurface9 *cur = NULL;
        HRESULT hr2;
        if (!g_probe_rt || !g_dev) {
            if (!logged2) { logged2 = 1;
                glog("probe2: NOT RUNNING (rt=%p dev=%p) -- a silent no-op would "
                     "look exactly like a pass", (void *)g_probe_rt, (void *)g_dev); }
            return;
        }
        if (SUCCEEDED(IDirect3DDevice9_GetRenderTarget(g_dev, 0, &cur)) && cur) {
            D3DSURFACE_DESC d;
            hr2 = IDirect3DDevice9_StretchRect(g_dev, cur, NULL, g_probe_rt, NULL, D3DTEXF_NONE);
            if (!logged2) {
                logged2 = 1;
                if (SUCCEEDED(IDirect3DSurface9_GetDesc(cur, &d)))
                    glog("probe2 LIVE: mid-scene StretchRect from bound RT %ux%u ms=%d "
                         "-> probe target, hr=0x%08lx", d.Width, d.Height,
                         (int)d.MultiSampleType, (unsigned long)hr2);
            }
            IDirect3DSurface9_Release(cur);
            InterlockedIncrement(&n2);
        } else if (!logged2) {
            logged2 = 1; glog("probe2: GetRenderTarget failed -- nothing issued");
        }
        return;
    }

    if (g_state != 1 || !g_dev || eye < 0 || eye > 1)
        return;                               /* not up yet -- first frames */
    if (!g_vkdev) return;                     /* novk.on: no interop, no capture */
    if (!g_eyes[eye][g_buf].surf) return;     /* notex.on: no target to resolve into */

    /* nocap.on: everything else stays -- interop, textures, transitions,
     * Submit -- but no D3D9 work is issued from the camera hook. It is the
     * last cut available before novr.on, and it separates "the capture
     * StretchRects wedge the device" from "merely having the interop device
     * and its textures alive does". */
    read_opts();
    if (g_nocap) return;

    if (FAILED(IDirect3DDevice9_GetRenderTarget(g_dev, 0, &rt)) || !rt)
        return;

    if (!g_rt_logged) {
        D3DSURFACE_DESC d;
        if (SUCCEEDED(IDirect3DSurface9_GetDesc(rt, &d))) {
            glog("scene render target %ux%u fmt=%d MULTISAMPLE=%d pool=%d usage=0x%lx",
                 d.Width, d.Height, (int)d.Format, (int)d.MultiSampleType,
                 (int)d.Pool, (unsigned long)d.Usage);
            setup_crop(d.Width, d.Height);
        }
        g_rt_logged = 1;
    }

    /* TWO STEPS, BECAUSE THE SOURCE IS MULTISAMPLED.
     *
     * D3D9 will not copy a SUB-RECTANGLE of a multisampled surface: a resolve
     * has to take the whole thing. Passing a source rect anyway is what turned
     * the headset black -- and it did NOT fail, StretchRect returned success
     * and produced nothing, so the capture counted and "DUAL VIEW live" was
     * still logged. A silent success is the worst possible failure mode here.
     *
     * So: resolve the whole MSAA surface into a plain intermediate first (no
     * rect, which is legal), then crop from that (not multisampled, so a rect
     * is fine). */
    hr = resolve_cropped(rt, g_eyes[eye][g_buf].surf);
    if (FAILED(hr)) {
        if (g_captured < 4) glog("capture eye %d StretchRect hr=0x%08lx", eye, (unsigned long)hr);
    } else {
        /* Mark the copy. submit_eye waits on this before handing the texture
         * to the compositor. */
        if (g_eyes[eye][g_buf].q)
            IDirect3DQuery9_Issue(g_eyes[eye][g_buf].q, D3DISSUE_END);
        InterlockedIncrement(&g_captured);
    }
    IDirect3DSurface9_Release(rt);
}


/* THE ASPECT FIX, SHARED BY BOTH CAPTURE PATHS.
 *
 * This lived only inside bo1vr_capture_eye, so the moment nocap.on routed the
 * pipeline through the Present-time resolve -- which is what the fix for
 * trigger 1 does, and what the stable configuration ships -- the crop and the
 * widened FOV silently stopped happening and the 1.96x squeeze came straight
 * back. It was reported from a headset as "still feels zoomed", which is what a
 * quietly-skipped correction feels like from the inside.
 *
 * Two steps, because the source is MULTISAMPLED: D3D9 will not copy a
 * sub-rectangle of a multisampled surface, and passing a rect anyway does not
 * fail -- StretchRect returns success and produces nothing. Resolve the whole
 * surface into a plain intermediate first, then crop from that. */
static HRESULT resolve_cropped(IDirect3DSurface9 *src, IDirect3DSurface9 *dst)
{
    HRESULT hr;

    if (g_have_crop && g_resolve) {
        hr = IDirect3DDevice9_StretchRect(g_dev, src, NULL, g_resolve, NULL, D3DTEXF_NONE);
        if (SUCCEEDED(hr))
            hr = IDirect3DDevice9_StretchRect(g_dev, g_resolve, &g_crop, dst, NULL,
                                              D3DTEXF_LINEAR);
        return hr;
    }
    return IDirect3DDevice9_StretchRect(g_dev, src, NULL, dst, NULL, D3DTEXF_LINEAR);
}

/* Per-eye tanHalfFov for camera.asi. Returns 0 until OpenVR is up. */
__declspec(dllexport) int bo1vr_get_eye_fov(int eye, float *tanx, float *tany)
{
    if (!g_fov_ok || eye < 0 || eye > 1 || !tanx || !tany) return 0;
    *tanx = g_fov[eye][0];
    *tany = g_fov[eye][1];
    return 1;
}

/* ------------------------------------------------------------ per-frame */

/* BISECT SWITCHES, read once. The freeze happens with submission on and not
 * with it off, but the watchdog shows us IDLE when it happens and the pose
 * thread still ticking -- so we are not blocked, we leave something wedged and
 * the game's own render loop stops afterwards. These split the submission path
 * into its three separable parts so one run names the culprit:
 *
 *   nolock.on    skip LockSubmissionQueue/ReleaseSubmissionQueue around Submit
 *   notrans.on   skip the image layout transitions
 *   nosubmit.on  do everything except the actual IVRCompositor::Submit
 *   nowaitlock.on  give up DXVK's submission lock around WaitGetPoses again,
 *                  i.e. put back the unsynchronised vkQueueSubmit that §13
 *                  identified as trigger 2. Present only to A/B the fix.
 *
 * Each is a file in C:\bo1vr. If the game stops freezing with one of them
 * present, that part is the culprit. */
static int opt(const char *name)
{
    char p[MAX_PATH];
    lstrcpyA(p, "C:\\bo1vr\\");
    lstrcatA(p, name);
    return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
}

/* Read once, from whichever comes first -- do_frame calls this BEFORE
 * WaitGetPoses, which submit_eye is too late for. */
static void read_opts(void)
{
    if (g_opts_read) return;
    g_opts_read = 1;
    g_nolock   = opt("nolock.on");
    g_notrans  = opt("notrans.on");
    g_nosubmit = opt("nosubmit.on");
    g_nogate   = opt("nogate.on");
    g_nowait   = opt("nowait.on");
    g_nocap    = opt("nocap.on");
    g_novk     = opt("novk.on");
    g_notex    = opt("notex.on");
    g_noviq    = opt("noviq.on");
    g_noq      = opt("noq.on");
    g_probe    = opt("probe.on");
    g_probe2   = opt("probe2.on");
    g_nowaitlock = opt("nowaitlock.on");
    glog("BISECT: nolock=%d notrans=%d nosubmit=%d nogate=%d nowait=%d nocap=%d novk=%d notex=%d noviq=%d noq=%d probe=%d probe2=%d nowaitlock=%d",
         g_nolock, g_notrans, g_nosubmit, g_nogate, g_nowait, g_nocap, g_novk, g_notex, g_noviq, g_noq, g_probe, g_probe2, g_nowaitlock);
}

/* ------------------------------------------------- per-frame timing capture
 *
 * WHY THIS EXISTS. Trigger 2 (§13) was found with a watchdog and a
 * thread-suspend autopsy -- instruments built after the fact, each costing
 * playtests to develop before it could say anything. A per-stage millisecond
 * record would have named the submit path on the FIRST frozen run, because the
 * stage that stops advancing IS the stage that hangs.
 *
 * The column set is lifted in spirit from Vice City VR's vr_perf_openxr_*.csv
 * (a reVC fork; x64, D3D12, none of our constraints) which carries
 * xr_wait_frame_ms / xr_acquire_ms / xr_end_frame_ms / submit_ms and, notably,
 * counts its own fallbacks and failures as first-class columns. This is that
 * idea mapped onto the stages THIS plugin actually has.
 *
 * A FROZEN FRAME WRITES NO ROW, deliberately. The row is emitted after the
 * real Present returns, so a hang leaves the last COMPLETED frame as the final
 * row and g_stage naming where the next one died. A half-written row would
 * have to be told apart from a merely slow one, and it could not be.
 *
 * Rules, because this file has already produced several hangs: no allocation,
 * no locks, no CRT beyond _snprintf, one fixed buffer appended in a single
 * WriteFile every PERF_FLUSH frames. It all runs on the render thread that is
 * already doing this work -- no new thread, no new ordering, nothing for the
 * submission queue to race against.
 *
 * ON BY DEFAULT, disabled with perf.off in C:\bo1vr. Two instruments in this
 * project were silent when they were finally needed and both runs were wasted;
 * an off-by-default diagnostic is that same mistake with extra steps. Cost is
 * ~200 bytes/frame, about 43 MB/hour at 60 fps. */
#define PERF_FLUSH 64
#define PERF_BUF   (PERF_FLUSH * 256)
#define PERF_MAX_STALE_MS 250.0         /* upper bound on rows lost to a hang */

static int    g_perf_on = -1;           /* -1 undecided, 0 off, 1 on */
static char   g_perf_buf[PERF_BUF];
static int    g_perf_len;
static int    g_perf_rows;
static double g_perf_t0;
static double g_perf_last_flush;
static LONG   g_present_n;              /* every Present, VR live or not */

/* Stage timers: written during the frame, read once when the row is emitted.
 * Per-eye entries are indexed [0]=left [1]=right, matching submit_eye's own
 * argument so there is no second convention to get wrong. */
static double g_t_resolve, g_t_wait, g_t_present;
static double g_t_gate[2], g_t_trans[2], g_t_submit[2];
static LONG   g_t_spins[2];

/* SUBTRACT A BASELINE IN COUNTER UNITS, THEN CONVERT.
 *
 * MEASURED: the first real-game capture quantised every column to exact
 * multiples of 32 ms. Whole frames fell below one tick and every per-stage
 * column read 0.000 -- an instrument that reported nothing while looking like
 * it worked, which is the exact failure this file keeps repeating.
 *
 * The cause is not the clock. QPF is 10 MHz (0.1 us) and the log line in
 * perf_init prints it. It is that D3D9 puts the x87 into SINGLE PRECISION --
 * a 24-bit mantissa -- for any device created without D3DCREATE_FPU_PRESERVE,
 * and neither the game (flags=0x40, see the CreateDevice log line) nor
 * fakegame passes it. The old form computed (double)counter * 1000.0 / freq
 * on an absolute counter of ~4.3e12; at 24 bits of mantissa the representable
 * step near counter*1000 is ~2^28 counts, i.e. tens of milliseconds. That is
 * the 32 ms, and it appeared only once a D3D9 device existed.
 *
 * Differencing FIRST makes the subtraction exact in 64-bit integers, so the
 * double only ever sees a small elapsed count and 24 bits is ample: at 43 ms
 * elapsed the step is ~3 ns. This is the right form regardless of the FPU
 * mode, which is why it is not conditional on it. */
static LARGE_INTEGER g_qpf, g_qpc0;

static double perf_now(void)
{
    LARGE_INTEGER c;
    if (!g_qpf.QuadPart) {
        if (!QueryPerformanceFrequency(&g_qpf) || !g_qpf.QuadPart) return 0.0;
        QueryPerformanceCounter(&g_qpc0);
    }
    if (!QueryPerformanceCounter(&c)) return 0.0;
    return (double)(c.QuadPart - g_qpc0.QuadPart) * 1000.0 / (double)g_qpf.QuadPart;
}

static void perf_flush(void)
{
    char path[MAX_PATH];
    HANDLE h;
    DWORD w, n;

    if (g_perf_len <= 0) return;
    n = GetTempPathA(MAX_PATH - 24, path);
    if (n && n < MAX_PATH - 24) {
        lstrcatA(path, "bo1vr_frames.csv");
        h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            WriteFile(h, g_perf_buf, (DWORD)g_perf_len, &w, NULL);
            CloseHandle(h);
        }
    }
    g_perf_len = 0;
}

/* Truncate and write the header once. The freeze harness relaunches the game
 * repeatedly; appending would let several runs read as one long session, which
 * is exactly the confusion that made freezerun.sh count non-launches as
 * passes. */
static void perf_init(void)
{
    static const char hdr[] =
        "present,frame,elapsed_ms,total_ms,resolve_ms,wait_poses_ms,"
        "gate_l_ms,trans_l_ms,submit_l_ms,gate_r_ms,trans_r_ms,submit_r_ms,"
        "present_ms,spins_l,spins_r,gate_timeouts,waitlocks,flat,skipped,"
        "dual,eye,submits,pose_ticks,stage\n";
    char path[MAX_PATH];
    HANDLE h;
    DWORD w, n;

    g_perf_on = (GetFileAttributesA("C:\\bo1vr\\perf.off") == INVALID_FILE_ATTRIBUTES);
    if (!g_perf_on) { glog("perf CSV disabled (perf.off)"); return; }

    n = GetTempPathA(MAX_PATH - 24, path);
    if (!n || n >= MAX_PATH - 24) { g_perf_on = 0; return; }
    lstrcatA(path, "bo1vr_frames.csv");
    h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { g_perf_on = 0; glog("perf CSV: cannot create file"); return; }
    WriteFile(h, hdr, (DWORD)(sizeof(hdr) - 1), &w, NULL);
    CloseHandle(h);
    g_perf_t0 = perf_now();
    /* Record the clock itself. If the columns ever look quantised again this
     * line says immediately whether the clock or the code is at fault. */
    glog("perf CSV live -> %s (QPF=%ld Hz, base=%ld%09ld)", path,
         (long)g_qpf.QuadPart, (long)(g_qpc0.QuadPart / 1000000000),
         (long)(g_qpc0.QuadPart % 1000000000));
}

static void perf_row(double total_ms)
{
    int k;

    if (g_perf_on < 0) perf_init();
    if (!g_perf_on) return;

    /* Flush BEFORE writing if the next row might not fit, so the buffer can
     * never overrun and _snprintf never has to truncate a row. */
    if (g_perf_len > PERF_BUF - 256) perf_flush();

    k = _snprintf(g_perf_buf + g_perf_len, PERF_BUF - g_perf_len - 1,
                  "%ld,%ld,%.1f,%.3f,%.3f,%.3f,"
                  "%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,"
                  "%.3f,%ld,%ld,%ld,%ld,%ld,%ld,"
                  "%d,%d,%ld,%ld,%ld\n",
                  g_present_n, g_frames, perf_now() - g_perf_t0, total_ms,
                  g_t_resolve, g_t_wait,
                  g_t_gate[0], g_t_trans[0], g_t_submit[0],
                  g_t_gate[1], g_t_trans[1], g_t_submit[1],
                  g_t_present, g_t_spins[0], g_t_spins[1],
                  g_gate_timeouts, g_waitlocks, g_flat_frames, g_skipped,
                  g_dual, g_cur_eye, g_submitted, g_pose_ticks, g_stage);
    if (k > 0) g_perf_len += k;

    /* FLUSH ON A TIME BOUND, not only on a row count.
     *
     * A count alone loses up to PERF_FLUSH-1 rows, and in a freeze those are
     * exactly the rows worth having. Not hypothetical: the first bench run of
     * this code presented 60 frames, never reached the 64-row mark, and wrote a
     * file containing nothing but the header.
     *
     * PERF_MAX_STALE_MS bounds the loss in TIME instead, so it holds at any
     * frame rate -- whatever the hang, the last row on disk is at most a
     * quarter second before it, and the watchdog fires at 3 s. Cost is at most
     * four file opens a second, on a thread already doing this work. */
    if (++g_perf_rows >= PERF_FLUSH ||
        perf_now() - g_perf_last_flush > PERF_MAX_STALE_MS) {
        g_perf_rows = 0;
        g_perf_last_flush = perf_now();
        perf_flush();
    }
}

static void submit_eye(int i)
{
    bo1vr_VkImageSubresourceRange sub;
    struct VRVulkanTextureData_t vkdata;
    struct Texture_t tex;
    EVRCompositorError ce;

    struct eye_target *E = &g_eyes[i][g_buf];
    double t0;

    /* Clear FIRST: the early return below would otherwise leave last frame's
     * numbers in place and the CSV would report work that did not happen. */
    g_t_gate[i] = g_t_trans[i] = g_t_submit[i] = 0.0;
    g_t_spins[i] = 0;

    read_opts();
    if (!g_vkdev || !E->vktex) return;         /* novk/notex: nothing to submit */

    sub.aspectMask     = BO1VR_VK_IMAGE_ASPECT_COLOR_BIT;
    sub.baseMipLevel   = 0;
    sub.levelCount     = E->info.mipLevels;
    sub.baseArrayLayer = 0;
    sub.layerCount     = E->info.arrayLayers;

    /* Exactly the order Proton's own D3D11 path uses
     * (vrcompositor_manual.c load_compositor_texture_dxvk). */
    /* GATE ON THE COPY HAVING LANDED.
     *
     * render-submit-sync-RE.md's finding, in its own setting: the shim released
     * a swapchain image the app had not actually vkQueueSubmitted yet, and the
     * compositor read an unwritten image. Its tell was vkQueueWaitIdle doing
     * nothing -- there was no queued work to drain -- and the failure was
     * load-gated. FlushRenderingCommands is the same kind of assumption: it
     * asks DXVK to flush, it does not establish that our StretchRect has been
     * consumed.
     *
     * A D3D9 EVENT query does establish it. GetData with D3DGETDATA_FLUSH
     * returns S_FALSE until every command issued before the query has been
     * consumed by the GPU.
     *
     * BOUNDED, deliberately. An unbounded wait here would be a new hang in a
     * codebase that has already produced several. If the budget expires we
     * submit anyway and count it: a stale frame is a far better failure than a
     * frozen game, and the counter turns "is this the race?" into a number. */
    if (E->q && !g_nogate) {
        int spins = 0;
        t0 = perf_now();
        while (IDirect3DQuery9_GetData(E->q, NULL, 0, D3DGETDATA_FLUSH) == S_FALSE) {
            if (++spins > 200000) { g_gate_timeouts++; break; }
        }
        g_t_gate[i]  = perf_now() - t0;
        g_t_spins[i] = spins;
        if (spins > g_gate_max_spins) g_gate_max_spins = spins;
    }

    t0 = perf_now();
    if (!g_notrans)
        g_vkdev->lpVtbl->TransitionTextureLayout(g_vkdev, E->vktex, &sub,
                                                 E->layout,
                                                 BO1VR_VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    g_vkdev->lpVtbl->FlushRenderingCommands(g_vkdev);
    if (!g_nolock)
        g_vkdev->lpVtbl->LockSubmissionQueue(g_vkdev);

    memset(&vkdata, 0, sizeof(vkdata));
    vkdata.m_nImage            = E->image;
    vkdata.m_pDevice           = (struct VkDevice_T *)g_vkdevice;
    vkdata.m_pPhysicalDevice   = (struct VkPhysicalDevice_T *)g_vkphys;
    vkdata.m_pInstance         = (struct VkInstance_T *)g_vkinst;
    vkdata.m_pQueue            = (struct VkQueue_T *)g_vkqueue;
    vkdata.m_nQueueFamilyIndex = g_qfamily;
    vkdata.m_nWidth            = E->info.extent.width;
    vkdata.m_nHeight           = E->info.extent.height;
    vkdata.m_nFormat           = (uint32_t)E->info.format;
    vkdata.m_nSampleCount      = 1;

    tex.handle      = &vkdata;
    tex.eType       = ETextureType_TextureType_Vulkan;
    tex.eColorSpace = EColorSpace_ColorSpace_Auto;
    g_t_trans[i]    = perf_now() - t0;    /* transitions + flush + queue lock */

    /* pBounds is NULL and must stay NULL: xrizer ignores it (Exp. 6), so it is
     * not a way to slice one texture into two eyes -- it would send the whole
     * thing to both. */
    t0 = perf_now();
    ce = g_nosubmit ? EVRCompositorError_VRCompositorError_None
                    : g_comp->Submit(i == 0 ? EVREye_Eye_Left : EVREye_Eye_Right, &tex, NULL,
                                     EVRSubmitFlags_Submit_Default);
    g_t_submit[i] = perf_now() - t0;

    if (!g_nolock)
        g_vkdev->lpVtbl->ReleaseSubmissionQueue(g_vkdev);
    if (!g_notrans)
        g_vkdev->lpVtbl->TransitionTextureLayout(g_vkdev, E->vktex, &sub,
                                                 BO1VR_VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                 E->layout);

    if (ce == EVRCompositorError_VRCompositorError_None) {
        InterlockedIncrement(&g_submitted);
        g_submit_fails = 0;
    } else if (++g_submit_fails > 240) {
        /* The compositor has rejected everything for ~2 seconds. It is gone or
         * has taken focus away. Stop submitting rather than hammering it every
         * frame forever -- the game keeps running and rendering to the monitor,
         * which is the correct thing to degrade to. */
        glog("compositor rejected %ld consecutive submits (last %d) -- standing down",
             g_submit_fails, (int)ce);
        g_state = -1;
    }
    else if (g_frames < 3 || (g_frames % 600) == 0)
        glog("frame %ld eye %d: Submit -> %d", g_frames, i, (int)ce);
}

/* Resolve the game's back buffer into both eye targets and submit them. */
static void do_frame(IDirect3DSwapChain9 *sc)
{
    IDirect3DSurface9 *bb = NULL;
    HRESULT hr;
    LONG n;
    int i;
    double t_resolve0, t_wait0;

    /* The watchdog starts BEFORE the VR-init gate and outside it, so it is
     * running in every configuration -- including the ones where VR is off or
     * failed. A freeze diagnostic that only exists when the thing it diagnoses
     * is enabled cannot be tested against a known-good run, and this one gets
     * exercised on the fakegame bench precisely because vr_init fails there. */
    start_watchdog();
    read_opts();
    g_render_tid = GetCurrentThreadId();     /* whoever presents IS the render thread */
    hook_query_getdata();

    if (g_state < 0 || !g_dev)
        return;

    if (g_state == 0) {
        g_state = -1;                       /* latch: try once, never per-frame */
        if (!vr_init())         { glog("VR init failed -- staying out of the way"); return; }
        /* novk.on keeps OpenVR (session, compositor, xrizer, monado) and drops
         * ONLY the DXVK Vulkan interop device and its textures. Every
         * per-frame switch has now failed to stop the freeze on its own while
         * novr.on stops it completely, so what is left to test is the setup
         * that all of those configurations share. This splits it in two. */
        if (!g_novk && !interop_init(g_dev)) { glog("interop init failed -- staying out of the way"); return; }
        g_state = 1;
        start_pose_thread();
        glog("PIPE LIVE: game frames -> compositor (alternate-eye)");
    }

    n = InterlockedIncrement(&g_frames);
    g_watch_frame = n;
    g_stage = 1;

    /* DO NOT CALL WaitGetPoses HERE.
     *
     * This used to. It is the conventional VR frame clock -- it blocks until
     * the runtime wants the next frame -- and blocking the GAME'S RENDER THREAD
     * on an external process is how the game hangs.
     *
     * Measured, from a real session: monado logged "Frame late by 1016ms",
     * then 1033, then 1050, then 1066 -- climbing by exactly one 60 Hz frame
     * period each time -- followed by "Session is visible but not active" and
     * END_SESSION. The compositor session went away; WaitGetPoses never
     * returned; the game's main thread parked in futex_wait_multiple with every
     * DXVK thread idle, while the audio thread carried on playing. That is
     * precisely the "not responding but I can hear the background audio"
     * report, and it is a deadlock we introduced.
     *
     * GetLastPoses is the non-blocking read of the same data, which is what the
     * head tracking will want, and Submit is happy without the wait. The cost
     * is that we no longer sync to the headset's cadence, so the compositor
     * reprojects instead -- a far better failure mode than freezing the game.
     * It also undoes the "the game's frame rate becomes the headset's"
     * consequence noted earlier.
     *
     * If a real frame clock is wanted later it belongs on a thread of OUR own,
     * never on a thread the game owns. */
    /* Poses come from our own thread now -- see pose_thread. Nothing on the
     * game's render thread may block on the compositor. */

    hr = IDirect3DSwapChain9_GetBackBuffer(sc, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    if (FAILED(hr) || !bb) {
        if (n < 3) glog("GetBackBuffer hr=0x%08lx", (unsigned long)hr);
        return;
    }

    /* Record what we are actually resolving FROM, once. Without this the log
     * cannot distinguish "the MSAA resolve path ran" from "the source happened
     * to be single-sampled and StretchRect was a plain blit" -- and under
     * `proton run` the host's own stderr is swallowed, so there is no other
     * channel to ask. */
    if (n == 1) {
        D3DSURFACE_DESC d;
        if (SUCCEEDED(IDirect3DSurface9_GetDesc(bb, &d))) {
            glog("source backbuffer %ux%u fmt=%d MULTISAMPLE=%d pool=%d -> per-eye %ux%u",
                 d.Width, d.Height, (int)d.Format, (int)d.MultiSampleType, (int)d.Pool,
                 g_rw, g_rh);
            /* Set the crop up HERE too. It used to happen only in
             * bo1vr_capture_eye, so the Present-time path ran uncorrected. */
            if (!g_have_crop)
                setup_crop(d.Width, d.Height);
        }
    }

    /* TRUE DUAL VIEW when camera.asi has already given us both eyes.
     *
     * It hooks R_RenderScene and calls the original twice per frame, capturing
     * the scene render target after each -- so by the time we get here both eye
     * textures already hold their own view, taken this frame. Nothing to
     * resolve, and no alternation: every frame delivers both eyes at the game's
     * full rate.
     *
     * FALLBACK is the old alternate-eye path, used until camera.asi's hook is
     * live (the first frames, or if it failed). AER updates one eye per frame,
     * which halves each eye's rate and makes the desktop mirror visibly twitch
     * left-right, because the monitor shows every frame and consecutive frames
     * are different eyes. That twitch is what dual view removes. */
    t_resolve0 = perf_now();
    if (InterlockedExchange(&g_captured, 0) >= 2) {
        if (!g_dual) { glog("DUAL VIEW live: both eyes rendered per frame"); g_dual = 1; }
    } else if (g_dual) {
        /* DUAL VIEW IS LIVE BUT THIS FRAME HAD NO SCENE.
         *
         * Loading screens, the pause menu and anything else 2D never call
         * R_RenderScene, so nothing captured. Submitting the eye textures
         * unchanged would send whatever each eye last held -- and because the
         * two were captured at different moments, they can be from different
         * places entirely. That is the "eyes get out of sync on a loadscreen"
         * that showed up in play.
         *
         * Resolve the finished frame into BOTH eyes instead: a 2D screen has no
         * parallax to lose, so mono is not a compromise here, it is correct.
         * (A proper floating screen in world space is the eventual answer for
         * menus; this at least makes them coherent.) */
        for (i = 0; i < 2; i++) {
            hr = resolve_cropped(bb, g_eyes[i][g_buf].surf);
            if (0) hr = IDirect3DDevice9_StretchRect(g_dev, bb, NULL, g_eyes[i][g_buf].surf, NULL,
                                              D3DTEXF_LINEAR);
            if (FAILED(hr)) break;
        }
        /* COUNT these, do not just log the first.
         *
         * Two different things could make the image flash, and the RATIO tells
         * them apart:
         *   - if flat frames are roughly half, we are alternating between the
         *     cropped stereo path and this full-frame mono one, and the flash
         *     is the aspect and content changing every other frame;
         *   - if flat frames are rare, the flash is something else -- most
         *     likely the compositor still reading an eye texture while the next
         *     frame overwrites it, which wants double-buffered eye targets.
         * Guessing between those two costs a playtest each. Counting costs
         * nothing. */
        g_flat_frames++;
    } else {
        hr = resolve_cropped(bb, g_eyes[g_cur_eye][g_buf].surf);
        if (0) hr = IDirect3DDevice9_StretchRect(g_dev, bb, NULL, g_eyes[g_cur_eye][g_buf].surf, NULL,
                                          D3DTEXF_LINEAR);
        if (FAILED(hr)) {
            if (n < 3) glog("StretchRect eye %d hr=0x%08lx", g_cur_eye, (unsigned long)hr);
            IDirect3DSurface9_Release(bb);
            return;
        }
    }

    /* WaitGetPoses HERE, on the same thread as Submit.
     *
     * Every failure in this sequence has involved OpenVR calls split across two
     * threads, and the evidence finally named it: with the queue lock removed,
     * xrizer panicked on ThreadId(2) -- OUR pose thread -- inside WaitGetPoses
     * ("Failed to acquire swapchain image"), the thread died, and its tick
     * counter froze while the render thread wedged in its own StretchRect.
     *
     * xrizer is a reimplementation of an API whose applications are
     * conventionally single-threaded, and nothing documents it as thread-safe.
     * Calling WaitGetPoses on one thread and Submit on another is therefore
     * undefined, and we have been doing exactly that since the hang fix.
     *
     * So: back to one thread, which is the SUPPORTED shape. The original hang
     * risk returns -- WaitGetPoses can block if the compositor session ends --
     * but failing in a configuration the runtime supports is worth far more
     * than not failing in one it does not, because only the former can be
     * reported, reasoned about, or fixed upstream. The watchdog will name it if
     * it happens, and the stand-down backoff still applies. */
    /* HOLD DXVK'S SUBMISSION QUEUE LOCK ACROSS WaitGetPoses.
     *
     * This is not a guess. Read from the two sources, at the exact revisions
     * running here (xrizer be664bb, Monado e26a272c1):
     *
     *   xrizer Compositor::WaitGetPoses, with the DEFAULT timing mode
     *   (Implicit), calls PostPresentHandoff() itself -> FrameController::
     *   end_frame -> xrEndFrame;
     *
     *   Monado's client_vk_compositor_layer_commit -> submit_fence ->
     *   vk_create_and_submit_fence_native, which does
     *
     *       os_mutex_lock(&vk->queue_mutex);
     *       vkQueueSubmit(vk->queue, 0, NULL, fence);
     *       os_mutex_unlock(&vk->queue_mutex);
     *
     *   and `vk->queue` is OUR queue -- xrizer's VulkanData::
     *   session_create_info hands xrCreateSession the device, queue family and
     *   queue index taken straight out of VRVulkanTextureData_t, i.e.
     *   ID3D9VkInteropDevice::GetSubmissionQueue's.
     *
     * So every frame there is a vkQueueSubmit on DXVK's queue, serialised
     * against Monado's own mutex and NOTHING ELSE. VkQueue is an externally
     * synchronised object: concurrent vkQueueSubmit from two threads is
     * undefined behaviour, and DXVK's submission thread is the other thread.
     * Submit itself is already inside LockSubmissionQueue (submit_eye), which
     * is why removing the transitions or the lock changed nothing -- the
     * unsynchronised call was never in the part being bisected. It is in
     * WaitGetPoses.
     *
     * That also explains the one configuration that survived: with
     * nosubmit.on, xrizer never runs initialize_real_session, the frame
     * controller stays None, PostPresentHandoff returns at "no frame
     * controller", and the foreign vkQueueSubmit never happens at all. Submit
     * is not the trigger because of what it hands over -- it is the trigger
     * because it is what BINDS Monado's client compositor to DXVK's queue.
     *
     * LockSubmissionQueue is DxvkDevice::lockSubmission(): drain the pending
     * submissions, then take the same mutex the submission thread holds around
     * vkQueueSubmit. Held here it makes Monado's fence submit mutually
     * exclusive with DXVK's, which is all the Vulkan spec asks for.
     *
     * Cost: DXVK cannot submit while we are blocked in xrWaitFrame. The game's
     * render thread is this thread, so it is not producing work meanwhile;
     * what it buys is bounded and measured below (g_waitlocks in the frame
     * line proves this ran -- a silent no-op here would look exactly like a
     * pass, which is how three earlier instruments in this project lied).
     *
     * nowaitlock.on restores the old, racy order for an A/B. */
    g_stage = 2;
    g_t_resolve = perf_now() - t_resolve0;
    t_wait0 = perf_now();
    if (!g_nowait) {
        int locked = (!g_nowaitlock && g_vkdev != NULL);
        if (locked) { g_vkdev->lpVtbl->LockSubmissionQueue(g_vkdev); g_waitlocks++; }
        g_comp->WaitGetPoses(g_rposes, 64, g_gposes, 64);
        if (locked) g_vkdev->lpVtbl->ReleaseSubmissionQueue(g_vkdev);
    }
    /* Includes the queue lock, deliberately: the lock and the wait are one
     * critical section now (§13) and splitting them in the log would invite
     * the reading that they are independent. */
    g_t_wait = perf_now() - t_wait0;
    InterlockedIncrement(&g_pose_ticks);

    g_stage = 2;
    for (i = 0; i < 2; i++) {
        g_stage = 3 + i;
        submit_eye(i);
    }

    /* PostPresentHandoff REMOVED -- it is where we wedge.
     *
     * The watchdog caught it: "no frame for 3 s, stuck at stage 5", stage 5
     * being this call, with a matching xrizer panic at the same moment
     * ("Failed to acquire swapchain image: ERROR_RUNTIME_FAILURE"). Afterwards
     * the hook reached idle and was never entered again -- the game's own
     * render loop had stopped -- and the pose ticks froze.
     *
     * It is an optional HINT in OpenVR: it tells the compositor the app has
     * finished handing over its frame so it can start work earlier. Skipping it
     * costs a little latency and nothing else. Given it is the confirmed wedge
     * point, it goes.
     *
     * This also kills the theory the watchdog was built to test: we were NOT
     * stuck at stage 3 or 4, so Submit holding DXVK's submission queue lock is
     * not the problem. */
    SetEvent(g_ev_consumed);
    /* Next frame writes into the other set, so nothing the compositor may
     * still be reading gets overwritten. */
    g_buf ^= 1;
    g_stage = 0;
    IDirect3DSurface9_Release(bb);

    /* Flip, and tell camera.asi which eye the NEXT frame belongs to. The eye
     * alternation lives here rather than in the camera hook because the eye is
     * a property of the FRAME -- one frame, one back buffer, one eye -- and
     * this is the only place that sees frame boundaries. R_SetViewParms can be
     * called more than once per frame (shadow and portal views go through it
     * too), so a counter there would not alternate per frame. */
    if (g_dual)
        return;                 /* dual view owns the eyes; no alternation */

    g_cur_eye ^= 1;
    if (!g_set_eye) {
        HMODULE cam = GetModuleHandleA("camera.asi");
        if (cam)
            g_set_eye = (void (*)(int))GetProcAddress(cam, "bo1vr_camera_set_eye");
        if (!g_set_eye && n < 3)
            glog("camera.asi not present -- frames will be mono (no per-eye camera)");
    }
    if (g_set_eye)
        g_set_eye(g_cur_eye);

    if (n == 1 || n == 2 || (n % 600) == 0)
        glog("frame %ld: %ld submits, %ld ticks, %ld flat, %ld skipped, gate max %ld spins, %ld timeouts, %ld waitlocks",
             n, g_submitted, g_pose_ticks, g_flat_frames, g_skipped,
             g_gate_max_spins, g_gate_timeouts, g_waitlocks);
}

/* ------------------------------------------------------------------ hooks */

typedef IDirect3D9 *(WINAPI *pfn_create9)(UINT);
typedef HRESULT (WINAPI *pfn_createdev)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD,
                                        D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);
typedef HRESULT (WINAPI *pfn_sc_present)(IDirect3DSwapChain9 *, const RECT *, const RECT *,
                                         HWND, const RGNDATA *, DWORD);
typedef HRESULT (WINAPI *pfn_reset)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);

static pfn_create9    real_create9;
static pfn_createdev  real_createdev;
static pfn_sc_present real_sc_present;
static pfn_reset      real_reset;


/* THE PROBE: is it OUR work, or ANY extra work?
 *
 * Every trial so far has run with the VR pipeline at least partly alive. This
 * one runs with novr.on -- no OpenVR, no interop, no eye targets, nothing --
 * and adds exactly one thing: a single D3DPOOL_DEFAULT render target, and one
 * StretchRect resolve of the back buffer into it per Present.
 *
 * That is the smallest possible piece of extra GPU work on the game's device,
 * issued from the safest possible place (Present, between frames, not mid-scene
 * from the camera hook). If the game still deadlocks on its own event query,
 * then the trigger is not our capture, our interop or our submission -- it is
 * that ANY additional command breaks the game's fence accounting, and the fix
 * has to be about flush/submission, not about which operation we choose.
 *
 * If it survives, the trigger is specific to what we do or where we do it, and
 * that is a much easier problem. */
static int g_probe_state;

static void probe_frame(IDirect3DSwapChain9 *sc)
{
    IDirect3DSurface9 *bb = NULL;
    HRESULT hr;

    if (g_probe_state < 0 || !g_dev) return;

    if (g_probe_state == 0) {
        D3DSURFACE_DESC d;
        g_probe_state = -1;
        if (FAILED(IDirect3DSwapChain9_GetBackBuffer(sc, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb)
            return;
        hr = IDirect3DSurface9_GetDesc(bb, &d);
        IDirect3DSurface9_Release(bb);
        if (FAILED(hr)) return;
        hr = IDirect3DDevice9_CreateRenderTarget(g_dev, d.Width, d.Height, d.Format,
                                                 D3DMULTISAMPLE_NONE, 0, FALSE,
                                                 &g_probe_rt, NULL);
        if (FAILED(hr) || !g_probe_rt) {
            glog("probe: CreateRenderTarget hr=0x%08lx", (unsigned long)hr);
            return;
        }
        glog("probe.on: one %ux%u RT, one StretchRect per Present, NO VR at all",
             d.Width, d.Height);
        g_probe_state = 1;
        return;
    }

    if (g_probe2) {          /* the work moves to the camera hook; count frames here */
        g_watch_frame = InterlockedIncrement(&g_frames);
        return;
    }

    if (FAILED(IDirect3DSwapChain9_GetBackBuffer(sc, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb)
        return;
    hr = IDirect3DDevice9_StretchRect(g_dev, bb, NULL, g_probe_rt, NULL, D3DTEXF_NONE);
    IDirect3DSurface9_Release(bb);
    if (FAILED(hr) && g_frames < 3)
        glog("probe: StretchRect hr=0x%08lx", (unsigned long)hr);

    /* The watchdog needs to see frames advancing, and do_frame will not run. */
    g_watch_frame = InterlockedIncrement(&g_frames);
}

static HRESULT WINAPI my_sc_present(IDirect3DSwapChain9 *sc, const RECT *a, const RECT *b,
                                    HWND w, const RGNDATA *d, DWORD f)
{
    HRESULT hr;
    double t0 = perf_now(), tp;

    start_watchdog();
    read_opts();

    /* Zero the stage timers here rather than in do_frame: do_frame has several
     * early returns (VR off, VR failed, GetBackBuffer failed, resolve failed)
     * and each one would otherwise leave the previous frame's numbers in the
     * row, reporting work that did not happen this frame. */
    g_present_n++;
    g_t_resolve = g_t_wait = 0.0;

    if (g_probe) probe_frame(sc);
    do_frame(sc);
    /* Call the real Present LAST: the game's own picture still goes to the
     * monitor exactly as before. Nothing above changes the render target, the
     * viewport or any state -- StretchRect and the interop calls do not. */
    tp = perf_now();
    hr = real_sc_present(sc, a, b, w, d, f);
    g_t_present = perf_now() - tp;

    /* The row lands AFTER the real Present, so a frozen frame writes nothing
     * and the last row plus g_stage says where the next one died. */
    perf_row(perf_now() - t0);
    return hr;
}

static HRESULT WINAPI my_reset(IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *pp)
{
    HRESULT hr;
    /* Our eye targets are D3DPOOL_DEFAULT and MUST be gone before Reset or the
     * Reset fails outright. Rebuild them afterwards. */
    if (g_state == 1) {
        glog("Reset: releasing eye targets");
        if (g_resolve) { IDirect3DSurface9_Release(g_resolve); g_resolve = NULL; }
        g_have_crop = 0; g_rt_logged = 0;
        release_eyes();
        g_state = 2;                        /* rebuild after the reset */
    }
    hr = real_reset(dev, pp);
    if (g_state == 2) {
        g_state = interop_init(dev) ? 1 : -1;
        glog("Reset: eye targets rebuilt -> state %d", g_state);
    }
    return hr;
}

static void hook_device(IDirect3DDevice9 *dev)
{
    IDirect3DSwapChain9 *sc = NULL;
    void **vt, **svt;

    if (g_dev_hooked || !dev) return;
    g_dev = dev;
    vt = *(void ***)dev;

    if (FAILED(IDirect3DDevice9_GetSwapChain(dev, 0, &sc)) || !sc) {
        glog("GetSwapChain(0) failed -- cannot hook the present path");
        return;
    }
    svt = *(void ***)sc;
    /* Slot 3: IUnknown's three, then Present. Slot 17 on the DEVICE is the
     * wrapper this game never calls (Exp. 10). */
    if (MH_CreateHook(svt[3], (void *)my_sc_present, (void **)&real_sc_present) == MH_OK &&
        MH_EnableHook(svt[3]) == MH_OK)
        glog("swapchain Present hooked (%p)", svt[3]);
    else
        glog("FAILED to hook swapchain Present");
    IDirect3DSwapChain9_Release(sc);

    if (MH_CreateHook(vt[16], (void *)my_reset, (void **)&real_reset) == MH_OK)
        MH_EnableHook(vt[16]);

    g_dev_hooked = 1;
    glog("device %p hooked", (void *)dev);
}

static HRESULT WINAPI my_createdevice(IDirect3D9 *self, UINT adapter, D3DDEVTYPE type,
                                      HWND focus, DWORD flags,
                                      D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **out)
{
    /* THE CANDIDATE FIX.
     *
     * The game asks for a SINGLE-THREADED device (flags=0x40, HWVP only). D3D9
     * then permits the runtime to omit internal locking, and DXVK takes that
     * permission. Every D3D9 call we add -- the capture StretchRects, the
     * interop transitions, the gate's GetData -- is extra traffic on a device
     * whose owner promised there would be none beyond its own.
     *
     * That fits the evidence in a way no other hypothesis has: the trigger is
     * touching extra resources rather than any particular operation; it is
     * stochastic (probe2 froze once in two runs); DXVK's worker threads are all
     * IDLE at the freeze rather than blocked, which is what a lost wakeup looks
     * like rather than a deadlock; and Monado, in another process, is
     * unaffected throughout.
     *
     * mt.on ORs in D3DCREATE_MULTITHREADED, which is the documented way to say
     * "this device will be used from more than one place". It costs some
     * locking overhead and nothing else. */
    if (opt("mt.on")) {
        flags |= D3DCREATE_MULTITHREADED;
        glog("mt.on: forcing D3DCREATE_MULTITHREADED (game asked for 0x%08lx)",
             (unsigned long)(flags & ~(DWORD)D3DCREATE_MULTITHREADED));
    }
    {
    HRESULT hr = real_createdev(self, adapter, type, focus, flags, pp, out);
    if (SUCCEEDED(hr) && out && *out) {
        /* BehaviorFlags decides whether calling D3D9 from any thread but the
         * creating one is legal. The freeze probe issues a query from the
         * watchdog thread, and that measurement is only trustworthy if
         * D3DCREATE_MULTITHREADED is set -- otherwise its answer is undefined
         * behaviour dressed up as evidence. */
        glog("CreateDevice adapter=%u %ux%u windowed=%d flags=0x%08lx%s%s", adapter,
             pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0,
             pp ? (int)pp->Windowed : -1, (unsigned long)flags,
             (flags & D3DCREATE_MULTITHREADED) ? " MULTITHREADED" : " single-threaded",
             (flags & D3DCREATE_HARDWARE_VERTEXPROCESSING) ? " HWVP" : "");
        hook_device(*out);
    }
    return hr;
    }
}

static IDirect3D9 *WINAPI my_create9(UINT sdk)
{
    IDirect3D9 *d3d = real_create9(sdk);
    if (d3d && !real_createdev) {
        void **vt = *(void ***)d3d;
        if (MH_CreateHook(vt[16], (void *)my_createdevice, (void **)&real_createdev) == MH_OK &&
            MH_EnableHook(vt[16]) == MH_OK)
            glog("CreateDevice hooked");
    }
    return d3d;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    HMODULE d3d9;
    void *fn;
    MH_STATUS st;

    (void)reserved;

    /* Write out whatever rows are still buffered on a CLEAN shutdown, so the
     * tail of a session is not silently dropped -- the bench lost 28 of 60
     * rows that way. WriteFile/CreateFile are safe here: no loader lock is
     * taken and no CRT startup is required, unlike the thread creation this
     * file already forbids in DllMain.
     *
     * This is a convenience, NOT the freeze guarantee. A frozen game never
     * reaches DLL_PROCESS_DETACH; the time-bounded flush in perf_row is what
     * covers that case, and it is the one that matters. */
    if (reason == DLL_PROCESS_DETACH) {
        if (g_perf_on > 0) perf_flush();
        return TRUE;
    }

    if (reason != DLL_PROCESS_ATTACH)
        return TRUE;

    DisableThreadLibraryCalls(inst);
    glog("attach pid=%lu", GetCurrentProcessId());

    /* d3d9.dll is a static import of BlackOps.exe, so it is already mapped when
     * any DllMain runs -- no nested LoadLibrary needed. */
    d3d9 = GetModuleHandleA("d3d9.dll");
    if (!d3d9) d3d9 = LoadLibraryA("d3d9.dll");
    if (!d3d9) { glog("no d3d9.dll in this process"); return TRUE; }

    fn = (void *)GetProcAddress(d3d9, "Direct3DCreate9");
    if (!fn) { glog("no Direct3DCreate9 export"); return TRUE; }

    st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        glog("MH_Initialize failed (%d)", (int)st);
        return TRUE;
    }
    /* DXVK reads its debug env at DxvkInstance creation, which happens INSIDE
     * the first Direct3DCreate9 -- so setting it here, while we arm that hook,
     * is still early enough. There is no launch environment to set under a
     * Steam launch (Exp. 9), and this is the only lever we have.
     *
     * DXVK_DEBUG=hang enables VK_KHR_device_fault and
     * VK_NV_device_diagnostic_checkpoints, and makes the submission queue print
     * hang info when a semaphore wait does not complete -- exactly our state. */
    if (opt("dxvkhang.on")) {
        char lp[MAX_PATH];
        DWORD ln;
        SetEnvironmentVariableA("DXVK_DEBUG", "hang");
        SetEnvironmentVariableA("DXVK_LOG_LEVEL", "info");
        ln = GetTempPathA(MAX_PATH - 16, lp);
        if (ln && ln < MAX_PATH - 16) {
            lstrcatA(lp, "dxvklog");
            CreateDirectoryA(lp, NULL);
            SetEnvironmentVariableA("DXVK_LOG_PATH", lp);
            glog("dxvkhang.on: DXVK_DEBUG=hang, DXVK logs -> %s", lp);
        }
    }
    if (MH_CreateHook(fn, (void *)my_create9, (void **)&real_create9) == MH_OK &&
        MH_EnableHook(fn) == MH_OK)
        glog("armed at Direct3DCreate9=%p", fn);
    else
        glog("FAILED to hook Direct3DCreate9");
    return TRUE;
}
