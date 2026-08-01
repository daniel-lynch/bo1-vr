/* experiments/06_visual/visual.c
 *
 * EXPERIMENT 6 -- SEE WHAT THE RUNTIME RECEIVED
 *
 * Experiment 5 proved delivery: 600 frames x 2 eyes counted arriving inside
 * monado-service, and a GPU readback proved the submitted surfaces held the
 * colours we drew. What it could not do was look at the result. Everything it
 * verified was verified BEFORE Submit, so three whole classes of bug would have
 * passed it silently:
 *
 *   - an eye swap  (left content presented to the right eye)
 *   - a vertical flip (D3D9 is top-down; Submit's pBounds was NULL and untested)
 *   - a colour-space error inside xrizer's blit (sRGB applied twice, say)
 *
 * This file submits a pattern that makes all three visible at a glance, and
 * run.sh photographs Monado's own compositor output (XRT_WINDOW_PEEK) to read
 * the answer off the far end of the chain.
 *
 * THE PATTERN, per eye, drawn into the 896x1007 D3D9 render target:
 *
 *      +--------------------------------------------------+
 *      | YELLOW |            U P             |    CYAN    |     corner tags are
 *      +--------+                            +------------+     the machine-
 *      |            [ moving marker ->]                   |     readable part
 *      |                                                  |
 *      |                +-------+                         |
 *      |                |   L   |  (or R)                 |     letter is the
 *      |                +-------+                         |     human-readable
 *      |                                                  |     part
 *      |                    D N                           |
 *      +---------+                          +-------------+
 *      | MAGENTA |  [grey ramp 0..255]      |    BLUE     |
 *      +--------------------------------------------------+
 *       background: LEFT = dark red, RIGHT = dark green
 *
 * Every question the task asks is answered by a single screenshot:
 *
 *   eye order      background colour of each half, and the letter L / R
 *   up is up       yellow must be top-left; a vertical flip puts magenta there
 *   mirroring      yellow top-left AND cyan top-right; a horizontal mirror
 *                  swaps them, which a letter alone might not settle
 *   180 rotation   yellow top-left; a rotation puts blue there
 *   colour space   the grey ramp: sRGB applied a second time lifts the
 *                  mid-tones far more than the ends, which is obvious in a
 *                  ramp and invisible in a flat fill
 *   liveness       the marker steps right one notch per frame, so two
 *                  screenshots taken apart prove the image is not a stale frame
 *
 * BO1VR_FLIP_V=1 submits VRTextureBounds_t{ uMin=0, vMin=1, uMax=1, vMax=0 }
 * instead of pBounds=NULL. That is OpenVR's own answer to a top-down source
 * texture, and running it both ways is what turns "we think the orientation is
 * right" into a measurement: exactly one of the two runs can be upright.
 *
 * Everything else -- the D3D9/DXVK interop, the Vulkan hand-off sequence, the
 * teardown order -- is Experiment 5's, unchanged. See its RESULTS.md.
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

struct eye_target {
    IDirect3DTexture9      *tex;
    IDirect3DSurface9      *surf;
    ID3D9VkInteropTexture  *vktex;
    bo1vr_VkImage           image;
    bo1vr_VkEnum            layout;
    bo1vr_VkImageCreateInfo info;
};

static HWND make_window(void)
{
    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = DefWindowProcA;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.lpszClassName = "bo1vr_exp06";
    RegisterClassA(&wc);
    return CreateWindowExA(0, "bo1vr_exp06", "bo1vr exp06", WS_OVERLAPPEDWINDOW,
                           0, 0, 320, 240, NULL, NULL, wc.hInstance, NULL);
}

/* ------------------------------------------------------- the test pattern -- */
/* Drawn entirely with IDirect3DDevice9::Clear over a rect list -- no shaders,
 * no vertex buffers, no fixed-function state to get wrong. Clear() takes an
 * array of D3DRECTs, so a whole glyph is one call. */

#define MAX_RECTS 768
struct rectlist { D3DRECT r[MAX_RECTS]; int n; int mirror_h; };

static void rl_reset(struct rectlist *rl) { rl->n = 0; }

