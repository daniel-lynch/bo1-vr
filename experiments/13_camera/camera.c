/* camera.c -- BAC-281 spike: get inside R_SetViewParms and read the camera.
 *
 * WHAT THIS DOES AND DELIBERATELY DOES NOT DO
 * -------------------------------------------
 * It hooks R_SetViewParms and OBSERVES. It does not move the camera.
 *
 * That is the whole point of the first pass. Two things have to be right before
 * moving anything is anything but a coin flip, and both are checkable purely by
 * reading:
 *
 *   1. THE CALLING CONVENTION. docs/camera-hook-plan.md measured this as a
 *      non-standard LTCG convention -- EDI = out, ESI = in, nothing on the
 *      stack -- across all five call sites. If that is wrong, a detour with a C
 *      signature reads garbage, and writing through the garbage corrupts the
 *      renderer in a way that will look like a DXVK bug for a day.
 *
 *   2. THE STRUCTURE OFFSETS. refdef vieworg at +0x20 and viewaxis at +0x34,
 *      GfxViewParms origin at +0x100 and axis at +0x110/+0x11C/+0x128.
 *
 * Both are testable without writing a byte, because the data checks itself:
 * a view axis is an ORTHONORMAL BASIS. If our offsets are right, the three rows
 * have unit length and zero pairwise dot products and a determinant of +1. If
 * they are wrong we are reading floats from the middle of some other field and
 * essentially none of that will hold. That is a real measurement, not a
 * plausibility argument -- and it is why this file computes it rather than
 * printing the numbers and inviting eyeballing.
 *
 * It also cross-checks the OUTPUT against the INPUT: GfxViewParms.origin should
 * equal refdef.vieworg, because R_SetViewParms copies it. If the two agree, both
 * sets of offsets are right AND the convention is right, because we could not
 * have found either without the other.
 *
 * NO __try/__except: 32-bit mingw has no SEH and this toolchain's DWARF-2
 * unwinder cannot walk BlackOps.exe's CFI-less MSVC frames (README Decision 6).
 * The detour touches only pointers the engine handed us.
 */

#include <windows.h>
#include <stdio.h>
#include <math.h>
#include <string.h>

#include "MinHook.h"

/* Absolute VAs from docs/camera-hook-plan.md, expressed relative to the PE's
 * preferred base so that a relocated image still resolves. */
#define PREFERRED_BASE   0x400000u
#define VA_R_SetViewParms 0x6C7F80u

/* refdef_t (base = cg + 0x8C100) */
#define RD_TANHALFFOVX   0x10
#define RD_TANHALFFOVY   0x14
#define RD_VIEWORG       0x20
#define RD_VIEWAXIS      0x34   /* [3][3], forward / LEFT / up */
#define RD_ZNEAR         0x5C

/* GfxViewParms, 0x140 bytes */
#define VP_VIEWMATRIX    0x00
#define VP_PROJMATRIX    0x40
#define VP_ORIGIN        0x100  /* vec4 */
#define VP_AXIS0         0x110
#define VP_AXIS1         0x11C
#define VP_AXIS2         0x128
#define VP_ZNEAR         0x138

static void camlog(const char *fmt, ...)
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
    memmove(buf + 9, buf, strlen(buf) + 1);
    memcpy(buf, "[camera] ", 9);
    strcat(buf, "\n");

    OutputDebugStringA(buf);
    /* GetTempPathA yields AppData\Local\Temp here, not steamuser\Temp. */
    n = GetTempPathA(MAX_PATH - 24, path);
    if (n && n < MAX_PATH - 24) {
        lstrcatA(path, "bo1vr_camera.log");
        h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD w;
            WriteFile(h, buf, (DWORD)strlen(buf), &w, NULL);
            CloseHandle(h);
        }
    }
}

static void  *o_R_SetViewParms;     /* MinHook trampoline */
static LONG   g_calls;

/* Forward declarations for the two asm thunks below. */
void hk_R_SetViewParms(void);
void call_original(void *out, void *in);
void __cdecl hk_body(void *out, void *in);

