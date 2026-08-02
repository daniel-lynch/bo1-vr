# BAC-281 — Render camera hook plan (BlackOps.exe, 32-bit SP/Zombies)

**Status: analysis only. Nothing was launched, nothing was patched, no git operation was run.**
All findings below come from static analysis of
`/mnt/games/steam/steamapps/common/Call of Duty Black Ops/BlackOps.exe` (read-only) using the
repo's own tooling in `research/engine/` plus a resyncing capstone sweep of `.text`
(1,848,523 instructions decoded).

Every claim is tagged **MEASURED** (with the instruction addresses that prove it) or
**ASSUMED** (with the cheap test that would settle it).

---

## 0. Read this first — two corrections to the brief

### 0.1 The documents the brief told me to read do not exist

The brief says to start from `docs/address-map.md`, `docs/stereo-path.md` and
`docs/kisakblack-assessment.md`. **There is no `docs/` directory in this repo** and none of
those three files exist anywhere in it. `experiments/10_d3d9hook/RESULTS.md` does exist and was
read. The repo's only prose is `README.md` (which still says "It hooks nothing in the game yet"
and documents experiments 0–5 only) and `experiments/*/RESULTS.md`.

So I could not build on the stated ground truth; I re-derived everything from the binary. Where
the brief stated a fact, I verified it independently and say so below. **This document is
therefore self-contained** — it does not depend on the missing files.

The four addresses the brief supplied were all confirmed:

| Brief's claim | Verdict | Evidence |
|---|---|---|
| `R_IsStereoActive = 0x6b8b20` | **MEASURED, correct** | `0x6b8b20: mov al,[0x396346b] / ret` — a 6-byte accessor |
| `g_stereoActive = 0x396346b` | **MEASURED, correct** | written at `0x6b75e5`, `0x6b761d`; read at `0x6b8b21`, `0x6d28dd`, `0x6eb379`, `0x6eb406`, `0x723af3` |
| `Dvar_FindVar = 0x5AE810` | **MEASURED, consistent** | null/empty-string guard then hash lookup `0x68b370` → `0x862280` |
| `CONST_SRC_CODE_EYEOFFSET = 191` | **MEASURED, correct** | code-constant name table at `0xb49e8c`, stride 0x14 `{const char *name; u32 index; 0,0,0}`; entry `0xb49f68` = `"eyeOffset"`, index `191 (0xBF)` |

I did **not** re-derive the `eyeOffset`-is-not-an-eye-offset correction — I took the brief's word
for it and built nothing on `eyeOffset`. The plan below never touches constant 191.

### 0.2 The engine's built-in "stereo" is NVIDIA 3D Vision and is useless to us

**MEASURED.** `g_stereoActive` is set inside the D3D9 device-creation function `0x6b7540`:

```
006b7540  push 0xb4f678          ; "Creating Direct3D device..."
...
006b75df  push 0x396346c         ; &stereoHandle
006b75e4  push ecx               ; = [0x3963448]  (the IDirect3DDevice9*)
006b75e5  mov byte ptr [0x396346b], 0
006b75f6  call 0x98efc6          ; -> jmp [0xba4dcc]  (nvapi.dll import thunk)
...
006b760d  call 0x98efde          ; -> jmp [0xba4dec]  (nvapi.dll import thunk)
006b761d  mov byte ptr [0x396346b], cl
```

`0x98efc6` and `0x98efde` are IAT thunks into **`nvapi.dll`** (strings `"nvapi.dll"` @ `0xb302b0`,
`"nvapi_QueryInterface"` @ `0xb302ba`; error strings "…no stereo hardware present…" @ `0xb34a38`).
The shape is `NvAPI_Stereo_CreateHandleFromIUnknown(dev,&h)` then `NvAPI_Stereo_IsActivated(h,&b)`.

The six callers of `R_IsStereoActive` (`0x4103c7`, `0x4bf709`, `0x773bb2`, `0x7742ec`, `0x774dcb`,
`0x77516b`) are all **2D/HUD depth placement**, not view duplication. Example, `0x775165`:

```
0077516b  call 0x6b8b20            ; R_IsStereoActive
00775172  je   0x7751b0            ; not stereo -> do nothing
00775174  movss xmm0,[esi+0x8a4cc]
...
007751a8  call 0x4fbd90            ; Dvar_SetFloat(dvar@[0x3b1fc54], lerped value)
```

Supporting dvars: `r_convergence`, `r_use_driver_convergence`, `r_stereoTurretShift`, and the help
string `"Stereo convergence."` @ `0xb5554c`.

**Conclusion: the driver duplicates the draw calls; the engine never builds two views.** There is
no per-eye camera to borrow here. Do not spend time on `g_stereoActive`. The plan below ignores it
entirely and drives the engine's *normal* single-view path twice.

---

## 1. The hook point

### 1.1 Primary hook — `R_SetViewParms` @ **`0x6C7F80`**

This is the function the brief calls `R_SetViewParmsForScene`. It is the single place where the
camera position, orientation, view matrix and projection matrix are all produced, and it is the
**only** producer of the `GfxViewParms` block that the rest of the renderer consumes.

