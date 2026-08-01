# KisakBlack assessment (BAC-286)

Assessment of <https://github.com/SwagSoftware/KisakBlack> — "an open source fully-buildable
reimplementation of Call of Duty Black Ops's Multi-Player .exe" — and what, if anything, it
changes for `bo1-vr` (BAC-274: VR mod for offline SP + zombies, targeting the retail `BlackOps.exe`).

Everything below was measured against a clone of the repo at commit `e91d380` (2026-07-23),
cross-checked against our own independently-derived data in `research/engine/` and
`docs/address-map.md`. Where the README makes a claim, it was ignored and the code was read instead.

---

## Bottom line

**Use it. It is real, it is substantive, and it is a large win — but not for the reason it first
appears, and not without three specific traps.**

- It is a genuine, near-complete Hex-Rays decompilation of the Black Ops multiplayer executable
  (~920k lines of own code excluding vendored libraries), **not** a stub skeleton and **not**
  derived from the leaked Treyarch source.
- All five systems we need have real implementations. None is a shell.
- Cross-validation against our binary is **strong but bounded, and the boundary is measurable**:
  the weapon-def table matches 373/374 exactly; playerState matches exactly for every field below
  offset `0x430` and matches *nothing* at or above it.
- It does **not** change the architecture. Injecting a DLL into retail `BlackOps.exe` remains the
  plan. The "build our own client" alternative is not reachable for our scope.
- The three traps: (a) struct offsets in its headers are **comments, not layout** — the compiled
  layout drifts from retail; (b) `GfxCmdBufSourceState.eyeOffset` is **not** a stereo eye offset
  and is the wrong VR camera hook; (c) copying any of it makes `bo1-vr` GPL-3.0, and the repo's
  own GPL-3.0 claim is not self-consistent.

---

## 1. What is actually in it

Scale, measured (not from the README):

| | |
|---|---|
| Commits reachable in clone | 205 (shallow depth 200; upstream reports ~260) |
| Own-code lines (excl. vendored libs) | 920,169 |
| Own-code files | 1,407 `.c/.cpp/.h` |
| Vendored third-party files | ~1,000 (libs, tracy, jpeg, zlib, speex, vpx, …) |
| Build | CMake → MSVC 2022, single target `KisakBlack`, x86, D3D9 |

There is **one** build target and it is the MP exe. The `src/game/` vs `src/game_mp/`,
`src/cgame/` vs `src/cgame_mp/` split is the *original Treyarch source layout* (shared vs
MP-specific), not an SP/MP build split — `cmake_files.cmake` compiles both sets into the same
binary, and `KISAK_MP` is used in only 3 places (all in `src/win32/`). There is no SP target and
no SP-conditional code.

### The five systems we need

| # | System | Where | Verdict |
|---|---|---|---|
| 1 | Camera / view path | `src/gfx_d3d/r_scene.cpp:2954` `R_SetViewParmsForScene`; `:2987` `R_SetupProjection`; `:1065` `R_SetupViewProjectionMatrices`; `src/gfx_d3d/r_state.cpp:545/566/585` `R_Derive*Matrix` | **Real, complete.** Full construction of all four `GfxViewParms` matrices from `refdef`. |
| 2 | Weapon fire path | `src/game/g_weapon.cpp:1098` `G_CalcMuzzlePoints` (35 lines) | **Real but reduced vs. our SP target — see §3.** |
| 3 | Reload state machine | `src/bgame/bg_weapons.cpp` `PM_Weapon` (`:1168-1446`), `PM_BeginWeaponReload` (`:2449`), `PM_Weapon_FinishReload` (`:2280`), `PM_Weapon_FinishReloadStart` (`:1930`); `src/bgame/bg_weapons_ammo.cpp:666` `PM_ReloadClip` | **Real, complete, including `segmentedReload`.** |
| 4 | Viewmodel / DObj skeleton | `src/xanim/dobj.h`, `dobj_skel.cpp` (864), `dobj_utils.cpp` (880), `src/cgame_mp/cg_pose_mp.cpp` (746), `src/cgame/cg_weapons.cpp` (7,615) | **Real, and the bone-override mechanism we need already exists and is already used on the viewmodel.** |
| 5 | Input / usercmd path | `src/bgame/bg_pmove.cpp:941` `PM_UpdateViewAngles` + 6 sibling clamp functions | **Real, complete.** |

Detail on the two that matter most:

**Camera (priority 1).** `R_SetViewParmsForScene(const refdef_s*, GfxViewParms*)` is the single
choke point. It zeroes the struct, copies `refdef->vieworg` → `viewParms->origin[0..2]` with
`origin[3] = 1.0f`, copies `refdef->viewaxis` → `viewParms->axis`, calls
`MatrixForViewer(origin, axis, viewMatrix.m)`, resolves `zNear`, then
`R_SetupProjection(refdef->tanHalfFovX, refdef->tanHalfFovY, viewParms)` which calls
`InfinitePerspectiveMatrix(...)` — the projection is an **infinite-far** perspective matrix,
which matters for how we build per-eye projections. Finally
`R_SetupViewProjectionMatrices` does `MatrixMultiply44` + `MatrixInverse44` to fill the third and
fourth matrices. This is exactly the code path we were black-boxing, and it is fully readable.

