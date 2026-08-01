# BAC-277 — Does `BlackOps.exe` contain a live in-engine two-view render path?

**Verdict: NO. PC stereo in Black Ops (2010) is NVIDIA 3D Vision *Automatic* — driver-side
only. The game does not render two views. Its entire contribution to stereo is (a) creating
an NVAPI stereo handle, (b) asking the driver each frame whether stereo is on, (c) pushing a
single convergence scalar to the driver, and (d) drawing a handful of HUD/crosshair elements
in world space instead of screen space so the driver separates them correctly.**

Consequence for BAC-274: **alternate-eye rendering from outside the engine is confirmed as the
right v1 strategy.** There is no engine stereo to drive. Nothing here can be repurposed into a
two-view path — there is no second view, no eye index, and no per-eye projection anywhere in
the binary.

---

## Binary and method

| | |
|---|---|
| File | `/mnt/games/steam/steamapps/common/Call of Duty Black Ops/BlackOps.exe` |
| Size | 8,101,944 bytes |
| Format | PE32 x86, `ImageBase` **0x400000**, `DllCharacteristics` 0x8000 (no `DYNAMIC_BASE`, so all VAs below are absolute and stable) |
| Sections | `.text` 0x401000 (v 0x5a1a10), `.rdata` 0x9a3000, `.data` 0xb6c000, `.tls`, `.rodata`, `.version`, `.rsrc` |
| Imports | `d3d9.dll`, `d3dx9_43.dll`, `binkw32.dll`, `steam_api.dll`, `XINPUT1_3.dll`, `DSOUND.dll`, … — **no `dxgi`, no `d3d10/11`** |
| PDB path string | `C:\projects_pc\cod\codsrc\src\obj\t5\CoDSteam_CEG_bin\BlackOps.pdb` (0xb61624) |

Tools: `pefile` + `capstone` (x86-32) via a throwaway venv. The binary was **not executed**.

**Packing / CEG check.** The PDB path says `CoDSteam_CEG_bin`, i.e. a Steam CEG build. No
64 KiB block anywhere in `.text` has Shannon entropy above 7.0, and every address examined
below disassembles into coherent, self-consistent x86. Nothing in the stereo code path is
encrypted or obfuscated.

**Correction to the ticket's premise.** The binary does *not* retain thousands of
`C:\projects_pc\cod\codsrc\...\*.cpp` source paths. There are **145**, and they are almost
entirely DemonWare (`bdNet`, `bdLobby`, `bdSocket`) and libtomcrypt — third-party libraries
that kept their asserts. Treyarch's own engine asserts were compiled out of the release build.
So function → source-file attribution via assert strings is **not available** for any renderer
code, and none of the naming below rests on it. Everything is named from behaviour
(NVAPI interface IDs, D3D9 vtable offsets, argument counts) and is flagged where uncertain.

---

## 1. The dvars: registration sites

Two dvar registration helpers are involved. Both tail into a common core at **0x862d70**
whose last stack argument is a type tag (`0` = bool, `1` = float):

* **0x45bb20** — `Dvar_RegisterBool(const char *name, bool value, int flags, const char *desc)`
* **0x679020** — `Dvar_RegisterFloat(const char *name, float value, float min, float max, int flags, const char *desc)`

Each returns `dvar_t *` in `EAX`, which the caller stores to a file-static global. **The
current value lives at offset `+0x18` in `dvar_t`** — confirmed by every reader below, and
cross-checked by type: bool dvars are read with `cmp byte ptr [reg+0x18], 0`, float dvars with
`fld dword ptr [reg+0x18]` / `movss`.

| dvar | registered at | call | default | min / max | flags | description string | `dvar_t *` stored to |
|---|---|---|---|---|---|---|---|
| `r_use_driver_convergence` | 0x6ce281 | 0x45bb20 | `false` | — | 1 | "Use the driver convergence values instead of the game defined values." (0xb554e8) | **0x3b1fca8** |
| `r_convergence` | 0x6ce2ba | 0x679020 | **6.06253** | -FLT_MAX / +FLT_MAX | 0 | "Stereo convergence." (0xb5554c) | **0x3b1fc54** |
| `r_stereoTurretShift` | 0x6cbce8 | 0x679020 | 0.0 | 0.0 / 10000.0 | 0x1000 | "3D turret shift" (0xb5359c) | **nothing — return value discarded** |
| `cg_drawCrosshair3D` | 0x4a3ed3 | 0x45bb20 | `true` | — | 0x800 | (empty, 0x9dd354) | **0x2f67ba8** |
| `cg_hudLegacyStereo3DScale` | — | — | — | — | — | — | **string absent from the image entirely** |

