# Viewmodel composition, and how to chop the arms off the gun

Target: `BlackOps.exe` (Steam retail, single-player), 32-bit PE, image base `0x400000`.
All addresses below are the game's own VAs. Derived **only** from disassembly/decompilation
of this binary (objdump + Ghidra headless). No KisakBlack, no PDBs, no source dump.

Every claim is tagged **VERIFIED** (read directly out of this binary — string xref, field
table, or unambiguous decompilation) or **INFERRED** (a reading that fits the evidence but
has not been proven by a second, independent path). Nothing here has been run in-game yet;
"VERIFIED" means verified *statically*.

---

## 0. Short answer

**The arms are a separate XModel from the weapon, and they can be hidden without hiding the
weapon.** VERIFIED.

The first-person viewmodel is a single **DObj** (dynamic object / animated model instance)
built from a *list* of XModels:

| DObj model index | XModel | Attach tag |
|---|---|---|
| 0 | the **viewhands** model (`ps.viewmodelIndex` → configstring → `R_RegisterModel`) | none (root) |
| 1 | `weapDef->gunXModel[camoVariant]` — **the weapon** | `tag_weapon` (or `tag_knife_attach` for weapType 7) |
| 2 | akimbo second weapon, only if dual-wield | `tag_weapon1` |
| n | NVG / gasmask model, if present | `tag_gasmask` or `tag_gasmask2` |
| n | clip/attachment model, if present | `tag_clip` |

