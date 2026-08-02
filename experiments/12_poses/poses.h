/* poses.h -- HMD and controller poses, in a form Black Ops can use.  (BAC-283)
 *
 * WHAT THIS IS
 * ------------
 * A small, self-contained C module that turns OpenVR tracked-device poses into
 * (a) the raw OpenVR quantities and (b) a CoD-convention origin + axis[3][3]
 * ready to be written into a refdef by the camera hook (docs/camera-hook-plan.md).
 *
 * It deliberately does NOT own the frame clock and does NOT create the OpenVR
 * session by default.  In the shipping mod the render path
 * (experiments/11_gameframe/gameframe.c) calls VR_InitInternal2 and
 * IVRCompositor::WaitGetPoses exactly once per frame; a second caller of
 * WaitGetPoses would fight it for the compositor's frame pacing.  So:
 *
 *   THE RENDER PATH FEEDS US        -> poses_update(renderPoses, count)
 *   EVERYONE ELSE ASKS THE RUNTIME  -> poses_poll()
 *
 * Those two are the whole input surface.  Pick one per process:
 *
 *   * poses_update() is the preferred path.  gameframe.c already has the array
 *     WaitGetPoses filled in; handing us the same array costs a memcpy-sized
 *     amount of work, introduces no new synchronisation with the runtime, and
 *     guarantees that the pose the camera hook uses is bit-identical to the one
 *     the compositor will reproject against.
 *
 *   * poses_poll() calls IVRSystem::GetDeviceToAbsoluteTrackingPose, which does
 *     NOT block and does NOT touch the compositor's frame clock.  Use it from
 *     gameplay code, a debug thread, or any process where nobody is calling
 *     WaitGetPoses.  Calling it *as well as* poses_update() is allowed but
 *     pointless: whichever ran last wins.
 *
 * NEVER call IVRCompositor::WaitGetPoses from this module.  It is not here, and
 * adding it would break the render path.
 *
 * VALIDITY IS A NORMAL STATE, NOT AN ERROR
 * ----------------------------------------
 * A controller that is switched off, asleep, or occluded is ordinary.  Every
 * accessor returns a validity flag; when `valid` is 0 the numeric fields hold
 * the LAST KNOWN GOOD pose (or all zeros if the slot has never been valid) and
 * must not be treated as live.  Nothing in this module ever hands back a
 * freshly-zeroed matrix that would read as "at the world origin, facing +X".
 *
 * THREADING
 * ---------
 * The writer is whatever thread calls poses_update()/poses_poll() -- under the
 * mod that is the D3D9 present hook.  Readers may be on the game thread.  Each
 * slot is protected by a seqlock, so poses_get() either returns a coherent
 * snapshot or reports failure; it never returns a half-updated pose.  There is
 * no blocking and no allocation anywhere in this file.
 *
 * NO C++, NO SEH.  32-bit mingw has neither usable exceptions across MSVC
 * frames (README Decision 6) nor __try/__except.  Every pointer is checked.
 */
#ifndef BO1VR_POSES_H
#define BO1VR_POSES_H

#include <stdint.h>

/* openvr_capi.h's type, forward-declared so a caller that only wants
 * poses_poll()/poses_get() needs no OpenVR headers at all.  Declaring the tag
 * again after openvr_capi.h has been included is legal and harmless. */
struct TrackedDevicePose_t;

/* ------------------------------------------------------------------ slots */

enum {
    POSES_HMD        = 0,
    POSES_HAND_LEFT  = 1,
    POSES_HAND_RIGHT = 2,
    POSES_SLOT_COUNT = 3
};

enum {
    POSES_EYE_LEFT  = 0,
    POSES_EYE_RIGHT = 1
};

/* OpenVR's k_unMaxTrackedDeviceCount. */
#define POSES_MAX_DEVICES   64u
/* OpenVR's k_unTrackedDeviceIndexInvalid. */
#define POSES_INDEX_NONE    0xFFFFFFFFu

/* 1 m = 39.3700787... inches.  See "COORDINATES" below for why inches. */
#define POSES_UNITS_PER_METRE_DEFAULT 39.3700787401575f