The `refdef` itself is produced upstream by `CG_CalcViewValues` (`src/cgame_mp/cg_view_mp.cpp:2060`),
with a second entry point `CG_CalcViewValues_ExtraCam` (`:5493`) — worth knowing, but it is the
MP killcam/missilecam path, not a general stereo mechanism.

**Bone override (priority 4).** This is the biggest unexpected win. The engine already has a
first-party mechanism for overriding a single bone after animation evaluation and before skinning,
and it is already used on the viewmodel DObj:

- `DObjSetLocalTag(obj, partBits, boneIndex, trans, angles)` — `src/xanim/dobj_utils.cpp:369`
- `DObjSetControlTagAngles(obj, partBits, boneIndex, angles)` — `src/xanim/dobj_utils.cpp:312`

These write the bone's local quat/trans into `obj->skel.mat[boneIndex]` **and set the bone's `anim`
bit**, which causes `DObjCalcAnim` (`src/xanim/xanim_calc.cpp:55-70`) to skip re-sampling that bone,
so the override survives into `DObjCalcSkel`'s hierarchy pass and on to skinning. Existing callers on
the *viewmodel* DObj: `CG_UpdateViewModelStackCounter` (`src/cgame/cg_weapons.cpp:1552`) and
`UpdateMinigunTag` (`:1507`), both invoked from `CG_AddPlayerWeapon`
(`src/cgame/cg_weapons.cpp:2302+`) after `CG_UpdateViewModelPose` and before `R_AddDObjToScene`.

So the answer to "can a single bone be overridden after animation evaluation but before rendering"
is **yes, and there is a supported code path for it, with two working examples in the shipped
binary to pattern-match against.** That converts an open research question into a located-code
question.

**Reload / `segmentedReload` (priority 3).** `segmentedReload` is not just a parse-table entry. It
is `bool WeaponDef::bSegmentedReload` (`src/bgame/bg_weapons.h:659`) and is **read and branched on
at 8 runtime sites**, all in `src/bgame/bg_weapons.cpp`:

- `PM_BeginWeaponReload:2478` — chooses `WEAPON_RELOAD_START` (multi-segment) vs. a plain reload
- `PM_Weapon_FinishReload:2280,2283` — the shell-by-shell loop: on finishing a segment, re-enter
  `PM_SetReloadingState` for the next shell unless fire is held
- `PM_Weapon_FinishReloadStart:1930` — `WEAPON_RELOAD_START_INTERUPT` if fire pressed
- `PM_Weapon_CheckForReload:2399` — interrupt an in-progress segmented reload
- `PM_UpdateAimDownSightLerp:722,726` — ADS-cancel gating

Per-shell ammo is added by `PM_ReloadClip` (`src/bgame/bg_weapons_ammo.cpp:666-712`), capped by
`iReloadStartAdd` or `iReloadAmmoAdd`, which fires `EV_RELOAD_ADDAMMO`. The confirmation that the
engine already supports interruptible incremental reload is **solid**, and there is a 51-entry
`weaponstate_t` enum (`src/bgame/bg_weapons.h:9-62`) giving us the full state vocabulary including
`WEAPON_RELOADING_INTERUPT`, `WEAPON_RELOAD_START_INTERUPT`, `WEAPON_RELOAD_QUICK*`.

Caveat on `reloadTime`/`reloadEmptyTime`: they are **not** on `WeaponDef`. They live on
`WeaponVariantDef` (`src/bgame/bg_weapons.h:862-863`), a separate struct. Our
`research/engine/weapfields.txt` conflates both tables; that's harmless for lookup but matters if
we ever compute a pointer.

### Code quality caveat

~30 sites are marked `// aislop` — regions the maintainer reconstructed with LLM assistance rather
than transcribing the decompiler output, e.g. `src/glass/glass_renderer.cpp:17`
(*"uses too much aislop … should be done manually with another pass"*),
`src/cgame_mp/cg_view_mp.cpp:2411`, `src/gfx_d3d/r_model_pose.cpp:18` (`R_UpdateSceneEntBounds`),
`src/physics/phys_gjk.h`, `src/physics/rigid_body.h`. Treat those functions as *approximately*
faithful. None of the five systems above is primarily aislop, but `r_model_pose.cpp` sits on the
pose path, so verify that one against our binary before relying on it.

---

## 2. Provenance: decompilation, not leaked source

**Verdict: Hex-Rays/IDA decompilation of the retail MP binary. High confidence. No evidence of
derivation from the 2020 Treyarch source leak.**

