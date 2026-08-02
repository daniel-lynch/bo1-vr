# Physical reload feasibility — the magazine-bone hypothesis (BAC-285)

Target: `BlackOps.exe`, 8,101,944 bytes, PE32 x86, ImageBase `0x400000`, no ASLR.
All addresses below are virtual addresses in that image. Nothing was executed.

Scope: offline single-player and zombies, on a copy of the game the user owns.

## Evidence grades used throughout

- **[BIN]** read directly out of the executable's code or data.
- **[ASSET]** read directly out of shipped game data (`zone/Common/*.ff`, `main/iw_00.iwd`).
- **[INF]** inference from BIN/ASSET facts. Stated as inference, with the reasoning.
- **[OPEN]** not established; explicitly still unknown.

---

## VERDICT

**The magazine-bone hypothesis holds.** Every load-bearing premise the previous
analysis treated as unavailable is in fact present in this binary and in the
shipped assets:

1. The weapon viewmodel is a real skeletal `DObj` with per-bone transforms
   evaluated every frame into a **plain writable heap array**. [BIN]
2. **`tag_clip` exists**, is a per-weapon bone on the *viewmodel* skeletons
   (`t5_weapon_*_viewmodel`), and it is the bone that carries the standard
   magazine geometry. This is not speculation: 27 shipped weapon files hide
   exactly that bone when an extended/dual magazine attachment is fitted. [ASSET]
3. There is a clean choke point. The renderer reads the bone array **live** out
   of `DObj_s + 0x54` at draw time; it is not snapshotted at animation time on
   the path examined. Writing one 32-byte entry between skeleton evaluation and
   render is a supported shape. [BIN]
4. Freezing `weaponTime` is structurally benign inside pmove: the weapon state
   machine's *only* advance condition is `weaponTime` crossing zero, and if it is
   already zero the whole tick is skipped. Holding it positive stalls the machine
   and does nothing else. [BIN] Caveats about prediction and the server copy below.
5. If the bone route were to fail, the fallback is unusually strong: the engine
   already hides named viewmodel bones by name at runtime, exposed to script as
   `useweaponhidetags`. Hiding the stock magazine and drawing a substitute is a
   first-class engine capability here, not a hack. [BIN][ASSET]

The two real risks are **(a)** the bone array is in model space, so a hijacked
bone does not drag children with it, and **(b)** predicted-vs-authoritative
`weaponTime` divergence in any networked context. Both are addressed below.

**Hard requirement, recorded:** whatever gets built, the stock button-press
reload must remain selectable at runtime. Speed Cola is a reload-speed perk and
high-round zombies is tuned around reload timing; a mod that only offers physical
reload breaks the game's balance. The design below keeps the engine driving the
timer precisely so that the stock path stays intact as the default.

---

## Q1. Does the weapon viewmodel use a skeletal model with per-bone transforms at runtime?

**Yes.** [BIN]

### The viewmodel is a DObj, at a reserved entity number

`0x570950` is a two-instruction accessor:

```
00570950  mov eax, dword ptr [esp + 4]     ; localClientNum
00570954  add eax, 0x600
00570959  ret
```

It returns `0x600 + localClientNum`. Its callers (`0x5d3a3a`, `0x5e5644`,
`0x64d816`, `0x64d837`, `0x894656`) feed that number into the same DObj lookup
used for ordinary entities. So the viewmodel occupies entity slot `0x600`
(1536) for local client 0.

`0x894656` is inside the `playviewmodelfx` script builtin (`0x8945d0`), which
resolves a *tag name on the viewmodel* — it errors with
`"PlayViewmodelFX(): unable to find viewmodel tag."` (string at `0x9c7dbc`) when
the name does not resolve. That is direct proof that the viewmodel carries
named, runtime-queryable tags.

### The DObj handle table

`0x47e340` — `Com_GetClientDObj(entnum)`:

```
0047e340  mov eax, dword ptr [esp + 4]
0047e344  movzx eax, word ptr [eax*2 + 0x2487d48]   ; u16 handle table, indexed by entnum
0047e34c  test ax, ax
0047e34f  je 0x47e35b                               ; 0 -> no DObj
0047e351  cwde
0047e352  imul eax, eax, 0x7c                       ; sizeof(DObj_s) == 0x7c
0047e355  add eax, 0x2489148                        ; DObj array base
0047e35a  ret
```

- handle table: `0x2487d48`, `u16` per entity number
- `DObj_s` array base: `0x2489148`, stride `0x7c` (124 bytes)

### Where the bone transforms live

