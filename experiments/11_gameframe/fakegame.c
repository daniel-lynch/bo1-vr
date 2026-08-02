/* fakegame.c -- stands in for BlackOps.exe so gameframe.asi can be exercised
 * without Steam.
 *
 * WHY THIS EXISTS. gameframe.asi needs one environment variable to work
 * (PROTON_USE_WOW64=1, see RESULTS.md §2), and under a real Steam launch there
 * is no environment to set. Getting that launch option in place is a one-time
 * human action; everything else about the plugin can and should be proven
 * before then, so that when the option is set the only untested thing left is
 * the game itself.
 *
 * It deliberately imitates what Exp. 10 MEASURED about the real game, not what
 * a D3D9 tutorial would do:
 *   - fullscreen-shaped back buffer at the game's own 2560x1440
 *   - D3DFMT_A8R8G8B8
 *   - 4x MULTISAMPLING, so the StretchRect resolve in gameframe.c is actually
 *     exercised rather than trivially skipped
 *   - presents through IDirect3DSwapChain9::Present, NOT Device::Present,
 *     because that is the path the game uses and therefore the only path the
 *     plugin hooks
 * A host that got any of those wrong would "pass" while testing nothing.
 *
 * The picture is a moving colour so successive frames differ and a frozen
 * compositor image is obvious.
 */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    HMODULE loader;
    IDirect3D9 *d3d;
    IDirect3DDevice9 *dev = NULL;
    IDirect3DSwapChain9 *sc = NULL;
    D3DPRESENT_PARAMETERS pp;
    WNDCLASSA wc;
    HWND hwnd;
    HRESULT hr;
    int i, frames = 300;
    const char *e = getenv("BO1VR_FRAMES");
    if (e) frames = atoi(e);

    /* Load the loader FIRST, exactly as the winmm shim does in the real game,
     * so the plugin's Direct3DCreate9 hook is armed before we call it. */
    fprintf(stderr, "fakegame: loading loader\n"); fflush(stderr);
    loader = LoadLibraryA("./bo1vr_loader.dll");
    if (!loader) { fprintf(stderr, "fakegame: loader failed %lu\n", GetLastError()); return 1; }

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "bo1vrfake";
    RegisterClassA(&wc);
    hwnd = CreateWindowExA(0, "bo1vrfake", "bo1vr fake game", WS_OVERLAPPEDWINDOW,
                           0, 0, 640, 360, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) { fprintf(stderr, "fakegame: no window\n"); return 1; }

    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) { fprintf(stderr, "fakegame: Direct3DCreate9 failed\n"); return 1; }

    memset(&pp, 0, sizeof pp);
    pp.Windowed             = TRUE;      /* windowed: no display-mode takeover on a desktop */
    pp.SwapEffect           = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat     = D3DFMT_A8R8G8B8;
    pp.BackBufferWidth      = 2560;
    pp.BackBufferHeight     = 1440;
    pp.BackBufferCount      = 1;
    pp.MultiSampleType      = D3DMULTISAMPLE_4_SAMPLES;   /* as the game has */
    pp.hDeviceWindow        = hwnd;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

    hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                 D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
                                 &pp, &dev);
    if (FAILED(hr)) {
        fprintf(stderr, "fakegame: CreateDevice 4xMSAA hr=0x%08lx, retrying without MSAA\n",
                (unsigned long)hr);
        pp.MultiSampleType = D3DMULTISAMPLE_NONE;
        hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                     D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
                                     &pp, &dev);
    }
    if (FAILED(hr) || !dev) { fprintf(stderr, "fakegame: CreateDevice hr=0x%08lx\n", (unsigned long)hr); return 1; }

    hr = IDirect3DDevice9_GetSwapChain(dev, 0, &sc);
    if (FAILED(hr) || !sc) { fprintf(stderr, "fakegame: GetSwapChain hr=0x%08lx\n", (unsigned long)hr); return 1; }

    fprintf(stderr, "fakegame: presenting %d frames through the SWAP CHAIN\n", frames);
    fflush(stderr);
    for (i = 0; i < frames; i++) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
        IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET,
                               D3DCOLOR_XRGB((i * 3) & 0xff, 40, 255 - ((i * 3) & 0xff)), 1.0f, 0);
        /* THE POINT: swap chain, not device. */
        IDirect3DSwapChain9_Present(sc, NULL, NULL, NULL, NULL, 0);
    }
    fprintf(stderr, "fakegame: done\n");
    fflush(stderr);
    IDirect3DSwapChain9_Release(sc);
    IDirect3DDevice9_Release(dev);
    IDirect3D9_Release(d3d);
    return 0;
}
