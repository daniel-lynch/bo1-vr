# Experiment 14 — head orientation: the maths, and proof it is not mirrored

`experiments/13_camera` deliberately shipped a **position-only** camera. Its
RESULTS.md says why: a translation error is instantly visible and harmless,
whereas *"a wrong rotation basis yields a subtly mirrored world that can survive
scrutiny for a long time"*.

This experiment is the rotation. It is a separate directory from the hook for
one reason: **everything here is pure arithmetic, so it can be executed.** No
windows.h, no MinHook, no OpenVR, no game. The same `headtrack.o` that
`camera.asi` links also builds `headtrack_mathcheck.exe`, which runs offline —
so what is checked is the shipping code, not a copy of it.

```
$ make && make check
OK: no libgcc runtime dependency
OK: no OpenVR in the offline check
headtrack_mathcheck: PASS (10 cases; no OpenVR, no game, no window)
```

---

## 1. The conversion, end to end

Nothing here re-derives the OpenVR → CoD transform. exp 12 measured it
(`cod = (-v.z, -v.x, v.y) × 39.3700787`, verified as the exact inverse of the
project's `godot = (-Y, Z, -X) × 0.0254`, 7 cases passing) and this module
consumes its output verbatim, as `poses_pose_t.cod_axis`.

| symbol | what it is | rows expressed in |
|---|---|---|
| `H` | the head (or eye) basis from exp 12 | **tracking** space, CoD convention |
| `G` | the reference basis from the game's own `refdef.viewaxis` | **world** space |
| `F` | what gets written to `refdef+0x34` | **world** space |

All three are 3×3 row-major with rows = **forward, left, up** — `refdef+0x34`'s
measured layout (camera-hook-plan §2.1, confirmed live in exp 13) and exp 12's
`cod_axis` layout, which is the whole reason there is no adapter here.

```
F = H * G
```

**The derivation, which is what fixes the order.** The rows of a basis are the
child frame's axes written in the parent's coordinates, so a vector with child
coordinates `a` has parent coordinates `a0*row0 + a1*row1 + a2*row2`. The head's
forward axis has *reference-frame* coordinates `H`'s row 0; re-expressing it in
the reference frame's parent — the world — gives
`H[0][0]*Grow0 + H[0][1]*Grow1 + H[0][2]*Grow2`, which is exactly row 0 of
`H*G`. Not `G*H`, which would mean the game's rotation happens in *head-local*
coordinates.

`H = I` ⟹ `F = G`: a head aligned with tracking-forward leaves the game's own
view untouched. That is case 1 of the self-check and it is the degenerate case
every wiring error breaks.

**The reference is yaw-only by default.** `G` is built from the heading of
`refdef.viewaxis` and nothing else; the game's pitch and roll are discarded and
come from the head instead. With a full-orientation reference, a mouse-pitched
body frame makes head *yaw* rotate about a **tilted** axis and the horizon rolls
when you turn your head. `HT_REF_FULL` keeps the other behaviour (mouse pitch
survives, head pitch stacks on it) because only a headset can settle which
complaint is worse.

**Positions ride the same transform.** The eye offset (`eye.cod_origin −
hmd.cod_origin`, i.e. the headset's *own* IPD via `GetEyeToHeadTransform`, not
an assumed one) is a tracking-space vector, so it becomes world-space by
`G^T v` — `ht_ref_to_world`. Transposing *that* mirrors the stereo pair, i.e.
swaps the eyes, which is invisible on a monitor. Case 8.

---

## 2. Which check catches a transpose, and which catches a sign error

This is the part that matters, so it is stated exactly.

| fault | caught by | how |
|---|---|---|
| **Transpose** of `H`, of `G`, of the result, or **swapped order** `G*H` | **case 3** | Game yawed 90°, head pitched 40°, *no head yaw*. Yaw and pitch do not commute, so all four wrong forms give different answers — unlike a pure-yaw test, where they are identical. The expected value is the hand-derived closed form `(cos p cos y, cos p sin y, sin p)`. The case then *computes* `G*H`, `H^T*G`, `H*G^T` and `(H*G)^T` and **requires each to be ≥ 0.1 away from the right answer**, so "this test can see a transpose" is a tested property, not a comment. |
| **Transpose of `H` alone** (= the inverse rotation) | also **case 2** | head yaw +40° on a game yaw of 30° must give **70°**, not −10°. Only the sign distinguishes them, which is why the two angles differ. |
| **Sign error** producing a mirror (any single row or component negated) | **case 4 + case 5** | `ht_check_basis` requires **det = +1** *and* `forward × left = up`. Case 4 runs a compound yaw/pitch/roll head pose through and demands both. Case 5 hands the checker a deliberately mirrored basis and **fails if it is accepted** — because a checker that accepts everything is worse than no checker. |
| **Sign error in the yaw matrix or the recentre** | **case 1, 7** | case 1 requires the yaw-only reference to reproduce the game's heading exactly; case 7 requires recentring by −ψ to zero the head's yaw and to leave pitch untouched. |
| **Transposed `ref → world`** (swapped eyes) | **case 8** | `(0,1,0)` in the reference frame must land on `(−sin φ, cos φ, 0)`; the case also asserts `G` and `G^T` disagree, so it cannot pass by luck. |
| **Anything, at run time, on live data** | **the yaw invariant** | In yaw-only mode the reference rotation never touches world up, so the third *column* of `F` must equal the third column of `H`, exactly. `ht_check_yaw_invariant` tests that every frame with no knowledge of the right answer. Case 6 proves it bites: it must **reject** a transposed `H` and a swapped order. |

Short version: **case 3 is the transpose/order detector; det = +1 plus
`fwd × left = up` (cases 4 and 5) is the sign/mirror detector; the yaw invariant
carries both properties onto live data where no closed form exists.**

### 2.1 The checks were mutation-tested — MEASURED

A self-check that passes no matter what is the failure mode this project keeps
hitting. Single-token mutations were compiled and run in a scratch copy
(deliverables untouched):

| mutation | result |
|---|---|
| `ht_compose` computes `G*H` | **FAIL case 3** — composition is wrong (order, transpose or sign) |
| `H` transposed inside `ht_compose` | **FAIL case 2** — head yaw did not add to game yaw |
| `G` transposed inside `ht_compose` | **FAIL case 1** — identity head changed the view |
| `ht_yaw_matrix` sin sign flipped | **FAIL case 1** — reference did not reproduce the game heading |
| composed **left row negated** (a mirror) | **FAIL case 1** |
| `ht_ref_to_world` transposed | **FAIL case 8** — the stereo pair would be swapped |
| `ht_vec_yaw` rotation sense flipped | **FAIL case 7** — recentring did not zero the head yaw |
| `ht_build_reference` heading `y` negated | **FAIL case 1** |
| **exp 13's own orthonormality test** (`|det| == 1`, no cross-product check) | **FAIL case 5 — a MIRRORED basis passed ht_check_basis** |
| unmodified control | PASS (10 cases) |

The last row is a real finding about the existing code, not a hypothetical:
**exp 13's `check_axis` accepted a left-handed basis.** It tested
`fabsf(fabsf(det) - 1) < 1e-3`, which is true of a mirror — precisely what a
single sign error in a rotation produces, and precisely what that function was
put there to catch. `camera.c` now calls `ht_check_basis` instead, and case 5
fails if anyone ever loosens it back.

(One redundancy worth recording: with the cross-product test present, the
`det > 0` test alone is not load-bearing for *that particular* mirror — removing
either one still catches it. Removing **both** does not. That is defence in
depth, and it is why the mutation of exp 13's exact historical test is the row
that goes red.)

---

## 3. What is NOT verified

* **Everything about how it looks.** No headset, no game, no compositor was run
  — by instruction, another workstream is holding the game process. Every
  statement about behaviour in a headset is unverified by construction.
* **That the yaw-only reference is the right choice.** It is the defensible one
  (level horizon) but discarding mouse pitch changes how the game plays, and
  only a person in a headset can judge that. `HT_REF_FULL` exists for that
  comparison.
* **That OpenVR tracking space really is +X right / +Y up / −Z forward here.**
  That is exp 12's assumption, inherited; its Test 2 ("turn to your left; `fwd`
  should rotate toward `[0 1 0]`") is still the experiment that settles it, and
  it has not been run. If it turns out to be wrong, the fix is in
  `poses_cod_dir_from_ovr`, not here, and every case in this file that pins a
  direction will go red and have to be updated deliberately.
* **The units.** Still ASSUMED inches (camera-hook-plan §5.4). The eye-offset
  check in `camera.c` now prints the headset's measured IPD in units *and* in
  millimetres, which turns that assumption into a number in the log the first
  time the code ever sees a real headset.

## 4. Files

```
headtrack.h   the API, the conventions, and the derivation of F = H * G
headtrack.c   ~200 lines of maths and ~300 of self-check
Makefile      make / make check / make verify
```
