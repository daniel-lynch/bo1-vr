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
#include "headtrack.h"   /* experiments/14_headtrack -- the rotation maths   */
#include "poses.h"       /* experiments/12_poses     -- OpenVR -> CoD poses  */

/* Absolute VAs from docs/camera-hook-plan.md, expressed relative to the PE's
 * preferred base so that a relocated image still resolves. */
#define PREFERRED_BASE   0x400000u
#define VA_R_SetViewParms 0x6C7F80u
#define VA_R_RenderScene  0x6C8C40u
#define VA_R_RenderSceneInternal 0x6C8CD0u

/* The client's accumulated view angles -- docs/motion-controls-plan.md.
 * FUN_00881930 (mouse look) adds the scaled mouse deltas straight into these,
 * so they ARE where the weapon points. Read-only so far; see the aim dry run. */
#define VA_CL_PITCH       0x2911E20u
#define VA_CL_YAW         0x2911E24u
/* frontEndData pointer, and the bump-allocated view-parms counter inside it.
 * camera-hook-plan 5.1 names slot exhaustion the HIGHEST risk of rendering the
 * scene twice, and gives exactly this as the test. */
#define VA_FRONTENDDATA   0x3B3708Cu
#define FED_VIEWPARMSCOUNT 0x16CBE0u

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

/* Is this 3x3 a RIGHT-HANDED orthonormal basis? If the offsets are wrong it
 * will not be orthonormal at all; if a rotation is wrong it can be perfectly
 * orthonormal and still be a mirror.
 *
 * THIS TEST USED TO ACCEPT A MIRROR. The first version required
 * fabsf(fabsf(det) - 1) < 1e-3, i.e. |det| = 1, which is true of a left-handed
 * basis too -- and a left-handed basis is exactly what a single sign error in a
 * rotation produces. It renders a world that moves correctly with your head and
 * is silently inside-out. ht_check_basis() requires det = +1 AND
 * forward x left = up; the old form was re-run against headtrack_mathcheck and
 * fails case 5.
 *
 * WHAT PINS WHAT, precisely -- an earlier version of this comment overclaimed.
 * The two criteria are REDUNDANT for a mirror: with orthonormal rows,
 * cross_err = 0 already implies det > 0, so reverting the det criterion ALONE
 * leaves the suite green and no test can pin it on its own. What case 5 pins is
 * the PAIR: remove both -- which is precisely the old |det| test -- and case 5
 * goes red. Defence in depth, not two independent tests. exp 14 RESULTS.md
 * §2.1 has the measurements. */
static void check_axis(const char *what, const float *m)
{
    ht_basis_t r;
    int ok = ht_check_basis(m, &r);
    camlog("  %s lengths %.5f %.5f %.5f  dots %.6f %.6f %.6f  det %+.5f  fxl-up %.6f -> %s",
         what, r.len[0], r.len[1], r.len[2], r.dot[0], r.dot[1], r.dot[2],
         r.det, r.cross_err,
         ok ? "RIGHT-HANDED ORTHONORMAL"
            : (r.mirrored ? "*** MIRRORED (det < 0) -- the world is inside out ***"
                          : "NOT orthonormal (offsets or convention WRONG)"));
}

/* --- per-eye camera state, driven from outside ------------------------- */

/* Set by gameframe.asi at the frame boundary, because that is where the eye
 * alternation belongs: the frame is submitted to ONE eye, so the eye is a
 * property of the frame, not of this hook. -1 means "leave the camera alone",
 * which is the state during menus and any frame we are not driving. */
/* CAM_EYE_CENTRE is ours, not part of the interface gameframe.asi drives: it
 * means "apply the head orientation, no eye offset", i.e. a mono view from the
 * head. It exists so head tracking can be observed on a flat monitor with
 * gameframe.asi absent -- RESULTS.md §5's isolation configuration. gameframe.asi
 * only ever passes 0, 1 or -1 and needs no knowledge of it. */
#define CAM_EYE_NONE    (-1)
#define CAM_EYE_LEFT      0
#define CAM_EYE_RIGHT     1
#define CAM_EYE_CENTRE    2

static volatile LONG g_eye = CAM_EYE_NONE;
static float g_aim_fwd[3];             /* the game's aim forward, world space   */
static float g_aim_ndc[2];             /* where that lands in the rendered view */
static int   g_aim_ndc_ok;
static volatile LONG g_aim_fresh;   /* set per scene view, CONSUMED by the getter */
static int  (*g_get_fov)(int, float *, float *);   /* gameframe.asi's bo1vr_get_eye_fov */
static unsigned char *g_base;                      /* image base, for the slot watch */
static unsigned g_max_slots;
static float         g_ipd_units = 2.6f;   /* see below */

/* Exported so gameframe.asi can drive us without either of us owning the other.
 * A tiny explicit interface beats a shared global in one of the two DLLs. */
__declspec(dllexport) void bo1vr_camera_set_eye(int eye)
{
    InterlockedExchange(&g_eye, eye);
}

/* WHERE THE WEAPON POINTS, AS NORMALISED DEVICE COORDINATES.
 *
 * (0,0) is the centre of the rendered image, +x right, +y up, edges at +/-1.
 * Returns 0 when there is nothing sensible to report -- no scene rendered yet,
 * or the aim is behind the viewer, which really happens once the player turns
 * far enough round. A caller that draws on a 0 return would put a reticle in a
 * place the gun is not pointing, which is the exact bug this exists to fix.
 *
 * gameframe.asi owns the device and the back buffer size, so it does the
 * pixel mapping; this side knows the view and the projection, so it does the
 * geometry. Neither has to learn the other's job. */
__declspec(dllexport) int bo1vr_camera_get_aim_ndc(float *x, float *y)
{
    /* CONSUME the freshness token. A menu frame renders no scene, so nothing
     * sets it, so this returns 0 and gameframe.asi draws nothing -- rather than
     * reusing the last gameplay aim over a screen that has no gun in it. */
    if (!InterlockedExchange(&g_aim_fresh, 0)) return 0;
    if (!g_aim_ndc_ok || !x || !y) return 0;
    *x = g_aim_ndc[0];
    *y = g_aim_ndc[1];
    return 1;
}

__declspec(dllexport) void bo1vr_camera_set_ipd_units(float u)
{
    g_ipd_units = u;
}

/* HOW FAR APART ARE THE EYES, IN THIS ENGINE'S UNITS?
 *
 * A human IPD is ~65 mm. docs/camera-hook-plan.md §5.4 leaves the world unit
 * ASSUMED as inches, and exp 12 assumes the same (39.37 units per metre). On
 * that assumption 65 mm is 2.56 units, hence the 2.6 default.
 *
 * This is the single number that a wrong unit assumption shows up in first, and
 * it shows up as an obvious, harmless symptom: the stereo separation looks like
 * a giant's or a doll's. That makes it a cheap discriminating test for §5.4
 * rather than a guess buried in the code -- and it is why it is a settable
 * value rather than a constant.
 *
 * Once the HMD is feeding us poses this number is only the FALLBACK: the real
 * eye separation comes from GetEyeToHeadTransform via exp 12, so it is measured
 * from the headset rather than assumed. The fallback still runs whenever there
 * is no pose (menus, no runtime, tracking lost). */

/* =========================== HEAD ORIENTATION ===========================
 *
 * exp 13's first pass moved the camera sideways and left the orientation
 * alone, on the explicit reasoning (RESULTS.md §"Deliberately position-only")
 * that a translation error is instantly visible and harmless while a wrong
 * rotation basis yields a subtly MIRRORED world that survives scrutiny. This is
 * the other half, and the whole design is arranged around that hazard:
 *
 *   * The maths lives in experiments/14_headtrack/headtrack.c, which has no
 *     windows.h and no OpenVR, so the identical object file also builds a
 *     console program that CHECKS it -- against hand-derived closed forms, and
 *     against the four wrong orderings (G*H, H^T*G, H*G^T, (H*G)^T), each of
 *     which the check requires to score as grossly wrong. `make check`.
 *   * That check runs again HERE, at DLL load. If it fails, orientation is
 *     never written -- the hook degrades to exactly the position-only
 *     behaviour it had before -- and the log says so in capitals. A bad
 *     rotation must not be able to reach the renderer just because a build got
 *     mangled.
 *   * Every composed basis is checked before it is written: right-handed
 *     orthonormal (det = +1, not |det| = 1) plus, in yaw-only mode, the exact
 *     third-column invariant described in headtrack.h. A frame that fails is
 *     NOT written and IS logged loudly; 30 consecutive failures disable
 *     orientation for the session.
 *
 * WHERE THE POSE COMES FROM. gameframe.asi owns the OpenVR session and the
 * frame clock; it exports no pose, so we take exp 12's second documented route:
 * poses_attach(), which asks the already-loaded openvr_api.dll for
 * IVRSystem_023 and never calls VR_InitInternal2, so it cannot steal or
 * duplicate the compositor session. Poses come from poses_poll()
 * (GetDeviceToAbsoluteTrackingPose), which does not touch the compositor's
 * frame pacing.
 *
 * That is a real compromise and it is worth naming: the pose we use is polled,
 * not the render pose gameframe.asi's WaitGetPoses hands the compositor, so the
 * two can differ by a fraction of a frame and the compositor's reprojection
 * will be very slightly inconsistent with what we drew. The fix is one line in
 * gameframe.c (`poses_update(rposes, 64)`, exp 12 §5 Test 3) plus a pose export
 * -- and that file belongs to another workstream, so it is not done here.
 */

