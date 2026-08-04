# BO1 crosshair draw + spread — RE findings

Target: `BlackOps.exe` (32-bit PE, image base `0x400000`, md5 `2b179a57416680b60462c5af05552ea2`).
All addresses are **virtual addresses** (image-base-relative, i.e. what you see in a debugger with default base).

Clean-room: everything below was derived from this binary only (objdump disassembly, Ghidra decompiler,
and PE data-table parsing). No external decompilation, PDB or source dump was consulted.

---

## 0. TL;DR

| What | Where |
|---|---|
| Crosshair dispatcher (per-frame entry) | **`0x4100F0`** — `CG_DrawCrosshair(int localClientNum)` |
| **Dynamic 4-bar reticle draw** (the thing that expands) | **`0x774870`** |
| **Spread → screen-pixel offset calculator** | **`0x7747B0`** |
| Static centre reticle draw | `0x7746B0` |
| ADS overlay reticle draw | `0x774550` |
| Engine spread source (degrees) | **`0x406CB0`** = `BG_GetSpreadForWeapon(ps, weapDef, float* outMin, float* outMax)` |
| Spread state variable | `ps->aimSpreadScale` @ **`ps+0x52C`**, reachable as **`*(float*)(cg + 0x8A890)`** |
| `cg_t*` global | **`0x2FF5354`** (a pointer variable, not the struct) |

The expansion in **screen pixels** is not cached anywhere — it is recomputed on the stack every frame
inside `0x7747B0`. To get it, either call `0x406CB0` yourself and redo ~6 lines of math (recommended),
or detour `0x7747B0` and capture its output.

---

## 1. CORRECTION to a previously "known fact"

> Previously recorded: *"cg_drawCrosshair registered at 0x4A3EB8 (dvar ptr global 0x2FF66D4)"*

**That global is wrong.** MSVC schedules the `mov %eax, <global>` that stores a `Dvar_Register*`
return value **after the argument pushes for the *next* registration call**, so a naive
"nearest mov after the call" reading is off by one slot.

```
4a3e8a: push $0x9dd354 ; push $1 ; push $1 ; push $0xa124cc   ("cg_drawSnapshotTime")
4a3e9d: call 0x45bb20                      <- Dvar_RegisterBool("cg_drawSnapshotTime")
4a3ea2: push $0x9dd354 ; push $0x1001 ; push $1 ; push $0x9c5a74  ("cg_drawCrosshair")
4a3eb3: mov  %eax,0x2ff66d4                <- stores the *cg_drawSnapshotTime* pointer
4a3eb8: call 0x45bb20                      <- Dvar_RegisterBool("cg_drawCrosshair")
4a3ebd: push $0x9dd354 ; push $0x800 ; push $1 ; push $0xa095b0   ("cg_drawCrosshair3D")
4a3ece: mov  %eax,0x2f67bc4                <- stores the *cg_drawCrosshair* pointer   ***
4a3ed3: call 0x45bb20                      <- Dvar_RegisterBool("cg_drawCrosshair3D")
4a3ed8: mov  %eax,0x2f67ba8                <- stores the *cg_drawCrosshair3D* pointer ***
```

**VERIFIED** (by a full automated scan of `.text` that pairs `push $<name-string>` → `call <Dvar_Register*>`
→ next `mov %eax,<bss global>`, cross-checked against the string contents at each pointer):