/* mirror_h != 0 mirrors every rect about the horizontal centre line, i.e. draws
 * the whole pattern upside down IN OUR OWN TEXTURE. That is the control run
 * that proves the screenshot responds to the vertical axis (BO1VR_DRAW_FLIP). */
static void rl_add(struct rectlist *rl, int x, int y, int w, int h)
{
    if (rl->n >= MAX_RECTS || w <= 0 || h <= 0) return;
    if (rl->mirror_h) y = rl->mirror_h - y - h;
    rl->r[rl->n].x1 = x;     rl->r[rl->n].y1 = y;
    rl->r[rl->n].x2 = x + w; rl->r[rl->n].y2 = y + h;
    rl->n++;
}

static void rl_flush(IDirect3DDevice9 *dev, struct rectlist *rl, D3DCOLOR c)
{
    if (rl->n) IDirect3DDevice9_Clear(dev, (DWORD)rl->n, rl->r, D3DCLEAR_TARGET, c, 1.0f, 0);
    rl_reset(rl);
}

/* A 5x7 bitmap font, just the six characters the pattern uses. Row 0 is the
 * TOP row -- which is the whole point of the exercise, so it is worth saying
 * out loud: if the glyphs come back upside down, the chain flipped them. */
struct glyph { char c; const char *rows[7]; };
static const struct glyph g_font[] = {
    { 'L', { "X....", "X....", "X....", "X....", "X....", "X....", "XXXXX" } },
    { 'R', { "XXXX.", "X...X", "X...X", "XXXX.", "X.X..", "X..X.", "X...X" } },
    { 'U', { "X...X", "X...X", "X...X", "X...X", "X...X", "X...X", ".XXX." } },
    { 'P', { "XXXX.", "X...X", "X...X", "XXXX.", "X....", "X....", "X...." } },
    { 'D', { "XXXX.", "X...X", "X...X", "X...X", "X...X", "X...X", "XXXX." } },
    { 'N', { "X...X", "XX..X", "X.X.X", "X..XX", "X...X", "X...X", "X...X" } },
};

static void draw_glyph(struct rectlist *rl, char c, int x, int y, int px)
{
    unsigned gi, row, col;
    for (gi = 0; gi < sizeof(g_font) / sizeof(g_font[0]); gi++) {
        if (g_font[gi].c != c) continue;
        for (row = 0; row < 7; row++)
            for (col = 0; col < 5; col++)
                if (g_font[gi].rows[row][col] == 'X')
                    rl_add(rl, x + (int)col * px, y + (int)row * px, px, px);
        return;
    }
}

/* width of a string at pixel size px: 5 wide per glyph + 1 of spacing */
static int text_w(const char *s, int px) { return (int)strlen(s) * 6 * px - px; }

static void draw_text(struct rectlist *rl, const char *s, int x, int y, int px)
{
    for (; *s; s++, x += 6 * px) draw_glyph(rl, *s, x, y, px);
}

/* Where each element lives, derived from the render-target size so the pattern
 * survives a different GetRecommendedRenderTargetSize(). analyse.py samples the
 * corner tags at the same fractions, so keep the two in step. */
struct layout {
    int tag;                 /* corner tag edge length */
    int s_px, b_px;          /* small / big font pixel size */
    int y_up, y_lane, y_big, y_dn, y_ramp, h_ramp;
};

static void layout_init(struct layout *L, uint32_t rw, uint32_t rh)
{
    L->tag    = (int)rw / 8;
    L->s_px   = (int)rh / 40;
    L->b_px   = (int)rh / 18;
    L->y_up   = L->tag + (int)rh / 50;
    L->y_lane = L->y_up + 7 * L->s_px + (int)rh / 80;
    L->y_big  = L->y_lane + L->s_px + (int)rh / 25;
    L->y_dn   = L->y_big + 7 * L->b_px + (int)rh / 60;
    L->h_ramp = (int)rh / 25;
    L->y_ramp = (int)rh - L->h_ramp;
}

