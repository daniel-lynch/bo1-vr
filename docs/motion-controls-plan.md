# Motion controls: what exists, what is missing, and the injection point

A status snapshot: how close is this to working motion controls? Short answer:
**the two hard halves are done and the join is located.** What remains is real work, but it is
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

## 2. The missing join -- FIRST CANDIDATE REFUTED, see 8

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

---

## 5. Update: the controller poses are confirmed live

First hardware observation of the exp 12 hand slots, from a real play session:

```
controller left:  pos 1.9 -9.4 20.4  fwd -0.974 -0.061 0.220  tracking_result=200 connected=1
controller right: pos 0.1 -8.6 19.9  fwd  0.229 -0.972 0.046  tracking_result=200 connected=1
controller left: NO valid pose (off, asleep, or not tracked)
```

Positions move with the hands, forward vectors are unit length and plausible,
`tracking_result` is 200 (`Running_OK`), and the left controller dropping out
when it was set down is exactly the case the change-detection was written for.

**Step 1 of §3 is done.** Exp 12's coordinate section stops being ASSUMED.

## 6. Why this is now the priority, not a nice-to-have

Playtest feedback after positional head tracking went in:

> "Updated headtracking does feel good but now makes accuracy hard because your
> head is moving in world space but the gun is in a fixed place"

That is not a bug in head tracking — it is the direct consequence of doing head
tracking properly while aim is still bolted to the flat-screen input path. The
view now translates and rotates in world space; the weapon still fires from the
player's fixed position along the game's own angles; and the crosshair is drawn
at screen centre, which is now wherever the head is looking rather than where
the gun points. Three frames of reference, two of them lying.

There are only two honest fixes, and they are §3's remaining steps:

1. **A crosshair that tells the truth** — shows where the WEAPON points, not
   where the head does (#41). `xhair3d.on` is now enabled to test the cheap
   version of this.
2. **Aim the gun with the hand** (#45), so the two frames agree by construction.

## 7. The aim dry run

The transform from a controller's tracking-space forward vector to the two
accumulators is not known. The recentre yaw applies to the hand as it does to
the head, and the reference basis was captured from the game's own view
direction, so there is a frame relationship to establish — and getting the sign
wrong spins the player uncontrollably, which is a ruined session that teaches
almost nothing.

So `aimlog.on` logs both sides of the equation and **writes nothing**:

```
aimlog: game pitch P yaw Y | hand raw pitch p yaw y | hand recentred yaw y' (yaw0 Y0) | head yaw h
```

Fit the transform from that, then write it behind its own switch. The addresses
are BSS values read out of a disassembly, so they are `VirtualQuery`-checked
before every read: a wrong address that silently returned plausible-looking
garbage would be far more expensive than one that faults.

---

## 8. REFUTED: `0x2911E20` / `0x2911E24` are not the view angles

The dry run earned its keep on its first session. Sample rows:

```
aimlog: game pitch 0.00 yaw 70.46 | hand raw pitch -17.81 yaw -98.64 | hand recentred yaw 26.68 (yaw0 -125.33) | head yaw -0.10
aimlog: game pitch 0.00 yaw  0.00 | hand raw pitch -30.40 yaw -42.13 | hand recentred yaw 83.20 (yaw0 -125.33) | head yaw 52.17
aimlog: game pitch 0.00 yaw  0.00 | hand raw pitch  5.75 yaw -67.14 | hand recentred yaw 58.19 (yaw0 -125.33) | head yaw 45.12
```

Through nearly all of actual play both globals read **0.00**, while the head
column swings across 100 degrees and the hand column across 60. A player
demonstrably aiming in many directions while the claimed "view angles" sit at
zero refutes the claim outright. They were non-zero only in the opening rows,
before gameplay proper, and held steady there rather than tracking anything.

So §2's identification was wrong. What `FUN_00881930` accumulates into them is a
mouse-input quantity that is consumed and cleared, not the player's absolute
orientation — consistent with them feeding `FUN_0051AE50`, which merely copies
the pair into a per-client structure (stride `0xEB0`, base `0xBA68xx`) for
`FUN_007576E0` to apply.

**The cost of getting this wrong was one log line, because the dry run never
wrote.** Had the transform been guessed and shipped, the symptom would have been
a player who cannot aim, in a build that also changed three other things.

## 9. The better source, which was already in hand

`hk_body` receives the refdef **before** head tracking is composed in, and saves
it in `save_axis` to restore afterwards. That saved forward row is the engine's
own aim direction — where the weapon points — and it needs no reverse
engineering, no BSS address and no memory-safety check, because the engine
hands it to us every frame.

It is now logged as `AIM pitch/yaw`, which does two jobs:

* it says what the two globals really are, by comparison against a known-good
  aim angle rather than another disassembly session;
* **it is what #41 needs.** A crosshair that tells the truth must be drawn where
  the weapon points, and that direction has been passing through our hook all
  along.

For #45 this also suggests a better shape than writing absolute angles: with the
current aim known each frame and the desired (hand) direction known, the
difference can be fed in as an input delta and let the engine's own clamping and
smoothing do their work — steering the aim rather than overwriting it. That
needs the input path (#46), but it fights the engine far less than a blind write.

---

## 10. Second angle candidate also refuted — and we should stop looking

`0x2911DA8` (from `docs/input-path-findings.md`, "the absolute view angles,
written by `0x448BB0` from cgame") read **`0.00 0.00 0.00` for an entire
session**, while the aim derived from the refdef axis showed real, changing
values in the same log line:

```
aimlog: AIM pitch 1.30 yaw -88.12 | ABS angles 0.00 0.00 0.00
```

Either the address is wrong or it is indexed per local client in a way we did
not account for. Not usable as written.

**No third candidate is being tried.** Three attempts at the absolute angles
have now cost three sessions, and the conclusion is that the goal was wrong
rather than the addresses: `hk_body` is *handed* the engine's own aim axis every
frame. That is strictly better than any global — no address to get wrong, no
version to check, no memory-safety guard, and it cannot be zero when the game is
running because the engine just used it to render.

The same session confirms it works: `AIM pitch 1.30 yaw -88.12` is a real
heading, and the reticle drawn from it landed at pixel 1287,854 of 2560x1440.

And for motion controls the target has moved anyway. `docs/viewmodel-findings.md`
found the weapon is already an attachment at `tag_weapon`, so driving that bone
puts the gun in the tracked hand without writing any angle at all. Writing view
angles was never going to be the shape of this.

**Standing lesson for this file:** prefer a value the engine hands you over an
address you went looking for. Two of the three confident identifications in this
document were wrong, and both were addresses; nothing derived from the refdef
has failed.