**Signature (MEASURED — non-standard, register arguments, no stack arguments):**

```c
// LTCG custom convention. edi = out, esi = in. Nothing on the stack. Plain `ret`.
void R_SetViewParms(/* EDI */ GfxViewParms *out, /* ESI */ const refdef_t *in);
```

Proof of the convention — the function reads `edi` and `esi` without ever initialising them
(`0x6c7f88 push edi` as the memset destination; `0x6c7f8e fld [esi+0x20]`), and every call site
loads both registers immediately before the call and pushes nothing:

| Call site | Setup | Enclosing function |
|---|---|---|
| `0x6c8c5f` | `mov edi,0x396f730` … `esi` = refdef | `0x6c8c40` |
| `0x6c8d96` | `mov edi,ebx` (fresh view slot), `esi` = refdef | `0x6c8cd0` |
| `0x6c8e73` | `lea edi,[edx+ecx+0x88000]`, `mov esi,[esp+0x14ec]` | `0x6c8ea0`-region |
| `0x6c8f5b` | `mov edi,ebp` ; `mov esi,ebx` | `0x6c8ea0`-region |
| `0x6cef64` | `mov edi,0x396f730` ; `lea esi,[esp+0x80]` | `0x6cee30` |

`0x6c8e65 inc eax` / `0x6c8e6d mov [ecx+0x16cbe0],eax` shows the call sites bump-allocate the
output slot; see §3.3.

**Because the arguments are in EDI/ESI, a MinHook detour with a C signature will not work.** The
detour must be `__declspec(naked)` (or a hand-written stub) that preserves and forwards EDI/ESI.
See §3.1 for the exact stub.

### 1.2 The call path from the frame loop (MEASURED)

```
CG_DrawActiveFrame            0x5C3420      (callers: 0x495084, 0x7A1471)
  │   ebp = cg = *(void**)0x2FF5354
  │   builds cg->refdef at cg+0x8C100
  ├─ 0x5C3654  lea eax,[ebp+0x8C100] ; call 0x6C8C40   <-- R_RenderScene(refdef)
  │
R_RenderScene (thin)          0x6C8C40      (single caller: 0x5C3654)
  ├─ 0x6C8C6C  R_SetSceneDofParms(0x396ECC4, refdef)      = 0x6C88D0
  ├─ 0x6C8C71  copies refdef+0x20..0x28 -> globals 0x3AC3060/64/68  (world view origin)
  ├─ 0x6C8C98  copies refdef+0x58 (time) -> 0x3AC3058, *0.001 -> 0x3AC305C
  └─ 0x6C8CC4  jmp 0x6C85D0                (sun / vision-set upload)

R_RenderSceneInternal         0x6C8CD0      <-- the real body, 0x14E8-byte frame
  ├─ 0x6C8D18  refdef+0x20..0x28 -> globals 0x396A644/648/64C   (vieworg)
  ├─ 0x6C8D42  refdef+0x34..0x3C -> globals 0x396A650/654/658   (viewaxis[0], forward)
  ├─ 0x6C8D69  call 0x6C2170                  (light/vis probe at the view origin)
  ├─ 0x6C8D6F  call 0x6C1F60(&refdef->vieworg)
  ├─ 0x6C8D74  ebx = frontEndData + 0x88000 + n*0x140 ; n = [frontEndData+0x16CBE0]++   <-- view slot
  ├─ 0x6C8D96  mov edi,ebx ; call 0x6C7F80     ***** R_SetViewParms(viewParms, refdef) *****
  ├─ 0x6C8DA1  call 0x6C8090(refdef, &localViewInfo)   (viewport / scissor, §4.3)
  ├─ 0x6C8DB6  call 0x6C88D0                  (DOF)
  ├─ 0x6C8DD0  call 0x72A6B0(&viewParms[0x100])
  └─ 0x6C8E2E  call 0x6C6450                  R_RenderView(viewInfo, …, viewParms, …, refdef, 0)
```

`frontEndData` = `*(char**)0x3B3708C` (358 references across the renderer — MEASURED).

### 1.3 What `0x6C7F80` reads and writes (MEASURED, instruction by instruction)

```
006c7f81  push 0x140 / push 0 / push edi / call 0x965480   ; memset(out, 0, 0x140)
006c7f8e  out[0x100] = in[0x20]        ; origin.x
006c7fa8  out[0x104] = in[0x24]        ; origin.y
006c7fb5  out[0x108] = in[0x28]        ; origin.z
006c7fb8  out[0x10C] = 1.0             ; origin.w   (const 0xB4623C = 1.0f)
006c7fc4  out[0x110] = in[0x34]        ; axis[0].x  (forward)
006c7fca  out[0x114] = in[0x38]
006c7fd0  out[0x118] = in[0x3C]
006c7fd6  out[0x11C] = in[0x40]        ; axis[1].x  (left)
006c7fdf  out[0x120] = in[0x44]
006c7fe8  out[0x124] = in[0x48]
006c7ff1  out[0x128] = in[0x4C]        ; axis[2].x  (up)
006c7ffa  out[0x12C] = in[0x50]
006c8003  out[0x130] = in[0x54]
006c8009  call 0x529FD0(out+0x100, out+0x110, out)   ; MatrixForViewer -> viewMatrix at out[0x00]
006c800e  zNear = in[0x5C]
006c801c    if (zNear <= 0)  zNear = 4.0f      ; const 0xB4BD68 = 4.0f
006c8038    if (zNear <= 1)  zNear = 1.0f      ; const 0xB4623C = 1.0f
006c8050  out[0x138] = zNear
006c8065  call 0x589600(in[0x10], in[0x14], zNear, out+0x40)  ; projection -> out[0x40]
006c8076  out[0x134] = -0.1f                   ; const 0xA357F0
006c807e  eax = out ; call 0x6C11F0(1)         ; optional tiled-render projection fixup (§4.4)
006c8086  ret
```