/* THE THUNKS.
 *
 * docs/camera-hook-plan.md writes these in MSVC `__asm { }`, which this
 * toolchain does not have. Top-level __asm__ with .globl is the same technique
 * src/winmm_shim.c already uses for its export thunks, and it keeps the
 * hand-written code to the four instructions that genuinely need to be
 * hand-written.
 *
 * mingw prefixes C symbols with '_'.
 *
 * hk_R_SetViewParms: the engine calls it with out=EDI, in=ESI and nothing on
 * the stack. Push them as ordinary cdecl arguments (right to left, so `in`
 * first) and call the C body. hk_body is cdecl, so it preserves EBX/ESI/EDI/EBP
 * -- which is why nothing needs restoring before the final `ret`.
 *
 * call_original: reload EDI/ESI from the stack and TAIL-JUMP to the trampoline.
 * A tail jump, not a call: the original ends in a plain `ret`, which then
 * returns straight to hk_body's call site. It takes no stack arguments, so the
 * two we pushed are still there for the C caller to clean up, exactly as cdecl
 * expects.
 */
__asm__(
    ".text\n"
    ".globl _hk_R_SetViewParms\n"
    "_hk_R_SetViewParms:\n"
    "    pushl %esi\n"          /* in  */
    "    pushl %edi\n"          /* out */
    "    call  _hk_body\n"
    "    addl  $8, %esp\n"
    "    ret\n"
);

/* CALL, not a tail JMP. The first version tail-jumped, on the reasoning that
 * the original's plain `ret` would land back in hk_body. It did not: the
 * pre-call logging appeared 8 times and the post-call logging zero times, while
 * the game carried on running normally -- so control left hk_body for good and
 * the engine got its frame anyway.
 *
 * A real call is also the faithful thing to do. The engine reaches
 * R_SetViewParms with nothing pushed, so at its first instruction ESP points at
 * a lone return address. Tail-jumping left OUR two arguments sitting above that
 * return address, a stack shape the function is never normally entered with.
 * Going through a frame here reproduces the engine's shape exactly and costs
 * one push and one pop. */
__asm__(
    ".text\n"
    ".globl _call_original\n"
    "_call_original:\n"
    "    pushl %ebp\n"
    "    movl  %esp, %ebp\n"
    "    pushl %esi\n"
    "    pushl %edi\n"
    "    movl  8(%ebp), %edi\n"   /* out */
    "    movl  12(%ebp), %esi\n"  /* in  */
    "    call  *_o_R_SetViewParms\n"
    "    popl  %edi\n"
    "    popl  %esi\n"
    "    popl  %ebp\n"
    "    ret\n"
);

/* --- the self-checks ---------------------------------------------------- */

static float dot3(const float *a, const float *b)
{
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}

/* Is this 3x3 an orthonormal basis? If the offsets are wrong it will not be. */
static void check_axis(const char *what, const float *m)
{
    float l0 = sqrtf(dot3(m + 0, m + 0));
    float l1 = sqrtf(dot3(m + 3, m + 3));
    float l2 = sqrtf(dot3(m + 6, m + 6));
    float d01 = dot3(m + 0, m + 3), d02 = dot3(m + 0, m + 6), d12 = dot3(m + 3, m + 6);
    /* det of the 3x3 */
    float det = m[0]*(m[4]*m[8] - m[5]*m[7])
              - m[1]*(m[3]*m[8] - m[5]*m[6])
              + m[2]*(m[3]*m[7] - m[4]*m[6]);
    int ok = fabsf(l0 - 1.f) < 1e-3f && fabsf(l1 - 1.f) < 1e-3f && fabsf(l2 - 1.f) < 1e-3f
          && fabsf(d01) < 1e-3f && fabsf(d02) < 1e-3f && fabsf(d12) < 1e-3f
          && fabsf(fabsf(det) - 1.f) < 1e-3f;
    camlog("  %s lengths %.5f %.5f %.5f  dots %.6f %.6f %.6f  det %+.5f  -> %s",
         what, l0, l1, l2, d01, d02, d12, det,
         ok ? "ORTHONORMAL (offsets confirmed)" : "NOT orthonormal (offsets or convention WRONG)");
}