Attribution of `EAX` → global uses the rule "the `mov [glob], eax` between a registration call
and the next one belongs to that call". The rule is validated independently on this very block:
it assigns `r_allow_intz` → 0x3b1fa10, and 0x3b1fa10 is exactly the gate at the top of the
INTZ-depth-format detection routine at 0x71f640 (which goes on to `CreateTexture` with FourCC
`'INTZ'` = 0x5a544e49). It also predicts that `r_use_driver_convergence` is read as a *byte*
and `r_convergence` as a *float*, which is what the readers do.

### Verified dead: `r_stereoTurretShift` and `cg_drawCrosshair3D`

* `r_stereoTurretShift` — the name string at **0xb535ac** has **exactly one** reference in the
  whole image: the `push` at 0x6cbce3 that feeds its own registration. Between the registration
  call at 0x6cbce8 and the next registration call at 0x6cbcfb there are only argument pushes,
  so the returned `dvar_t *` is never stored. No global holds it, and no code can find it by
  name. **Registered, never read.** Vestigial.
* `cg_drawCrosshair3D` — registered, and its `dvar_t *` *is* stored to 0x2f67ba8, but 0x2f67ba8
  has **exactly one** reference image-wide: that store at 0x4a3ed9. **Registered, never read.**
  Vestigial. (The real crosshair-in-3D branch exists — see §4 — but it is gated on
  live stereo state, not on this dvar.)
* `cg_hudLegacyStereo3DScale` — a byte-level search for `hudLegacy` (case-insensitive) over
  every section returns nothing. The dvar does not exist in the PC build. It is a
  console-build leftover in the dvar lists you were working from.

---

## 2. The dvars: readers, and where they lead

Reference counts are from a full-image scan for the 4-byte little-endian value of each address.

`r_convergence` (**0x3b1fc54**) — 3 references: the registration store, plus

* **0x6eb395**, inside the Present/end-frame function at 0x6eb1f0 — *reads* it and hands it to
  the driver;
* **0x775184**, inside 0x7750b0 — *writes* it (via `Dvar_SetFloat`).

`r_use_driver_convergence` (**0x3b1fca8**) — 2 references: the registration store, plus
**0x6eb382**, in the same Present function.

### The one place the value goes (0x6eb1f0, the Present function)

`0x6eb1f0` is the frame-present routine: it calls `IDirect3DSwapChain9::Present` (vtable +0xC
on the swap-chain object fetched from `0x39660dc`), handles `D3DERR_DEVICELOST` (`0x88760868`)
and device reset. Immediately after presenting:

```
006eb349  mov    eax, [0x396346c]        ; g_stereoHandle
006eb34e  test   eax, eax
006eb350  je     0x6eb404                ; no handle -> g_stereoActive = 0
006eb356  lea    edx, [esp + 0x13]
006eb35a  push   edx
006eb35b  push   eax
006eb361  call   0x98efde                ; NvAPI_Stereo_IsActivated(handle, &isOn)
006eb369  test   eax, eax
006eb36b  jne    0x6eb404
006eb371  cmp    byte ptr [esp + 0x13], al
006eb375  setne  al
006eb378  mov    [0x396346b], al         ; g_stereoActive = isOn
006eb37d  test   al, al
006eb37f  je     0x6eb3a8
006eb381  mov    eax, [0x3b1fca8]        ; r_use_driver_convergence
006eb386  cmp    byte ptr [eax + 0x18], 0
006eb38a  jne    0x6eb3a8                ; driver convergence -> do nothing
006eb38c  mov    edx, [0x396346c]        ; g_stereoHandle
006eb393  mov    ecx, [0x3b1fc54]        ; r_convergence
006eb399  fld    dword ptr [ecx + 0x18]  ; its current float value
006eb39c  fstp   dword ptr [esp]
006eb39f  push   edx
006eb3a0  call   0x98f002                ; NvAPI_Stereo_SetConvergence(handle, value)
```