/* --------------------------------------------------------------- the pose */

typedef struct poses_pose_s {
    /* --- status ------------------------------------------------------- */
    int      valid;            /* 1 = everything below is a live pose        */
    int      connected;        /* device is present but maybe not tracking   */
    int32_t  tracking_result;  /* raw ETrackingResult (200 = Running_OK)     */
    uint32_t device_index;     /* OpenVR index, or POSES_INDEX_NONE          */
    uint32_t serial;           /* bumped on every write to this slot         */
    uint32_t valid_serial;     /* `serial` of the last VALID write           */

    /* --- raw OpenVR, tracking space, metres ---------------------------- */
    /* mDeviceToAbsoluteTracking exactly as the runtime gave it: row-major
     * 3x4, columns 0..2 are the device's X/Y/Z axes in tracking space and
     * column 3 is its position.  Kept so callers can do their own maths
     * without re-deriving it from the vectors below. */
    float m34[3][4];
    float pos_m[3];            /* = column 3                                 */
    float right_ovr[3];        /* = column 0  (device +X)                    */
    float up_ovr[3];           /* = column 1  (device +Y)                    */
    float fwd_ovr[3];          /* = -column 2 (device -Z is "forward")       */
    float vel_m_s[3];          /* vVelocity, tracking space                  */
    float angvel_rad_s[3];     /* vAngularVelocity, tracking space           */

    /* --- CoD convention, game units ------------------------------------ */
    /* Origin relative to the tracking-space origin; the caller adds it to the
     * player's world position.  Axis rows are (forward, LEFT, up) -- note
     * left, not right -- matching refdef_t.viewaxis (camera-hook-plan §2.1). */
    float cod_origin[3];
    float cod_axis[3][3];
} poses_pose_t;

/* ================================================================ lifecycle
 *
 * Three ways in.  Pick the least invasive one that works.
 */

/* (1) Preferred inside the mod.  Adopt an IVRSystem_023 FnTable the caller
 *     already holds -- gameframe.c's g_sys.  We take no ownership, create no
 *     session, and shut nothing down.  `tbl` must really be an
 *     IVRSystem_023 FnTable (see 04_live_fntable/ivrsystem_023.h for why the
 *     version number matters: master's _026 table is NOT a prefix of _023's).
 *     Passing NULL unbinds.  Returns 1 on success. */
int  poses_bind(void *ivrsystem_023_fntable);

/* (2) For a plugin that runs alongside a component which has ALREADY called
 *     VR_InitInternal2 in this process.  Loads C:\bo1vr\openvr_api.dll (it will
 *     already be mapped, so this is a refcount bump) and asks it for
 *     FnTable:IVRSystem_023.  Does NOT call VR_InitInternal2, so it cannot
 *     steal or duplicate the scene session.  Returns 1 on success. */
int  poses_attach(void);

/* (3) Standalone.  LoadLibrary + VR_InitInternal2(VRApplication_Scene) +
 *     GetGenericInterface.
 *
 *     *** THIS TAKES THE COMPOSITOR. ***  Only one OpenVR scene application can
 *     hold it at a time, so calling this inside a process where gameframe.c is
 *     live, or while another VR app is running, will break that app.  Use it
 *     only from a dedicated test host.  Returns 1 on success. */
int  poses_init_standalone(void);

/* 1 once any of the above has succeeded. */
int  poses_ready(void);

/* Drops the binding.  Calls VR_ShutdownInternal ONLY if we were the ones who
 * called VR_InitInternal2 (i.e. only after poses_init_standalone). */
void poses_shutdown(void);

/* ==================================================================== input */