`0x59c4a0` — `DObjGetRotTransArray(dobj)`:

```
0059c4a0  mov eax, dword ptr [esp + 4]
0059c4a4  mov eax, dword ptr [eax + 0x54]
0059c4a7  ret
```

`DObj_s + 0x54` is a **pointer to the bone transform array**. Nothing clever —
a plain pointer to a plain array.

### Element layout

`0x4541d0` is the read helper. It resolves a bone by name token and returns a
pointer into that array:

```
004541d0  mov eax, [esp+8]          ; arg1 = bone name token (u16 scr_const)
004541d6  mov edi, [esp+0xc]        ; arg0 = entity (first dword is entnum)
004541db  push edi
004541dc  call 0x5a0e30             ; CG_DObjGetBoneIndex(ent, token) -> int, <0 on fail
004541e1  mov esi, eax
004541e8  jge 0x4541ef
004541ea  ...                       ; return NULL
004541ef  push esi
004541f0  push edi
004541f1  call 0x4be720             ; ensure this bone is evaluated this frame
004541f6  push edi
004541f7  call 0x53b780             ; -> Com_GetClientDObj(ent->entnum) -> DObjGetRotTransArray
004541ff  shl esi, 5                ; boneIndex * 32
00454203  add eax, esi
00454206  ret
```

- **stride is 32 bytes per bone** (`shl esi, 5`) [BIN]
- the translation is at **+0x10** within the element. Established from the sole
  `tag_clip` consumer at `0x4f55e8`, which subtracts two bones' positions:
  `movss xmm0, [ebx+0x10]` / `subss xmm0, [esi+0x10]`, then `+0x14`, `+0x18`. [BIN]
- the remaining 16 bytes at +0x00 and the float at +0x1c are, by the standard
  `DObjAnimMat` layout, `float quat[4]` and `float scale`. **[INF]** — the offsets
  0x00..0x0F and 0x1C were not independently confirmed in this pass. They are
  cheap to confirm at runtime.

So: `DObjAnimMat { vec4 quat; vec3 trans; float scale; }`, 32 bytes, array at
`*(void**)(dobj + 0x54)`, indexed by bone index.

### Per-frame evaluation

`0x4be720` — "make sure bone N is current":

```
004be72d  call 0x47e340        ; dobj = Com_GetClientDObj(ent->entnum)
004be73a  call 0x4faa70        ; already-computed check for this frame
004be744  jne  done            ; nonzero -> nothing to do
004be74d  call 0x45af30        ; build partBits for the requested bone
004be75f  mov  eax, [edx + 0xb75644]   ; per-entity-type callback table, indexed by ent[0x15a]
004be772  call eax                     ; optional pre-skel hook (procedural bones)
004be77d  call 0x564ab0        ; DObjCalcSkel(dobj, partBits)
```

- `0x564ab0` = `DObjCalcSkel(dobj, partBits*)` — the animation system's skeleton
  evaluation. Four callers: `0x47e1d7`, `0x4be77d`, `0x502bb3`, `0x54a83a`. [BIN]
- `0x4faa70` gates on a global frame counter at `0x2865e84`, so evaluation is
  memoised **per frame per part**. [BIN]
- **Note the callback table at `0xb75644`**, stride 0x30, indexed by `ent[0x15a]`,
  invoked *between* partBits construction and `DObjCalcSkel`. This is the engine's
  own "procedural bone" hook. See Q3.

### The renderer reads that array directly

`0x6c9860` (renderer region) does:

```
006c98aa  call 0x59c4a0          ; DObjGetRotTransArray(dobj)
006c98b6  mov  ebx, eax
006c98cd  shl  edx, 5            ; boneIndex * 32
006c98d1  add  ebx, edx
006c98e8  call 0x5127f0          ; quat/trans -> matrix
006c98ed  movss xmm4, [ebx]      ; reads the element in place
```

It pulls the pointer fresh and indexes it at draw time. [BIN]

---

## Q2. Is there a magazine bone or tag on weapon viewmodels?

**Yes — `tag_clip`, confirmed three independent ways.** [BIN][ASSET]

### 1. The executable knows the name

`"tag_clip"` at `0x9eb178`. It is registered into the engine's interned-string
table at `0x5ea87c` (inside the bulk registration function `0x5ea220`):

```
005ea87c  push 0x9eb178          ; "tag_clip"
005ea881  mov  word ptr [0x23a566a], ax
005ea887  call 0x41b120          ; SL_GetString
```