| dvar | register call site | register fn | dvar-pointer global |
|---|---|---|---|
| `cg_drawSnapshotTime` | `0x4A3E9D` | `0x45BB20` Bool | `0x2FF66D4` |
| **`cg_drawCrosshair`** | `0x4A3EB8` | `0x45BB20` Bool | **`0x2F67BC4`** |
| **`cg_drawCrosshair3D`** | `0x4A3ED3` | `0x45BB20` Bool | **`0x2F67BA8`** |
| `cg_drawTurretCrosshair` | `0x4A3EEB` | `0x45BB20` Bool | `0x2F67C40` |
| `cg_drawCrosshairNames` | `0x4A3F09` | `0x45BB20` Bool | `0x2FF6728` |
| **`cg_crosshairAlpha`** | `0x4A47F7` | `0x679020` Float | **`0x2F67CCC`** |
| **`cg_crosshairAlphaMin`** | `0x4A482B` | `0x679020` Float | **`0x2FF5364`** |
| **`cg_crosshairDynamic`** | `0x4A4846` | `0x45BB20` Bool | **`0x2FF6718`** |
| `cg_crosshairEnemyColor` | `0x4A4861` | `0x45BB20` Bool | `0x2FF66B0` |
| `cg_drawGun` | `0x4A38B1` | `0x45BB20` Bool | `0x2F67F28` |
| `cg_fov` | `0x4A3945` | `0x679020` Float | `0x2FF6888` |
| `hud_missionFailed` | `0x4A65E7` | `0x45BB20` Bool | `0x2F67CF4` |
| `cg_forceSniperBobHack` | `0x4A6D8E` | `0x45BB20` Bool | `0x2F67CE0` |
| `show_reticle_during_swimming` | `0x661037` | `0x45BB20` Bool | `0x00BCD210` |
| `perk_weapSpreadMultiplier` | `0x54863C` | `0x679020` Float | `0x00BDF31C` |
| `cl_paused` | `0x4A56F0` | `0x651910` | `0x2FF68CC` |
| `cg_drawpaused` | `0x4A5727` | `0x45BB20` Bool | `0x2FF6844` |
| `zombiemode` | `0x82BC92` | `0x45BB20` Bool | `0x0243FDD4` |

Sanity anchor: `cg_crosshairAlphaMin` (`0x4A482B`) and `cg_crosshairDynamic` (`0x4A4846`) are
consecutive registrations in the same function, and `0x2F67BC4` is read exactly once in `.text`
(by the crosshair gate `0x7744B0`) — which matches the observed behaviour that forcing
`cg_drawCrosshair 0` kills the reticle. `0x2FF66D4` is **never read** in `.text`.

Also **VERIFIED**: `cg_drawCrosshair3D`'s pointer global `0x2F67BA8` is **never read anywhere in `.text`**.
The dvar is dead. That independently explains "there is no world-projected 3D crosshair underneath".

---

## 2. Call chain

```
CG_DrawActiveFrame/CG_Draw2D  0x655A50  (localClientNum)
  └─ 0x4100F0   CG_DrawCrosshair(localClientNum)          <- the dispatcher
       ├─ 0x7744B0   "should draw crosshair?" gate  (reads cg_drawCrosshair)
       ├─ 0x4816D0   crosshair screen position (x,y) from weapon aim vs view axis
       ├─ 0x774610   ADS crosshair fade/offset (adsCrosshairInFrac/OutFrac, adsAimPitch)
       ├─ 0x774550   draw ADS overlay reticle
       ├─ 0x7746B0   draw STATIC centre reticle   (weapDef->reticleCenter)
       └─ 0x774870   draw DYNAMIC 4-bar reticle   (weapDef->reticleSide)   <<<< THE ONE
              └─ 0x7747B0   spread(deg) -> pixel offset       <<<< THE SPREAD
                     └─ 0x406CB0  BG_GetSpreadForWeapon(ps, weapDef, &min, &max)
```

Other crosshair entry points reached from `0x4100F0` for special cases:
`0x774240` (vehicle/turret path, `ps->pm_flags & 0x4000`), `0x773F50` (turret, gated on
`cg_drawTurretCrosshair`), `0x774B60`.

`0x4100F0` has exactly one caller: `0x655BDD` inside `0x655A50`.
`0x7744B0` has exactly one caller: `0x4102FD` inside `0x4100F0`.

---

## 3. `0x7747B0` — the spread calculator (THE key function)

Non-standard calling convention (MSVC LTCG register allocation on a static function):

```
void CG_CalcCrosshairSpreadOfs(
        float *sideSize,    /* [esp+4]  in : {w,h} of one reticle bar, in virtual px  */
        float  scale,       /* [esp+8]  in : uniform scale (== 1.0f on the normal path)*/
        float *outOfs)      /* [esp+C]  out: {xOfs, yOfs} in 640x480 virtual px        */
    /* IMPLICIT: EDI = cg_t*  (i.e. *(void**)0x2FF5354)                                */
    /* IMPLICIT: ESI = WeaponDef* (from Weapon_GetDef, 0x425770)                       */
```