Note it **does not** read or write `eyeOffset` / code constant 191. Confirmed: no reference to
displacement `0x19F0` occurs anywhere in `0x6C7F80..0x6C8090`.

---

## 2. The view structure

### 2.1 `refdef_t` — the input (base = `cg + 0x8C100`)

`cg = *(void**)0x2FF5354` (MEASURED — used as the base in `0x45E720`, `0x50B6F0`, `0x5ACA40`,
`0x6766E0`, `0x60BC30`, and `0x5C3420` loads it into `ebp`).

| Offset | Type | Meaning | Evidence |
|---|---|---|---|
| `+0x00` | int | viewport x | `0x793FB0`, `0x794500`; consumed as `[ebp]` in `0x6C8090` |
| `+0x04` | int | viewport y | `0x6C8355 mov edx,[ebp+4]` |
| `+0x08` | int | viewport width | `0x6C830F mov ecx,[ebp+8]` |
| `+0x0C` | int | viewport height | `0x6C8381 mov ecx,[ebp+0xC]` |
| **`+0x10`** | **float** | **tanHalfFovX** | passed as arg0 to `0x589600`; written at `cg+0x8C110` in `0x60BC30` as `aspect * tanHalfFovY` |
| **`+0x14`** | **float** | **tanHalfFovY** | arg1 to `0x589600`; written at `cg+0x8C114` as `tanf(fov*DEG2RAD*0.5f)*0.75f` |
| `+0x18` | float | fov (degrees, bookkeeping) | `cg+0x8C118`, written from the same local in `0x60BC30` |
| **`+0x20`** | **float[3]** | **vieworg** | `0x6C7F8E/FA/B5`; also `0x6C8C71`, `0x6C8D18`, `0x62D3DA` |
| **`+0x34`** | **float[3][3]** | **viewaxis** (forward, left, up) | `0x6C7FC4`…`0x6C8003` copies 9 consecutive floats `+0x34`…`+0x54` |
| **`+0x5C`** | **float** | **zNear** (0 ⇒ 4.0, clamped ≥ 1.0) | `0x6C800E` … `0x6C8050` |
| `+0x58` | int | scene time (ms) | `0x6C8C98` → `0x3AC3058` |
| `+0x64` | 0x94 bytes | copied verbatim to viewInfo+0x38 | `0x6C818D rep movsd (0x25 dwords)` |
| `+0xF8` | 0x1B4 bytes | copied verbatim to viewInfo+0xCC | `0x6C81B9 rep movsd (0x6D dwords)` |
| `+0x2E4`…`+0x384` | mixed | vision-set / filter parameters | `0x6C80F2`…`0x6C8242` |
| `+0x183A4` | int | scene index (selects DOF slot) | `0x6C8DA6 imul eax,0x34; add 0x396ECF8` |
| `+0x184D4` | float[3] | DOF world point | `0x6C891E` |

The struct is large (`0x6C84CC add ebp,0x1500` implies at least ~0x1500 of "header" plus the
`0x183xx` scene arrays). Only the fields above matter for the camera.

**Independent cross-check of the axis interpretation (MEASURED):** the verified viewmodel-placement
function `0x797BE0` walks the *client copy* of the same axis at `cg+0xA9D14` in exactly three rows
of three, multiplying each row by a separate `cg_gun_ofs_*` dvar and accumulating into an output
vec3 (`0x797C8E`…`0x797D5E`). That is `origin += axis[i] * ofs[i]`, i.e. a row-major 3×3 with
`axis[0]` = forward. The brief's viewmodel anchors and this refdef agree.

### 2.2 `GfxViewParms` — the output (0x140 bytes)

Size is **MEASURED**: `0x6C7F81 push 0x140` (the memset length) and the allocator stride
`0x6C8D80 lea edx,[eax+eax*4]; shl edx,6` = `n * 5 * 64` = `n * 0x140`.

