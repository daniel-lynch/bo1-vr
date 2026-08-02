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
