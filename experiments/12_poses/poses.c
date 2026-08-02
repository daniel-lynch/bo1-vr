/* poses.c -- HMD and controller poses for the bo1-vr mod.  (BAC-283)
 *
 * See poses.h for the API and for the OpenVR -> CoD coordinate derivation.
 * This file is deliberately buildable three ways from one source:
 *
 *   (default)          -> poses.o, a module for gameframe.c / the camera hook
 *   -DPOSES_SELFTEST   -> poses_selftest.asi, a plugin that reports what it can
 *                         see.  GATED BEHIND A MARKER FILE, see §selftest.
 *   -DPOSES_MATHCHECK  -> poses_mathcheck.exe, a console program that checks the
 *                         coordinate transform against hand-worked expectations
 *                         and never loads openvr_api.dll at all.
 *
 * NOTHING HERE CALLS IVRCompositor::WaitGetPoses.  gameframe.c owns the frame
 * clock; a second caller would fight it for frame pacing.  The render path
 * hands us its array (poses_update) and everyone else asks IVRSystem directly
 * (poses_poll).  That split is the whole design.
 *
 * No C++ and no __try/__except: 32-bit mingw has no SEH, and this toolchain's
 * DWARF-2 unwinder cannot walk BlackOps.exe's CFI-less MSVC frames (README
 * Decision 6).  The defence is to null-check every pointer, to validate every
 * float that comes back from the runtime, and never to keep a pose we cannot
 * show is a rotation.
 */

#define OPENVR_API_NODLL 1

#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include "openvr_capi.h"
#include "ivrsystem_023.h"
#include "poses.h"

/* Compiler-only barrier.  x86 does not reorder the loads and stores the
 * seqlock below cares about; the compiler would. */
#define POSES_BARRIER() __asm__ __volatile__("" ::: "memory")

/* ---------------------------------------------------------------- logging */

static int g_log_on;

static void plog(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    char path[MAX_PATH];
    DWORD n;
    HANDLE h;

    if (!g_log_on)
        return;

    memcpy(buf, "[poses] ", 8);
    va_start(ap, fmt);
    _vsnprintf(buf + 8, sizeof(buf) - 16, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 16] = '\0';
    strcat(buf, "\n");

    OutputDebugStringA(buf);
    /* A file is the only channel that survives a Steam launch: there is no
     * stderr and WINEDEBUG cannot be set (Exp. 9 / gameframe.c). */
    n = GetTempPathA(MAX_PATH - 24, path);
    if (n && n < MAX_PATH - 24) {
        lstrcatA(path, "bo1vr_poses.log");
        h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD w;
            WriteFile(h, buf, (DWORD)strlen(buf), &w, NULL);
            CloseHandle(h);
        }
    }
}

void poses_set_logging(int on) { g_log_on = on ? 1 : 0; }

/* ------------------------------------------------------------------ state */

static struct IVRSystem_023_FnTable *g_sys;
static HMODULE g_ovr;
static int     g_we_inited;          /* did WE call VR_InitInternal2? */

static poses_pose_t  g_slot[POSES_SLOT_COUNT];
static volatile LONG g_seq[POSES_SLOT_COUNT];   /* even = stable, odd = writing */
static uint32_t      g_index[POSES_SLOT_COUNT] = {
    POSES_INDEX_NONE, POSES_INDEX_NONE, POSES_INDEX_NONE
};

static volatile LONG g_frames;
static uint32_t      g_rescan_interval = 90;
static uint32_t      g_since_rescan;
static int           g_universe = ETrackingUniverseOrigin_TrackingUniverseStanding;
static float         g_predict;
static float         g_upm = POSES_UNITS_PER_METRE_DEFAULT;

static struct { int have; float m[3][4]; } g_eye2head[2];

static int g_warned_basis;           /* log a bad rotation once, not per frame */
static int g_warned_roles;

/* ------------------------------------------------------------------- math */

static int fin(float f)
{
    /* NaN fails the first test, +/-Inf the second. */
    return (f == f) && (f < 3.0e38f) && (f > -3.0e38f);
}