Because the gun is a *child* of the hands (it hangs off `tag_weapon`, a bone that lives in
the hands model's skeleton), you cannot simply drop model 0 from the DObj — the gun would
lose its parent bone. What you *can* do is leave model 0 in the DObj and make the renderer
skip all of its surfaces, which the engine already supports natively via the DObj's
**hide-part-bits**.

Recommended suppression point: write hide-bits for bone indices `0 .. numBones(model0)-1`
into the viewmodel DObj at `dobj + 0x5C` (5 dwords, 160 bits, MSB-first). See §6.

---

## 1. The weapon asset: `handModel` vs `gunModel` vs `worldModel`

**VERIFIED.** The weapon-asset field parse table lives in `.data` at **`0xB6E6D8`**,
`748` entries of 12 bytes each: `{ const char *name; u32 structOffset; u32 typeEnum; }`.
(The 748/12 geometry comes from the defaulting loop at `0x76C6EC`: base `0xB6E6DC`
= `&entry[0].structOffset`, `0xBB` iterations × 4 sub-entries × `0x0C` stride = `0x30`.)

Relevant entries, read straight out of the table:

| Entry VA | Field name | Offset in weapon asset | Type |
|---|---|---|---|
| `0xB6E6E4` | `AIOverlayDescription` | `0x0E4` | 0 (string) |
| `0xB6E708` | `gunModel` | `0x8EC` | `0x0A` (XModel) |
| `0xB6E714`…`0xB6E7BC` | `gunModel2` … `gunModel16` | `0x8F0` … `0x928` | `0x0A` |
| **`0xB6E7C8`** | **`handModel`** | **`0x0EC`** | **`0x0A` (XModel)** |
| `0xB6E7D4` | `hideTags` | `0xA34` | `0x28` (tag array) |
| `0xB6E7E0` | `notetrackSoundMap` | `0xA74` | `0x2A` |
| `0xB6F32C`…`0xB6F3E0` | `worldModel` … `worldModel16` | `0xAC4` … `0xB00` | `0x0A` |

So at the *asset* level the hands (`handModel`) are already a distinct XModel reference from
the gun (`gunModel1..16`, which are camo variants of the same weapon) and from the
third-person `worldModel1..16`.

**VERIFIED** — the asset initialiser at `0x76C680` wires the internal pointers:

```
0x76C680  ecx = def + 0x0E4                 ; the nested WeaponDef
          [def + 0x08] = ecx                ;  WeaponDef*
          [def + 0x10] = def + 0x92C        ;  szXAnims[]
          [def + 0x00] = ""                 ;  name default
          [def + 0x18] = def + 0xA34        ;  hideTags[0x20] (u16 scriptStrings)
          [ecx + 0x04] = def + 0x8EC        ;  XModel **gunXModel   -> gunModel[16]
          [ecx + 0x10] = def + 0xA74        ;  notetrackSoundMap
          [ecx + 0x14] = def + 0xA9C
          [ecx + 0x30C]= def + 0xAC4        ;  XModel **worldModel  -> worldModel[16]
          [ecx + 0x650]= def + 0xB04
          [ecx + 0x654]= def + 0xB80
          [ecx + 0x7BC]= def + 0xBFC
```

**INFERRED** but strongly supported: there are two accessors,

* `0x00444740(weaponIndex)` → the **outer** weapon asset (`+0x10` = `szXAnims`,
  `+0x18` = `hideTags`). Used everywhere anims and hideTags are needed.
* `0x00425770(weaponIndex)` → the **nested `WeaponDef`** = outer `+ 0x0E4`
  (so `+0x04` = `XModel **gunXModel`, `+0x08` = `XModel *handXModel`).

That mapping is what makes `*(XModel**)(ret425770 + 4)` in the DObj builder (§2) resolve to
`gunModel[]`, and it lines up with the parse table exactly (`0x0E4 + 0x04 = 0x0E8`,
`0x0E4 + 0x08 = 0x0EC` = `handModel`).

### `handModel` is apparently *not* what supplies the first-person arms in T5

**INFERRED / open.** I could not find any read of `weapDef + 0x08` (`handModel`) on the
client viewmodel path. The DObj's model 0 comes from a **configstring**, not from the
weapon asset — see §3. `handModel` looks like a carried-over CoD4-era field that T5 no
longer uses for the view hands. This does not change the conclusion (the hands are still a
separate XModel), but do not build a mod around patching `handModel`.

---

## 2. Where the viewmodel DObj is built — `0x00796A50`

**VERIFIED** (full decompilation; tag identities resolved from the scriptString init block
at `0x5EA700`–`0x5EB400`).

```c
// FUN_00796a50(localClientNum, cent, weaponIndex, camoVariant, viewhandsModel,
//              nvgModel, clipModel, flagA, flagB)
weapCompleteDef = 0x444740(weaponIndex);
weapDef         = 0x425770(weaponIndex);
if (weapDef->gunXModel[camoVariant] == NULL) return;          // no gun asset -> bail

cache = (u32*)(*(u32*)0xC1C6DC + weaponIndex * 0x24);          // per-weapon model cache
cache[0] = viewhandsModel;
*((u8*)cache + 0x0C) = camoVariant;
cache[1] = nvgModel;
cache[2] = clipModel;

models[0] = { .model = cache[0],                     .tag = 0 };                // HANDS
models[1] = { .model = weapDef->gunXModel[variant],
              .tag   = (weapDef->weapType == 7) ? SL"tag_knife_attach"
                                                : SL"tag_weapon" };             // GUN
n = 2;
if (weapDef->dualWield /* +0x56A */) {
    altDef    = 0x425770(weapDef->altWeaponIndex /* +0x594 */);
    models[2] = { .model = altDef->gunXModel[0], .tag = SL"tag_weapon1" };
    n = 3;
}
if (cache[1]) models[n++] = { cache[1], overrideNVGModelWithKnife ? SL"tag_gasmask2"
                                                                  : SL"tag_gasmask" };
if (cache[2]) models[n++] = { cache[2], SL"tag_clip" };

viewModel->dobj = DObjCreate(models, n, viewModel->animTree, ...);   // 0x004CF420
...
CG_SetWeaponHidePartBits(weapCompleteDef, viewModel, viewModel->dobj, -1);   // 0x005B0CE0
if (dualWield) CG_SetWeaponHidePartBits(altCompleteDef, viewModel, dobj, 2);
DObjSetHidePartBits(viewModel->dobj, &viewModel->partBits);                  // 0x005AE010
```

The model-list entry is 8 bytes: `{ XModel *model; u16 tagScriptString; u8 flags; u8 pad; }`
— **VERIFIED** from the stack writes at `0x796B13`–`0x796BFA` (stride 8, model at `+0`,
tag u16 at `+4`, byte at `+6`).

### Tag scriptString globals used here (VERIFIED)

Resolved by walking the `SL_GetString` init block (result of call *k* is stored by the
`mov %ax, GLOBAL` that follows call *k*):

| Global | String |
|---|---|
| `0x023A5682` | `tag_knife_attach` |
| `0x023A568C` | `tag_weapon` |
| `0x023A568E` | `tag_weapon1` |
| `0x023A566C` | `tag_clip` |
| `0x023A569C` | `tag_gasmask` |
| `0x023A569E` | `tag_gasmask2` |
| `0x023A5692` | `tag_camera` |
| `0x023A566E` | `tag_flash` |

`0x02F67C28` = `overrideNVGModelWithKnife` (bool dvar, flags `0x1000`) — VERIFIED from its
registration at `0x4A6396` / name string `0x9EC208`.

---

## 3. Where the hands model comes from — `0x0050D510`

**VERIFIED.**

```c
viewmodelIndex = cg->snap->ps[+0x188];                  // "viewmodelIndex" is a ps field
name  = CG_ConfigString(viewmodelIndex + 0x5F3);        // 0x004ABF90
model = R_RegisterModel(name);                          // 0x006BF730
for each weapon in ps->weapons[15]:
    if (cache[weap][0] != model)                        // viewhands changed?
        CG_SetupViewWeaponDObj(lc, cent, weap, variant, model, cache[1], cache[2], 1, 0);
```

So DObj model 0 is the model named by the configstring the GSC `setviewmodel()` /
`ps.viewmodelIndex` mechanism selects — i.e. the per-character **viewhands** asset. There is
a sibling `0x00527A90` that does the same rebuild when the **camo variant** changes.

Supporting strings present in the binary: `usage: setviewmodel(<model name>)`,
`viewmodelIndex`, `showviewmodel`, `hideviewmodel`, `viewmodel_`.

---

## 4. Where the viewmodel is submitted for drawing

**VERIFIED.**

```
0x00677610   CG_AddViewWeapon (outer)
             - computes viewmodel origin/axis (0x797060 / 0x797FB0 / 0x797E90),
               caches them at 0xC2E3D0..0xC2E3E8
             - computes `drawGun`:
                   drawGun = (cg[+0xA9D44] == 0);
                   if (cg[+0x0C] != 0 || !cg_drawGun->current.enabled
                       || 0x0066FA20(cg, &x))          drawGun = false;
             - calls 0x0050B6F0(lc, &originAxis, ps, cg+0x8BD7C, drawGun)

0x0050B6F0   viewmodel scene submission + muzzle flash
             if (drawGun) {
                 R_AddDObjToScene( *(DObj**)0xC1C6D8,      // <-- the viewmodel DObj
                                   cg + 0xC5E04,           // placement object
                                   0x3FF,                  // entnum 1023
                                   0x400003,               // renderFx
                                   &origin, 0,0,0, ... );  // 0x006BFDF0
                 ... muzzle flash at tag_flash (0x023A566E) ...
             }
```

`cg_drawGun` — **VERIFIED**:
* registered at `0x004A38A5`–`0x004A38B1`: `Dvar_RegisterBool("cg_drawGun", 1, 0x80, "")`
* its `dvar_t*` is stored at **`0x02F67F28`**
* flag `0x80` = **cheat-protected**. VERIFIED: the generic set path at `0x00861D9B` tests
  `dvar->flags & 0x80` (`test %al,%al; jns`) against `sv_cheats` (`0x02620BE8`) and prints
  `"%s is cheat protected."`. (`0x40` = read-only, `0x10` = write-protected.)
* `dvar_t` layout used throughout: `+0x00` name, `+0x0C` flags (byte), `+0x18` current value.

Setting `cg_drawGun 0` removes **the whole DObj** — hands *and* weapon. It is therefore the
wrong lever for this job, but it is a useful A/B switch while bringing the hook up, and a mod
DLL can poke `*(u8*)(*(u32*)0x02F67F28 + 0x18)` directly without tripping the cheat check.

`cg_drawGun` is also read at `0x004102E9` / `0x00410390` (laser/aim-assist path) and
`0x00774551`.

---

## 5. How the renderer decides which viewmodel surfaces to draw

This is the important part, and it is where the arms can be removed surgically.

### 5.1 DObj layout (VERIFIED)

| Offset | Meaning | Evidence |
|---|---|---|
| `+0x09` | `u8 numModels` | `0x006653C0` returns `*(u8*)(dobj+9)` |
| `+0x0A` | `u8 totalBones` | bounds check in `0x005F8A90` |
| `+0x5C` … `+0x6F` | **`u32 hidePartBits[5]`** (160 bits) | written by `0x005AE010`, read by `0x005B56B0` |
| `+0x78` | `XModel **models` | `0x00648670` returns `((XModel**)dobj[0x78])[i]` |

`XModel + 0x04` = `u8 numBones` (**VERIFIED**, `0x00415910`).

### 5.2 Bone indices are DObj-global and simply concatenated (VERIFIED)

`0x005F8A90(dobj, tagScriptString, u8 *boneIndexInOut, modelFilter)` walks the DObj's models
in order and accumulates `boneOffset += model->numBones` as it goes, returning a **single
flat bone index across the whole DObj**. There is no name-based bone merging.

Consequence: model 0 (the hands) owns bone indices `[0, numBones(model0))`, model 1 (the gun)
owns `[numBones(model0), …)`, and so on.

### 5.3 The hide-part-bits setter (VERIFIED)

`0x005B0CE0` = `CG_SetWeaponHidePartBits` (identified by its own error string,
`"CG_SetWeaponHidePartBits: No such bone tag (%s) for weapon (%s)\n"` at `0x9DAE28`,
pushed at `0x5B0D33`):

```c
for (i = 0; i < 0x40; i += 2) {                 // 32 u16 hideTags
    tag = weapCompleteDef->hideTags[i];         // = *(u16*)(def[0x18] + i)
    if (!tag) return;
    bone = 0xFE;
    if (DObjFindBoneByTag(dobj, tag, &bone, modelFilter))
        viewModel->partBits[bone >> 5] |= 0x80000000u >> (bone & 31);   // MSB-first
    else
        Com_Printf("CG_SetWeaponHidePartBits: No such bone tag (%s) for weapon (%s)\n", ...);
}
```

`viewModel->partBits` is at `cg_viewModelArray[lc] + 0x10`, and `0x005AE010`
(`DObjSetHidePartBits`) copies those 5 dwords to `dobj + 0x5C`.

**Bit order: MSB-first.** Bone *b* is bit `0x80000000 >> (b & 31)` of word `b >> 5`.

### 5.4 The consumer — `0x006C9AE0` (R_GenerateDObjSurfaces) (VERIFIED)

Identified by its error string `"FAILED to allocate surfs on frame %u: DObj %s needs %i surfs\n"`
(`0xB500EC`, pushed at `0x6CA07A`). Decompiled core:

```c
numModels = DObjGetNumModels(dobj);
DObjGetHidePartBits(dobj, hideBits /*5 dwords*/);          // 0x005B56B0 -> dobj+0x5C
boneOffset = 0;
for (m = 0; m < numModels; m++) {
    model = DObjGetModel(dobj, m);
    nBones = model->numBones;                              // XModel+0x04
    lod = (s8)sceneEnt[0x48 + m];                          // per-model LOD byte
    if (lod >= 0) {
        surfCount = XModelGetSurfaces(model, &surfs, lod);
        for each surface {
            surfBones[5] = surface bone-usage mask (5 dwords at surf+0x30);
            aligned[5]   = surfBones >> boneOffset;        // multi-word right shift
            if ((hideBits & aligned) == 0)  emit the surface;
            else                            emit skip marker 0xFFFFFFFD;
        }
    }
    boneOffset += nBones;
}
sceneEnt[0x68] = surf list;
```

`0x006C18C0` / `0x006C1C90` (`R_AddDObjSurfacesCamera` / `R_AddDObjSurfaces`, named by the
profile strings at `0xB4FE14` / `0xB4FE2C` pushed at `0x6C1C53` / `0x6C1F2B`) then walk that
list and honour the `0xFFFFFFFD` skip markers and the same `sceneEnt[0x48+m] < 0` per-model
gate.

So there are **two** per-model kill switches in the render frontend:

1. `hidePartBits` at `dobj + 0x5C` — per **bone**, client-thread data, set once at DObj
   setup and persistent. Any surface touching a hidden bone is dropped.
2. `sceneEnt[0x48 + modelIdx] = -1` — per **model**, render-frontend data
   (`0x03AD1F30 + i*0x84`, `+0x48` is a 32-byte per-model LOD array). I did **not** locate
   the code that fills it (**open**), so this route is not currently actionable.

---

## 6. Recommendation: how to hide the arms and keep the gun

**Mechanism: set the viewmodel DObj's hide-part-bits for every bone belonging to DObj model 0.**

Confidence: **high** for "this is the right mechanism and the right data"; **medium** for
"model 0 is always exactly the arms and nothing else" (see the risks below) — that one wants
a five-minute runtime check before you build on it.

Sketch (no code written; documentation only):

```
DObj *vm = *(DObj**)0xC1C6D8;              // cg_viewModelArray[0].dobj
if (!vm) return;
XModel *hands = ((XModel**)(*(u32*)((u8*)vm + 0x78)))[0];   // DObjGetModel(vm, 0)
unsigned n = *((u8*)hands + 4);                              // numBones of model 0
u32 *bits = (u32*)((u8*)vm + 0x5C);
for (unsigned b = 0; b < n && b < 160; b++)
    bits[b >> 5] |= 0x80000000u >> (b & 31);
```

Notes and gotchas:

* **Re-apply it.** `0x00796A50` rewrites `dobj+0x5C` wholesale from
  `cg_viewModelArray[lc]+0x10` (via `0x005AE010`) every time the DObj is rebuilt — weapon
  change, camo change, viewhands change, akimbo. Either (a) OR the mask into
  `cg_viewModelArray[lc] + 0x10` as well so it survives the rebuild, or (b) hook
  `0x005AE010` and OR the extra bits in as they are written, or (c) just re-apply every frame
  from your existing per-frame hook. (b) is the cleanest single hook.
* **160-bit ceiling.** `hidePartBits` is only 5 dwords. If the viewhands model has more than
  160 bones the tail cannot be hidden. Viewhands models are small (wrists/forearms) so this
  is very unlikely, but assert on `n <= 160`.
* **Bit order matters.** MSB-first (`0x80000000 >> (b & 31)`). Getting it backwards would
  hide the gun instead, which is a nice self-check during bring-up.
* **This does not free any work.** The surfaces are still walked; they are just replaced by
  skip markers. Cost is negligible; do not expect a perf win.
* **Attachments ride on the hands.** The NVG/gasmask model attaches to `tag_gasmask` on
  model 0 and the clip model to `tag_clip`. Hiding model 0's *bones* only drops model 0's
  own *surfaces* — the child models' surfaces reference *their own* bones (at a higher
  `boneOffset`) and keep drawing. That is what we want for the gun; if the NVG model also
  becomes undesirable in VR it needs its own bone range hidden.