Quantified decompiler fingerprints across `src/`:

| Artifact | Count |
|---|---|
| `// XREF:` cross-reference comments | 7,320 |
| `// padding byte` | 4,592 |
| `LODWORD(...)` / `HIDWORD(...)` | 3,110 |
| `// sizeof=0x...` | 2,077 |
| `_QWORD` / `_DWORD` / `_BYTE` | 2,281 |
| `goto LABEL_n` | 1,306 |
| `result = ` (Hex-Rays return idiom) | 1,509 |
| `loc_XXXXXX` labels | 512 |
| `[esp+..h] [ebp-..h]` stack comments | present in 609 files |

**1,033 of 1,407 (73%) of own-code files** contain at least one unambiguous decompiler fingerprint.

The four strongest pieces of evidence:

1. **A literal analyst note left in the tree**: `DemonWare/bdConnection.h:7` —
   `struct bdConnectionListener; // dont even see a def for this in IDA`. This only makes sense if
   someone was reconstructing a header while staring at an IDA database. If they had the leaked
   source, the definition would simply be there.
2. **Commit messages describing decompiler-artifact bug hunts**: `cf8a58d fix more IDA magic
   offsets`, `1805540 occluders fix and COERCE_*** ida macro fix`, `e0e7c85 fix rest of _noreturn
   functions (IDA)`, `f4aa678 redecompile Phys_CreateUserBody()`, `a7ca16e Fix known bad
   translations of hex constants to bogus global data structs`. The history is a per-subsystem
   "decompile → get it compiling → fix runtime bugs" arc (~35 `"<folder>/ compiles"` commits, then
   `90ab732 linker fixes, .exe now builds`).
3. **Total absence of original-source artifacts in the game code**: no Treyarch/Infinity Ward
   copyright headers, no `.vcproj`/`.sln` remnants of the original build, no Perforce/CVS keywords,
   no dev TODOs with names or dates, no `#if 0` blocks. Those artifacts *do* appear normally in the
   genuinely third-party vendored libraries — the contrast is the point.
4. **Where original strings *do* appear, they appear as data, not as source.** 2,035 assert calls
   in `gfx_d3d` alone carry the original build path as a string literal, e.g.
   `Assert_MyHandler("C:\\projects_pc\\cod\\codsrc\\src\\gfx_d3d\\r_scene.cpp", 994, ...)`. These
   are `.rdata` strings recovered from the binary. A leaked source tree would have `__FILE__`, not
   a hardcoded absolute path.

Authorship is `LWSS` 188 commits / `simasce` 12 / other 5 — a solo effort with occasional PRs,
consistent with a decompilation project rather than a redistributed leak.

**Practical consequence for us: the provenance risk is the ordinary "clean-room-ish reverse
engineering of a copyrighted binary" risk that our own project already carries.** It is not the
much sharper "this is stolen source code" risk. That said, note the *side benefit* of point 4: the
assert strings give us the **original Treyarch file names and line numbers** for thousands of call
sites, which is independently useful for orienting in our binary.

---

## 3. MP-versus-SP divergence: how much transfers

The engine is shared; the exes are separately compiled. What we measured:

**Struct layouts: mostly identical, with a clean, locatable break.** See §4 — playerState is
byte-identical below offset `0x430` and diverges progressively above it; the weapon-def table is
essentially identical throughout.

**Addresses: nothing transfers.** Obviously. But we already have `docs/address-map.md` for that,
and KisakBlack's *original-path assert strings* (§2 point 4) give a second, independent way to
identify functions in our binary by matching the file/line constants pushed before
`Assert_MyHandler` calls. That is a genuinely new capability this repo hands us.

**Code: mostly transfers, but not always.** The one concrete divergence we found in our priority
list is significant and is a negative finding:

> **`G_CalcMuzzlePoints` in the MP build has only one branch.**
> `src/game/g_weapon.cpp:1098-1132` is 35 lines with a single `if (ent->client)` and **no else** —
> player view-angles path only (`AngleVectors(viewang, wp->forward, wp->right, wp->up)` +
> `G_GetPlayerViewOrigin`). There is no vehicle branch and no `tag_flash` branch inside it.
> Our SP `0x670b70` has three.

The AI/`tag_flash` logic exists in the MP tree, but as a *separate parallel function*, not a branch:
`Actor_GetMuzzleInfo` (`src/game/actor_senses.cpp:659-691`) does the
`G_DObjGetWorldTagMatrix(self->ent, scr_const.tag_flash, tagMat)` lookup with per-frame caching, and
`Actor_FillWeaponParms` (`src/game/actor_aim.cpp:8-96`) fills `weaponParms` for AI. A third,
render-side `CalcMuzzlePoint` (singular) lives in `src/cgame/cg_weapons.cpp:6859-6904` with a
two-branch local-player-vs-`tag_flash` structure, used only for tracer/impact FX.

