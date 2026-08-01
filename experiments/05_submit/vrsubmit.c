/* experiments/05_submit/vrsubmit.c
 *
 * EXPERIMENT 5 -- GET A TEXTURE TO THE COMPOSITOR
 *
 * Experiment 4 proved tracking: a 32-bit mingw DLL under Proton's new-WoW64
 * gets a live IVRCompositor_029 FnTable and a valid HMD pose. It did not
 * submit a frame. This one does, and it is the experiment that decides whether
 * stereo *rendering* is feasible.
 *
 * The chain under test, on top of Experiment 4's:
 *
 *   IDirect3DDevice9 (DXVK d3d9.dll, 32-bit PE)
 *     -> ID3D9VkInteropDevice  / ID3D9VkInteropTexture       (DXVK interop)
 *     -> VkImage + VkInstance/VkPhysicalDevice/VkDevice/VkQueue (winevulkan PE handles)
 *     -> VRVulkanTextureData_t, Texture_t{ eType = TextureType_Vulkan }
 *     -> IVRCompositor_029::Submit
 *     -> Proton vrclient (unwraps the PE Vulkan handles to native ones)
 *     -> xrizer -> OpenXR -> monado-service
 *
 * WHY Vulkan AND NOT TextureType_DirectX -- this is the load-bearing finding:
 *
 *   Proton's vrclient DOES translate a DirectX texture to Vulkan for you, in
 *   vrclient_x64/vrcompositor_manual.c: load_compositor_texture() switches on
 *   texture->eType and for TextureType_DirectX calls load_compositor_texture_dxvk(),
 *   which does
 *
 *       QueryInterface( texture_iface, &IID_IDXGIVkInteropSurface, ... )
 *
 *   IDXGIVkInteropSurface is DXVK's *DXGI/D3D11* interop interface. A D3D9
 *   surface does not implement it -- DXVK's D3D9 exposes ID3D9VkInteropTexture
 *   instead -- so that QueryInterface fails, vrclient logs
 *   "Invalid D3D11 texture %p." and passes the raw IDirect3DSurface9 pointer
 *   through untranslated. xrizer then rejects it, because
 *   SupportedBackend::is_texture_type_supported() accepts only Vulkan and
 *   OpenGL. Proton's D3D9-VR path therefore does not exist; the app has to do
 *   the D3D9 -> Vulkan interop itself. That is what this file does, mirroring
 *   Proton's own D3D11 sequence step for step (transition to
 *   TRANSFER_SRC_OPTIMAL, flush, lock queue, submit, unlock, transition back).
 *
 * AND NOT openvr_api_dxvk.dll: that DLL is a byte-for-byte plain build of
 * Valve's openvr_api.dll (its 18 exports are exactly openvr_api's, it contains
 * no Vulkan/DXVK/D3D symbols or strings, and it refers to itself internally as
 * "openvr_api.dll"). DXVK loads it under that name only to avoid clashing with
 * a game's own copy, and only to call GetVulkanInstanceExtensionsRequired /
 * GetVulkanDeviceExtensionsRequired before creating its VkInstance/VkDevice
 * (dxvk src/dxvk/dxvk_openvr.cpp). It performs no texture interop whatsoever.
 * Step 3 below measures what actually gets loaded under that name.
 *
 * PASS CONDITION: frames we rendered demonstrably reaching monado-service.
 * A VRCompositorError_None from Submit is NOT accepted as proof on its own --
 * run.sh cross-checks the Monado journal, and captures Monado's compositor
 * window so the per-eye colours can be seen.
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

/* ------------------------------------------------------------------ log --- */

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
#define STEP(s) P("--- step: %s", s)

/* --------------------------------------------------------------- openvr --- */

typedef intptr_t    (__cdecl *pfn_GetGenericInterface)(const char *, EVRInitError *);
typedef intptr_t    (__cdecl *pfn_InitInternal2)(EVRInitError *, EVRApplicationType, const char *);
typedef void        (__cdecl *pfn_ShutdownInternal)(void);
typedef const char *(__cdecl *pfn_ErrSymbol)(EVRInitError);

static pfn_GetGenericInterface fnGGI;
static pfn_InitInternal2       fnInit2;
static pfn_ShutdownInternal    fnShutdown;
static pfn_ErrSymbol           fnErrSym;

static const char *esym(EVRInitError e) { return fnErrSym ? fnErrSym(e) : "?"; }