Reconstructed body (from the raw disassembly at `0x7747B0`–`0x774866`, all constants read
out of `.rdata`):

```c
float lo, hi;
BG_GetSpreadForWeapon(&cg->ps /* cg+0x8A364 */, weapDef, &lo, &hi);   /* 0x406CB0, degrees */

float spreadDeg = lo + (cg->ps.aimSpreadScale * (1.0f/255.0f)) * (hi - lo);
                                     /* 0x8A890(%edi)      * 0.00392156886 @0xA0ACD0     */
spreadDeg *= scale;                                        /*  mulss 0x20(%esp)          */

float px = tanf(spreadDeg * 0.0174532924f) /* DEG2RAD @0xA3AD40, tan == 0x9654FA */
         / cg->refdef.tanHalfFovY          /* 0x8C114(%edi)                              */
         * 240.0f;                         /* @0xA41FCC = half of the 480 virtual height */

if (px < (float)weapDef->reticleMinOfs)    /* 0x1B0(%esi), int                           */
    px = (float)weapDef->reticleMinOfs;

outOfs[0] = px - sideSize[0] * weapDef->hipReticleSidePos;  /* 0x4FC(%esi), float        */
outOfs[1] = px - sideSize[1] * weapDef->hipReticleSidePos;
```

Verbatim key instructions (VERIFIED, objdump):

```
7747c4: lea    0x8a364(%edi),%edx        ; &cg->ps
7747cc: call   0x406cb0                  ; BG_GetSpreadForWeapon(ps, weapDef, &lo, &hi)
7747d1: movss  0x1c(%esp),%xmm1          ; lo
7747d7: movss  0x8a890(%edi),%xmm0       ; ps->aimSpreadScale        <<<<
7747df: mulss  0xa0acd0,%xmm0            ; * 1/255
7747e7: movss  0x24(%esp),%xmm2          ; hi
7747ed: subss  %xmm1,%xmm2               ; hi-lo
7747f1: mulss  %xmm2,%xmm0
7747f5: addss  %xmm1,%xmm0               ; lo + f*(hi-lo)
7747f9: mulss  0x20(%esp),%xmm0          ; * scale
7747ff: mulss  0xa3ad40,%xmm0            ; * DEG2RAD
77480a: cvtps2pd %xmm0,%xmm0
77480d: call   0x9654fa                  ; tan(double)->double
774812: cvtsi2ss 0x1b0(%esi),%xmm1       ; (float)weapDef->reticleMinOfs
77481e: divss  0x8c114(%edi),%xmm0       ; / tanHalfFovY
774826: mulss  0xa41fcc,%xmm0            ; * 240
77482e: comiss %xmm0,%xmm1 ; jbe ...     ; px = max(px, reticleMinOfs)
77483b: mulss  0x4fc(%esi),%xmm1         ; sideSize[0] * hipReticleSidePos
77484a: movss  %xmm2,(%ebx)              ; outOfs[0]
774860: movss  %xmm0,0x4(%ebx)           ; outOfs[1]
```

### Constants (VERIFIED, read from `.rdata`)

| VA | f32 | meaning |
|---|---|---|
| `0xA0ACD0` | `0.00392156886` | `1/255` — aimSpreadScale is stored 0..255 |
| `0xA3AD40` | `0.0174532924` | π/180 |
| `0xA41FCC` | `240.0` | half of the 480-unit virtual screen height |
| `0x9B449C` | `0.5` | |
| `0x9CDCBC` | `0.125` | bar nudge |
| `0xA4E394` | `1.0` | |
| `0xA2FDCC` | `0.01` | alpha epsilon |
| `0xA13A24` / `0x9DEFB4` / `0xA3F8F8` | `90` / `180` / `270` | bar rotations (deg) |
| `0x9D6508` | `15000.0` | trace distance for the depth probe |
| `0x9DB5F0` | `10000.0` | depth clamp |
| `0x9A84FC` / `0x9D60EC` | `-320` / `-240` | half virtual screen (640x480) |

---

## 4. `0x406CB0` — `BG_GetSpreadForWeapon` (the engine's spread, in DEGREES)

Plain **cdecl**, 4 args. Fully usable from a mod as-is.