* **A cheap A/B while bringing this up:** poke `*(u8*)(*(u32*)0x02F67F28 + 0x18) = 0`
  (`cg_drawGun`). If the whole viewmodel vanishes you are hooked at the right point in the
  frame; then swap to the partBits approach to keep the gun.

### Rejected alternatives

* **`cg_drawGun 0`** — removes gun and hands together (§4). Also cheat-flagged.
* **Dropping model 0 from the DObj model list in `0x00796A50`** — the gun's attach tag
  (`tag_weapon`) lives on model 0's skeleton; without it the gun has no parent bone. Also
  the view anims are authored against the hands skeleton. Do not do this.
* **Replacing the viewhands XModel** (force `R_RegisterModel` at `0x0050D510` to a
  hands-less asset) — clean in principle, but the replacement must still carry the full
  animated skeleton *and* `tag_weapon`/`tag_clip`/`tag_gasmask`, i.e. it is an authoring
  job, not a code job. Keep in the back pocket if partBits turns out to be too coarse.
* **Material-based filtering** — the surface loop in `0x006C9AE0` has the material in hand
  (`0x006C9860` builds the sort key from it) but there is no existing per-material gate to
  reuse; you would be adding one. The bone gate already exists and is free.

### Verifying "model 0 is the arms" at runtime (recommended before shipping)