static int   g_ht_math_ok;              /* ht_selfcheck passed at load        */
static int   g_ht_enable = 1;
/* DEFAULT: HT_REF_FULL, changed after the first headset session.
 *
 * Yaw-only shipped as the default because it keeps the horizon level, which is
 * the right behaviour for a VR game -- but Black Ops is not a VR game, and in
 * yaw-only mode the game's PITCH never reaches the picture. The mouse still
 * pitches the player's actual view angles, so the gun, the crosshair and the
 * engine's idea of where you are looking all move vertically while the picture
 * stays flat. The first field report was "head tracking worked until I moved
 * the mouse", which is what that feels like from inside a headset.
 *
 * Full reference makes the mouse do exactly what it does in the flat game and
 * adds the head on top: with the head level the view is bit-identical to what
 * the engine would have rendered on its own. It also keeps the scripted
 * cameras -- death cams, vehicle sections, the intro -- which yaw-only throws
 * away. The cost is that a head YAW held under a mouse PITCH tilts the horizon,
 * which case 12 now measures rather than merely describing.
 *
 * This is a REASONED default, not a validated one. Both modes are one keypress
 * apart (RCtrl+F11) precisely so the question can be settled by a person in a
 * headset rather than by argument. */
static int   g_ht_mode   = HT_REF_FULL;
static int   g_pitch_warned;
static int   g_ht_pos;                  /* room-scale translation: OFF, see below */
static int   g_ht_bound;                /* poses_attach() succeeded           */
static int   g_ht_dead;                 /* disabled for the session           */
static int   g_ht_first;                /* logged the first applied frame     */
static long  g_ht_fail;                 /* consecutive rejected bases         */
static long  g_ht_fail_total;
static float g_yaw0;                    /* recentre offset, radians           */
static volatile LONG g_recentre = 1;    /* a recentre is pending              */
static int   g_recentre_now;            /* ... and it was asked for by a human,
                                         * so do not wait for the settle       */
static long  g_good_run;                /* consecutive Running_OK head poses  */
static int   g_eye_off_ok = 1;          /* the per-eye offsets are believable */
static int   g_eyes_checked;
static float g_ref_pos[3];              /* head position at recentre          */
static int   g_have_ref_pos;

/* One frame's worth of pose, sampled once per frame in hk_R_RenderScene and
 * read by hk_body. Same thread (the render thread) for both, so no locking:
 * poses.c's seqlock already covers the OpenVR writer.
 *
 * Three slots: CAM_EYE_LEFT, CAM_EYE_RIGHT, and CAM_EYE_CENTRE -- the head
 * itself, with a zero eye offset. */
static struct {
    int   valid;
    float axis[9];       /* eye basis, TRACKING coords, CoD convention (rows
                          * forward/left/up) -- exp 12's cod_axis verbatim   */
    float eye_off[3];    /* eye origin - head origin, tracking coords, units */
} g_eyepose[3];
static float g_head_pos[3];
static int   g_head_valid;

/* Defined below, in "REACHING THIS AT RUN TIME". */
static void ht_request_recentre(int immediate, const char *who);
static void ht_log_state(const char *why);

__declspec(dllexport) void bo1vr_camera_set_head_tracking(int on)
{
    g_ht_enable = on ? 1 : 0;
    ht_log_state("bo1vr_camera_set_head_tracking()");
}

/* HT_REF_YAW_ONLY (default) or HT_REF_FULL -- see headtrack.h for the
 * trade-off. Only a headset can settle which is right, so both exist, and both
 * are reachable at run time (RCtrl+F11, or reffull.on). */
__declspec(dllexport) void bo1vr_camera_set_ref_mode(int mode)
{
    g_ht_mode = (mode == HT_REF_FULL) ? HT_REF_FULL : HT_REF_YAW_ONLY;
    ht_log_state("bo1vr_camera_set_ref_mode()");
}

/* Make the direction the head is facing right now read as "straight ahead".
 * Takes effect on the next sampled frame. Also reachable as RCtrl+F9 or by
 * dropping C:\bo1vr\recentre.on -- see "REACHING THIS AT RUN TIME" below. */
__declspec(dllexport) void bo1vr_camera_recentre(void)
{
    ht_request_recentre(1, "bo1vr_camera_recentre()");
}

/* ROOM-SCALE TRANSLATION, off by default -- and this is not timidity.
 * cod_origin is measured from the TRACKING ORIGIN, so a standing player's head
 * is ~65 units up. The game's vieworg is already at the player's eye. Adding
 * the raw head position would put the camera 65 units above the player's head.
 * With this on, only the DELTA from the recentre position is added, which is
 * lean and crouch and nothing else. It is still off by default because the
 * delta is only as good as the recentre, and nobody has yet stood up in front
 * of this code. */
__declspec(dllexport) void bo1vr_camera_set_position_tracking(int on)
{
    g_ht_pos = on ? 1 : 0;
    if (on && !g_have_ref_pos)
        ht_request_recentre(1, "bo1vr_camera_set_position_tracking()");
    ht_log_state("bo1vr_camera_set_position_tracking()");
}

/* The world-scale knob (exp 12 risk #2: units are ASSUMED inches). Changes the
 * scale of the HMD's own eye separation and of any positional tracking. */
__declspec(dllexport) void bo1vr_camera_set_units_per_metre(float u)
{
    poses_set_units_per_metre(u);
}

/* ===================== REACHING THIS AT RUN TIME ========================
 *
 * The five exports below are the programmatic interface, and for a while they
 * were the ONLY one -- which made them dead code: nothing in the process
 * resolves them (gameframe.asi looks up bo1vr_camera_set_eye and nothing else),
 * so HT_REF_FULL could not be selected, head tracking could not be turned off
 * without deleting camera.asi (which also removes stereo), and recentring could
 * not be triggered at all. An experiment whose comparison cannot be performed
 * is not an experiment, and the yaw-only default is exactly the choice that can
 * only be judged by flipping it while wearing the headset.
 *
 * So there are two paths that need no rebuild and no cooperation from anyone:
 *
 *   SWITCH FILES in C:\bo1vr, the pattern gameframe.c already uses for its
 *   bisect (novr.on, nolock.on, ...). Re-read every 90 frames, so they can be
 *   dropped or removed while the game runs:
 *
 *       nohead.on      head orientation off (position-only, the exp 13 camera)
 *       reffull.on     HT_REF_FULL instead of yaw-only          <- the §7 comparison
 *       roomscale.on   positional head tracking on
 *       recentre.on    recentre now. CONSUMED: the file is deleted, so it acts
 *                      as a button rather than a state, and can be pressed
 *                      again by re-creating it.
 *
 *   KEYS, polled once per frame, for anything that has to be done with the
 *   headset actually on. RIGHT Ctrl is the modifier because Black Ops binds
 *   left Ctrl (crouch) and leaves the right one alone:
 *
 *       RCtrl+F9    recentre
 *       RCtrl+F10   head orientation on/off
 *       RCtrl+F11   yaw-only <-> full reference     <- the §7 comparison
 *       RCtrl+F12   positional tracking on/off
 *
 * Only GetAsyncKeyState's 0x8000 "is down now" bit is read, never the 0x0001
 * "pressed since last call" bit: that low bit is process-wide state and reading
 * it would consume the event out from under the game's own input. Edges are
 * detected here instead.
 *
 * Every change logs the resulting state, so the log says what the build was
 * doing rather than what it was built to do.
 */

static int switch_on(const char *name)
{
    char path[MAX_PATH];
    lstrcpynA(path, "C:\\bo1vr\\", MAX_PATH);
    lstrcatA(path, name);
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

static void switch_consume(const char *name)
{
    char path[MAX_PATH];
    lstrcpynA(path, "C:\\bo1vr\\", MAX_PATH);
    lstrcatA(path, name);
    DeleteFileA(path);
}

/* A SWITCH THAT CARRIES A NUMBER.
 *
 * Same directory and the same "edit a file, no rebuild" idiom as the flags, but
 * for the settings that are a value rather than a yes/no. World scale is the
 * first: it has no correct answer that can be derived, only one that is found
 * by feel in the headset, so it has to be adjustable by the person wearing it
 * between one look and the next.
 *
 * Parsed by hand rather than with the CRT. This DLL deliberately does not pull
 * in the CRT's locale machinery, and atof() is locale-sensitive -- on a comma
 * -decimal locale "1.5" parses as 1. A knob that silently means something
 * different on someone else's machine is worse than no knob. */
/* Is this address actually committed and readable? The aim accumulators are
 * BSS addresses read out of a disassembly, and a wrong one would either fault
 * or -- worse -- silently read garbage that looks like plausible angles. */
static int mem_readable_cam(const void *p, SIZE_T n)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || !VirtualQuery(p, &mbi, sizeof mbi)) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    return (SIZE_T)((const char *)mbi.BaseAddress + mbi.RegionSize - (const char *)p) >= n;
}

