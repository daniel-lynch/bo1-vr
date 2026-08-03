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
 * forward x left = up, and headtrack_mathcheck case 5 fails if it ever stops
 * doing so (the old |det| form was re-run against it: it passes a mirror). */
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
static volatile LONG g_eye = -1;
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
static int   g_ht_mode   = HT_REF_YAW_ONLY;
static int   g_ht_pos;                  /* room-scale translation: OFF, see below */
static int   g_ht_bound;                /* poses_attach() succeeded           */
static int   g_ht_dead;                 /* disabled for the session           */
static int   g_ht_first;                /* logged the first applied frame     */
static long  g_ht_fail;                 /* consecutive rejected bases         */
static long  g_ht_fail_total;
static float g_yaw0;                    /* recentre offset, radians           */
static volatile LONG g_recentre = 1;    /* recentre on the first valid pose   */
static int   g_eye_off_ok = 1;          /* the per-eye offsets are believable */
static int   g_eyes_checked;
static float g_ref_pos[3];              /* head position at recentre          */
static int   g_have_ref_pos;

/* One frame's worth of pose, sampled once per frame in hk_R_RenderScene and
 * read by hk_body. Same thread (the render thread) for both, so no locking:
 * poses.c's seqlock already covers the OpenVR writer. */
static struct {
    int   valid;
    float axis[9];       /* eye basis, TRACKING coords, CoD convention (rows
                          * forward/left/up) -- exp 12's cod_axis verbatim   */
    float eye_off[3];    /* eye origin - head origin, tracking coords, units */
} g_eyepose[2];
static float g_head_pos[3];
static int   g_head_valid;

__declspec(dllexport) void bo1vr_camera_set_head_tracking(int on)
{
    g_ht_enable = on ? 1 : 0;
}

/* HT_REF_YAW_ONLY (default) or HT_REF_FULL -- see headtrack.h for the
 * trade-off. Only a headset can settle which is right, so both exist. */
__declspec(dllexport) void bo1vr_camera_set_ref_mode(int mode)
{
    g_ht_mode = (mode == HT_REF_FULL) ? HT_REF_FULL : HT_REF_YAW_ONLY;
}

/* Make the direction the head is facing right now read as "straight ahead".
 * Takes effect on the next sampled frame. */
__declspec(dllexport) void bo1vr_camera_recentre(void)
{
    InterlockedExchange(&g_recentre, 1);
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
    if (on && !g_have_ref_pos) InterlockedExchange(&g_recentre, 1);
}

/* The world-scale knob (exp 12 risk #2: units are ASSUMED inches). Changes the
 * scale of the HMD's own eye separation and of any positional tracking. */
