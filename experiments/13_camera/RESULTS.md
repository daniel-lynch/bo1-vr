# Experiment 13 — inside `R_SetViewParms` (BAC-281 spike)

**Question.** `docs/camera-hook-plan.md` derived the camera hook from the binary
alone: `R_SetViewParms` @ `0x6C7F80`, a non-standard LTCG convention with
**EDI = out, ESI = in and nothing on the stack**, plus `refdef` and
`GfxViewParms` field offsets. All of it was static analysis. Is it right?

**Yes — all of it, confirmed live in the running game:**

```
[camera] module base 00400000 -> R_SetViewParms 006C7F80
[camera] hooked R_SetViewParms; trampoline=050B0FE0. OBSERVE ONLY -- nothing is written.
[camera] call #900  out=03D4E540 in=29CF5440
[camera]   refdef vieworg  = 243.500 478.300 105.800
[camera]   refdef fwd      = -0.96593 -0.00000 -0.25882
[camera]   refdef left     = 0.00000 -1.00000 0.00000
[camera]   refdef up       = -0.25882 -0.00000 0.96593
[camera]   refdef axis  lengths 1.00000 1.00000 1.00000  dots 0.000000 0.000000 -0.000000  det +1.00000  -> ORTHONORMAL
[camera]   viewParms origin= 243.500 478.300 105.800 (w=1.000)
[camera]   viewParms ax lengths 1.00000 1.00000 1.00000  dots ...  det +1.00000  -> ORTHONORMAL
[camera]   |viewParms.origin - refdef.vieworg| = 0.000000  -> MATCH
```

---

## 1. Why this is a measurement and not a plausible-looking log

Printing floats and calling them a camera is exactly the kind of evidence this
project has been burned by. Two properties make the numbers check themselves:

* **A view axis is an orthonormal basis.** If the offsets were wrong we would be
  reading floats from the middle of neighbouring fields, and unit lengths, zero
  pairwise dots and a determinant of +1 would essentially never all hold. They
  hold on every sample, for **both** the input `refdef` axis (`+0x34`) and the
  output `GfxViewParms` axis (`+0x110/+0x11C/+0x128`).
* **The output origin must equal the input origin**, because `R_SetViewParms`
  copies it. It does, to `0.000000`.

That last one is the load-bearing check, because it can only pass if **all
three** independent facts are right at once: the EDI/ESI convention (or we would
not have a valid `in` or `out` at all), the `refdef` layout, and the
`GfxViewParms` layout. No two of them could look correct while the third was
wrong.

The values are also self-evidently a real camera: the origin moves through
world space across frames (`243.5 478.3 105.8`, later `69.2 584.0 112.0`), and
the axis rows are recognisably forward/left/up.

## 2. The thunk, and the bug in the first version

The plan's stub is MSVC `__asm { }`, which this toolchain does not have.
Top-level `__asm__` with `.globl` is the technique `src/winmm_shim.c` already
uses, and it keeps the hand-written part to the few instructions that must be.

**The first version tail-jumped to the trampoline** on the reasoning that the
original's plain `ret` would land back in `hk_body`. It did not. The symptom was
precise and would have been easy to misread: the pre-call logging appeared 8
times, the post-call logging **zero** times, and the game carried on rendering
perfectly. Control left `hk_body` for good and the engine still got its frame.

The fix is a real `call` through a frame, which is also the faithful thing to
do: the engine reaches `R_SetViewParms` with nothing pushed, so at its first
instruction ESP points at a lone return address. Tail-jumping left our two
arguments sitting above that return address — a stack shape the function is
never normally entered with.

```
_call_original:
    pushl %ebp ; movl %esp,%ebp ; pushl %esi ; pushl %edi
    movl 8(%ebp),%edi        /* out */
    movl 12(%ebp),%esi       /* in  */
    call *_o_R_SetViewParms
    popl %edi ; popl %esi ; popl %ebp ; ret
```

The Makefile's `verify` target disassembles both thunks on every build, because
a compiler that decided to "help" with either of them would produce a failure
that looks like a game bug.

## 3. Other things the run established

