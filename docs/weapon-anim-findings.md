# Weapon / viewmodel animation: can playback time be driven externally? (BAC-279)

Target: `BlackOps.exe`, 8,101,944 bytes, PE32 x86, `ImageBase 0x400000`, no ASLR.
All addresses are virtual addresses in that image. Nothing was executed; every
claim below was read out of the binary's own code and data.

Clean-room: derived from this binary only. No decompilation project, PDB or
source dump was consulted.

## Evidence grades

- **[V] VERIFIED** — read directly out of this executable's code or data, and the
  reasoning from those bytes to the claim is mechanical.
- **[I] INFERRED** — a judgement built on [V] facts. The reasoning is stated so it
  can be attacked.
- **[?] UNKNOWN** — explicitly not established.

Names in `CamelCase` are *my* labels for unnamed functions, chosen to describe
what the code does. They are not symbols recovered from the binary.

---

## VERDICT

**Yes. The weapon animation's playback time is a plain, normalised `float` in a
writable global array, and the engine already ships a supported primitive for
setting it to an arbitrary value every frame — which it already uses on the
viewmodel, driven by a continuous 0..1 scalar.**

Four independent facts carry this:

1. Viewmodel animation time is a **normalised phase in [0,1]** stored as a `float`
   at `+0x18` (and its partner at `+0x1c`) of a 64-byte entry in a **plain global
   array at `0x2770fd8`**. [V]
2. `XAnimSetTime` at **`0x5e79c0`** writes that phase to **any caller-supplied
   float, with no clamping, no range check and no ordering constraint** in the
   engine function itself. The `[0,1]` clamp exists *only* in the script-builtin
   wrapper at `0x78b300`, not in the primitive. [V]
3. Playback **rate is already fully arbitrary and data-driven**: a per-anim `float`
   at entry `+0x30`, set verbatim from the 5th argument of `XAnimSetGoalWeight`
   (`0x4076c0` → `0x86cd20`), with no sign or range check. For a reload the game
   *computes* it as `animLengthMsec / weaponDef->reloadTime` (`0x795040`). [V]
4. **The engine already does exactly what this design asks for.** `0x795590` calls
   `XAnimSetTime` on viewmodel anim slots `0x40` and `0x41` every frame with
   `t` and `1.0 - t`, where `t` is `playerState->fWeaponPosFrac` (`ps+0x168`) —
   the ADS transition fraction. A viewmodel animation whose phase is scrubbed
   from a continuous scalar every frame is a shipping feature of this binary,
   not a hack. [V]

**One important negative result.** A **negative rate does not play the animation
backwards.** The per-frame advance (`0x86d400` / `0x86d300`) contains a guard that
refuses to commit a time that has moved backwards. Reverse scrubbing must be done
by **writing the phase** (fact 2), not by flipping the sign of the rate (fact 3).
[V for the guard; [I] for "therefore negative rate is a dead end" — see Q3.]

**Second important fact for the design.** Game-logic timing and animation playback
are **decoupled**. `ps->weaponTime` counts down from `weaponDef->reloadTime`, a
fixed integer msec from the weapon asset; the animation rate is then computed to
*fit* that duration. Nothing reads the animation's phase to decide when the reload
finishes. So scrubbing the animation does **not** by itself advance or stall the
reload — that still requires the `weaponTime` work already documented in
`reload-feasibility.md`. The two must be driven together. [V]

---

## Q1. Where does a weapon animation's time live?

### The anim entry pool

Every active animation instance is a 64-byte entry in one global array.

`0x5e79c0` (`XAnimSetTime`) is the clearest single view of it:

```
005e79c0  mov   eax, [esp+4]           ; XAnimTree* tree
005e79c4  movzx eax, word ptr [eax+4]  ; tree->rootAnimIndex (u16); 0 -> no tree
005e79cb  je    0x5e7a3e
005e79cd  mov   edx, [esp+8]           ; animIndex
005e79d6  call  0x86c5a0               ; XAnimFindEntry(animIndex, root) -> entry idx
005e79e0  je    0x5e7a3e               ; 0 -> not present
005e79f9  mov   esi, eax
005e79fb  shl   esi, 6                 ; * 0x40
005e79fe  add   esi, 0x2770fd8         ; <-- POOL BASE, STRIDE 0x40
...
005e7a15  call  ecx                    ; optional notify callback, tree->vtbl[0x48]
005e7a1a  movss xmm0, [esp+0x10]       ; the caller's float time
005e7a24  movss [esi+0x18], xmm0       ; entry.time      = t
005e7a29  mov   [esi+0x20], ax         ; entry.loops     = 0
005e7a2d  movss [esi+0x1c], xmm0       ; entry.timePrev  = t
005e7a32  mov   [esi+0x22], cx         ; entry.loopsPrev = 0
005e7a36  or    edx, -1
005e7a39  mov   [esi+0x02], dx         ; entry.dirty     = 0xffff
```

- Pool base **`0x2770fd8`**, stride **`0x40`**. [V]
- `XAnimFindEntry` = `0x86c5a0`, maps `(animIndex, treeRoot) -> entry index`. [V]

### Entry layout (offsets from the entry base)

| off | size | field (my name) | how established |
|---|---|---|---|
| `+0x02` | u16 | `dirty` — set to `0xffff` on every mutation | [V] `0x5e7a39`, `0x86d3d8`, `0x86d4ba` |
| `+0x0c` | u16 | first child entry index | [V] `0x86d2c1`, `0x86d5ec` walk it |
| `+0x0e` | u16 | sibling / next index | [V] `0x86b451` |
| `+0x10` | u16 | anim asset index | [V] `0x86d645` compares it |
| `+0x12` | u16 | non-zero ⇒ this is a *synced* node; `+0x14` is then a pointer | [V] `0x86d560`, `0x86b58f` |
| `+0x14` | u32 / u8 | union: node pointer when `+0x12 != 0`, else flag byte (`&3`, `&2`) | [V] `0x86d56d` vs `0x86d583`, `0x86d43b` |
| **`+0x18`** | **f32** | **`time` — normalised phase, 0.0 .. 1.0** | [V] read by `XAnimGetTime` `0x547690`; written by the advance `0x86d4b2` |
| **`+0x1c`** | **f32** | **`timePrev` — the value the advance adds the frame delta to** | [V] read at `0x86d41b` / `0x86d338` |
| `+0x20` | i16 | loop count | [V] `0x86d4b6` |
| `+0x22` | i16 | previous loop count | [V] `0x86d420` |
| `+0x24` | f32 | blend-time remaining | [V] `0x86cd20` writes it |
| `+0x28` | f32 | goal weight | [V] `0x86cd20` line `*(0x2771000+i) = param_2` |
| `+0x2c` | f32 | current weight | [V] `0x86cd20`, `0x86d52b` (weight 0 ⇒ entry freed) |
| **`+0x30`** | **f32** | **`rate` — playback rate multiplier** | [V] `0x86cd20` writes arg5 here; `0x86d590`/`0x86d315` multiply by it |
| `+0x34` | u8 | "weight was set instantly" flag | [V] `0x86cd20`, `0x86ca71` |

### Time is a normalised phase, not milliseconds

The constant compared against everywhere in the advance is `0xa4e394 = 1.0f`, with
`0x9e0b18 = 0.9999998f` used as the "just short of the end" epsilon and
`0x9eb234 = 0.999f`. Wrapping subtracts exactly `1.0`. [V]

```
0086d430  comiss xmm0, xmm2        ; xmm2 = 1.0f
0086d439  jb    0x86d470           ; below 1.0 -> normal
0086d460  subss xmm0, xmm2         ; wrap: newTime -= 1.0
0086d464  inc   ebp                ;       loops++
0086d465  comiss xmm0, xmm2
0086d468  jae   0x86d460
```

So **`0.0` = first frame, `1.0` = last frame, and the mod does not need to know the
animation's length in order to scrub it.** [V]

The real length is available if wanted: `XAnimGetLengthMsec` = **`0x4fa040`**:

```
004fa040  mov   eax, [esp+8]              ; animIndex
004fa044  mov   ecx, [esp+4]              ; tree
004fa048  shl   eax, 4
004fa04b  mov   eax, [eax+ecx+0x24]       ; XAnimParts* = tree->anims[idx]  (16-byte slots @ tree+0x24)
004fa04f  movzx edx, word ptr [eax+0x0e]  ; parts->numFrames
004fa053  cvtsi2ss xmm0, edx
004fa057  divss xmm0, [eax+0x30]          ; / parts->frameRate
004fa05c  mulss xmm0, [0x9f2500]          ; * 1000.0
004fa064  cvttss2si eax, xmm0
004fa068  ret                             ; -> length in msec (int)
```

- Tree anim slots: `tree + 0x24 + idx*0x10`, holding an `XAnimParts*`. [V]
- `XAnimParts`: `numFrames` u16 `@+0x0e`, `frameRate` f32 `@+0x30`. [V]

---

## Q2. Is playback rate variable? Where is it set?

**Yes, and the game itself already varies it per weapon and per animation.** [V]

### The setter

`0x4076c0` = `XAnimSetGoalWeight(XAnimTree** tree, int animIndex, float goalWeight,
float blendTime, float rate, int notifyToken, uint flags, int enableNotify,
int notifyArg)`.

Its tail calls `0x86cd20`, which stores the arguments into the entry:

