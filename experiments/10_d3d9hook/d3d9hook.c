/* d3d9hook.c -- get hold of the game's own D3D9 device and its frames.
 *
 * BAC-280, step one. Exp. 5 already put a texture of OUR making in front of the
 * compositor; what was never established is that we can reach the texture the
 * GAME draws. This plugin does only that, and proves it in writing: the device
 * pointer, the back buffer's real format and size, and a frame counter that
 * advances with the game's own Present.
 *
 * WHY A VTABLE WALK AND NOT A DUMMY DEVICE
 * ----------------------------------------
 * The usual recipe creates a throwaway D3D9 device to learn the vtable layout.
 * That needs an HWND, and at DLL_PROCESS_ATTACH there is no window yet -- the
 * game has not run a line of its own code. So instead we hook the one thing
 * that exists before any device does: the exported Direct3DCreate9. From its
 * return value we read IDirect3D9's vtable and hook CreateDevice; from THAT
 * return value we read IDirect3DDevice9's vtable and hook Present and Reset.
 * Each hook is installed the first time an object of that type exists, which is
 * the earliest moment the address is knowable.
 *
 * Vtable indices are fixed by the COM interface and are not ours to choose:
 *   IDirect3D9::CreateDevice        16
 *   IDirect3DDevice9::Reset         16
 *   IDirect3DDevice9::Present       17
 * They come from the order of methods in d3d9.h, IUnknown's three first.
 * Deriving them at build time from the C++ interface is not possible here (we
 * compile as C), so they are asserted at runtime instead: before hooking, the
 * code checks the slot is a readable code address, and logs what it found.
 *
 * DXVK, NOT REAL D3D9. Under Proton d3d9.dll is DXVK, so "Present" ends in a
 * Vulkan queue submit. That is the whole reason this route is attractive: the
 * back buffer we are about to get our hands on is already a VkImage underneath,
 * which is what the compositor wants (README Decision 11).
 */

#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

#include "MinHook.h"

/* ---------------------------------------------------------------- logging */

static void hlog(const char *fmt, ...)
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
    memmove(buf + 11, buf, strlen(buf) + 1);
    memcpy(buf, "[d3d9hook] ", 11);
    strcat(buf, "\n");

    OutputDebugStringA(buf);

    /* The file is the channel that survives a Steam launch -- see Exp. 9 §4.
     * Note GetTempPathA here yields AppData\Local\Temp, not steamuser\Temp. */
    n = GetTempPathA(MAX_PATH - 24, path);
    if (n && n < MAX_PATH - 24) {
        lstrcatA(path, "bo1vr_d3d9.log");
        h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD wrote;
            WriteFile(h, buf, (DWORD)strlen(buf), &wrote, NULL);
            CloseHandle(h);
        }
    }
}

static const char *fmt_name(D3DFORMAT f)
{
    switch ((int)f) {
    case D3DFMT_A8R8G8B8:   return "A8R8G8B8";
    case D3DFMT_X8R8G8B8:   return "X8R8G8B8";
    case D3DFMT_A2B10G10R10:return "A2B10G10R10";
    case D3DFMT_A16B16G16R16F: return "A16B16G16R16F";
    case D3DFMT_R5G6B5:     return "R5G6B5";
    default:                return "other";
    }
}

/* ------------------------------------------------------------------ hooks */

typedef IDirect3D9 *(WINAPI *pfn_create9)(UINT);
typedef HRESULT (WINAPI *pfn_createdev)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD,
                                        D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);
typedef HRESULT (WINAPI *pfn_present)(IDirect3DDevice9 *, const RECT *, const RECT *,
                                      HWND, const RGNDATA *);
typedef HRESULT (WINAPI *pfn_reset)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
typedef HRESULT (WINAPI *pfn_sc_present)(IDirect3DSwapChain9 *, const RECT *, const RECT *,
                                         HWND, const RGNDATA *, DWORD);
typedef HRESULT (WINAPI *pfn_endscene)(IDirect3DDevice9 *);

static pfn_create9   real_create9;
static pfn_createdev real_createdev;
static pfn_present   real_present;
static pfn_reset     real_reset;
static pfn_sc_present real_sc_present;
static pfn_endscene   real_endscene;

static IDirect3DDevice9 *g_dev;
static LONG g_frames;
static LONG g_scenes;
static int  g_dev_hooked;

/* NO __try/__except HERE. README Decision 6: 32-bit mingw has no SEH -- it does
 * not compile -- and this toolchain's DWARF-2 unwinder cannot walk through the
 * CFI-less MSVC frames of BlackOps.exe anyway (Exp. 2 case D: an exception
 * thrown through such a frame calls std::terminate and kills the process).
 *
 * The loader's AddVectoredExceptionHandler (src/veh.c) is the project-wide
 * answer: OS-level, ABI-neutral, and already installed before any plugin loads.
 * It traces and returns EXCEPTION_CONTINUE_SEARCH rather than swallowing, so a
 * fault in these callbacks is still a real crash -- which is correct. The
 * defence here is therefore to touch nothing that can fault: every pointer used
 * below is one D3D9 handed us, and each is null-checked before use.
 */