| Offset | Size | Field | Evidence |
|---|---|---|---|
| `+0x000` | 0x40 | **viewMatrix** (row-major 4×4) | output of `0x529FD0`, called with `out` as 3rd arg |
| `+0x040` | 0x40 | **projectionMatrix** (row-major 4×4) | output of `0x589600`, called with `out+0x40` as 4th arg |
| `+0x080` | 0x40 | viewProjectionMatrix (**ASSUMED**) | slot exists (5×64 stride); not written by `0x6C7F80` |
| `+0x0C0` | 0x40 | inverseViewProjectionMatrix (**ASSUMED**) | as above |
| `+0x100` | 16 | **origin** as vec4, `w = 1.0` | `0x6C7F8E`…`0x6C7FB8` |
| `+0x110` | 12 | **axis[0]** forward | `0x6C7FC4`…`0x6C7FD0` |
| `+0x11C` | 12 | **axis[1]** left | `0x6C7FD6`…`0x6C7FE8` |
| `+0x128` | 12 | **axis[2]** up | `0x6C7FF1`…`0x6C8003` |
| `+0x134` | 4 | constant `-0.1f` (depth-hack near clip) | `0x6C8076` |
| `+0x138` | 4 | **zNear** (clamped) | `0x6C8050` |
| `+0x13C` | 4 | unused / zero from memset | — |

`+0x080` and `+0x0C0` being view-projection and its inverse is **ASSUMED** from the 5×64 stride and
from the engine's own code-constant name table, which defines `viewProjectionMatrix` (index 213)
and `inverseViewProjectionMatrix` (index 214) four and five slots after `viewMatrix` (index 201) —
matching a 4-matrix run at `0x00/0x40/0x80/0xC0` plus the origin/axis tail.
**Cheap test:** after the game is available again, break at `0x6C8086` (the `ret`) and dump the
0x140 bytes at EDI; check whether `[0x80..0xBF]` equals `viewMatrix × projectionMatrix`. If it is
all zero, nothing downstream reads it from here and it does not matter.

### 2.3 The view matrix — `MatrixForViewer` @ `0x529FD0`

**Signature (MEASURED — plain `__cdecl`, args at `[esp+4]`, `[esp+8]`, `[esp+0xC]`):**

```c
void __cdecl MatrixForViewer(const float origin[3], const float axis[3][3], float outView[16]);
```

Column construction (MEASURED at `0x529FD0`–`0x52A072`, with `xmm1 = -0.0f` from `0x9F0ED0` used as
the sign-flip mask):

```
outView[0][0] = -axis[1][0]      outView[0][1] = axis[2][0]      outView[0][2] = axis[0][0]
outView[1][0] = -axis[1][1]      outView[1][1] = axis[2][1]      outView[1][2] = axis[0][1]
outView[2][0] = -axis[1][2]      outView[2][1] = axis[2][2]      outView[2][2] = axis[0][2]
outView[3][0] = -dot(origin, -axis[1])
outView[3][1] = -dot(origin,  axis[2])
outView[3][2] = -dot(origin,  axis[0])
```

i.e. view-space **X = right = −axis[1]**, **Y = up = axis[2]**, **Z = forward = axis[0]**, row-major,
row-vector convention (`v_view = v_world * viewMatrix`), left-handed. This is the standard CoD
mapping and it means **you never need to touch the view matrix directly** — write `origin` and
`axis` into the refdef and the engine derives the matrix correctly.

### 2.4 The projection matrix — `R_SetupProjectionMatrix` @ `0x589600`

**Signature (MEASURED — plain `__cdecl`, 4 stack args):**

```c
void __cdecl R_SetupProjectionMatrix(float tanHalfFovX, float tanHalfFovY,
                                     float zNear, float outProj[16]);
```

Full body (it is only 0x5C bytes):

```
00589605  push 0x40 / push 0 / push esi / call 0x965480    ; memset(outProj, 0, 0x40)
0058960F  xmm0 = 0.999511719f                              ; K, const 0x9B8E38
0058961A  outProj[0x00] = K / tanHalfFovX                  ; [0][0]
00589627  xmm1          = K / tanHalfFovY
0058962D  outProj[0x28] = K                                ; [2][2]
0058963A  outProj[0x2C] = 1.0f                             ; [2][3]
00589650  outProj[0x14] = xmm1                             ; [1][1]
00589645  outProj[0x38] = zNear * -0.999511719f            ; [3][2]  (const 0x9DABD8)
```

So, in row-major `[r][c] @ 0x40 + r*16 + c*4` terms:

```
[0][0] = K/tanHalfFovX     [1][1] = K/tanHalfFovY
[2][2] = K                 [2][3] = 1
[3][2] = -K*zNear          everything else = 0
```

This is a left-handed, **infinite-far-plane**, **symmetric** perspective projection
(`w_clip = z_view`, depth → K as z → ∞; `K = 0.999511719 = 2047/2048`). There is no far plane and
no `r_znear`/`r_zfar` dvar involved — `r_zfar` (Dvar\* at `0x3B1FA4C`, accessor `0x6B6350`) is a
**fog** distance, its help string is `"Set to 0 to disable fog"` @ `0xB50BD0`. Do not confuse them.

**Crucially, `[2][0]` and `[2][1]` — the frustum-shear terms VR needs — are zero only because of
the `memset`.** The matrix layout has room for them and the engine already writes `[2][0]` itself
in one code path (§4.4). Nothing structurally prevents an asymmetric frustum.

---

## 3. How to write a per-eye camera

