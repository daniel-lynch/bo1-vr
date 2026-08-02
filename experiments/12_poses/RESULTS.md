# Experiment 12 — controller and HMD poses (BAC-283)

A self-contained module that turns OpenVR tracked-device poses into something
the Black Ops camera hook can write into a `refdef`.

**Nothing in this experiment has been run against a headset, a compositor, or the
game.** Another workstream holds Monado and BlackOps.exe, and only one OpenVR
scene application can hold the compositor at a time. So the whole of §3 below is
ASSUMED, and says so. What *was* run is in §2, and it is deliberately the part
that needs no runtime at all.

---

## 1. What is here

| File | What it is |
|---|---|
| `poses.h` | The API. Commented so a caller needs nothing else, including the full coordinate derivation. |
| `poses.c` | The implementation. One source file, three builds. |
| `Makefile` | `all` / `verify` / `check` / `install` / `clean`. |

Three artefacts from `poses.c`:

| Artefact | Built with | Purpose |
|---|---|---|
| `out/poses.o`, `out/libposes.a` | (nothing) | the module `gameframe.c` and the camera hook link against |
| `out/poses_selftest.asi` | `-DPOSES_SELFTEST` | plugin that reports what it can see. **Inert unless a marker file exists** — see §4. |
| `out/poses_mathcheck.exe` | `-DPOSES_MATHCHECK` | the coordinate maths, offline. Never loads `openvr_api.dll`. |

### API shape

```c
/* in */                          /* who calls it */
int  poses_bind(void *ivrsystem_023_fntable);   /* gameframe.c, with its g_sys      */
int  poses_attach(void);                        /* a plugin, session already exists */
int  poses_init_standalone(void);               /* a test host — TAKES THE COMPOSITOR */

void poses_update(const struct TrackedDevicePose_t *poses, uint32_t count);
int  poses_poll(void);

/* out */
int  poses_get(int slot, poses_pose_t *out);    /* POSES_HMD / _HAND_LEFT / _HAND_RIGHT */
int  poses_get_eye(int eye, poses_pose_t *out); /* HMD pose ∘ GetEyeToHeadTransform     */
int  poses_format(int slot, char *buf, uint32_t cap);
uint32_t poses_frame_count(void);

/* config */
poses_set_universe / _prediction / _units_per_metre / _rescan_interval / _logging
poses_rescan(); poses_ready(); poses_shutdown(); poses_selfcheck(why, cap);

/* pure helpers, usable without any of the above */
poses_cod_dir_from_ovr / poses_cod_pos_from_ovr / poses_cod_from_ovr_m34
```

`poses_pose_t` carries, per slot: `valid`, `connected`, raw `ETrackingResult`,
`device_index`, a write `serial` and the `serial` of the last valid write; the
raw OpenVR `m34[3][4]`, position in metres, the OpenVR right/up/forward vectors,
linear and angular velocity; and the CoD-convention `cod_origin[3]` (game units)
plus `cod_axis[3][3]` (rows: forward, **left**, up).

### The `poses_update` / `poses_poll` split

`gameframe.c` calls `IVRCompositor::WaitGetPoses` once per frame and that call
*is* the frame clock. A second caller would fight it, so **`poses.c` contains no
call to `WaitGetPoses` and the `verify` target fails the build if `poses.o` ever
gains a reference to it.**

* `poses_update(poses, count)` — the render path hands over the array it already
  has. No blocking, no new synchronisation, and the pose the camera hook uses is
  bit-identical to the one the compositor will reproject against. Pass the
  **render** array for anything drawn this frame; pass the **game** array if you
  are driving simulation. This is the preferred path.
* `poses_poll()` — `IVRSystem::GetDeviceToAbsoluteTrackingPose`, which returns
  immediately and does not participate in the compositor's frame pacing. For
  gameplay code, a debug thread, or any process where nobody calls
  `WaitGetPoses`. Both may be used in one process; last writer wins.

---

## 2. PROVEN

Everything in this section was executed on this machine, just now.

### 2.1 It builds, cleanly, with no libgcc runtime dependency — MEASURED

`make` from a clean tree: zero compiler warnings at `-Wall -Wextra`, and

```
out/poses_selftest.asi:   DLL Name: KERNEL32.dll   DLL Name: msvcrt.dll
out/poses_mathcheck.exe:  DLL Name: KERNEL32.dll   DLL Name: msvcrt.dll
OK: no libgcc runtime dependency
OK: no WaitGetPoses reference in poses.o
```