static float dot3(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* Squared length.  No sqrtf anywhere in this file: we only ever compare
 * against a tolerance band, so squares do, and that keeps the module free of
 * any libm dependency. */
static float len3sq(const float a[3])
{
    return dot3(a, a);
}

void poses_cod_dir_from_ovr(const float v[3], float out[3])
{
    float x, y, z;
    if (!v || !out) return;
    x = v[0]; y = v[1]; z = v[2];
    out[0] = -z;   /* CoD forward =  OpenVR -Z */
    out[1] = -x;   /* CoD left    =  OpenVR -X */
    out[2] =  y;   /* CoD up      =  OpenVR +Y */
}

void poses_cod_pos_from_ovr(const float v_m[3], float out[3])
{
    float t[3];
    if (!v_m || !out) return;
    poses_cod_dir_from_ovr(v_m, t);
    out[0] = t[0] * g_upm;
    out[1] = t[1] * g_upm;
    out[2] = t[2] * g_upm;
}

void poses_cod_from_ovr_m34(const float m34[3][4], float out_origin[3],
                            float out_axis[3][3])
{
    float v[3];
    if (!m34) return;

    if (out_origin) {
        v[0] = m34[0][3]; v[1] = m34[1][3]; v[2] = m34[2][3];
        poses_cod_pos_from_ovr(v, out_origin);
    }
    if (out_axis) {
        /* Column 2 is the device's +Z, which points BACKWARDS out of it. */
        v[0] = -m34[0][2]; v[1] = -m34[1][2]; v[2] = -m34[2][2];
        poses_cod_dir_from_ovr(v, out_axis[0]);          /* forward */
        /* Column 0 is the device's +X = its right, so left is its negation. */
        v[0] = -m34[0][0]; v[1] = -m34[1][0]; v[2] = -m34[2][0];
        poses_cod_dir_from_ovr(v, out_axis[1]);          /* left */
        v[0] =  m34[0][1]; v[1] =  m34[1][1]; v[2] =  m34[2][1];
        poses_cod_dir_from_ovr(v, out_axis[2]);          /* up */
    }
}

/* out = a * b, both 3x4 affine with an implied (0,0,0,1) last row. */
static void m34_mul(const float a[3][4], const float b[3][4], float out[3][4])
{
    float t[3][4];
    int i, j;
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++)
            t[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
        t[i][3] = a[i][0] * b[0][3] + a[i][1] * b[1][3] + a[i][2] * b[2][3] + a[i][3];
    }
    memcpy(out, t, sizeof(t));
}

/* Is this really a rigid transform?  A runtime that hands back garbage, a
 * partially written struct, or an FnTable slot mismatch will nearly always fail
 * one of these -- and a silently wrong pose is far more expensive to debug than
 * a pose reported as invalid. */
static int m34_is_rigid(const float m[3][4])
{
    float c0[3], c1[3], c2[3];
    int i, j;

    for (i = 0; i < 3; i++)
        for (j = 0; j < 4; j++)
            if (!fin(m[i][j]))
                return 0;

    for (i = 0; i < 3; i++) { c0[i] = m[i][0]; c1[i] = m[i][1]; c2[i] = m[i][2]; }

    /* Squared lengths within 1 +/- ~5%. */
    if (len3sq(c0) < 0.90f || len3sq(c0) > 1.10f) return 0;
    if (len3sq(c1) < 0.90f || len3sq(c1) > 1.10f) return 0;
    if (len3sq(c2) < 0.90f || len3sq(c2) > 1.10f) return 0;
    /* Orthogonal to ~3 degrees. */
    if (dot3(c0, c1) > 0.05f || dot3(c0, c1) < -0.05f) return 0;
    if (dot3(c0, c2) > 0.05f || dot3(c0, c2) < -0.05f) return 0;
    if (dot3(c1, c2) > 0.05f || dot3(c1, c2) < -0.05f) return 0;
    return 1;
}

/* ------------------------------------------------------- slot seqlock i/o */

static void slot_commit(int s, const poses_pose_t *np)
{
    InterlockedIncrement(&g_seq[s]);     /* -> odd, "writing" */
    POSES_BARRIER();
    g_slot[s] = *np;
    POSES_BARRIER();
    InterlockedIncrement(&g_seq[s]);     /* -> even, "stable" */
}

int poses_get(int slot, poses_pose_t *out)
{
    int tries;

    if (!out)
        return 0;
    if (slot < 0 || slot >= POSES_SLOT_COUNT) {
        memset(out, 0, sizeof(*out));
        out->device_index = POSES_INDEX_NONE;
        return 0;
    }

    for (tries = 0; tries < 32; tries++) {
        LONG a = g_seq[slot];
        if (a & 1)
            continue;                    /* a write is in flight */
        POSES_BARRIER();
        *out = g_slot[slot];
        POSES_BARRIER();
        if (g_seq[slot] == a)
            return out->valid;
    }
    /* Could not get a coherent read.  Reporting "no pose" is the only honest
     * answer; a torn matrix would be worse than none. */
    memset(out, 0, sizeof(*out));
    out->device_index = POSES_INDEX_NONE;
    return 0;
}

uint32_t poses_frame_count(void) { return (uint32_t)g_frames; }

/* ------------------------------------------------------- device identity */

static const char *slot_name(int s)
{
    return s == POSES_HMD ? "hmd" : s == POSES_HAND_LEFT ? "left" : "right";
}