#define TAG_TL D3DCOLOR_XRGB(255, 255,   0)   /* yellow  */
#define TAG_TR D3DCOLOR_XRGB(  0, 255, 255)   /* cyan    */
#define TAG_BL D3DCOLOR_XRGB(255,   0, 255)   /* magenta */
#define TAG_BR D3DCOLOR_XRGB(  0,   0, 255)   /* blue    */

static void draw_eye(IDirect3DDevice9 *dev, const struct layout *L,
                     uint32_t rw, uint32_t rh, int eye, int frame, int mirror)
{
    static const D3DCOLOR bg[2] = { D3DCOLOR_XRGB(140, 16, 16),    /* LEFT  dark red   */
                                    D3DCOLOR_XRGB( 16, 110, 32) }; /* RIGHT dark green */
    struct rectlist rl;
    int i, w, x, mx;

    rl.mirror_h = mirror ? (int)rh : 0;
    rl_reset(&rl);
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, bg[eye], 1.0f, 0);

    /* corner tags -- one Clear each, so a wrong colour cannot come from a
     * wrong rect ordering */
    rl_add(&rl, 0, 0, L->tag, L->tag);                                rl_flush(dev, &rl, TAG_TL);
    rl_add(&rl, (int)rw - L->tag, 0, L->tag, L->tag);                 rl_flush(dev, &rl, TAG_TR);
    rl_add(&rl, 0, (int)rh - L->tag, L->tag, L->tag);                 rl_flush(dev, &rl, TAG_BL);
    rl_add(&rl, (int)rw - L->tag, (int)rh - L->tag, L->tag, L->tag);  rl_flush(dev, &rl, TAG_BR);

    /* text + the big per-eye letter, all white */
    w = text_w("UP", L->s_px);
    draw_text(&rl, "UP", ((int)rw - w) / 2, L->y_up, L->s_px);
    w = text_w("DN", L->s_px);
    draw_text(&rl, "DN", ((int)rw - w) / 2, L->y_dn, L->s_px);
    draw_glyph(&rl, eye == 0 ? 'L' : 'R', ((int)rw - 5 * L->b_px) / 2, L->y_big, L->b_px);

    /* liveness: one notch right per frame, wrapping */
    mx = (frame * (L->s_px / 2 + 1)) % ((int)rw - 2 * L->s_px);
    rl_add(&rl, mx, L->y_lane, L->s_px * 2, L->s_px);
    rl_flush(dev, &rl, D3DCOLOR_XRGB(255, 255, 255));

    /* grey ramp: 8 steps of 255/7, between the two bottom corner tags */
    x = L->tag + (int)rw / 40;
    w = ((int)rw - 2 * x) / 8;
    for (i = 0; i < 8; i++) {
        int v = i * 255 / 7;
        rl_add(&rl, x + i * w, L->y_ramp, w, L->h_ramp);
        rl_flush(dev, &rl, D3DCOLOR_XRGB(v, v, v));
    }
}

/* ------------------------------------------------------------- readback --- */
/* What we SENT, sampled at exactly the fractions analyse.py samples on what was
 * RECEIVED. Printing both makes the comparison a table, not an impression. */

struct probe { DWORD tl, tr, bl, br, bg, ramp[8]; };

static int readback_probe(IDirect3DDevice9 *dev, IDirect3DSurface9 *rt,
                          const struct layout *L, UINT w, UINT h,
                          const char *tag, struct probe *out)
{
    IDirect3DSurface9 *sys = NULL;
    D3DLOCKED_RECT lr;
    HRESULT hr;
    int ok = 0, i, rx, rw8;

    memset(out, 0, sizeof(*out));
    hr = IDirect3DDevice9_CreateOffscreenPlainSurface(dev, w, h, D3DFMT_X8R8G8B8,
                                                      D3DPOOL_SYSTEMMEM, &sys, NULL);
    if (FAILED(hr)) { P("  readback %s: CreateOffscreenPlainSurface 0x%08lx", tag, (unsigned long)hr); goto done; }
    hr = IDirect3DDevice9_GetRenderTargetData(dev, rt, sys);
    if (FAILED(hr)) { P("  readback %s: GetRenderTargetData 0x%08lx", tag, (unsigned long)hr); goto done; }
    hr = IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY);
    if (FAILED(hr)) { P("  readback %s: LockRect 0x%08lx", tag, (unsigned long)hr); goto done; }