No `libgcc_s_dw2-1.dll`, no `libwinpthread`, no `msvcp`, and no import of
`openvr_api.dll` — the module resolves OpenVR entirely at run time (by
`LoadLibraryA("C:\\bo1vr\\openvr_api.dll")`, absolute, exactly as `gameframe.c`
does) or not at all when the host binds its own FnTable via `poses_bind`. No
`libm` either: the module never calls `sqrtf`, only compares squared lengths.

`poses.o` and `libposes.a` build as an ordinary object/archive, so linking the
module into another `.asi` is `$(CC) ... other.c out/poses.o`.

### 2.2 The coordinate transform is arithmetically correct — MEASURED

`make check` runs `out/poses_mathcheck.exe` under wine. That program's only
imports are KERNEL32 and msvcrt; it calls `poses_selfcheck()` and nothing else,
so it cannot reach a runtime or a compositor.

```
poses_mathcheck: PASS (7 cases; no OpenVR was loaded)
```

The seven cases:

1. An identity OpenVR pose maps to the CoD identity basis (forward `+X`, left
   `+Y`, up `+Z`) at the origin.
2. An OpenVR translation of (1, 2, 3) m maps to CoD (−3, −1, 2) × 39.3700787.
3. A +90° yaw about OpenVR `+Y` turns the device to its left: forward becomes
   CoD `+Y`, left becomes CoD `−X`, up is unchanged.
4. The output basis stays right-handed — `axis[0] × axis[1] == axis[2]`, which
   is what `MatrixForViewer` assumes when it negates `axis[1]` to get view-space
   right (camera-hook-plan §2.3).
5. **Round trip through the project's own CoD→Godot mapping.** Applying
   `godot = (−Y, Z, −X) × 0.0254` to our CoD output returns the OpenVR metres we
   started with, to 1e−4. This is the cross-check described in §3.1.
6. The rigid-transform validator rejects a 2× scale and accepts the identity.
7. An invalid pose neither reports valid nor clobbers the last known good
   position, and an absent device reports `POSES_INDEX_NONE` — the "controller
   that is off must not read as aiming at the world origin" rule.

### 2.3 That self-check is load-bearing, not decorative — MEASURED

A test that passes no matter what is worse than no test. Four single-token
mutations of the transform were compiled and run in a scratch copy (the
deliverables were not modified):

| Mutation | Result |
|---|---|
| `out[0] = -z` → `z` | FAIL case 1: identity: forward != CoD +X |
| `out[1] = -x` → `x` | FAIL case 1: identity: left != CoD +Y |
| `out[2] = y` → `-y` | FAIL case 1: identity: up != CoD +Z |
| drop the metres→units scale | FAIL case 2: wrong axis order or scale |

Every sign and the scale factor is pinned by the check.

### 2.4 What 2.2 does *not* prove

It proves the code implements the transform in §3.1. It does **not** prove that
§3.1 is the right transform for this game — that needs a headset, a running
BlackOps.exe, and the camera hook. See §5.

---

## 3. The OpenVR → CoD transform

### 3.1 Derivation

Two conventions, then algebra.

**OpenVR** (Valve's documented convention; ASSUMED-because-documented, not
measured here): tracking space and device space are both right-handed with
**+X right, +Y up, −Z forward**, in **metres**. A `TrackedDevicePose_t`'s
`mDeviceToAbsoluteTracking` is row-major 3×4: columns 0–2 are the device's X, Y,
Z axes expressed in tracking space, column 3 is its position.

**Black Ops**: `refdef+0x34` is a row-major 3×3 whose rows are **forward, left,
up** — this is **MEASURED**, by the disassembly in `docs/camera-hook-plan.md`
§2.1/§2.3 (`R_SetViewParms` copies rows to `viewParms+0x110/+0x11C/+0x128`;
`MatrixForViewer` negates row 1 to get view-space right; the viewmodel-placement
function at `0x797BE0` walks the same layout independently). The system is
right-handed: forward × left = up. Units are **inches** — **ASSUMED**
(camera-hook-plan §5.4: CoD/Quake lineage says so, but nobody has yet logged
`vieworg` over a known distance).

For an OpenVR vector `v = (x, y, z)`:

```
cod.forward = −v.z      (OpenVR forward is −Z)
cod.left    = −v.x      (OpenVR +X is right, so left is −X)
cod.up      =  v.y
```

and for a **position**, × 39.3700787 (metres → inches). Directions are not
scaled. As a matrix that is a pure rotation (det = +1), so it maps a
right-handed orthonormal device basis to a right-handed orthonormal CoD basis
and preserves cross products.

Applied to a device matrix `M`:

```
cod_axis[0] (forward) = cod(−column 2)     column 2 is device +Z, i.e. backwards
cod_axis[1] (left)    = cod(−column 0)     column 0 is device +X, i.e. right
cod_axis[2] (up)      = cod( column 1)
cod_origin            = cod( column 3) × units_per_metre
```

### 3.2 Cross-check against the project's Godot mapping

The rest of the project uses `godot = (−Y, Z, −X) × 0.0254` for CoD → Godot.
Godot's basis is the same as OpenVR's (+X right, +Y up, −Z forward, metres), so
with `cod = (fwd, left, up)`:

| | | |
|---|---|---|
| `godot.x = −cod.y = −left = right` | ✓ Godot X is right | |
| `godot.y =  cod.z =  up` | ✓ Godot Y is up | |
| `godot.z = −cod.x = −fwd = back` | ✓ Godot forward is −Z | |
| `× 0.0254` | ✓ inches → metres | |

That mapping is *exactly the inverse* of §3.1, which is the strongest
confirmation available without running anything, and case 5 of the self-check
verifies the inversion numerically. **Both directions rest on the same assumed
conventions, so this is a consistency check, not an independent measurement.**

### 3.3 What is deliberately NOT done

* **No yaw reference, no recentre, no world offset.** `cod_origin` /`cod_axis`
  are relative to the OpenVR tracking origin. Aligning "forward in the room"
  with "forward in the game", adding the player's world position, and snap-turn
  all belong to the caller — this module has no opinion about where the player
  is standing.
* **Grip ≠ aim.** A controller pose is its *grip* pose: origin in the palm, −Z
  along the handle, which on every modern controller is tens of degrees off
  where the user thinks they are pointing. Turning that into a muzzle needs a
  per-controller offset and belongs in the weapon code. The public
  `poses_cod_*` helpers exist so that code can apply its offset in whichever
  space it prefers.
* **Hands are never guessed from index order.** Devices are identified by
  `GetTrackedDeviceClass`, then by `GetControllerRoleForTrackedDeviceIndex`,
  with `GetTrackedDeviceIndexForControllerRole` as a second question to the same
  runtime. If neither reports a role the hand slots stay empty and the log says
  so. Swapped hands are worse than absent hands.

---

## 4. UNTESTED — i.e. everything about behaviour

Not one line below has been executed against a runtime. All ASSUMED.

* That `poses_bind` accepts `gameframe.c`'s `g_sys` and the FnTable slots line
  up. The `IVRSystem_023` table is copied from `04_live_fntable`, which did
  MEASURE those slots against Proton's vrclient, but that was `GetTrackingSpace`
  and friends, not `GetTrackedDeviceClass` / `GetControllerRole…`.
* That `GetTrackedDeviceClass` and `GetControllerRoleForTrackedDeviceIndex`
  return anything useful under xrizer + Monado. **This is the single biggest
  risk in the module** — xrizer synthesises the OpenVR device model on top of
  OpenXR, and if it reports role `Invalid` for both controllers the hand slots
  stay empty and the module produces no controller poses at all.