The resulting token lives at global **`0x23a566a`**. It sits in an alphabetical
run with `tag_butt`, `tag_flash`, `tag_brass`, `tag_aim`, `tag_aim_pitch` —
i.e. the engine's registered bone/tag vocabulary. [BIN]

Exactly one site in the whole image reads that token: `0x4f558b`, inside
`0x4f5530`. That function looks up `tag_aim_pitch` and `tag_clip` on a
`centity_t`, then traces 30 steps between them — a barrel/clearance check, not
viewmodel code. So the *executable* only uses `tag_clip` for one narrow purpose.
That is not a problem for us: what matters is that the name is a registered token
and that the models carry the bone.

### 2. The shipped zone data contains the name

Decompressing `zone/Common/common.ff` (header `IWffu100`, version `0x1d9`, raw
zlib from offset 12) and `common_zombie.ff` puts `tag_clip` in the zone's
scriptString pool at byte offset 4616. The zombie zone's full tag vocabulary
includes a whole magazine family: [ASSET]

```
tag_clip  tag_clip1  tag_clip2  tag_clip_empty  tag_clip_ext  tag_clip_extended
tag_clip_release  tag_double_clip  tag_dual_clip  tag_dualmag  tag_big_mag
tag_ext_clip  tag_ext_clip1  tag_extended_clip  tag_fullauto_clip  tag_drum
tag_speedloader  tag_bolt  tag_bolt_handle  tag_pump2  tag_mk_pump  tag_shell
tag_spent0 .. tag_spent5  tag_brass  tag_brass1  tag_brass2
```

alongside the expected `tag_flash`, `tag_scope*`, `tag_silencer`, `tag_foregrip`,
`tag_m203`, `tag_masterkey`.

### 3. The shipped weapon files prove it is a *viewmodel* bone carrying the magazine

This is the decisive one. Every weapon file has a `hideTags` list. Extracting
`weapons/sp/*` from `main/iw_00.iwd` (756 files) and filtering for weapons that
hide `tag_clip`: [ASSET]

| weapon file | `gunModel` | hides |
|---|---|---|
| `ak47_extclip_sp` | `t5_weapon_ak47_viewmodel` | `TAG_CLIP`, … |
| `commando_extclip_sp` | `t5_weapon_commando_viewmodel` | `TAG_CLIP`, `TAG_DOUBLE_CLIP`, … |
| `m1911_extclip_sp` | `t5_weapon_1911_sp_viewmodel` | `TAG_CLIP`, `tag_suppressor`, … |
| `rpk_dualclip_sp` | `t5_weapon_rpk_viewmodel` | `TAG_DRUM`, `TAG_CLIP`, … |
| `l96a1_extclip_sp` | `t5_weapon_l96a1_viewmodel` | `tag_clip`, … |
| … 27 files total | all `t5_weapon_*_viewmodel` | |

Across all 756 sp weapon files, `hideTags` name frequencies include
`TAG_CLIP` ×27, `TAG_EXTENDED_CLIP` ×80, `TAG_DUAL_CLIP` ×59,
`TAG_DOUBLE_CLIP` ×54, `tag_ext_clip` ×45, `tag_big_mag` ×24.

The pattern is unambiguous: each gun's viewmodel contains **every** magazine
variant as geometry, and the weapon file hides all of them except the one this
variant uses. `ak47_extclip_sp` hides `TAG_CLIP` (the stock mag) and keeps the
extended one; `ak47_dualclip_sp` hides `TAG_CLIP` and `TAG_EXTENDED_CLIP`.

Two consequences:

- `tag_clip` is a bone present on the ordinary weapon **viewmodel** skeleton,
  not just on world models or turrets. **[ASSET]**
- The magazine geometry is bound to that bone. Hiding the bone removes the
  magazine, so the magazine's vertices are weighted to it. **[INF]** — strong,
  but it is inference from the hide behaviour, not from parsing the XModel's
  surface/bone weights. If you want it airtight, dump `viewmodel_ak74u`'s bone
  weights, or just move the bone at runtime and look.

Also worth recording: `tag_clip` is the *only* magazine tag with an engine-side
registered token; the variants (`tag_ext_clip` etc.) are asset-side scriptStrings
only. For a mod that resolves names dynamically this makes no difference —
`DObjGetBoneIndex` takes a token, and `SL_FindString` will intern any name.

### The weapon field table does not name bones

`weapfields.txt` has `clipName` (`ithaca`, a sound/name key) and `clipSize`, but
no field that names the magazine bone. The bone name is baked into the model, not
configurable. [BIN]

---

## Q3. Can a single bone's transform be overridden per frame from outside?