```
0086cd20  ...
          *(float*)(entry + 0x28) = goalWeight   ; DAT_02771000
          *(float*)(entry + 0x30) = rate         ; DAT_02771008   <-- verbatim, no check
          *(float*)(entry + 0x24) = blendTime bookkeeping
```

There is **no clamp, no `fabs`, no `max(0, …)` and no error path on `rate`** in
either `0x4076c0` or `0x86cd20`. The only sanitising in the whole function is on
`goalWeight` (`if (goalWeight < 0.001f) goalWeight = 0;`, `0x4076c0`). [V]

Contrast with the *script* API: `setanimratecomplete` (builtin `0x802a00`) *does*
reject a negative rate —

```
"SetAnimRateComplete: must set nonnegative animRate"   @ 0x9ac2c8
```

— which is a script-layer guard, not an engine-layer one, and applies to a
different (server-entity) path. [V]

### The rate the game computes for a reload

`0x795040` = `CG_GetViewmodelAnimRate(weaponVariantDef*, XAnimTree*, animSlot)`:

```
0079508f  mov  eax, [0xb72c08 + esi*8]        ; table[slot].animField
00795096  test eax, eax
00795098  jl   0x7950a2
0079509a  mov  ecx, [ebx+8]
0079509d  mov  edi, [eax+ecx]                 ; XAnim asset handle
007950a0  jmp  0x7950b0
007950a2  mov  eax, [0xb72c0c + esi*8]        ; table[slot].timeField
007950ab  jl   0x7950d5                       ; neither -> return 1.0f
007950ad  mov  edi, [eax+ebx]                 ; DURATION IN MSEC from the weapon def
007950b0  test edi, edi
007950b2  jne  0x7950bb
007950b4  xorps xmm0, xmm0                    ; 0 -> rate 0.0  (frozen!)
007950bb  push esi
007950bc  push ebp
007950bd  call 0x4fa040                       ; XAnimGetLengthMsec(tree, slot)
007950c5  cvtsi2ss xmm1, edi                  ; desired duration
007950cb  cvtsi2ss xmm0, eax                  ; asset length
007950cf  divss xmm0, xmm1
007950d4  ret                                 ; rate = animLengthMsec / durationMsec
```

**`rate = animLength / desiredDuration`.** A rate of `1.0` means "play at the
asset's authored framerate". [V]

The table it indexes, at **`0xb72c08`**, is `{int animFieldOffset; int timeFieldOffset}`
per viewmodel anim slot, `0x40` slots. The reload rows:

| slot | source | weapon-def field |
|---|---|---|
| `0x09` | `timeField = 0x024` | `reloadTime` |
| `0x0b` | `timeField = 0x028` | `reloadEmptyTime` |
| `0x0e` | `timeField = 0x02c` | `reloadQuickTime` |
| `0x0f` | `timeField = 0x030` | `reloadQuickEmptyTime` |
| `0x13` | `timeField = 0x03c` | (weapon-def `+0x3c`) |
| `0x3e` | `timeField = 0x028` | `reloadEmptyTime` (second hand) |
| `0x3f` | `timeField = 0x024` | `reloadTime` (second hand) |

[V] — table dumped from `.data`; field names from the weapon-def parse table (below).

**Note `0x7950b4`: a duration of 0 produces `rate = 0.0f`, and the engine handles
that path deliberately.** A zero rate is a supported, in-band value. [V]

---

## Q3. The per-frame advance: what does it accept?

### The leaf advance — `0x86d400`

Reached from the dispatcher `0x86d510`:

```
0086d583  testb $0x3, [edx+0x14]     ; leaf anim?
0086d589  push  esi
0086d58a  push  ebp
0086d58b  call  0x86b580             ; XAnimGetNormalisedRate(tree, idx) = 1/lengthSeconds
0086d590  fmuls [edx+0x30]           ; * entry.rate
0086d596  fmuls [esp+0x1c]           ; * dtime (seconds)
0086d5a0  fucomip st, st(1)          ; == 0.0 ?
0086d5a8  jp    0x86d5bd
0086d5aa  ...
0086d5b0  call  0x86b4f0             ; delta == 0 -> nothing to do
0086d5c5  call  0x86d400             ; else advance by delta
```

and inside `0x86d400`:

```
0086d41b  movss xmm1, [edi+0x1c]         ; timePrev
0086d42a  addss xmm0, [esp+0x1c]         ; newTime = timePrev + delta
0086d430  comiss xmm0, xmm2 (1.0f)
0086d439  jb    0x86d470
0086d43b  testb $2, [edi+0x14]           ; non-looping?
0086d441  movss xmm0, [0x9e0b18]         ; 0.9999998f
0086d449  subss xmm1, xmm0
0086d44d  comiss xmm1, [0xa25860]        ; 0.0f
0086d454  jb    0x86d46a
0086d456  movaps xmm0, xmm2              ; clamp to exactly 1.0
0086d460  subss xmm0, xmm2               ; (looping) wrap by 1.0, loops++
...
;   ---- the monotonicity guard ----
0086d470  movswl eax, [esi+0x08]         ; entry.loops        (esi == entry+0x18)
0086d474  movss  xmm2, [esi]             ; entry.time
0086d478  movswl ecx, bp                 ; newLoops
0086d47b  sub    ecx, eax
0086d47e  cvtsi2ss xmm1, ecx             ; (newLoops - oldLoops)
0086d481  subss  xmm2, xmm0              ; entry.time - newTime
0086d485  comiss xmm2, xmm1
0086d488  ja     0x86d50a                ; BACKWARDS -> abandon, commit nothing
;   ---- commit ----
0086d4b2  movss  [esi], xmm0             ; entry.time = newTime
0086d4b6  mov    [esi+0x08], bp          ; entry.loops = newLoops
0086d4ba  mov    [edi+0x02], ax          ; dirty = 0xffff
0086d4c3  call   0x86b870                ; fire notetracks over (timePrev, newTime)
0086d4d4  ...                            ; propagate to child entries via 0x86d1f0
```

### What that means

| input | result |
|---|---|
| `rate > 0`, `dtime > 0` | normal forward playback [V] |
| `rate == 0` (or `dtime == 0`) | `delta == 0` → the advance returns before touching anything. **A clean freeze.** [V] |
| `rate` scaled by any positive factor | time advances proportionally. **Variable-speed forward playback works with no other change.** [V] |
| `rate < 0` (or `dtime < 0`) | `newTime < entry.time`, so `entry.time - newTime > 0 > (newLoops - oldLoops)`, the guard at `0x86d488` takes `ja`, and **nothing is committed** [V]. Backwards playback via a negative rate therefore does not work. [I — the arithmetic is [V]; the "therefore" assumes `entry.time` and `entry.timePrev` are in step, which `XAnimSetTime` guarantees because it writes both to the same value.] |

The synced-node variant `0x86d300` carries the identical guard at `0x86d3a9`, and
computes `delta = entry.rate(+0x30) * node->rate(+0x34) * dtime` — i.e. there is a
**second, per-node rate multiplier** in that path. [V]

The blend-node case (`0x86d5d2`) multiplies `dtime` by the node's rate and recurses
into children, so **a rate set on a parent node scales an entire subtree**. [V]

### `+0x18` vs `+0x1c` — an honest caveat

The advance reads `+0x1c` and writes `+0x18`. I did **not** find the code that
rolls `+0x18` back into `+0x1c` between frames; the only writers to `+0x1c` I
located are `XAnimSetTime` (`0x5e7a2d`), the clear-to-zero paths (`0x86bf40`,
`0x86ca5a`, `0x86d884`), the child-propagation copy (`0x86d26e`) and the rewind
helper (`0x5840e9`). **[?]** — the exact division of labour between the two fields
is unresolved.

This does **not** affect the plan, because the primitive the design would use,
`XAnimSetTime`, **writes both fields to the same value**, which is self-consistent
under either reading. It does mean you should not write `+0x18` alone by hand.

---

## Q4. `XAnimSetTime` — the scrub primitive

`0x5e79c0` (listing in Q1) is the direct write. Its signature:

```c
void XAnimSetTime(XAnimTree *tree, int animIndex, float time, int notifyArg);
```

- Writes `entry.time` **and** `entry.timePrev` to `time`, and resets both loop
  counters to 0. [V]
- Setting both to the same value makes the notetrack interval zero-width, so
  **no animation notifies fire on a scrub** — sounds and events are not spammed.
  (`0x86b870` fires notetracks strictly between `timePrev` and `time`.) [V]
- **No clamp of any kind inside the function.** [V]

The `[0,1]` clamp people may remember belongs to the *script builtin* wrapper
`setanimtime` at **`0x78b300`**, which is a different function:

```
0078b43c: if (t < 0)   { t = 0.0f;  error "must be > 0" }
          if (t > 1.0) { t = 1.0f;  error "must be < 1" }
          if (!IsTimedAnimation(...))  error "not a timed animation"
          if (t == 1.0 && IsLooping(...)) error "cannot set time 1 on looping animation"
          XAnimSetTime(tree, anim, t, -1);
```

A native mod calling `0x5e79c0` directly bypasses all of that. [V]

Companion primitives:

| address | my name | note |
|---|---|---|
| `0x547690` | `XAnimGetTime(tree, animIndex) -> float` | returns `entry.time` (`+0x18`) [V] |
| `0x4fa040` | `XAnimGetLengthMsec(tree, animIndex) -> int` | see Q1 [V] |
| `0x4076c0` | `XAnimSetGoalWeight(tree, idx, weight, blendTime, rate, …)` | see Q2 [V] |
| `0x86c5a0` | `XAnimFindEntry(animIndex, treeRoot) -> entryIdx` | [V] |
| `0x86d510` | `XAnimUpdateAnim(tree, animIdx, dtime, flags)` — recursive advance | [V] |
| `0x5de700` | `XAnimUpdate(tree, dtimeSeconds, flags)` — the per-frame entry point | [V] |
| `0x584000` | rewind/seek helper; also writes both time fields | [V] |

---

## Q5. The viewmodel path, end to end

### The viewmodel array

`0xc1c6d8` is a pointer to `cg_viewModelArray`, allocated and zeroed at `0x5334d0`:

```
00533581  imul esi, ebx, 0x34          ; numLocalClients * 0x34
00533584  push 0xa0704c                ; "cg_viewModelArray"
0053358d  call 0x674880                ; Hunk_Alloc(tag)
00533599  mov  [0xc1c6d8], eax
0053359e  call 0x965480                ; memset 0
```

**Stride `0x34` per local client.** [V]

Fields established:

| off | field | evidence |
|---|---|---|
| `+0x00` | `XAnimTree**` (passed to `XAnimUpdate`) | [V] `0x4b6986` `mov ecx,[esi]` → `0x5de700` |
| `+0x08` | `XAnimTree*` (passed to `XAnimSetTime`) | [V] `0x7951e0` uses `*(vm+8)` |
| `+0x0c` | weapon-variant def pointer used for the rate lookup | [I] `0x7951f5` `mov ebx,[eax+0xc]` then fed to `0x795040` |
| `+0x24` | cached `weapAnim` (right hand) | [V] `0x795a15` `cmp [ebx+0x24], eax` |
| `+0x28` | current anim slot (right) | [V] `0x7951e0` |
| `+0x2c` | cached `weapAnimLeft` | [V] `0x79586d` |
| `+0x30` | current anim slot (left) | [V] `0x7951e0` |

### The per-frame driver — `0x4b6870`

This is the single place the viewmodel's animation clock is advanced.

```c
// 0x4b6870  CG_UpdateViewModelAnim(localClientNum, arg)
if (cg->[0x8a368] > 8) { FUN_00463a80(...); return; }          // early-out path
weapon = CG_GetPlayerWeapon(&cg->ps);                          // 0x4540b0
...
FUN_00796a50(...);                                             // attachment / world model
FUN_00795770(localClientNum, &cg->ps, cg_viewModelArray, arg); // <-- picks the anim, sets weight+RATE
...
FUN_005de700(*cg_viewModelArray,                               // XAnimTree**
             (float)cg->frametime * 0.001f,                    // <-- THE DELTA TIME
             3);                                              // flags: 1=notify 2=notetracks
FUN_00796f40(localClientNum, &cg->ps, 1);
```

The delta-time computation, verbatim:

```
004b6970  cvtsi2ss xmm0, dword ptr [eax+0x8a344]   ; cg->frametime (msec, int)
004b6978  mulss    xmm0, [0x9abd88]                ; * 0.001f
004b6983  push     3
004b6986  mov      ecx, [esi]                      ; esi = cg_viewModelArray
004b6988  movss    [esp], xmm0
004b698d  push     ecx
004b698e  call     0x5de700                        ; XAnimUpdate
```

`0x9abd88` = `0.001f`. [V] `0x5de700` does **not** clamp or validate `dtime`. [V]

**`0x4b698e` is the single choke point for viewmodel animation time.** There is
exactly one call to `0x5de700` in the viewmodel path (the other two callers,
`0x5e3ac4` and `0x796e08`, are elsewhere). [V]

### The anim selector — `0x795770` and `0x7951e0`

`0x795770` reads `playerState->weapAnim` and dispatches:

```
00795a0f  mov   eax, [edx+0x524]        ; ps->weapAnim
00795a15  cmp   eax, [ebx+0x24]         ; vs viewmodel's cached weapAnim
00795a34  and   eax, 0xfffffbff         ; strip the toggle bit 0x400
00795a3f  cmp   eax, 0x34               ; range check: 0 .. 0x34
00795a48  jmp   dword ptr [0x7967c8 + eax*4]   ; 53-entry jump table
```

and the left hand identically off `ps+0x528` with its own 17-entry table at
`0x79679c`. [V]

Each jump-table arm ends in `0x7951e0`, which is the real worker:

```c
// 0x7951e0  CG_SetViewmodelAnim(localClientNum, XAnimTree* tree, int slot,
//                               float blendTime, int setStartTime)
weapDef = BG_GetWeaponDef(weapon);            // 0x425770
rate    = CG_GetViewmodelAnimRate(...);       // 0x795040  -> len/duration
if (setStartTime) startPhase = FUN_007950f0(...);
... sprint / ADS rate adjustments ...
for (i = 1; i < 0x40; i++) {
    if (i == slot) {
        XAnimSetGoalWeight(tree, i, 1.0f, blendTime, rate, 0, 1, 1, -1);   // 0x4076c0
        if (setStartTime)
            XAnimSetTime(*(tree+8), i, startPhase, -1);                     // 0x5e79c0
    } else {
        XAnimSetGoalWeight(tree, i, 0.0f, blendTime, 1.0f, 0, 0, 0, -1);
    }
}
```

**The engine's own "start this weapon animation" function already takes a rate and
an explicit start phase.** [V]

---

## Q6. What does `weapAnim` (ps + 0x524) actually encode?

**An animation id in the low bits, plus a restart *toggle* bit at `0x400`.** [V]

Every write in pmove has the identical shape — read the old value, keep *only* the
toggle bit, invert it, then OR in the new id:

```
0076b173  mov  eax, [esi+0x524]
0076b179  not  eax
0076b17b  and  eax, 0x400        ; keep only bit 10, flipped
0076b180  or   eax, 0xf          ; anim id 0x0f
0076b183  mov  [esi+0x524], eax
0076b189  mov  ecx, [edi+0x24]   ; weaponDef->reloadTime
0076b18c  mov  [esi+0x3c], ecx   ; ps->weaponTime = reloadTime
```

- Bit `0x400` is a **toggle**, flipped on every set. Its purpose is to make
  `weapAnim` change value even when the same animation is requested twice in a
  row, so the client restarts it. The client strips it with
  `and eax, 0xfffffbff` before dispatch (`0x795a34`) and compares the *whole*
  value against its cache (`0x795a15`) to detect the restart. [V]
- Observed ids written by pmove span `0x01 .. 0x35`, consistent with the client's
  `cmp eax, 0x34` range check. [V]
- Netfield row at `0xa5c6ec`: `{"weapAnim", 0x524, …, 0x0b, …}` — **11 bits**,
  which is exactly enough for id ≤ `0x34` plus the toggle at bit 10. [V]
- `weapAnimLeft` is `ps+0x528`. [V]

**There is no animation *time* stored anywhere near `weapAnim` in `playerState_t`.**
The neighbours are `fWeaponPosFrac 0x168`, `adsDelayTime 0x16c`,
`aimSpreadScale 0x52c`. Animation phase is not networked at all — it lives only in
the client-side XAnim pool. [V] This is good news: scrubbing it cannot desync
anything, because nothing is watching it.

---

## Q7. The reload state machine and where its duration comes from

### Duration is a fixed msec value from the weapon asset, not the anim length

Weapon-def parse table (`.data`, rows `{const char* name; int offset; int type}`,
12-byte stride, ~`0xb6e800`–`0xb6fd00`):

| offset | field |
|---|---|
| `0x024` | `reloadTime` |
| `0x028` | `reloadEmptyTime` |
| `0x02c` | `reloadQuickTime` |
| `0x030` | `reloadQuickEmptyTime` |
| `0x498` | `rechamberTime` |
| `0x49c` | `rechamberBoltTime` |
| `0x4b8` | `reloadShowRocketTime` |
| `0x4c0` | `reloadAddTime` |
| `0x4c4` | `reloadEmptyAddTime` |
| `0x4d0` | `reloadStartTime` |
| `0x4d4` | `reloadStartAddTime` |
| `0x666` | `segmentedReload` (bool) |
| `0x944` | `rechamberAnim` |
| `0x950` / `0x954` | `reloadAnim` / `reloadAnimRight` |
| `0x958` / `0xa24` | `reloadEmptyAnim` / `reloadEmptyAnimLeft` |
| `0x95c` / `0x960` | `reloadStartAnim` / `reloadEndAnim` |
| `0x964` | `reloadQuickAnim` |

[V] — all read directly from the table.

The selection is visible at `0x766126`–`0x766176`:

```
00766126  mov edx, [ebx+0x30]   ; reloadQuickEmptyTime
00766139  push 0x13
0076613d  mov edx, [ebx+0x28]   ; reloadEmptyTime
00766141  push 0x13
00766164  mov edx, [ebx+0x2c]   ; reloadQuickTime
0076616e  push 0x0f
00766176  mov edx, [ebx+0x24]   ; reloadTime
0076617a  push 0x12
```

and `0x7661fd` compares `ps->weaponTime` against `reloadTime >> 1` — the "reload is
half done" test used to credit ammo. [V]

**Nothing in this path consults the animation's phase.** The animation is fitted to
the timer (Q2), never the other way round. [V]

### The countdown