| | |
|---|---|
| Image base | `0x400000` — **not relocated**, so the plan's absolute VAs are directly usable. The code resolves relative to the preferred base anyway. |
| FOV | `tanHalfFov` = `0.84943 / 0.47780` in the menu, `1.02640 / 0.57735` in a map. `0.57735` is `tan(30°)`, i.e. a 60° vertical FOV — a sane, recognisable value. |
| Call rate | ~7 calls logged at the sampling points over ~50 s, consistent with one call per rendered frame. |
| `zNear` | **Discrepancy.** `refdef+0x5C` reads `0.00000` while `GfxViewParms+0x138` reads `4.00000`. The output is the believable one, so `+0x5C` is probably not `zNear` in the input, or it is filled in later. Flagged, not fixed — nothing here depends on it. |

## 4. What this does NOT do

It **observes**. Nothing is written. Moving the camera per eye is the next step
and is now a small change rather than a gamble: the same `hk_body` writes
`refdef+0x20` / `+0x34` before `call_original` and restores them after.

The restore is not optional — `docs/camera-hook-plan.md` §3.4 measured **122
readers of the view origin** across the client, reaching it through
`cg->refdef` by pointer.

## 5. Files

```
camera.c    the hook: two asm thunks, a C body, and the self-checks
Makefile    `make`, `make install` (-> C:\bo1vr); verify disassembles the thunks
```

Since §7 it also compiles and links two modules from sources it does not own —
`../12_poses/poses.c` and `../14_headtrack/headtrack.c` — into its own `out/`.
Neither source directory is written to.

Tested with `gameframe.asi` moved aside, so the only variable was this hook.

---

## 6. Alternate-eye stereo, wired (BAC-274 v1 architecture)

The camera hook now shifts the view per eye, and `gameframe.asi` drives the
alternation. Confirmed live:

```
[gameframe] PIPE LIVE: game frames -> compositor (alternate-eye)
[gameframe] frame 1200: 2400 successful eye submits
[camera] call #1000  eye=1 (camera shifted)
[camera] call #1001  eye=0 (camera shifted)
[camera] call #1002  eye=0 (camera shifted)
[camera] call #1003  eye=1 (camera shifted)
[camera] call #1004  eye=1 (camera shifted)
[camera] call #1005  eye=0 (camera shifted)
```

**Two `R_SetViewParms` calls per frame, both on the same eye, flipping every
frame.** That is exactly AER. The two calls have different `out` pointers
(`03D4E400`, `03D4E540`), i.e. two view slots per frame — consistent with the
bump-allocating call sites in `camera-hook-plan` §3.3.

### The aliasing trap, which cost a run

The first version sampled at `n % 900 == 0` and reported **`eye=1` every single
time** — indistinguishable from an alternation that was stuck. It was not: with
~2 calls per frame, every multiple of 900 lands on the same frame parity. A run
of **consecutive** calls shows the real pattern and cannot alias.

Worth generalising: a periodic sampler and a periodic signal will lie to you,
and the lie looks like a constant.

### Where the eye alternation lives, and why

In `gameframe.c`, at `Present`, not in the camera hook. The eye is a property of
the **frame** — one frame, one back buffer, one eye — and `Present` is the only
place that sees frame boundaries. `R_SetViewParms` runs more than once per frame
(measured above), so a counter there would not alternate per frame.

`camera.asi` exports `bo1vr_camera_set_eye`, which `gameframe.asi` resolves with
`GetProcAddress` once. A small explicit interface, rather than a shared global
in one of the two DLLs.

### Deliberately position-only — SUPERSEDED by §7, kept for the reasoning

The camera is shifted sideways by half an IPD along the view's own **left** axis
and nothing else. Orientation is untouched.

That is a choice, not an omission: a sign error in a translation is instantly
visible and harmless, whereas a wrong rotation basis yields a subtly mirrored
world that can survive scrutiny for a long time — this project has already lost
time to exactly that with the props' UV pair. Head orientation comes next, from
exp 12's poses, once the translation has been confirmed by eye.

The origin **is restored** after `call_original`: 122 measured readers reach it
through `cg->refdef` by pointer, so leaving the offset in place would move the
player's idea of where they are, not just the picture.

### IPD as a unit test

`g_ipd_units` defaults to 2.6, from a 65 mm human IPD and the ASSUMED
inches-per-unit of `camera-hook-plan` §5.4. If that assumption is wrong the
symptom is obvious and harmless — the stereo separation looks like a giant's or
a doll's — which makes it a cheap discriminating test rather than a guess buried
in the code. `bo1vr_camera_set_ipd_units()` changes it.