**Yes, and there is more than one place to do it.** [BIN]

### The primitive

```c
// all addresses verified above
DObj_s*      dobj  = Com_GetClientDObj(0x600 + localClientNum);   // 0x47e340
bool         ok    = DObjGetBoneIndex(dobj, tokenTagClip, &idx, -1); // 0x5f8a90
DObjAnimMat* bones = *(DObjAnimMat**)((char*)dobj + 0x54);        // via 0x59c4a0
bones[idx].trans   = wanted;   // +0x10, +0x14, +0x18
bones[idx].quat    = wantedQ;  // +0x00 .. +0x0C   [INF on exact field]
```

`0x5f8a90` is the raw `DObjGetBoneIndex(dobj, nameToken, &outIndex, -1)`; it is
what both `0x5a0e30` (the entity-level wrapper) and the `hidepart` builtin
(`0x7f1e90`) call. [BIN]

### Where to hook

Three candidate choke points, best first:

**(a) The per-entity-type procedural-bone callback table at `0xb75644`.**
`0x4be720` and `0x502b70` both do:

```
mov  eax, [ecx + 0xb75644]     ; ecx = ent[0x15a] * 0x30
test eax, eax
je   skip
call eax                       ; (ent, partBits*)
```

This is the engine's own extension point, called *after* partBits are computed
and *immediately before* `DObjCalcSkel`. It is a writable function-pointer table
in `.data`. **[INF]**: it is called before rather than after skeleton evaluation,
so a callback installed here can adjust inputs but cannot post-process the output
array. Useful, but not the primary hook. Worth confirming what `ent[0x15a]` is
for the viewmodel entity and whether the slot is free.

**(b) Hook the return of `DObjCalcSkel` (`0x564ab0`).**
On return, if `dobj == Com_GetClientDObj(0x600 + lc)`, overwrite
`bones[tagClipIndex]`. This is the tightest post-evaluation point that exists.
Caveat: `0x4faa70` memoises per frame, so `DObjCalcSkel` is not guaranteed to be
called every frame for the bones you care about — if nothing requests the mag
bone in a given frame your write is simply stale from last frame, which is
visually wrong but not fatal. Fix by forcing evaluation yourself (call `0x4be720`
with the mag bone) once per frame from your own per-frame tick, then writing.

**(c) Write from your own per-frame tick, immediately before the viewmodel is
added to the scene.** The renderer reads `dobj+0x54` live at `0x6c9860`, so any
write that lands after evaluation and before the scene walk is honoured. The
viewmodel placement function the prior work identified (`0x797be0`, with
`0x797e90` nearby) is in the right neighbourhood to anchor this.

Recommended: **(b) plus a forced evaluation**, i.e. each frame call
`0x4be720(viewmodelEnt, magBoneIndex)` to guarantee the array is current, then
write the entry. That is deterministic and does not depend on what else happened
to request bones that frame.

### The important limitation: model space, no child propagation

`DObjAnimMat` entries are **already-composed absolute transforms in model space**,
not local parent-relative transforms. Two consequences:

- Overwriting one entry moves that bone's geometry and nothing else. For a
  magazine with no children this is exactly what we want. **[INF]** — inferred
  from the read helper returning a directly-usable position (`0x4f55e8` uses
  `[bone+0x10]` as a position with no parent walk) and from the renderer building
  a matrix straight from one element.
- Anything attached *to* `tag_clip` — an extended-mag attachment model, an FX —
  will follow only if it resolves the tag after your write. Attachments resolved
  earlier in the frame will not. For the stock magazine, which is the case that
  matters, there is nothing attached.

Also: the write is in **model space of the viewmodel**, and the viewmodel is
itself placed relative to the view. To make the mag track a physical controller
you must transform the controller pose into viewmodel model space, which means
knowing the viewmodel's placement transform for that frame. `0x797be0` is where
that lives. This is arithmetic, not a blocker, but it is real work and it is where
the visual quality of the effect will be won or lost.

---

## Q4. Is freezing `weaponTime` actually safe?

**Inside pmove: yes, and more cleanly than expected. Across the client/server
boundary: needs care.** [BIN]

### Structure established

The pmove weapon code (`0x760000`–`0x76c000`) accesses playerState through
**pointers**, not fixed offsets, because of dual-wield. Every reload function
opens with the same idiom (e.g. `0x766910`, `0x769090`, `0x766590`):