int poses_rescan(void)
{
    uint32_t found[POSES_SLOT_COUNT];
    uint32_t i;
    int s, n = 0;

    found[0] = found[1] = found[2] = POSES_INDEX_NONE;
    g_since_rescan = 0;

    if (!g_sys || !g_sys->GetTrackedDeviceClass)
        return 0;

    /* Identify by CLASS first, then by ROLE -- never by index order.  Index 0
     * is the HMD by convention, but the hands land wherever the runtime
     * happens to enumerate them, and swapping a player's hands is exactly the
     * kind of bug that gets blamed on the tracking rather than on us. */
    for (i = 0; i < POSES_MAX_DEVICES; i++) {
        ETrackedDeviceClass c = g_sys->GetTrackedDeviceClass((TrackedDeviceIndex_t)i);

        if (c == ETrackedDeviceClass_TrackedDeviceClass_HMD) {
            if (found[POSES_HMD] == POSES_INDEX_NONE)
                found[POSES_HMD] = i;
        } else if (c == ETrackedDeviceClass_TrackedDeviceClass_Controller) {
            ETrackedControllerRole r =
                ETrackedControllerRole_TrackedControllerRole_Invalid;
            if (g_sys->GetControllerRoleForTrackedDeviceIndex)
                r = g_sys->GetControllerRoleForTrackedDeviceIndex((TrackedDeviceIndex_t)i);
            if (r == ETrackedControllerRole_TrackedControllerRole_LeftHand) {
                if (found[POSES_HAND_LEFT] == POSES_INDEX_NONE)
                    found[POSES_HAND_LEFT] = i;
            } else if (r == ETrackedControllerRole_TrackedControllerRole_RightHand) {
                if (found[POSES_HAND_RIGHT] == POSES_INDEX_NONE)
                    found[POSES_HAND_RIGHT] = i;
            }
            /* Role Invalid/OptOut/Treadmill/Stylus: left unassigned on
             * purpose.  The fallback below is the only guess we make. */
        }
    }

    /* Fallback: ask the runtime directly.  Some drivers report a role through
     * GetTrackedDeviceIndexForControllerRole before they report one per
     * device.  Still not index-order luck -- the runtime is telling us which
     * hand it is; we are just asking the question the other way round. */
    if (g_sys->GetTrackedDeviceIndexForControllerRole) {
        static const ETrackedControllerRole roles[2] = {
            ETrackedControllerRole_TrackedControllerRole_LeftHand,
            ETrackedControllerRole_TrackedControllerRole_RightHand
        };
        int k;
        for (k = 0; k < 2; k++) {
            int sl = (k == 0) ? POSES_HAND_LEFT : POSES_HAND_RIGHT;
            uint32_t idx;
            if (found[sl] != POSES_INDEX_NONE)
                continue;
            idx = (uint32_t)g_sys->GetTrackedDeviceIndexForControllerRole(roles[k]);
            if (idx < POSES_MAX_DEVICES)
                found[sl] = idx;
        }
    }

    if (found[POSES_HAND_LEFT] == POSES_INDEX_NONE &&
        found[POSES_HAND_RIGHT] == POSES_INDEX_NONE && !g_warned_roles) {
        g_warned_roles = 1;
        plog("no controller reported a hand role; hands stay unassigned "
             "(deliberate -- guessing by index order gets them swapped)");
    }

    for (s = 0; s < POSES_SLOT_COUNT; s++) {
        if (g_index[s] != found[s])
            plog("%s: device index %u -> %u", slot_name(s),
                 g_index[s], found[s]);
        g_index[s] = found[s];
        if (found[s] != POSES_INDEX_NONE)
            n++;
    }
    return n;
}

/* ----------------------------------------------------------- pose ingest */

/* p == NULL means "this slot has no device right now". */
static void write_slot(int s, uint32_t idx, const struct TrackedDevicePose_t *p)
{
    poses_pose_t np = g_slot[s];         /* keep last-known numbers */
    int i;

    np.device_index = idx;
    np.serial++;
    np.valid = 0;

    if (!p) {
        np.connected = 0;
        np.tracking_result = 0;
        slot_commit(s, &np);
        return;
    }

    np.connected = p->bDeviceIsConnected ? 1 : 0;
    np.tracking_result = (int32_t)p->eTrackingResult;

    if (!p->bPoseIsValid) {
        /* Normal: the controller is asleep, out of range, or switched off.
         * We keep the last good numbers and say so.  We do NOT zero them --
         * an all-zero pose reads as "standing at the tracking origin, facing
         * CoD +X", which is a plausible-looking lie. */
        slot_commit(s, &np);
        return;
    }

    if (!m34_is_rigid((const float (*)[4])p->mDeviceToAbsoluteTracking.m)) {
        if (!g_warned_basis) {
            g_warned_basis = 1;
            plog("%s: pose flagged valid but its matrix is not a rotation "
                 "(%.3f %.3f %.3f / %.3f %.3f %.3f / %.3f %.3f %.3f) -- "
                 "rejecting", slot_name(s),
                 p->mDeviceToAbsoluteTracking.m[0][0],
                 p->mDeviceToAbsoluteTracking.m[0][1],
                 p->mDeviceToAbsoluteTracking.m[0][2],
                 p->mDeviceToAbsoluteTracking.m[1][0],
                 p->mDeviceToAbsoluteTracking.m[1][1],
                 p->mDeviceToAbsoluteTracking.m[1][2],
                 p->mDeviceToAbsoluteTracking.m[2][0],
                 p->mDeviceToAbsoluteTracking.m[2][1],
                 p->mDeviceToAbsoluteTracking.m[2][2]);
        }
        slot_commit(s, &np);
        return;
    }

    memcpy(np.m34, p->mDeviceToAbsoluteTracking.m, sizeof(np.m34));
    for (i = 0; i < 3; i++) {
        np.pos_m[i]    =  np.m34[i][3];
        np.right_ovr[i]=  np.m34[i][0];
        np.up_ovr[i]   =  np.m34[i][1];
        np.fwd_ovr[i]  = -np.m34[i][2];
        np.vel_m_s[i]      = fin(p->vVelocity.v[i])        ? p->vVelocity.v[i] : 0.0f;
        np.angvel_rad_s[i] = fin(p->vAngularVelocity.v[i]) ? p->vAngularVelocity.v[i] : 0.0f;
    }
    poses_cod_from_ovr_m34((const float (*)[4])np.m34, np.cod_origin, np.cod_axis);

    np.valid = 1;
    np.valid_serial = np.serial;
    slot_commit(s, &np);
}