That is the whole of it. One scalar, once per frame, into the driver. `r_use_driver_convergence`
does exactly what its description says: it suppresses the game's `SetConvergence` call and
leaves the driver's own convergence in place.

### The one place the value comes from (0x7750b0)

`0x7750b0` traces/derives a distance (it calls a trace helper at 0x774b60, clamps against a
per-weapon float at `[eax+0x78]`) and then:

```
0077516b  call   0x6b8b20                ; R_IsStereoActive()
00775172  je     0x7751b0                ; not stereo -> skip entirely
00775174  movss  xmm0, [esi + 0x8a4cc]   ; a 0..1 blend factor in client state
0077517c  movss  xmm1, [0xa4e394]        ; 1.0
00775184  mov    eax, [0x3b1fc54]        ; r_convergence dvar_t*
00775189  subss  xmm1, xmm0              ; 1 - t
0077518d  mulss  xmm1, [0xa14ac0]        ; * 6.06253   (== r_convergence's own default)
00775195  mulss  xmm0, [0xa40540]        ; * 1.9224
0077519e  addss  xmm1, xmm0
007751a7  push   eax
007751a8  call   0x4fbd90                ; Dvar_SetFloat(dvar, value)
```

So the game lerps convergence between **6.06253** (hip) and **1.9224** (presumably fully
aimed-down-sights) as a function of a client-state blend factor, and only bothers when stereo
is actually active. This is a convergence *hint generator*. It sets a dvar; §2 then hands that
dvar to NVAPI. Nothing else consumes it.

---

## 3. The NVAPI surface — the decisive evidence

The binary contains the standard statically-linked NVAPI stub: `LoadLibrary("nvapi.dll")` at
0xb302b0, `GetProcAddress("nvapi_QueryInterface")` at 0xb302ba, `NvAPI_Initialize`
(ID `0x0150E828`, pushed literally at 0x98ea74), then a resolve loop over a **230-entry table
of `{void *fnptr; unsigned id;}` pairs at 0xba4734**, with a matching array of 6-byte
`jmp dword ptr [slot]` thunks starting at **0x98ead4**.

The table is the compile-time list of every NVAPI entry point this build *could* call. The
stereo block sits at indices 204–229. Reading it out:

| idx | thunk | interface ID | function |
|---|---|---|---|
| 204 | 0x98ef9c | 0xBE7692EC | `Stereo_CreateConfigurationProfileRegistryKey` |
| 205 | 0x98efa2 | 0xF117B834 | `Stereo_DeleteConfigurationProfileRegistryKey` |
| 206 | 0x98efa8 | 0x24409F48 | `Stereo_SetConfigurationProfileValue` |
| 207 | 0x98efae | 0x49BCEECF | `Stereo_DeleteConfigurationProfileValue` |
| 208 | 0x98efb4 | 0x239C4545 | `Stereo_Enable` |
| 209 | 0x98efba | 0x2EC50C2B | `Stereo_Disable` |
| **210** | **0x98efc0** | **0x348FF8E1** | **`Stereo_IsEnabled`** — called |
| **211** | **0x98efc6** | **0xAC7E37F4** | **`Stereo_CreateHandleFromIUnknown`** — called |
| **212** | **0x98efcc** | **0x3A153134** | **`Stereo_DestroyHandle`** — called |
| 213 | 0x98efd2 | 0xF6A1AD68 | `Stereo_Activate` |
| 214 | 0x98efd8 | 0x2D68DE96 | `Stereo_Deactivate` |
| **215** | **0x98efde** | **0x1FB0BC30** | **`Stereo_IsActivated`** — called |
| 216 | 0x98efe4 | 0x451F2134 | `Stereo_GetSeparation` |
| 217 | 0x98efea | 0x5C069FA3 | `Stereo_SetSeparation` |
| 218 | 0x98eff0 | 0xDA044458 | `Stereo_DecreaseSeparation` |
| 219 | 0x98eff6 | 0xC9A8ECEC | `Stereo_IncreaseSeparation` |
| 220 | 0x98effc | 0x4AB00934 | `Stereo_GetConvergence` |
| **221** | **0x98f002** | **0x3DD6B54B** | **`Stereo_SetConvergence`** — called |
| 222 | 0x98f008 | 0x4C87E317 | `Stereo_DecreaseConvergence` |
| 223 | 0x98f00e | 0xA17DAABE | `Stereo_IncreaseConvergence` |
| 224–229 | … | 0xE6839B43, 0x7BE27FA2, 0x932CB140, 0x8B7E99B5, 0x3CD58F89, 0x6E1042B0 | notification message / reverse-blit control / surface-creation-mode / debug — none called |