```c
void BG_GetSpreadForWeapon(playerState_t *ps, WeaponDef *weap, float *outMin, float *outMax);
```

Body (Ghidra decompile, field names resolved from the engine's own tables — see §6):

```c
if (ps->spreadOverrideState /*+0x174*/ == 2) {
    *outMin = *outMax = (float)ps->spreadOverride /*+0x170*/;
} else {
    float h = ps->viewHeightCurrent;               /* +0x190, float                       */
    float t;
    if (h <= 40.0f) {                              /* @0xA33860 = 40 (crouch height)      */
        t = (h - 11.0f) * (1.0f/29.0f);            /* @0x9EDF14=11, @0x9F76B8=0.03448276  */
        *outMin = lerp(weap->hipSpreadProneMin,  weap->hipSpreadDuckedMin, t);
        *outMax = lerp(weap->hipSpreadProneMax,  weap->hipSpreadDuckedMax, t);
    } else {
        t = (h - 40.0f) * (1.0f/20.0f);            /* @0x9EFB40 = 0.05                    */
        *outMin = lerp(weap->hipSpreadDuckedMin, weap->hipSpreadStandMin, t);
        *outMax = lerp(weap->hipSpreadDuckedMax, weap->hipSpreadMax,      t);
    }
}
if (weap->fireType /*+0x30*/ == 5 && ps->[+0x1B0] > 1) {   /* multi-barrel/akimbo-ish */
    float m = FUN_00966c00();                              /* not chased               */
    *outMin *= m; *outMax *= m;
}
if (ps->spreadOverrideState == 1) *outMax = (float)ps->spreadOverride;

if (zombiemode /*0x243FDD4*/ && (ps->[+0x4FC] & 0x0C000000) && ps->fWeaponPosFrac /*+0x168*/ < 1.0f) {
    float m = perk_weapSpreadMultiplier->current.value;     /* dvar ptr @0x00BDF31C     */
    *outMin *= m; *outMax *= m;
}
```

Note the prone/duck/stand blend key is the **view height** (11 / 40 / 60), not a stance enum.
`h == 11` → prone, `h == 40` → crouched, `h == 60` → standing. (Values 11 and 40 are VERIFIED
constants; 60 is INFERRED from `1/20` giving `t == 1` at `h == 60`.)

---

## 5. `0x774870` — the dynamic reticle draw

```c
void CG_DrawCrosshairDynamic(
        int   localClientNum,   /* [esp+4]  -> screen-placement table &0xC78DA0 + n*0x78 */
        float color[4],         /* [esp+8]                                                */
        float x,                /* [esp+C]  virtual-screen x of crosshair centre          */
        float y,                /* [esp+10]                                               */
        float depth,            /* [esp+14] world depth for the 3D-projected UI draw      */
        float scale)            /* [esp+18] == 1.0f on the normal path                    */
    /* IMPLICIT: EAX = weapon index (byte), fed to 0x444740 and 0x425770 (Weapon_GetDef)  */
```

Reconstructed (Ghidra + objdump):

```c
weapDef = Weapon_GetDef(weaponIndex);            /* 0x425770 */
if (!weapDef->reticleSide /*+0x1A4, material*/) return;

/* --- alpha --------------------------------------------------------------- */
float adsFade = 1.0f;
if (CG_GetAdsFrac(cg, &t) /*0x66FA20*/)
    adsFade = 1.0f - t / max(1.0f, *(float*)(0x444740(weaponIndex) + 0x78));

float alpha = (1.0f - cg->ps.aimSpreadScale * (1/255.0f))   /* 0x8A890 */
            * cg_crosshairAlpha->current.value              /* 0x2F67CCC + 0x18 */
            * adsFade;
if (alpha < cg_crosshairAlphaMin->current.value)            /* 0x2FF5364 + 0x18 */
    alpha = cg_crosshairAlphaMin->current.value;
if (alpha < 0.01f) return;                                  /* nothing drawn        */

/* --- geometry ------------------------------------------------------------ */
float side[2];
side[0] = side[1] = (float)weapDef->reticleSideSize /*+0x1AC, int*/ * scale;

float ofs[2];
CG_CalcCrosshairSpreadOfs(side, scale, ofs);     /* 0x7747B0, EDI=cg, ESI=weapDef */

float nudge = side[0] * 0.125f;
float half  = side[0] * 0.5f;

/* four bars, rotations 0 / 90 / 180 / 270 degrees */
CG_DrawRotatedPic(place, x - half,                        (y - side[0]) - ofs[1] - nudge, depth, side[0], side[0], 2,2,   0, color, reticleSide);
CG_DrawRotatedPic(place, x + ofs[0],                       y - half,                      depth, side[0], side[0], 2,2,  90, color, reticleSide);
CG_DrawRotatedPic(place, x - half - nudge,                 y + ofs[1],                    depth, side[0], side[0], 2,2, 180, color, reticleSide);
CG_DrawRotatedPic(place, (x - side[0]) - ofs[0] - nudge,   y - half - nudge,              depth, side[0], side[0], 2,2, 270, color, reticleSide);
```

`CG_DrawRotatedPic` is `0x694830` → `0x5CD7B0` (virtual→real screen placement) + `0x65DFA0` (emit).
`0x7746B0` / `0x774550` / `0x774240` / `0x773F50` all use `0x4DE340` (non-rotated stretch-pic).

**So `ofs[0]` is the horizontal half-expansion and `ofs[1]` the vertical half-expansion,
in 640x480 virtual-screen units, measured from the crosshair centre to the inner edge of the bar
(the `- side*hipReticleSidePos` term already backs the bar's own size out).**

---

## 6. Struct offsets — how they were established

### 6.1 WeaponDef field table (VERIFIED)

There is a `{const char *name; int offset; int type}` table, **867 rows, 12-byte stride**, at
**`0xB6E6D8` .. `0xB70F7C`** in `.data` (found by searching for a pointer to the string
`"adsCrosshairInFrac"` @ `0xA3E5DC`).

The offsets in that table are **`0xE4` larger** than the offsets the code uses off the pointer
returned by `Weapon_GetDef` (`0x425770`). Four independent confirmations of the `0xE4` delta:

| table name | table offset | code offset used | where |
|---|---|---|---|
| `hipSpreadStandMin` | `0x5B0` | `0x4CC` | `0x406CB0` |
| `hipSpreadDuckedMin` | `0x5B4` | `0x4D0` | `0x406CB0` |
| `hipSpreadProneMin` | `0x5B8` | `0x4D4` | `0x406CB0` |
| `hipSpreadMax` / `DuckedMax` / `ProneMax` | `0x5BC/5C0/5C4` | `0x4D8/4DC/4E0` | `0x406CB0` |
| `crosshairColorChange` | `0x588` | `0x4A4` | `0x773DB0` |
| `adsCrosshairInFrac` | `0x764` | `0x680` | `0x774610` |
| `adsCrosshairOutFrac` | `0x768` | `0x684` | `0x774610` |
| `dualWield` | `0x64E` | `0x56A` | `0x7744B0` |

Crosshair-relevant WeaponDef fields (code-relative offsets, i.e. what to use off `0x425770`'s return):

| code offset | name | type | used by |
|---|---|---|---|
| `+0x01C` | `weaponType` | int | `0x7746B0` |
| `+0x030` | `fireType` | int | `0x406CB0` |
| `+0x1A0` | `reticleCenter` | Material* | `0x7746B0`, `0x774550`, `0x774240` |
| `+0x1A4` | `reticleSide` | Material* | **`0x774870`** |
| `+0x1A8` | `reticleCenterSize` | int | `0x7746B0`, `0x774550` |
| `+0x1AC` | `reticleSideSize` | int | **`0x774870`** |
| `+0x1B0` | `reticleMinOfs` | int | **`0x7747B0`** (pixel floor for the spread) |
| `+0x1B4` | `activeReticleType` | enum | — |
| `+0x4A4` | `crosshairColorChange` | bool | `0x773DB0` (colour) |
| `+0x4CC..0x4E0` | `hipSpread{Stand,Ducked,Prone}Min` / `hipSpread{Max,DuckedMax,ProneMax}` | float, **degrees** | `0x406CB0` |
| `+0x4FC` | `hipReticleSidePos` | float | **`0x7747B0`** |
| `+0x56A` | `dualWield` | bool | `0x7744B0` |
| `+0x67C` | `adsAimPitch` | float, degrees | `0x774610` |
| `+0x680` | `adsCrosshairInFrac` | float | `0x774610` |
| `+0x684` | `adsCrosshairOutFrac` | float | `0x774610` |

### 6.2 playerState_t (VERIFIED against the netfield table at `0xA5C6B0`, 60-byte stride)

| offset | name | source |
|---|---|---|
| `+0x144` | `weapon` (byte) | code (`0x7744B0`, `0x4540B0`) — INFERRED name |
| `+0x158` | `weaponstate` | INFERRED (neighbour of the VERIFIED `weaponstateLeft`) |
| `+0x15C` | `weaponstateLeft` | netfield table — VERIFIED |
| `+0x168` | `fWeaponPosFrac` | netfield table — VERIFIED |
| `+0x170` | `spreadOverride` | netfield table — VERIFIED |
| `+0x174` | `spreadOverrideState` | INFERRED from use in `0x406CB0` |
| `+0x190` | view height (11/40/60) | INFERRED from the 11/40 constants |
| `+0x444` | `viewlocked_entNum` | netfield table — VERIFIED |
| **`+0x52C`** | **`aimSpreadScale`** (float, 0..255) | netfield table — VERIFIED |

### 6.3 cg_t (all VERIFIED by use, names INFERRED)

| offset | meaning |
|---|---|
| `+0x8A364` | `playerState_t predictedPlayerState` (so `aimSpreadScale` = `cg+0x8A890`) |
| `+0x8A3B4` | a millisecond timer (used `% 1000` for the pulsing static reticle) |
| `+0x8A444` | `ps->pm_flags` mirror (bits `0x300` vehicle, `0x4000` turret) |
| `+0x8A4CC` | ADS fraction (0..1); `== 1.0f` means fully ADS |
| `+0x8C09C` | ADS direction flag (0 → use `adsCrosshairOutFrac`, else `adsCrosshairInFrac`) |
| `+0x8C110` | `refdef.tanHalfFovX` |
| **`+0x8C114`** | **`refdef.tanHalfFovY`** |
| `+0x8C118` | fov in degrees |
| `+0x8C120..0x8C12C` | `refdef.vieworg` |
| `+0x8C134..0x8C15C` | `refdef.viewaxis[3][3]` |

`0x8C110/0x8C114` VERIFIED as tan-of-half-fov: written at `0x60BCFF`/`0x60BD15` from
`tan(fovDeg * 0.0174532924 * 0.5)`, with `0x8C114 = tan(halfFov)*0.75` (0.75 = 480/640) and
`0x8C110 = 0x8C114 * aspect`. Consistent with the projection in `0x4816D0`
(`x` divided by `0x8C110` and scaled by `-320`, `y` divided by `0x8C114` and scaled by `-240`).

`0x9654FA` VERIFIED to be `tan(double)->double` in xmm0: it is the same routine used to build
`tanHalfFov` from `fov*DEG2RAD*0.5` at `0x60BCDF`.

---

## 7. The gate — `0x7744B0`

```c
/* IMPLICIT: ESI = &cg->ps */
bool CG_ShouldDrawCrosshair(void)
{
    /* 0x674400 is a stub: `xor al,al; ret` — always 0 in this build (VERIFIED) */
    if ( (cl_paused->current.integer != 0 && cg_drawpaused->current.bool)   /* 0x2FF68CC, 0x2FF6844 */
      || !cg_drawCrosshair->current.bool                                    /* 0x2F67BC4 <<<< */
      ||  hud_missionFailed->current.bool                                   /* 0x2F67CF4 */
      ||  ps->weaponstate == 0x12 || ps->weaponstate == 0x13 || ps->weaponstate == 0x14 )
        return false;

    int st = ps->weaponstate;
    if (Weapon_GetDef(ps->weapon)->dualWield) { if (ps->weaponstateLeft == 0xB) return false; }
    else                                      { if (st == 0xB) return false; }

    if (st >= 1 && st <= 5) return false;   /* raising / dropping / reloading states */
    return true;
}
```

---

## 8. What this means for the mod

### 8a. Reading the engine-computed expansion

The pixel expansion is **not** stored anywhere — `0x7747B0` computes it into stack locals each
frame. Three options, in order of preference:

**Option A (recommended) — recompute from engine state using the engine's own `BG_` function.**
Everything needed is reachable without hooking:

```c
typedef void (__cdecl *BG_GetSpreadForWeapon_t)(void *ps, void *weap, float *mn, float *mx);
typedef void*(__cdecl *Weapon_GetDef_t)(int weaponIndex);
typedef int  (__cdecl *BG_GetActiveWeapon_t)(void *ps);   /* 0x4540B0 */

#define CG            (*(char**)0x2FF5354)
#define CG_PS         (CG + 0x8A364)
#define AIMSPREADSCALE (*(float*)(CG + 0x8A890))
#define TANHALFFOVY    (*(float*)(CG + 0x8C114))

float crosshairSpreadPx(void)
{
    void *weap = ((Weapon_GetDef_t)0x425770)(((BG_GetActiveWeapon_t)0x4540B0)(CG_PS));
    float mn, mx;
    ((BG_GetSpreadForWeapon_t)0x406CB0)(CG_PS, weap, &mn, &mx);

    float deg = mn + (AIMSPREADSCALE * (1.0f/255.0f)) * (mx - mn);
    float px  = tanf(deg * 0.0174532924f) / TANHALFFOVY * 240.0f;   /* 640x480 virtual */

    float minOfs = (float)*(int*)((char*)weap + 0x1B0);             /* reticleMinOfs   */
    return px < minOfs ? minOfs : px;
}
```

`px` is a **half-extent in 480-tall virtual-screen units** — the distance from crosshair centre to
the inner edge of a bar, before the `- reticleSideSize * hipReticleSidePos` inset that `0x7747B0`
applies. For a VR reticle you almost certainly want the raw angle instead:

```c
float crosshairSpreadDegrees(void);   /* = `deg` above, i.e. the full engine spread angle */
```

which is resolution- and FOV-independent and is what you want to project into the VR view.

**Option B — detour `0x7747B0`** and copy `outOfs[0..1]` on the way out. Gives you exactly the
number the engine used, but you must preserve `EDI`/`ESI` (non-standard convention) and the
function is only 0xB7 bytes, so a trampoline needs care.

**Option C — detour `0x774870`** (plain cdecl, 6 stack args + `EAX` = weapon index). This is the
natural place to *both* read and suppress: you get `x`, `y`, `color[4]`, `depth`, `scale` handed to
you, and you can call `0x7747B0` yourself (set `EDI`/`ESI` first) or just return without drawing.

### 8b. Suppressing the original

- Cheapest: force the `cg_drawCrosshair` dvar to 0 — write `0` to
  `*(unsigned char*)(*(void**)0x2F67BC4 + 0x18)` (it's a `Dvar_RegisterBool`, so `current` is a byte).
  This kills the whole `0x4100F0` body via the `0x7744B0` gate. **Already empirically confirmed**
  by the project to remove the crosshair entirely.
- Finer-grained: `ret`-patch or detour `0x774870` (dynamic bars) and `0x7746B0` (static centre)
  independently, leaving the ADS overlay (`0x774550`) intact if you want the scope reticle.
- Note the turret/vehicle paths (`0x774240`, `0x773F50`) bypass `0x7744B0` — `0x773F50` is gated on
  `cg_drawTurretCrosshair` (`0x2F67C40`) instead, and `0x774240` is not gated on any crosshair dvar
  at all. If the mod must be airtight in vehicles, handle those too.

---

## 9. VERIFIED vs INFERRED — explicit list

### VERIFIED (directly readable from the binary; I re-derived each one)

- Every dvar-name → dvar-pointer-global row in §1, by an automated `.text` scan that pairs the
  pushed name string with the register call and the following `mov %eax,<global>`, plus manual
  disassembly reading of the `0x4A3E8A..0x4A3F09` block.
- `0x2F67BC4` (cg_drawCrosshair pointer) is read exactly once in `.text`, at `0x7744D0` inside `0x7744B0`.
- `0x2F67BA8` (cg_drawCrosshair3D pointer) is never read in `.text`.
- `0x4100F0` is called only from `0x655BDD`; `0x7744B0` only from `0x4102FD`.
- The full instruction sequence of `0x7747B0` quoted in §3, including `movss 0x8A890(%edi),%xmm0`.
- `0x774870` and `0x7746B0` both read `cg+0x8A890`.
- All float constants in §3 (read out of `.rdata` at the exact VAs the instructions reference).
- The four bar rotations 0/90/180/270 in `0x774870`.
- The WeaponDef name/offset table at `0xB6E6D8` (867 rows) and the `0xE4` delta, via four
  independent name↔code-offset matches.
- `aimSpreadScale` at `ps+0x52C`, `fWeaponPosFrac` at `+0x168`, `spreadOverride` at `+0x170`,
  `weaponstateLeft` at `+0x15C`, `viewlocked_entNum` at `+0x444` — from the netfield table at `0xA5C6B0`.
- `cg+0x8A364` is `&cg->ps` — `lea 0x8a364(%ebx),%esi` at `0x42579F` and `0x7747C4`, with the
  result passed to functions that index `+0x144`/`+0x158`/`+0x52C`.
- `0x8C114` is `tan(fov/2)`-derived (written at `0x60BCFF` from `tan(fov*DEG2RAD*0.5)`).
- `0x9654FA` is `tan` (double in/out via xmm0), by the same `0x60BCDF` site.
- `0x674400` is a stub returning 0 (`xor al,al; ret`).
- `0x425770` is `WeaponDef *Weapon_GetDef(int index)` — `mov 0xBE19A8(,%eax,4),%ecx; mov 0x8(%ecx),%eax; ret`.
- `perk_weapSpreadMultiplier` dvar pointer at `0x00BDF31C`, used in `0x406CB0`.

### INFERRED (reasoned, not proven)

- **Function names.** `CG_DrawCrosshair`, `BG_GetSpreadForWeapon`, `CG_ShouldDrawCrosshair` etc. are
  my labels based on behaviour. Only the *dvar* and *weapon-field* and *netfield* names come from
  the binary's own strings.
- `ps+0x190` is the view height. Confidence high (the 11.0 and 40.0 constants and the 1/29, 1/20
  reciprocals are exactly a prone/crouch/stand blend) but I did not find a writer to confirm.
- `ps+0x174` = `spreadOverrideState`. Named by analogy to the VERIFIED `spreadOverride` at `+0x170`.
- `ps+0x158` = `weaponstate`. VERIFIED that `+0x15C` is `weaponstateLeft`; `+0x158` being the
  right-hand equivalent is an inference from the dual-wield branch in `0x7744B0`.
- `cg+0x8A4CC` = ADS fraction. Inferred from being compared to 1.0 and 0.0 and fed to `0x774610`
  alongside `adsCrosshairInFrac/OutFrac`.
- `scale` (`0x774870` param 6 / `0x7747B0` param 2) is `1.0f` on the normal path. Ghidra shows the
  local is only ever assigned `1.0`; I did not exhaustively audit every store to that stack slot.
- `weapDef+0x78` (from `0x444740`, a *different* accessor than `0x425770`) being the ADS transition
  duration — inferred from `adsTime / that` being used as a 0..1 fade.
- `FUN_00966C00` (the `fireType == 5` multiplier in `BG_GetSpreadForWeapon`) was not chased.
- The `0x4816D0` output being the crosshair *screen position* (weapon aim direction projected into
  the view) rather than something else — strongly implied by the maths (dot products with
  `viewaxis`, divided by `tanHalfFov`, scaled by `-320`/`-240`) but not confirmed against a
  running game.

### NOT DONE

- No runtime verification. Nothing here has been checked against a live process; all of it is
  static analysis. The `cg_drawCrosshair 0` behaviour is the project's own prior observation.
- `adsCrosshairInFrac` / `adsCrosshairOutFrac` are **weapon-file fields, not dvars** — there is no
  dvar of those names in this binary (VERIFIED: the strings appear only in the WeaponDef field
  table at `0xB70400`/`0xB7040C`, and no `Dvar_Register*` call references them).
- `cg_crosshairDynamic` (`0x2FF6718`) is read in `0x4100F0` at `0x4103A0`, where clearing it forces
  the crosshair x-offset to 0 and replaces the y-offset with the ADS-derived value. I did not fully
  disentangle which of the two stack slots is x and which is y at that point; treat that one branch
  as unresolved.