```
mov  al, [ps + 0x61]                      ; "is left hand" flag
lea  ecx, [ps + 0x15c] / [ps + 0x158]     ; &weaponstateLeft / &weaponstate
lea  ecx, [ps + 0x44]  / [ps + 0x3c]      ; &weaponTimeLeft  / &weaponTime
lea  ecx, [ps + 0x48]  / [ps + 0x40]      ; &weaponDelayLeft / &weaponDelay
lea  ecx, [ps + 0x164] / [ps + 0x160]
```

This confirms the netfield offsets from prior work and adds the pairing:

| field | right | left |
|---|---|---|
| `weaponTime` | `ps+0x3c` | `ps+0x44` |
| `weaponDelay` | `ps+0x40` | `ps+0x48` |
| `weaponstate` | `ps+0x158` | `ps+0x15c` |
| `weapAnim` | `ps+0x524` | `ps+0x528` |
| `weapon` | `ps+0x144` (byte) and `ps+0x134` (dword) | |

`0x425770` is `BG_GetWeaponDef(weaponIndex)`.

### The countdown, and why holding it is benign

`PM_Weapon` is `0x7694a0`, called only from `0x44e26f` and `0x44e3c8` (pmove).
The countdown is at `0x7696ad`:

```
007696a6  mov eax, [esp + 0x40]
007696aa  mov ebp, [eax + 0x28]        ; pm->msec
007696ad  mov ecx, [esp + 0x10]        ; int* pWeaponTime  (&ps->weaponTime or Left)
007696b1  mov eax, [ecx]
007696b3  test eax, eax
007696b5  je   0x7698dc                ; ALREADY ZERO -> skip the entire tick
007696bb  sub  eax, ebp
007696bd  mov  [ecx], eax              ; the one and only decrement
007696bf  ...
007696e4  cmp  dword ptr [eax], 0
007696e7  jg   0x7698dc                ; STILL POSITIVE -> skip all state transitions
```

Two facts fall out:

1. **The weapon state machine's only advance condition is `weaponTime` reaching
   zero or below.** Holding it positive stalls the machine and does nothing else.
   There is no separate animation-completion signal, no timeout, no watchdog
   anywhere in `PM_Weapon` that fires on a stalled timer. [BIN]
2. **`weaponTime == 0` is a distinguished value** — it short-circuits the whole
   tick. So freeze by holding a *positive* value, never by writing 0.

Immediately above the countdown, `0x769691` does `divss xmm1, xmm0` then
`call 0x536e90` (float→int) to produce `ebp`. That is the reload-speed multiplier
path — `perk_weapReloadMultiplier` (string `0x9bcd78`), i.e. **Speed Cola scales
the decrement, not the target**. A freeze that clamps the decrement therefore
interacts with Speed Cola exactly the way the stock reload does, which is what we
want for balance. [BIN][INF on the perk attribution]

The only other in-place decrement of `ps->weaponTime` anywhere in the image is
`0x76b0f3`, in `0x76b0c0`, and it is gated on `ps->pm_flags & 0x400000` plus a
pending-weapon match (`ps+0x4f0`, `ps+0x4f4`). That is the deferred weapon-switch
path, not reload. It will not fight you during a reload; it *will* fight you if a
weapon switch is queued, so suppress switching while held. [BIN]

### What can still bite

- **Prediction reconciliation.** `weaponTime`, `weaponstate`, `weaponDelay` and
  `weapAnim` are all netfields (589-entry playerState table). In single-player
  and in local zombies the "server" is in-process, so a freeze applied in pmove is
  applied on both sides and there is nothing to reconcile. If the freeze is
  applied **only** on the client-predicted playerState and not in the shared
  pmove path, every snapshot will snap it back. **The freeze must be applied
  inside `PM_Weapon`'s tick (or by clamping `pm->msec` for the weapon path), not
  by poking `ps->weaponTime` from cgame.** [INF] — this is the single most likely
  way to get a "it works for one frame then snaps" bug.
- **Co-op / networked zombies.** Not analysed. Treat as out of scope; the epic is
  scoped to offline SP and zombies. [OPEN]
- **Blocked-action gate.** `0x7691a5` blocks other weapon actions while
  `weaponstate` is in `0x12`, `0x13`, `0x14`, `0x15..0x1a`, or `0x29..0x2b` —
  the reload family. Held mid-reload, the player therefore cannot fire, switch
  or melee, which is the correct behaviour and is enforced for free. [BIN]
- **`ps+0x50`** is a secondary countdown decremented by `pm->msec` at `0x76aa74`
  and used at `0x76aa89`/`0x76aa96` to bounce `weaponstate` `0x18 -> 0x17`. It
  ticks independently of `weaponTime`. If you freeze `weaponTime` during a
  segmented reload you probably need to freeze this too, or the loop will
  re-trigger under you. **[INF]** — identified but not fully traced. Verify
  before shipping.