`0x005E0F40(dobj)` yields a printable name used in the surf-allocation error message; and
XModel names are reachable from `DObjGetModel(vm, i)`. Dumping
`i, name(DObjGetModel(vm,i)), numBones` for `i = 0..numModels-1` on a live frame will settle
it in one line of log — expect `0: viewhands_*`, `1: viewmodel_<weapon>`.

---

## 7. Tag/bone inventory for the *next* step (attaching the weapon to a tracked hand)

The project is heading toward parenting the weapon to a real tracked hand instead of to the
view. The relevant facts:

* The gun is already an **attachment at `tag_weapon`** (VERIFIED, §2). If you can control the
  transform of the bone named `tag_weapon` in the viewmodel DObj, you control the weapon's
  pose without touching the weapon model at all. That is the natural seam for motion
  controls: hide model 0's surfaces (§6), keep its skeleton, and drive `tag_weapon`.
* Melee-type weapons (`weapDef->weapType == 7`) attach at **`tag_knife_attach`** instead.
* Akimbo second weapon attaches at **`tag_weapon1`**.
* Reading a tag off a DObj: `0x004BB2E0(placementObj, dobj, tagScriptString, outOrigin, outAxis)`
  returns non-zero on success (used at `0x0045E795` with `tag_camera`, and at `0x00513F62`
  for the generic view/world tag lookup). `0x0044A0A0` is a sibling used at `0x0052BD51`.
  Both **INFERRED** as tag-pose getters from call-site shape, not proven.
