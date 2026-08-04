# Driving `tag_weapon` from a tracked controller pose

Target: `BlackOps.exe` (Steam retail, single-player), 32-bit PE, image base `0x400000`.
Derived **only** from disassembly/decompilation of this binary (objdump + Ghidra headless).
No KisakBlack, no PDBs, no source dump.

Companion to `docs/viewmodel-findings.md`, which established (VERIFIED) that the viewmodel is
one DObj whose model 0 is the viewhands and whose model 1 is the weapon, attached at
`tag_weapon`.

Every claim is tagged **VERIFIED** (read directly out of this binary — unambiguous
decompilation, string xref, or two independent code paths agreeing) or **INFERRED** (a
reading that fits the evidence but has not been proven a second way). Nothing here has been
run in-game; "VERIFIED" means verified *statically*. Several confident identifications in
this project have failed at runtime — treat §8 as the bring-up checklist, not a formality.

---

## 0. Verdict

**FEASIBLE. High confidence.**

The engine already has a first-class, per-frame, externally-driven-bone mechanism, it is used
by shipping code on several entity types, and the viewmodel DObj goes through the exact same
skeleton evaluator as everything else. There is no need to invent anything and no need to
detach the weapon into its own DObj.

* Per-bone transforms live in a **per-frame array at `dobj + 0x54`**, 32 bytes per bone,
  quaternion + translation. VERIFIED.
* They are computed **lazily, on demand, by `DObjCalcSkel`**, which has exactly two entry
  points: `0x0054A7F0` (bone-mask form) and `0x0047E180` (single-bone form). VERIFIED.
* After evaluation the transforms are **absolute in DObj/object space**, not parent-relative.
  VERIFIED. This is the fact that makes the job easy: an attached model's bones are composed
  against the *already absolute* attach bone, so rewriting a contiguous run of bones with one
  rigid delta is correct and needs no hierarchy walk.
* The engine's own external-bone API is `0x0046A4E0` /
  `0x0055F940` / `0x00426350` — "claim this bone, then write its local transform" — invoked
  from a dedicated pre-evaluation hook `0x005C7DC0` that runs inside `DObjCalcSkel`. VERIFIED.

**Recommended hook: detour `0x0054A7F0` (`DObjCalcSkel`) and `0x0047E180`, call the original,
then rewrite the weapon model's bone range in place.** Rationale and the exact recipe in §6.
The alternative — using the engine's own pre-evaluation API at `0x005C7DC0` — is described in
§7 and rejected only because it requires expressing the pose *relative to the parent hand
bone*, which is not yet known at that point in the frame.

---

## 1. The bone transform: `DObjAnimMat`, 32 bytes

**VERIFIED.**

| Offset | Type | Meaning | Evidence |
|---|---|---|---|
| `+0x00` | `float quat[4]` | rotation, component order **(x, y, z, w)** | `0x0068E5F0` builds a 3×3 axis from it |
| `+0x10` | `float trans[3]` | translation | `0x004BB2E0` returns `+0x10/0x14/0x18` as the tag origin |
| `+0x1C` | `float transWeight` | `2.0f / (x²+y²+z²+w²)` — the pre-divided "2/|q|²" factor used by every quat→matrix conversion | recomputed at the end of `0x0086A190`, `0x0086A2B0`, `0x0086A830` |

Stride is **`0x20`**, proved three ways:
* `0x00607FD0` returns `rotTransArray + boneIndex * 0x20`.
* `0x00489CF0` sizes the whole array as `*(u8*)(dobj + 0x0A) << 5` (`totalBones * 32`).
* every read/write in `0x0086A2B0` / `0x0086A830` indexes at `* 0x20`.

Component order comes out of `0x0068E5F0` (quat → 3×3 axis):
`axis[0] = 1 - w2*(q[1]² + q[2]²)` where `w2 = q[7] = transWeight` — so `q[1]`,`q[2]` are y,z,
`q[0]` is x, `q[3]` is w. VERIFIED.

`0x00544DF0` is **`AnglesToQuat`** (VERIFIED — three `fcos`/`fsin` pairs on
`angles[1]`, `angles[0]`, `angles[2]` scaled by `_DAT_009C31CC`, half-angle Euler → quat).
It writes the 4 floats at `+0x00` and is the only rotation input the engine's own bone-set
API accepts.

---

## 2. Where the transforms live, and the DObj fields that govern them

**VERIFIED.** `sizeof(DObj) == 0x7C` (the CG DObj pool at `0x02489148` has stride `0x7C`;
`0x004CF420` returns `&pool[i * 0x7C]`).