void __cdecl hk_body(void *out, void *in)
{
    LONG n = InterlockedIncrement(&g_calls);

    /* Read BEFORE calling the original: `in` is the live refdef and the original
     * may legitimately change what it points at afterwards. */
    if (n <= 2 || (n % 900) == 0) {
        const unsigned char *rd = (const unsigned char *)in;
        const float *org  = (const float *)(rd + RD_VIEWORG);
        const float *axis = (const float *)(rd + RD_VIEWAXIS);
        camlog("call #%ld  out=%p in=%p", n, out, in);
        camlog("  refdef vieworg  = %.3f %.3f %.3f", org[0], org[1], org[2]);
        camlog("  refdef fwd      = %.5f %.5f %.5f", axis[0], axis[1], axis[2]);
        camlog("  refdef left     = %.5f %.5f %.5f", axis[3], axis[4], axis[5]);
        camlog("  refdef up       = %.5f %.5f %.5f", axis[6], axis[7], axis[8]);
        camlog("  tanHalfFov      = %.5f %.5f   zNear = %.5f",
             *(const float *)(rd + RD_TANHALFFOVX),
             *(const float *)(rd + RD_TANHALFFOVY),
             *(const float *)(rd + RD_ZNEAR));
        check_axis("refdef axis ", axis);
    }

    call_original(out, in);

    /* Now cross-check the OUTPUT. R_SetViewParms copies the origin across, so
     * agreement here means the refdef offsets, the GfxViewParms offsets and the
     * EDI/ESI convention are ALL correct -- none of the three could look right
     * on its own if another were wrong. */
    if (n <= 2 || (n % 900) == 0) {
        const unsigned char *vp = (const unsigned char *)out;
        const float *vorg = (const float *)(vp + VP_ORIGIN);
        const float *iorg = (const float *)((const unsigned char *)in + RD_VIEWORG);
        float ax[9];
        float d;
        memcpy(ax + 0, vp + VP_AXIS0, 12);
        memcpy(ax + 3, vp + VP_AXIS1, 12);
        memcpy(ax + 6, vp + VP_AXIS2, 12);
        camlog("  viewParms origin= %.3f %.3f %.3f (w=%.3f)", vorg[0], vorg[1], vorg[2], vorg[3]);
        check_axis("viewParms ax", ax);
        d = fabsf(vorg[0]-iorg[0]) + fabsf(vorg[1]-iorg[1]) + fabsf(vorg[2]-iorg[2]);
        camlog("  |viewParms.origin - refdef.vieworg| = %.6f  -> %s", d,
             d < 1e-3f ? "MATCH: convention and BOTH structure layouts confirmed"
                       : "MISMATCH: something in the chain is wrong");
        camlog("  zNear out       = %.5f", *(const float *)(vp + VP_ZNEAR));
    }
}

static DWORD WINAPI init(LPVOID p)
{
    unsigned char *base = (unsigned char *)GetModuleHandleA(NULL);
    void *target;
    MH_STATUS st;

    (void)p;
    if (!base) { camlog("no module base"); return 0; }
    /* Relative to the preferred base, so a relocated image still resolves. */
    target = base + (VA_R_SetViewParms - PREFERRED_BASE);
    camlog("module base %p -> R_SetViewParms %p", (void *)base, target);

    st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        camlog("MH_Initialize failed (%d)", (int)st);
        return 0;
    }
    if (MH_CreateHook(target, (void *)hk_R_SetViewParms, &o_R_SetViewParms) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        camlog("FAILED to hook R_SetViewParms at %p", target);
        return 0;
    }
    camlog("hooked R_SetViewParms; trampoline=%p. OBSERVE ONLY -- nothing is written.",
         o_R_SetViewParms);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        camlog("attach pid=%lu", GetCurrentProcessId());
        init(NULL);
    }
    return TRUE;
}