Confirmed unchanged from `reload-feasibility.md`: the sole decrement is at
`0x7696ad` inside `PM_Weapon` (`0x7694a0`), it short-circuits the whole tick when
`weaponTime == 0`, and skips all state transitions while it is `> 0`. The reload
multiplier (Speed Cola, `perk_weapReloadMultiplier` @ `0x9bcd78`) scales the
*decrement*, at `0x769691`. [V]

### Forcing a viewmodel animation from outside pmove

`0x76b0c0` is a state-driven forcer keyed on `ps->pm_flags & 0x400000` plus the
pending pair `ps+0x4f0` (weapon) / `ps+0x4f4` (target state). It sets `weapAnim`
and `weaponTime` together, and errors on an unhandled state with

```
"Trying to force viewmodel to play an animation not supported by code: %u."  @ 0xa43370
```

used at `0x76b23f`. The matching script builtin `forceviewmodelanimation` is at
**`0x7d9390`**. [V]

This is a **second, higher-level entry point** for putting the viewmodel into a
chosen animation, and it is the one the engine exposes to script.

---

## Q8. The precedent: the engine already scrubs a viewmodel animation

`0x795590`, called from `0x795836` inside `0x795770`:

```c
// 0x795590  CG_SetViewmodelBlendPose(float t, int slot)
XAnimSetGoalWeight(...);                      // 0x4076c0, three calls
XAnimSetTime(tree, 0x40, t,        -1);       // 0x5e79c0
XAnimSetTime(tree, 0x41, 1.0f - t, -1);       // 0x5e79c0
```

and the caller supplies `t`:

```
00795816  flds  [eax+0x168]     ; playerState->fWeaponPosFrac
0079581c  ...
00795836  call  0x795590
```

`ps+0x168` is `fWeaponPosFrac` (netfield table; its neighbour at `0x16c` is
`adsDelayTime`) — the **continuous 0..1 ADS transition fraction**. [V]

Slots `0x40` and `0x41` sit outside the `1..0x3f` range that `0x7951e0` sweeps, and
the `0xb72c08` table gives them no weapon-def fields — they are dedicated
"scrubbed blend pose" slots. [V]

**This is the proposed design, already shipping.** A pair of viewmodel animations
whose playback phase is written every frame from a scalar the player controls,
with weights blended between them. The only difference for VR reloads is which
scalar drives it and which slots are targeted.

---

## FEASIBILITY

### Verdict: FEASIBLE. High confidence.

| capability | verdict | confidence |
|---|---|---|
| Read the current reload animation's phase | **yes**, `XAnimGetTime(0x547690)`, returns 0..1 | high [V] |
| Freeze it | **yes**, two independent ways: rate `0.0`, or scale `dtime` to 0 at `0x4b698e` | high [V] |
| Play at variable positive speed | **yes**, per-anim rate float, or scale `dtime` | high [V] |
| Scrub to an arbitrary phase, forwards **or backwards** | **yes**, `XAnimSetTime(0x5e79c0)`, no clamp, no notify spam | high [V] |
| Play backwards by setting a negative rate | **no** — the advance's monotonicity guard drops it | high [V] for the guard |
| Drive phase from a controller scalar every frame | **yes**, and the engine already does this for ADS | high [V] |

### The hook point, concretely

**Primary (recommended): call the engine's own primitives, hook nothing.**

Everything needed is a normal `cdecl` function at a fixed address in a non-ASLR
image:

```c
// signatures established above
void  XAnimSetTime      (void *tree, int animIndex, float phase, int notifyArg); // 0x5E79C0
float XAnimGetTime      (void *tree, int animIndex);                              // 0x547690
int   XAnimGetLengthMsec(void *tree, int animIndex);                              // 0x4FA040
int   XAnimSetGoalWeight(void **tree, int animIndex, float goalWeight,
                         float blendTime, float rate, int notifyToken,
                         unsigned flags, int enableNotify, int notifyArg);         // 0x4076C0

void *cg_viewModelArray = *(void**)0xC1C6D8;      // stride 0x34 per local client
void *vmTree            = *(void**)((char*)cg_viewModelArray + 0x08);
```

Per frame, after the game has run `CG_UpdateViewModelAnim` (`0x4b6870`) — i.e. from
a hook on its return, or on the frame hook the project already owns:

```
1. slot = reload slot for this weapon      (0x09 reload, 0x0b reloadEmpty,
                                            0x0e/0x0f quick variants)
2. XAnimSetGoalWeight(&vmTree, slot, 1.0f, 0.0f, /*rate*/ 0.0f, 0,0,1,-1);
       -> rate 0 stops the engine advancing it at all  [V: 0x86d5a8 / 0x86d325]
3. phase = clamp01(f(controller pose));    // the mag's travel along its guide
4. XAnimSetTime(vmTree, slot, phase, -1);  // free scrub, both directions
```

Then, separately, hold `ps->weaponTime` per the existing
`reload-feasibility.md` plan so the *game logic* follows the player rather than the
clock, and release it (restore a real rate, let `weaponTime` run down) on
completion.

**Secondary (blunter, whole-viewmodel): patch the delta time.** One `float` on the
stack at `0x4b698e`:

```
004b6988  movss [esp], xmm0     ; <- dtime in seconds, feeding 0x5de700
```

Scaling this scales *everything* on the viewmodel tree — good for a global
slow/freeze, wrong for scrubbing one animation while idle sway keeps running.
Use only as a fallback. Note a **negative** value here will be silently dropped by
the guard, so this route cannot rewind.

**Tertiary (script-level, no native code):** `forceviewmodelanimation` (`0x7d9390`)
plus `setanimtime` (`0x78b300`) exist as script builtins. `setanimtime` clamps to
`[0,1]` and refuses looping anims — acceptable for a reload, which is one-shot.
This is a viable prototyping route before committing to native hooks.

### What still has to be worked out

- **[?] Which XAnim slot a given weapon's reload occupies at runtime.** The
  `0xb72c08` table says slot `0x09` is the one driven by `reloadTime`, and
  `weapAnim` id `0x0f` is the id pmove writes alongside `weaponTime = reloadTime`
  (`0x76b17b`/`0x76b189`) — but the `weapAnim id -> XAnim slot` mapping goes
  through the 53-entry jump table at `0x7967c8`, which I did not decode arm by
  arm. **Decode that table before writing code.** It is a bounded, mechanical job.
- **[?] `+0x18` / `+0x1c` roles** (Q3). Mitigated by only ever using
  `XAnimSetTime`, which writes both.
- **[?] Segmented reload** (`segmentedReload` @ weapon-def `0x666`, shotguns) uses
  a start/loop/end triple and the secondary countdown at `ps+0x50`. Scrubbing a
  looping middle section is a different problem; the `setanimtime` wrapper's
  "cannot set time 1 on looping animation" check hints the engine treats looping
  anims specially. Scope the first implementation to non-segmented reloads.
- **[?] Notetracks.** `XAnimSetTime` deliberately fires none (zero-width interval).
  That means **scrubbing will not play the mag-out / mag-in sounds**. If those are
  wanted, they must be driven by the mod from the phase crossing thresholds.
  This is a feature, not a bug — but it is work.
- **[?] `0x796f40`**, called right after `XAnimUpdate` in `0x4b6870`, is untraced.
  If it re-derives anything from anim time it could fight a scrub applied before
  it. Applying the scrub *after* `0x4b6870` returns avoids the question.

### What would falsify this

If someone finds that the pose sampler (`DObjCalcSkel`, `0x564ab0`) reads a *cached*
time snapshotted before `XAnimSetTime` runs, a scrub applied after `0x4b6870` would
be a frame late rather than wrong. That is a latency question, not a feasibility
one, and `reload-feasibility.md` already established the renderer reads the bone
array live from `DObj_s+0x54` at draw time.

---

## Address table