/* THE RENDER PATH'S ENTRY POINT.  Hand us the array WaitGetPoses just filled
 * in; `count` is the number of entries it wrote (64 in gameframe.c).  Indexed
 * by OpenVR device index, as WaitGetPoses returns it.
 *
 * Pass the RENDER pose array for anything that will be drawn this frame -- it
 * is predicted to photon time and is what the compositor will reproject
 * against.  Pass the GAME pose array instead if you are driving simulation.
 * Do not interleave the two: whichever you pass last is what poses_get()
 * returns.
 *
 * Safe to call with poses==NULL or count==0 (it becomes a no-op) and safe to
 * call before any binding exists -- device identification then falls back to
 * the last known slot assignment, or to nothing.  Cheap: no OpenVR calls at
 * all except the occasional periodic rescan. */
void poses_update(const struct TrackedDevicePose_t *poses, uint32_t count);

/* THE FALLBACK FOR EVERYONE ELSE.  Calls
 * IVRSystem::GetDeviceToAbsoluteTrackingPose(origin, prediction, ...), which
 * returns immediately and does not touch the compositor's frame clock, then
 * feeds the result through the same path as poses_update().
 *
 * Returns 1 if the runtime was asked, 0 if there is no binding.  A 1 does NOT
 * mean any device was tracking -- check the per-slot validity for that. */
int  poses_poll(void);

/* ================================================================== output */

/* Coherent snapshot of a slot.  `out` is always fully written when the call
 * returns 1 or 0-with-a-binding; the return value is out->valid.  Returns 0
 * (and zeroes *out) for a bad slot, a NULL out, or a read that could not be
 * made coherent. */
int  poses_get(int slot, poses_pose_t *out);

/* The HMD pose composed with IVRSystem::GetEyeToHeadTransform(eye), i.e. where
 * that eye actually is.  This is what the camera hook wants for its per-eye
 * refdef (camera-hook-plan §3.2 step 1).  The eye-to-head transform is fetched
 * once per eye and cached -- it is a constant of the headset.
 *
 * Returns out->valid.  Invalid whenever the HMD pose is invalid or the
 * eye-to-head transform could not be obtained. */
int  poses_get_eye(int eye, poses_pose_t *out);

/* Number of times poses_update()/poses_poll() has run.  Handy for "has the
 * pose source stalled?" checks. */
uint32_t poses_frame_count(void);

/* One line of human-readable status for a slot, for logs.  Always NUL
 * terminates; returns the number of characters written. */
int  poses_format(int slot, char *buf, uint32_t cap);

/* ================================================================== config */

/* Tracking universe used by poses_poll() (0 = seated, 1 = standing,
 * 2 = raw).  Default: standing.  Does not affect poses_update(), whose origin
 * is whatever the render path asked WaitGetPoses for -- that is the
 * compositor's SetTrackingSpace setting, not ours. */
void poses_set_universe(int etracking_universe_origin);

/* Seconds-to-photons prediction used by poses_poll().  Default 0.0f, i.e.
 * "now".  Ignored by poses_update(). */
void poses_set_prediction(float seconds);

/* Game units per metre.  Default POSES_UNITS_PER_METRE_DEFAULT (inches).  Also
 * the VR world-scale knob: a larger value makes the world feel smaller. */
void poses_set_units_per_metre(float u);
float poses_units_per_metre(void);

/* How many updates between device rescans.  0 disables periodic rescanning; a
 * rescan still happens on bind and whenever a slot's device stops matching its
 * expected class/role.  Default 90. */
void poses_set_rescan_interval(uint32_t updates);

/* Re-run device identification now (class via GetTrackedDeviceClass, hand via
 * GetControllerRoleForTrackedDeviceIndex).  Returns the number of slots that
 * ended up with a device.  Requires a binding; returns 0 without one. */
int  poses_rescan(void);

/* Offline consistency check of everything in this file that does not need a
 * runtime: the coordinate transform against hand-worked expectations, the
 * handedness identity, the round trip through the project's CoD->Godot
 * mapping, the rotation validator, and the "an invalid pose must not clobber
 * or fabricate a pose" rule.  Loads nothing and connects to nothing.
 *
 * Returns 0 on success or the number of the first failing case, with an
 * explanation in `why`.  Not thread-safe and it scribbles on the HMD slot, so
 * call it before the module goes live. */
int  poses_selfcheck(char *why, int cap);