A full `E8`/`E9` rel32 scan of `.text` for calls landing on any of the 230 thunks finds
**exactly ten call sites, hitting nine distinct thunks**:

| thunk | ID | probable name | call sites |
|---|---|---|---|
| 0x98eae0 | 0xF951A4D1 | `SYS_GetDriverAndBranchVersion` | 0x71f6ec |
| 0x98eb9a | 0x4B708B54 | `D3D_GetCurrentSLIState` | 0x6b78ff |
| 0x98ebdc | 0xC7985ED5 | `D3D9_GetSurfaceHandle` | 0x6e4d3e, 0x71f73b |
| 0x98ec24 | 0xB380F218 | `D3D9_Get…RenderTargetHandle` (probable) | 0x6e4d1c |
| 0x98ecb4 | 0xAEAECD41 | `D3D9_StretchRectEx` | 0x6e4d9d, 0x6e4dcc, 0x71f76d |
| 0x98efc0 | 0x348FF8E1 | `Stereo_IsEnabled` | 0x71f6cc |
| 0x98efc6 | 0xAC7E37F4 | `Stereo_CreateHandleFromIUnknown` | 0x6b75f6 |
| 0x98efcc | 0x3A153134 | `Stereo_DestroyHandle` | 0x6b8161 |
| 0x98efde | 0x1FB0BC30 | `Stereo_IsActivated` | 0x6b760d, 0x6eb361 |
| 0x98f002 | 0x3DD6B54B | `Stereo_SetConvergence` | 0x6eb3a0 |

The interface-ID naming is corroborated independently by argument counts at each call site
(the caller-cleanup `add esp, N` after each `__cdecl` call): `IsEnabled` 1 arg,
`CreateHandleFromIUnknown` 2, `DestroyHandle` 1, `IsActivated` 2, `SetConvergence` 2 — all
matching the published NVAPI signatures.

**`NvAPI_Stereo_SetActiveEye` (ID `0x96EEA9F8`) does not appear anywhere in the image.** Nor
does `NvAPI_Stereo_SetSurfaceCreationMode` (`0xF5DCFCBA`), nor `NvAPI_Stereo_SetDriverMode`
(`0x36F1C413`). A byte-level search of every section for those DWORDs returns zero hits.

`SetActiveEye` is *the* API a game uses when it wants to drive 3D Vision "Direct" mode and
render each eye itself. Its total absence, combined with the presence of nothing but
`IsActivated` + `SetConvergence`, is the single strongest piece of evidence here: this build
was written against **3D Vision Automatic**, where the driver duplicates and re-projects the
game's single-view draw calls and the game's only job is to nominate the convergence plane and
avoid drawing things at screen depth.

The three non-stereo D3D9 NVAPI calls (`GetSurfaceHandle`, `StretchRectEx`) belong to the
INTZ depth-buffer-read path at 0x6e4d60 / 0x71f640, not to stereo. `Stereo_IsEnabled` is used
only as a *capability gate*: in 0x71f640 the game checks vendor == 0x10DE, then if stereo is
enabled system-wide it demands a higher driver version (`>= 0x65BD`) before trusting the INTZ
path. That is a driver-bug workaround, not a render decision.

---

## 4. The complete stereo-conditional surface of the engine

There are exactly two pieces of stereo state in the binary:

* **`g_stereoHandle`** at **0x396346c** — the `StereoHandle` from NVAPI. 8 references.
* **`g_stereoActive`** at **0x396346b** — a bool cached from `Stereo_IsActivated`. 7 references.

Because there is nothing else, enumerating their references enumerates *every* place the
engine behaves differently in stereo. That list is closed and short.