* Finding a bone index by tag name: `0x005F8A90(dobj, scriptString, &boneIdx, modelFilter)`
  (VERIFIED, §5.2). `modelFilter = -1` searches all models.

Tag names present in this binary that matter for a first-person rig (all VERIFIED as strings;
which of them actually exist on a given asset is an asset question):

`tag_weapon`, `tag_weapon1`, `tag_weapon_left`, `tag_weapon_right`, `tag_weapon_chest`,
`tag_knife_attach`, `tag_knife_fx`, `tag_bayonet`, `tag_inhand`, `tag_clip`, `tag_flash`,
`tag_flash_2`, `tag_flash_alt`, `tag_brass`, `tag_brass_2`, `tag_butt`, `tag_barrel`,
`tag_aim`, `tag_aim_animated`, `tag_aim_pitch`, `tag_laser`, `tag_camera`, `tag_eye`,
`tag_gasmask`, `tag_gasmask2`, `tag_stowed_back`, `tag_origin`, `tag_fx`.

Skeleton joint names present (third-person/character rigs, listed for completeness — the
viewhands rig is likely a subset): `j_mainroot`, `j_spinelower`, `j_spineupper`, `j_spine4`,
`j_neck`, `j_head`, `j_clavicle_le/ri`, `j_shoulder_le/ri`, `j_elbow_le/ri`,
`j_wrist_le/ri`, `j_wristtwist_le`, `j_palm_le`, `j_hip_le/ri`, `j_knee_le/ri`,
`j_ankle_le/ri`, `j_ball_le/ri`, `j_helmet`, `j_gun`-adjacent `j_barrel`, `j_counter`.