| address | my name / meaning | grade |
|---|---|---|
| `0x2770fd8` | XAnim entry pool base, stride `0x40` | V |
| `0xc1c6d8` | pointer to `cg_viewModelArray`, stride `0x34`/local client | V |
| `0xb72c08` | viewmodel anim slot → weapon-def field table, `{anim, time}` ints, `0x40` rows | V |
| `0x7967c8` | `weapAnim` id → handler jump table, 53 arms (right hand) | V |
| `0x79679c` | same for left hand, 17 arms | V |
| `0x4076c0` | `XAnimSetGoalWeight(tree, idx, weight, blendTime, **rate**, …)` | V |
| `0x86cd20` | its worker; stores rate verbatim to entry `+0x30` | V |
| `0x5e79c0` | **`XAnimSetTime(tree, idx, phase, notifyArg)` — the scrub primitive** | V |
| `0x547690` | `XAnimGetTime(tree, idx) -> float` | V |
| `0x4fa040` | `XAnimGetLengthMsec(tree, idx) -> int` | V |
| `0x86c5a0` | `XAnimFindEntry(animIdx, treeRoot) -> entryIdx` | V |
| `0x86b580` | normalised rate for an anim (`1/lengthSeconds`, weighted over children) | V |
| `0x86b870` | notetrack dispatcher over `(timePrev, time)` | V |
| `0x86d300` | advance, synced-node path (second rate at node `+0x34`) | V |
| `0x86d400` | advance, leaf path; contains the monotonicity guard at `0x86d488` | V |
| `0x86d510` | `XAnimUpdateAnim(tree, idx, dtime, flags)` recursive dispatcher | V |
| `0x86d1f0` | propagate parent time to child entries | V |
| `0x5de700` | `XAnimUpdate(tree, dtimeSeconds, flags)` | V |
| `0x4b6870` | `CG_UpdateViewModelAnim` — the viewmodel frame driver | V |
| `0x4b698e` | **the single call site advancing viewmodel anim time**; `dtime` at `[esp]` | V |
| `0x795770` | viewmodel anim selector; reads `ps->weapAnim` at `0x795a0f` | V |
| `0x7951e0` | `CG_SetViewmodelAnim(client, tree, slot, blendTime, setStartTime)` | V |
| `0x795040` | `rate = animLengthMsec / weaponDef durationMsec` | V |
| `0x7950f0` | start-phase chooser for a subset of slots | I |
| `0x795590` | **ADS blend: `XAnimSetTime(0x40, t)` / `(0x41, 1-t)` from `fWeaponPosFrac`** | V |
| `0x78b300` | script builtin `setanimtime` (clamps `[0,1]`) | V |
| `0x78b0b0` | script builtin `getanimtime` | V |
| `0x802a00` | script builtin `setanimratecomplete` (rejects negative) | V |
| `0x7d9390` | script builtin `forceviewmodelanimation` | V |
| `0x76b0c0` | pmove forced-viewmodel-anim state machine | V |
| `0x7694a0` | `PM_Weapon` | V |
| `0x7696ad` | the sole `ps->weaponTime` decrement | V |
| `0x425770` | `BG_GetWeaponDef(weaponIndex)` | V |
| `0x9abd88` | `0.001f` | V |
| `0xa4e394` | `1.0f` (end of animation) | V |
| `0x9e0b18` | `0.9999998f` (end epsilon) | V |
| `0x9f2500` | `1000.0f` | V |
| `0x9f0ed0` | `-0.0f` sign-flip mask | V |

### playerState_t offsets touched here

| offset | field | grade |
|---|---|---|
| `0x03c` / `0x044` | `weaponTime` / `weaponTimeLeft` | V (prior work, re-confirmed at `0x76b18c`) |
| `0x050` | `fadeTime` (secondary countdown used by segmented reload) | V |
| `0x144` | `weapon` (byte) | V |
| `0x158` / `0x15c` | `weaponstate` / `weaponstateLeft` | V |
| `0x168` | **`fWeaponPosFrac`** — the ADS scalar that scrubs anims `0x40`/`0x41` | V |
| `0x16c` | `adsDelayTime` | V |
| `0x4f0` / `0x4f4` | pending weapon / pending forced state | V |
| `0x524` / `0x528` | **`weapAnim` / `weapAnimLeft`** — id in low bits, toggle at `0x400`, 11 bits | V |
| `0x52c` | `aimSpreadScale` | V (prior work) |

---

# APPENDIX A — the `weapAnim id -> XAnim slot` table at `0x7967C8`, decoded

*(Second pass, BAC-279. This section closes the `[?]` loose end left at the end of
Q8 / "What still has to be worked out". Same clean-room rules: every claim below
was read out of `BlackOps.exe` itself. Nothing was executed. Grades `[V]`/`[I]`/`[?]`
as defined at the top of this document.)*

## A0. Headline answer

**It is a plain jump table, and it is a simple, static, one-to-one `id -> slot`
map.** 53 arms, `id = ps->weapAnim & ~0x400`, range-checked `> 0x34 -> default`.
Every arm is ~35 bytes of the same shape:

```
  795bf4:  mov  eax, [esp+0x3c]        ; setStartTime  (arg4 of 0x795770)
  795bf8:  push eax
  795bf9:  push ecx                    ; (scratch, immediately overwritten)
  795bfa:  fstp DWORD PTR [esp]        ; blendTime (an st(0) carried in from earlier)
  795bfd:  push 0x2c                   ; <<< THE SLOT
  795bff:  push esi                    ; XAnimTree**   (= viewmodel[0].field_00)
  795c00:  push edi                    ; localClientNum
  795c01:  mov  eax, ebp               ; <<< weaponIndex, passed in EAX (!)
  795c03:  mov  DWORD PTR [ebx+0x28], 0x2c   ; cache the slot in the viewmodel
  795c0a:  call 0x7951e0               ; CG_SetViewmodelAnim
```

Nine of the 53 arms add a "does this weapon actually have that animation?"
guard and a fallback; three add a companion *camera* animation. Those are
detailed in A3. Nothing is data-driven per weapon except those presence checks.

**The three slots the physical-reload feature needs:**

| what | XAnim slot | weapon-file field | reached by `weapAnim` id | duration source |
|---|---|---|---|---|
| **RELOAD** | **`0x09`** | `reloadAnim` | **`0x0F`** | `weaponVariantDef+0x24` `reloadTime` |
| **RELOAD EMPTY** | **`0x0B`** | `reloadEmptyAnim` | **`0x10`** | `weaponVariantDef+0x28` `reloadEmptyTime` |
| **CHARGING HANDLE / BOLT** | **`0x06`** | `rechamberAnim` | **`0x06`** | `weaponDef+0x3B4` `rechamberTime` |
| (bolt while ADS) | `0x2C` | `adsRechamberAnim` | `0x09` | — (no duration row; rate stays `1.0`) |

All `[V]`. Cross-checked three independent ways (A2).

Speed-Cola / "quick" reloads are **separate slots**, not a rate change:
`reloadQuickAnim` = slot `0x0E` (id `0x13`), `reloadQuickEmptyAnim` = slot `0x0F`
(id `0x14`). A mod that only handles slots `0x09`/`0x0B` will miss a
Speed-Cola reload entirely. `[V]`

## A1. The slot namespace: slot index == index into the weapon's `szXAnims` array

This is the fact that makes the whole table legible, and it was not established
before.

`0x7951e0` passes the slot number **verbatim** as the `animIndex` argument of
`XAnimSetGoalWeight` and `XAnimSetTime`. Therefore **the viewmodel `XAnimTree`'s
animation indices *are* these slot numbers**, `1 .. 0x41`. `[V — mechanical]`

The nine guarded arms reveal where the names live:

```
  795e6a:  mov  eax, [esp+0x10]        ; = FUN_00444740(weaponIndex)  -> WeaponVariantDef*
  795e6e:  mov  ecx, [eax+0x10]        ; wvd->szXAnims   (const char **)
  795e71:  mov  edx, [ecx+0x64]        ; szXAnims[0x19]
  795e74:  cmp  BYTE PTR [edx], 0x0    ; empty string  => weapon has no such anim
```

`0x64/4 = 0x19`, and the slot this arm plays is `0x19`. Across all nine guarded
arms the checked array index equals the played slot (two deliberate exceptions,
A3). **So `szXAnims[i]` is the asset name of tree animation `i`.** `[V]`

Anchoring that array against the weapon-asset parse table at `0xB6E6D8`
(`{const char* name; int offset; int type}`, 12-byte stride, 867 rows; `type == 0`
marks an XAnim-name field) gives the whole namespace. The anim-name rows occupy
parse offsets `0x930 .. 0xA30` contiguously at 4-byte stride, and

> **slot = (parseOffset − 0x92C) / 4**

Three independent checks that the base is `0x92C` and not `0x930`: `[V]`

1. `0x795825` gates the ADS blend on `szXAnims[0x104/4 = 0x41]`. `0x92C+0x104 =
   0xA30 = adsDownAnim`. Slot `0x41` is exactly the slot `0x795590` scrubs with
   `1.0 - fWeaponPosFrac` (Q8). With base `0x930` it would land on `hideTags`.
2. `0x7966e6` gates slot `0x39` on `szXAnims[0xE4/4 = 0x39]`. `0x92C+0xE4 = 0xA10
   = mantleCameraAnim`, and `0x795430` is the *camera*-anim setter (slots
   `0x35..0x39`).
3. The `0xB72C08` rate table (Q2) says slot `0x09` is driven by `reloadTime`,
   `0x0B` by `reloadEmptyTime`, `0x0E`/`0x0F` by the quick variants, `0x3E`/`0x3F`
   by the left-hand reload times. Base `0x92C` puts `reloadAnim`,
   `reloadEmptyAnim`, `reloadQuickAnim`, `reloadQuickEmptyAnim`,
   `reloadEmptyAnimLeft`, `reloadAnimLeft` on exactly those slots.

### The full slot namespace (66 slots, `0x00 .. 0x41`)