static int switch_float(const char *name, float *out)
{
    char path[MAX_PATH], buf[64];
    HANDLE h;
    DWORD got = 0;
    int i = 0, neg = 0;
    double v = 0.0, frac = 0.0, scale = 1.0;
    int digits = 0;

    lstrcpynA(path, "C:\\bo1vr\\", MAX_PATH);
    lstrcatA(path, name);
    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    if (!ReadFile(h, buf, sizeof buf - 1, &got, NULL)) got = 0;
    CloseHandle(h);
    if (!got) return 0;
    buf[got] = 0;

    while (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\r' || buf[i] == '\n') i++;
    if (buf[i] == '-') { neg = 1; i++; } else if (buf[i] == '+') i++;
    for (; buf[i] >= '0' && buf[i] <= '9'; i++) { v = v * 10.0 + (buf[i] - '0'); digits++; }
    if (buf[i] == '.') {
        i++;
        for (; buf[i] >= '0' && buf[i] <= '9'; i++) {
            scale *= 0.1; frac += (buf[i] - '0') * scale; digits++;
        }
    }
    if (!digits) return 0;                 /* an empty or junk file is not a value */
    v = (v + frac) * (neg ? -1.0 : 1.0);
    *out = (float)v;
    return 1;
}

static void ht_request_recentre(int immediate, const char *who)
{
    InterlockedExchange(&g_recentre, 1);
    if (immediate) g_recentre_now = 1;
    camlog("recentre requested by %s", who);
}

static void ht_log_state(const char *why)
{
    camlog("head tracking state (%s): orientation %s, reference %s, "
           "positional %s, attached %s", why,
           g_ht_dead ? "DEAD" : (g_ht_enable ? "on" : "off"),
           g_ht_mode == HT_REF_FULL ? "FULL (game pitch+roll kept)"
                                    : "yaw-only (level horizon)",
           g_ht_pos ? "on" : "off",
           g_ht_bound ? "yes" : "no");
}

/* Edge-triggered modifier+key. Returns 1 exactly once per physical press. */
static int chord_pressed(int mod_down, int vk, int *prev)
{
    int down = mod_down && (GetAsyncKeyState(vk) & 0x8000) != 0;
    int edge = down && !*prev;
    *prev = down;
    return edge;
}

/* Called once per frame from headtrack_sample, i.e. only while a 3D scene is
 * rendering. That is where every one of these settings has any effect, so a
 * key pressed in a menu simply does nothing rather than being queued. */
static void headtrack_controls(void)
{
    static long   ticks;
    static int    p9, p10, p11, p12;
    static int    file_state_known;
    static int    was_nohead, was_reffull, was_room;
    int changed = 0;
    /* One read of the modifier, not one per key. */
    int mod = (GetAsyncKeyState(VK_RCONTROL) & 0x8000) != 0;

    if (chord_pressed(mod, VK_F9, &p9))
        ht_request_recentre(1, "RCtrl+F9");
    if (chord_pressed(mod, VK_F10, &p10)) {
        g_ht_enable = !g_ht_enable;
        changed = 1;
    }
    if (chord_pressed(mod, VK_F11, &p11)) {
        g_ht_mode = (g_ht_mode == HT_REF_FULL) ? HT_REF_YAW_ONLY : HT_REF_FULL;
        changed = 1;
    }
    if (chord_pressed(mod, VK_F12, &p12)) {
        g_ht_pos = !g_ht_pos;
        if (g_ht_pos && !g_have_ref_pos) ht_request_recentre(1, "roomscale enable");
        changed = 1;
    }

    /* The files, about once a second. GetFileAttributesA on four names at frame
     * rate would be four syscalls per frame for a setting nobody changes. */
    if ((ticks++ % 90) == 0) {
        int nohead  = switch_on("nohead.on");
        int room    = switch_on("roomscale.on");
        /* The mode files name a mode outright rather than toggling a flag,
         * because the DEFAULT moved from yaw-only to full and a file whose
         * absence meant "yaw-only" would silently have changed meaning under
         * anyone who already had one. yawonly.on wins if both are present. */
        int yawonly = switch_on("yawonly.on");
        int reffull = switch_on("reffull.on");
        int want    = yawonly ? HT_REF_YAW_ONLY : (reffull ? HT_REF_FULL : g_ht_mode);
        int modesel = yawonly ? 1 : (reffull ? 2 : 0);

        if (!file_state_known) {
            file_state_known = 1;
            was_nohead = nohead; was_reffull = modesel; was_room = room;
            if (nohead)  { g_ht_enable = 0; changed = 1; }
            if (modesel && want != g_ht_mode) { g_ht_mode = want; changed = 1; }
            if (room)    { g_ht_pos = 1; changed = 1; }
        } else {
            if (nohead != was_nohead)   { was_nohead = nohead;   g_ht_enable = !nohead; changed = 1; }
            if (modesel != was_reffull) {
                was_reffull = modesel;
                /* Removing both files leaves the mode where it is rather than
                 * snapping back to a default mid-session. */
                if (modesel && want != g_ht_mode) { g_ht_mode = want; changed = 1; }
            }
            if (room != was_room)       { was_room = room;       g_ht_pos = room; changed = 1; }
        }
        if (switch_on("recentre.on")) {
            switch_consume("recentre.on");
            ht_request_recentre(1, "recentre.on (consumed)");
        }

        /* WORLD SCALE, re-read live.
         *
         * Game units per metre: how much real head movement and eye separation
         * are worth in the world. The engine's unit is ASSUMED to be the inch
         * (camera-hook-plan 5.4), hence the 39.37 default, but the assumption
         * has never been confirmed against anything -- and even if it is exactly
         * right, "correct" and "comfortable" are not the same number. A smaller
         * value makes the world feel bigger and the player smaller.
         *
         * This is deliberately live rather than read once at load: it is tuned
         * by feel with the headset on, and a setting you must restart the game
         * to try is a setting nobody converges on. Values outside 1..1000 are
         * rejected -- a typo that lands at 0 would collapse the eye separation
         * and the head translation to nothing, which reads as "head tracking
         * broke" rather than as a bad number. */
        {
            static float applied;
            float upm;
            if (switch_float("worldscale.txt", &upm) && upm >= 1.0f && upm <= 1000.0f) {
                if (upm != applied) {
                    applied = upm;
                    poses_set_units_per_metre(upm);
                    camlog("world scale: %.3f units/metre (was %.3f) -- from worldscale.txt",
                           upm, POSES_UNITS_PER_METRE_DEFAULT);
                    changed = 1;
                }
            }
        }
    }

    if (changed)
        ht_log_state("changed at run time");
}

/* ============ IS ORIENTATION BEING APPLIED, RIGHT NOW, OR NOT? ==========
 *
 * The field report was "head tracking worked until I moved the mouse", and
 * answering it took an audit of every early return in this file. The log should
 * have answered it. Every path that skips the orientation write now names
 * itself at the moment the answer CHANGES -- not once at startup, not every
 * frame, but on the transition. If orientation ever stops there is a line at the
 * instant it stopped, saying which path; if it never stops, the absence of that
 * line is itself the evidence, and the report is about what the picture DOES
 * rather than about the hook failing.
 *
 * That distinction -- a bug, versus the yaw-only reference behaving exactly as
 * designed -- should never again need an argument to settle.
 */
static int  g_ht_applying = -1;         /* -1 unknown, 0 no, 1 yes            */
static long g_ht_flips;
static long g_views, g_views_oriented;
static float g_last_game_pitch, g_last_head_yaw, g_last_head_pitch;
static float g_aim_yaw, g_aim_pitch;   /* the game's own aim, pre-head-rotation */

static void ht_note(int applying, const char *why)
{
    if (g_ht_applying == applying)
        return;
    g_ht_applying = applying;
    if (++g_ht_flips <= 40)
        camlog("*** ORIENTATION %s: %s",
               applying ? "APPLIED (resumed)" : "NOT APPLIED", why);
    else if (g_ht_flips == 41)
        camlog("*** orientation is flapping; further transition lines suppressed");
}

static void ht_die(const char *why)
{
    if (g_ht_dead) return;
    g_ht_dead = 1;
    camlog("*** HEAD ORIENTATION DISABLED for this session: %s", why);
    camlog("*** falling back to position-only stereo (the exp 13 behaviour)");
}

/* A composed basis we refuse to write. Loud, with the numbers that say WHICH
 * way it is wrong -- a mirror, a stretch, or a broken invariant -- because
 * "head tracking looks odd" is not a bug report anyone can act on. */
static void ht_reject(const ht_basis_t *r, float inv_err, int have_inv)
{
    g_ht_fail++;
    g_ht_fail_total++;
    if (g_ht_fail <= 5 || (g_ht_fail % 300) == 0) {
        camlog("*** REJECTED composed view basis (#%ld, %ld total): "
               "len %.4f %.4f %.4f dots %.5f %.5f %.5f det %+.4f fxl-up %.5f%s",
               g_ht_fail, g_ht_fail_total, r->len[0], r->len[1], r->len[2],
               r->dot[0], r->dot[1], r->dot[2], r->det, r->cross_err,
               r->mirrored ? "  MIRRORED (det<0)" : "");
        if (have_inv)
            camlog("*** yaw invariant |F[i][2]-H[i][2]| = %.6f (want 0): the "
                   "reference is not a pure yaw, or the composition order is wrong",
                   inv_err);
        camlog("*** the camera keeps the GAME's orientation this frame");
    }
    if (g_ht_fail >= 30)
        ht_die("30 consecutive composed bases failed their checks");
}

/* Are the two eye offsets believable? Two questions the runtime can fail:
 *   - is the separation in a human range (0.5 .. 6.0 units, ~13..152 mm)?
 *   - is the LEFT eye actually to the left, along the head's own left axis?
 * The first is also the units-per-metre test in disguise: if the world is not
 * inches, a correct 63 mm IPD comes out as the wrong number of units and this
 * line says so with a figure instead of leaving it to be discovered in a
 * headset. A NO to the second means the stereo pair is swapped, the classic
 * "VR gives me a headache and I cannot say why" bug, and it is invisible on a
 * monitor. We do not try to fix it; we refuse to use the offsets and say so. */
static void ht_check_eye_offsets(void)
{
    float d[3], sep, along;
    const float *hl;

    if (g_eyes_checked || !g_eyepose[0].valid || !g_eyepose[1].valid)
        return;
    g_eyes_checked = 1;

    d[0] = g_eyepose[0].eye_off[0] - g_eyepose[1].eye_off[0];
    d[1] = g_eyepose[0].eye_off[1] - g_eyepose[1].eye_off[1];
    d[2] = g_eyepose[0].eye_off[2] - g_eyepose[1].eye_off[2];
    sep = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);

    hl = g_eyepose[0].axis + 3;                 /* head LEFT, tracking coords */
    along = d[0]*hl[0] + d[1]*hl[1] + d[2]*hl[2];

    camlog("eye offsets: separation %.3f units (%.1f mm at %.2f units/m), "
           "left-eye-is-left projection %+.3f",
           sep, sep / poses_units_per_metre() * 1000.0f,
           poses_units_per_metre(), along);

    if (sep < 0.5f || sep > 6.0f) {
        g_eye_off_ok = 0;
        camlog("*** eye separation %.3f units is not a human IPD -- either "
               "GetEyeToHeadTransform is wrong or units-per-metre (%.2f) is. "
               "Using the assumed %.2f-unit shift instead.",
               sep, poses_units_per_metre(), g_ipd_units);
    } else if (along <= 0.0f) {
        g_eye_off_ok = 0;
        camlog("*** THE EYES ARE SWAPPED: the left eye's offset points RIGHT "
               "along the head's own left axis (%.3f). Refusing the runtime's "
               "offsets and using the assumed shift.", along);
    }
}

