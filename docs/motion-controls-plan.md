# Motion controls: what exists, what is missing, and the injection point

Status as of the "how close are we?" question. Short answer: **the two hard
halves are done and the join is located.** What remains is real work, but it is
wiring rather than discovery.

## 1. What already exists

### The pose side — built, verified offline, running, unread

`experiments/12_poses` turns OpenVR tracked-device poses into CoD-convention
bases. It already has `POSES_HAND_LEFT` and `POSES_HAND_RIGHT` alongside
`POSES_HMD`, each carrying `cod_origin[3]` (game units) and `cod_axis[3][3]`
(rows: forward, **left**, up), plus `connected`, raw `tracking_result`, and
velocities.

It is bound and polling in the live build — head tracking runs through exactly
this path. **But nothing reads the hand slots**: `camera.c` called `poses_get`
only for `POSES_HMD`.

Two caveats recorded in exp 12's own RESULTS.md and worth repeating:

* nothing in that module had **ever been run against a headset** — its whole
  coordinate section is marked ASSUMED. This build now logs both controller
  poses (validity changes plus a slow heartbeat), which is the cheapest way to
  turn that assumption into an observation before anything is built on it.
* a controller pose is the **GRIP** pose: origin in the palm, -Z along the
  handle. Turning it into a muzzle needs a per-controller offset, and exp 12
  deliberately left that to the caller.

### The camera side — done

Head look and aim are already decoupled: `hk_body` writes the head basis into
the refdef for the render and restores it afterwards, so the game's own aim is
untouched. That separation is precisely what motion control needs — it means
aiming can be driven independently without fighting the view.

## 2. The missing join, now located

Aim in this engine accumulates into two floats, written by the mouse-look
function `FUN_00881930`:

| Address | What |
|---|---|
| `0x2911E20` | accumulated **pitch** |
| `0x2911E24` | accumulated **yaw** |

From the decompilation, with the dvar layout this project already verified
(`current` at `+0x18`):

```c
/* yaw   */ fVar10 = *(float *)(DAT_0290bec4 + 0x18) * dx;   /* m_yaw   */
            DAT_02911e24 = DAT_02911e24 - fVar10;
/* pitch */ fVar10 = *(float *)(DAT_0290bef4 + 0x18) * dy;   /* m_pitch */
            DAT_02911e20 = DAT_02911e20 + fVar10;
```

and both are then packed into the structure handed to `FUN_0051AE50` /
`FUN_005C2180` and written back. So these two globals ARE the player's view
angles, and writing them is how the weapon is aimed.

Supporting dvars found in `CL_Init` (`0x590C10`), all with their pointer globals:

| dvar | pointer global | use |
|---|---|---|
| `m_yaw` | `0x290BEC4` | yaw scale |
| `m_pitch` | `0x290BEF4` | pitch scale |
| `sensitivity` | `0x290BEB8` | |
| `cl_bypassMouseInput` | `0x28D9084` | **stop the mouse fighting the controller** |
| `cl_freelook` | `0x2910254` | |

`cl_bypassMouseInput` is the useful one: a registered bool that suppresses mouse
look, so controller-driven aim does not have to fight a second writer. It is
reachable through the `g_forced[]` table that already exists.

## 3. What is left

1. **Confirm the controller poses on hardware.** Instrumented; needs one
   playtest. Everything below is worthless if they are not live.
2. **Grip → muzzle offset.** Per-controller, and it decides whether the gun
   points where the hand points or where the barrel would.
3. **Drive aim from the pose.** Convert the controller forward vector to
   yaw/pitch and write `0x2911E20` / `0x2911E24`, with `cl_bypassMouseInput`
   forced so nothing else writes them the same frame. Absolute (aim = where the
   controller points) is the VR-correct default; relative is the fallback if
   absolute fights the engine's own clamping.
4. **Buttons and triggers.** Untouched so far. OpenVR legacy button state is the
   cheaper route than an action manifest; it has to reach the game as input,
   which is a separate injection point from the angles above and is NOT yet
   located.
5. **Two-handed hold, holsters, physical reload** — all downstream of 1-4.

## 4. Honest estimate

Aiming with the controller is the milestone worth naming, and it is close: the
data exists, the write target is known, and the mouse can be told to stand
aside. Steps 1-3 are a playtest and a modest amount of code.

Full motion controls in the sense the Vice City VR release notes use the phrase —
tracked hands, two-handed grips, physical scopes, holsters, manual reload — is a
much longer road, and step 4 (input) has not even been surveyed yet.
