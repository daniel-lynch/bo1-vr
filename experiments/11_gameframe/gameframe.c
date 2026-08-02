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
};

static IDirect3DDevice9   *g_dev;
static ID3D9VkInteropDevice *g_vkdev;
static bo1vr_VkInstance    g_vkinst;
static bo1vr_VkPhysicalDevice g_vkphys;
static bo1vr_VkDevice      g_vkdevice;
static bo1vr_VkQueue       g_vkqueue;
static uint32_t            g_qindex, g_qfamily;
static struct eye_target   g_eyes[2];
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

    for (i = 0; i < 2; i++) {
        /* X8R8G8B8, not A8R8G8B8: the back buffer has an alpha channel whose
         * contents are whatever the game left there, and the compositor would
         * honour it. Forcing opaque here costs nothing and cannot produce a
         * see-through eye. */
        hr = IDirect3DDevice9_CreateTexture(dev, g_rw, g_rh, 1, D3DUSAGE_RENDERTARGET,
                                            D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT,
                                            &g_eyes[i].tex, NULL);
        if (FAILED(hr)) { glog("CreateTexture eye %d hr=0x%08lx", i, (unsigned long)hr); return 0; }
        hr = IDirect3DTexture9_GetSurfaceLevel(g_eyes[i].tex, 0, &g_eyes[i].surf);
        if (FAILED(hr)) { glog("GetSurfaceLevel eye %d", i); return 0; }

        hr = IDirect3DTexture9_QueryInterface(g_eyes[i].tex, &IID_ID3D9VkInteropTexture,
                                              (void **)&g_eyes[i].vktex);
        if (FAILED(hr) || !g_eyes[i].vktex) {
            hr = IDirect3DSurface9_QueryInterface(g_eyes[i].surf, &IID_ID3D9VkInteropTexture,
                                                  (void **)&g_eyes[i].vktex);
        }
        if (FAILED(hr) || !g_eyes[i].vktex) {
            glog("no ID3D9VkInteropTexture for eye %d (0x%08lx)", i, (unsigned long)hr);
            return 0;
        }

        memset(&g_eyes[i].info, 0, sizeof(g_eyes[i].info));
        g_eyes[i].info.sType = BO1VR_VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        hr = g_eyes[i].vktex->lpVtbl->GetVulkanImageInfo(g_eyes[i].vktex, &g_eyes[i].image,
                                                        &g_eyes[i].layout, &g_eyes[i].info);
        if (FAILED(hr) || !g_eyes[i].image) { glog("no VkImage for eye %d", i); return 0; }
        if (g_eyes[i].info.extent.width != g_rw || g_eyes[i].info.extent.height != g_rh) {
            glog("eye %d VkImage extent %ux%u != requested %ux%u -- the interop struct "
                 "layout is wrong, not just the values", i,
                 g_eyes[i].info.extent.width, g_eyes[i].info.extent.height, g_rw, g_rh);
            return 0;
        }
        glog("eye %d: VkImage=0x%016llx layout=%d %ux%u fmt=%d", i,
             (unsigned long long)g_eyes[i].image, (int)g_eyes[i].layout,
             g_eyes[i].info.extent.width, g_eyes[i].info.extent.height,
             (int)g_eyes[i].info.format);
    }
    return 1;
}