static void ingest(const struct TrackedDevicePose_t *poses, uint32_t count)
{
    int s;

    InterlockedIncrement(&g_frames);

    if (g_rescan_interval && ++g_since_rescan >= g_rescan_interval)
        poses_rescan();

    for (s = 0; s < POSES_SLOT_COUNT; s++) {
        uint32_t idx = g_index[s];
        if (!poses || idx == POSES_INDEX_NONE || idx >= count)
            write_slot(s, idx, NULL);
        else
            write_slot(s, idx, &poses[idx]);
    }
}

/* ================= THE RENDER PATH'S ENTRY POINT =========================
 * gameframe.c already calls WaitGetPoses once per frame and owns the frame
 * clock.  It passes the array it just filled in; we never ask the compositor
 * for anything.  This function makes no blocking call and (apart from the
 * periodic device rescan) no OpenVR call at all. */
void poses_update(const struct TrackedDevicePose_t *poses, uint32_t count)
{
    if (!poses || !count) {
        /* Still tick, so a caller that has lost its poses shows up as stale
         * rather than as frozen-but-valid. */
        ingest(NULL, 0);
        return;
    }
    ingest(poses, count);
}

/* ================= THE FALLBACK FOR EVERYONE ELSE ========================
 * GetDeviceToAbsoluteTrackingPose returns immediately and does not participate
 * in the compositor's frame pacing, so this is safe to call from a gameplay or
 * debug thread while the render path is running its own WaitGetPoses loop. */
int poses_poll(void)
{
    struct TrackedDevicePose_t p[POSES_MAX_DEVICES];

    if (!g_sys || !g_sys->GetDeviceToAbsoluteTrackingPose)
        return 0;

    memset(p, 0, sizeof(p));
    g_sys->GetDeviceToAbsoluteTrackingPose((ETrackingUniverseOrigin)g_universe,
                                           g_predict, p, POSES_MAX_DEVICES);
    ingest(p, POSES_MAX_DEVICES);
    return 1;
}

/* ------------------------------------------------------------ eye poses */

static int eye_to_head(int eye, float out[3][4])
{
    struct HmdMatrix34_t m;

    if (eye != POSES_EYE_LEFT && eye != POSES_EYE_RIGHT)
        return 0;
    if (g_eye2head[eye].have) {
        memcpy(out, g_eye2head[eye].m, sizeof(g_eye2head[eye].m));
        return 1;
    }
    if (!g_sys || !g_sys->GetEyeToHeadTransform)
        return 0;

    /* A 48-byte struct returned by value through a __stdcall FnTable.  Exp. 4
     * MEASURED that this particular return works under Proton's vrclient (its
     * RESULTS.md: "survived the 48-byte struct return"); the 8-byte
     * GetHiddenAreaMesh return is the one that is broken. */
    m = g_sys->GetEyeToHeadTransform(eye == POSES_EYE_LEFT ? EVREye_Eye_Left
                                                          : EVREye_Eye_Right);
    if (!m34_is_rigid((const float (*)[4])m.m)) {
        plog("eye %d: GetEyeToHeadTransform returned a non-rotation; "
             "eye poses disabled", eye);
        return 0;
    }
    memcpy(g_eye2head[eye].m, m.m, sizeof(g_eye2head[eye].m));
    g_eye2head[eye].have = 1;
    plog("eye %d to head: offset %.4f %.4f %.4f m", eye,
         m.m[0][3], m.m[1][3], m.m[2][3]);
    memcpy(out, g_eye2head[eye].m, sizeof(g_eye2head[eye].m));
    return 1;
}