__declspec(dllexport) void bo1vr_camera_set_units_per_metre(float u)
{
    poses_set_units_per_metre(u);
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
#define STALE_LIMIT 90          /* frames of dropout we hold the last pose for */
static long g_pose_stale;

static void headtrack_sample(void)
{
    poses_pose_t hmd, ey;
    int e, i, got;

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

    /* TRACKING DROPOUT: hold, then let go.
     *
     * A pose that goes invalid for a few frames is ordinary (exp 12's header
     * says so). The two candidate behaviours are both bad in different ways:
     * dropping straight back to the game's orientation SNAPS the world by
     * however far the head was turned, which is the single most unpleasant
     * thing a headset can do; holding the last pose forever leaves the view
     * stuck at an angle nobody can correct once the headset is off the head.
     * So: hold for a second, then let go and say so. */
    if (got == 2) {
        if (g_pose_stale > STALE_LIMIT)
            camlog("head tracking: pose regained after %ld frames", g_pose_stale);
        g_pose_stale = 0;
    } else if (++g_pose_stale == STALE_LIMIT + 1) {
        g_eyepose[0].valid = g_eyepose[1].valid = 0;
        camlog("head tracking: no valid HMD pose for %d frames -- releasing the "
               "view back to the game's orientation", STALE_LIMIT);
    } else if (g_pose_stale > STALE_LIMIT) {
        g_eyepose[0].valid = g_eyepose[1].valid = 0;
    }

    ht_check_eye_offsets();

    if (g_recentre && g_head_valid) {
        InterlockedExchange(&g_recentre, 0);
        g_yaw0 = ht_yaw_of((const float *)hmd.cod_axis);
        memcpy(g_ref_pos, hmd.cod_origin, sizeof g_ref_pos);
        g_have_ref_pos = 1;
        camlog("recentred: head yaw %.1f deg is now straight ahead; "
               "reference position %.1f %.1f %.1f",
               g_yaw0 * 57.29578f, g_ref_pos[0], g_ref_pos[1], g_ref_pos[2]);
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
    float inv_err = 0.0f;
    int   inv_ok = 1, i;
    float *org;

    *did_pos = 0;
    if (g_ht_dead || !g_ht_enable || !g_ht_math_ok)
        return 0;
    if (eye != 0 && eye != 1)
        return 0;
    if (!g_eyepose[eye].valid)
        return 0;

    /* The game's own view axis is the reference frame. If it is not a basis we
     * are looking at the wrong memory or at a torn read -- leave everything
     * alone rather than write a "best effort" rotation. */
    if (!ht_build_reference(game, g_ht_mode, G)) {
        static int moaned;
        if (!moaned) {
            moaned = 1;
            check_axis("refdef axis REJECTED as a reference:", game);
        }
        return 0;
    }

    /* Recentre, then compose. H is the head in the (recentred) tracking frame;
     * G is that frame in the world; F = H * G is the head in the world. */
    ht_rotate_rows_yaw(g_eyepose[eye].axis, -g_yaw0, H);
    ht_compose(H, G, F);

    if (g_ht_mode == HT_REF_YAW_ONLY)
        inv_ok = ht_check_yaw_invariant(F, H, 1e-3f, &inv_err);
    if (!ht_check_basis(F, &rep) || !inv_ok) {
        ht_reject(&rep, inv_err, g_ht_mode == HT_REF_YAW_ONLY);
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
    if (eye == 0 || eye == 1) {
        float *org = (float *)(rd + RD_VIEWORG);
        memcpy(save_org, org, sizeof save_org);
        memcpy(save_axis, rd + RD_VIEWAXIS, sizeof save_axis);
        moved = 1;

        oriented = headtrack_apply(rd, (int)eye, &head_pos);

        if (!head_pos) {
            const float *left = (const float *)(rd + RD_VIEWAXIS + 12);
            float half = g_ipd_units * 0.5f * (eye == 0 ? 1.0f : -1.0f);
            org[0] += left[0] * half;
            org[1] += left[1] * half;
            org[2] += left[2] * half;
        }

        /* AND THE FIELD OF VIEW. The game renders ~60 degrees vertical; a
         * headset wants roughly double, and feeding the narrow FOV to a wide
         * display is what "super zoomed in" is. Setting the refdef tangents
         * lets the engine build its own projection from them, rather than us
         * hand-building a matrix and having to get its handedness right. */
        if (g_get_fov) {
            float tx, ty;
            if (g_get_fov((int)eye, &tx, &ty)) {
                float *fx = (float *)(rd + RD_TANHALFFOVX);
                float *fy = (float *)(rd + RD_TANHALFFOVY);
                save_fov[0] = *fx; save_fov[1] = *fy;
                *fx = tx; *fy = ty;
                fov_set = 1;
            }
        }
    }

    /* Read BEFORE calling the original: `in` is the live refdef and the original
     * may legitimately change what it points at afterwards. */
    /* SAMPLE CONSECUTIVELY, not at multiples of 900. The first version did the
     * latter and reported eye=1 every single time, which looked exactly like a
     * stuck alternation. It was aliasing: R_SetViewParms runs ~3 times per
     * frame (shadow and portal views go through it too), so every multiple of
     * 900 landed on the same frame parity. A run of consecutive calls shows the
     * real pattern and cannot alias. */
    if ((n >= 1000 && n < 1012) || n <= 2) {
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
    if ((n >= 1000 && n < 1012) || n <= 2) {
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
static int  g_dual_logged;

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

    if (!g_capture) {
        HMODULE gf = GetModuleHandleA("gameframe.asi");
        if (gf) g_capture = (void (*)(int))GetProcAddress(gf, "bo1vr_capture_eye");
        if (!g_capture) {
            static int moaned;
            if (!moaned) {
                moaned = 1;
                camlog("no bo1vr_capture_eye (gameframe.asi %s) -- single render, "
                       "alternate-eye fallback", gf ? "loaded but lacks the export" : "not loaded");
            }
            /* A second render would only overwrite the first with nothing to
             * capture it, so render once and let the fallback handle the eyes. */
            o_R_RenderScene(refdef);
            return;
        }
        g_get_fov = (int (*)(int, float *, float *))
                    GetProcAddress(GetModuleHandleA("gameframe.asi"), "bo1vr_get_eye_fov");
        camlog("dual view: capture entry resolved -- two renders per frame%s",
               g_get_fov ? "; HMD FOV available" : "; NO HMD FOV (still game FOV)");
    }

    /* ONE pose sample per frame, and both eyes are built from it. Sampling per
     * view instead would give the two eyes poses from different instants: the
     * pair would disagree by however far the head turned in between, which is a
     * vertical-disparity headache rather than a visible glitch. */
    headtrack_sample();

    bo1vr_camera_set_eye(0);
    o_R_RenderScene(refdef);
    g_capture(0);

    bo1vr_camera_set_eye(1);
    o_R_RenderScene(refdef);
    g_capture(1);

    /* Back to "not ours", so any later view this frame -- and the next frame up
     * to its own R_RenderScene -- is left completely alone. */
    bo1vr_camera_set_eye(-1);

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
    void *target, *scene;
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
                   "orientation enabled, mode=%s", HT_SELFCHECK_CASES,
                   HT_SELFCHECK_CASES,
                   g_ht_mode == HT_REF_FULL ? "full" : "yaw-only");
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