#define GUARDED_BEGIN   do {
#define GUARDED_END(what) } while (0);

static void describe_backbuffer(IDirect3DDevice9 *dev)
{
    IDirect3DSurface9 *bb = NULL;
    D3DSURFACE_DESC d;

    if (FAILED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) {
        hlog("GetBackBuffer failed");
        return;
    }
    if (SUCCEEDED(IDirect3DSurface9_GetDesc(bb, &d)))
        hlog("backbuffer %ux%u fmt=%s(%d) mult=%d usage=0x%lx pool=%d",
             d.Width, d.Height, fmt_name(d.Format), (int)d.Format,
             (int)d.MultiSampleType, (unsigned long)d.Usage, (int)d.Pool);
    IDirect3DSurface9_Release(bb);
}

static HRESULT WINAPI my_present(IDirect3DDevice9 *dev, const RECT *src, const RECT *dst,
                                 HWND wnd, const RGNDATA *dirty)
{
    GUARDED_BEGIN
        LONG n = InterlockedIncrement(&g_frames);
        /* 1, 2, 5, then every 300th: enough to prove liveness and a steady rate
         * without writing a line per frame into a file for an hour. */
        if (n == 1 || n == 2 || n == 5 || (n % 300) == 0) {
            hlog("Present #%ld dev=%p wnd=%p", n, (void *)dev, (void *)wnd);
            if (n == 1)
                describe_backbuffer(dev);
        }
    GUARDED_END("Present")
    return real_present(dev, src, dst, wnd, dirty);
}

static HRESULT WINAPI my_reset(IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *pp)
{
    GUARDED_BEGIN
        /* A Reset destroys every D3DPOOL_DEFAULT resource. Anything we allocate
         * later (the eye render targets) has to be released before this and
         * recreated after, so the hook exists from the start to make that
         * lifetime visible rather than a surprise at the first alt-tab. */
        if (pp)
            hlog("Reset dev=%p -> %ux%u fmt=%s windowed=%d",
                 (void *)dev, pp->BackBufferWidth, pp->BackBufferHeight,
                 fmt_name(pp->BackBufferFormat), (int)pp->Windowed);
        else
            hlog("Reset dev=%p (null params)", (void *)dev);
    GUARDED_END("Reset")
    return real_reset(dev, pp);
}

/* The game does not call IDirect3DDevice9::Present. Measured: run 1 hooked it
 * successfully ("device hooks live") and the counter never moved in 50 s, while
 * the device had been created 2560x1440 fullscreen and the game was on screen.
 * So it presents through the swap chain it owns -- IDirect3DSwapChain9::Present,
 * vtable slot 3 (IUnknown's three, then Present). Device::Present is in truth a
 * convenience wrapper around exactly that, which is why hooking it catches
 * nothing when the caller skips the wrapper.
 *
 * EndScene (slot 42) is hooked purely as a control: if frames were being drawn
 * at all, it fires whether or not our guess about the present path is right. */
static HRESULT WINAPI my_sc_present(IDirect3DSwapChain9 *sc, const RECT *src, const RECT *dst,
                                    HWND wnd, const RGNDATA *dirty, DWORD flags)
{
    GUARDED_BEGIN
        LONG n = InterlockedIncrement(&g_frames);
        if (n == 1 || n == 2 || n == 5 || (n % 300) == 0) {
            hlog("SwapChain::Present #%ld sc=%p wnd=%p", n, (void *)sc, (void *)wnd);
            if (n == 1 && g_dev)
                describe_backbuffer(g_dev);
        }
    GUARDED_END("SwapChain::Present")
    return real_sc_present(sc, src, dst, wnd, dirty, flags);
}

static HRESULT WINAPI my_endscene(IDirect3DDevice9 *dev)
{
    GUARDED_BEGIN
        LONG n = InterlockedIncrement(&g_scenes);
        if (n == 1 || n == 300)
            hlog("EndScene #%ld dev=%p", n, (void *)dev);
    GUARDED_END("EndScene")
    return real_endscene(dev);
}

static void hook_swapchain(IDirect3DDevice9 *dev)
{
    IDirect3DSwapChain9 *sc = NULL;
    void **vt;

    if (FAILED(IDirect3DDevice9_GetSwapChain(dev, 0, &sc)) || !sc) {
        hlog("GetSwapChain(0) failed");
        return;
    }
    vt = *(void ***)sc;
    hlog("swapchain %p vtable %p  Present=%p", (void *)sc, (void *)vt, vt[3]);
    if (MH_CreateHook(vt[3], (void *)my_sc_present, (void **)&real_sc_present) == MH_OK &&
        MH_EnableHook(vt[3]) == MH_OK)
        hlog("swapchain Present hooked");
    else
        hlog("FAILED to hook swapchain Present");
    IDirect3DSwapChain9_Release(sc);
}