static const char *comperr(EVRCompositorError e)
{
    switch ((int)e) {
    case 0:   return "None";
    case 1:   return "RequestFailed";
    case 100: return "IncompatibleVersion";
    case 101: return "DoNotHaveFocus";
    case 102: return "InvalidTexture";
    case 103: return "IsNotSceneApplication";
    case 104: return "TextureIsOnWrongDevice";
    case 105: return "TextureUsesUnsupportedFormat";
    case 106: return "SharedTexturesNotSupported";
    case 107: return "IndexOutOfRange";
    case 108: return "AlreadySubmitted";
    case 109: return "InvalidBounds";
    case 110: return "AlreadySet";
    default:  return "?";
    }
}

/* ------------------------------------------------------------------ d3d9 -- */

typedef IDirect3D9 *(WINAPI *pfn_Direct3DCreate9)(UINT);

/* Reports which file a given already-loaded module actually came from, and how
 * big it is. The point is the openvr_api_dxvk.dll directory-swap bug: "it
 * loaded" is not the same claim as "the right bitness loaded", and the byte
 * size distinguishes them unambiguously (i386 631960 vs x86_64 836760). */
static void report_module_file(const char *label, HMODULE h)
{
    char path[MAX_PATH];
    HANDLE f;
    LARGE_INTEGER sz;
    WORD machine = 0;

    if (!h) { P("  %-22s NOT LOADED (err=%lu)", label, GetLastError()); return; }
    if (!GetModuleFileNameA(h, path, sizeof(path))) {
        P("  %-22s @%p (GetModuleFileNameA failed)", label, (void *)h);
        return;
    }
    f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        IMAGE_DOS_HEADER dos; DWORD got; LONG lfa;
        if (GetFileSizeEx(f, &sz) && ReadFile(f, &dos, sizeof(dos), &got, NULL) && got == sizeof(dos)) {
            lfa = dos.e_lfanew;
            if (SetFilePointer(f, lfa + 4, NULL, FILE_BEGIN) != INVALID_SET_FILE_POINTER)
                ReadFile(f, &machine, sizeof(machine), &got, NULL);
        } else sz.QuadPart = -1;
        CloseHandle(f);
    } else sz.QuadPart = -1;

    P("  %-22s @%p  %s", label, (void *)h, path);
    P("  %-22s size=%lld bytes, PE machine=0x%04x (%s)", "", (long long)sz.QuadPart, machine,
      machine == 0x014c ? "i386" : machine == 0x8664 ? "x86_64" : "?");
}

/* ------------------------------------------------------------------------- */

struct eye_target {
    IDirect3DTexture9      *tex;
    IDirect3DSurface9      *surf;
    ID3D9VkInteropTexture  *vktex;
    bo1vr_VkImage           image;
    bo1vr_VkEnum            layout;      /* the layout DXVK keeps it in */
    bo1vr_VkImageCreateInfo info;
};

static HWND make_window(void)
{
    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = DefWindowProcA;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.lpszClassName = "bo1vr_exp05";
    RegisterClassA(&wc);
    return CreateWindowExA(0, "bo1vr_exp05", "bo1vr exp05", WS_OVERLAPPEDWINDOW,
                           0, 0, 320, 240, NULL, NULL, wc.hInstance, NULL);
}

/* Read a couple of pixels back off a render target. Submit returning None
 * proves a call happened; it says nothing about the texture having the content
 * we think it has. This closes that gap: it shows the image actually holds the
 * per-eye colour and the moving white marker, so "the frames Monado received"
 * are demonstrably the frames we drew. GetRenderTargetData is the D3D9 way to
 * pull a D3DPOOL_DEFAULT render target into lockable system memory. */
static int readback_probe(IDirect3DDevice9 *dev, IDirect3DSurface9 *rt,
                          UINT w, UINT h, const char *tag,
                          DWORD *out_bg, DWORD *out_marker)
{
    IDirect3DSurface9 *sys = NULL;
    D3DLOCKED_RECT lr;
    HRESULT hr;
    DWORD bg = 0, mk = 0;
    int ok = 0;

    hr = IDirect3DDevice9_CreateOffscreenPlainSurface(dev, w, h, D3DFMT_X8R8G8B8,
                                                      D3DPOOL_SYSTEMMEM, &sys, NULL);
    if (FAILED(hr)) { P("  readback %s: CreateOffscreenPlainSurface 0x%08lx", tag, (unsigned long)hr); goto done; }
    hr = IDirect3DDevice9_GetRenderTargetData(dev, rt, sys);
    if (FAILED(hr)) { P("  readback %s: GetRenderTargetData 0x%08lx", tag, (unsigned long)hr); goto done; }
    hr = IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY);
    if (FAILED(hr)) { P("  readback %s: LockRect 0x%08lx", tag, (unsigned long)hr); goto done; }

    /* (8,8) is outside the marker band (which spans x = w/4 .. 3w/4);
     * (w/2, h/16) is inside it on frame 0. */
    bg = *(const DWORD *)((const char *)lr.pBits + 8 * lr.Pitch + 8 * 4) & 0x00FFFFFFu;
    mk = *(const DWORD *)((const char *)lr.pBits + (h / 16) * lr.Pitch + (w / 2) * 4) & 0x00FFFFFFu;
    IDirect3DSurface9_UnlockRect(sys);
    P("  readback %s: background(8,8)=0x%06lx  marker(%u,%u)=0x%06lx",
      tag, (unsigned long)bg, w / 2, h / 16, (unsigned long)mk);
    ok = 1;