/* Once per rendered frame, from hk_R_RenderScene. */
#define STALE_LIMIT   90        /* frames of dropout we hold the last pose for */
#define SETTLE_FRAMES 30        /* steady poses before the automatic recentre  */
#define TRACKING_RUNNING_OK 200 /* ETrackingResult_TrackingResult_Running_OK   */
static long g_pose_stale;

static void headtrack_sample(void)
{
    poses_pose_t hmd, ey;
    int e, i, got;

    /* BEFORE the enable check, or RCtrl+F10 would be a one-way switch: turning
     * head tracking off would also turn off the code that reads the key. */
    headtrack_controls();

    if (!g_ht_enable || g_ht_dead || !g_ht_math_ok)
        return;

    if (!g_ht_bound) {
        static long tries;
        /* gameframe.asi may not have created the session yet, so retry -- but
         * about once a second, not 300 times a second. */
        if ((tries++ % 90) != 0)
            return;
        if (!poses_attach()) {
            if (tries == 1)
                camlog("head tracking: no OpenVR session yet (poses_attach) -- "
                       "retrying about once a second");
            return;
        }
        g_ht_bound = 1;
        /* EXPLICITLY standing. exp 12 risk #4: poll and the render path can
         * disagree about the universe, and if they do the same physical head
         * reads at two different heights. */
        poses_set_universe(1);
        camlog("head tracking: attached to the existing OpenVR session "
               "(no VR_InitInternal2), standing universe");
    }

    poses_poll();
    g_head_valid = poses_get(POSES_HMD, &hmd);
    if (g_head_valid)
        memcpy(g_head_pos, hmd.cod_origin, sizeof g_head_pos);

    /* AIM DRY RUN -- MEASUREMENT ONLY, NEVER A WRITE.
     *
     * docs/motion-controls-plan.md locates the aim accumulators: the mouse-look
     * function FUN_00881930 adds mouse deltas straight into 0x2911E20 (pitch)
     * and 0x2911E24 (yaw), so writing those two floats is how the weapon gets
     * aimed. The transform from a controller's tracking-space forward vector to
     * those two numbers is NOT known, though -- the recentre yaw applies to the
     * hand exactly as it does to the head, and the reference basis was captured
     * from the game's own view direction, so there is a frame relationship to
     * establish and getting it wrong spins the player uncontrollably.
     *
     * Guessing it and playtesting the guess is the expensive way round: a wrong
     * sign is a ruined session and tells you almost nothing. Logging both sides
     * of the equation for one session costs nothing and fits the transform from
     * data. So this reads the game's live angles and the hand's angles side by
     * side and writes NOTHING. aimlog.on, off by default.
     *
     * When the relationship is clear, #45 writes it -- behind its own switch. */
    if (switch_on("aimlog.on") && g_base) {
        static long an;
        if ((an++ % 90) == 0) {
            const float *gp = (const float *)(g_base + (VA_CL_PITCH - PREFERRED_BASE));
            const float *gy = (const float *)(g_base + (VA_CL_YAW   - PREFERRED_BASE));
            poses_pose_t rh;
            if (poses_get(POSES_HAND_RIGHT, &rh) && mem_readable_cam(gp, 4) &&
                mem_readable_cam(gy, 4)) {
                const float *f = rh.cod_axis[0];          /* forward row */
                float hyaw = ht_yaw_of((const float *)rh.cod_axis) * 57.29578f;
                /* CoD pitch is NEGATIVE up. Same clamp idiom as g_last_game_pitch
                 * below -- asinf of a component that rounds to 1.0000001 is NaN,
                 * and a NaN here would poison the log rather than the game. */
                float hpit = -asinf(f[2] > 1.0f ? 1.0f : (f[2] < -1.0f ? -1.0f : f[2]))
                             * 57.29578f;
                camlog("aimlog: AIM pitch %.2f yaw %.2f | globals %.2f %.2f | "
                       "hand raw pitch %.2f yaw %.2f | hand recentred yaw %.2f "
                       "(yaw0 %.2f) | head yaw %.2f",
                       g_aim_pitch, g_aim_yaw, *gp, *gy, hpit, hyaw,
                       hyaw - g_yaw0 * 57.29578f, g_yaw0 * 57.29578f, g_last_head_yaw);
            }
        }
    }

    /* CONTROLLERS: OBSERVED, NOT YET USED.
     *
     * exp 12 built the hand slots and verified their maths offline, but its
     * RESULTS.md is explicit that NOTHING in it has ever been run against a
     * headset -- the whole coordinate section is marked ASSUMED. Motion control
     * is the next real feature and every part of it stands on these two poses
     * being live and sane, so the cheapest useful thing this build can do is
     * say whether they are, before anything is built on top.
     *
     * Logged rarely and only on CHANGE of validity, plus a slow heartbeat: a
     * controller that goes to sleep on the desk and wakes when picked up is the
     * normal case, and it is exactly what a once-only line would misreport. */
    {
        static int  known[2] = { -1, -1 };
        static long hb;
        static const char *hand[2] = { "left", "right" };
        int s;
        for (s = 0; s < 2; s++) {
            poses_pose_t p;
            int ok = poses_get(s == 0 ? POSES_HAND_LEFT : POSES_HAND_RIGHT, &p);
            int beat = ok && (hb % 900) == 0;
            if (ok != known[s] || beat) {
                known[s] = ok;
                if (!ok)
                    camlog("controller %s: NO valid pose (off, asleep, or not tracked)",
                           hand[s]);
                else
                    camlog("controller %s: pos %.1f %.1f %.1f  fwd %.3f %.3f %.3f  "
                           "tracking_result=%d connected=%d",
                           hand[s], p.cod_origin[0], p.cod_origin[1], p.cod_origin[2],
                           p.cod_axis[0][0], p.cod_axis[0][1], p.cod_axis[0][2],
                           (int)p.tracking_result, p.connected);
            }
        }
        hb++;
    }

    got = 0;
    for (e = 0; e < 2; e++) {
        if (!poses_get_eye(e, &ey))
            continue;
        memcpy(g_eyepose[e].axis, ey.cod_axis, sizeof g_eyepose[e].axis);
        for (i = 0; i < 3; i++)
            g_eyepose[e].eye_off[i] = ey.cod_origin[i] - hmd.cod_origin[i];
        g_eyepose[e].valid = 1;
        got++;
    }
    /* The centre slot is the head itself: the HMD's own basis and no offset.
     * It needs no GetEyeToHeadTransform, so it stays available on a runtime
     * where the per-eye transform is refused. */
    if (g_head_valid) {
        memcpy(g_eyepose[CAM_EYE_CENTRE].axis, hmd.cod_axis,
               sizeof g_eyepose[CAM_EYE_CENTRE].axis);
        memset(g_eyepose[CAM_EYE_CENTRE].eye_off, 0,
               sizeof g_eyepose[CAM_EYE_CENTRE].eye_off);
        g_eyepose[CAM_EYE_CENTRE].valid = 1;
    }

    /* TRACKING DROPOUT: hold, then let go.
     *
     * A pose that goes invalid for a few frames is ordinary (exp 12's header
     * says so). The two candidate behaviours are both bad in different ways:
     * dropping straight back to the game's orientation SNAPS the world by
     * however far the head was turned, which is the single most unpleasant
     * thing a headset can do; holding the last pose forever leaves the view
     * stuck at an angle nobody can correct once the headset is off the head.
     * So: hold for a second, then let go and say so. */
    /* The HEAD pose is what orientation needs; the per-eye transforms are a
     * bonus. Gating this on both eyes would release the view on a runtime that
     * simply refuses GetEyeToHeadTransform, where the head pose is perfectly
     * good and mono head tracking would have worked. */
    if (g_head_valid) {
        if (g_pose_stale > STALE_LIMIT)
            camlog("head tracking: pose regained after %ld frames (eyes: %d/2)",
                   g_pose_stale, got);
        g_pose_stale = 0;
    } else if (++g_pose_stale == STALE_LIMIT + 1) {
        g_eyepose[0].valid = g_eyepose[1].valid = g_eyepose[2].valid = 0;
        camlog("head tracking: no valid HMD pose for %d frames -- releasing the "
               "view back to the game's orientation", STALE_LIMIT);
    } else if (g_pose_stale > STALE_LIMIT) {
        g_eyepose[0].valid = g_eyepose[1].valid = g_eyepose[2].valid = 0;
    }

    ht_check_eye_offsets();

    /* WAIT FOR THE POSE TO SETTLE BEFORE THE AUTOMATIC RECENTRE.
     *
     * g_yaw0 is captured once and then defines "straight ahead" for the whole
     * session, so capturing it from the first pose after poses_attach() -- which
     * is the moment the runtime is least likely to have a real one -- would nail
     * a wrong forward direction in place for as long as the game runs, with no
     * way to correct it. So the automatic recentre waits for SETTLE_FRAMES
     * consecutive poses that the runtime itself reports as Running_OK. A human
     * asking for a recentre does not wait: they can see what they are doing. */
    if (g_head_valid && hmd.tracking_result == TRACKING_RUNNING_OK)
        g_good_run++;
    else
        g_good_run = 0;

    if (g_recentre && g_head_valid && (g_recentre_now || g_good_run >= SETTLE_FRAMES)) {
        InterlockedExchange(&g_recentre, 0);
        g_recentre_now = 0;
        g_yaw0 = ht_yaw_of((const float *)hmd.cod_axis);
        memcpy(g_ref_pos, hmd.cod_origin, sizeof g_ref_pos);
        g_have_ref_pos = 1;
        camlog("recentred after %ld steady poses (tracking_result %d): head yaw "
               "%.1f deg is now straight ahead; reference position %.1f %.1f %.1f",
               g_good_run, (int)hmd.tracking_result, g_yaw0 * 57.29578f,
               g_ref_pos[0], g_ref_pos[1], g_ref_pos[2]);
    } else if (g_recentre) {
        /* Explain the silence: "waiting for a steady pose" and "the recentre
         * code never runs" otherwise look identical from the log. */
        static long moans;
        if ((moans++ % 300) == 0)
            camlog("recentre pending: head pose %s, tracking_result %d, "
                   "%ld/%d steady poses so far",
                   g_head_valid ? "valid" : "INVALID", (int)hmd.tracking_result,
                   g_good_run, SETTLE_FRAMES);
    }
}