`g_stereoHandle` (0x396346c):

| site | what it does |
|---|---|
| 0x6b75e0 / 0x6b75ee / 0x6b7603 | in the D3D device-creation routine **0x6b7530**: after `IDirect3D9::CreateDevice` (vtable +0x40) succeeds, zero the handle, `NvAPI_Stereo_CreateHandleFromIUnknown(pDevice, &g_stereoHandle)`, then prime `g_stereoActive` from `Stereo_IsActivated` |
| 0x6b815c / 0x6b816d | device shutdown: `Stereo_DestroyHandle`, then zero |
| 0x6eb34a / 0x6eb38e | the Present code in §2 |
| 0x71f939 | a size global (0x39660c4) is set to 0x400; then `if (g_stereoHandle != NULL && renderWidth[0x3966150] <= 0x400) it becomes 0x200` — a **budget halving**, presumably because the driver is about to double the game's surface allocations |

`g_stereoActive` (0x396346b):

| site | what it does |
|---|---|
| 0x6b75e7 (write 0), 0x6b761f (write) | device creation, above |
| 0x6eb379 (read), 0x6eb406 (write 0) | Present, above |
| **0x6b8b21** | the accessor **`R_IsStereoActive()` at 0x6b8b20** — literally `mov al, [0x396346b]; ret` |
| 0x6d28dd | in the big frame routine at 0x6d2100: `if (!g_stereoActive && !r_reflectionProbeRegenerateAll) call 0x6e3350(...)` — one render step is **skipped** in stereo |
| 0x723af3 | early-out at the top of a float computation (blend/DOF-shaped) when stereo is active — another effect **disabled** in stereo |

`R_IsStereoActive()` has exactly **six** callers: 0x4103c7, 0x4bf709, 0x773bb2, 0x7742ec,
0x774dcb, 0x77516b. One of them (0x77516b) is the convergence generator from §2. The other
five are all in HUD / crosshair / reticle code, and three of them (0x4103c7, 0x4bf709,
0x7742ec) contain the **identical** stereo branch:

```
    call  R_IsStereoActive
    test  al, al
    je    <2D path>
    ; stereo path: read the client view origin and view axis out of client state
    movss xmm1, [esi + 0x8c120]   ; origin.x        (+0x8c124 = y, +0x8c128 = z)
    movss xmm4, [esi + 0x8c134]   ; forward.x       (+0x8c138 = y, +0x8c13c = z)
    movss xmm0, [0x9d6508]        ; 15000.0
    mulss xmm4, xmm0
    addss xmm4, xmm1              ; origin + forward * 15000
    ...
```

That is: **when stereo is on, draw this HUD element as a world-space quad 15000 units down the
view axis instead of as a screen-space 2D element.** It is the standard 3D Vision Automatic
HUD fix — give the overlay a real depth so the driver's automatic separation puts it somewhere
sane instead of pinned at the screen plane. The sixth caller, 0x773bb2, does a smaller version:
in stereo it widens/offsets a 2D rect (`x → -x, 2x`; `y → -y, 2y`) instead of using it as-is.

**None of these render anything twice. None of them index an eye. There is no eye parameter
anywhere in the call chain.**

---

## 5. Things checked and ruled out

* **Two swap chains / one per eye.** `0x39660dc` is an array (count at 0x39660d4, stride 0x10)
  and looked promising. It is not per-eye: 0x6b7890 appends one entry per successfully
  validated *display mode* during device init, and 0x71f360 populates slot 0 via
  `IDirect3DDevice9::GetSwapChain(dev, 0, &arr[0])` + `GetBackBuffer`. Present (0x6eb1f0)
  presents index `[0x39660d0]` once. Single view, single present.
* **SLI misread as stereo.** `0x39660b4` next to the stereo globals is a *GPU count* from
  `NvAPI_D3D_GetCurrentSLIState` (0x6b78ff) / an AMD equivalent (0x98ea00), clamped to 4. Not
  eyes.
* **A separate non-NVIDIA stereo path** (e.g. the game presenting twice for AMD/Intel). None
  exists — there is no stereo code outside the NVAPI-gated blocks above, and no vendor branch
  other than the SLI and INTZ ones.