int poses_get_eye(int eye, poses_pose_t *out)
{
    float e2h[3][4];
    int hmd_valid, i;

    if (!out)
        return 0;
    hmd_valid = poses_get(POSES_HMD, out);
    if (eye != POSES_EYE_LEFT && eye != POSES_EYE_RIGHT) {
        out->valid = 0;
        return 0;
    }
    if (!eye_to_head(eye, e2h)) {
        out->valid = 0;
        return 0;
    }

    /* eyeToTracking = hmdToTracking * eyeToHead. */
    m34_mul((const float (*)[4])out->m34, (const float (*)[4])e2h, out->m34);
    for (i = 0; i < 3; i++) {
        out->pos_m[i]     =  out->m34[i][3];
        out->right_ovr[i] =  out->m34[i][0];
        out->up_ovr[i]    =  out->m34[i][1];
        out->fwd_ovr[i]   = -out->m34[i][2];
    }
    poses_cod_from_ovr_m34((const float (*)[4])out->m34,
                           out->cod_origin, out->cod_axis);
    /* The eye's linear velocity is not the head's once the head is rotating;
     * rather than publish a subtly wrong number, publish none. */
    memset(out->vel_m_s, 0, sizeof(out->vel_m_s));

    out->valid = hmd_valid ? 1 : 0;
    return out->valid;
}

/* ----------------------------------------------------------------- config */

void poses_set_universe(int o)          { g_universe = o; }
void poses_set_prediction(float s)      { g_predict = fin(s) ? s : 0.0f; }
void poses_set_units_per_metre(float u) { if (fin(u) && u > 0.0f) g_upm = u; }
float poses_units_per_metre(void)       { return g_upm; }
void poses_set_rescan_interval(uint32_t n) { g_rescan_interval = n; }

int poses_format(int slot, char *buf, uint32_t cap)
{
    poses_pose_t p;
    int n;

    if (!buf || cap < 2)
        return 0;
    buf[0] = '\0';
    if (slot < 0 || slot >= POSES_SLOT_COUNT) {
        n = _snprintf(buf, cap - 1, "bad slot %d", slot);
        buf[cap - 1] = '\0';
        return n < 0 ? (int)strlen(buf) : n;
    }
    poses_get(slot, &p);
    if (p.device_index == POSES_INDEX_NONE)
        n = _snprintf(buf, cap - 1, "%-5s no device", slot_name(slot));
    else if (!p.valid)
        n = _snprintf(buf, cap - 1,
                      "%-5s idx=%u NOT VALID (connected=%d result=%d) "
                      "last good serial %u of %u",
                      slot_name(slot), p.device_index, p.connected,
                      (int)p.tracking_result, p.valid_serial, p.serial);
    else
        n = _snprintf(buf, cap - 1,
                      "%-5s idx=%u ovr[%.3f %.3f %.3f]m  cod org[%.1f %.1f %.1f] "
                      "fwd[%.3f %.3f %.3f] left[%.3f %.3f %.3f] up[%.3f %.3f %.3f]",
                      slot_name(slot), p.device_index,
                      p.pos_m[0], p.pos_m[1], p.pos_m[2],
                      p.cod_origin[0], p.cod_origin[1], p.cod_origin[2],
                      p.cod_axis[0][0], p.cod_axis[0][1], p.cod_axis[0][2],
                      p.cod_axis[1][0], p.cod_axis[1][1], p.cod_axis[1][2],
                      p.cod_axis[2][0], p.cod_axis[2][1], p.cod_axis[2][2]);
    buf[cap - 1] = '\0';
    return n < 0 ? (int)strlen(buf) : n;
}

/* --------------------------------------------------------------- binding */

static void after_bind(void)
{
    g_eye2head[0].have = g_eye2head[1].have = 0;
    g_warned_basis = 0;
    g_warned_roles = 0;
    plog("bound to IVRSystem_023 at %p; %d device(s) identified",
         (void *)g_sys, poses_rescan());
}

int poses_bind(void *tbl)
{
    g_sys = (struct IVRSystem_023_FnTable *)tbl;
    if (!g_sys)
        return 0;
    /* Cheapest possible sanity check that this is a table and not, say, a
     * struct we were handed by mistake: the slots we use must be non-NULL. */
    if (!g_sys->GetTrackedDeviceClass || !g_sys->GetDeviceToAbsoluteTrackingPose) {
        plog("poses_bind: table at %p has NULL slots -- not IVRSystem_023?", tbl);
        g_sys = NULL;
        return 0;
    }
    after_bind();
    return 1;
}

typedef intptr_t (__cdecl *pfn_GGI)(const char *, EVRInitError *);
typedef intptr_t (__cdecl *pfn_Init2)(EVRInitError *, EVRApplicationType, const char *);
typedef void     (__cdecl *pfn_Shutdown)(void);

/* "FnTable:IVRSystem_023", built from the constant in ivrsystem_023.h rather
 * than spelled out, so the version this module asks for can never drift away
 * from the version the FnTable declaration was generated for.  Neither Proton
 * 10.0-4b's vrclient nor xrizer knows an IVRSystem past _023, and master's _026
 * table is NOT a prefix of _023's -- get this wrong and calls land on the wrong
 * slot silently (04_live_fntable/ivrsystem_023.h). */
static intptr_t get_ivrsystem(pfn_GGI ggi, EVRInitError *err)
{
    char name[64];
    _snprintf(name, sizeof(name) - 1, "FnTable:%s", IVRSystem_023_Version);
    name[sizeof(name) - 1] = '\0';
    return ggi(name, err);
}

