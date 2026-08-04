# What `xoxor4d/t5-rtx` gives us

`https://github.com/xoxor4d/t5-rtx` is an RTX Remix compatibility mod for the
Black Ops **single-player** client. It reimplements the game's rendering through
the D3D9 fixed-function pipeline so Remix can see the geometry. That goal has
nothing to do with ours — but getting there required mapping BO1's render
pipeline, and that map is the thing we have been building one hand-disassembled
function at a time.

Clone kept at `/home/dlynch/dev/reference/t5-rtx` (`1c7a49e`), deliberately
**outside this repository** so it can never be committed by accident.

## Provenance and the rule

t5-rtx carries **no licence file** — all rights reserved. Its credits are
Nukem9's LinkerMod and RektInator, i.e. ordinary reverse engineering, not leaked
source or a decompilation dump. So unlike KisakBlack there is no NDA or DMCA
taint on the *knowledge*; there is ordinary copyright on the *expression*.

**The rule, same as KisakBlack: facts only, never code.** Addresses and struct
offsets are facts about `BlackOps.exe` and are not copyrightable. Every address
recorded below was re-verified against our own binary with `objdump` before
being written down — which is both the legally clean path and simply good
practice, since a mismatched build would poison everything downstream.

## Their build is our build

Our `BlackOps.exe` is md5 `2b179a57416680b60462c5af05552ea2`. Spot-checked
against a disassembly of that exact file:

| Their symbol | Address | What our binary has there |
|---|---|---|
| `R_SetRenderTarget(source, state, rt_index)` | `0x726650` | reads `[esp+0x10]` as a **BYTE** after one `push` — the third arg, `uint8_t`, exactly as declared |
| `R_SetRenderTargetSize(source /*ecx*/, ...)` | `0x7265E0` | `push esi; mov esi,ecx` — takes `ecx`, as declared |
| `Dvar_FindVar(const char* name)` | `0x5AE810` | `mov eax,[esp+4]; test eax,eax` — cdecl, null-checks the name |
| `Material_RegisterHandle(const char* name)` | `0x6D4080` | `mov ecx,[esp+4]; cmp BYTE PTR [ecx],0` — empty-string check |
| `Sys_GetValue(int)` | `0x67D4F0` | reads a global, then `fs:0x2c` (TLS) |
| `R_Set3D` | `0x7244C0` | a 10-byte `call`/`jmp` thunk — the LTCG shape, and commented out in their own header |

Six for six. Their `DxGlobals* dx = 0x3963440` also sits in the same large BSS
region as the fence addresses we found independently (`.data` ends at
`0xBA6400`, so both are well past it). The map transfers.

## The four things worth acting on

### 1. `r_smp_backend` — a possible real fix for trigger 1

t5-rtx forces these off every frame: `r_smp_backend`, `r_skinCache`,
`r_fastSkin`, `r_smc_enable`, `r_depthPrepass`, `r_dof_enable`, `r_distortion`.
**`r_smp_backend` is present in our binary** (checked by string match; so are
`r_skinCache`, `r_fastSkin`, `r_multiGpu`).

This matters because of what §10–§12 established: the freeze is the game's own
render thread spinning forever on a D3D9 EVENT query that never retires. That
spin loop *is* the SMP backend's frame fence. If `r_smp_backend 0` makes the
game render on the main thread, the fence ring may not be driven at all — which
would remove trigger 1 at its root instead of routing around it with `nocap.on`.