| Offset | Meaning | Evidence |
|---|---|---|
| `+0x00` | `XAnimTree *` | `0x0042A2E0` uses `*(int*)dobj` as the tree |
| `+0x04` | `u16` — index of this DObj's **bone-alias record** (see §4.3) | `0x00687530(*(u16*)(dobj+4), 0)` in `0x0045AF30`, `0x004B6160`, `0x00564AB0` |
| `+0x06` | `u16` entnum + 1 | `0x00564AB0` uses `*(u16*)(dobj+6) - 1` |
| `+0x09` | `u8 numModels` | prior work |
| `+0x0A` | `u8 totalBones` | prior work; also the skel allocation size |
| `+0x10` | `volatile int` **spinlock** | `0x00504970` acquires, `0x0054D480` releases |
| `+0x14` … `+0x24` | `u32 controlledBits[5]` — "this bone's transform was supplied externally; do not animate it" | written by `0x00429660`/`0x0061A6E0`/`0x0055F940`, read by `0x0042A2E0` and `0x005F46A0` |
| `+0x28` … `+0x38` | `u32 absoluteBits[5]` — alternate composition mode (see §4.2) | written by `0x0061A6E0`, read by `0x00564AB0` → passed as the last arg to `0x0086A2B0`/`0x0086A830` |
| `+0x3C` … `+0x4C` | `u32 calculatedBits[5]` — "this bone is already composed this frame" | `0x00455C50` tests it, `0x00564AB0` ORs into it |
| `+0x50` | `int skelFrameId` | `0x00477A70` writes, `0x005D73A0` compares |
| `+0x54` | `DObjAnimMat *bones` | `0x0059C4A0` returns it |
| `+0x58` | `float radius` (bounds, recomputed by the render frontend) | `0x00727EC0` writes `sceneEnt->dobj + 0x58` |
| `+0x70` / `+0x71` | `s8 lodLevel` / `u8 flags` | `0x00433870` |
| `+0x78` | `XModel **models` — followed immediately by `u8 attachBone[numModels]` | `0x0045AF30` reads `*(u8*)((u8*)models + m + numModels*4)` |

All bit arrays are **MSB-first**, same convention as `hidePartBits`: bone *b* is bit
`0x80000000 >> (b & 31)` of word `b >> 5`. VERIFIED.

### 2.1 The array is per-frame scratch, not persistent

**VERIFIED**, `0x005D73A0`:

```c
bool DObjSkelIsValidForFrame(DObj *dobj, int frameId) {
    if (dobj->skelFrameId /*+0x50*/ != frameId) {
        memset((u8*)dobj + 0x14, 0, 0x44);   // controlledBits, absoluteBits,
        return false;                         // calculatedBits, +0x50, +0x54
    }
    return dobj->bones /*+0x54*/ != NULL;
}
```

`0x14 + 0x44 = 0x58`, i.e. it wipes everything from `controlledBits` through the `bones`
pointer. The array itself is then allocated fresh out of a bump pool
(`0x00489CF0` → size, `0x004D97F0` → alloc, `0x00477A70` → install; failure prints
`"WARNING: CL_SKEL_MEMORY_SIZE exceeded - not calculating skeleton\n"`).

**Consequence: any override must be re-applied every frame.** There is nothing to write once.

### 2.2 XModel fields used by the skeleton

**VERIFIED** (all read by `0x0042A2E0`, `0x0045AF30`, `0x0086A830`, `0x006635A0`):

| Offset | Meaning |
|---|---|
| `+0x04` | `u8 numBones` |
| `+0x05` | `u8 numRootBones` |
| `+0x08` | `u16 *boneNames` — scriptStrings, indexed by local bone index |
| `+0x0C` | `u8 *parentOffsets` — for non-root bone *i*, `parent = i - parentOffsets[i - numRootBones]` |
| `+0x10` | `s16 (*bindQuats)[4]` — bind-pose rotation, `× 1/32768` |
| `+0x14` | `float (*baseTrans)[3]` — bind-pose translation, added during composition |
| `+lod*0x20 + 0x2C` | `u16` bone count contributing at this LOD |
| `+lod*0x20 + 0x30` | `u32 boneUsageBits[5]` for this LOD |

---

## 3. When transforms are computed each frame

**VERIFIED.** There are exactly two `DObjCalcSkel` entry points, and both have the same shape.

### 3.1 `0x0054A7F0` — mask form (this is the one the renderer uses)

```c
// u32* DObjCalcSkel(void *placementObj, DObj *dobj, u32 partBits[5])
// returns the DObjAnimMat array (or 0)
DObjLock(dobj);                                   // 0x00504970  (spin on dobj+0x10)
if (AllocOrValidateSkel(dobj, partBits, &out))    // 0x00528000
    { DObjUnlock(dobj); return out; }             //   -> nothing to do this call
ExpandPartBitsToAncestors(dobj, partBits);        // 0x004B6160  (in place)
DObjCalcSkelPreCalc(placementObj, dobj, partBits);// 0x005C7DC0  <-- EXTERNAL OVERRIDE HOOK
DObjCalcSkelInternal(dobj, partBits);             // 0x00564AB0  <-- the evaluator
DObjUnlock(dobj);                                 // 0x0054D480
return out;
```

### 3.2 `0x0047E180` — single-bone form

Identical, but `0x0045AF30` builds the mask from one bone index by walking up the parent
chain (and across model boundaries via `attachBone[]`), and it returns `void`.
This is what tag lookups go through: `0x00607FD0(placement, dobj, tagScriptString)` →
`0x0047E180` → `rotTransArray + bone*0x20`, which is how `0x004BB2E0` (tag origin/axis) and
the muzzle-flash placement at `0x0050B6F0` get their answer.