done:
    if (sys) IDirect3DSurface9_Release(sys);
    if (out_bg) *out_bg = bg;
    if (out_marker) *out_marker = mk;
    return ok;
}

static const char *vkfmt(bo1vr_VkEnum f)
{
    switch (f) {
    case BO1VR_VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case BO1VR_VK_FORMAT_R8G8B8A8_SRGB:  return "R8G8B8A8_SRGB";
    case BO1VR_VK_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case BO1VR_VK_FORMAT_B8G8R8A8_SRGB:  return "B8G8R8A8_SRGB";
    default: return "?";
    }
}

__declspec(dllexport) int vrsubmit_run(void)
{
    HMODULE hovr, hd3d9, hdxvkovr;
    EVRInitError err;
    intptr_t token, iface;
    struct VR_IVRCompositor_FnTable *comp = NULL;
    struct IVRSystem_023_FnTable *sys = NULL;
    pfn_Direct3DCreate9 pD3DCreate9;
    IDirect3D9 *d3d = NULL;
    IDirect3DDevice9 *dev = NULL;
    ID3D9VkInteropInterface *vkiface = NULL;
    ID3D9VkInteropDevice *vkdev = NULL;
    bo1vr_VkInstance vkinstance = NULL, vkinstance2 = NULL;
    bo1vr_VkPhysicalDevice vkphys = NULL;
    bo1vr_VkDevice vkdevice = NULL;
    bo1vr_VkQueue vkqueue = NULL;
    uint32_t queue_index = 0, queue_family = 0;
    struct eye_target eyes[2];
    D3DPRESENT_PARAMETERS pp;
    HWND hwnd;
    HRESULT hr;
    uint32_t rw = 0, rh = 0;
    int fail = 0, frames = 0, submits_ok = 0, i, f;
    int nframes;
    const DWORD eye_colour[2] = { D3DCOLOR_XRGB(220, 20, 20),    /* LEFT  = red   */
                                  D3DCOLOR_XRGB(20, 200, 40) };  /* RIGHT = green */

    memset(eyes, 0, sizeof(eyes));

    {
        const char *lp = getenv("BO1VR_LOG");
        g_log = fopen(lp ? lp : "vrsubmit.log", "w");
    }
    {
        const char *n = getenv("BO1VR_FRAMES");
        nframes = n ? atoi(n) : 600;
        if (nframes < 1) nframes = 1;
    }

    P("=== EXPERIMENT 5: D3D9/DXVK texture -> IVRCompositor::Submit ===");
    P("built with GCC %s, pointer size %d, pid %lu, frames=%d",
      __VERSION__, (int)sizeof(void *), GetCurrentProcessId(), nframes);

    /* ---- 1. OpenVR, exactly as Experiment 4 left it ------------------- */
    STEP("1. LoadLibraryA(openvr_api.dll) + VR_InitInternal2");
    hovr = LoadLibraryA("openvr_api.dll");
    if (!hovr) { P("FAIL: no openvr_api.dll, err=%lu", GetLastError()); return 1; }
    report_module_file("openvr_api.dll", hovr);

    fnGGI      = (pfn_GetGenericInterface)(void *)GetProcAddress(hovr, "VR_GetGenericInterface");
    fnInit2    = (pfn_InitInternal2)      (void *)GetProcAddress(hovr, "VR_InitInternal2");
    fnShutdown = (pfn_ShutdownInternal)   (void *)GetProcAddress(hovr, "VR_ShutdownInternal");
    fnErrSym   = (pfn_ErrSymbol)          (void *)GetProcAddress(hovr, "VR_GetVRInitErrorAsSymbol");
    if (!fnGGI || !fnInit2) { P("FAIL: missing openvr entry points"); return 2; }

    err = (EVRInitError)0xDEADBEEF;
    token = fnInit2(&err, EVRApplicationType_VRApplication_Scene, "");
    P("VR_InitInternal2 -> token=%p err=%d (%s)", (void *)token, (int)err, esym(err));
    if (err != EVRInitError_VRInitError_None) { P("FAIL: VR_InitInternal2"); return 3; }

    err = (EVRInitError)0xDEADBEEF;
    iface = fnGGI("FnTable:" "IVRCompositor_029", &err);
    P("IVRCompositor_029 FnTable = %p (err %d)", (void *)iface, (int)err);
    if (!iface) { P("FAIL: NULL compositor FnTable"); fnShutdown(); return 4; }
    comp = (struct VR_IVRCompositor_FnTable *)iface;

    err = (EVRInitError)0xDEADBEEF;
    iface = fnGGI("FnTable:" "IVRSystem_023", &err);
    P("IVRSystem_023 FnTable    = %p (err %d)", (void *)iface, (int)err);
    sys = (struct IVRSystem_023_FnTable *)iface;
    if (sys) sys->GetRecommendedRenderTargetSize(&rw, &rh);
    if (!rw || !rh) { rw = 896; rh = 1007; P("NOTE: falling back to %ux%u", rw, rh); }
    P("PASS-1: OpenVR up; per-eye render target %u x %u", rw, rh);

    /* ---- 2. what does the name openvr_api_dxvk.dll actually resolve to? --
     * README Correction B: Proton 10.0-4b copies the i386 build to system32
     * and the x86_64 build to syswow64, i.e. swapped. A 32-bit process
     * resolves the bare name from syswow64. Measure both, do not assume:
     *   2a the system copy, by absolute path -- states the prefix's condition;
     *   2b the bare name, which is what DXVK actually calls. run.sh stages a
     *      correct i386 build next to the exe, and the app directory precedes
     *      the system directories in the search order, so 2b should succeed
     *      even while 2a fails.
     * Fixing it *in the prefix* does not stick: Proton's own script re-copies
     * both files at every launch (proton lines 1077-1079). */
    STEP("2. probe openvr_api_dxvk.dll (README Correction B)");
    {
        char sysdir[MAX_PATH], syspath[MAX_PATH];
        HMODULE hsys;
        UINT n = GetSystemDirectoryA(sysdir, sizeof(sysdir));  /* -> syswow64 for a 32-bit process */
        if (n) {
            snprintf(syspath, sizeof(syspath), "%s\\openvr_api_dxvk.dll", sysdir);
            P("  2a. LoadLibraryA(\"%s\")", syspath);
            hsys = LoadLibraryA(syspath);
            report_module_file("  system copy", hsys);
            if (!hsys)
                P("  -> the swapped-directory bug is PRESENT: the system copy a 32-bit process "
                  "resolves is the x86_64 build (LoadLibrary err=%lu, 193 = BAD_EXE_FORMAT)",
                  GetLastError());
            else
                P("  -> the system copy is loadable from a 32-bit process");
        }
    }
    P("  2b. LoadLibraryA(\"openvr_api_dxvk.dll\")  [bare name: app dir first]");
    hdxvkovr = LoadLibraryA("openvr_api_dxvk.dll");
    report_module_file("  effective", hdxvkovr);
    if (!hdxvkovr)
        P("  -> NOT loadable under the bare name: DXVK's dxvk_openvr.cpp would log "
          "\"OpenVR: Failed to locate module\"");
    else if (!GetProcAddress(hdxvkovr, "VR_GetGenericInterface"))
        P("  -> loaded but has no VR_GetGenericInterface: wrong file");
    else
        P("  -> loadable 32-bit build, VR_GetGenericInterface present: workaround confirmed");

    /* ---- 3. D3D9 through DXVK ---------------------------------------- */
    STEP("3. LoadLibraryA(d3d9.dll) + Direct3DCreate9");
    hd3d9 = LoadLibraryA("d3d9.dll");
    if (!hd3d9) { P("FAIL: no d3d9.dll, err=%lu", GetLastError()); fail = 1; goto out; }
    report_module_file("d3d9.dll", hd3d9);

    pD3DCreate9 = (pfn_Direct3DCreate9)(void *)GetProcAddress(hd3d9, "Direct3DCreate9");
    if (!pD3DCreate9) { P("FAIL: no Direct3DCreate9"); fail = 1; goto out; }
    d3d = pD3DCreate9(D3D_SDK_VERSION);
    P("Direct3DCreate9 -> %p", (void *)d3d);
    if (!d3d) { P("FAIL: Direct3DCreate9 returned NULL"); fail = 1; goto out; }

    STEP("3b. QueryInterface(IDirect3D9 -> ID3D9VkInteropInterface)");
    hr = IDirect3D9_QueryInterface(d3d, &IID_ID3D9VkInteropInterface, (void **)&vkiface);
    P("QI ID3D9VkInteropInterface -> hr=0x%08lx ptr=%p", (unsigned long)hr, (void *)vkiface);
    if (FAILED(hr) || !vkiface) {
        P("FAIL: this d3d9.dll is not DXVK (wined3d has no Vulkan interop interface)");
        fail = 1; goto out;
    }
    vkiface->lpVtbl->GetInstanceHandle(vkiface, &vkinstance);
    P("PASS-2: d3d9.dll is DXVK; VkInstance = %p", (void *)vkinstance);
    if (!vkinstance) { P("FAIL: NULL VkInstance"); fail = 1; goto out; }

    STEP("3c. CreateDevice");
    hwnd = make_window();
    P("hwnd = %p", (void *)hwnd);
    if (!hwnd) { P("FAIL: CreateWindowExA, err=%lu", GetLastError()); fail = 1; goto out; }
    memset(&pp, 0, sizeof(pp));
    pp.Windowed              = TRUE;
    pp.SwapEffect            = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat      = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth       = 320;
    pp.BackBufferHeight      = 240;
    pp.BackBufferCount       = 1;
    pp.hDeviceWindow         = hwnd;
    pp.PresentationInterval  = D3DPRESENT_INTERVAL_IMMEDIATE;
    hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                 D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
                                 &pp, &dev);
    P("CreateDevice -> hr=0x%08lx dev=%p", (unsigned long)hr, (void *)dev);
    if (FAILED(hr) || !dev) { P("FAIL: CreateDevice"); fail = 1; goto out; }
    P("PASS-3: live D3D9 device on DXVK inside a 32-bit new-WoW64 process");

    STEP("3d. QueryInterface(IDirect3DDevice9 -> ID3D9VkInteropDevice)");
    hr = IDirect3DDevice9_QueryInterface(dev, &IID_ID3D9VkInteropDevice, (void **)&vkdev);
    P("QI ID3D9VkInteropDevice -> hr=0x%08lx ptr=%p", (unsigned long)hr, (void *)vkdev);
    if (FAILED(hr) || !vkdev) { P("FAIL: no ID3D9VkInteropDevice"); fail = 1; goto out; }

    vkdev->lpVtbl->GetVulkanHandles(vkdev, &vkinstance2, &vkphys, &vkdevice);
    vkdev->lpVtbl->GetSubmissionQueue(vkdev, &vkqueue, &queue_index, &queue_family);
    P("  VkInstance       = %p  (matches IDirect3D9's: %s)", (void *)vkinstance2,
      vkinstance2 == vkinstance ? "yes" : "NO");
    P("  VkPhysicalDevice = %p", (void *)vkphys);
    P("  VkDevice         = %p", (void *)vkdevice);
    P("  VkQueue          = %p  queueIndex=%u queueFamilyIndex=%u",
      (void *)vkqueue, queue_index, queue_family);
    if (!vkphys || !vkdevice || !vkqueue) {
        P("FAIL: a NULL Vulkan handle came back -- a plausible-looking NULL is exactly the "
          "false pass this project has already been burned by");
        fail = 1; goto out;
    }
    P("PASS-4: Vulkan handles obtained from the D3D9 device");

    /* ---- 4. one render target per eye, and its VkImage ---------------- */
    STEP("4. CreateTexture(RENDERTARGET) per eye + GetVulkanImageInfo");
    for (i = 0; i < 2; i++) {
        hr = IDirect3DDevice9_CreateTexture(dev, rw, rh, 1, D3DUSAGE_RENDERTARGET,
                                            D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT,
                                            &eyes[i].tex, NULL);
        if (FAILED(hr)) { P("FAIL: CreateTexture eye %d hr=0x%08lx", i, (unsigned long)hr); fail = 1; goto out; }
        hr = IDirect3DTexture9_GetSurfaceLevel(eyes[i].tex, 0, &eyes[i].surf);
        if (FAILED(hr)) { P("FAIL: GetSurfaceLevel eye %d", i); fail = 1; goto out; }

        hr = IDirect3DTexture9_QueryInterface(eyes[i].tex, &IID_ID3D9VkInteropTexture,
                                              (void **)&eyes[i].vktex);
        if (FAILED(hr) || !eyes[i].vktex) {
            P("  eye %d: QI on the texture failed (0x%08lx), trying the surface", i, (unsigned long)hr);
            hr = IDirect3DSurface9_QueryInterface(eyes[i].surf, &IID_ID3D9VkInteropTexture,
                                                  (void **)&eyes[i].vktex);
        }
        if (FAILED(hr) || !eyes[i].vktex) {
            P("FAIL: no ID3D9VkInteropTexture for eye %d (0x%08lx)", i, (unsigned long)hr);
            fail = 1; goto out;
        }

        memset(&eyes[i].info, 0, sizeof(eyes[i].info));
        eyes[i].info.sType = BO1VR_VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        hr = eyes[i].vktex->lpVtbl->GetVulkanImageInfo(eyes[i].vktex, &eyes[i].image,
                                                       &eyes[i].layout, &eyes[i].info);
        P("  eye %d: GetVulkanImageInfo hr=0x%08lx VkImage=0x%016llx layout=%d",
          i, (unsigned long)hr, (unsigned long long)eyes[i].image, (int)eyes[i].layout);
        if (FAILED(hr) || !eyes[i].image) { P("FAIL: no VkImage for eye %d", i); fail = 1; goto out; }
        P("    extent=%ux%ux%u mips=%u layers=%u samples=%d format=%d (%s) usage=0x%08x tiling=%d",
          eyes[i].info.extent.width, eyes[i].info.extent.height, eyes[i].info.extent.depth,
          eyes[i].info.mipLevels, eyes[i].info.arrayLayers, (int)eyes[i].info.samples,
          (int)eyes[i].info.format, vkfmt(eyes[i].info.format),
          (unsigned)eyes[i].info.usage, (int)eyes[i].info.tiling);
        if (eyes[i].info.extent.width != rw || eyes[i].info.extent.height != rh) {
            P("FAIL: VkImage extent does not match the texture we asked for -- the interop "
              "struct layout is wrong, not just the values");
            fail = 1; goto out;
        }
        if (!(eyes[i].info.usage & BO1VR_VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
            P("    WARN: image lacks TRANSFER_SRC usage; xrizer copies from it as a transfer source");
    }
    P("PASS-5: a real VkImage behind each eye's D3D9 render target, extents agree");

    /* ---- 4b. the control: submit the D3D9 surface as TextureType_DirectX --
     * Opt-in (BO1VR_TRY_DIRECTX=1) because it is EXPECTED TO KILL THE PROCESS.
     * This is the measurement behind the claim at the top of this file. What
     * should be visible in the wine trace:
     *   1. Proton's vrclient takes the DirectX path
     *      (vrcompositor_manual.c: load_compositor_texture_dxvk),
     *      QueryInterface()s our surface for IID_IDXGIVkInteropSurface -- the
     *      DXGI/D3D11 interop IID, which DXVK's D3D9 objects do not implement --
     *      fails, and logs "Invalid D3D11 texture %p."; it then forwards the
     *      raw IDirect3DSurface9 pointer with eType still DirectX.
     *   2. xrizer's SupportedBackend::new() has no arm for ETextureType::DirectX
     *      and ends in `other => panic!("Unsupported texture type: {other:?}")`.
     *      Per Exp. 4, a Rust panic here unwinds into a frame Wine cannot
     *      dispatch and the process dies.
     * i.e. Proton's D3D11 VR bridge does not extend to D3D9, in either half. */
    if (getenv("BO1VR_TRY_DIRECTX")) {
        static struct TrackedDevicePose_t r0[64], g0[64];
        struct Texture_t dxtex;
        EVRCompositorError ce;
        STEP("4b. CONTROL: Submit(eType = TextureType_DirectX, handle = IDirect3DSurface9*)");
        P("  expect: vrclient \"Invalid D3D11 texture\", then an xrizer panic that kills us");
        comp->WaitGetPoses(r0, 64, g0, 64);
        dxtex.handle      = eyes[0].surf;
        dxtex.eType       = ETextureType_TextureType_DirectX;
        dxtex.eColorSpace = EColorSpace_ColorSpace_Auto;
        ce = comp->Submit(EVREye_Eye_Left, &dxtex, NULL, EVRSubmitFlags_Submit_Default);
        P("  SURVIVED: Submit(DirectX) -> %d (%s)", (int)ce, comperr(ce));
        P("  (if you are reading this line, the D3D9 DirectX path did NOT kill the process)");
        fail = ce == EVRCompositorError_VRCompositorError_None ? 0 : 1;
        goto out;
    }

    /* ---- 5. the frame loop ------------------------------------------- */
    STEP("5. frame loop: WaitGetPoses -> render -> Submit(L) -> Submit(R) -> PostPresentHandoff");
    for (f = 0; f < nframes; f++) {
        static struct TrackedDevicePose_t render[64], game[64];
        EVRCompositorError ce;
        MSG msg;

        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }

        ce = comp->WaitGetPoses(render, 64, game, 64);
        if (ce != EVRCompositorError_VRCompositorError_None) {
            P("frame %d: WaitGetPoses -> %d (%s)", f, (int)ce, comperr(ce));
            if (f == 0) { fail = 1; goto out; }
        }

        for (i = 0; i < 2; i++) {
            struct VRVulkanTextureData_t vkdata;
            struct Texture_t tex;
            bo1vr_VkImageSubresourceRange sub;
            D3DRECT marker;

            /* Render: solid per-eye colour plus a white block that walks down
             * the image, so successive frames are distinguishable and a
             * left/right mix-up is obvious at a glance. */
            IDirect3DDevice9_SetRenderTarget(dev, 0, eyes[i].surf);
            IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, eye_colour[i], 1.0f, 0);
            marker.x1 = (LONG)(rw / 4);
            marker.x2 = (LONG)(rw / 4 + rw / 2);
            marker.y1 = (LONG)(((unsigned)f * 7u) % (rh - rh / 8));
            marker.y2 = marker.y1 + (LONG)(rh / 8);
            IDirect3DDevice9_Clear(dev, 1, &marker, D3DCLEAR_TARGET,
                                   D3DCOLOR_XRGB(255, 255, 255), 1.0f, 0);

            if (f == 0) {
                DWORD bg = 0, mk = 0;
                const char *tag = i == 0 ? "eye0/LEFT (want red 0xdc1414 + white)"
                                         : "eye1/RIGHT (want green 0x14c828 + white)";
                readback_probe(dev, eyes[i].surf, rw, rh, tag, &bg, &mk);
                if (bg != (eye_colour[i] & 0x00FFFFFFu) || mk != 0x00FFFFFFu) {
                    P("FAIL: the render target does not contain what we drew "
                      "(bg 0x%06lx vs 0x%06lx, marker 0x%06lx vs 0xffffff)",
                      (unsigned long)bg, (unsigned long)(eye_colour[i] & 0x00FFFFFFu),
                      (unsigned long)mk);
                    fail = 1; goto out;
                }
                if (i == 1)
                    P("PASS-5b: both render targets read back exactly the colours we drew");
            }

            /* Hand the image over, in exactly the order Proton's own D3D11 path
             * uses (vrcompositor_manual.c load_compositor_texture_dxvk). */
            sub.aspectMask     = BO1VR_VK_IMAGE_ASPECT_COLOR_BIT;
            sub.baseMipLevel   = 0;
            sub.levelCount     = eyes[i].info.mipLevels;
            sub.baseArrayLayer = 0;
            sub.layerCount     = eyes[i].info.arrayLayers;

            vkdev->lpVtbl->TransitionTextureLayout(vkdev, eyes[i].vktex, &sub,
                                                   eyes[i].layout,
                                                   BO1VR_VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            vkdev->lpVtbl->FlushRenderingCommands(vkdev);
            vkdev->lpVtbl->LockSubmissionQueue(vkdev);

            memset(&vkdata, 0, sizeof(vkdata));
            vkdata.m_nImage            = eyes[i].image;
            vkdata.m_pDevice           = (struct VkDevice_T *)vkdevice;
            vkdata.m_pPhysicalDevice   = (struct VkPhysicalDevice_T *)vkphys;
            vkdata.m_pInstance         = (struct VkInstance_T *)vkinstance2;
            vkdata.m_pQueue            = (struct VkQueue_T *)vkqueue;
            vkdata.m_nQueueFamilyIndex = queue_family;
            vkdata.m_nWidth            = eyes[i].info.extent.width;
            vkdata.m_nHeight           = eyes[i].info.extent.height;
            vkdata.m_nFormat           = (uint32_t)eyes[i].info.format;
            vkdata.m_nSampleCount      = 1;

            tex.handle      = &vkdata;
            tex.eType       = ETextureType_TextureType_Vulkan;
            tex.eColorSpace = EColorSpace_ColorSpace_Auto;

            ce = comp->Submit(i == 0 ? EVREye_Eye_Left : EVREye_Eye_Right, &tex, NULL,
                              EVRSubmitFlags_Submit_Default);

            vkdev->lpVtbl->ReleaseSubmissionQueue(vkdev);
            vkdev->lpVtbl->TransitionTextureLayout(vkdev, eyes[i].vktex, &sub,
                                                   BO1VR_VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                   eyes[i].layout);

            if (f == 0)
                P("frame 0 eye %d: Submit -> %d (%s)", i, (int)ce, comperr(ce));
            if (ce == EVRCompositorError_VRCompositorError_None) submits_ok++;
            else if (f == 0) {
                P("FAIL: first Submit rejected: %d (%s)", (int)ce, comperr(ce));
                fail = 1; goto out;
            } else if ((f % 100) == 0)
                P("frame %d eye %d: Submit -> %d (%s)", f, i, (int)ce, comperr(ce));
        }

        comp->PostPresentHandoff();
        frames++;
        if (f == 0) P("PASS-6: first stereo pair submitted with VRCompositorError_None");
        if ((f % 100) == 0) P("  ... frame %d done (%d successful submits so far)", f, submits_ok);
    }

    P("submitted %d frames, %d successful eye submits", frames, submits_ok);
    if (submits_ok != frames * 2) {
        P("FAIL: %d of %d eye submits were rejected", frames * 2 - submits_ok, frames * 2);
        fail = 1;
    }

    /* A frame counter maintained on the RUNTIME's side, not ours. xrizer bumps
     * metrics.index once per PostPresentHandoff that actually reaches
     * xrEndFrame (compositor.rs), and reports it as m_nFrameIndex. If Submit
     * were quietly discarding our frames this would still read 0. */
    STEP("5b. comp->GetFrameTiming() -- ask the runtime how many frames it ended");
    {
        struct Compositor_FrameTiming t;
        memset(&t, 0, sizeof(t));
        t.m_nSize = sizeof(t);
        if (comp->GetFrameTiming(&t, 0)) {
            P("runtime-side m_nFrameIndex = %u (we called PostPresentHandoff %d times)",
              t.m_nFrameIndex, frames);
            P("runtime-side m_flSystemTimeInSeconds = %f", t.m_flSystemTimeInSeconds);
            if (t.m_nFrameIndex == 0) {
                P("FAIL: the runtime ended 0 frames -- Submit returned None but nothing was presented");
                fail = 1;
            } else {
                P("PASS-7: the runtime's own frame counter advanced to %u", t.m_nFrameIndex);
            }
        } else {
            P("NOTE: GetFrameTiming returned false");
        }
    }

out:
    /* ORDER MATTERS. xrizer's OpenXR session is created *on* the app's
     * VkInstance/VkDevice -- the ones DXVK owns. Releasing the D3D9 device
     * first destroys them under the runtime's feet, and Cleanup then faults:
     *
     *   warn:vrclient:winIVRClientCore_IVRClientCore_003_Cleanup
     *        IVRClientCore_IVRClientCore_003_Cleanup failed, status 0xc0000005
     *
     * and the process hangs instead of exiting. Measured. Shut the runtime
     * down first, then tear down D3D9. Real mods must do the same. */
    STEP("6. teardown: VR_ShutdownInternal first, then D3D9");
    if (fnShutdown) fnShutdown();
    P("  VR_ShutdownInternal returned");
    for (i = 0; i < 2; i++) {
        if (eyes[i].vktex) eyes[i].vktex->lpVtbl->Release(eyes[i].vktex);
        if (eyes[i].surf)  IDirect3DSurface9_Release(eyes[i].surf);
        if (eyes[i].tex)   IDirect3DTexture9_Release(eyes[i].tex);
    }
    if (vkdev)   vkdev->lpVtbl->Release(vkdev);
    if (vkiface) vkiface->lpVtbl->Release(vkiface);
    if (dev)     IDirect3DDevice9_Release(dev);
    if (d3d)     IDirect3D9_Release(d3d);
    P("  D3D9 released");

    P("=== EXPERIMENT 5 END: %s ===", fail ? "FAIL" : "PASS");
    if (g_log) { fclose(g_log); g_log = NULL; }
    return fail ? 10 : 0;
}

#ifdef BO1VR_ASI
/* Same .asi discipline as Experiment 4: asi_load_all() runs under the loader
 * lock, so DllMain must do nothing but CreateThread and return. */
static DWORD WINAPI vrsubmit_thread(void *arg)
{
    HANDLE done;
    (void)arg;
    vrsubmit_run();
    done = CreateEventA(NULL, TRUE, FALSE, "bo1vr_exp05_done");
    if (done) SetEvent(done);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE i, DWORD r, LPVOID v)
{
    (void)v;
    if (r == DLL_PROCESS_ATTACH) {
        HANDLE t;
        DisableThreadLibraryCalls(i);
        t = CreateThread(NULL, 0, vrsubmit_thread, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
#else
BOOL WINAPI DllMain(HINSTANCE i, DWORD r, LPVOID v) { (void)i;(void)r;(void)v; return TRUE; }
#endif