* **Quad-buffer / stereo surfaces.** No `NvAPI_Stereo_SetSurfaceCreationMode`, no
  `D3DPRESENT`-level stereo flag, D3D9 (not D3D9Ex) only.
* **Other stereo dvars.** A full string sweep of every section for
  `stereo|convergence|3d|eye|separation|nvapi|nv3d` and for identifiers matching `*3D*` turns
  up only the four dvars in §1 plus unrelated audio (`snd_stereo_3d`, `snd_draw3D`), AI
  (`geteye`, `haseyes`), UI (`r_ui3d_*`, `ui3dWindow`) and script names. There is no hidden
  `r_stereo3D`, no `r_eyeSeparation`, no per-eye toggle.

---

## 6. What is established vs. what is inferred

**Established (direct disassembly, addresses given above):**

1. `r_convergence` and `r_use_driver_convergence` are registered at 0x6ce2ba / 0x6ce281 and
   stored to 0x3b1fc54 / 0x3b1fca8.
2. The only consumer of `r_convergence` is `NvAPI_Stereo_SetConvergence` at 0x6eb3a0, gated on
   `!r_use_driver_convergence`. Its only producer is `Dvar_SetFloat` at 0x7751a8.
3. The binary's NVAPI import table (0xba4734, 230 entries) contains the full stereo API, but
   only `Stereo_IsEnabled`, `Stereo_CreateHandleFromIUnknown`, `Stereo_DestroyHandle`,
   `Stereo_IsActivated` and `Stereo_SetConvergence` are ever called.
4. `NvAPI_Stereo_SetActiveEye`'s interface ID does not occur anywhere in the file.
5. `r_stereoTurretShift` (0xb535ac) and `cg_drawCrosshair3D` (via 0x2f67ba8) are registered and
   never read. `cg_hudLegacyStereo3DScale` does not exist in the PC build.
6. The complete set of code that branches on stereo state is 13 sites, listed in §4, and each
   one either disables an effect, resizes a budget, or moves a HUD element into world space.

**Inferred (reasonable but not proven):**

* The named identities of NVAPI functions rest on the published interface-ID table plus
  argument-count agreement. I am confident about the five stereo ones and about
  `Initialize`/`GetErrorMessage`; `0xB380F218` is a guess.
* `[esi+0x8a4cc]` being an ADS blend factor is inferred from the 6.06→1.92 convergence lerp,
  not proven.
* 0x6e3350 (the pass skipped in stereo at 0x6d28dd) and 0x723af0 were not identified. They are
  skipped/short-circuited under stereo; I did not determine what they are.

**Limits of the negative.** This is a closed-world argument over reachable code: I enumerated
every reference to the two stereo globals and every call into the NVAPI thunk table, and found
no eye iteration. A dormant path that touched *neither* global and *no* NVAPI entry point —
i.e. pure dead code, never called and gated on nothing — would not show up in these scans, and
I did not do a whole-binary control-flow reachability analysis to exclude it. But such a path
would by definition be unreachable and undrivable, so it would not change the architecture
decision either way. There is no plausible route by which BAC-274 could "turn on" an engine
two-view renderer, because there is no dvar, no NVAPI hook, and no stereo state that leads to
one.

---

## 7. Recommendation

Proceed with **alternate-eye rendering driven from outside the engine**. The engine offers
nothing to hijack.

Two small things worth carrying forward:

* **`r_convergence` is a real, live, writable float that the game itself drives per-frame
  (0x7750b0).** If the mod ever wants the engine's own notion of "how far away is the player
  looking right now", the blend at 0x775184 is a free source of it, and the dvar at 0x3b1fc54
  is readable at `+0x18`. That is a depth/focus hint, not a stereo mechanism.
* **`R_IsStereoActive()` at 0x6b8b20 is a one-instruction function returning a byte at
  0x396346b.** Forcing that byte to 1 would switch on the game's world-space HUD placement and
  turn off the two effects disabled in stereo — without any NVIDIA driver involvement, since
  everything downstream of it is pure game logic. That is potentially useful for a VR HUD, and
  it is the *only* engine-side stereo behaviour that can be triggered independently of NVAPI.
  Note it is written every frame from `Stereo_IsActivated` at 0x6eb378, so it would have to be
  forced after that store or the store patched out.