### NOT verified

That the result **looks** correct in a headset: real parallax, correct eye
order, comfortable depth. That is BAC-282 and it needs hardware on a head.

---

## 7. Head orientation, wired (BAC-282)

The hook now turns the camera as well as moving it. `refdef+0x34` is written
with the HMD's orientation composed onto the game's heading, and restored after
`call_original` for the same reason the origin always was.

**Nothing in this section has been run.** The game process was held by another
workstream throughout; this is a build-and-static-verify change. What *was*
executed is the maths, offline — see `experiments/14_headtrack/RESULTS.md`.

### The composition, in one line

```
F = H * G            written to refdef.viewaxis, then restored
```

* `H` — the eye basis from exp 12 (`poses_pose_t.cod_axis`), rows
  forward/left/up in **tracking** space, already in the CoD convention. exp 12's
  transform is reused verbatim; there is no second coordinate convention in the
  tree.
* `G` — the **reference** basis, built from `refdef.viewaxis`. By default it is
  the game's *heading only*: a pure yaw about world up, with the game's pitch
  and roll discarded and taken from the head instead. A full-orientation
  reference makes head yaw rotate about a mouse-tilted axis, and the horizon
  rolls when you turn your head. `bo1vr_camera_set_ref_mode(HT_REF_FULL)`
  selects the other behaviour for comparison in a headset.
* `F` — the head in world space.