/* Write the head orientation into the refdef, and the eye offset into the view
 * origin. Returns 1 if the ORIENTATION was written (in which case the caller
 * must restore the axis), and sets *did_pos if the origin was moved too.
 *
 * Nothing here is written unless the composed basis passes its checks. */
static int headtrack_apply(unsigned char *rd, int eye, int *did_pos)
{
    const float *game = (const float *)(rd + RD_VIEWAXIS);
    float G[9], H[9], F[9], off[3], world[3] = { 0.0f, 0.0f, 0.0f };
    ht_basis_t rep;
    float inv_err = 0.0f, rt_err = 0.0f;
    int   inv_ok = 1, i;
    float *org;

    *did_pos = 0;
    g_views++;
    if (g_ht_dead || !g_ht_enable || !g_ht_math_ok) {
        ht_note(0, g_ht_dead ? "disabled for the session after repeated rejects"
                : !g_ht_math_ok ? "the maths self-check failed at load"
                : "switched off (RCtrl+F10 or nohead.on)");
        return 0;
    }
    /* CAM_EYE_CENTRE is a real slot here, not just at the call site: it is the
     * head with a zero eye offset, and it is what makes head tracking visible
     * with gameframe.asi absent. Rejecting it here would reintroduce the exact
     * silent-nothing this round was sent to fix, one layer further down. */
    if (eye < CAM_EYE_LEFT || eye > CAM_EYE_CENTRE)
        return 0;
    if (!g_eyepose[eye].valid) {
        ht_note(0, "no valid HMD pose (tracking lost, or the runtime stopped)");
        return 0;
    }

    /* The game's own view axis is the reference frame. If it is not a basis we
     * are looking at the wrong memory or at a torn read -- leave everything
     * alone rather than write a "best effort" rotation. */
    if (!ht_build_reference(game, g_ht_mode, G)) {
        static int moaned;
        if (!moaned) {
            moaned = 1;
            check_axis("refdef axis REJECTED as a reference:", game);
        }
        ht_note(0, "the game's own view axis is not a valid basis");
        return 0;
    }

    /* Recentre, then compose. H is the head in the (recentred) tracking frame;
     * G is that frame in the world; F = H * G is the head in the world. */
    ht_rotate_rows_yaw(g_eyepose[eye].axis, -g_yaw0, H);
    ht_compose(H, G, F);

    /* THE ROUND TRIP HOLDS IN BOTH MODES. F*G^T must come back as H, which
     * catches a transposed head, a swapped order, a non-rotation reference and
     * arithmetic damage -- and unlike the yaw invariant it still says something
     * when the reference is the game's full orientation. Until it existed,
     * HT_REF_FULL had no runtime guard at all (LOW-8), which made the mode a
     * player switches to when yaw-only feels wrong also the mode with the least
     * checking behind it. */
    if (!ht_check_round_trip(F, G, H, 1e-3f, &rt_err))
        inv_ok = 0;
    if (inv_ok && g_ht_mode == HT_REF_YAW_ONLY)
        inv_ok = ht_check_yaw_invariant(F, H, 1e-3f, &inv_err);
    if (!ht_check_basis(F, &rep) || !inv_ok) {
        ht_reject(&rep, inv_err, g_ht_mode == HT_REF_YAW_ONLY);
        if (rt_err > 1e-3f)
            camlog("*** round trip |F*G^T - H| = %.6f (want 0): the composition "
                   "and the reference disagree", rt_err);
        ht_note(0, "the composed basis failed its checks");
        return 0;
    }
    g_ht_fail = 0;

    /* Position: the eye's offset from the head, plus (optionally) how far the
     * head has moved since the recentre. Both are tracking-space vectors, so
     * they get the same recentre yaw as the orientation and are then rotated
     * into the world by the reference basis. */
    if (g_eye_off_ok) {
        memcpy(off, g_eyepose[eye].eye_off, sizeof off);
        if (g_ht_pos && g_have_ref_pos)
            for (i = 0; i < 3; i++)
                off[i] += g_head_pos[i] - g_ref_pos[i];
        ht_vec_yaw(off, -g_yaw0, off);
        ht_ref_to_world(G, off, world);
        org = (float *)(rd + RD_VIEWORG);
        for (i = 0; i < 3; i++)
            org[i] += world[i];
        *did_pos = 1;
    }

    /* LOG BEFORE THE WRITE. `game` aliases refdef+0x34; once the composed basis
     * is memcpy'd over it, printing game[] prints the composed basis and the
     * log reads as though the game and the head agreed exactly. That is the
     * house speciality -- an instrument that reports something reassuring -- so
     * the order of these two statements is load-bearing. */
    if (!g_ht_first) {
        g_ht_first = 1;
        camlog("HEAD ORIENTATION LIVE (mode=%s, position tracking %s)",
               g_ht_mode == HT_REF_FULL ? "full" : "yaw-only",
               g_ht_pos ? "on" : "off");
        camlog("  game fwd  %.4f %.4f %.4f -> ref yaw %.1f deg",
               game[0], game[1], game[2], ht_yaw_of(G) * 57.29578f);
        camlog("  head fwd  %.4f %.4f %.4f (recentred, tracking space)",
               H[0], H[1], H[2]);
        camlog("  final fwd %.4f %.4f %.4f  det %+.5f", F[0], F[1], F[2], rep.det);
        camlog("  eye %d offset in world = %.4f %.4f %.4f (%s)", eye,
               world[0], world[1], world[2],
               *did_pos ? "from the headset's own eye-to-head"
                        : "NOT applied -- assumed IPD shift instead");
    }

    /* --- the running answer to "is it working?" ------------------------
     *
     * game pitch  is what the MOUSE is doing vertically, straight off the
     *             refdef: asin of the forward row's z.
     * head pitch  is what the HEAD is doing, in the recentred reference frame.
     * In yaw-only mode the first of those does not reach the picture at all,
     * and that is the single fact the field report turns on. */
    g_last_game_pitch = asinf(game[2] > 1.0f ? 1.0f : (game[2] < -1.0f ? -1.0f : game[2]))
                        * 57.29578f;
    g_last_head_yaw   = ht_yaw_of(H) * 57.29578f;
    g_last_head_pitch = asinf(H[2] > 1.0f ? 1.0f : (H[2] < -1.0f ? -1.0f : H[2]))
                        * 57.29578f;
    g_views_oriented++;
    ht_note(1, "composed basis accepted");

    /* THE MOUSE IS PITCHING AND THE PICTURE IS NOT. Said once, in words, the
     * first time it is unmistakably happening. "head tracking worked until I
     * moved the mouse" should have been one line in a log, not an audit. */
    if (g_ht_mode == HT_REF_YAW_ONLY && !g_pitch_warned &&
        (g_last_game_pitch > 20.0f || g_last_game_pitch < -20.0f)) {
        g_pitch_warned = 1;
        camlog("*** MOUSE PITCH IS BEING DISCARDED. The game's view is pitched "
               "%+.1f deg but yaw-only mode keeps the picture level, so the mouse "
               "moves the GUN vertically and not the PICTURE.", g_last_game_pitch);
        camlog("*** This is the yaw-only reference working as designed, not a "
               "tracking failure. RCtrl+F11 (or reffull.on) switches to the full "
               "reference, where mouse pitch moves the picture as it always did.");
    }

    /* A periodic line, so a playtest log answers "did it ever stop?" by
     * inspection rather than by reasoning about early returns. ~4 views per
     * frame, so this is roughly every 8 seconds at 60 fps. */
    if ((g_views % 1800) == 0)
        camlog("heartbeat: mode=%s, oriented %ld/%ld views, game pitch %+.1f deg, "
               "head yaw %+.1f pitch %+.1f deg, rejects %ld, recentre yaw %+.1f deg",
               g_ht_mode == HT_REF_FULL ? "full" : "yaw-only",
               g_views_oriented, g_views, g_last_game_pitch,
               g_last_head_yaw, g_last_head_pitch, g_ht_fail_total,
               g_yaw0 * 57.29578f);

    memcpy(rd + RD_VIEWAXIS, F, sizeof F);
    return 1;
}