That is the difference between the current workaround and a fix, and `nocap.on`
is exactly what forces our capture to Present, which is what blocks proper
per-eye capture (#30). So this one lever plausibly unblocks both.

**TESTED AND REFUTED — see `experiments/11_gameframe/RESULTS.md` §15.** It does
the opposite: `nocap.on` + `smpoff.on` froze 2/2 (at 49 s and 41 s) where the
control froze 0/3. Both override routes demonstrably worked — registration-time
interception of `Dvar_RegisterBool`, and a per-frame force — so the game really
did run with SMP disabled, and froze sooner. Kept in the tree behind `smpoff.on`,
off by default.

The detour was still worth it: it confirmed `dvar_s.current` at `+0x18` from the
game's own `cmp BYTE PTR [eax+0x18], 0` at `0x6D5815`, `Dvar_RegisterBool` at
`0x45BB20`, `Dvar_FindVar` at `0x5AE810`, and that `r_smp_backend` is read in
exactly one place (`0x6D5810`, branching to `Sys_IsMainThread` when clear).

That leaves **`R_RENDERTARGET_RESOLVED_SCENE` (below) as the live lead for #32.**

### 2. The engine resolves the scene itself

Their `R_RENDERTARGET_*` enum includes:

```
R_RENDERTARGET_SCENE                = 0x3
R_RENDERTARGET_RESOLVED_POST_SUN    = 0x5
R_RENDERTARGET_RESOLVED_SCENE       = 0x6
R_RENDERTARGET_8BIT_SWAPCHAIN_BACKBUFFER = 0x12
```

There is already a **resolved** (non-multisampled) scene target. Trigger 1 is
our own mid-scene MSAA `StretchRect` tearing down the live render pass; if we
can read `RESOLVED_SCENE` instead, we never issue that resolve. `R_SetRenderTarget`
(`0x726650`) and `R_SetRenderTargetSize` (`0x7265E0`) are the accessors, and
`gfxCmdBufSourceState` / `gfxCmdBufState` are the globals they take.

### 3. A second hook site where `viewParms` is live — for #30

Their scene hook is at **`0x6C8DF1`**, with the original instruction
`mov ecx,[esi+0x183A8]`, and at that point **`ebx` holds `GfxViewParms*`**.

`0x6C8DF1` is inside `R_RenderSceneInternal` (`0x6C8CD0`), only `0x5B` past the
`R_SetViewParms` call site our own plan already documents at `0x6C8D96` — where
we independently found `edi = ebx = viewParms`. Two derivations, same register.

This is useful precisely because it is *after* `R_SetViewParms` has run, so the
struct is fully populated — including `viewProjectionMatrix` at `+0x80`, which
`R_SetViewParms` never writes.

What they do there is instructive even though we want the opposite: they push
the game's matrices straight onto the device with
`SetTransform(D3DTS_VIEW/D3DTS_PROJECTION, ...)`. We do not want that (we want
to *modify* the view per eye, not mirror it), but it confirms the matrices at
`+0x00` and `+0x40` are D3D-ready row-major and need no conversion.

### 4. `GfxViewParms` — our ASSUMED slots corroborated

Their layout matches our `docs/camera-hook-plan.md` §2.2 field for field, and
resolves the two entries we had marked ASSUMED (`viewProjectionMatrix` at
`+0x80`, `inverseViewProjectionMatrix` at `+0xC0`) plus one we had recorded as
unused (`+0x13C` is `zFar`). Verified against our binary: `sizeof` is exactly
`0x140` (the memset length at `0x6C7F81`), and every store into `[edi+...]`
across `0x6C7F80..0x6C8090` lands inside a declared field. Written up in
`camera-hook-plan.md` §2.2.1.

## The fence machinery, and an honest gap

`DxGlobals` (their `0x3963440`) declares three separate D3D9 query sets:

```
IDirect3DQuery9* fencePool[8];  unsigned nextFence;  int gpuSync;  int gpuCount;
IDirect3DQuery9* flushGpuQuery;
IDirect3DQuery9* swapFence[4];
```

So the engine has named machinery for exactly the thing §10 found empirically.
**But I have not established which set our spin loop reads.** Our loop at
`0x6EBB40` uses an array at `0x3966134`, a count at `0x39660B4` and an index at
`0x396A4CC`; the declared field order puts the count *after* the array, not
`0x80` bytes before it, so either our loop reads a different set or their layout
is approximate here — their header does carry explicit `char pad[592]` filler
elsewhere, so parts of it are admittedly reconstructed. Resolving this needs the
initialisation site (whatever calls `CreateQuery` in a loop and stores to
`0x3966134`), which I have not chased. Recorded so nobody assumes it is settled.

## What is not useful

- `fixed_function.cpp` (63 KB) — replacing shaders with fixed-function for
  Remix. We want the game's own shading, not a reimplementation of it.
- Their `ceg.cpp` handles the Steam DRM, but for launching outside Steam. We
  already launch through the real Steam client, which is the supported path
  (Exp. 8 §10) and the one that keeps CEG happy.
- The map-settings / fog / sun / sky work is Remix-specific art direction.