---

## The segmented-reload thread

This was flagged as the most productive lead, and it is: **the engine already
has an interruptible, incremental, per-shell reload**, and its parts are visible.

### It is real and it ships

Stakeout (`weapons/sp/ithaca_sp`): [ASSET]

```
segmentedReload   = 1        reloadAmmoAdd     = 1
reloadStartTime   = 1        reloadStartAddTime = 0.75    reloadStartAdd = 1
reloadTime        = 0.567    reloadAddTime      = 0.3
reloadEndTime     = 0.767
clipSize          = 4        rechamberTime      = 0.65
gunModel          = t5_weapon_ithaca_viewmodel
reloadStartAnim   = viewmodel_ithaca_reload_in
reloadAnim        = viewmodel_ithaca_reload_loop
reloadEndAnim     = viewmodel_ithaca_reload_out
```

`segmentedReload\1` is set on 30+ shipped weapons: `ithaca*`, `hs10*`, `ks23*`,
`china_lake*`, `python*` (revolver), and the whole `mk_*` masterkey family. It is
a **start / loop / end** animation triple, not a monolithic timer. [ASSET]

### The loop in code

- `0x766590` is the per-increment credit function. It resolves the weaponTime and
  weaponDelay pointers, fires `EV_EJECT_BRASS` (34) at `0x766632`, then either:
  - re-arms and returns (`0x7666cc`: `*pWeaponDelay = n; ret`) — **continue the
    loop**, or
  - falls through to `0x7666da` / `0x7666ea`: `call 0x518030` — **credit ammo**.
- `0x518030` is the ammo-credit function. It clamps the increment, adjusts the
  reserve, and fires `EV_RELOAD_ADDAMMO` (23) at `0x51819d`. [BIN]
- `0x766590`'s only callers are `0x768d92` and `0x768f00`, inside `0x768d60` and
  `0x768ea0` — both of which also fire `EV_RELOAD_END` (21) at `0x768e3b` /
  `0x768f9b`. So credit-one-shell and end-reload are the two exits of the same
  function pair. [BIN]
- `0x76aa30` is the loop-back: when `weaponstate == 0x18` it sets
  `weaponstate = 0x17` and calls `0x6979b0`, gated on `ps+0x50` reaching zero. [BIN]

Reload-family `weaponstate` values observed being written: `0x16` at `0x76a4d2`
(with `weaponTime` from the weapon def), `0x17` at `0x76a704` and `0x76aa96`,
`0x18` at `0x76a5ad` (with `weaponTime = 0`), `0x19` at `0x76a7eb`/`0x76a80d`,
`0x1b` at `0x76ac33`, `0x27` at `0x76a79f`. Full enum naming not established. [OPEN]

Event ids relevant here (from `evtable.txt`, all confirmed as `PM_AddEvent`
(`0x4a1d90`) arguments in this pass):
`EV_RELOAD=18`, `EV_RELOAD_FROM_EMPTY=19`, `EV_RELOAD_START=20`,
`EV_RELOAD_END=21`, `EV_RELOAD_START_NOTIFY=22`, `EV_RELOAD_ADDAMMO=23`,
**`EV_RECHAMBER_WEAPON=33`** (fired at `0x765ec4`, `0x765edb`),
`EV_EJECT_BRASS=34` (fired at `0x765dc5`, `0x766632`).

**Correction to prior work:** there *is* a chambering concept — `EV_RECHAMBER_WEAPON`,
plus `rechamberTime`, `rechamberBoltTime`, `rechamberWhileAds`, `rechamberAnim`
and `adsRechamberAnim` in the weapon files. It is a timed state, not a
round-in-chamber boolean, so the prior conclusion ("no chambering state") is
right in spirit and wrong in the letter. Recording it because a bolt-action VR
mod would want it. [BIN][ASSET]

### One unresolved thing about `weapfields.txt`

`weapfields.txt` gives `segmentedReload` at weapon-def offset `0x666`. I scanned
all of `.text` for any instruction with a `disp32` of `0x666`: the four-byte
pattern occurs only twice in the whole code section (`0x95202d`, `0x9908b2`) and
neither decodes as such an access. Same for `0x665` (`noPartialReload`) and
`0x667` (`noADSAutoReload`).