So: **the pieces of SP's three-branch function are all present and readable, but distributed across
three files.** We still get the source, we just have to reassemble it. Also note the strong positive
here — the signature we derived black-box, `(gentity_t*, out{forward,right,up,muzzle}, int)`, is
**exactly** `(const gentity_s*, weaponParms*, int shotCount)` where `weaponParms`
(`src/bgame/bg_weapons.h:905`, `sizeof=0x44`) is `float forward[3]; right[3]; up[3];
muzzleTrace[3]; gunForward[3]; const WeaponVariantDef*; const WeaponDef*`. Our RE was right, and we
now have the field names and the two trailing pointers we hadn't identified.

**Zombies:** the MP tree contains real zombie-mode identifiers (`zombiemode`, `ZombieMap`,
`zombietron`, `zombiefive_discovered`, `zombietronCorpseCount`) across 57 files, so some
zombies-relevant code is in the shared layers. This is helpful context but should not be read as
"zombies is fully in here" — our target content lives in `BlackOps.exe`, which this project does not
reimplement.

**Net assessment:** call it **high transfer for struct/enum/algorithm knowledge, moderate for
function-level structure, zero for addresses.** The mode of work changes from "guess what this
function does from disassembly" to "read the MP source, then find the corresponding code in our
binary and check for SP-specific divergence." That is a large speedup, but the verification step
does not go away — as `G_CalcMuzzlePoints` shows, the SP version can be a genuinely different
function.

---

## 4. Cross-validation against our verified offsets

This is the section that determines how much to trust the rest. Ten independent checks.

### 4.1 `GfxViewParms` — **full agreement, 6/6**

`src/gfx_d3d/r_gfx.h:115`, annotated `// sizeof=0x140`. Compiled and measured:

| Our claim | KisakBlack | |
|---|---|---|
| total size `0x140` | `0x140` | match |
| four matrices | `viewMatrix`, `projectionMatrix`, `viewProjectionMatrix`, `inverseViewProjectionMatrix`, each `GfxMatrix` = `0x40` | match |
| matrix at `+0x80` | `viewProjectionMatrix` @ `0x80` | match |
| `origin` at `+0x100` | `float origin[4]` @ `0x100` | match |
| `axis` at `+0x110` | `float axis[3][3]` @ `0x110` | match |
| nothing at/beyond `+0x140` | tail is `depthHackNearClip`/`zNear`/`zFar` @ `0x134/0x138/0x13C`, ends exactly at `0x140` | match |

This is the strongest single result. It also *names* the three tail floats we had only as "nothing
beyond `0x140`", and confirms `origin` is a `float[4]` with `origin[3] = 1.0f`
(`r_scene.cpp:2961`), not a `float[3]` plus padding.

### 4.2 `GfxCmdBufSourceState.eyeOffset` — **agreement on the number, disagreement on the meaning, and a layout trap**

Three separate findings here.

**(a) The offset is confirmed — via arithmetic, not via the compiled struct.**
`src/gfx_d3d/rb_state.h:241` is annotated `// sizeof=0x1A90`. Working the declared members forward:
`matrices` `0x800` → `input` `0xE90` → `viewParms` `0x140` → `shadowLookupMatrix` `0x40` →
`constVersions[229]` `0x1CA` → `matrixVersions[8]` `0x10`, landing at `0x19EA`; and working the
tail *backward* from the annotated `0x1A90` total, `eyeOffset` must start at `0x19F0` for the
remaining members to end exactly on `0x1A90`. **`eyeOffset` is at `+0x19F0` in the MP binary too.
Confirms our `+0x19F0`.**

**(b) But the header does not compile to that layout.** The `// padding byte` lines in these headers
are *comments*. They record what IDA saw; they do not create padding. I extracted the struct and
compiled it: `sizeof` comes out `0x1A90` (correct, because trailing alignment absorbs the error) but
**`eyeOffset` compiles at `0x19EC`, four bytes low**, and every member after it is shifted by 4.
The header even disagrees with itself — it lists 12 padding bytes where 6 are required.

> **This is the single most important caveat in this document.** KisakBlack's headers are
> *self-consistent for building KisakBlack* (which does not need binary compatibility with anything)
> but are **not a reliable source of byte offsets for our binary**. The true offsets live in the
> `// sizeof=` and `// padding byte` comments, and must be recomputed by hand. Never
> `offsetof()` a KisakBlack struct and use the result against `BlackOps.exe`.

**(c) `eyeOffset` is not what we thought it was.** Our note says `+0x19F0` was "semantically
confirmed by a write of `{0,0,0,1.0f}` at `0x6D2E12`". That write is `RB_SetIdentity`
(`src/gfx_d3d/rb_backend.cpp:363-376`), which switches to `VIEW_MODE_IDENTITY`, memcpys
`rg.identityViewParms` over `viewParms`, and zeroes `eyeOffset` with `w = 1.0f`. So the address
identification is **correct**.