* That `GetDeviceToAbsoluteTrackingPose` is implemented at all by xrizer, and
  that it is really non-blocking there (it is by OpenVR's contract).
* That `GetEyeToHeadTransform`'s 48-byte by-value struct return works. Exp. 4
  MEASURED that this specific return worked in its run ("survived the 48-byte
  struct return"); the 8-byte `GetHiddenAreaMesh` return is the one known to be
  broken. Assumed to still hold here.
* That the poses are in the tracking universe anyone expects. `poses_poll` asks
  for *standing* by default; `poses_update` inherits whatever the render path's
  `WaitGetPoses` used, which is the compositor's `SetTrackingSpace` setting, not
  ours. **These two can disagree**, and if they do, the same physical head will
  produce two different heights.
* The seqlock. It is ~10 lines and obviously correct on paper, but it has never
  had two real threads on it.
* Everything about the `.asi`: that the loader picks it up, that the marker-file
  gate behaves, that the log lands in `%TEMP%\bo1vr_poses.log`.

### The self-test plugin is inert by default — on purpose

An `.asi` in `C:\bo1vr` is loaded by *every* BlackOps.exe launch. A plugin that
quietly called `VR_InitInternal2` would seize the compositor from whatever else
was using it, which is exactly the accident this experiment was told to avoid.
So `poses_selftest.asi` runs the offline maths check, writes one line, and stops
— unless a marker file sits next to it:

| Marker in `C:\bo1vr` | Effect |
|---|---|
| *(none)* | offline maths check only. Never touches OpenVR. |
| `poses_selftest.on` | `poses_attach()` — adopts an **existing** session. Never calls `VR_InitInternal2`. |
| `poses_selftest_standalone.on` | additionally allowed to **create** a session. **This takes the compositor.** |

`make install` prints this same warning.

---

## 5. The exact test to run first

Run these in order. Stop at the first one that fails; each is cheap and each
isolates one assumption.

**Test 0 — no headset needed, do this now.**
```sh
cd experiments/12_poses && make && make check
```
Expect `poses_mathcheck: PASS (7 cases; no OpenVR was loaded)`. This is the
green baseline; if it ever goes red the transform has been broken by an edit.

**Test 1 — does the runtime even name the devices? (the biggest risk)**
With Monado up and the game *not* running:
```sh
make install
touch "$PFX/drive_c/bo1vr/poses_selftest_standalone.on"   # takes the compositor
# launch the game, or any host that loads dinput8.dll, then:
cat "$PFX/drive_c/users/steamuser/AppData/Local/Temp/bo1vr_poses.log"
```
Look for `hmd: device index 4294967295 -> 0` and, critically, `left`/`right`
lines. **If the log says "no controller reported a hand role", stop** — nothing
downstream can work, and the fix is in xrizer's device model, not here. Delete
the marker file afterwards.

**Test 2 — sanity of the numbers, still before any camera work.**
In the same log, with the headset on a desk and the controllers held at chest
height:
* HMD `cod org` Z (up) should be roughly the headset's height above the floor in
  inches — ~60–70 if standing, and it should *change* when you stand up. If it
  is near zero the tracking universe is seated, not standing.
* Turn to your left. `fwd` should rotate from ≈`[1 0 0]` toward ≈`[0 1 0]`.
  **If it rotates toward `[0 −1 0]` instead, the handedness assumption in §3.1
  is wrong** and the fix is to negate `out[1]` in `poses_cod_dir_from_ovr` —
  which will also flip case 1 of the self-check, so update the expectation
  deliberately rather than loosening it.
* Left and right controllers must be the ones you think they are. Wave one.

**Test 3 — feed it from the render path.**
Two lines in `gameframe.c` (**not applied — that file is owned by another
workstream**), inside `do_frame`, immediately after the existing
`WaitGetPoses`:

```c
    g_comp->WaitGetPoses(rposes, 64, gposes, 64);
    if (g_sys && !poses_ready()) poses_bind(g_sys);   /* once; cheap after that */
    poses_update(rposes, 64);                         /* render poses: drawn this frame */
```
plus `#include "poses.h"` and `out/poses.o` on the link line. Then check that
`poses_frame_count()` climbs at the frame rate and that the logged HMD pose
matches what Test 2 showed — that is the proof that binding and the FnTable are
right.

**Test 4 — only then**, hand `poses_get_eye()` to the camera hook
(`docs/camera-hook-plan.md` §3.2 step 1). Do not start here: an error in Tests
1–3 will look exactly like a broken camera hook.

---

## 6. Known risks, in order

1. **xrizer may not report controller roles.** Everything about hands depends on
   it. Test 1 answers this in five minutes and is the reason Test 1 is first.
2. **Units are ASSUMED inches.** If Black Ops is not inches, every position is
   wrong by a constant factor and nothing else is. `poses_set_units_per_metre()`
   is the one-line fix, and it doubles as the VR world-scale knob.
3. **Handedness of the left/right axis.** §3.1 says CoD `+Y` is *left*, from the
   measured `MatrixForViewer` negation. If that reading is wrong, the world is
   mirrored — which is subtle enough to be missed for a while. Test 2 catches it.
4. **Tracking-universe mismatch** between `poses_update` (inherits the render
   path's) and `poses_poll` (standing by default). Pick one and set it
   explicitly before shipping.
5. **Grip vs aim.** Correct poses will still feel wrong in-game until the weapon
   code applies a controller-specific grip→muzzle offset. Not a bug in this
   module, but it will be reported as one.
6. **The seqlock has never seen two threads.** Low risk (short critical section,
   x86, retry-on-torn-read) but genuinely untested.

---

## 7. Constraints honoured

* No game launch, no compositor connection, no `VR_InitInternal2` executed. The
  only program run was `poses_mathcheck.exe`, whose imports are KERNEL32 and
  msvcrt and which calls `poses_selfcheck()` and returns.
* No existing file modified. Everything is new under `experiments/12_poses/`;
  `experiments/11_gameframe/gameframe.c` was read only, and the two-line
  integration in Test 3 is written out here rather than applied.
* No git operations. Files are left uncommitted.
* Nothing under `/mnt/games/steam/steamapps/common/` touched; `make install`
  targets the prefix's `drive_c/bo1vr` and nothing else.