#define PIX(px, py) (*(const DWORD *)((const char *)lr.pBits + (py) * lr.Pitch + (px) * 4) & 0x00FFFFFFu)
    out->tl = PIX(L->tag / 2, L->tag / 2);
    out->tr = PIX((int)w - L->tag / 2, L->tag / 2);
    out->bl = PIX(L->tag / 2, (int)h - L->tag / 2);
    out->br = PIX((int)w - L->tag / 2, (int)h - L->tag / 2);
    out->bg = PIX((int)w / 2, L->tag + (int)h / 100);
    rx  = L->tag + (int)w / 40;
    rw8 = ((int)w - 2 * rx) / 8;
    for (i = 0; i < 8; i++)
        out->ramp[i] = PIX(rx + i * rw8 + rw8 / 2, L->y_ramp + L->h_ramp / 2);
#undef PIX
    IDirect3DSurface9_UnlockRect(sys);

    P("  SENT %s: bg=0x%06lx  TL=0x%06lx TR=0x%06lx BL=0x%06lx BR=0x%06lx",
      tag, (unsigned long)out->bg, (unsigned long)out->tl, (unsigned long)out->tr,
      (unsigned long)out->bl, (unsigned long)out->br);
    P("  SENT %s: ramp = %02lx %02lx %02lx %02lx %02lx %02lx %02lx %02lx", tag,
      (unsigned long)(out->ramp[0] & 0xff), (unsigned long)(out->ramp[1] & 0xff),
      (unsigned long)(out->ramp[2] & 0xff), (unsigned long)(out->ramp[3] & 0xff),
      (unsigned long)(out->ramp[4] & 0xff), (unsigned long)(out->ramp[5] & 0xff),
      (unsigned long)(out->ramp[6] & 0xff), (unsigned long)(out->ramp[7] & 0xff));
    ok = 1;
done:
    if (sys) IDirect3DSurface9_Release(sys);
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

/* ------------------------------------------------------------------------- */