### 3.1 The stub (the hook is register-argument, so it must be naked)

```c
// Trampoline produced by MinHook for 0x6C7F80.
static void (*o_R_SetViewParms)(void);   // called only via the naked thunk below

// Our per-frame state, set by the OpenVR pose code before the frame is submitted.
typedef struct { float origin[3]; float axis[3][3]; float l,r,t,b; } vr_eye_t;
static vr_eye_t  g_eye[2];
static int       g_curEye = -1;          // -1 = not ours, leave alone
static int       g_lastEye = -1;

// C body. `out` and `in` are handed over explicitly by the naked thunk.
static void __cdecl hk_body(void *out, void *in)
{
    unsigned char *rd = (unsigned char *)in;
    float save_org[3], save_axis[9];

    if (g_curEye >= 0) {
        memcpy(save_org,  rd + 0x20, sizeof save_org);
        memcpy(save_axis, rd + 0x34, sizeof save_axis);
        memcpy(rd + 0x20, g_eye[g_curEye].origin, sizeof save_org);
        memcpy(rd + 0x34, g_eye[g_curEye].axis,   sizeof save_axis);
    }

    g_lastEye = g_curEye;
    call_original(out, in);              // naked thunk, restores EDI/ESI

    if (g_curEye >= 0) {
        memcpy(rd + 0x20, save_org,  sizeof save_org);   // MUST restore, see 3.4
        memcpy(rd + 0x34, save_axis, sizeof save_axis);
        vr_patch_projection((float *)((unsigned char *)out + 0x40), &g_eye[g_curEye]);
    }
}

__declspec(naked) static void hk_R_SetViewParms(void)
{
    __asm {
        push  esi
        push  edi          // args for hk_body(out=edi, in=esi) -> pushed right-to-left
        call  hk_body
        add   esp, 8
        ret
    }
}
```

`call_original` must restore EDI/ESI before jumping to the MinHook trampoline, because the original
reads them:

```c
__declspec(naked) static void call_original(void *out, void *in) {
    __asm {
        mov edi, [esp+4]
        mov esi, [esp+8]
        jmp o_R_SetViewParms      // MinHook trampoline for 0x6C7F80
    }
}
```

**Do not** let a C++ exception escape any of this — repo Decision 6 (`README.md`) applies: 32-bit
mingw uses DWARF-2 unwinding and cannot unwind through MSVC frames. Keep the detour pure C.

### 3.2 Order of operations, per eye

1. Before the frame, get the HMD pose and compute, for each eye, the world-space eye origin and a
   CoD-convention `axis[3][3]` (`axis[0]`=forward, `axis[1]`=**left**, `axis[2]`=up — note *left*,
   not right; the view matrix negates `axis[1]`). Apply the OpenVR eye-to-head transform in world
   units. **Black Ops units are inches** (**ASSUMED** — see §5.4).
2. Set `g_curEye = 0`, render the scene, present to the left eye texture.
3. Set `g_curEye = 1`, render the scene, present to the right eye texture.
4. Set `g_curEye = -1` for everything else (UI3D, extra-cams, reflection probes) so those views are
   untouched.

### 3.3 Rendering the scene twice is already supported by the allocator (MEASURED)

`R_RenderSceneInternal` does not use a fixed view-parms buffer; it bump-allocates one:

```
006c8d74  mov ecx,[0x3B3708C]                   ; frontEndData
006c8d7a  mov eax,[ecx+0x16CBE0]                ; viewParmsCount
006c8d80  lea edx,[eax+eax*4] ; shl edx,6       ; edx = count * 0x140
006c8d86  lea ebx,[edx+ecx+0x88000]             ; slot = frontEndData + 0x88000 + count*0x140
006c8d8d  inc eax
006c8d90  mov [ecx+0x16CBE0],eax                ; count++
```

Two calls per frame therefore get two distinct `GfxViewParms`. **How many slots exist before the
allocator overruns into whatever lives after `frontEndData+0x88000` is not known** — see §5.1.

The cleanest way to drive two views is to hook **`R_RenderScene` @ `0x6C8C40`** (`__cdecl`, one
argument, `refdef *`, single caller `0x5C3654`) and call the original twice:

```c
void __cdecl hk_R_RenderScene(void *refdef) {
    g_curEye = 0; o_R_RenderScene(refdef);
    g_curEye = 1; o_R_RenderScene(refdef);
    g_curEye = -1;
}
```

This is a normal cdecl hook and needs no asm. Combined with the `0x6C7F80` hook it gives one
refdef, two views, two cameras, two projections.

### 3.4 What fights back

* **The refdef is `cg->refdef`, not a copy.** `0x5C3654` passes `cg + 0x8C100` directly. If you
  write the eye pose into it and do not restore it, everything downstream in the same frame that
  reads `cg->refdef.vieworg` sees the eye position instead of the head position — and there are
  **122 measured references to `cg+0x8C120`** and 33 to `cg+0x8C134` across the client
  (sound listener, viewmodel placement, tracers, culling, `CG_...` HUD code). **Always restore
  before returning** (§3.1 does).