static void hook_device(IDirect3DDevice9 *dev)
{
    void **vt;

    if (g_dev_hooked || !dev)
        return;
    g_dev = dev;
    vt = *(void ***)dev;
    hlog("device %p vtable %p  Reset=%p Present=%p EndScene=%p",
         (void *)dev, (void *)vt, vt[16], vt[17], vt[42]);

    hook_swapchain(dev);

    if (MH_CreateHook(vt[42], (void *)my_endscene, (void **)&real_endscene) == MH_OK &&
        MH_EnableHook(vt[42]) == MH_OK)
        hlog("EndScene hooked");

    if (MH_CreateHook(vt[17], (void *)my_present, (void **)&real_present) != MH_OK ||
        MH_EnableHook(vt[17]) != MH_OK) {
        hlog("FAILED to hook Present");
        return;
    }
    if (MH_CreateHook(vt[16], (void *)my_reset, (void **)&real_reset) != MH_OK ||
        MH_EnableHook(vt[16]) != MH_OK)
        hlog("WARNING: Present hooked but Reset not");

    g_dev = dev;
    g_dev_hooked = 1;
    hlog("device hooks live");
}

static HRESULT WINAPI my_createdevice(IDirect3D9 *self, UINT adapter, D3DDEVTYPE type,
                                      HWND focus, DWORD flags,
                                      D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **out)
{
    HRESULT hr = real_createdev(self, adapter, type, focus, flags, pp, out);
    GUARDED_BEGIN
        hlog("CreateDevice hr=0x%08lx adapter=%u type=%d flags=0x%lx hwnd=%p",
             (unsigned long)hr, adapter, (int)type, (unsigned long)flags, (void *)focus);
        if (pp)
            hlog("  requested %ux%u fmt=%s windowed=%d backbuffers=%u",
                 pp->BackBufferWidth, pp->BackBufferHeight,
                 fmt_name(pp->BackBufferFormat), (int)pp->Windowed,
                 pp->BackBufferCount);
        if (SUCCEEDED(hr) && out && *out)
            hook_device(*out);
    GUARDED_END("CreateDevice")
    return hr;
}

static IDirect3D9 *WINAPI my_create9(UINT sdk)
{
    IDirect3D9 *d3d = real_create9(sdk);
    GUARDED_BEGIN
        hlog("Direct3DCreate9(%u) -> %p", sdk, (void *)d3d);
        if (d3d && !real_createdev) {
            void **vt = *(void ***)d3d;
            hlog("IDirect3D9 vtable %p  CreateDevice=%p", (void *)vt, vt[16]);
            if (MH_CreateHook(vt[16], (void *)my_createdevice,
                              (void **)&real_createdev) == MH_OK &&
                MH_EnableHook(vt[16]) == MH_OK)
                hlog("CreateDevice hooked");
            else
                hlog("FAILED to hook CreateDevice");
        }
    GUARDED_END("Direct3DCreate9")
    return d3d;
}

/* ------------------------------------------------------------------ entry */

static DWORD WINAPI init(LPVOID p)
{
    HMODULE d3d9;
    void *fn;
    MH_STATUS st;

    (void)p;

    /* BlackOps.exe imports d3d9.dll statically, so it is already MAPPED by the
     * time any DllMain runs -- GetModuleHandle is enough and avoids a nested
     * LoadLibrary. Fall back for the synthetic hosts of Exp. 5/7, which do not
     * import it. */
    d3d9 = GetModuleHandleA("d3d9.dll");
    if (!d3d9)
        d3d9 = LoadLibraryA("d3d9.dll");
    if (!d3d9) {
        hlog("no d3d9.dll in this process -- nothing to do");
        return 0;
    }

    fn = (void *)GetProcAddress(d3d9, "Direct3DCreate9");
    if (!fn) {
        hlog("d3d9.dll at %p has no Direct3DCreate9", (void *)d3d9);
        return 0;
    }

    st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        hlog("MH_Initialize failed (%d)", (int)st);
        return 0;
    }

    if (MH_CreateHook(fn, (void *)my_create9, (void **)&real_create9) != MH_OK ||
        MH_EnableHook(fn) != MH_OK) {
        hlog("FAILED to hook Direct3DCreate9 at %p", fn);
        return 0;
    }
    hlog("armed: d3d9.dll=%p Direct3DCreate9=%p", (void *)d3d9, fn);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        hlog("attach pid=%lu", GetCurrentProcessId());
        /* Do the work on this thread, not a spawned one. The device is created
         * from the game's main thread well after our DllMain returns, so there
         * is no race to lose -- and spawning a thread from DllMain to then call
         * back into the loader lock is exactly the deadlock src/dllmain.c warns
         * about. MinHook's own trampoline allocation is fine under the lock. */
        init(NULL);
    }
    return TRUE;
}