| slot | weapon-file field | | slot | weapon-file field |
|---|---|---|---|---|
| `0x00` | *(unused — the sweep in `0x7951e0` starts at 1)* | | `0x21` | `lowReadyOutAnim` |
| `0x01` | `idleAnim` | | `0x22` | `contFireInAnim` |
| `0x02` | `emptyIdleAnim` | | `0x23` | `contFireLoopAnim` |
| `0x03` | `fireAnim` | | `0x24` | `contFireOutAnim` |
| `0x04` | `holdFireAnim` | | `0x25` | `deployAnim` |
| `0x05` | `lastShotAnim` | | `0x26` | `breakdownAnim` |
| **`0x06`** | **`rechamberAnim`** | | `0x27` | `detonateAnim` |
| `0x07` | `meleeAnim` | | `0x28` | `nightVisionWearAnim` |
| `0x08` | `meleeChargeAnim` | | `0x29` | `nightVisionRemoveAnim` |
| **`0x09`** | **`reloadAnim`** | | `0x2A` | `adsFireAnim` |
| `0x0A` | `reloadAnimRight` | | `0x2B` | `adsLastShotAnim` |
| **`0x0B`** | **`reloadEmptyAnim`** | | **`0x2C`** | **`adsRechamberAnim`** |
| `0x0C` | `reloadStartAnim` | | `0x2D` | `dtp_in` |
| `0x0D` | `reloadEndAnim` | | `0x2E` | `dtp_loop` |
| `0x0E` | `reloadQuickAnim` | | `0x2F` | `dtp_out` |
| `0x0F` | `reloadQuickEmptyAnim` | | `0x30` | `dtp_empty_in` |
| `0x10` | `raiseAnim` | | `0x31` | `dtp_empty_loop` |
| `0x11` | `firstRaiseAnim` | | `0x32` | `dtp_empty_out` |
| `0x12` | `dropAnim` | | `0x33` | `slide_in` |
| `0x13` | `altRaiseAnim` | | `0x34` | `mantleAnim` |
| `0x14` | `altDropAnim` | | `0x35` | `sprintCameraAnim` |
| `0x15` | `quickRaiseAnim` | | `0x36` | `dtpInCameraAnim` |
| `0x16` | `quickDropAnim` | | `0x37` | `dtpLoopCameraAnim` |
| `0x17` | `emptyRaiseAnim` | | `0x38` | `dtpOutCameraAnim` |
| `0x18` | `emptyDropAnim` | | `0x39` | `mantleCameraAnim` |
| `0x19` | `sprintInAnim` | | `0x3A` | `fireAnimLeft` |
| `0x1A` | `sprintLoopAnim` | | `0x3B` | `lastShotAnimLeft` |
| `0x1B` | `sprintOutAnim` | | `0x3C` | `idleAnimLeft` |
| `0x1C` | `sprintInEmptyAnim` | | `0x3D` | `emptyIdleAnim` *(left; the parse table reuses the name)* |
| `0x1D` | `sprintLoopEmptyAnim` | | `0x3E` | `reloadEmptyAnimLeft` |
| `0x1E` | `sprintOutEmptyAnim` | | `0x3F` | `reloadAnimLeft` |
| `0x1F` | `lowReadyInAnim` | | **`0x40`** | **`adsUpAnim`** *(the scrubbed ADS pair, Q8)* |
| `0x20` | `lowReadyLoopAnim` | | **`0x41`** | **`adsDownAnim`** |

`[V]` for the whole table. `0x42` is used as a "no left-hand animation"
sentinel by the left selector (`0x7959d3`); it is not a real slot. `[V]`

## A2. The table itself

`0x795a3f`: `cmp eax,0x34` / `ja 0x79670f` / `jmp [0x7967c8 + eax*4]`. `[V]`

| id | arm | slot | animation | notes |
|---|---|---|---|---|
| `0x00` | `0x795a4f` | *(none)* | — | zeroes weight on slots `1..0x3f`, then `0x795500` picks `idleAnim`(`0x01`) or `emptyIdleAnim`(`0x02`) |
| `0x01` | `0x795a4f` | *(none)* | — | identical arm to `0x00` |
| `0x02` | `0x795aff` | `0x03` | `fireAnim` | |
| `0x03` | `0x795b22` | `0x3A` | `fireAnimLeft` | writes the **left** cache `vm+0x30` |
| `0x04` | `0x795b45` | `0x05` | `lastShotAnim` | |
| `0x05` | `0x795b68` | `0x3B` | `lastShotAnimLeft` | left cache |
| **`0x06`** | `0x795b8b` | **`0x06`** | **`rechamberAnim`** | **bolt / charging handle cycle** |
| `0x07` | `0x795bae` | `0x2A` | `adsFireAnim` | |
| `0x08` | `0x795bd1` | `0x2B` | `adsLastShotAnim` | |
| **`0x09`** | `0x795bf4` | **`0x2C`** | **`adsRechamberAnim`** | **bolt cycle while aiming** |
| `0x0A` | `0x795c17` | `0x07` | `meleeAnim` | |
| `0x0B` | `0x795c3a` | `0x08` | `meleeChargeAnim` | |
| `0x0C` | `0x795c5d` | `0x12` | `dropAnim` | |
| `0x0D` | `0x795c80` | `0x10` | `raiseAnim` | |
| `0x0E` | `0x795ca3` | `0x11` | `firstRaiseAnim` | |
| **`0x0F`** | `0x795cc6` | **`0x09`** | **`reloadAnim`** | **the reload** |
| **`0x10`** | `0x795ce9` | **`0x0B`** | **`reloadEmptyAnim`** | **the empty reload** |
| `0x11` | `0x795d0c` | `0x0C` | `reloadStartAnim` | **segmented only** |
| `0x12` | `0x795d2f` | `0x0D` | `reloadEndAnim` | **segmented only** |
| `0x13` | `0x795d52` | `0x0E` | `reloadQuickAnim` | Speed-Cola / quick |
| `0x14` | `0x795d75` | `0x0F` | `reloadQuickEmptyAnim` | Speed-Cola / quick, empty |
| `0x15` | `0x795d98` | `0x14` | `altDropAnim` | |
| `0x16` | `0x795dbb` | `0x13` | `altRaiseAnim` | |
| `0x17` | `0x795dde` | `0x16` | `quickDropAnim` | |
| `0x18` | `0x795e01` | `0x15` | `quickRaiseAnim` | |
| `0x19` | `0x795e24` | `0x18` | `emptyDropAnim` | |
| `0x1A` | `0x795e47` | `0x17` | `emptyRaiseAnim` | |
| `0x1B` | `0x795e6a` | `0x19` | `sprintInAnim` | guarded; no fallback (does nothing if absent) |
| `0x1C` | `0x795ea0` | `0x1A` | `sprintLoopAnim` | guarded on `szXAnims[0x19]`; else idle. Also starts camera slot `0x35` |
| `0x1D` | `0x795f24` | `0x1B` | `sprintOutAnim` | guarded; also **clears** camera slots `0x35..0x39` (`0x7954c0`) |
| `0x1E` | `0x795f7b` | `0x1C` | `sprintInEmptyAnim` | **falls back to `0x19`** |
| `0x1F` | `0x795fc3` | `0x1D` | `sprintLoopEmptyAnim` | **falls back to `0x1A`**; camera `0x35` |
| `0x20` | `0x79601e` | `0x1E` | `sprintOutEmptyAnim` | **falls back to `0x1B`**; camera `0x35` |
| `0x21` | `0x79611b` | `0x1F` | `lowReadyInAnim` | extra gate on global `*(u8*)0x243FDD4+0x18` |
| `0x22` | `0x79616e` | `0x20` | `lowReadyLoopAnim` | same gate |
| `0x23` | `0x7961c4` | `0x21` | `lowReadyOutAnim` | same gate |
| `0x24` | `0x7960b2` | `0x22` | `contFireInAnim` | |
| `0x25` | `0x7960d5` | `0x23` | `contFireLoopAnim` | |
| `0x26` | `0x7960f8` | `0x24` | `contFireOutAnim` | |
| `0x27` | `0x79620c` | `0x04` | `holdFireAnim` | |
| `0x28` | `0x79622f` | `0x27` | `detonateAnim` | |
| `0x29` | `0x796252` | `0x28` | `nightVisionWearAnim` | |
| `0x2A` | `0x796275` | `0x29` | `nightVisionRemoveAnim` | |
| `0x2B` | `0x796298` | `0x25` | `deployAnim` | |
| `0x2C` | `0x7962bb` | `0x26` | `breakdownAnim` | |
| `0x2D` | `0x7962de` | `0x2D` | `dtp_in` | guarded; camera `0x36` |
| `0x2E` | `0x796365` | `0x2E` | `dtp_loop` | guarded; camera `0x37` |
| `0x2F` | `0x7963ec` | `0x2F` | `dtp_out` | guarded; camera `0x38` |
| `0x30` | `0x796473` | `0x30` | `dtp_empty_in` | **falls back to `0x2D`**; camera `0x36` |
| `0x31` | `0x796505` | `0x31` | `dtp_empty_loop` | **falls back to `0x2E`**; camera `0x37` |
| `0x32` | `0x7965a2` | `0x32` | `dtp_empty_out` | **falls back to `0x2F`**; camera `0x38` |
| `0x33` | `0x79663f` | `0x33` | `slide_in` | guarded |
| `0x34` | `0x796678` | `0x34` | `mantleAnim` | guarded **and** `FUN_0056AC60(ps) != 0`; camera `0x39` |
| `>0x34` | `0x79670f` | *(none)* | — | idle + `Com_PrintWarning("WeaponRunXModelAnims: Unknown weapon animation %i\n" @0xA468F0)` |

`[V]` for every row: each was read from the arm's `push imm8` / `mov [ebx+0x28],imm32`
pair. Note slot `0x0A` (`reloadAnimRight`) is **not reachable from this table** —
no arm selects it. It is referenced only by the dual-wield helper `0x7956C0`'s
dispatch byte table. `[V]` for the absence, `[?]` for what actually drives it.

### Independent confirmation from the *writer* side (pmove)

The ids are written by `0x49C6B0(ps, id, isLeftHand)` or inline. In `0x766070`
(the reload-start chooser) the id and the duration are set together, and they
agree with the `0xB72C08` rate table on every branch — this is a genuine
end-to-end cross-check, writer → table → rate table: `[V]`