* **`R_RenderScene` (`0x6C8C40`) caches the origin into globals before the view is built.** It
  writes `refdef+0x20..0x28` into `0x3AC3060/64/68` at `0x6C8C71`, and `R_RenderSceneInternal`
  writes them again into `0x396A644/648/64C` and the forward axis into `0x396A650/654/658` at
  `0x6C8D18`/`0x6C8D42`. These feed lighting/vis lookups (`0x6C2170`, `0x6C1F60`). If you modify the
  refdef *inside* `0x6C7F80` (as §3.1 does) those globals keep the **head** pose, which is what you
  want for lighting/PVS coherence between eyes. If you instead modify the refdef *before*
  `R_RenderScene`, they will diverge per eye — measurably more work for PVS and a likely source of
  flicker. **Prefer the `0x6C7F80` hook for the pose.**
* **`0x6C11F0` runs after the projection is built** (`0x6C807E`) and *overwrites* `proj[0][0]`
  (`+0x40`), `proj[1][1]` (`+0x54`) and `proj[2][0]` (`+0x60`) when tiled rendering is enabled. It
  is gated on `[0x396EE10] != 0 && [0x396EE14] > 0`, both zero in normal play (**ASSUMED** — test
  in §5.3). Patch the projection **after** `0x6C7F80` returns, as §3.1 does, so you land after this.
* **Do not touch `eyeOffset` (code constant 191, `GfxCmdBufSourceState+0x19F0`).** Per the brief it
  is the camera-relative rebasing origin; `0x6C7F80` never writes it and neither should you.

---

## 4. FOV and projection

### 4.1 Where the FOV comes from (MEASURED)

`0x60BC30` computes, into the refdef:

```
t                  = tanf(fov_deg * 0.0174533f * 0.5f)      ; 0xA3AD40=DEG2RAD, 0x9B449C=0.5
cg->refdef+0x14    = t * 0.75f                              ; 0x9CF2F0 = 0.75 = 3/4  -> tanHalfFovY
cg->refdef+0x10    = aspect * (t * 0.75f)                   ; aspect from [0x2FF5324]+0x10 -> tanHalfFovX
cg->refdef+0x18    = fov_deg
```

`fov_deg` comes from the `cg_fov` dvar, **Dvar\* = `0x2FF6888`** (MEASURED: registered at
`0x4A3945` with the string `"cg_fov"` @ `0x9B1A48`; the result is stored at `0x4A3961`). The
current float value is at `Dvar+0x18` (MEASURED — every reader uses `[reg+0x18]`, e.g. `0x60BD23`,
`0x685465`, `0x797BFE`). Readers of `0x2FF6888`: `0x437EF0`, `0x4A3860`, `0x55CC90`, `0x60BC30`,
`0x685410`, `0x797BE0`.

The 0.75 factor means `cg_fov` is a **horizontal FOV defined at 4:3**, converted to vertical, then
re-widened by the true aspect ratio. Setting `cg_fov` alone can only ever produce a *symmetric*
frustum, so it is not sufficient for VR.

### 4.2 Can the projection be set per eye? Yes.

Three independent facts make the asymmetric per-eye projection straightforward:

1. `R_SetupProjectionMatrix` (`0x589600`) is a **plain `__cdecl` function with the output pointer as
   its 4th argument** — it can be hooked directly with a normal C signature, no asm.
2. It `memset`s the whole 0x40 bytes first, so `[2][0]` and `[2][1]` are *deliberately* zero, not
   structurally absent.
3. The engine itself already writes `[2][0]` (`viewParms+0x60`) in `0x6C11F0` for tiled rendering
   (`0x6C126F movss [esi+0x60],xmm2`), proving the rest of the pipeline tolerates a sheared
   projection.

**Recommended: patch after the fact rather than replacing `0x589600`.** In `hk_body` (§3.1), after
the original returns, rewrite `viewParms+0x40` from the OpenVR raw projection
(`IVRSystem::GetProjectionRaw` gives `l, r, t, b` as tangents at the near plane):

```c
static void vr_patch_projection(float *P /* = viewParms+0x40 */, const vr_eye_t *e)
{
    const float K = 0.999511719f;              // 0x9B8E38, MEASURED
    float zNear   = *(float *)((char *)P - 0x40 + 0x138);   // viewParms+0x138
    float w = e->r - e->l, h = e->t - e->b;

    memset(P, 0, 0x40);
    P[0]  = K * 2.0f / w;                      // [0][0]
    P[5]  = K * 2.0f / h;                      // [1][1]
    P[8]  = -K * (e->r + e->l) / w;            // [2][0]  <-- horizontal shear (per-eye)
    P[9]  = -K * (e->t + e->b) / h;            // [2][1]  <-- vertical shear
    P[10] = K;                                 // [2][2]
    P[11] = 1.0f;                              // [2][3]
    P[14] = -K * zNear;                        // [3][2]
}
```

Sanity check: with a symmetric frustum (`r = -l = tanHalfFovX`, `t = -b = tanHalfFovY`) this
reduces exactly to the engine's own output — `P[0] = K/tanHalfFovX`, `P[5] = K/tanHalfFovY`,
`P[8] = P[9] = 0`. That equivalence is the first thing to verify.