But the *semantics* are not stereo. `eyeOffset` is the **world-space view origin used to make model
matrices camera-relative** — a float-precision measure, not an eye separation. Evidence:

- `RB_SetEyeOffsetConstant(source, drawSurfListViewOrigin)` — `rb_backend.cpp:1542-1557` — copies
  the draw-surf-list's *view origin* into it, or zeroes it when `drawSurfListViewOrigin[3] == 0`,
  then publishes it as shader constant `CONST_SRC_CODE_EYEOFFSET` (= 191, `r_state.h:205`).
- Consumers subtract it: `src/gfx_d3d/r_draw_staticmodel.cpp:233-235`
  `origin[i] = smodelDrawInst->placement.origin[i] - source->eyeOffset[i]`; likewise
  `r_draw_xmodel.cpp:94-110` before `R_GetWorldMatrixForModelSurf`.
- The material system exposes it by name: `r_material_load_obj.cpp:631` `{ "eyeOffset", 191u, ... }`.

**Consequence: writing a per-eye offset into `eyeOffset[4]` would translate the world geometry, not
the camera, and would desynchronise it from the view/projection matrices.** The VR camera hook is
`R_SetViewParmsForScene` / the `refdef` that feeds it — i.e. `refdef->vieworg` + `refdef->viewaxis`
+ `tanHalfFovX/Y`, produced by `CG_CalcViewValues`. This finding alone probably justifies the time
spent on this assessment.

Related: BO1's shipped "3D" support is **NVIDIA 3D Vision driver-side only** — the engine sets
`NvAPI_Stereo_SetConvergence(dx.nvStereoHandle, r_convergence->current.value)`
(`rb_backend.cpp:4764`) and registers `r_convergence`, `r_use_driver_convergence`,
`r_stereoTurretShift`. There is **no engine-side dual-view render loop to hijack.** We will have to
drive the view twice ourselves.

### 4.3 `DxGlobals` — **full agreement, 3/3**

`src/gfx_d3d/r_init.h:58`, `// sizeof=0x2D00`. Member walk:

| Member | KisakBlack offset | Our claim |
|---|---|---|
| `nvInitialized` (bool) | `0x2A` | present |
| `nvStereoActivated` (bool) | **`0x2B`** | `+0x2B` — match |
| `nvStereoHandle` (void*) | **`0x2C`** | `+0x2C` — match |