void __cdecl hk_body(void *out, void *in)
{
    LONG n = InterlockedIncrement(&g_calls);
    LONG eye = g_eye;
    unsigned char *rd = (unsigned char *)in;
    float save_org[3], save_axis[9], save_fov[2];
    float sent_org[3], sent_axis[9];
    int   moved = 0, fov_set = 0, oriented = 0, head_pos = 0;

    /* THE CAMERA IS MOVED AND TURNED HERE.
     *
     * Orientation: the head basis composed onto the game's heading, written as
     * refdef.viewaxis. headtrack_apply() writes NOTHING unless the composed
     * basis passes its checks, so "no pose", "bad pose" and "maths broken" all
     * degrade to the same safe thing -- the position-only stereo below.
     *
     * Position: the eye's real offset from the head, from the headset's own
     * GetEyeToHeadTransform. When that is unavailable or not believable we fall
     * back to shifting sideways by half an ASSUMED IPD along the view's LEFT
     * axis (refdef axis row 1 -- `left`, not `right`, camera-hook-plan §2.1).
     * Note that the fallback reads the axis AFTER the orientation was written,
     * so it shifts along the head-rotated left axis, not the game's. */
    if (eye >= CAM_EYE_LEFT && eye <= CAM_EYE_CENTRE) {
        float *org = (float *)(rd + RD_VIEWORG);
        memcpy(save_org, org, sizeof save_org);
        memcpy(save_axis, rd + RD_VIEWAXIS, sizeof save_axis);

        /* THE GAME'S OWN AIM, CAPTURED BEFORE WE TOUCH IT.
         *
         * This is the refdef axis as the engine built it -- head tracking has
         * not been composed in yet -- so its forward row IS where the weapon
         * points. That makes it two things at once:
         *
         *   - ground truth for what the 0x2911E20/0x2911E24 globals actually
         *     are. The first aim dry run found them reading 0.00 through most of
         *     play, which refutes them being the view angles but does not say
         *     what they ARE; comparing them against a known-good aim yaw settles
         *     it without another disassembly session.
         *   - exactly what #41 needs. A crosshair that tells the truth has to be
         *     drawn where the WEAPON points, and that direction is right here,
         *     already in hand, needing no further reverse engineering at all.
         */
        g_aim_yaw   = ht_yaw_of(save_axis) * 57.29578f;
        g_aim_pitch = -asinf(save_axis[2] > 1.0f ? 1.0f :
                            (save_axis[2] < -1.0f ? -1.0f : save_axis[2])) * 57.29578f;
        memcpy(g_aim_fwd, save_axis, sizeof g_aim_fwd);
        moved = 1;

        oriented = headtrack_apply(rd, (int)eye, &head_pos);

        /* CAM_EYE_CENTRE is a mono view from the head, so it gets no eye
         * offset -- neither the headset's nor the assumed one. */
        if (!head_pos && eye != CAM_EYE_CENTRE) {
            const float *left = (const float *)(rd + RD_VIEWAXIS + 12);
            float half = g_ipd_units * 0.5f * (eye == CAM_EYE_LEFT ? 1.0f : -1.0f);
            org[0] += left[0] * half;
            org[1] += left[1] * half;
            org[2] += left[2] * half;
        }

        /* AND THE FIELD OF VIEW. The game renders ~60 degrees vertical; a
         * headset wants roughly double, and feeding the narrow FOV to a wide
         * display is what "super zoomed in" is. Setting the refdef tangents
         * lets the engine build its own projection from them, rather than us
         * hand-building a matrix and having to get its handedness right. */
        /* CENTRE GETS THE FOV TOO. It is exempt from the eye OFFSET above --
         * a mono view from the head has no parallax to add -- but it is still a
         * view being shown on a headset, and it was excluded here as well. That
         * left the one configuration meant for testing head tracking on a flat
         * monitor, and every nocap.on run, rendering at the game's ~80 degrees
         * and then being cropped and blown up as if it were 124: the "still
         * pretty zoomed" of the first playtest. gameframe.asi's two eyes carry
         * identical tangents (they differ only in the off-centre part, which we
         * do not use), so eye 0's are the right ones to ask for. */
        if (g_get_fov) {
            float tx, ty;
            int fov_eye = (eye == CAM_EYE_CENTRE) ? CAM_EYE_LEFT : (int)eye;
            if (g_get_fov(fov_eye, &tx, &ty)) {
                float *fx = (float *)(rd + RD_TANHALFFOVX);
                float *fy = (float *)(rd + RD_TANHALFFOVY);
                save_fov[0] = *fx; save_fov[1] = *fy;
                *fx = tx; *fy = ty;
                fov_set = 1;
            }
        }

        /* WHERE THE WEAPON POINTS, IN THIS VIEW.
         *
         * Computed HERE because this is the one place where both halves exist
         * at once: the head-composed basis has just been written into the
         * refdef, and save_axis still holds the aim the engine built before we
         * touched it. Project one through the other and the result is the
         * screen position the crosshair should be at.
         *
         * The 2D crosshair is drawn at screen centre, and screen centre is now
         * wherever the head is looking -- so it does not merely look wrong, it
         * points somewhere the gun does not. Playtest: "makes accuracy hard
         * because your head is moving in world space but the gun is in a fixed
         * place."
         *
         * NDC, not pixels, because the consumer knows the target size and this
         * hook does not. Note the tangents used are the WIDENED ones written
         * just above -- the same ones the scene is rendered with -- so a
         * consumer mapping NDC onto the full back buffer lands correctly even
         * though gameframe.asi later crops the centre out of it.
         *
         * Rows are forward, LEFT, up (camera-hook-plan 2.1), so a target to the
         * left has a positive `l` and belongs at negative NDC x. */
        if (fov_set) {
            const float *F = (const float *)(rd + RD_VIEWAXIS);
            const float *a = g_aim_fwd;
            float f = a[0]*F[0] + a[1]*F[1] + a[2]*F[2];
            float l = a[0]*F[3] + a[1]*F[4] + a[2]*F[5];
            float u = a[0]*F[6] + a[1]*F[7] + a[2]*F[8];
            /* Behind the viewer, or so near the plane that the divide explodes.
             * Turning around far enough really does put the gun behind you --
             * that is the #44 case -- and a reticle smeared to infinity is a
             * worse answer than no reticle. */
            if (f > 0.01f) {
                float nx = -(l / f) / *(float *)(rd + RD_TANHALFFOVX);
                float ny =  (u / f) / *(float *)(rd + RD_TANHALFFOVY);
                g_aim_ndc[0] = nx;
                g_aim_ndc[1] = ny;
                g_aim_ndc_ok = (nx > -1.5f && nx < 1.5f && ny > -1.5f && ny < 1.5f);
            } else {
                g_aim_ndc_ok = 0;
            }
            /* THIS FRAME HAD A SCENE. Set last, and consumed by the getter.
             *
             * Without it the reticle is drawn from whatever the last scene left
             * behind, so it hangs over the main menu and the loading screens --
             * pointing at nothing, because there is no weapon. Playtest: "the
             * red one is permanently on as well ... not good for menus".
             *
             * A stale-value problem wants a freshness token, not a guess about
             * which UI states exist: 2D screens simply never reach this code, so
             * "was this computed since the last time anyone asked" is exactly
             * the right question and needs no list of menus to maintain. */
            InterlockedExchange(&g_aim_fresh, 1);
        }
    }

    /* Read BEFORE calling the original: `in` is the live refdef and the original
     * may legitimately change what it points at afterwards. */
    /* SAMPLE CONSECUTIVELY, not at multiples of 900. The first version did the
     * latter and reported eye=1 every single time, which looked exactly like a
     * stuck alternation. It was aliasing: RESULTS.md §6 MEASURED two
     * R_SetViewParms calls per frame (two view slots, different `out`
     * pointers), so every multiple of 900 landed on the same frame parity. A
     * run of consecutive calls shows the real pattern and cannot alias.
     *
     * AND KEEP SAMPLING, every 3600 calls. The window used to be 1000-1011 and
     * nothing afterwards, which put every sample in the first few seconds --
     * i.e. in the MAIN MENU, whose background is two static cameras. Two
     * playtest reports were analysed off menu frames before that became
     * obvious: the origins never moved between samples, and both reports were
     * about gameplay. Four consecutive calls every 3600 keeps the run-length
     * that defeats aliasing while actually observing what is complained about. */
    if ((n >= 1000 && n < 1012) || n <= 2 || (n % 3600) < 4) {
        const unsigned char *rd = (const unsigned char *)in;
        const float *org  = (const float *)(rd + RD_VIEWORG);
        const float *axis = (const float *)(rd + RD_VIEWAXIS);
        camlog("call #%ld  out=%p in=%p  eye=%ld%s%s", n, out, in, eye,
             moved ? " (camera shifted)" : "",
             oriented ? " (HEAD ORIENTATION applied)" : "");
        camlog("  refdef vieworg  = %.3f %.3f %.3f", org[0], org[1], org[2]);
        camlog("  refdef fwd      = %.5f %.5f %.5f", axis[0], axis[1], axis[2]);
        camlog("  refdef left     = %.5f %.5f %.5f", axis[3], axis[4], axis[5]);
        camlog("  refdef up       = %.5f %.5f %.5f", axis[6], axis[7], axis[8]);
        camlog("  tanHalfFov      = %.5f %.5f   zNear = %.5f",
             *(const float *)(rd + RD_TANHALFFOVX),
             *(const float *)(rd + RD_TANHALFFOVY),
             *(const float *)(rd + RD_ZNEAR));
        check_axis("refdef axis ", axis);
        if (oriented) {
            camlog("  head-composed fwd  = %.5f %.5f %.5f", axis[0], axis[1], axis[2]);
            camlog("  head-composed left = %.5f %.5f %.5f", axis[3], axis[4], axis[5]);
            camlog("  head-composed up   = %.5f %.5f %.5f", axis[6], axis[7], axis[8]);
        }
    }

    /* WHAT WE ACTUALLY HANDED THE ENGINE. Snapshotted here, before the call,
     * because the post-call cross-check below compares the engine's output
     * against its INPUT -- and by then we will have restored the refdef, so the
     * refdef no longer holds what the engine was given.
     *
     * The original version of that cross-check compared against the restored
     * refdef. That was correct while this hook only observed; the moment it
     * started moving the camera the check began reporting MISMATCH on a
     * perfectly healthy frame, i.e. it would have cried wolf exactly when
     * someone was watching it for a real fault. */
    memcpy(sent_org,  rd + RD_VIEWORG,  sizeof sent_org);
    memcpy(sent_axis, rd + RD_VIEWAXIS, sizeof sent_axis);

    call_original(out, in);

    /* RESTORE. Not optional: camera-hook-plan §3.4 measured 122 readers of the
     * view origin across the client, all reaching it through cg->refdef by
     * pointer. Leaving our offset in place would move the player's idea of
     * where they are -- audio, tracers, and anything else that asks the refdef
     * where the camera is -- not just the picture.
     *
     * The AXIS matters at least as much. The same readers take the view
     * direction from here; leaving the head rotation in place would aim the
     * player's weapon, their sound field and their PVS wherever they last
     * looked, and would then compose the NEXT frame's head rotation on top of
     * this one -- a view that spins up under its own feedback. Restored
     * unconditionally whenever we saved it, on every path out. */
    if (moved) {
        memcpy(rd + RD_VIEWORG, save_org, sizeof save_org);
        memcpy(rd + RD_VIEWAXIS, save_axis, sizeof save_axis);
    }
    if (fov_set) {
        *(float *)(rd + RD_TANHALFFOVX) = save_fov[0];
        *(float *)(rd + RD_TANHALFFOVY) = save_fov[1];
    }

    /* Now cross-check the OUTPUT. R_SetViewParms copies the origin across, so
     * agreement here means the refdef offsets, the GfxViewParms offsets and the
     * EDI/ESI convention are ALL correct -- none of the three could look right
     * on its own if another were wrong. */
    if ((n >= 1000 && n < 1012) || n <= 2 || (n % 3600) < 4) {
        const unsigned char *vp = (const unsigned char *)out;
        const float *vorg = (const float *)(vp + VP_ORIGIN);
        const float *rorg = (const float *)((const unsigned char *)in + RD_VIEWORG);
        float ax[9];
        float d, a;
        int i;
        memcpy(ax + 0, vp + VP_AXIS0, 12);
        memcpy(ax + 3, vp + VP_AXIS1, 12);
        memcpy(ax + 6, vp + VP_AXIS2, 12);
        camlog("  viewParms origin= %.3f %.3f %.3f (w=%.3f)", vorg[0], vorg[1], vorg[2], vorg[3]);
        check_axis("viewParms ax", ax);
        d = fabsf(vorg[0]-sent_org[0]) + fabsf(vorg[1]-sent_org[1]) + fabsf(vorg[2]-sent_org[2]);
        camlog("  |viewParms.origin - what we sent| = %.6f  -> %s", d,
             d < 1e-3f ? "MATCH: convention and BOTH structure layouts confirmed"
                       : "MISMATCH: something in the chain is wrong");
        /* THE ROTATION REACHED THE RENDERER, or it did not. Without this line,
         * "the engine used our basis" and "the engine rebuilt the basis from
         * the player's angles and ignored ours" produce identical evidence:
         * a viewParms axis that is orthonormal and plausible. */
        a = 0.0f;
        for (i = 0; i < 9; i++) {
            float e = fabsf(ax[i] - sent_axis[i]);
            if (e > a) a = e;
        }
        camlog("  max|viewParms.axis - what we sent| = %.6f  -> %s", a,
             a < 1e-3f ? "the engine used OUR basis"
                       : "the engine did NOT use our basis -- orientation is being overridden");
        camlog("  refdef restored: org %.3f %.3f %.3f (offset returned %.4f)",
             rorg[0], rorg[1], rorg[2],
             fabsf(rorg[0]-sent_org[0]) + fabsf(rorg[1]-sent_org[1]) + fabsf(rorg[2]-sent_org[2]));
        camlog("  zNear out       = %.5f", *(const float *)(vp + VP_ZNEAR));
    }
}