static void release_eyes(void)
{
    int i;
    for (i = 0; i < 2; i++) {
        if (g_eyes[i].vktex) { g_eyes[i].vktex->lpVtbl->Release(g_eyes[i].vktex); }
        if (g_eyes[i].surf)  IDirect3DSurface9_Release(g_eyes[i].surf);
        if (g_eyes[i].tex)   IDirect3DTexture9_Release(g_eyes[i].tex);
        memset(&g_eyes[i], 0, sizeof(g_eyes[i]));
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
static volatile LONG g_pose_ticks;

static DWORD WINAPI pose_thread(LPVOID p)
{
    (void)p;
    glog("pose thread up (WaitGetPoses lives here, not on the render thread)");
    for (;;) {
        if (!g_comp) break;
        g_comp->WaitGetPoses(g_rposes, 64, g_gposes, 64);
        InterlockedIncrement(&g_pose_ticks);
    }
    return 0;
}

static void start_pose_thread(void)
{
    DWORD tid;
    if (g_pose_thread || !g_comp) return;
    /* Started from the render thread at first frame, never from DllMain:
     * creating a thread under the loader lock is the deadlock src/dllmain.c
     * warns about. */
    g_pose_thread = CreateThread(NULL, 0, pose_thread, NULL, 0, &tid);
    if (!g_pose_thread) glog("CreateThread for poses failed (err=%lu)", GetLastError());
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

    if (g_state != 1 || !g_dev || eye < 0 || eye > 1)
        return;                               /* not up yet -- first frames */

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
    if (g_have_crop && g_resolve) {
        hr = IDirect3DDevice9_StretchRect(g_dev, rt, NULL, g_resolve, NULL, D3DTEXF_NONE);
        if (SUCCEEDED(hr))
            hr = IDirect3DDevice9_StretchRect(g_dev, g_resolve, &g_crop,
                                              g_eyes[eye].surf, NULL, D3DTEXF_LINEAR);
    } else {
        hr = IDirect3DDevice9_StretchRect(g_dev, rt, NULL, g_eyes[eye].surf, NULL,
                                          D3DTEXF_LINEAR);
    }
    if (FAILED(hr)) {
        if (g_captured < 4) glog("capture eye %d StretchRect hr=0x%08lx", eye, (unsigned long)hr);
    } else {
        InterlockedIncrement(&g_captured);
    }
    IDirect3DSurface9_Release(rt);
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

static void submit_eye(int i)
{
    bo1vr_VkImageSubresourceRange sub;
    struct VRVulkanTextureData_t vkdata;
    struct Texture_t tex;
    EVRCompositorError ce;

    sub.aspectMask     = BO1VR_VK_IMAGE_ASPECT_COLOR_BIT;
    sub.baseMipLevel   = 0;
    sub.levelCount     = g_eyes[i].info.mipLevels;
    sub.baseArrayLayer = 0;
    sub.layerCount     = g_eyes[i].info.arrayLayers;

    /* Exactly the order Proton's own D3D11 path uses
     * (vrcompositor_manual.c load_compositor_texture_dxvk). */
    g_vkdev->lpVtbl->TransitionTextureLayout(g_vkdev, g_eyes[i].vktex, &sub,
                                             g_eyes[i].layout,
                                             BO1VR_VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    g_vkdev->lpVtbl->FlushRenderingCommands(g_vkdev);
    g_vkdev->lpVtbl->LockSubmissionQueue(g_vkdev);

    memset(&vkdata, 0, sizeof(vkdata));
    vkdata.m_nImage            = g_eyes[i].image;
    vkdata.m_pDevice           = (struct VkDevice_T *)g_vkdevice;
    vkdata.m_pPhysicalDevice   = (struct VkPhysicalDevice_T *)g_vkphys;
    vkdata.m_pInstance         = (struct VkInstance_T *)g_vkinst;
    vkdata.m_pQueue            = (struct VkQueue_T *)g_vkqueue;
    vkdata.m_nQueueFamilyIndex = g_qfamily;
    vkdata.m_nWidth            = g_eyes[i].info.extent.width;
    vkdata.m_nHeight           = g_eyes[i].info.extent.height;
    vkdata.m_nFormat           = (uint32_t)g_eyes[i].info.format;
    vkdata.m_nSampleCount      = 1;

    tex.handle      = &vkdata;
    tex.eType       = ETextureType_TextureType_Vulkan;
    tex.eColorSpace = EColorSpace_ColorSpace_Auto;

    /* pBounds is NULL and must stay NULL: xrizer ignores it (Exp. 6), so it is
     * not a way to slice one texture into two eyes -- it would send the whole
     * thing to both. */
    ce = g_comp->Submit(i == 0 ? EVREye_Eye_Left : EVREye_Eye_Right, &tex, NULL,
                        EVRSubmitFlags_Submit_Default);

    g_vkdev->lpVtbl->ReleaseSubmissionQueue(g_vkdev);
    g_vkdev->lpVtbl->TransitionTextureLayout(g_vkdev, g_eyes[i].vktex, &sub,
                                             BO1VR_VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                             g_eyes[i].layout);

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
    static struct TrackedDevicePose_t rposes[64], gposes[64];
    HRESULT hr;
    LONG n;
    int i;

    if (g_state < 0 || !g_dev)
        return;

    if (g_state == 0) {
        g_state = -1;                       /* latch: try once, never per-frame */
        if (!vr_init())         { glog("VR init failed -- staying out of the way"); return; }
        if (!interop_init(g_dev)) { glog("interop init failed -- staying out of the way"); return; }
        g_state = 1;
        start_pose_thread();
        glog("PIPE LIVE: game frames -> compositor (alternate-eye)");
    }

    n = InterlockedIncrement(&g_frames);

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
        if (SUCCEEDED(IDirect3DSurface9_GetDesc(bb, &d)))
            glog("source backbuffer %ux%u fmt=%d MULTISAMPLE=%d pool=%d -> per-eye %ux%u",
                 d.Width, d.Height, (int)d.Format, (int)d.MultiSampleType, (int)d.Pool,
                 g_rw, g_rh);
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
            hr = IDirect3DDevice9_StretchRect(g_dev, bb, NULL, g_eyes[i].surf, NULL,
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
        hr = IDirect3DDevice9_StretchRect(g_dev, bb, NULL, g_eyes[g_cur_eye].surf, NULL,
                                          D3DTEXF_LINEAR);
        if (FAILED(hr)) {
            if (n < 3) glog("StretchRect eye %d hr=0x%08lx", g_cur_eye, (unsigned long)hr);
            IDirect3DSurface9_Release(bb);
            return;
        }
    }

    for (i = 0; i < 2; i++)
        submit_eye(i);

    g_comp->PostPresentHandoff();
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
        glog("frame %ld: %ld eye submits, %ld pose ticks, %ld flat frames (%ld%%)",
             n, g_submitted, g_pose_ticks, g_flat_frames,
             n ? (g_flat_frames * 100) / n : 0);
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

static HRESULT WINAPI my_sc_present(IDirect3DSwapChain9 *sc, const RECT *a, const RECT *b,
                                    HWND w, const RGNDATA *d, DWORD f)
{
    do_frame(sc);
    /* Call the real Present LAST: the game's own picture still goes to the
     * monitor exactly as before. Nothing above changes the render target, the
     * viewport or any state -- StretchRect and the interop calls do not. */
    return real_sc_present(sc, a, b, w, d, f);
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
    HRESULT hr = real_createdev(self, adapter, type, focus, flags, pp, out);
    if (SUCCEEDED(hr) && out && *out) {
        glog("CreateDevice adapter=%u %ux%u windowed=%d", adapter,
             pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0,
             pp ? (int)pp->Windowed : -1);
        hook_device(*out);
    }
    return hr;
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
    if (MH_CreateHook(fn, (void *)my_create9, (void **)&real_create9) == MH_OK &&
        MH_EnableHook(fn) == MH_OK)
        glog("armed at Direct3DCreate9=%p", fn);
    else
        glog("FAILED to hook Direct3DCreate9");
    return TRUE;
}