`H = I` gives `F = G`, i.e. the game's own view untouched. The order is fixed by
the row convention (rows are the child frame's axes in parent coordinates), not
by taste; `G * H` would apply the game's rotation in head-local coordinates.

The eye offset now comes from the **headset's own `GetEyeToHeadTransform`**
rather than the assumed 2.6-unit IPD, rotated into the world by `G^T`. The
assumed shift remains as the fallback whenever there is no pose — and whenever
the runtime's offsets fail their sanity check.

### Which check catches what — the whole point of the exercise

§6 argued that a wrong rotation basis is dangerous because it *survives
scrutiny*. So the checks are built to fail on the specific faults, and were
mutation-tested to prove it (nine mutations, nine reds — table in exp 14 §2.1):

* **A transpose, or the wrong multiplication order** → `headtrack_mathcheck`
  **case 3**: game yawed 90°, head pitched 40°, no head yaw. Yaw and pitch do
  not commute, so `G*H`, `H^T*G`, `H*G^T` and `(H*G)^T` all differ from the
  hand-derived closed form — and the case *computes all four* and requires each
  to be ≥ 0.1 away, so its discriminating power is tested rather than asserted.
  A pure-yaw test would have passed every one of them.
* **A sign error, i.e. a mirror** → `ht_check_basis`, which requires
  **det = +1** *and* `forward × left = up`; **case 5** feeds it a mirrored basis
  and fails if it is accepted.
* **At run time, on live data** → the yaw invariant, which watches exactly one
  thing: that `ht_compose` carried `H`'s third column through unchanged. In
  yaw-only mode that column must survive, so a **transposed `H`**, a **swapped
  order**, and arithmetic damage inside `ht_compose` are all caught, and case 6
  proves each is rejected. **It is blind to every error in `G`** — the yaw-only
  branch writes `G`'s third column as the literal `(0,0,1)`, so `H·Gᵀ`,
  `H·yaw(wrong angle)` and even `H·I` were all measured at `err = 0.000000` and
  **accepted**. `G` is pinned offline by cases 1 and 9 instead. An earlier
  version of this section claimed the invariant caught "any sign error in the
  yaw matrix"; that was false, it was caught in review, and case 6 now pins the
  blindness so the claim cannot silently drift again.

**A real defect this turned up in the existing code.** §1's `check_axis` — the
function this experiment's whole credibility rested on — tested
`fabsf(fabsf(det) - 1) < 1e-3`, i.e. `|det| = 1`. That is **true of a mirrored
basis**, which is exactly what a single sign error in a rotation produces. It
would have reported `ORTHONORMAL (offsets confirmed)` on an inside-out world.
It now calls `ht_check_basis`, which requires `det = +1` **and**
`forward × left = up`.

Precisely what the suite pins, since an earlier version of this section got it
wrong: those two criteria are **redundant**. With orthonormal rows,
`cross_err = 0` already implies `det > 0`, so reverting the determinant
criterion *alone* leaves the suite green — no test pins it by itself. What case
5 pins is the **pair**: remove both, which is exactly the old `|det|` test, and
case 5 goes red. Defence in depth, not two independent tests. Measurements in
exp 14 §2.1.

### Fail loud, fail safe

Three independent gates, all of which degrade to *exactly the previous
position-only behaviour* rather than to a plausible-looking wrong one:

| gate | when | what happens |
|---|---|---|
| `ht_selfcheck()` at DLL load | the maths is broken in this build | orientation never written; one line in CAPITALS naming the failing case |
| per-view basis + invariant check | a composed basis is not right-handed orthonormal, or the invariant breaks | that view keeps the **game's** orientation; the numbers (lengths, dots, det, mirrored flag, invariant error) are logged; 30 consecutive failures disable orientation for the session |
| eye-offset sanity | separation is not a human IPD, or the left eye is not on the left | the runtime's offsets are refused, the assumed IPD shift is used, and the log says which of the two it was |

The eye-offset check doubles as the **units** experiment: it prints the
headset's measured IPD both in game units and in millimetres, so
camera-hook-plan §5.4's assumed inches becomes a number in the log the first
time this code sees a real headset.

### Two instruments that had started to lie

Both were found by reading, not by running, and both are the failure mode this
file keeps warning about — an instrument that prints something reassuring.

1. **`|viewParms.origin - refdef.vieworg|`** (§1's load-bearing check) compared
   the engine's output against the refdef *after the restore*. That was right
   while the hook only observed; the moment it started shifting the camera the
   check began printing `MISMATCH: something in the chain is wrong` on a
   perfectly healthy frame. It now compares against a snapshot of what was
   actually handed to the engine, and the restore is reported separately.
2. The first-frame log printed `game fwd` from a pointer that aliases
   `refdef+0x34` — *after* the composed basis had been memcpy'd over it. It
   would have shown the game and the head agreeing exactly, always. The log now
   happens before the write, and the code says why the order matters.

A third check is new: `max|viewParms.axis - what we sent|`. Without it, "the
engine used our basis" and "the engine rebuilt the basis from the player's
angles and ignored ours" produce identical evidence — a `viewParms` axis that is
orthonormal and plausible.

### Where the pose comes from, and the compromise in it

`gameframe.asi` owns the OpenVR session and the frame clock but exports no pose,
so `camera.asi` takes exp 12's second documented route: `poses_attach()`, which
asks the already-loaded `openvr_api.dll` for `IVRSystem_023` and **never calls
`VR_InitInternal2`**, so it cannot steal or duplicate the compositor session.
Poses come from `poses_poll()`, which does not touch the compositor's frame
pacing. `make verify` fails the build if a `WaitGetPoses` reference or an
OpenVR import ever reaches `camera.asi`.

The compromise: that is the *polled* pose, not the render pose `WaitGetPoses`
hands the compositor, so the two can differ by a fraction of a frame and
reprojection will be slightly inconsistent with what was drawn. The fix is one
line in `gameframe.c` (`poses_update(rposes, 64)`, exp 12 §5 Test 3) plus a pose
export — that file belongs to another workstream and was not touched.

Sampling happens **once per frame**, in the `R_RenderScene` hook, and both eyes
are built from that one sample. Sampling per view would give the two eyes poses
from different instants — a vertical-disparity headache rather than a visible
glitch.

### The isolation configuration, which used to be the one that could not work

The sample call originally sat *below* the `bo1vr_capture_eye` resolution block,
whose failure path returns early. With `gameframe.asi` absent, that early return
meant `poses_attach()` was never reached and head tracking was silently and
permanently dead — in exactly the configuration §5 documents as this
experiment's isolation test ("tested with `gameframe.asi` moved aside, so the
only variable was this hook"). The one arrangement in which someone would go
looking for a head-tracking fault was the one arrangement in which head tracking
could not run, and it would have looked like a broken pose pipeline. Found in
review.

The sample is now the first thing the hook does, and that path renders **mono
from the head** (`CAM_EYE_CENTRE`: head orientation, no eye offset) instead of
leaving the camera alone, so head tracking can be watched on a flat monitor with
no `gameframe.asi` at all. The log says `NO STEREO ... but head orientation
still applies` rather than the old silent single line.

### Controls, and why exports alone were not controls

```c
bo1vr_camera_set_head_tracking(int on);      /* default on                    */
bo1vr_camera_set_ref_mode(int mode);         /* 0 = yaw-only (default), 1 = full */
bo1vr_camera_recentre(void);                 /* "straight ahead" is now here  */
bo1vr_camera_set_position_tracking(int on);  /* room-scale lean, default OFF  */
bo1vr_camera_set_units_per_metre(float u);   /* world scale                   */
```

These five shipped as the *only* interface, and review pointed out that made
them **dead code**: nothing in the process resolves them — `gameframe.asi` looks
up `bo1vr_camera_set_eye` and nothing else. So `HT_REF_FULL` could not be
selected (the comparison this section calls for could not be performed), head
tracking could not be switched off without deleting `camera.asi` and losing
stereo with it, and recentring could not be triggered at all — which mattered
more than it sounds, because `g_yaw0` was a one-shot: a garbage first pose right
after `poses_attach()` would have fixed a wrong "straight ahead" for the entire
session with no way to correct it.

Two paths now exist that need no rebuild and no cooperation from any other
component:

| switch file in `C:\bo1vr` | key | effect |
|---|---|---|
| `nohead.on` | RCtrl+F10 | orientation off — back to the position-only camera |
| `reffull.on` | RCtrl+F11 | `HT_REF_FULL` instead of yaw-only — **the comparison** |
| `roomscale.on` | RCtrl+F12 | positional head tracking on |
| `recentre.on` (consumed) | RCtrl+F9 | recentre now |

The files follow `gameframe.c`'s existing bisect-switch pattern and are re-read
every 90 frames, so they work while the game is running; `recentre.on` is
deleted when it fires, so it behaves like a button. Only `GetAsyncKeyState`'s
`0x8000` "is down now" bit is read, never the `0x0001` "pressed since last call"
bit — that low bit is process-wide state and reading it would consume the event
out from under the game's own input. Every change logs the resulting state.

The automatic first recentre now also **waits for 30 consecutive poses the
runtime reports as `Running_OK`** before capturing `g_yaw0`, and says in the log
what it is waiting for. A human-requested recentre does not wait.

One new runtime dependency, visible in `make verify`: `USER32.dll`, for
`GetAsyncKeyState`. It is a system DLL that every Win32 GUI process — including
BlackOps.exe — already has mapped, so nothing new is shipped; the `verify`
target's rule is that *libgcc* must not appear, and it still does not.

Room-scale translation is off by default and that is not timidity:
`cod_origin` is measured from the *tracking origin*, so a standing player's head
is ~65 units up, while the game's `vieworg` is already at their eye. With it on,
only the **delta since the recentre** is added — lean and crouch, nothing else.

### Tracking dropout

A dropped pose holds the last one for 90 frames and then releases the view back
to the game's orientation, with a log line. Releasing immediately would *snap*
the world by however far the head was turned; holding forever leaves the view
stuck at an angle nobody can correct once the headset is off.

### The yaw-only default is a GUESS, and it is the open design question

Yaw-only keeps the horizon level, which is why it is the default. It also has
consequences nobody here has lived with:

* **It decouples aim from view.** The refdef axis is restored after the call, so
  the player's weapon still points where the *mouse* points, not where they are
  looking. In a headset that is either fine (you aim with the mouse, you look
  with your head) or deeply wrong, and no amount of reading settles it.
* **It discards scripted pitch and roll.** Black Ops moves the camera itself in
  death cams, vehicle sections and the intro. Those are pitch and roll, and
  yaw-only throws them away — the player would keep a level horizon through a
  sequence built on tilting it.

`HT_REF_FULL` keeps all of it and pays with a horizon that rolls when you turn
your head under mouse pitch. **Which is worse can only be decided in a headset**,
which is precisely why the runtime toggle above exists. Neither default is
validated. Do not read the yaw-only default as a conclusion.

### NOT verified — everything about behaviour

*(Superseded in part by §8: the first headset session confirmed `poses_attach()`,
the self-check, the recentre and the pose pipeline. Everything below that it does
not name is still untested.)*

The game was not run, by instruction, in either round. So: that the composed view
looks right, is not mirrored, does not swim, and does not fight the mouse; that
90 frames is a sensible dropout hold; that 30 `Running_OK` poses is long enough
for a runtime to settle; that `RCtrl+F9..F12` are actually free in Black Ops,
that `GetAsyncKeyState` sees keys at all under Proton with the game holding raw
input, and that `CAM_EYE_CENTRE` renders a sane mono view. The maths is executed
and mutation-tested. **The behaviour is not tested at all**, and the controls
added to make it testable are themselves untested.

---

## 8. "Head tracking worked until I moved the mouse" (first headset session)

The plumbing worked: attached to the live session, self-check 10/10, recentred
after 30 steady poses at `tracking_result 200`. Then that report.

**The verdict is (b): nothing stopped. The mouse's PITCH was being thrown away,
by design, and from inside a headset that is indistinguishable from head
tracking failing.** The evidence is an exhaustive audit of every path that can
skip the orientation write, and it is short enough to check.

### Why it cannot be (a)

`headtrack_apply` has exactly five ways to not write an orientation:

| path | can a mouse move cause it? |
|---|---|
| `g_ht_dead` / `!g_ht_enable` / `!g_ht_math_ok` | No. Only 30 consecutive rejects, `RCtrl+F10`/`nohead.on`, or a load-time self-check failure — **all three log loudly.** |
| eye slot out of range | No. Set by our own `R_RenderScene` hook. |
| `!g_eyepose[eye].valid` | No. Written only from pose data; a mouse cannot invalidate a pose. Logs after 90 frames. |
| `ht_build_reference` returned 0 | Only if the game's own view axis stops being orthonormal, which mouse input does not do. (The "both rows vertical" branch is unreachable for a real basis: if forward is vertical then left, being perpendicular to it, is horizontal.) |
| composed basis rejected | `F = H·G` of two rotations is a rotation, and in yaw-only mode `G`'s third column is literally `(0,0,1)`, so the invariant cannot fail. Logs loudly regardless. |

**The double-counting question specifically:** `g_yaw0` is written in exactly one
place, from `hmd.cod_axis` — the head's yaw in *tracking* space. It is never a
function of the game's heading. So the composed yaw is
`head_yaw − g_yaw0 + game_yaw`, each term contributing once; a mouse turn moves
`game_yaw` and nothing else. Nor is there a feedback path: the refdef axis is
restored unconditionally under `if (moved)` with no `return` between the write
and the restore, so `G` is always built from the *game's* axis and never from our
own previous output. A stale `g_yaw0` shifts "straight ahead" by a constant; it
cannot be *triggered* by a mouse turn, and it cannot accumulate.

### What was actually happening

In yaw-only mode the reference is the game's **heading only**. The game's pitch
is discarded — but the engine's view angles still pitch, so the mouse moves the
**gun, the crosshair, and the engine's idea of where you are looking** while the
picture stays flat. Push the mouse up for a couple of seconds and the game
believes you are staring at the sky while you see level ground; everything you
then do behaves oddly, and the head still working is no comfort. That is the
report, exactly.

There is a second, sharper edge underneath it, which review predicted as LOW-9:
once mouse pitch passes **±84.3°** the heading has to come from the left row
instead of the forward row, and the old code **switched** at that threshold. The
two sources agree only when there is no roll; under roll they differ by up to the
roll angle, so crossing that band made the picture **jump in yaw**. Nothing drove
pitch through that band until head tracking shipped and a mouse started driving
it.

### Fixed

* **The band is blended, not switched** (`HT_BAND_LO`/`HT_BAND_HI`, 84.3°→78.5°):
  continuous, no hysteresis state, and *exactly* the old behaviour outside the
  band. exp 14 **case 11** sweeps pitch 70°→89.5° at 25° of roll and fails if the
  heading moves more than 2° per 0.25° step; the old threshold form, reproduced
  as a one-token mutation, **fails it**.
* **The default is now `HT_REF_FULL`.** With the head level, full reference
  renders bit-identically to the flat game, the mouse does what it always did,
  and the scripted cameras (death cams, vehicle sections, the intro) survive.
  Yaw-only is one keypress away. A *reasoned* default, not a validated one.
* **`HT_REF_FULL` is no longer the least-checked path.** It had no runtime guard
  at all (the yaw invariant says nothing there) and one offline case that tested
  only `H = I` — least-checked exactly when a frustrated player switches to it.
  It now has `ht_check_round_trip` (`F·Gᵀ` must come back as `H`; valid in **both**
  modes; catches a transposed head, a swapped order, a non-rotation reference)
  plus exp 14 **case 12**, which composes a *rolled* and a *yawed* head onto a
  pitched reference against hand-derived closed forms.
* **The mode difference is executable, not prose.** Case 12 asserts that a head
  yaw over a pitched reference tilts the horizon in full mode (`F.left.z ≠ 0`)
  and does not in yaw-only (`F.left.z == 0`). That is the whole HIGH-3 trade-off,
  as a test.

### The log now answers this question by itself

Working out (a) vs (b) took an audit. It should have taken one line, so:

* `*** ORIENTATION NOT APPLIED: <reason>` / `APPLIED (resumed)` — logged on the
  **transition**, naming which of the five paths. If orientation ever stops there
  is a line at the instant it stopped; if it never stops, the absence of that
  line is the evidence.
* `*** MOUSE PITCH IS BEING DISCARDED. The game's view is pitched %+.1f deg …` —
  once, in words, the first time the game's pitch exceeds 20° in yaw-only mode,
  naming the fix (`RCtrl+F11`).
* `heartbeat: mode=…, oriented N/M views, game pitch …, head yaw/pitch …,
  rejects …` every 1800 views (~8 s at 60 fps). `oriented == views` across a
  session *is* the proof that nothing stopped.

### `nocap.on` — confirmed, head tracking still samples

The stable configuration needs `nocap.on`, which makes `bo1vr_capture_eye` return
early. It is still **exported**, so `GetProcAddress` succeeds, `g_capture` is
non-NULL, and the dual-render path runs as before. Independently of that,
`headtrack_sample()` is the **first statement** in the hook — above the
`g_capture` block and its early return — so every configuration samples,
including the one where the export is missing entirely. That was the HIGH-1 fix,
and it is what makes this robust to a change in `gameframe.asi`'s exports.

One thing for whoever owns `gameframe.c` (not touched): with `nocap.on` the two
scene renders still run, eye 0 then eye 1, and the Present-time resolve takes
whatever the back buffer holds — always the **second** render. Head tracking is
unaffected (one pose, both renders), but the stereo pair may not be what it looks
like.

### The playtest that settles it — please run in this order

**A. Did anything ever stop? (30 seconds; answers (a) vs (b) for good.)**
Load a map, look around with your head only, confirm it tracks. Now move the
mouse — pitch it up and down deliberately. Look around with your head again.
Quit and send `%TEMP%\bo1vr_camera.log`.
*Tell me:* does the log contain any `ORIENTATION NOT APPLIED` line? If yes, paste
it — that is (a), and the line names the cause. If no, and the `heartbeat` lines
read `oriented N/N`, then orientation never stopped and this section is confirmed.

**B. Does the mouse feel right now?** This build defaults to the full reference.
*Tell me:* does moving the mouse up and down move the **picture** again, the way
it does without the headset? (Expected yes. If the picture still will not pitch,
that is a real bug and A's log will say why.)

**C. Which reference do you actually want?** **RCtrl+F11** switches; play a
minute in each.
*Tell me three things:* (i) which one you can aim in; (ii) in **full**, hold the
mouse pitched up and turn your head left and right — is the horizon tilt
ignorable or nauseating?; (iii) in **yaw-only**, does anything go strange in a
death cam or a scripted sequence?

**D. The band fix (yaw-only only — full mode never uses that code).** In
yaw-only, push the mouse all the way up until the gun points at the sky, then pan
left and right.
*Tell me:* does the picture snap or jump in yaw at any point? (Before this build
it would, by up to the roll angle. Expected now: smooth.)

`RCtrl+F9` recentres if "straight ahead" ends up wrong. `RCtrl+F10` turns head
orientation off without losing stereo — a useful A/B if something feels wrong and
you want to know whether the head is causing it.

### Still needs a headset, after this round

Everything in §8 above is reasoning over code plus offline tests. Unverified:
that full reference actually feels correct; that the horizon tilt is tolerable;
that the blended band is smooth *in practice* (the test proves the maths is
continuous, not that the picture is); that mouse pitch through the full range
does not expose something else; and that the new heartbeat and transition lines
appear at all in a real log. The keybinds remain untested — if `RCtrl+F11` does
nothing, use `yawonly.on` / `reffull.on` in `C:\bo1vr` instead, and please say so,
because that would mean `GetAsyncKeyState` is not reaching us under Proton.