I do **not** conclude from this that segmented reload is unimplemented — the
field-offset table cannot be trusted field-by-field. Counter-evidence: `PM_Weapon`
reads `weapDef+0x3ac` and `weapDef+0x3f8` as millisecond times
(`0x76a70e`, `0x7686e9`), but `weapfields.txt` names those `proneRotR` (a float)
and `rocketModel` (a pointer). Meanwhile `weapDef+0x639` (`reloadWhileAds`) *is*
read in reload-gating contexts, and `weapDef+0x24` (`reloadTime`) is written into
`weaponTime` alongside anim id `0x12` at `0x76b189`. So the table appears to
merge more than one struct's fields.

**Action for whoever picks this up:** `weapfields.txt` offsets must be verified
per-field against actual code before being relied on. The name/offset pairs
themselves come straight from the `.data` parse table (12-byte entries:
`{const char* name; int offset; int type}`, `segmentedReload` entry at
`0xb6fce0`), so the *pairs* are sound; what is unsound is assuming every pair
describes the same base struct. [OPEN]

---

## Q5. Fallback routes if the bone hijack disappoints

**The fallback is strong — arguably strong enough to be the primary plan.** [BIN][ASSET]

### Viewmodel parts can be hidden by name, at runtime

- Weapon files carry a `hideTags` list of bone names, applied to the viewmodel.
  Parsed at `0x4ad317` (string `"hideTags"` at `0x9e070c`). [BIN][ASSET]
- `useweaponhidetags <weaponName>` is a **script builtin in both VMs** — server
  `0x7fe370`, client `0x8974d0` — which applies another weapon's hide set at
  runtime. Error string `"useweaponhidetags called with unknown weapon name %s"`
  at `0x9b4b18`. [BIN]
- `hidepart` / `showpart` / `showallparts` are entity builtins
  (`0x7f1e90` / `0x7f2000` / `0x7f2170`). `hidepart` resolves the part with
  `DObjGetBoneIndex` (`0x5f8a90`) on `Com_GetClientDObj(ent->entnum)` and errors
  `"cannot find part '%s' in entity model"` (`0x9ca338`). Since the viewmodel is
  a DObj at entnum `0x600 + lc`, the same primitive reaches it. [BIN]

So "hide the viewmodel's own magazine" is a one-line operation the engine already
performs on every extended-mag weapon variant, every match.

### The substitute magazine

`attach` / `detach` / `detachall` / `getattachtagname` builtins exist
(`0x7f1a10`, `0x7f1b00`, `0x7f1c10`, `0x7f1d30`), as do `gettagorigin` /
`gettagangles` in both VMs. A cosmetic magazine world model attached to a hand
tag, with `TAG_CLIP` hidden on the viewmodel, reproduces the felt outcome without
touching the skeleton at all. [BIN]

The tradeoff is honest: the bone route gives you the *actual* magazine with the
right material and the right silhouette and no seam; the attach route gives you a
separate model that must be visually matched and whose disappear/appear moments
must be hidden inside the animation. The bone route is better if it works, and
the evidence says it should. Build the hide-plus-attach fallback anyway — it is
cheap, it shares all the pose maths, and it is the graceful degradation for any
weapon whose viewmodel turns out not to have `tag_clip` (the 27 confirmed are the
extended-mag variants; base variants very likely have it too, but that was not
enumerated). [OPEN]

---

## Sketch of an implementation

Ordered so that each step is independently verifiable in-game.

**Step 0 — confirm the two inferences.** In a dev build, resolve `tag_clip` on the
viewmodel DObj, print the bone index and the +0x10 translation each frame during
a reload, and confirm it moves with the animation. Then write a constant offset
into it and confirm the magazine — and only the magazine — moves. That single
experiment settles `[INF]` items 2 and 3 (geometry binding, model-space
semantics) and the quat/scale layout.

**Step 1 — hide-and-replace prototype.** `useweaponhidetags`-style hide of
`TAG_CLIP` plus a cosmetic mag model attached to the controller. No timer
changes. This proves the pose maths and the viewmodel-model-space transform
without touching pmove. Ship-able on its own as a lesser effect.

**Step 2 — the freeze.** In `PM_Weapon`'s tick, clamp the weapon-time decrement
to zero while a "held" flag is set, never writing `weaponTime = 0`. Also clamp
`ps+0x50`. Suppress weapon switching while held (`pm_flags & 0x400000` path).
Verify against the stock reload that ammo still credits and Speed Cola still
scales.

**Step 3 — the bone hijack.** Per frame: force-evaluate the mag bone
(`0x4be720`), then write `bones[magIdx]` with the controller pose transformed into
viewmodel model space. Enter on the reload animation's mag-out moment, hold with
the freeze, release on the insert gesture.