static HMODULE load_openvr(void)
{
    HMODULE h;
    if (g_ovr)
        return g_ovr;
    /* C:\bo1vr by absolute path, exactly as gameframe.c does: the bare name
     * would find Proton's openvr_api_DXVK.dll in the system directories, which
     * is a different DLL (README Correction).  If the render path already
     * loaded it this is only a refcount bump. */
    h = LoadLibraryA("C:\\bo1vr\\openvr_api.dll");
    if (!h) h = LoadLibraryA("openvr_api.dll");
    if (!h) plog("no openvr_api.dll (err=%lu)", GetLastError());
    g_ovr = h;
    return h;
}

int poses_attach(void)
{
    HMODULE h = load_openvr();
    pfn_GGI ggi;
    EVRInitError err = (EVRInitError)0;
    intptr_t iface;

    if (!h)
        return 0;
    ggi = (pfn_GGI)(void *)GetProcAddress(h, "VR_GetGenericInterface");
    if (!ggi) { plog("openvr_api.dll has no VR_GetGenericInterface"); return 0; }

    /* NOT VR_InitInternal2.  If nobody in this process has created the session
     * yet this simply returns NULL, which is the correct outcome: we must not
     * be the one to take the compositor. */
    iface = get_ivrsystem(ggi, &err);
    if (!iface) {
        plog("attach: no IVRSystem_023 (err=%d) -- has anyone called "
             "VR_InitInternal2 in this process yet?", (int)err);
        return 0;
    }
    return poses_bind((void *)iface);
}

int poses_init_standalone(void)
{
    HMODULE h = load_openvr();
    pfn_Init2 init2;
    pfn_GGI ggi;
    EVRInitError err = (EVRInitError)0xDEADBEEF;
    intptr_t iface;

    if (!h)
        return 0;
    init2 = (pfn_Init2)(void *)GetProcAddress(h, "VR_InitInternal2");
    ggi   = (pfn_GGI)  (void *)GetProcAddress(h, "VR_GetGenericInterface");
    if (!init2 || !ggi) { plog("openvr_api.dll lacks the entry points"); return 0; }

    /* *** TAKES THE COMPOSITOR.  See the warning in poses.h. *** */
    init2(&err, EVRApplicationType_VRApplication_Scene, "");
    if (err != EVRInitError_VRInitError_None) {
        plog("VR_InitInternal2 err=%d", (int)err);
        return 0;
    }
    g_we_inited = 1;

    iface = get_ivrsystem(ggi, &err);
    if (!iface) { plog("no IVRSystem_023 (err=%d)", (int)err); return 0; }
    return poses_bind((void *)iface);
}

int poses_ready(void) { return g_sys != NULL; }

void poses_shutdown(void)
{
    int s;
    g_sys = NULL;
    for (s = 0; s < POSES_SLOT_COUNT; s++) {
        poses_pose_t np = g_slot[s];
        np.valid = 0;
        np.connected = 0;
        np.serial++;
        slot_commit(s, &np);
    }
    if (g_we_inited && g_ovr) {
        pfn_Shutdown sd = (pfn_Shutdown)(void *)GetProcAddress(g_ovr, "VR_ShutdownInternal");
        if (sd) sd();
        g_we_inited = 0;
    }
    g_ovr = NULL;      /* deliberately not FreeLibrary: the render path may
                        * still be using the same module. */
}

/* ===================================================== offline math check
 *
 * Pure arithmetic, no OpenVR, no compositor.  Compiled into every build so the
 * self-test .asi can run it before it decides whether it is even allowed to
 * talk to a runtime.  Returns 0 on success, or the number of the first failed
 * case, and writes an explanation into `why`. */

static int close3(const float a[3], const float b[3], float eps)
{
    int i;
    for (i = 0; i < 3; i++) {
        float d = a[i] - b[i];
        if (d < 0) d = -d;
        if (!(d <= eps)) return 0;
    }
    return 1;
}

