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

    if (ce == EVRCompositorError_VRCompositorError_None)
        InterlockedIncrement(&g_submitted);
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
        glog("PIPE LIVE: game frames -> compositor");
    }

    n = InterlockedIncrement(&g_frames);

    /* The compositor's frame clock. This blocks until the runtime wants the
     * next frame, which is the point -- it is what makes our submissions land
     * in step with the headset rather than at whatever rate the game feels
     * like. It also means the game's frame rate is now the HEADSET's. */
    g_comp->WaitGetPoses(rposes, 64, gposes, 64);

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

    for (i = 0; i < 2; i++) {
        /* THE RESOLVE. The back buffer is 4x multisampled (Exp. 10) and cannot
         * be submitted or read directly. StretchRect from a multisampled source
         * to a single-sampled destination performs the resolve; it is also
         * doing the 2560x1440 -> per-eye downscale in the same call, which is
         * free here and would otherwise need a second pass.
         *
         * Both eyes get the same picture on purpose: this experiment proves the
         * pipe, not stereo. Per-eye content needs the camera hook (BAC-281). */
        hr = IDirect3DDevice9_StretchRect(g_dev, bb, NULL, g_eyes[i].surf, NULL,
                                          D3DTEXF_LINEAR);
        if (FAILED(hr)) {
            if (n < 3) glog("StretchRect eye %d hr=0x%08lx", i, (unsigned long)hr);
            IDirect3DSurface9_Release(bb);
            return;
        }
        submit_eye(i);
    }

    g_comp->PostPresentHandoff();
    IDirect3DSurface9_Release(bb);

    if (n == 1 || n == 2 || (n % 600) == 0)
        glog("frame %ld: %ld successful eye submits", n, g_submitted);
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