Exact. (This one is a real independent confirmation: we derived `+0x2B`/`+0x2C` from disassembly of
the SP binary and from `t5-rtx`, and KisakBlack's MP-derived struct lands on the same bytes.)

### 4.4 playerState netfields — **102/104 below `0x430`, 0/63 at or above it**

`playerStateFields[179]` at `src/qcommon/msg_mp.cpp:1991`, diffed against the 178-entry playerState
block in `research/engine/netfields.txt` (lines 361–538).

- 167 field names in common. **102 agree exactly on both offset and size.**
- Every one of the 102 agreements is at MP offset `< 0x430` (highest agreeing offset: `0x1C0`).
- **Every** common field at MP offset `>= 0x430` diverges — 63 of them — by a monotonically
  increasing delta as SP inserts extra fields:

  | MP offset range | SP delta |
  |---|---|
  | `0x430`–`0x458` | `+0x04` |
  | `0x460`–`0x46C` | `+0x10` |
  | `0x47C`–`0x4E0` | `+0x08` |
  | `0x4F0`–`0xB78` | `+0x10` |

- Two sub-`0x430` divergences, both size-only and both weapon indices: `weapon` @ `0x144` is
  **2 bytes in MP, 1 byte in SP**; `lastStandPrevWeapon` is `0x146`/2 in MP vs `0x145`/1 in SP.
  SP uses byte weapon indices; MP uses shorts. Worth remembering.
- MP-only fields (11): `perks[0]`, `perks[1]`, `killCamEntity`, `killCamTargetEntity`,
  `spyplaneTypeEnabled`, `satelliteTypeEnabled`, `artilleryInboundIconLocation`,
  `locationSelectionType`, `stowedWeaponCamo`, `sprintState.sprintCooldown`,
  `sprintState.sprintDuration`, `renderOptions`.
- SP-only fields (11): `binoculars`, `groundTiltAngles[0..2]`, `linkAngles[0]`, `linkAngles[2]`,
  `linkFlags`, `loopSoundId`, `loopSoundFade`, `perks` (scalar, not array), `radarTypeEnabled`.

Against our ten claimed netfield offsets specifically:

| Field | Ours (SP) | KisakBlack (MP) | |
|---|---|---|---|
| `viewangles` | `0x180` | `0x180` | match |
| `delta_angles` | `0x84` | `0x84` | match |
| `weaponstate` | `0x158` | `0x158` | match |
| `weaponTime` | `0x3c` | `0x3c` | match |
| `weaponTimeLeft` | `0x44` | `0x44` | match |
| `fWeaponPosFrac` | `0x168` | `0x168` | match |
| `viewmodelIndex` | `0x17c` | `0x17c` | match |
| `offhandPrimary` | `0x13c` | `0x13c` | match |
| `weapAnim` | `0x524` | `0x514` | **diverge, +0x10** |
| `meleeChargeYaw` | `0x4dc` | `0x4d4` | **diverge, +0x08** |

**8/10 match, and both misses are exactly predicted by the `>= 0x430` rule.** Our own numbers are
vindicated in both directions: the eight that agree are confirmed by an independent source, and the
two that disagree are the two we would have expected to, which is itself evidence our SP dump is
correct rather than evidence it is wrong.

> **Working rule: trust KisakBlack playerState offsets below `0x430`. Re-derive everything at or
> above `0x430` from our binary.** `weapAnim` and `meleeChargeYaw`, both of which we need, are above
> the line — but we already have them measured.

### 4.5 Weapon fields — **373/374 exact, and 4/4 on the ones we care about**

Parse table at `src/bgame/bg_weapons_load_obj.cpp`, diffed against
`research/engine/weapfields.txt` on `(offset, type)`:

- 374 field names in common → **373 agree exactly on offset *and* type code**.
- The single apparent miss (`emptyIdleAnim`) is an artifact: the name appears twice in the
  KisakBlack table (offsets 2352/2356 and 2592), and our dump matches the second (`0xa20` = 2592).
  Correcting for that, agreement is **374/374**.

Our four claimed fields:

| Field | Ours | KisakBlack | |
|---|---|---|---|
| `reloadTime` | `0x24` type 8 | `36` type 8 | match |
| `reloadEmptyTime` | `0x28` type 8 | `40` type 8 | match |
| `segmentedReload` | `0x666` type 5 | `1638` type 5 | match |
| `reloadAmmoAdd` | `0x668` type 4 | `1640` type 4 | match |

**The weapon-def layout is identical between the MP and SP builds.** This is the cleanest result in
the whole assessment and it means the entire ~750-entry weapon field table in KisakBlack — names,
types, and semantics — can be used directly against our binary.

### 4.6 Entity events — **3/3**

`entity_event_t` at `src/bgame/bg_misc.h:18+`, contiguous from `EV_NONE = 0`, with a parallel name
table at `src/bgame/bg_misc.cpp:121-126` confirming the mapping:

| Event | Ours | KisakBlack | |
|---|---|---|---|
| `EV_RELOAD_START` | 20 | `0x14` = 20 | match |
| `EV_RELOAD_END` | 21 | `0x15` = 21 | match |
| `EV_RELOAD_ADDAMMO` | 23 | `0x17` = 23 | match |

All three are genuinely fired at runtime (`bg_weapons.cpp:2483`, `:1940`/`:2295`,
`bg_weapons_ammo.cpp:709`) and consumed client-side (`src/cgame/cg_event.cpp:677-717`). We also get
the three neighbours for free: `EV_RELOAD` = 18, `EV_RELOAD_FROM_EMPTY` = 19,
`EV_RELOAD_START_NOTIFY` = 22.

### 4.7 Scorecard

| Item | Result |
|---|---|
| `GfxViewParms` (6 sub-claims) | 6/6 exact |
| `GfxCmdBufSourceState.eyeOffset` offset | confirmed at `+0x19F0` (by annotation arithmetic; **not** by the compiled header) |
| `GfxCmdBufSourceState.eyeOffset` semantics | **our interpretation was wrong** — it is camera-relative rendering, not stereo |
| `DxGlobals` `nvInitialized`/`+0x2B`/`+0x2C` | 3/3 exact |
| playerState netfields (10 claimed) | 8/10 exact; 2 diverge, both predicted by the `>= 0x430` rule |
| playerState netfields (all common) | 102/104 exact below `0x430`; 0/63 above |
| Weapon fields (4 claimed) | 4/4 exact incl. type codes |
| Weapon fields (all common) | 374/374 exact |
| Entity events (3 claimed) | 3/3 exact |

**Aggregate: 29 of 31 specific claims confirmed; both misses explained by a measurable MP/SP rule;
one semantic correction to our own model.** This is a high trust level for the rest of the source,
subject to the two structural caveats (header offsets are comments; the `0x430` line).

---

## 5. GPL-3.0 implications

KisakBlack is GPL-3.0 (top-level `LICENSE`, added in commit `50e9588`). There are **zero per-file
license headers** anywhere in the project's own code — a repo-wide grep for
`SPDX-License-Identifier` or GPL banners across `src/`, `DemonWare/`, `tl/` returns exactly one hit,
and it is a vendored file's own MIT tag (`src/tracy/server/tracy_robin_hood.h:12`).

`bo1-vr` currently has **no LICENSE file at all**. That is itself a decision waiting to be made,
independent of KisakBlack. Three options, with what each commits us to:

**(a) Read it, then write our own code — "clean-ish room".**
We read KisakBlack to understand the engine, take away *facts* (offsets, enum values, algorithm
shape, function names, call order), and write original code against our binary.
→ **Commits us to nothing.** Facts and interfaces are not copyrightable in most relevant
jurisdictions, and this is the same category of knowledge we were already extracting by
disassembly. This is the default and it captures nearly all of the value described in §1 and §4.
Recommended hygiene if we take this path: don't paste, and note in commit messages where a fact
came from so the derivation is auditable.

**(b) Copy struct definitions / enums / field tables.**
This is the grey middle. Struct layouts and enum values *are* facts about the binary and would be
weak copyright subject-matter, but a verbatim copy of a 51-entry `weaponstate_t` with KisakBlack's
exact spellings and ordering, or a 750-row weapon field table, looks like copying an expressive
selection and arrangement even if each datum is a fact.
→ **Legally arguable, practically risky, and — importantly — technically unwise anyway**, because
of §4.2(b): their headers' compiled offsets are not our binary's offsets. If we want structs, we
should generate them from *our* measurements (which we already have in `research/engine/`), using
KisakBlack only to supply names. That gets us the readability benefit with neither the licence
question nor the layout trap.
→ If we do copy any of it, treat it as (c).

**(c) Copy implementation.**
Any non-trivial function body lifted from KisakBlack makes `bo1-vr` a derivative work. GPL-3.0 is
strongly copyleft and has no linking exception, so:
→ **The entire `bo1-vr` DLL must be released under GPL-3.0**, with complete corresponding source
offered to anyone who receives a binary. That is not necessarily a bad outcome for a hobby mod, but
it is a one-way door: it forecloses any future proprietary component, and it obliges us to keep
publishing source for every release. It also means we inherit an awkward association with the
provenance problems in §5b.

There is one further wrinkle worth flagging: **KisakBlack's own GPL-3.0 grant is of doubtful
validity.** The code is a decompilation of Activision/Treyarch's copyrighted binary; Treyarch never
licensed it. The maintainers cannot license out rights they do not hold. So option (c) buys us a
GPL obligation without necessarily buying us a reliable licence *to* the underlying code. Practically
this means (c) is the worst of both worlds and (a) is clearly the right default.

**Not deciding this here.** But if the outcome of (a) is what we expect, `bo1-vr` remains free to
pick its own licence, and adding one (even MIT) before this becomes entangled would be sensible.

### 5b. Bundled content with worse provenance

The repo commits and *links against* proprietary binaries it has no right to redistribute. This
matters because it is the sort of thing that gets a repo DMCA'd out from under us — worth mirroring
our clone rather than depending on upstream availability.

| Path | Size | Provenance |
|---|---|---|
| `src/nvapi/nvapi.lib`, `nvapi64.lib` | 868 KB, 1.0 MB | NVIDIA NVAPI. Header `src/nvapi/nvapi.h:3-10` reads *"PROPRIETARY and CONFIDENTIAL to NVIDIA … provided solely under the terms of an NVIDIA software license agreement and/or non-disclosure agreement"*, `Target Profile: NDA-developer`. |
| `src/binklib/binkw32.lib`, `binkw32.dll` | 17 KB, 176 KB | RAD Game Tools Bink, commercial middleware (`radbase.h:39`). |
| `src/steam/redistributable_bin/steamclient.dll` | 4.5 MB | Valve Steam client binary. |
| `src/steam/redistributable_bin/Steam.dll` | 2.9 MB | Valve Steam client binary. |
| `steam_api.lib/.dll`, `tier0_s.dll`, `vstdlib_s.dll`, `steam_api64.*`, `sdkencryptedappticket*` | up to 1 MB each | Valve Steamworks / internal runtime. |
| `src/libs/libvpx-1.5.0/*.lib/.dll/.pdb` | up to 9.9 MB | libvpx (BSD — fine legally, just shouldn't be committed). |

`CMakeLists.txt:72-84` links `steam_api.lib binkw32.lib nvapi.lib libvpx.lib`, and `.gitignore` has
no rules excluding them, so this is deliberate. Statically bundling NDA-restricted NVAPI and non-free
Steamworks binaries in a GPL-3.0 source tree is a textbook GPL incompatibility, on top of likely
breaching NVIDIA's own terms.

`DemonWare/` (33 files) and `src/monkey/` are **decompiled reimplementations of Activision's
proprietary networking and telemetry middleware** — better than copied SDK source, but still
reverse-engineered proprietary IP.

Genuinely clean vendored code (all with retained licence text): tracy (BSD-3, `src/tracy/LICENSE`),
libjpeg 6b (IJG), zlib, speex (Xiph BSD), miniLZO (GPLv2+, GPL-compatible), libtommath (public
domain), libtomcrypt (public domain), AMD CubeMapGen (`src/CubeMapGenLib/License.doc`, modified BSD).

**None of this touches us if we take option (a)** — we would be reading source, not linking
binaries. It is listed so the exposure is on the record and so we know not to vendor any of it.

---

## 6. Does it change the architecture?

**No. Stay with DLL injection into retail `BlackOps.exe`.**

The "build a modded client instead" alternative is not reachable for our scope, for three
independent reasons, any one of which is sufficient:

1. **It is the wrong executable.** KisakBlack reimplements `BlackOpsMP.exe`. Our scope is offline
   single-player and zombies, which live in `BlackOps.exe`. There is no SP build target, no SP
   conditional compilation (`KISAK_MP` appears 3 times, all in `src/win32/`), and no SP entry point.
   Building KisakBlack gives us a multiplayer client.
2. **We do not have the binary it would replace.** Our Steam depot contains no multiplayer
   executable at all. We could not run KisakBlack against our own game files even if we wanted a
   multiplayer VR mod — the README's build instructions assume a game install that includes the MP
   side.
3. **The work would not be smaller.** Even granting a hypothetical KisakBlack-SP, we would be
   taking on a 920k-line decompiled codebase with known reconstruction gaps (~30 `aislop` regions,
   an active issue tracker, "known exploits" warned about in the README) and maintaining a from-source
   engine fork, versus writing a few thousand lines of hooks against a binary that already works.
   And it would put us squarely in option (c) of §5 — full GPL-3.0, no way back.

There is a fourth consideration that cuts the other way and is worth naming honestly: a source build
would make the *hard* parts of VR — rendering the scene twice per frame, restructuring the view
pipeline for per-eye submission — dramatically easier than hooking them. §4.2 established there is
no engine-side stereo loop, so the retail-binary path means we have to construct one via hooks. That
is real cost. But it does not outweigh points 1 and 2, which are disqualifying rather than merely
expensive.

**What KisakBlack changes instead is the *method*, not the architecture.** Concretely, it should be
used as:

- **A reference implementation to read alongside disassembly.** Locate the function in KisakBlack,
  understand it, then find and verify the SP counterpart in our binary.
- **A symbol source.** ~2,000 assert call sites per major subsystem embed the original Treyarch
  file path and line number as string literals; these give us an independent way to name functions
  in `BlackOps.exe` by matching the constants pushed before `Assert_MyHandler`.
- **A cross-check for future measurements**, with the two rules established in §4: playerState
  offsets are trustworthy below `0x430` only, and struct offsets must be read from the
  `// sizeof=` / `// padding byte` comments rather than compiled.

### Concrete follow-ups this unblocks

| Was | Now |
|---|---|
| Find and characterise the view/projection construction by RE | Read `R_SetViewParmsForScene` (`r_scene.cpp:2954`) and locate its SP counterpart. Note: infinite-far projection. |
| Decide whether `eyeOffset[4]` is the VR camera hook | **It is not.** Hook the `refdef` / `R_SetViewParmsForScene` instead. Retire that approach before building on it. |
| Determine whether a viewmodel bone can be overridden pre-render | **Yes.** `DObjSetLocalTag` / `DObjSetControlTagAngles`, with two existing viewmodel call sites to pattern-match (`cg_weapons.cpp:1507`, `:1552`). |
| Determine whether interruptible incremental reload is supported | **Yes.** `bSegmentedReload` drives 8 runtime branches; `weaponstate_t` has `WEAPON_RELOAD_START_INTERUPT` and `WEAPON_RELOADING_INTERUPT`. |
| Reconstruct SP's three-branch muzzle function | Read all three MP pieces — `G_CalcMuzzlePoints` (player), `Actor_GetMuzzleInfo`/`Actor_FillWeaponParms` (AI `tag_flash`), `CalcMuzzlePoint` in cgame (FX) — then map onto `0x670b70`. Expect divergence; SP merged what MP split. |
| Weapon field semantics | Take the whole ~750-entry table from `bg_weapons_load_obj.cpp` — verified 374/374 against ours. |

---

## Appendix: how to reproduce

```
git clone https://github.com/SwagSoftware/KisakBlack.git   # assessed at e91d380, 2026-07-23
```

Cloned outside this repo (scratchpad); deliberately **not** vendored. Given §5b, mirroring the clone
somewhere durable is advisable — a repo shipping `steamclient.dll` and NDA-marked NVAPI headers is
not a safe long-term dependency.

Struct layouts in §4.1/§4.2 were verified by extracting the declarations into a standalone
translation unit and compiling it with 4-byte pointer stand-ins, not by reading the annotations.
Field-table diffs in §4.4/§4.5 were produced by parsing `src/qcommon/msg_mp.cpp` and
`src/bgame/bg_weapons_load_obj.cpp` and joining on field name against
`research/engine/netfields.txt` and `research/engine/weapfields.txt`.