int poses_selfcheck(char *why, int cap)
{
    float m[3][4], org[3], ax[3][3], v[3], g[3];
    float saved = g_upm;
    int rc = 0;

    if (why && cap > 0) why[0] = '\0';
    g_upm = POSES_UNITS_PER_METRE_DEFAULT;

#define FAIL(n, msg) do { if (why && cap > 0) { _snprintf(why, cap - 1, "%s", msg); \
                          why[cap - 1] = '\0'; } rc = (n); goto done; } while (0)

    /* 1. Identity device pose.  OpenVR identity means right=+X, up=+Y,
     *    forward=-Z; mapped into CoD that is forward=+X, left=+Y, up=+Z, i.e.
     *    the CoD identity basis. */
    memset(m, 0, sizeof(m));
    m[0][0] = m[1][1] = m[2][2] = 1.0f;
    poses_cod_from_ovr_m34((const float (*)[4])m, org, ax);
    {
        static const float e0[3] = {1,0,0}, e1[3] = {0,1,0}, e2[3] = {0,0,1};
        static const float z[3] = {0,0,0};
        if (!close3(org, z, 1e-6f))  FAIL(1, "identity: origin not zero");
        if (!close3(ax[0], e0, 1e-6f)) FAIL(1, "identity: forward != CoD +X");
        if (!close3(ax[1], e1, 1e-6f)) FAIL(1, "identity: left != CoD +Y");
        if (!close3(ax[2], e2, 1e-6f)) FAIL(1, "identity: up != CoD +Z");
    }

    /* 2. Translation only.  OpenVR (1,2,3) m -> CoD (-3,-1,2) * 39.37 in. */
    m[0][3] = 1.0f; m[1][3] = 2.0f; m[2][3] = 3.0f;
    poses_cod_from_ovr_m34((const float (*)[4])m, org, ax);
    {
        float e[3];
        e[0] = -3.0f * POSES_UNITS_PER_METRE_DEFAULT;
        e[1] = -1.0f * POSES_UNITS_PER_METRE_DEFAULT;
        e[2] =  2.0f * POSES_UNITS_PER_METRE_DEFAULT;
        if (!close3(org, e, 1e-2f)) FAIL(2, "translation: wrong axis order or scale");
    }

    /* 3. Yaw +90 degrees about OpenVR +Y, which turns the device to its LEFT.
     *    R_y(90) = [[0,0,1],[0,1,0],[-1,0,0]] in row-major.  The device's
     *    forward (-Z) becomes OpenVR -X, i.e. left; in CoD that is +Y. */
    memset(m, 0, sizeof(m));
    m[0][2] =  1.0f;
    m[1][1] =  1.0f;
    m[2][0] = -1.0f;
    poses_cod_from_ovr_m34((const float (*)[4])m, org, ax);
    {
        static const float f[3] = {0,1,0}, l[3] = {-1,0,0}, u[3] = {0,0,1};
        if (!close3(ax[0], f, 1e-6f)) FAIL(3, "yaw90: forward should be CoD +Y (left)");
        if (!close3(ax[1], l, 1e-6f)) FAIL(3, "yaw90: left should be CoD -X (back)");
        if (!close3(ax[2], u, 1e-6f)) FAIL(3, "yaw90: up should stay CoD +Z");
    }

    /* 4. The basis must stay right-handed: forward x left == up, which is what
     *    MatrixForViewer assumes when it negates axis[1] to get view right. */
    {
        float c[3];
        c[0] = ax[0][1] * ax[1][2] - ax[0][2] * ax[1][1];
        c[1] = ax[0][2] * ax[1][0] - ax[0][0] * ax[1][2];
        c[2] = ax[0][0] * ax[1][1] - ax[0][1] * ax[1][0];
        if (!close3(c, ax[2], 1e-5f)) FAIL(4, "handedness: forward x left != up");
    }

    /* 5. Round trip through the project's own CoD -> Godot mapping,
     *    godot = (-Y, Z, -X) * 0.0254.  Godot's basis equals OpenVR's, so
     *    applying it to our CoD output must give back the metres we started
     *    with.  This is the cross-check quoted in poses.h. */
    v[0] = 0.37f; v[1] = -1.21f; v[2] = 2.04f;
    poses_cod_pos_from_ovr(v, org);
    g[0] = -org[1] * 0.0254f;
    g[1] =  org[2] * 0.0254f;
    g[2] = -org[0] * 0.0254f;
    if (!close3(g, v, 1e-4f)) FAIL(5, "godot round trip: (-Y,Z,-X)*0.0254 did not invert");

    /* 6. A non-rotation must be rejected, and a rotation accepted. */
    memset(m, 0, sizeof(m));
    m[0][0] = m[1][1] = m[2][2] = 2.0f;
    if (m34_is_rigid((const float (*)[4])m)) FAIL(6, "m34_is_rigid accepted a 2x scale");
    m[0][0] = m[1][1] = m[2][2] = 1.0f;
    if (!m34_is_rigid((const float (*)[4])m)) FAIL(6, "m34_is_rigid rejected the identity");

    /* 7. An invalid pose must not overwrite the last good one, and must not
     *    read as "at the origin". */
    {
        struct TrackedDevicePose_t p;
        poses_pose_t got;
        memset(&p, 0, sizeof(p));
        p.mDeviceToAbsoluteTracking.m[0][0] = 1.0f;
        p.mDeviceToAbsoluteTracking.m[1][1] = 1.0f;
        p.mDeviceToAbsoluteTracking.m[2][2] = 1.0f;
        p.mDeviceToAbsoluteTracking.m[0][3] = 5.0f;
        p.bPoseIsValid = 1;
        p.bDeviceIsConnected = 1;
        write_slot(POSES_HMD, 0, &p);
        if (!poses_get(POSES_HMD, &got) || got.pos_m[0] != 5.0f)
            FAIL(7, "valid pose did not land in the slot");

        p.bPoseIsValid = 0;
        p.mDeviceToAbsoluteTracking.m[0][3] = 0.0f;
        write_slot(POSES_HMD, 0, &p);
        if (poses_get(POSES_HMD, &got))
            FAIL(7, "invalid pose still reported valid");
        if (got.pos_m[0] != 5.0f)
            FAIL(7, "invalid pose clobbered the last known good position");

        write_slot(POSES_HMD, POSES_INDEX_NONE, NULL);
        if (poses_get(POSES_HMD, &got) || got.device_index != POSES_INDEX_NONE)
            FAIL(7, "absent device did not report absent");
        if (got.pos_m[0] != 5.0f)
            FAIL(7, "absent device clobbered the last known good position");
    }