**Step 4 — segmented weapons.** The shotguns/masterkey/revolver family is where
this pays off most, because the engine already credits ammo incrementally
(`0x766590` → `0x518030` → `EV_RELOAD_ADDAMMO`) and already loops
`weaponstate 0x18 -> 0x17` (`0x76aa30`). One shell per gesture is a much smaller
step from stock behaviour than one magazine per gesture.

**Step 5 — the balance requirement.** A dvar (or menu toggle) selecting
`reload_mode = classic | physical`, defaulting to `classic`, with `classic`
taking every code path exactly as shipped. Zombies at high rounds and Speed Cola
must behave identically in `classic`. No exceptions: the freeze must be a no-op
when the flag is clear, not a "fast" version of the same code.

---

## Address summary

| address | what | grade |
|---|---|---|
| `0x47e340` | `Com_GetClientDObj(entnum)`; table `0x2487d48` (u16), array `0x2489148`, stride `0x7c` | BIN |
| `0x570950` | viewmodel entity number = `0x600 + localClientNum` | BIN |
| `0x59c4a0` | `DObjGetRotTransArray(dobj)` → `dobj+0x54` | BIN |
| `0x5f8a90` | `DObjGetBoneIndex(dobj, token, &out, -1)` | BIN |
| `0x5a0e30` | `CG_DObjGetBoneIndex(ent, token)` → int, `<0` on fail | BIN |
| `0x4be720` | ensure bone evaluated this frame (memoised via `0x4faa70`, frame ctr `0x2865e84`) | BIN |
| `0x564ab0` | `DObjCalcSkel(dobj, partBits)` | BIN |
| `0xb75644` | per-entity-type pre-skel callback table, stride `0x30`, index `ent[0x15a]` | BIN |
| `0x4541d0` | `GetTagMatrix(ent, token)` → `boneArray + idx*32` | BIN |
| `0x6c9860` | renderer reads bone array live at draw time | BIN |
| `0x23a566a` | interned token for `"tag_clip"` (registered at `0x5ea87c`) | BIN |
| `0x7694a0` | `PM_Weapon` (callers `0x44e26f`, `0x44e3c8`) | BIN |
| `0x7696ad` | the sole `weaponTime` countdown; `==0` short-circuits; `>0` blocks transitions | BIN |
| `0x769691` | reload-speed multiplier scaling of the decrement | BIN |
| `0x76b0c0` | deferred weapon-switch timer, the only other `weaponTime` decrement (`0x76b0f3`) | BIN |
| `0x766590` | segmented reload per-increment; `EV_EJECT_BRASS` at `0x766632` | BIN |
| `0x518030` | ammo credit; `EV_RELOAD_ADDAMMO` at `0x51819d` | BIN |
| `0x76aa30` | segmented loop-back `weaponstate 0x18 -> 0x17`, gated on `ps+0x50` | BIN |
| `0x4a1d90` | `PM_AddEvent(ps, ev)` | BIN |
| `0x425770` | `BG_GetWeaponDef(weaponIndex)` | BIN |
| `0x7f1e90` / `0x7f2000` / `0x7f2170` | `hidepart` / `showpart` / `showallparts` | BIN |
| `0x7fe370` / `0x8974d0` | `useweaponhidetags` (server / client VM) | BIN |
| `0x8945d0` | `playviewmodelfx`, resolves a named tag on the viewmodel | BIN |
| `0x4ad317` | `hideTags` weapon-file field parse | BIN |

## How to reproduce the asset findings

```
# fastfile -> raw (T5: 'IWffu100', u32 version, raw zlib from offset 12)
python3 -c "import zlib;d=open('zone/Common/common_zombie.ff','rb').read();
open('/tmp/z.raw','wb').write(zlib.decompressobj().decompress(d[12:]))"
grep -aoE 'tag_[a-z0-9_]+' /tmp/z.raw | sort -u

# weapon files
unzip -o main/iw_00.iwd 'weapons/*' -d /tmp/wep
# weapon files are backslash-separated key\value pairs on one line
grep -lai 'segmentedReload\\1' /tmp/wep/weapons/sp/*
```

## Licence note

`xoxor4d/t5-rtx` and `JBShady/T4M-Enhanced` publish T5/T4 struct definitions with
no LICENSE file, so all rights are reserved. Nothing in this document was copied
from either; every offset above was derived from `BlackOps.exe` and from shipped
game data on this machine. Where a layout matches published documentation, that
is convergent observation of the same binary, and it is labelled `[INF]` wherever
this pass did not independently confirm it.
