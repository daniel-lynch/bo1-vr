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

### Deliberately position-only

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