**The sign of `[2][0]`/`[2][1]` is ASSUMED.** It depends on whether view-space X points the same way
as OpenVR's. The engine's view matrix puts X = −axis[1] = right and Z = forward with `w_clip = +z`,
which matches OpenVR's right-handed-eye-space-with-negated-Z only after a handedness flip.
**Cheap test:** render one eye with a deliberately large horizontal shear and confirm the image
shifts toward the expected side; flip the sign if not. This is a two-minute test once the game can
be launched.

### 4.3 Viewport (needed for per-eye render targets)

`0x6C8090(refdef, viewInfo)` computes the pixel viewport from the refdef's `x/y/width/height` and
two global render-resolution scalars **`0x3966148` (X)** and **`0x396614C` (Y)**, writing:

* `viewInfo+0x14A0` = x, `+0x14A4` = y, `+0x14A8` = width, `+0x14AC` = height
* `viewInfo+0x14C0..0x14CC` = the scissor rect (copied from the viewport at `0x6C84E4` when
  `refdef+0x183A0 == 0`)

If per-eye targets differ from the back buffer (they should — the back buffer is 2560×1440 with
**4× MSAA**, per `experiments/10_d3d9hook/RESULTS.md`), this is where to intervene. Note that
experiment's finding: a multisampled surface cannot be handed to the compositor and needs a
`StretchRect` resolve first.

### 4.4 `0x6C11F0` — the engine's own asymmetric-projection precedent

```
006c11f0  cmp [0x396EE10],0        ; tiling enabled?
006c1208  cmp [0x396EE14],0 ; jle  ; tile count > 0?
006c1225  [esi+0x40] *= tileCount  ; proj[0][0]
006c123c  [esi+0x54] *= tileCount  ; proj[1][1]
006c126f  [esi+0x60] = …           ; proj[2][0]   <-- shear
006c128f  …                        ; proj[2][1]
```

This is almost certainly the high-resolution-screenshot / reflection-probe tile renderer. It is
**the proof that the shader pipeline consumes `proj[2][0]`/`[2][1]` correctly**, and it is also the
one thing that will silently undo your projection if it ever fires (§3.4).

---

## 5. Risks and unknowns

Each with the cheapest test that settles it. **None of these were tested — the game was not
launched, per the brief.**

### 5.1 How many `GfxViewParms` slots exist? (highest risk)
The allocator at `0x6C8D74` bumps `[frontEndData+0x16CBE0]` with no visible bound check in
`0x6C8CD0`. Rendering the scene twice doubles the consumption, and UI3D/extra-cam views
(`r_ui3d_*`, `r_extracam_*`, `0x6CEE30`) also allocate.
**Test:** hook `0x6C8CD0` entry, log `[[0x3B3708C]+0x16CBE0]` each call for a normal frame; then
log the same with the double-render active. If the count in a normal frame is already ≥ 4, the pool
is likely ≥ 8 and doubling is safe. Also breakpoint-watch `frontEndData+0x88000 + n*0x140` for the
largest observed `n` to find the pool's end.

### 5.2 Is the frame loop willing to render the scene twice at all?
`CG_DrawActiveFrame` calls `R_RenderScene` once, then draws the 2D HUD. Calling it twice may
double-submit scene entities, or the second call may be cheap and correct.
**Test:** hook `0x6C8C40` and call the original twice with an *unmodified* refdef. If the frame
renders identically at ~half the framerate, the path is re-entrant. If it corrupts or crashes, the
per-eye split must instead happen further down (two `R_RenderView` calls from one scene setup) or
via alternate-frame rendering as a fallback.

### 5.3 Is `0x6C11F0`'s tiling path inert in normal play?
**Test:** read `*(int*)0x396EE10` and `*(int*)0x396EE14` once in-game. If both are 0, the fixup
never runs and §3.1's post-hoc projection patch is safe unconditionally. If not, patch the
projection *inside* a hook on `0x589600` instead and let `0x6C11F0` run on top.

### 5.4 World units
The whole plan assumes Black Ops uses **inches** (CoD/Quake lineage), so an OpenVR metre-space eye
offset must be scaled by ~39.37 before being added to the view origin.
**Test:** log `cg->refdef.vieworg` (`cg+0x8C120`) while walking a known distance, or compare the
player's eye height above the floor to ~64 units (standing) / ~40 (crouched), the CoD norm.

### 5.5 `viewParms+0x80` / `+0xC0`
**ASSUMED** to be viewProjection and its inverse; `0x6C7F80` leaves them zeroed. If something
downstream recomputes them from `viewMatrix × projectionMatrix` *after* our patch, we are fine; if
something cached them *before*, our projection change will be partly ignored (typically visible as
correct geometry but wrong post-process/SSAO/fog depth reconstruction).
**Test:** dump `viewParms[0x80..0xBF]` at `0x6C8086` and again just before `0x6C6450` at
`0x6C8E2E`. If they are non-zero at the second point, find the writer and hook it too.