__declspec(dllexport) int visual_run(void)
{
    HMODULE hovr, hd3d9;
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
    struct layout L;
    D3DPRESENT_PARAMETERS pp;
    HWND hwnd;
    HRESULT hr;
    uint32_t rw = 0, rh = 0;
    int fail = 0, frames = 0, submits_ok = 0, i, f;
    int nframes, flip_v, swap_eyes, draw_flip;
    struct VRTextureBounds_t bounds_flip;
    DWORD want_tag[4];

    memset(eyes, 0, sizeof(eyes));

    { const char *lp = getenv("BO1VR_LOG"); g_log = fopen(lp ? lp : "visual.log", "w"); }
    { const char *n = getenv("BO1VR_FRAMES"); nframes = n ? atoi(n) : 2400; if (nframes < 1) nframes = 1; }
    flip_v    = getenv("BO1VR_FLIP_V")     != NULL;
    swap_eyes = getenv("BO1VR_SWAP_EYES")  != NULL;
    draw_flip = getenv("BO1VR_DRAW_FLIP")  != NULL;

    want_tag[0] = draw_flip ? 0xFF00FF : 0xFFFF00;   /* what should be top-left */
    want_tag[1] = draw_flip ? 0x0000FF : 0x00FFFF;
    want_tag[2] = draw_flip ? 0xFFFF00 : 0xFF00FF;
    want_tag[3] = draw_flip ? 0x00FFFF : 0x0000FF;

    P("=== EXPERIMENT 6: see what the compositor received ===");
    P("built with GCC %s, pointer size %d, pid %lu, frames=%d",
      __VERSION__, (int)sizeof(void *), GetCurrentProcessId(), nframes);
    P("pBounds  : %s", flip_v ? "VRTextureBounds_t{0,1,1,0} (vMin=1, vMax=0)"
                              : "NULL (full 0..1 range, exactly as Experiment 5)");
    P("eye order: %s", swap_eyes ? "DELIBERATELY SWAPPED -- left pattern submitted as Eye_Right"
                                 : "normal");
    P("pattern  : %s", draw_flip ? "DELIBERATELY DRAWN UPSIDE DOWN in our own texture"
                                 : "normal");

    /* ---- 1. OpenVR, exactly as Experiment 5 left it ------------------- */
    STEP("1. LoadLibraryA(openvr_api.dll) + VR_InitInternal2");
    hovr = LoadLibraryA("openvr_api.dll");
    if (!hovr) { P("FAIL: no openvr_api.dll, err=%lu", GetLastError()); return 1; }

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
    sys = (struct IVRSystem_023_FnTable *)iface;
    if (sys) sys->GetRecommendedRenderTargetSize(&rw, &rh);
    if (!rw || !rh) { rw = 896; rh = 1007; P("NOTE: falling back to %ux%u", rw, rh); }
    layout_init(&L, rw, rh);
    P("PASS-1: OpenVR up; per-eye render target %u x %u", rw, rh);
    P("  pattern layout: tag=%d s_px=%d b_px=%d  y: up=%d lane=%d big=%d dn=%d ramp=%d+%d",
      L.tag, L.s_px, L.b_px, L.y_up, L.y_lane, L.y_big, L.y_dn, L.y_ramp, L.h_ramp);

    /* ---- 2. D3D9 through DXVK ---------------------------------------- */
    STEP("2. LoadLibraryA(d3d9.dll) + Direct3DCreate9 + interop");
    hd3d9 = LoadLibraryA("d3d9.dll");
    if (!hd3d9) { P("FAIL: no d3d9.dll, err=%lu", GetLastError()); fail = 1; goto out; }
    pD3DCreate9 = (pfn_Direct3DCreate9)(void *)GetProcAddress(hd3d9, "Direct3DCreate9");
    if (!pD3DCreate9) { P("FAIL: no Direct3DCreate9"); fail = 1; goto out; }
    d3d = pD3DCreate9(D3D_SDK_VERSION);
    if (!d3d) { P("FAIL: Direct3DCreate9 returned NULL"); fail = 1; goto out; }

    hr = IDirect3D9_QueryInterface(d3d, &IID_ID3D9VkInteropInterface, (void **)&vkiface);
    if (FAILED(hr) || !vkiface) { P("FAIL: this d3d9.dll is not DXVK (hr=0x%08lx)", (unsigned long)hr); fail = 1; goto out; }
    vkiface->lpVtbl->GetInstanceHandle(vkiface, &vkinstance);

    hwnd = make_window();
    if (!hwnd) { P("FAIL: CreateWindowExA, err=%lu", GetLastError()); fail = 1; goto out; }
    memset(&pp, 0, sizeof(pp));
    pp.Windowed             = TRUE;
    pp.SwapEffect           = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat     = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth      = 320;
    pp.BackBufferHeight     = 240;
    pp.BackBufferCount      = 1;
    pp.hDeviceWindow        = hwnd;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                 D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
                                 &pp, &dev);
    if (FAILED(hr) || !dev) { P("FAIL: CreateDevice hr=0x%08lx", (unsigned long)hr); fail = 1; goto out; }

    hr = IDirect3DDevice9_QueryInterface(dev, &IID_ID3D9VkInteropDevice, (void **)&vkdev);
    if (FAILED(hr) || !vkdev) { P("FAIL: no ID3D9VkInteropDevice"); fail = 1; goto out; }
    vkdev->lpVtbl->GetVulkanHandles(vkdev, &vkinstance2, &vkphys, &vkdevice);
    vkdev->lpVtbl->GetSubmissionQueue(vkdev, &vkqueue, &queue_index, &queue_family);
    if (!vkphys || !vkdevice || !vkqueue) { P("FAIL: a NULL Vulkan handle came back"); fail = 1; goto out; }
    P("PASS-2: D3D9 on DXVK; VkInstance=%p VkDevice=%p VkQueue=%p (match: %s)",
      (void *)vkinstance2, (void *)vkdevice, (void *)vkqueue,
      vkinstance2 == vkinstance ? "yes" : "NO");

    /* ---- 3. one render target per eye, and its VkImage ---------------- */
    STEP("3. CreateTexture(RENDERTARGET) per eye + GetVulkanImageInfo");
    for (i = 0; i < 2; i++) {
        hr = IDirect3DDevice9_CreateTexture(dev, rw, rh, 1, D3DUSAGE_RENDERTARGET,
                                            D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT,
                                            &eyes[i].tex, NULL);
        if (FAILED(hr)) { P("FAIL: CreateTexture eye %d hr=0x%08lx", i, (unsigned long)hr); fail = 1; goto out; }
        hr = IDirect3DTexture9_GetSurfaceLevel(eyes[i].tex, 0, &eyes[i].surf);
        if (FAILED(hr)) { P("FAIL: GetSurfaceLevel eye %d", i); fail = 1; goto out; }
        hr = IDirect3DTexture9_QueryInterface(eyes[i].tex, &IID_ID3D9VkInteropTexture,
                                              (void **)&eyes[i].vktex);
        if (FAILED(hr) || !eyes[i].vktex) { P("FAIL: no ID3D9VkInteropTexture for eye %d", i); fail = 1; goto out; }

        memset(&eyes[i].info, 0, sizeof(eyes[i].info));
        eyes[i].info.sType = BO1VR_VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        hr = eyes[i].vktex->lpVtbl->GetVulkanImageInfo(eyes[i].vktex, &eyes[i].image,
                                                       &eyes[i].layout, &eyes[i].info);
        if (FAILED(hr) || !eyes[i].image) { P("FAIL: no VkImage for eye %d", i); fail = 1; goto out; }
        P("  eye %d: VkImage=0x%016llx layout=%d extent=%ux%u format=%d (%s)",
          i, (unsigned long long)eyes[i].image, (int)eyes[i].layout,
          eyes[i].info.extent.width, eyes[i].info.extent.height,
          (int)eyes[i].info.format, vkfmt(eyes[i].info.format));
        if (eyes[i].info.extent.width != rw || eyes[i].info.extent.height != rh) {
            P("FAIL: VkImage extent disagrees with the D3D9 texture"); fail = 1; goto out;
        }
    }
    P("PASS-3: a real VkImage behind each eye's D3D9 render target");

    /* ---- 4. the frame loop ------------------------------------------- */
    bounds_flip.uMin = 0.0f; bounds_flip.uMax = 1.0f;
    bounds_flip.vMin = 1.0f; bounds_flip.vMax = 0.0f;   /* top-down source */

    STEP("4. frame loop: WaitGetPoses -> draw pattern -> Submit(L) -> Submit(R) -> PostPresentHandoff");
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

            IDirect3DDevice9_SetRenderTarget(dev, 0, eyes[i].surf);
            draw_eye(dev, &L, rw, rh, i, f, draw_flip);

            if (f == 0) {
                struct probe pr;
                const char *tag = i == 0 ? "eye0/LEFT " : "eye1/RIGHT";
                if (!readback_probe(dev, eyes[i].surf, &L, rw, rh, tag, &pr)) { fail = 1; goto out; }
                if (pr.tl != want_tag[0] || pr.tr != want_tag[1] ||
                    pr.bl != want_tag[2] || pr.br != want_tag[3]) {
                    P("FAIL: the corner tags are not where we put them in our OWN texture "
                      "(TL/TR/BL/BR = %06lx/%06lx/%06lx/%06lx, want %06lx/%06lx/%06lx/%06lx)",
                      (unsigned long)pr.tl, (unsigned long)pr.tr,
                      (unsigned long)pr.bl, (unsigned long)pr.br,
                      (unsigned long)want_tag[0], (unsigned long)want_tag[1],
                      (unsigned long)want_tag[2], (unsigned long)want_tag[3]);
                    fail = 1; goto out;
                }
                if (i == 1)
                    P("PASS-4: both render targets read back the pattern we drew, the way we "
                      "drew it, before submit -- so anything the screenshot shows differently "
                      "was done downstream of us");
            }

            sub.aspectMask     = BO1VR_VK_IMAGE_ASPECT_COLOR_BIT;
            sub.baseMipLevel   = 0;
            sub.levelCount     = eyes[i].info.mipLevels;
            sub.baseArrayLayer = 0;
            sub.layerCount     = eyes[i].info.arrayLayers;

            vkdev->lpVtbl->TransitionTextureLayout(vkdev, eyes[i].vktex, &sub, eyes[i].layout,
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

            /* swap_eyes routes eye 0's texture to Eye_Right and vice versa.
             * The screenshot MUST swap; if it does not, the picture is not of
             * what we submitted and no conclusion drawn from it is worth
             * anything. This is the control that pBounds turned out not to be. */
            {
                int target = swap_eyes ? 1 - i : i;
                ce = comp->Submit(target == 0 ? EVREye_Eye_Left : EVREye_Eye_Right, &tex,
                                  flip_v ? &bounds_flip : NULL, EVRSubmitFlags_Submit_Default);
            }

            vkdev->lpVtbl->ReleaseSubmissionQueue(vkdev);
            vkdev->lpVtbl->TransitionTextureLayout(vkdev, eyes[i].vktex, &sub,
                                                   BO1VR_VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                   eyes[i].layout);

            if (f == 0) P("frame 0 eye %d (%s): Submit -> %d (%s)",
                          i, i == 0 ? "LEFT" : "RIGHT", (int)ce, comperr(ce));
            if (ce == EVRCompositorError_VRCompositorError_None) submits_ok++;
            else if (f == 0) { P("FAIL: first Submit rejected: %d (%s)", (int)ce, comperr(ce)); fail = 1; goto out; }
        }

        comp->PostPresentHandoff();
        frames++;
        if (f == 0) P("PASS-5: first stereo pair submitted with VRCompositorError_None");
        if ((f % 300) == 0) P("  ... frame %d done (%d successful submits so far)", f, submits_ok);
    }

    P("submitted %d frames, %d successful eye submits", frames, submits_ok);
    if (submits_ok != frames * 2) {
        P("FAIL: %d of %d eye submits were rejected", frames * 2 - submits_ok, frames * 2);
        fail = 1;
    }

    STEP("4b. comp->GetFrameTiming() -- the runtime's own frame counter");
    {
        struct Compositor_FrameTiming t;
        memset(&t, 0, sizeof(t));
        t.m_nSize = sizeof(t);
        if (comp->GetFrameTiming(&t, 0)) {
            P("runtime-side m_nFrameIndex = %u (we called PostPresentHandoff %d times)",
              t.m_nFrameIndex, frames);
            if (t.m_nFrameIndex == 0) { P("FAIL: the runtime ended 0 frames"); fail = 1; }
            else P("PASS-6: the runtime's own frame counter advanced to %u", t.m_nFrameIndex);
        } else P("NOTE: GetFrameTiming returned false");
    }