### 3.3 The viewmodel's path to `0x0054A7F0`

**VERIFIED chain, one link INFERRED:**

```
0x0050B6F0   R_AddDObjToScene(*(DObj**)0xC1C6D8, cg+0xC5E04, 1023, 0x400003, ...)
0x006BFDF0   R_AddDObjToScene -> scene-DObj array 0x03AD1F30, stride 0x84
                   sceneEnt + 0x70 = DObj*
                   sceneEnt + 0x74 = placement object          [INFERRED, see below]
0x006CA150   frontend per-DObj driver:
                   rotTrans = R_GetSceneDObjSkel(sceneEnt, &sceneDObj, 0);   // 0x00727EC0
                   if (rotTrans && CAS(sceneEnt+0x2C, 2 -> 3))
                       R_GenerateDObjSurfaces(sceneDObj, ..., rotTrans);     // 0x006C9AE0
0x00727EC0   R_GetSceneDObjSkel:
                   R_GetDObjPartBits(dobj, partBits, lodBytes);   // 0x006635A0
                   0x00727E80(sceneEnt, dobj)  ->  DObjCalcSkel(sceneEnt[0x74], dobj, partBits)
                   ... then computes the DObj bounds from the resulting bone positions,
                       writing dobj + 0x58 when they no longer fit ...
```

`sceneEnt + 0x74` as the placement object is **INFERRED** — `0x00727E80` passes
`*(void**)(param_1 + 0x74)` as `DObjCalcSkel`'s first argument and `+0x70` is the known
`DObj*`, so `+0x74` is the obvious partner, but I did not find the write.

`0x006635A0` (`R_GetDObjPartBits`, VERIFIED) builds the requested mask from **each model's
per-LOD bone-usage mask**, shifted by that model's bone offset, gated only on
`lodBytes[m] >= 0`. `hidePartBits` does **not** narrow it — hiding the arms (per
`viewmodel-findings.md` §6) does not stop their bones being computed, which is exactly what
we need since `tag_weapon` lives on the hands skeleton.

`tag_weapon` itself is a tag bone with no skinned surfaces, so it will not be in the LOD
usage mask — but `0x004B6160` then expands the mask upward from the highest requested bone,
adding every parent **and every `attachBone[m]`**, so requesting any weapon bone drags
`tag_weapon` in. VERIFIED (`0x004B6160`, the `uVar10 = attachBone[m]` branch).

**Important ordering fact:** the bounds/radius recomputation in `0x00727EC0` happens *after*
the `DObjCalcSkel` call returns. A post-hook on `0x0054A7F0` therefore lands before the
bounds are taken, so a moved weapon will not be wrongly culled. VERIFIED by address order
(`0x728056` is the `DObjCalcSkel` call; the min/max block follows it).

---

## 4. Absolute vs relative — the decisive question

**VERIFIED: the array holds parent-relative *local* transforms going in, and is converted
**in place** to **DObj-object-space absolute** transforms by the evaluator.**

`0x00564AB0` (`DObjCalcSkelInternal`) is:

```c
work = ~requested | calculatedBits;          // bones we must not touch
if (work is all-ones for all 5 words) return;
XAnimSampleToLocal(dobj, requested);         // 0x0042A2E0  -> fills LOCAL quat/trans
alias = 0x00687530(*(u16*)(dobj+4), 0);      // per-DObj bone-alias record, see 4.3
todo[i]     = ~(alias->bits[i] | work[i]);   // requested, not yet done, not an alias
absBits[i]  = dobj->absoluteBits[i];
calculatedBits |= requested;
boneOffset = 0;
for (m = 0; m < numModels; m++) {
    XModel *model = models[m];
    CopyAliasedBones(model, dobj, boneOffset);                       // 0x0086A120
    if (attachBone[m] == 0xFF)
        NormaliseRootBones(model, dobj, boneOffset, todo);           // 0x0086A190
    else
        ComposeRootBones(model, dobj, boneOffset, todo, absBits,     // 0x0086A2B0
                         attachBone[m]);       // <-- composes against bones[attachBone[m]]
    ComposeRemainingBones(model, dobj, boneOffset + model->numRootBones,
                          todo, absBits);                            // 0x0086A830
    boneOffset += model->numBones;
}
```

### 4.1 The composition itself

`0x0086A830`, for a non-root bone *i* of model *m* (VERIFIED):

```c
DObjAnimMat *b = &bones[i];
DObjAnimMat *p = &bones[i - model->parentOffsets[iLocal - numRootBones]];
b->quat  = p->quat * b->quat;                 // quaternion product
b->trans += model->baseTrans[iLocal - numRootBones];
TransformPoint(p, &b->trans);                 // 0x00478850: rotate by p->quat, add p->trans
b->transWeight = 2.0f / |b->quat|²;
```

`0x00478850` (VERIFIED) is exactly "rotate a point by a `DObjAnimMat`'s quaternion (using its
`transWeight`) and add its translation". Because the parent is **already** absolute when the
child is composed (bones are ordered parent-before-child within a model, and models are
ordered parent-model-before-attached-model), the result is absolute in DObj space.