/* --- TRUE DUAL VIEW ------------------------------------------------------
 *
 * R_RenderScene @ 0x6C8C40 is plain __cdecl taking one argument (the refdef)
 * with a single caller, so unlike R_SetViewParms it needs no asm. Calling the
 * original TWICE per frame gives two full scene renders with two different
 * cameras -- camera-hook-plan §3.3 measured that the view-parms slot is
 * bump-allocated, so the second call gets its own GfxViewParms rather than
 * stamping on the first.
 *
 * After each render we ask gameframe.asi to capture the scene render target
 * into that eye's texture. Without the capture the second render would simply
 * overwrite the first and we would be back to one view per frame.
 *
 * THIS IS WHAT REMOVES THE TWITCH. Alternate-eye rendering drew one eye per
 * frame, so the monitor -- which shows every frame -- saw the camera jumping
 * left-right at frame rate, and each eye updated at only half the game's rate.
 * Now every frame contains both eyes and the monitor shows one steady view.
 *
 * The pose is still applied inside the 0x6C7F80 hook, NOT here, and that is
 * deliberate: §3.4 measured that R_RenderScene caches the view origin into the
 * lighting/PVS globals at 0x3AC3060 and 0x396A644 BEFORE the view is built.
 * Patching inside 0x6C7F80 leaves those holding the HEAD pose, which is what
 * keeps lighting and visibility coherent between the two eyes.
 */
typedef void (__cdecl *pfn_renderscene)(void *refdef);
static pfn_renderscene o_R_RenderScene;
static void (*g_capture)(int);
static int  (*g_cap_enabled)(void);   /* gameframe.asi's bo1vr_capture_enabled */
static int  g_dual_logged;
static int  g_mono_logged;

/* R_RenderSceneInternal 0x6C8CD0 -- the function that actually builds the scene
 * view. See the note at the hook site in init(). */
typedef void (__cdecl *pfn_rsinternal)(void *refdef, unsigned arg2);
static pfn_rsinternal o_R_RenderSceneInternal;
static int  g_internal_hooked;   /* 0 -> keep the old whole-frame eye behaviour */
static LONG g_internal_calls;

/* THE SCENE VIEW, AND ONLY THE SCENE VIEW.
 *
 * With this in place the eye stops being a mode that hangs over the whole frame
 * and becomes what it should always have been: a property of one render. The
 * previous build had to leave CAM_EYE_CENTRE set permanently because the eye had
 * to still be set when the scene view was built somewhere we were not watching;
 * that also handed the head basis and the headset's 124-degree FOV to every
 * other view built through R_SetViewParms -- shadow, portal, sun -- which is
 * wrong even where it is not visible.
 *
 * Set on entry, RESTORED on exit rather than cleared, so this composes with the
 * dual-view path (which sets eye 0 / eye 1 around its own two calls) instead of
 * fighting it. */
static void __cdecl hk_R_RenderSceneInternal(void *refdef, unsigned arg2)
{
    LONG prev = g_eye;
    LONG n = InterlockedIncrement(&g_internal_calls);

    if (n == 1)
        camlog("R_RenderSceneInternal reached -- eye scoping is live (entry eye=%ld)", prev);

    /* Only take over when nobody else has claimed the eye. Inside dual view the
     * caller has already set 0 or 1 and that is the more specific answer. */
    if (prev == CAM_EYE_NONE && g_cap_enabled && !g_cap_enabled())
        bo1vr_camera_set_eye(CAM_EYE_CENTRE);

    o_R_RenderSceneInternal(refdef, arg2);

    bo1vr_camera_set_eye((int)prev);
}