### 5.6 The 4× MSAA back buffer
From `experiments/10_d3d9hook/RESULTS.md` (already measured by the other workstream): the swap
chain's back buffer is 2560×1440 `A8R8G8B8` with `MultiSampleType = 4`, and the game presents via
**`IDirect3DSwapChain9::Present` (vtable slot 3)**, not `IDirect3DDevice9::Present`. Per-eye targets
must be separate non-MSAA render targets, or resolved with `StretchRect` before submission. This is
orthogonal to the camera hook but gates the end-to-end result.

### 5.7 Multithreaded renderer
`r_smp_backend`, `r_smp_worker`, `r_multithreaded_device` dvars exist. `frontEndData`
(`*(void**)0x3B3708C`) is a per-frame front-end buffer consumed by a back end, which implies the
front/back ends may run on different threads.
**Test:** log `GetCurrentThreadId()` in the `0x6C7F80` hook. If it is not the main thread, all
per-eye state (`g_curEye`) must be thread-local or passed through the view parms rather than a
global.

### 5.8 The `0x6C7F80` prologue is hookable
`0x6C7F80` starts `push ecx; push 0x140; push 0; push edi; call 0x965480` — 1 + 2 + 2 + 1 + 5 = 11
bytes before the first relative call, so a 5-byte `E9` detour needs to relocate `push ecx; push
0x140; push 0; push edi` (6 bytes, all position-independent). MinHook's HDE32 handles this
trivially. **ASSUMED** no other code jumps into `0x6C7F80+1..+5`; the only inbound control flow
found is the five `call` sites in §1.1.
**Test:** `MH_CreateHook` returning `MH_OK` and the trampoline disassembling cleanly is sufficient.

---

## 6. Summary of addresses

| Symbol (our naming) | Address | Convention | Notes |
|---|---|---|---|
| `CG_DrawActiveFrame` | `0x5C3420` | cdecl | `ebp = cg`; builds `cg->refdef` |
| `R_RenderScene` | `0x6C8C40` | `__cdecl(refdef*)` | single caller `0x5C3654`; **hook here to render twice** |
| `R_RenderSceneInternal` | `0x6C8CD0` | `__cdecl(refdef*, …)` | allocates the view-parms slot |
| **`R_SetViewParms`** | **`0x6C7F80`** | **EDI=out, ESI=in, no stack args** | **primary camera hook** |
| `MatrixForViewer` | `0x529FD0` | `__cdecl(org, axis, out)` | builds the 4×4 view matrix |
| `R_SetupProjectionMatrix` | `0x589600` | `__cdecl(tanX, tanY, zNear, out)` | builds the 4×4 projection |
| `R_SetupViewportAndScissor` | `0x6C8090` | `__cdecl(refdef, viewInfo)` | viewport at `viewInfo+0x14A0` |
| `R_RenderView` | `0x6C6450` | `__cdecl(viewInfo, …, viewParms, …, refdef, bool)` | the actual scene draw |
| `R_TiledProjectionFixup` | `0x6C11F0` | EAX=viewParms, `__cdecl(bool)` | overwrites `[0][0]`,`[1][1]`,`[2][0]` |
| `R_GetZFar` (fog!) | `0x6B6350` | cdecl, returns st0 | reads `r_zfar` Dvar\* `0x3B1FA4C` — **not** a far plane |
| `R_IsStereoActive` | `0x6B8B20` | cdecl | 3D Vision only — ignore |
| `Dvar_FindVar` | `0x5AE810` | `__cdecl(const char*)` | Dvar float value at `Dvar+0x18` |

| Global | Address | Meaning |
|---|---|---|
| `cg` | `*(void**)0x2FF5354` | client game state; `refdef` at `cg+0x8C100` |
| `frontEndData` | `*(char**)0x3B3708C` | per-frame front-end buffer |
| viewParms pool | `frontEndData + 0x88000`, stride `0x140` | count at `frontEndData + 0x16CBE0` |
| view origin (renderer) | `0x3AC3060`, `0x396A644` | cached copies of `refdef->vieworg` |
| view forward (renderer) | `0x396A650` | cached copy of `refdef->viewaxis[0]` |
| `cg_fov` Dvar\* | `0x2FF6888` | value at `+0x18` |
| `g_stereoActive` | `0x396346B` | NVIDIA 3D Vision flag — ignore |
| tiling gate | `0x396EE10`, `0x396EE14` | must be 0 for §3.1 to be safe |

---

## 7. Reproducing this analysis

```sh
cd /home/dlynch/dev/bo1-vr/research/engine
python3 d.py  6c7f80 60          # disassemble
python3 fs.py 6c7f80             # function summary with string annotations
python3 cx.py 6c7f80             # call xrefs
```

The wider displacement/absolute-memory index used for the struct mapping is not checked in; it is
regenerated in ~13 s by a resyncing linear sweep of `.text` that restarts capstone on each decode
failure (the naive single `md.disasm()` in `d.py` desyncs at ~`0x4A2600` and silently stops — that
is why `dvars.txt` only holds 53 of the several thousand dvars). If this needs to be permanent,
fold the resync loop into `research/engine/` as its own script.