out:
    /* Experiment 5's teardown rule: the runtime's OpenXR session lives on the
     * VkInstance/VkDevice that DXVK owns, so shut the runtime down first or
     * Cleanup faults and the process hangs. */
    STEP("5. teardown: VR_ShutdownInternal first, then D3D9");
    if (fnShutdown) fnShutdown();
    for (i = 0; i < 2; i++) {
        if (eyes[i].vktex) eyes[i].vktex->lpVtbl->Release(eyes[i].vktex);
        if (eyes[i].surf)  IDirect3DSurface9_Release(eyes[i].surf);
        if (eyes[i].tex)   IDirect3DTexture9_Release(eyes[i].tex);
    }
    if (vkdev)   vkdev->lpVtbl->Release(vkdev);
    if (vkiface) vkiface->lpVtbl->Release(vkiface);
    if (dev)     IDirect3DDevice9_Release(dev);
    if (d3d)     IDirect3D9_Release(d3d);

    P("=== EXPERIMENT 6 END: %s ===", fail ? "FAIL" : "PASS");
    if (g_log) { fclose(g_log); g_log = NULL; }
    return fail ? 10 : 0;
}

BOOL WINAPI DllMain(HINSTANCE i, DWORD r, LPVOID v) { (void)i;(void)r;(void)v; return TRUE; }