`0x0086A190` (root model, no attach bone) does nothing but recompute `transWeight` — i.e. the
root model's root bones' local transforms *are* their absolute transforms. That is the base
case that pins down "absolute means DObj space, not world space". VERIFIED.

`0x0086A2B0` (an attached model's root bones) composes them against
`bones[attachBone[m]]` — **the tag bone in the parent model**. VERIFIED.

**This is the whole feasibility argument:** the weapon (DObj model 1) hangs off exactly one
absolute transform, `bones[attachBone[1]]` = the hands' `tag_weapon`. Everything below it is
a pure rigid chain from that transform.

### 4.2 `absoluteBits` (`dobj + 0x28`)

When a bone's bit is set here, `0x0086A2B0`/`0x0086A830` take an alternate rotation
composition that involves `bones[0]` (the DObj root bone) as well as the parent. Translation
is composed identically in both branches.

**INFERRED and unresolved.** My algebraic reading of the else-branch reduces to the same
product as the normal branch, which means I have the quaternion multiply order wrong
somewhere. The only writer is `0x0061A6E0`, whose only caller is `0x00426350` (a spin-bone
helper used for weapon counters/drums — §5). **Do not build on `absoluteBits`.** If a design
ends up wanting it, it needs its own investigation.

### 4.3 Bone aliasing (`0x00687530` records) — the engine merges duplicate tag bones

**INFERRED, but well-supported.** Each DObj has a variable-length record fetched by
`0x00687530(*(u16*)(dobj + 0x04), 0)` = `*(void**)0x03067C00 + 4 + n*0x10`:

```c
struct DObjBoneAlias {
    u32 aliasBits[5];      // bones that are copies, not computed
    struct { u8 dst1; u8 src1; } links[];   // both stored +1
};
```

* `0x0086A120` copies the whole 32-byte transform from `bones[src1-1]` to `bones[dst1-1]`.
* `0x0045AF30` / `0x004B6160`, when walking up from an aliased bone, jump to `src1-1`
  instead of the bone's structural parent.
* `0x00564AB0` excludes aliased bones from normal composition (`todo &= ~aliasBits`).

The obvious purpose is: when an attached model carries its own copy of the tag it hangs from
(a weapon XModel usually has its own `tag_weapon`), that copy is made to track the parent
model's bone instead of being animated. I could not find the code that *builds* the record,
so this is INFERRED — but it matters for us in a good way: if the weapon model has its own
`tag_weapon`, it will follow whatever we write, for free.

Note `0x005F8A90` (`DObjFindBoneByTag`) walks models in order and returns the **first**
match, so `DObjFindBoneByTag(vm, tag_weapon, &b, -1)` returns *model 0's* copy — the one that
actually drives everything. VERIFIED.

---

## 5. The engine's existing external-bone mechanism

**VERIFIED. This is a real, shipping, per-frame API — not something we are inventing.**

### 5.1 The primitives

| VA | Signature | What it does |
|---|---|---|
| `0x005F28E0` | `void (DObj*, const float trans[3], const float angles[3] /*NULL = identity*/, int bone)` | writes `bones[bone].quat = AnglesToQuat(angles)`, `.trans = trans`, `.transWeight = 0` — a **local** transform |
| `0x00429660` | `int (DObj*, const u32 partBits[5], int bone)` | claims the bone: fails if not in `partBits` or already in `controlledBits`; otherwise sets `controlledBits` and returns 1 |
| `0x0061A6E0` | `int (DObj*, const u32 partBits[5], int bone)` | same, and additionally sets `absoluteBits` |
| `0x0046A4E0` | `int (DObj*, const u32 partBits[5], int bone, const float trans[3], const float angles[3])` | `0x00429660` + `0x005F28E0`. **The clean public setter.** |
| `0x0055F940` | same args as `0x0046A4E0` | identical, with the `bone < 0xFE` guard inlined |
| `0x00426350` | `void (DObj*, const u32 partBits[5], int bone, const float angles[3])` | `0x0061A6E0` + `0x005F28E0` with `trans = vec3_origin` (`0x00A5E370`) — the `absoluteBits` variant |

`0x005F28E0` sets `transWeight = 0` because the compose pass recomputes it. That is the
tell that these are meant to be called **before** `0x00564AB0`, i.e. from inside
`0x005C7DC0`.

### 5.2 The hook the engine calls them from — `0x005C7DC0`

**VERIFIED.** Runs inside both `DObjCalcSkel` entry points, after the skel memory exists and
the part bits are expanded, before the evaluator:

```c
// void DObjCalcSkelPreCalc(void *placementObj, DObj *dobj, u32 partBits[5])
DObjGetControlledBits(dobj, snapshot);                  // 0x005F46A0 -> dobj+0x14
if (placementObj->type /*+0x02*/ != 0x14) {
    ... per-entity attached-part / FX bone overrides via 0x005FC1A0 ...
}
switch (placementObj->type) {
  case 0x01:            0x00782200(partBits);   break;  // many 0x0055F940/0x005F28E0 writes
  case 0x0B:            0x00782270(dobj);       break;  // three 0x00426350 spin bones
  case 0x0D: case 0x0F: 0x007823E0(partBits);   break;  // three 0x00426350 spin bones
  case 0x10: case 0x12: 0x004264E0(...);        break;
}
if (placementObj[+0x14]) 0x0059A150(placementObj, dobj, partBits);  // per-piece bone offsets
0x00782E90(placementObj, dobj, snapshot);                           // ragdoll world-space roots
if (placementObj[+0x05] && placementObj[+0x08] > 0) 0x00683800(...);
```

Real users, all VERIFIED:

* `0x00592F10` — a vehicle-ish rig: nine `0x0046A4E0` calls driving named bone indices from
  16-bit network fields scaled by `_DAT_009CDEC4` (short → degrees).
* `0x005D0810` — drives bones found by tag, including `tag_flash` (`0x023A566E`), with
  `0x0055F940` and `0x00426350`. **This is a weapon-model bone driver.**
* `0x00797650` — in the CG weapon module (`0x797xxx`), `DObjFindBoneByTag` + `0x0055F940`,
  writing one axis of a translation from a per-weapon table. Reached from `0x0046DB20`,
  which gates on `weapDef->weapType == 6`.
* `0x00662A30` — rotates a bone found by tag (`0x023A5906`) over 500 ms via `0x00426350`;
  an ammo-counter / drum animation.

### 5.3 How an override survives the animation

**VERIFIED**, `0x0042A2E0` (the XAnim → local-transform sampler, called first by the
evaluator):

```c
skip[i] = ~requested[i] | controlledBits[i];     // read BEFORE the OR below
if (skip is all-ones) return;
controlledBits[i] |= requested[i];               // everything requested is now "controlled"
if (dobj->animTree) XAnimSample(...);            // 0x00873F40, writes into dobj->bones
for each bone b:
    if (!skip[b]) {                              // requested and NOT already controlled
        bones[b].quat  = root ? identity : unpack(model->bindQuats[b]);
        bones[b].trans = 0;
    }
```

So a bone whose `controlledBits` bit was set by `0x005C7DC0` is left exactly as written.
That is the contract. VERIFIED.

### 5.4 There is no script-level path

Searched the string table: `gettagorigin`, `gettagangles`, `linkto`, `playerlinkto`,
`playerlinktoabsolute`, `playerlinktodelta`, `linktocamera`, `enablelinkto` — all read-only
or whole-entity attachment. **No GSC function writes a bone transform.** VERIFIED (absence of
evidence, so: no `setbone*`, no `bonecontroller`, no `boneangles` string exists in the
binary).

There is also a **per-entity-type callback table at `0x00B75644`, stride `0x30`**, invoked as
`(*fn)(entity, partBits)` immediately before `0x00564AB0` on the game-side skel paths
`0x004BE720` and `0x00502B70` (VERIFIED). This is a second, coarser dispatch of the same
idea. It is *not* on the client viewmodel path, so it is not the lever here, but it is worth
knowing about if the design ever moves server-side.

---

## 6. Recommended hook and recipe

**Hook `0x0054A7F0` (`DObjCalcSkel`, mask form) and `0x0047E180` (single-bone form), as
post-hooks: call the original, then fix up the weapon's bones.**

Why this and not the engine's own `0x005C7DC0` route:

* After the original returns, every bone in the DObj that was requested is **absolute in DObj
  space**. A controller pose is naturally absolute too. The transform between them is one
  rigid delta and no hierarchy reasoning is required.
* `0x005F28E0` writes a *parent-relative* transform. To use it you must know
  `bones[parent(tag_weapon)]` — a wrist bone of the hands rig — but at `0x005C7DC0` time that
  bone has not been composed yet, and you cannot call `DObjCalcSkel` re-entrantly because the
  `dobj + 0x10` spinlock is already held (`0x00504970` would deadlock). See §7 for the
  one-frame-lag workaround if this route is ever preferred.
* Post-hooking is **idempotent**, which matters because `DObjCalcSkel` is called more than
  once per frame per DObj (the render frontend, plus every tag lookup). See below.
* `0x0054A7F0` is plain cdecl with three stack arguments and returns the bone array — an easy
  detour. `0x00727E80`, the wrapper immediately above it, passes arguments in registers, so
  hook the callee, not the wrapper.

### 6.1 Sketch (documentation only — no code written, nothing built)

```c
typedef struct { float quat[4]; float trans[3]; float transWeight; } DObjAnimMat;  // 0x20

// post-hook on DObjCalcSkel(placement, dobj, partBits) and on 0x0047E180
static void VR_DriveWeaponBone(void *dobj)
{
    if (dobj != *(void**)0xC1C6D8) return;                 // viewmodel DObj only
    DObjAnimMat *bones = *(DObjAnimMat**)((char*)dobj + 0x54);
    if (!bones) return;                                    // skel alloc failed this frame

    unsigned char  numModels = *((unsigned char*)dobj + 0x09);
    void         **models    = *(void***)((char*)dobj + 0x78);
    unsigned char *attach    = (unsigned char*)(models + numModels);
    if (numModels < 2) return;

    unsigned b   = attach[1];                              // = tag_weapon's flat bone index
    unsigned off = *((unsigned char*)models[0] + 4);       // bone offset of model 1
    unsigned cnt = *((unsigned char*)models[1] + 4);       // numBones of model 1
    if (b == 0xFF || b >= *((unsigned char*)dobj + 0x0A)) return;

    DObjAnimMat desired;                                   // controller pose, DObj space
    VR_GetWeaponPoseInViewmodelSpace(&desired);            // see 6.3

    DObjAnimMat delta;                                     // desired * inverse(bones[b])
    MakeDelta(&delta, &desired, &bones[b]);
    for (unsigned i = off; i < off + cnt; i++)
        ComposeInPlace(&delta, &bones[i]);                 // quat and trans
    bones[b] = desired;                                    // pins the tag itself
}
```

### 6.2 Why it is idempotent, and why later partial evaluations still come out right

* `bones[b]` is set to `desired` on the first call. On any later call in the same frame
  `bones[b]` is already `desired`, so `delta` is identity and re-applying is a no-op.
* `calculatedBits` (`dobj + 0x3C`) means a later `DObjCalcSkel` with a *superset* mask only
  composes the *new* bones. Those bones compose against parents that already carry the delta
  (either `bones[b]` itself, or a model-1 bone we already moved), so they come out correct
  without any help. VERIFIED from the structure of `0x00564AB0` + `0x0086A830`.
* `bones[b]` is never recomposed once `calculatedBits` has it. VERIFIED.

Both properties depend on setting `bones[b] = desired` exactly, not on accumulating deltas.

### 6.3 Getting into DObj space

DObj space for the viewmodel is the space of the placement object passed to
`R_AddDObjToScene`, `cg + 0xC5E04`, whose origin/axis are the viewmodel origin/axis computed
by `CG_AddViewWeapon` (`0x00677610`) and cached at `0x00C2E3E0` (origin) / `0x00C2E3D0`
(angles) — prior work, VERIFIED there. So

```
desired_dobj = inverse(viewmodelPlacement) * desired_world
```

Per `docs/motion-controls-plan.md` §10's standing lesson, prefer the value the engine hands
you: the placement object pointer is `DObjCalcSkel`'s own first argument, and its origin is at
`+0x24` and its 3×3 axis at `+0x30` (VERIFIED — that is what `0x00782E90` reads out of it).
Use those rather than the cached globals.

### 6.4 Housekeeping

* **`transWeight` must be maintained.** After writing `bones[i].quat`, set
  `bones[i].transWeight = 2.0f / (x²+y²+z²+w²)` (`2.0f` for a unit quat). Everything
  downstream — `0x0068E5F0`, `0x00478850`, the skinning matrices — divides by it implicitly.
  Leaving it at 0 collapses the model to a point.
* **`0x0047E180` must be hooked too** or the muzzle flash, laser and any other tag lookup
  will resolve against an un-moved weapon. `0x0050B6F0` places the flash at `tag_flash`
  (`0x023A566E`), which is a model-1 bone, via `0x004BB2E0` → `0x00607FD0` → `0x0047E180`.
* **Do not hold the DObj lock.** A post-hook runs after `0x0054D480` released `dobj + 0x10`;
  keep it that way.
* **Thread.** `0x006CA150` / `0x00727EC0` run on the render frontend with a CAS state machine
  on `sceneEnt + 0x2C`. Read the controller pose through whatever snapshot the existing
  per-frame hook already publishes; do not sample the tracking API from inside the detour.
* **`hidePartBits` and this mechanism are independent.** Hiding model 0 (`viewmodel-findings`
  §6) removes the arms' surfaces and leaves their bones — including `tag_weapon` — fully
  computed. The two changes compose.

---

## 7. The alternative: use the engine's own API (rejected, but viable)

Detour `0x005C7DC0`, and before calling the original:

```c
if (dobj == *(DObj**)0xC1C6D8) {
    unsigned char b = 0xFE;
    if (DObjFindBoneByTag(dobj, *(u16*)0x023A568C /*tag_weapon*/, &b, -1))   // 0x005F8A90
        DObjSetBoneTransform(dobj, partBits, b, trans, angles);              // 0x0046A4E0
}
```

This is the sanctioned path, it is one call, and `controlledBits` guarantees the animation
will not overwrite it (§5.3). The blocker is that `trans`/`angles` are **relative to
`tag_weapon`'s parent bone**, whose absolute transform is not available at that moment.

Workable fix if this route is ever preferred: a post-hook on `0x0054A7F0` caches
`bones[parent(tag_weapon)]` each frame, and the `0x005C7DC0` pre-hook uses the previous
frame's value to build the local transform. The parent is a wrist/hand bone whose motion in
*viewmodel* space is small and smooth, so one frame of lag on the correction term is
visually free. It is strictly more machinery than §6 for no benefit, but it is the option
that stays inside the engine's own idiom, and it is the one to fall back on if the post-hook
turns out to be fighting something not visible statically.

### 7.1 What was rejected outright

* **Detaching the weapon into its own DObj.** Not necessary — the attachment already
  concentrates the entire weapon on one transform (§4.1). It would also cost the weapon its
  view anims, its `tag_clip`/`tag_flash` relationships to the hands rig, and the
  `hidePartBits` work already done.
* **`absoluteBits` (`0x00426350` / `0x0061A6E0`).** Semantics unresolved (§4.2), one caller,
  no way to validate statically.
* **Anything script-side.** No GSC bone writer exists (§5.4).
* **Patching `attachBone[1]` to point at a different bone.** Only re-parents; gives no
  control over the transform.

---

## 8. What is NOT proven — bring-up checklist

Everything below is cheap to settle with one logging session and should be settled before any
of it is built on.

1. **That the viewmodel DObj actually reaches `0x0054A7F0`.** The chain in §3.3 is solid but
   `sceneEnt + 0x74` is INFERRED. *Check:* log `(placement, dobj, partBits[0..4])` at
   `0x0054A7F0` for one frame and confirm `*(void**)0xC1C6D8` appears exactly once, with a
   non-null placement.
2. **That `attach[1]` really is `tag_weapon`'s flat bone index.** *Check:* compare
   `attach[1]` against `DObjFindBoneByTag(vm, *(u16*)0x023A568C, &b, -1)` — they must agree.
   If they do not, use the tag lookup and treat the model-list layout claim as wrong.
3. **The bone range of model 1.** *Check:* dump `i, DObjGetModel(vm,i) name, numBones` for
   `i = 0..numModels-1` (already on the `viewmodel-findings` §6 checklist) and confirm
   model 1 is the weapon and that `off + cnt <= totalBones`.
4. **Quaternion handedness and multiplication order.** The safest first test writes a pure
   *translation* delta (identity rotation, +10 units along one axis) and checks that the gun
   moves and the hands do not. Only then introduce rotation. Getting the quat order backwards
   is the single most likely failure and is invisible statically.
5. **`absoluteBits`** (§4.2) — unresolved, and deliberately unused.
6. **The bone-alias record** (§4.3) — INFERRED. If the weapon model has its own `tag_weapon`
   it will follow for free; if the alias table is not what I think it is, that bone will
   instead be animated independently and will visibly detach. *Check:* dump
   `*(u16*)(dobj+4)` and the first few words of `0x00687530(that, 0)` for the viewmodel.
7. **Multiple local clients / akimbo.** `0xC1C6D8` is `cg_viewModelArray`; entry stride is
   `0x34`. Akimbo puts a second weapon at `tag_weapon1` as DObj model 2 — the recipe in §6.1
   hard-codes model 1 and would leave the off-hand weapon behind.
8. **Cost.** Negligible: one quaternion multiply and one point transform per model-1 bone,
   and weapon models carry a handful of bones. No allocation, no extra evaluation.

---

## 9. Address reference

### Functions

| VA | What | Confidence |
|---|---|---|
| `0x0054A7F0` | `DObjCalcSkel(placement, dobj, partBits[5])` → bone array. **Recommended hook.** | VERIFIED |
| `0x0047E180` | `DObjCalcSkelForBone(placement, dobj, boneIndex)`. **Also hook.** | VERIFIED |
| `0x005C7DC0` | `DObjCalcSkelPreCalc` — the engine's external-bone-override hook | VERIFIED |
| `0x00564AB0` | `DObjCalcSkelInternal(dobj, partBits)` — the evaluator | VERIFIED |
| `0x0042A2E0` | XAnim → local transforms; honours `controlledBits` | VERIFIED |
| `0x00873F40` | XAnim sampler proper (called by the above) | INFERRED |
| `0x0086A120` | copy aliased bones | VERIFIED |
| `0x0086A190` | root model's root bones: recompute `transWeight` only | VERIFIED |
| `0x0086A2B0` | attached model's root bones: compose against `bones[attachBone[m]]` | VERIFIED |
| `0x0086A830` | all other bones: compose against parent, add `baseTrans` | VERIFIED |
| `0x00478850` | transform a point by a `DObjAnimMat` | VERIFIED |
| `0x0068E5F0` | `DObjAnimMat` quaternion → 3×3 axis | VERIFIED |
| `0x00544DF0` | `AnglesToQuat(quatOut, angles)` | VERIFIED |
| `0x0046A4E0` | `DObjSetBoneTransform(dobj, partBits, bone, trans, angles)` → 0/1 | VERIFIED |
| `0x0055F940` | same, with `bone < 0xFE` guard | VERIFIED |
| `0x00426350` | `absoluteBits` variant, `trans` forced to origin | VERIFIED |
| `0x005F28E0` | raw local-transform write (no claim, no guard) | VERIFIED |
| `0x00429660` | claim bone → `controlledBits` | VERIFIED |
| `0x0061A6E0` | claim bone → `controlledBits` + `absoluteBits` | VERIFIED |
| `0x005F46A0` | read `controlledBits[5]` out of `dobj + 0x14` | VERIFIED |
| `0x00455C50` | test `calculatedBits` for one bone | VERIFIED |
| `0x0059C4A0` | `DObjGetRotTransArray(dobj)` = `*(void**)(dobj + 0x54)` | VERIFIED |
| `0x00489CF0` | skel allocation size = `totalBones * 32` | VERIFIED |
| `0x00477A70` | install skel memory + frame id | VERIFIED |
| `0x005D73A0` | per-frame validity check; `memset(dobj+0x14, 0, 0x44)` on a new frame | VERIFIED |
| `0x00528000` / `0x004C2E80` | alloc-or-reuse skel memory (mask form / bone form) | VERIFIED |
| `0x00504970` / `0x0054D480` | DObj spinlock acquire / release (`dobj + 0x10`) | VERIFIED |
| `0x004B6160` | expand partBits to ancestors + attach bones (mask form) | VERIFIED |
| `0x0045AF30` | build partBits for one bone by walking up the chain | VERIFIED |
| `0x005F8A90` | `DObjFindBoneByTag(dobj, tag, &boneIdx, modelFilter)` | VERIFIED |
| `0x00522E60` | per-model bone-name search used by the above | VERIFIED |
| `0x00607FD0` | tag → `DObjAnimMat*` (calls `0x0047E180`) | VERIFIED |
| `0x004BB2E0` | tag → origin + 3×3 axis | VERIFIED |
| `0x00727EC0` | `R_GetSceneDObjSkel` — frontend: partBits, `DObjCalcSkel`, bounds | VERIFIED |
| `0x00727E80` | thin wrapper → `DObjCalcSkel(sceneEnt[0x74], dobj, partBits)` | VERIFIED |
| `0x006635A0` | `R_GetDObjPartBits` from per-LOD bone-usage masks | VERIFIED |
| `0x006CA150` | frontend driver: `R_GetSceneDObjSkel` then `R_GenerateDObjSurfaces` | VERIFIED |
| `0x00687530` | `pool[j] + 4 + n*0x10` — fetches the bone-alias record | VERIFIED (mechanics) / INFERRED (meaning) |
| `0x0047E340` | handle → `DObj*` | INFERRED |
| `0x00592F10` | example external-bone driver (nine `0x0046A4E0` calls) | VERIFIED |
| `0x005D0810` | example weapon-model bone driver (uses `tag_flash`) | VERIFIED |
| `0x00797650` | CG weapon-module bone driver (`weapType == 6`, via `0x0046DB20`) | VERIFIED |
| `0x00662A30` | counter/drum spin bone over 500 ms | VERIFIED |
| `0x0059A150` | per-piece bone offsets from a table (`0x004BA090`) | VERIFIED |
| `0x00782E90` | ragdoll/world-space root bones → DObj space | INFERRED |
| `0x004CF420` / `0x00433870` / `0x006193E0` | DObj create wrapper / init / real init | VERIFIED / VERIFIED / INFERRED |

### Globals

| VA | What | Confidence |
|---|---|---|
| `0x00C1C6D8` | `cg_viewModelArray`; `[0] = DObj *` (the viewmodel DObj) | VERIFIED (prior work) |
| `0x023A568C` | `u16` scriptString `tag_weapon` | VERIFIED (prior work) |
| `0x023A566E` | `u16` scriptString `tag_flash` | VERIFIED (prior work) |
| `0x02489148` | CG DObj pool, stride `0x7C` | VERIFIED |
| `0x03067C00` | base of the bone-alias record pools (indexed by the 2nd arg of `0x00687530`) | VERIFIED |
| `0x03AD1F30` | scene-DObj array, stride `0x84`; `+0x70` `DObj*`, `+0x74` placement, `+0x2C` state | VERIFIED / INFERRED (`+0x74`) |
| `0x00B75644` | per-entity-type skel-override callback table, stride `0x30` | VERIFIED |
| `0x00A5E370` | `vec3_origin` | VERIFIED |
| `0x009B5860` | `2.0f` (the `transWeight` numerator) | VERIFIED |
| `0x00A4E394` | `1.0f` | VERIFIED |
| `0x009C31CC` | degrees → half-radians constant used by `AnglesToQuat` | VERIFIED |
| `0x009CDEC4` | `s16` → degrees scale used by the network-driven bone drivers | INFERRED |

---

## 10. Open questions

1. `absoluteBits` semantics (§4.2). Only matters if §6's approach is abandoned.
2. Who builds the bone-alias record and what `dobj + 0x04` indexes exactly (§4.3).
3. Who writes `sceneEnt + 0x74` (the placement object) in `R_AddDObjToScene` (§3.3).
4. `0x00873F40`, the XAnim sampler — not read. It is passed the bone array and a 30 KB
   scratch buffer; whether it independently respects `controlledBits` or relies on
   `0x0042A2E0`'s masking was not established. It does not change the recommendation (the
   post-hook runs after everything), but it does matter for §7.
5. Akimbo (DObj model 2 at `tag_weapon1`) and any NVG/clip models are untouched by this
   design. If they need to follow the hand too, each is its own attach bone and its own
   contiguous bone range — the same recipe applies per model.