static void __cdecl hk_R_RenderScene(void *refdef)
{
    /* Log the FIRST call unconditionally. Without this, "R_RenderScene never
     * ran" and "it ran but the capture entry would not resolve" produce exactly
     * the same evidence -- nothing at all -- and the first run of this hook hit
     * precisely that ambiguity: the log showed the hook installed and then
     * silence, because the session never left the menu and R_RenderScene only
     * runs for a 3D scene. */
    if (!g_dual_logged) {
        g_dual_logged = 1;
        camlog("R_RenderScene reached (first 3D scene)");
    }

    /* ONE pose sample per frame, and both eyes are built from it. Sampling per
     * view instead would give the two eyes poses from different instants: the
     * pair would disagree by however far the head turned in between, which is a
     * vertical-disparity headache rather than a visible glitch.
     *
     * THIS MUST STAY ABOVE THE g_capture BLOCK. The first version of this had it
     * below, and the early return in the "no gameframe.asi" branch meant
     * poses_attach() was never reached and head tracking was silently, PERMANENTLY
     * dead in precisely the configuration RESULTS.md §5 documents as the
     * isolation test -- gameframe.asi moved aside, this hook the only variable.
     * The one arrangement in which someone would go looking for a head-tracking
     * bug was the one arrangement in which head tracking could not run. */
    headtrack_sample();

    if (!g_capture) {
        HMODULE gf = GetModuleHandleA("gameframe.asi");
        if (gf) g_capture = (void (*)(int))GetProcAddress(gf, "bo1vr_capture_eye");
        if (!g_capture) {
            static int moaned;
            if (!moaned) {
                moaned = 1;
                camlog("*** NO STEREO: bo1vr_capture_eye unavailable (gameframe.asi %s).",
                       gf ? "loaded but lacks the export" : "NOT LOADED");
                camlog("*** one render per frame, MONO, no eye offset -- but head "
                       "orientation still applies, so this is the configuration to "
                       "test head tracking in on a flat monitor.");
            }
            /* MONO, not "leave the camera alone". A second render would only
             * overwrite the first with nothing to capture it, so render once --
             * with the CENTRE eye, which applies the head orientation and no eye
             * offset. Without this the standalone configuration sets no eye at
             * all, hk_body does nothing, and head tracking cannot be observed
             * even though it is running. */
            bo1vr_camera_set_eye(CAM_EYE_CENTRE);
            o_R_RenderScene(refdef);
            bo1vr_camera_set_eye(CAM_EYE_NONE);
            return;
        }
        g_get_fov = (int (*)(int, float *, float *))
                    GetProcAddress(GetModuleHandleA("gameframe.asi"), "bo1vr_get_eye_fov");
        g_cap_enabled = (int (*)(void))
                    GetProcAddress(GetModuleHandleA("gameframe.asi"), "bo1vr_capture_enabled");
        camlog("dual view: capture entry resolved -- two renders per frame%s",
               g_get_fov ? "; HMD FOV available" : "; NO HMD FOV (still game FOV)");
    }

    /* RENDERING TWICE IS ONLY WORTH IT IF SOMETHING CAPTURES THE FIRST ONE.
     *
     * Under nocap.on (and novk/notex/probe2) bo1vr_capture_eye returns without
     * doing anything, so the second render simply overwrites the first and the
     * eye textures are filled from the back buffer at Present instead. Two
     * renders then buy nothing and cost plenty: the back buffer ends up holding
     * whichever camera drew last, and gameframe.asi -- still seeing no dual
     * view -- was flipping the eye every frame on top of that. Pulsing.
     *
     * One CENTRE render is the honest shape for that configuration: head
     * orientation and headset FOV, no eye offset, nothing to alternate. Mono,
     * but coherent. If gameframe.asi is older than this export, assume capture
     * works and behave exactly as before. */
    if (g_cap_enabled && !g_cap_enabled()) {
        if (!g_mono_logged) {
            g_mono_logged = 1;
            camlog("MONO: gameframe.asi reports capture disabled -- ONE centre render "
                   "per frame (head orientation + headset FOV, no eye offset)");
        }
        bo1vr_camera_set_eye(CAM_EYE_CENTRE);
        o_R_RenderScene(refdef);
        /* Once R_RenderSceneInternal is hooked, the eye is scoped there and
         * leaving it set here would put it back to being a whole-frame mode --
         * the very thing that hook exists to stop. Clear it, and keep the old
         * leave-it-set behaviour ONLY as the degraded path for when that hook
         * could not be installed, so a hook failure loses precision rather than
         * losing head tracking altogether. */
        if (g_internal_hooked) {
            bo1vr_camera_set_eye(CAM_EYE_NONE);
            return;
        }
        /* AND IT IS DELIBERATELY LEFT SET -- do not "fix" this to CAM_EYE_NONE.
         *
         * MEASURED: R_SetViewParms (0x6C7F80) has five call sites --
         * 0x6C8C5F, 0x6C8D96, 0x6C8E73, 0x6C8F5B, 0x6CEF64 -- and only the
         * FIRST is inside the R_RenderScene we hook. Ghidra shows even that one
         * is conditional:
         *
         *     if (*(char *)(DAT_03b1fd24 + 0x14) != '\0') { ...; R_SetViewParms(); }
         *
         * The views that actually reach the screen come through
         * R_RenderSceneInternal (0x6C8CD0) at 0x6C8D96, which this hook does
         * not wrap at all. So bracketing o_R_RenderScene sets the eye across a
         * window that usually contains NO R_SetViewParms call, and the log
         * showed exactly that: every sampled call read eye=-1.
         *
         * That also explains why head tracking appeared to work before and
         * stopped when the alternate-eye fallback was removed. It was never the
         * bracket doing it -- gameframe.asi set the eye at Present and left it
         * set, so it happened to still be set when the real scene view was
         * built. The alternation is what made it pulse; the persistence is what
         * made it work at all. Keep the persistence, drop the alternation.
         *
         * Leaving CENTRE set means all five sites get the head basis and the
         * headset FOV, not just the scene. That is the same breadth the old
         * fallback already had, so it is no worse -- and hk_body restores the
         * refdef after every call, so nothing leaks into the game's own state.
         * Narrowing it to the scene view wants a hook on 0x6C8CD0, which is the
         * proper fix and a bigger change than a playtest should carry. */
        return;
    }

    bo1vr_camera_set_eye(0);
    o_R_RenderScene(refdef);
    g_capture(0);

    bo1vr_camera_set_eye(1);
    o_R_RenderScene(refdef);
    g_capture(1);

    /* Back to "not ours", so any later view this frame -- and the next frame up
     * to its own R_RenderScene -- is left completely alone. */
    bo1vr_camera_set_eye(CAM_EYE_NONE);

    /* SLOT WATCH. camera-hook-plan 5.1: the view-parms slot is bump-allocated
     * with no visible bound check, and rendering the scene twice doubles the
     * consumption. If the pool is smaller than we now need, the overrun writes
     * past frontEndData+0x88000 into whatever lives after it -- which would
     * show up exactly as this does: fine for a while, then a crash. Log the
     * high-water mark so the guess becomes a number. */
    if (g_base) {
        unsigned char *fed = *(unsigned char **)(g_base + (VA_FRONTENDDATA - PREFERRED_BASE));
        /* Report the FIRST reading unconditionally, whatever it is.
         *
         * The first version of this watch logged only when a plausible value
         * ROSE, and after a crash it had produced no lines at all -- which is
         * useless, because "the counter never grew", "the pointer was null" and
         * "the address is wrong so the value was garbage" all look identical
         * from outside. An instrument that is silent when it fails teaches
         * nothing. */
        static int reported;
        if (!reported) {
            reported = 1;
            if (!fed)
                camlog("slot watch: frontEndData is NULL at %p", 
                       (void *)(g_base + (VA_FRONTENDDATA - PREFERRED_BASE)));
            else
                camlog("slot watch: frontEndData=%p viewParmsCount=%u (raw, after 2 renders)",
                       (void *)fed, *(unsigned *)(fed + FED_VIEWPARMSCOUNT));
        }
        if (fed) {
            unsigned c = *(unsigned *)(fed + FED_VIEWPARMSCOUNT);
            if (c > g_max_slots) {
                g_max_slots = c;
                camlog("view-parms high-water mark: %u slots", c);
            }
        }
    }
}

static DWORD WINAPI init(LPVOID p)
{
    unsigned char *base = (unsigned char *)GetModuleHandleA(NULL);
    void *target, *scene, *internal;
    MH_STATUS st;

    (void)p;
    if (!base) { camlog("no module base"); return 0; }
    g_base = base;

    /* THE ROTATION MATHS CHECKS ITSELF BEFORE IT IS ALLOWED TO RUN.
     *
     * This is the same ht_selfcheck() that experiments/14_headtrack's
     * `make check` runs offline, against the same object file, and it is run
     * again here because "it passed on the build machine" and "the code in this
     * process is correct" are different statements. A failure disables head
     * orientation for the session rather than shipping a mirrored world: the
     * hook then behaves exactly as it did before this change. */
    {
        char why[192];
        int rc = ht_selfcheck(why, (int)sizeof why);
        g_ht_math_ok = (rc == 0);
        if (rc)
            camlog("*** HEAD-TRACKING MATHS SELF-CHECK FAILED at case %d: %s "
                   "-- ORIENTATION DISABLED, position-only stereo", rc, why);
        else
            camlog("head-tracking maths self-check: PASS (%d/%d cases) -- "
                   "orientation enabled", HT_SELFCHECK_CASES,
                   HT_SELFCHECK_CASES);
        ht_log_state("at load");
        camlog("runtime controls: RCtrl+F9 recentre, F10 orientation on/off, "
               "F11 yaw-only/full reference, F12 positional; or the files "
               "nohead.on / reffull.on / roomscale.on / recentre.on in C:\\bo1vr");
    }
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
    camlog("hooked R_SetViewParms; trampoline=%p", o_R_SetViewParms);

    scene = base + (VA_R_RenderScene - PREFERRED_BASE);
    if (MH_CreateHook(scene, (void *)hk_R_RenderScene, (void **)&o_R_RenderScene) == MH_OK &&
        MH_EnableHook(scene) == MH_OK)
        camlog("hooked R_RenderScene %p -- DUAL VIEW (two renders per frame)", scene);
    else
        camlog("FAILED to hook R_RenderScene at %p -- falling back to alternate-eye", scene);

    /* AND THE FUNCTION THAT ACTUALLY BUILDS THE SCENE VIEW.
     *
     * R_RenderScene (0x6C8C40) turned out not to be it -- see the note in
     * hk_R_RenderScene. R_RenderSceneInternal (0x6C8CD0) is: Ghidra shows it
     * bump-allocating a view-parms slot out of the pool and calling
     * R_SetViewParms exactly once, at 0x6C8D96,
     *
     *     puVar1 = frontEndData + 0x88000 + slot * 0x140;   // sizeof GfxViewParms
     *     slot++;
     *     R_SetViewParms();
     *     ... FUN_006c6450(...)                             // the scene render
     *
     * which independently confirms both the 0x140 stride and the
     * frontEndData+0x88000 pool base this file's slot watch already assumed.
     *
     * Verified before hooking: it ends in a plain `ret` at 0x6C8E3F, so it is
     * __cdecl and the caller cleans; the function spans 0x6C8CD0..0x6C8E3F, so
     * the R_SetViewParms at 0x6C8D96 is inside it while the ones at 0x6C8E73
     * and 0x6C8F5B are not; and it has exactly one caller (0x6CF077).
     *
     * Bracketing HERE is what lets the eye stop being a global mode. */
    internal = base + (VA_R_RenderSceneInternal - PREFERRED_BASE);
    if (MH_CreateHook(internal, (void *)hk_R_RenderSceneInternal,
                      (void **)&o_R_RenderSceneInternal) == MH_OK &&
        MH_EnableHook(internal) == MH_OK) {
        g_internal_hooked = 1;
        camlog("hooked R_RenderSceneInternal %p -- the eye is now SCOPED to the scene view",
               internal);
    } else {
        camlog("FAILED to hook R_RenderSceneInternal at %p -- falling back to leaving the "
               "eye set across the whole frame (works, but also catches shadow/portal views)",
               internal);
    }
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