| pmove site | id set | duration written to `ps->weaponTime` | slot the table gives | `0xB72C08[slot].timeField` |
|---|---|---|---|---|
| `0x766121` | `0x14` | `wvd+0x30` `reloadQuickEmptyTime` | `0x0F` `reloadQuickEmptyAnim` | `0x030` ✓ |
| `0x766138` | `0x10` | `wvd+0x28` `reloadEmptyTime` | `0x0B` `reloadEmptyAnim` | `0x028` ✓ |
| `0x76615F` | `0x13` | `wvd+0x2C` `reloadQuickTime` | `0x0E` `reloadQuickAnim` | `0x02C` ✓ |
| `0x766171` | `0x0F` | `wvd+0x24` `reloadTime` | `0x09` `reloadAnim` | `0x024` ✓ |
| `0x766327` | `0x11` | `wd+0x3EC` `reloadStartTime` | `0x0C` `reloadStartAnim` | *(anim-side row)* ✓ |
| `0x768E29`, `0x768F89` | `0x12` | `wd+0x3F4` `reloadEndTime` | `0x0D` `reloadEndAnim` | *(anim-side row)* ✓ |

**Correction to Q7 of this document.** The earlier excerpt of `0x766126`–`0x76617A`
interleaved two different `push`es. The `push 0x13` / `push 0x12` at `0x76612C`,
`0x766143`, `0x76617C` are the **`weaponstate`** argument to `0x4A1D90`, *not*
`weapAnim` ids. The `weapAnim` ids are the earlier `push`es feeding `0x49C6B0`
(`0x14`, `0x10`, `0x13`, `0x0F`), as tabulated above. `[V]`

### The left-hand table at `0x79679C` is not 17 arms

It is **6 arms plus a 17-byte index table** at `0x7967B4`:
`0x795888`: `cmp eax,0x10 / ja` → `movzx edx, [0x7967B4 + id]` → `jmp [0x79679C + edx*4]`. `[V]`

Index bytes: `00 00 01 05 02 05 05 05 05 05 05 05 05 05 05 03 04`

| left id | arm | slot |
|---|---|---|
| `0x00`, `0x01` | `0x7958AA` | sweeps `0x3A..0x3F` to zero, then `0x3C` `idleAnimLeft` |
| `0x02` | `0x79594B` | `0x3A` `fireAnimLeft` |
| `0x04` | `0x7959B1` | `0x3B` `lastShotAnimLeft` |
| **`0x0F`** | `0x79596D` | **`0x3F` `reloadAnimLeft`** |
| **`0x10`** | `0x79598F` | **`0x3E` `reloadEmptyAnimLeft`** |
| `0x03`, `0x05..0x0E`, `>0x10` | `0x7959D3` | sentinel `0x42` = "no left animation" |

**The id space is shared between hands** — `0x0F` is "reload" and `0x10` is
"reload empty" on both sides. `[V]`

## A3. The two apparent off-by-ones are not bugs

`id 0x1C` checks `szXAnims[0x19]` (`sprintInAnim`) but plays slot `0x1A`
(`sprintLoopAnim`); `id 0x1F` checks `szXAnims[0x1C]` (`sprintInEmptyAnim`) but
plays `0x1D` (`sprintLoopEmptyAnim`). In both cases the *"in"* animation is used
as the presence sentinel for the whole set. Every other guarded arm checks the
slot it plays. `[V]` for the reads; `[I]` for "sentinel" as the intent.

## A4. How a mod addresses the right slot at runtime

### Recommended: read the slot the engine just chose (no table needed)

Every arm caches its result before calling `0x7951E0`:

```
  795cd5:  mov DWORD PTR [ebx+0x28], 0x09     ; ebx = &cg_viewModelArray[client]
```

so after `CG_UpdateViewModelAnim` (`0x4B6870`) has run for the frame:

```c
char *vm     = (char*)(*(void**)0x00C1C6D8) + 0x34 * localClientNum;
int   slotR  = *(int*)(vm + 0x28);   // right-hand slot the engine is playing
int   slotL  = *(int*)(vm + 0x30);   // left-hand slot  (0x42 == none)
```

`[V]` — written unconditionally by every arm; `-1` is written by the
"start from scratch" paths, `0x42` by the left sentinel. This is robust against
the fallbacks in A2 (e.g. it tells you the weapon fell back from
`dtp_empty_in` to `dtp_in`), which a static id→slot lookup cannot.

### Alternative: static lookup

```c
/* index by (ps->weapAnim & ~0x400); -1 == "no single slot" */
static const signed char kWeapAnimSlot[0x35] = {
/*00*/ -1,   -1,   0x03, 0x3A, 0x05, 0x3B, 0x06, 0x2A,
/*08*/ 0x2B, 0x2C, 0x07, 0x08, 0x12, 0x10, 0x11, 0x09,
/*10*/ 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x14, 0x13, 0x16,
/*18*/ 0x15, 0x18, 0x17, 0x19, 0x1A, 0x1B, 0x1C, 0x1D,
/*20*/ 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x04,
/*28*/ 0x27, 0x28, 0x29, 0x25, 0x26, 0x2D, 0x2E, 0x2F,
/*30*/ 0x30, 0x31, 0x32, 0x33, 0x34,
};
/* ids 0x1E,0x1F,0x20,0x30,0x31,0x32 may fall back to 0x19,0x1A,0x1B,0x2D,0x2E,0x2F */
```

To resolve a fallback yourself, or to test whether a weapon has an animation at all:

```c
void       *wvd      = ((void**)0x00BE19A8)[weaponIndex];      /* == FUN_00444740 */
const char **szXAnims = *(const char***)((char*)wvd + 0x10);
int haveIt = szXAnims[slot] && szXAnims[slot][0];
```

`[V]` for `0x00BE19A8` (`0x444740` is literally `return (&DAT_00BE19A8)[i];`) and
for `wvd+0x10`.

### Tree handles

```c
char       *vm   = (char*)(*(void**)0x00C1C6D8) + 0x34*client;
void      **hnd  = *(void***)(vm + 0x00);   /* XAnimTree**  -> XAnimSetGoalWeight  */
void       *tree = *hnd;                    /* XAnimTree*   -> XAnimSetTime/GetTime */
```

`0x5F4600` is exactly `return *param_1;`, and `0x795590` (the shipping ADS
scrub) uses precisely this pair. `[V]`
`vm+0x08` holds an `XAnimTree*` used by `0x7951E0` for its own `XAnimSetTime`
call; `vm+0x0C` holds the `XAnimTree*` passed as the `tree` argument of
`XAnimGetLengthMsec` by `0x795040`/`0x7950F0`. Whether `+0x08`, `+0x0C` and `*hnd`
are the same pointer is **`[?]`** — use `*hnd`, which is what the engine's own
scrub path uses.

> **Correction to Q5.** `vm+0x0C` is *not* "the weapon-variant def pointer used
> for the rate lookup". It is an `XAnimTree*`. The variant def comes from
> `FUN_00444740(weaponIndex)`. `[V]`

### Calling-convention warning

These are **not plain `cdecl`**; MSVC gave them register arguments:

| function | register args | stack args |
|---|---|---|
| `0x7951E0` `CG_SetViewmodelAnim` | **`EAX` = weaponIndex** | `(localClientNum, XAnimTree** , slot, float blendTime, int setStartTime)` |
| `0x795040` rate | **`ESI` = slot** | `(WeaponVariantDef*, XAnimTree*)` |
| `0x7950F0` start phase | **`EAX` = slot, `EDX` = WeaponVariantDef*** | — |
| `0x795430` camera-anim set | **`EAX` = weaponIndex, `ECX`/`EDI` scratch** | `(localClientNum, XAnimTree**, …)` |

`[V]` — read off the call sites. Calling `0x7951E0` from C without setting `EAX`
will read a garbage weapon index. Prefer the raw XAnim primitives (Q4).

## A5. New, and directly useful: the engine already derives phase from `weaponTime`

`0x7950F0` — invoked by `0x7951E0` whenever its `setStartTime` argument is
non-zero, and its result is handed straight to `XAnimSetTime` — is:

```
  7950f0:  mov  ecx, [0x2FF5354]
  7950f7:  mov  esi, [ecx+0x8a3a0]      ; ps->weaponTime      (msec remaining)
  7950fe:  mov  edi, [ecx+0x8a3a8]      ; ps->weaponTimeLeft
  795104:  lea  ecx, [eax-6]
  795107:  cmp  ecx, 0x39 / ja 0x795191 ; -> 0.0f for slots outside 6..0x3F
  795110:  movzx ecx, [ecx+0x7951a4] / jmp [0x795198 + ecx*4]
  ...
  79513f:  mov  ecx, eax                ; eax = the slot's duration in msec
  795141:  sub  ecx, esi                ;      duration - remaining
  795144:  cvtsi2ss xmm0, ecx
  795148:  cvtsi2ss xmm1, eax
  79514c:  divss xmm0, xmm1             ; -> (duration - weaponTime) / duration
```

**`startPhase = (durationMsec − ps->weaponTime) / durationMsec`.** `[V]`

- Applies only to slots `{0x06, 0x09..0x18, 0x2C}` (right, using `weaponTime`)
  and `{0x3E, 0x3F}` (left, using `weaponTimeLeft`). Everything else returns
  `0.0f`; a slot with no duration field returns `1.0f`. `[V]`