If the viewhands rig turns out to expose `j_shoulder_*` / `j_elbow_*` / `j_wrist_*`, a
*partial* chop becomes available too — hide only the bones from the elbow up and keep the
hands — by feeding those bone indices to the same partBits mask instead of the whole
`[0, numBones(model0))` range. Same mechanism, finer selection. **INFERRED**; depends
entirely on how the viewhands assets are skinned (a surface skinned across both the kept and
hidden bones will be dropped entirely, because the test is "hidden if *any* used bone is
hidden").

There is also a built-in, asset-driven version of exactly this: the weapon file's
**`hideTags`** (up to 32 tag names, offset `0xA34`) is applied through the same partBits
path (§5.3), and there is a console command `useweaponhidetags <weaponName>` plus a
`useOnlyAltWeaoponHideTagsInAltMode` flag (both VERIFIED as strings). That is how the game
hides attachment geometry today, and it confirms the intended usage of the mechanism we are
borrowing.

---

## 8. Address / global reference

### Functions

| VA | What | Confidence |
|---|---|---|
| `0x00677610` | `CG_AddViewWeapon` outer: viewmodel placement + `drawGun` decision | VERIFIED |
| `0x0050B6F0` | viewmodel scene submission (`R_AddDObjToScene`) + muzzle flash | VERIFIED |
| `0x006BFDF0` | `R_AddDObjToScene` — fills scene-DObj array `0x03AD1F30`, stride `0x84` | VERIFIED |
| `0x00796A50` | `CG_SetupViewWeaponDObj` — builds the viewmodel DObj model list | VERIFIED |
| `0x004CF420` | DObj create wrapper (`models[], numModels, animTree, …`) | VERIFIED |
| `0x005B0CE0` | `CG_SetWeaponHidePartBits` (named by its own error string) | VERIFIED |
| `0x005AE010` | `DObjSetHidePartBits(dobj, u32 bits[5])` → `dobj+0x5C` | VERIFIED |
| `0x005B56B0` | `DObjGetHidePartBits(dobj, u32 out[5])` | VERIFIED |
| `0x006C9AE0` | `R_GenerateDObjSurfaces` — LOD gate + partBits gate (named by error string) | VERIFIED |
| `0x006C18C0` | `R_AddDObjSurfacesCamera` (profile string `0xB4FE14`) | VERIFIED |
| `0x006C1C90` | `R_AddDObjSurfaces` (profile string `0xB4FE2C`) | VERIFIED |
| `0x006653C0` | `DObjGetNumModels` = `*(u8*)(dobj+9)` | VERIFIED |
| `0x00648670` | `DObjGetModel(dobj, i)` | VERIFIED |
| `0x00415910` | `XModelNumBones` = `*(u8*)(model+4)` | VERIFIED |
| `0x005F8A90` | DObj bone lookup by tag scriptString → flat bone index | VERIFIED |
| `0x0050D510` | rebuild viewmodel DObj when `ps.viewmodelIndex` model changes | VERIFIED |
| `0x00527A90` | rebuild viewmodel DObj when camo variant changes | VERIFIED |
| `0x004B6870` | per-frame viewmodel update (setup + `WeaponRunXModelAnims`) | VERIFIED |
| `0x00795770` | `WeaponRunXModelAnims` (named by its own warning string) | VERIFIED |
| `0x00444740` | weapon asset by index (outer: `+0x10` szXAnims, `+0x18` hideTags) | INFERRED |
| `0x00425770` | nested `WeaponDef` by index (`+0x04` gunXModel**, `+0x08` handXModel*) | INFERRED |
| `0x006BF730` | `R_RegisterModel(name)` | INFERRED |
| `0x004ABF90` | `CG_ConfigString(index)` | INFERRED |
| `0x004BB2E0` | get tag origin/axis on a DObj | INFERRED |
| `0x00797BE0` | viewmodel placement (already documented in `camera-hook-plan.md` §2.1) | prior work |

### Globals

| VA | What | Confidence |
|---|---|---|
| `0x00C1C6D8` | `cg_viewModelArray` pointer; entry is `0x34` bytes per local client | VERIFIED |
| — `+0x00` | `DObj *dobj` (the viewmodel DObj) | VERIFIED |
| — `+0x08` | `XAnimTree *` | VERIFIED |
| — `+0x0C` | anim-tree source / weapon key | INFERRED |
| — `+0x10` | `u32 partBits[5]` (mirrored into `dobj+0x5C`) | VERIFIED |
| — `+0x24/0x28/0x2C/0x30` | last-played anim bookkeeping | VERIFIED |
| `0x00C1C6DC` | `cg_weaponArray` pointer; `0x24` bytes per weapon index | VERIFIED |
| — `[0]` | viewhands `XModel*` currently baked into the DObj | VERIFIED |
| — `[1]` | NVG/gasmask `XModel*` | VERIFIED |
| — `[2]` | clip/attachment `XModel*` | VERIFIED |
| — `+0x0C` (byte) | camo variant | VERIFIED |
| `0x02F67F28` | `cg_drawGun` `dvar_t*` (bool, default 1, flags `0x80` = cheat) | VERIFIED |
| `0x02F67C28` | `overrideNVGModelWithKnife` `dvar_t*` | VERIFIED |
| `0x02620BE8` | `sv_cheats` `dvar_t*` | VERIFIED |
| `0x03AD1F30` | scene-DObj array, stride `0x84`; `+0x48` per-model LOD bytes, `+0x68` surf list, `+0x70` `DObj*` | VERIFIED |
| `0x00C2E3D0` / `0x00C2E3E0` | cached viewmodel angles / origin written by `0x00677610` | VERIFIED |
| `0x00B6E6D8` | weapon-asset field parse table, 748 × 12 bytes | VERIFIED |

### Dvar layout (VERIFIED)

`+0x00` `const char *name`, `+0x0C` `u8 flags` (`0x10` write-protected, `0x40` read-only,
`0x80` cheat), `+0x18` current value (bool/int/float share the slot).

---

## 9. Open questions

1. Who writes `sceneEnt[0x48 + modelIdx]` (the per-model LOD byte)? Not located. Would give a
   second, coarser suppression point and is worth 20 minutes if partBits misbehaves.
2. Is `handModel` (weapon asset `+0x0EC`) read anywhere on the client? No reader found.
3. What exactly are `cg + 0xA9D44` and `cg + 0x0C`, the other two terms in the `drawGun`
   expression at `0x006776ED`? Likely "third person / in vehicle" style gates; not chased.
4. Runtime confirmation that DObj model 0 is the viewhands (see §6, five-minute log dump).