#undef FAIL
done:
    g_upm = saved;
    /* Leave no test residue in the live slots. */
    memset(&g_slot[POSES_HMD], 0, sizeof(g_slot[POSES_HMD]));
    g_slot[POSES_HMD].device_index = POSES_INDEX_NONE;
    return rc;
}

/* =================================================== §selftest -- the .asi
 *
 * Built only with -DPOSES_SELFTEST.  It exists so another engineer can drop one
 * file into C:\bo1vr and see what this module makes of the runtime.
 *
 * IT IS GATED BEHIND MARKER FILES ON PURPOSE.  An .asi in C:\bo1vr is loaded by
 * every BlackOps.exe launch, and a plugin that quietly called VR_InitInternal2
 * would seize the compositor from whatever else is using it.  So:
 *
 *   C:\bo1vr\poses_selftest.on             -> attach to an existing session and
 *                                             report.  Never calls
 *                                             VR_InitInternal2.
 *   C:\bo1vr\poses_selftest_standalone.on  -> additionally allowed to CREATE a
 *                                             session if none exists.  THIS
 *                                             TAKES THE COMPOSITOR.
 *
 * With neither file present the plugin runs the offline maths check, writes one
 * line to the log, and does nothing else.
 */
#ifdef POSES_SELFTEST

static int marker(const char *name)
{
    return GetFileAttributesA(name) != INVALID_FILE_ATTRIBUTES;
}

static DWORD WINAPI selftest_thread(LPVOID arg)
{
    char line[512];
    poses_pose_t eye;
    int rc, i, s;
    int allow_attach, allow_standalone;

    (void)arg;
    poses_set_logging(1);
    plog("selftest: pid=%lu", GetCurrentProcessId());

    rc = poses_selfcheck(line, sizeof(line));
    if (rc)
        plog("selftest: offline maths check FAILED at case %d: %s", rc, line);
    else
        plog("selftest: offline maths check PASSED (no OpenVR involved)");

    allow_attach     = marker("C:\\bo1vr\\poses_selftest.on");
    allow_standalone = marker("C:\\bo1vr\\poses_selftest_standalone.on");
    if (!allow_attach && !allow_standalone) {
        plog("selftest: no marker file in C:\\bo1vr, so NOT touching OpenVR. "
             "Create poses_selftest.on to attach to an existing session, or "
             "poses_selftest_standalone.on to create one (that takes the "
             "compositor).");
        goto done;
    }

    if (!poses_attach()) {
        if (!allow_standalone) {
            plog("selftest: nothing to attach to, and standalone is not "
                 "permitted -- stopping here rather than taking the compositor");
            goto done;
        }
        plog("selftest: creating our own session (marker says that is allowed)");
        if (!poses_init_standalone()) {
            plog("selftest: no session; stopping");
            goto done;
        }
    }

    /* poses_poll(), not WaitGetPoses: this thread must not touch the frame
     * clock even when it does own the session. */
    for (i = 0; i < 200; i++) {
        poses_poll();
        if (i < 3 || (i % 50) == 0) {
            for (s = 0; s < POSES_SLOT_COUNT; s++) {
                poses_format(s, line, sizeof(line));
                plog("  [%3d] %s", i, line);
            }
            if (poses_get_eye(POSES_EYE_LEFT, &eye))
                plog("  [%3d] left eye at cod org[%.1f %.1f %.1f]", i,
                     eye.cod_origin[0], eye.cod_origin[1], eye.cod_origin[2]);
        }
        Sleep(16);
    }
    plog("selftest: %u updates", poses_frame_count());
    poses_shutdown();

done:
    {
        HANDLE ev = CreateEventA(NULL, TRUE, FALSE, "bo1vr_exp12_done");
        if (ev) { SetEvent(ev); CloseHandle(ev); }
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    HANDLE t;
    (void)reserved;
    if (reason != DLL_PROCESS_ATTACH)
        return TRUE;
    DisableThreadLibraryCalls(inst);
    /* Never do this work under the loader lock. */
    t = CreateThread(NULL, 0, selftest_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
    return TRUE;
}
#endif /* POSES_SELFTEST */

/* ============================================== the offline check as an exe */
#ifdef POSES_MATHCHECK
int main(void)
{
    char why[256];
    int rc = poses_selfcheck(why, sizeof(why));
    if (rc) {
        printf("poses_mathcheck: FAIL case %d: %s\n", rc, why);
        return rc;
    }
    printf("poses_mathcheck: PASS (7 cases; no OpenVR was loaded)\n");
    return 0;
}
#endif