- `ps` sits at `cg + 0x8A364`: `0x8A3A0 − 0x8A364 = 0x3C` = `weaponTime`,
  `0x8A3A8 − 0x8A364 = 0x44` = `weaponTimeLeft`, and `cg+0x8A368` (used at
  `0x4B6870`) is `ps+0x04`, the same field pmove tests with `cmp [esi+4],9`
  before every `weapAnim` write. `[V]`

Two consequences for BAC-279:

1. **`phase ↔ weaponTime` is the engine's own relation, and it is exactly the
   linear one.** A mod that drives `ps->weaponTime` from a controller scalar and
   lets the engine re-derive the phase is working *with* the design, not against
   it. The linear map is authoritative, not a guess.
2. **`0x7951E0` will overwrite a scrub** if it is called with `setStartTime != 0`
   on the same frame. It only calls `XAnimSetTime` when the requested slot
   changes or the restart toggle flips, so this matters at reload *start*, but a
   scrub must be applied after `0x4B6870` returns (as Q8 already recommends), not
   before.

## A6. Segmented (shotgun-style) reloads — what is actually different

`segmentedReload` is `weaponDef + 0x582` at runtime (parse-table row `0x666`
minus the `0xE4` bias that applies to `WeaponDef` rows). The sequence, from
`0x76630D` onward: `[V]`

```
if (weapDef->segmentedReload && weapDef->reloadStartTime /* wd+0x3EC */) {
    weapAnim      = 0x11;                 // slot 0x0C reloadStartAnim
    weaponTime    = wd->reloadStartTime;
    weaponstate   = 0x0D;
}
   ... then, once per shell, the ordinary chooser 0x766070 runs again:
    weapAnim      = 0x0F / 0x10 / 0x13 / 0x14   // slots 0x09 / 0x0B / 0x0E / 0x0F
    weaponTime    = reloadTime / reloadEmptyTime / quick variants
   ... and at the end (0x768E08 / 0x768F72):
if (weapDef->reloadEndTime /* wd+0x3F4 */) {
    weapAnim      = 0x12;                 // slot 0x0D reloadEndAnim
    weaponTime    = wd->reloadEndTime;
    weaponstate   = 0x0F;
}
```

So a segmented reload is **not** a looping animation. It is
`reloadStartAnim` → *N* fresh instances of the **same** `reloadAnim` slot `0x09`
(one per shell, each announced by flipping the `0x400` toggle so the client
restarts it) → `reloadEndAnim`. `[V]`

That is better news than Q8's "scrubbing a looping middle section" worry: each
shell is an ordinary one-shot on slot `0x09` and is individually scrubbable by
the same code path as a magazine reload. The extra work is (a) noticing the
toggle flip to know a new shell started, and (b) that `weaponTime` is reloaded
per shell rather than once. **The "different problem" is bookkeeping, not a
different animation mechanism.** `[I]` — the mechanism is `[V]`; "therefore it is
easy" is a judgement.

**`[?]` Which weapons set `segmentedReload`.** That lives in the weapon assets
inside the fastfiles, not in the executable. Not determinable here.

## A7. Structure offsets used above, and the `0xE4` bias explained

The 867-row parse table at `0xB6E6D8` describes **two** structures, which is why
some offsets appear twice with unrelated names (e.g. `0x24` is both `reloadTime`
and `flameVar_streamChunkDistSwayVelMax`). `[V]`

- `WeaponVariantDef *wvd = ((void**)0x00BE19A8)[weaponIndex]` (`= FUN_00444740`).
  Its rows use the parse offset **directly**: `wvd+0x24 reloadTime`,
  `+0x28 reloadEmptyTime`, `+0x2C reloadQuickTime`, `+0x30 reloadQuickEmptyTime`,
  `+0x3C altRaiseTime`. `[V]`
- `WeaponDef *wd = *(void**)(wvd + 0x08)`, which is the same pointer
  `BG_GetWeaponDef` / `0x425770` returns. Its rows are the parse offset
  **minus `0xE4`**: `wd+0x3B4 rechamberTime` (row `0x498`),
  `wd+0x3EC reloadStartTime` (row `0x4D0`), `wd+0x3F4 reloadEndTime` (row `0x4D8`),
  `wd+0x582 segmentedReload` (row `0x666`). `[V]` — established by
  `0x795040`, which adds the `0xB72C08` *animField* offsets to `*(wvd+8)` and the
  *timeField* offsets to `wvd` itself, and confirmed at `0x76630D` /
  `0x766378` where `[ebx+0x582]` and `[ebx+0x56A]` share a base.
- `const char **szXAnims = *(const char***)(wvd + 0x10)`, indexed by slot. `[V]`
  Whether this array is a per-variant override allocation or simply points into
  `wd` is **`[?]`** and does not matter — the expression above is what the engine
  evaluates.

> The claim in the task brief that "field-table offsets are `0xE4` larger than
> those used off `Weapon_GetDef 0x425770`" is **backwards**: the bias applies to
> the `WeaponDef` rows (the struct `0x425770` returns), and does **not** apply to
> the `WeaponVariantDef` rows (`reloadTime` and friends), which are direct. `[V]`

## A8. What is still not established

- **`[?]` Whether the viewmodel tree really carries an entry for every slot at
  runtime.** Slot index == tree `animIndex` is `[V]`, but if a weapon leaves
  `szXAnims[9]` empty the tree may have no entry `9` at all —
  `XAnimFindEntry (0x86C5A0)` would return 0 and both `XAnimSetTime` and
  `XAnimGetTime` would silently no-op. **Check `szXAnims[slot][0] != 0` before
  driving a slot, and treat a `XAnimGetTime` of exactly `0.0` on an
  otherwise-active reload as "no entry", not "phase zero".**
- **`[?]` Slot `0x0A` `reloadAnimRight`.** Unreachable from either jump table.
  Only the dual-wield helper `0x7956C0`'s dispatch table mentions it.
- **`[?]` `0x7956C0`'s exact role.** It runs after the main selector when
  `weapDef+0x56A` (dual-wield) is set, sweeps left slots `0x3A..0x3F` asking
  `0x4321F0` for each one's weight, and forces `0x3C idleAnimLeft` if they are all
  zero. Traced enough to know it cannot disturb a right-hand reload; not traced
  further.
- **`[?]` The gate at `*(u8*)0x243FDD4 + 0x18`** used by the three `lowReady` ids.
- **`[?]` `FUN_0056AC60(ps)`**, the second gate on `mantleAnim` (id `0x34`).
- **`[?]` Which ids the *server* forces via `0x76B0C0` / `forceviewmodelanimation`.**
  That path writes `weapAnim` directly (`0x76B126`, `0x76B183`, `0x76B1AA`,
  `0x76B1D4`, `0x76B1FB`) with ids `0x0F`, `0x02`, `0x2A`, `0x29` among others;
  `0x76B183` writing id `0x0F` alongside `weaponTime = reloadTime` is the same
  reload the table maps to slot `0x09`. `[V]` for that one row; the rest of the
  state machine was not enumerated.
- **`[?]` `weapAnim` id `0x35`.** Written at `0x766D1B` (weaponstate `0x2F`,
  `weaponTime = 2000`), it is **out of range** of the `0x34` bound and lands on
  the default arm, which drops to idle and prints
  `"WeaponRunXModelAnims: Unknown weapon animation %i"`. Either intentional
  (a logic-only state with no viewmodel animation) or a shipped bug. Not
  resolvable statically.

## A9. Additions to the address table

| address | meaning | grade |
|---|---|---|
| `0x7967C8` | `weapAnim` → arm jump table, 53 `u32` entries, decoded in A2 | V |
| `0x79679C` | left-hand arm table, **6** `u32` entries | V |
| `0x7967B4` | left-hand `id → arm index` byte table, 17 bytes | V |
| `0x00BE19A8` | `WeaponVariantDef*` array indexed by weapon index | V |
| `0x444740` | `BG_GetWeaponVariantDef(i)` = `((void**)0xBE19A8)[i]` | V |
| `wvd+0x08` | `WeaponDef*` (same pointer `0x425770` returns) | V |
| `wvd+0x10` | `const char **szXAnims`, indexed by slot | V |
| `0x5F4600` | `return *p;` — turns the `XAnimTree**` at `vm+0x00` into `XAnimTree*` | V |
| `0x7950F0` | start phase = `(duration − ps->weaponTime) / duration`, slots 6..0x18/0x2C/0x3E/0x3F | V |
| `0x795430` | set a **camera** anim, sweeps slots `0x35..0x39` | V |
| `0x7954C0` | clear all camera anim weights, slots `0x35..0x39` | V |
| `0x795500` | idle chooser → slot `0x01 idleAnim` or `0x02 emptyIdleAnim` | V |
| `0x7956C0` | dual-wield left-idle fallback → slot `0x3C` | V |
| `0x49C6B0` | `PM_SetWeaponAnim(ps, id, isLeftHand)` — writes `ps+0x524`/`+0x528` with the `0x400` toggle | V |
| `0x4A1D90` | `PM_SetWeaponState(ps, state)` (the `push 0x12`/`0x13` values in Q7) | I |
| `0x766070` | reload chooser: picks id `0x0F`/`0x10`/`0x13`/`0x14` + matching duration | V |
| `0xA468F0` | `"WeaponRunXModelAnims: Unknown weapon animation %i\n"` | V |
| `cg+0x8A364` | `playerState_t` inside the `cg` struct at `[0x2FF5354]` | V |