/* Append this module's diagnostics to %TEMP%\bo1vr_poses.log.  Off by default,
 * because under a Steam launch there is no stderr (Exp. 9) and an always-on
 * per-frame log would be the only thing in it. */
void poses_set_logging(int on);

/* ============================================================= COORDINATES
 *
 * The transform below is DERIVED, not measured -- nothing here has been run
 * against a headset.  The derivation is from two conventions the project has
 * already committed to, so what is ASSUMED is those conventions, not the
 * algebra.
 *
 * OpenVR tracking space (Valve's documented convention, right-handed):
 *      +X right, +Y up, -Z forward, metres.
 * OpenVR device space, same for HMD and controllers:
 *      +X right, +Y up, -Z out the front of the device.
 *
 * CoD/Quake world space (MEASURED for the axis ORDER, by the disassembly in
 * docs/camera-hook-plan.md §2.1/§2.3: refdef+0x34 is a row-major 3x3 whose rows
 * are forward, LEFT, up, and MatrixForViewer negates row 1 to get view-space
 * right).  Right-handed: forward x left = up.
 *      axis[0] forward, axis[1] left, axis[2] up, units = inches (ASSUMED,
 *      camera-hook-plan §5.4 -- the CoD/Quake lineage says inches but nobody
 *      has yet logged vieworg over a known distance to confirm it).
 *
 * Cross-check against the project's own CoD -> Godot mapping,
 * `godot = (-Y, Z, -X) * 0.0254`.  Godot's basis is the same as OpenVR's
 * (+X right, +Y up, -Z forward, metres), so with cod = (fwd, left, up):
 *      godot.x =  -cod.y = -left  = right    consistent
 *      godot.y =   cod.z =  up               consistent
 *      godot.z =  -cod.x = -fwd   = back     consistent
 *      scale   =  0.0254 = inches -> metres  consistent
 * That mapping is therefore exactly the inverse of the one below, which is the
 * strongest confirmation available without running anything.
 *
 * Inverting it, for an OpenVR vector v = (x, y, z) in metres:
 *
 *      cod.forward =  -v.z        (OpenVR forward is -Z)
 *      cod.left    =  -v.x        (OpenVR +X is right, so left is -X)
 *      cod.up      =   v.y
 *      then x 39.3700787 for a POSITION; directions are not scaled.
 *
 * As a matrix that is a pure rotation (det = +1), so it maps a right-handed
 * orthonormal device basis to a right-handed orthonormal CoD basis and
 * preserves cross products: cod_axis[0] x cod_axis[1] == cod_axis[2] still
 * holds, which is what the engine's view matrix assumes.
 *
 * NO YAW REFERENCE IS APPLIED.  cod_origin/cod_axis are expressed relative to
 * the OpenVR tracking origin with tracking +Z-back mapped onto CoD +X-forward.
 * Aligning "forward in the room" with "forward in the game" -- recentre, snap
 * turn, and adding the player's world position -- is the caller's job; this
 * module has no opinion about where the player is standing.
 *
 * GRIP vs AIM.  A controller's pose is its GRIP pose: the origin sits in the
 * palm and -Z runs along the handle, which on every modern controller points
 * some tens of degrees away from where the user thinks they are aiming.
 * Turning a grip pose into a muzzle needs a per-controller offset and belongs
 * in the weapon code, not here.  The helpers below exist so that code can do
 * it in whichever space it prefers.
 */

/* Direction: OpenVR -> CoD axes, no scaling.  in and out may alias. */
void poses_cod_dir_from_ovr(const float v[3], float out[3]);

/* Position: OpenVR metres -> CoD units, axes swapped and scaled by
 * poses_units_per_metre().  in and out may alias. */
void poses_cod_pos_from_ovr(const float v_m[3], float out[3]);

/* A whole OpenVR 3x4 device-to-tracking matrix -> CoD origin + axis[3][3].
 * Either output pointer may be NULL. */
void poses_cod_from_ovr_m34(const float m34[3][4], float out_origin[3],
                            float out_axis[3][3]);

#endif /* BO1VR_POSES_H */
