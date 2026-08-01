# BAC-276 — Does a published BO1 address map land on our CEG-wrapped binary?

**Answer: yes, byte-exactly. The systematic offset is 0. Confidence: very high.**

Custom Executable Generation does **not** shift the address space of our copy of
`BlackOps.exe`. A hardcoded address map published for the retail Steam single-player
executable — specifically the one in `xoxor4d/t5-rtx` — applies to our file with no
rebasing, no delta, and no per-address fixups.

This was the load-bearing assumption behind reusing published symbols. It is now
measured rather than inferred.

---

## 1. What was tested, and against what

| | |
|---|---|
| Target | `/mnt/games/steam/steamapps/common/Call of Duty Black Ops/BlackOps.exe` |
| Size | 8,101,944 bytes |
| MD5 | `2b179a57416680b60462c5af05552ea2` |
| SHA-256 | `cc9aee4fd1bb3d4d8290d637b1a09f8413ec5c728065bb5e7baed15239f48548` |
| `ImageBase` | `0x400000`, `DllCharacteristics 0x8000` → no ASLR |

Section layout (`pefile`), which every address below is interpreted against:

| section | VA start | VA end (virtual) | virtual size | raw size |
|---|---|---|---|---|
| `.text` | `0x401000` | `0x9A2A10` | `0x5A1A10` | `0x5A1C00` |
| `.rdata` | `0x9A3000` | `0xB6B6BA` | `0x1C86BA` | `0x1C8800` |
| `.data` | `0xB6C000` | `0x465F6C4` | `0x3AF36C4` | `0x3A400` |
| `.tls` | `0x4660000` | `0x466006D` | `0x6D` | `0x200` |
| `.rodata` | `0x4661000` | `0x4661770` | `0x770` | `0x800` |
| `.version` | `0x4662000` | `0x4662004` | `0x4` | `0x200` |
| `.rsrc` | `0x4663000` | `0x46760C4` | `0x130C4` | `0x13200` |

`SizeOfImage 0x4277000`. Note `.data` is overwhelmingly BSS — 0x3AF36C4 virtual
against 0x3A400 raw — which is why the published globals sit at addresses like
`0x40CA570` that are far past the end of the file on disk.

The reference map is `src/game/functions.hpp`, `src/game/functions.cpp`,
`src/game/structs.hpp` and the three SP modules of **`xoxor4d/t5-rtx`**
(`master`, GitHub API reports `"license": null` — **all rights reserved**).
It was fetched and read as *documentation of facts about a binary*. Addresses and
structure offsets are independently rediscoverable measurements, not authorship;
**no code from it is copied into this repository**, and every description below is
written from our own disassembly. If we later want these symbols in source form,
we generate them from our own analysis, not from their headers.

Tooling: `pefile 2024.8.26` + `capstone 5.0.7` under the scratchpad venv, plus
`objdump`, `openssl`, `strings`. The binary was never executed.

---

## 2. Headline result: the systematic offset is zero

Four independent classes of test were run at every candidate offset δ in
[-256, +256] plus every multiple of 0x1000 in [-0x10000, +0x10000]:

* **entry** (18 tests) — is the published function address a function entry, *and*
  is it the target of at least one `E8` call somewhere in `.text`?
* **noplen** (19 tests) — the plugin overwrites N bytes at an interior address.
  Do the instructions starting there have boundaries summing to **exactly** N?
* **call** (3 tests) — sites the plugin redirects as calls. Is the byte `0xE8`?
* **jcc** (3 tests) — sites the plugin rewrites to `0xEB`. Is the byte a short
  conditional jump (`0x70`–`0x7F`)?

```
  delta      total   entry(18) noplen(19) call(3) jcc(3)
        +0   43/43      18        19        3      3
       +48   14/43       2        11        0      1
      +203   13/43       0        11        2      0
      +143   13/43       0        13        0      0
        +5   13/43       0        11        2      0
      +256   12/43       2        10        0      0
```

**δ = 0 scores 43/43. The next best offset anywhere in the swept range scores 14/43**,
and its score is entirely explained by the base rate of short instructions. There is
no second candidate. If CEG shifted our address space by any amount up to ±64 KiB,
this sweep would have found it.

### Base rate — the tests are not trivially satisfiable

Over 4,000 randomly chosen 16-byte-aligned addresses in `.text`:

```
random 16-aligned .text addrs: n=4000  FUNC_START=277 (6.9%)  FUNC_START & callers>0=157 (3.9%)
random unaligned .text addrs:  n=4000  FUNC_START=65  (1.6%)
distinct E8 call targets in .text: 16317
```

So "is a called function entry" is satisfied by chance 3.9% of the time. Eighteen
independent hits at 3.9% each is ~1 in 10²⁶. And the noplen/call/jcc tests are
interior-address tests, which a mere rebase-detector could not pass at all.

### A built-in negative control

`functions.hpp` and `functions.cpp` carry a dozen **commented-out** addresses,
several of which are duplicate names with different values (two different
`Cbuf_AddText`, two `Vec2UnpackTexCoords`, two `DB_FileExists`) — evidently
copy-paste residue from a different CoD title. They are the perfect control group,
and our tests reject them cleanly:

| commented-out address | claimed name | verdict | callers |
|---|---|---|---|
| `0x594200` | Cbuf_AddText | not a function start (`add byte ptr [eax], al`) | 0 |
| `0x5E4D30` | Vec2UnpackTexCoords | unpadded, mid-code | 0 |
| `0x48FC10` | DB_FileExists | mid-instruction (`adc edi, [edx]`) | 0 |
| `0x59AC50` | Com_Error | mid-instruction (`and al, 0x14`) | 0 |
| `0x48D560` | DB_EnumXAssets_FastFile | mid-instruction (`fmul dword ptr [edi]`) | 0 |
| `0x48E7B0` | DB_LoadXAssets | mid-instruction (`add al, 0x32`) | 0 |
| `0x54EAB0` | G_Spawn | not a function start | 0 |
| `0x5465A0` | G_CallSpawnEntity | not a function start | 0 |
| `0x54AC40` | G_DObjUpdate | not a function start | 0 |
| `0x5C5100` / `0x5EED90` | Dvar_RegisterVariant | not function starts | 0 |
| `0x54A480` | G_ModelIndex | *is* a padded entry, but uncalled — **weak, unresolved** | 0 |

11 of 12 rejected. Every *active* address passed. A methodology that accepted
everything would have accepted these too.

---

## 3. Hit / miss table — named functions

`FUNC_START` = preceded by `0xCC` inter-function padding (MSVC's filler) or by a
terminator instruction ending exactly at the address, and the first instruction
decodes as a plausible prologue. `callers` = count of `E8 rel32` instructions in
`.text` targeting the address.

### 3.1 The published symbol map (`functions.hpp` / `functions.cpp`, SP)

| address | claimed symbol | verdict | callers | corroboration | strength |
|---|---|---|---|---|---|
| `0x5AE810` | `Dvar_FindVar(const char*)` | HIT | 57 | takes `[esp+4]`, null-checks, empty-string-checks, then hashes; call sites push `"fs_game"`, `"ui_deadquote"`, `"saved_gameskill"`; three call sites sit beside `"Dvar %s has an invalid dvar name"` | **strong** |
| `0x51BD00` | `Dvar_RegisterEnum` | HIT | 38 | **38 of 38** call sites push a dvar name within 64 bytes (`"debugRenderMask"`, `"snd_speakerConfiguration"`, `"cg_drawFPS"`) | **strong** |
| `0x45BB20` | `Dvar_RegisterBool` | HIT | 758 | 393 call sites push a dvar name; `sub esp,0x10` then narrows an argument to `al` (a `bool`) | **strong** |
| `0x679020` | `Dvar_RegisterFloat` | HIT | 1054 | 389 call sites push a dvar name; entry moves three `movss` float arguments off the stack — matches `(name, value, min, max, flags, desc)` | **strong** |
| `0x6D4080` | `Material_RegisterHandle` | HIT | 213 | 176 call sites push a material name (`"white"`, `"hud_firstplace"`, `"gfx_fxt_fx_distortion_water_droplet"`); empty-name path returns a fixed default material pointer | **strong** |
| `0x61BE00` | `Dvar_HasLatchedValue` | HIT | 10 | signature shape only (`dvar_s*` in `[esp+4]`) | weak |
| `0x61CFC0` | `Dvar_MakeLatchedCurrent` | HIT | 7 | signature shape only | weak |
| `0x67D4F0` | `Sys_GetValue(int)` | HIT | 577 | reads TLS via `fs:[0x2C]` with slot index from `[0x3956508]`, indexes `[tls+0x28]` by the `int` argument, returns it — exactly `void*(int)` | **strong** |
| `0x5A48F0` | `Sys_IsMainThread()` | HIT | 71 | same TLS preamble and same slot global `0x3956508`, then tests `[tls+0x2C]` — a sibling accessor in the same translation unit | **strong** |
| `0x5AC8E0` | `Vec2UnpackTexCoords` | HIT | 2 | signature shape only (`unsigned`, `float*`) | weak |
| `0x5B4B50` | `DB_FileExists(name, src)` | HIT | 4 | reads two stack args, allocates a 0x100 path buffer, formats into it | moderate |
| `0x726650` | `R_SetRenderTarget(src, state, idx)` | HIT | 48 | reads exactly three cdecl args; clamps the index against 3 and 0xA; compares it to `[state+0x1128]` (the current render-target id) and indexes a render-target table at `0x45EB1E8` | **strong** |
| `0x7265E0` | `R_SetRenderTargetSize(src /*ecx*/, idx /*al*/)` | HIT | 26 | takes the struct in `ECX` and the index in `AL` exactly as published, then writes `[ecx+0x1A7C]`, `[ecx+0x1A80]`, `[ecx+0x1A84]` — see §5, these are the published `viewportBehavior` / `renderTargetWidth` / `renderTargetHeight` | **strong** |
| `0x6B32C0` | `R_AddCellSurfacesAndCullGroupsInFrustumDelayed` | HIT | 6 | `sub esp,0x18` frame; renderer-region address | weak |
| `0x6FCC50` | `R_AddDebugString` | HIT | 2 | prologue only | weak |
| `0x619D00` | `Cmd_ExecuteSingleCommand(int,int,const char*)` | HIT | 27 | reads `[esp+0xC]` first — the third argument, the command string | moderate |
| `0x49B930` | `Cbuf_AddText` | HIT | 115 | prologue `push 0x35`; heavily called | weak-moderate |
| `0x43C520` | `Com_PrintMessage(int,const char*,char)` | HIT | 21 | first argument compared against `0x1F` (channel count) before use; second argument treated as a string with a 0x1000 length bound | moderate |
| `0x7244C0` | `R_Set3D` *(commented out upstream)* | **partial** | 0 | is a valid entry, but the body is `call 0x4ED550; jmp 0x6D4350` — a CEG deobfuscation **thunk**, not the function body. Address is real; the *name* is unconfirmed | **flagged** |

**19/19 land on real function entries. 18/19 are additionally proven live by call
count.** Ten carry independent semantic corroboration; six are prologue-plus-shape
only and are marked weak; one (`R_Set3D`) is a thunk whose identity we could not
confirm and should not be relied on without re-derivation.

### 3.2 Named engine/renderer functions from the plugin's SP modules

All 44 checked are function entries; 40 are also live call targets.

| address | name as published | verdict | callers | note |
|---|---|---|---|---|
| `0x6B82E0` | `R_Init` | HIT | 1 | body references `"fonts/smalldevfont"` and *"Sun sprite occlusion query calibration failed; reverting to low-quality sun visibility test"* — unambiguously renderer init. **strong** |
| `0x4F20F0` | `LiveStats_Init` | HIT | 0 | first instruction is `push 0x9D0CD0` = `"ddl/stats.ddl"`. **strong** |
| `0x62DD40` | `LiveStats_ResetStats` | HIT | 1 | reached only through the CEG wrapper below |
| `0x79E6D0` | `Con_Restricted_SetLists` | HIT | 0 | reached only through the CEG wrapper below |
| `0x6CFB30` | `RB_EndSceneRendering` | HIT | 3 | called from `0x6D0519`, see §4 |
| `0x6CFD20` | `R_SetAndClearSceneTarget` | HIT | 2 | called from `0x6D0403`, see §4 |
| `0x745280` | `R_SkinXModelCmd` | HIT | 1 | |
| `0x6B0B00` | `R_AddAllSceneEntSurfacesCamera` | HIT | 1 | |
| `0x6B7220` | `R_CompareDisplayModes` | HIT | 0 | passed to `qsort`, hence no `E8` caller |
| `0x6B3DA0` | `R_AddCellSurfacesAndCullGroupsInFrustum` | HIT | 1 | `sub esp,0xD24` — a large frustum working set |
| `0x73F8C0` | `R_SetMaterial` | HIT | 0 | |
| `0x737ED0` | `R_DrawLit` | HIT | 2 | |
| `0x6E21B0` | `R_SetUI3DWindow` | HIT | 3 | |
| `0x6E26A0` | `RB_UI3D_Render` | HIT | 2 | |
| `0x528A20` | `DB_IsZoneLoaded` | HIT | 7 | |
| `0x631B10` | `DB_LoadXAssets` | HIT | 18 | `sub esp,0x310` |
| `0x70B210` | renderer dvar registration | HIT | 1 | called from `0x6B833A`, see §4 |
| `0x8EE640` | CEG helper hooked by the plugin | HIT | 30 | |
| `0x6B8B20` | `R_IsStereoActive` *(from our own prior work)* | HIT | 6 | see §6 |

Plus 25 further addresses used as call targets or patch anchors, all of which are
function entries: `0x621530`, `0x4C8890`, `0x50DB60`, `0x4F5F30`, `0x6BFBD0`,
`0x6BFDF0`, `0x6CA150`, `0x60B390`, `0x5D2F00`, `0x434750`, `0x4155F0`, `0x658F70`,
`0x5A06C0`, `0x5A0720`, `0x6D7B90`, `0x6EBBB0`, `0x5DF2B0`, `0x6EB760`, `0x740C30`,
`0x5C4BA0`, `0x6ED220`, `0x7244F0`, `0x49D640`, `0x723E90`, `0x727860`.

### 3.3 The CEG functions themselves — 80/80

The plugin neutralises 80 distinct CEG check functions. This is the most direct
possible test of the question, because these are exactly the functions CEG
*generates*: if personalisation moved anything, it would move these first.

**All 80 are function entries in our binary**, and — decisively — the code shape at
each address matches the category the upstream author assigned to it:

| upstream category | count | prologue we observe at every address in the group |
|---|---|---|
| "ceg func called by multiple other funcs" | 2 | `cmp byte ptr [esp+4], 0` (identical clones; 37 and 33 callers) |
| "Registry key checks" | 3 | `sub esp, 0x84` |
| "Single function, 32-bit hash check" | 5 | `mov eax, dword ptr [<global>]` |
| "Direct `ExitProcess()` check" | 10 | `sub esp, 0x74` or `sub esp, 0x474` |
| "Wrapper `ExitProcess()` check, day-of-week gated" | 40 | `push ecx` — **all forty** |
| "Wrapper check with HWBP detection" | 16 | `push ebp` — all sixteen, at `0x9A23B0` + 0x40·k, a perfectly regular table, each preceded by exactly 12 `0xCC` bytes |
| "Direct HWBP check" | 3 | `push ebp`, at `0x9A2980` + 0x30·k |

Two independent regular tables (stride 0x40 and stride 0x30, right at the end of
`.text` at `0x9A2xxx`, just under the section end `0x9A2A10`) with uniform prologues
and uniform padding. That is CEG's generated code, sitting exactly where the
published map says it is.

The three CEG *obfuscation wrappers* are also confirmed, and their published
forwarding targets are correct in structure:

```
0060CC10  e89b0cf9ff   call 0x59D8B0      ; CEGObfuscate<LiveStats_Init>
0060CC15  ffe0         jmp eax
0063DCC0  e8abc4f6ff   call 0x5AA170      ; CEGObfuscate<LiveStats_ResetStats>
0063DCC5  ffe0         jmp eax
00580460  e8ab900100   call 0x599510      ; CEGObfuscate<Con_Restricted_SetLists>
00580465  ffe0         jmp eax
```

Each is a 7-byte `call <deobfuscator>; jmp eax` stub followed by `0xCC` padding —
the resolver returns the real target in `EAX`. The three published real targets
(`0x4F20F0`, `0x62DD40`, `0x79E6D0`) are all function entries with **zero** `E8`
callers, exactly as expected for functions reachable only through such a stub.
That the caller-count is 0 in each case is itself corroboration, not a miss.

---

## 4. Interior-address tests — the reason confidence is "very high"

Function entries are 16-byte aligned and abundant; landing on one is suggestive.
Landing byte-exactly *inside* a function is not. Every test below targets an
interior address.

### 4.1 Overwrite-length tests — 19/19 exact

The plugin blanks N bytes at an address. If our instruction boundaries differed by
even one byte, the decoded length would not equal N.

| address | N | what our binary actually decodes there | ✓ |
|---|---|---|---|
| `0x7D905F` | 5 | `call 0x54B930` | ✓ |
| `0x7D91CF` | 5 | `call 0x5289B0` | ✓ |
| `0x6C8DEB` | 6 | `mov ecx, [esi+0x183A8]` | ✓ |
| `0x6B5FC4` | 9 | `lea esi,[ecx+edx*8]` + `mov edx,[0x3B1FAE0]` | ✓ |
| `0x6B5FB3` | 2 | `jl 0x6B601C` | ✓ |
| `0x7217DC` | 2 | `jne 0x7217FC` | ✓ |
| `0x7215D9` | 6 | `jae 0x72183D` | ✓ |
| `0x72173F` | 2 | `jne 0x72175F` | ✓ |
| `0x74784D` | 2 | `jne 0x747817` | ✓ |
| `0x747618` | 2 | `jne 0x7475E2` | ✓ |
| `0x5E6F24` | 6 | `xor ecx,ecx` + `test edx,edx` + `jbe 0x5E6F83` | ✓ |
| `0x6B5FAD` | 6 | `mov [esp+0x10],eax` + `test esi,esi` | ✓ |
| `0x7215D2` | 7 | `addss xmm5,xmm6` + `comiss xmm4,xmm5` | ✓ |
| `0x72160E` | 7 | `addss xmm5,xmm0` + `comiss xmm5,xmm4` | ✓ |
| `0x6D0516` | 8 | `push ebx` + `push esi` + `push ebp` + `call 0x6CFB30` | ✓ |
| `0x6D04FE` | 5 | `call 0x723E90` | ✓ |
| `0x6D0424` | 6 | `lea edx,[edi+0x2320]` | ✓ |
| `0x6EBEC9` | 5 | `call 0x6B82E0` | ✓ |
| `0x6B855E` | 7 | `call 0x5F4140` + `test eax,eax` | ✓ |

Three of these decode into calls whose **targets are themselves separately
published numbers**: `0x6D0516` → `RB_EndSceneRendering` `0x6CFB30`,
`0x6EBEC9` → `R_Init` `0x6B82E0`, `0x6D04FE` → `0x723E90`. Two independently
published hex constants agreeing through a `call rel32` in our file cannot survive
any nonzero shift — a shift would have to move both by the same amount *and* leave
the relative displacement byte-identical, which is only true for δ = 0.

### 4.2 Opcode-class tests — 6/6

| address | required | our bytes | ✓ |
|---|---|---|---|
| `0x6B8300` | `0xE8` call | `e8 ebc10600` → `call 0x7244F0` | ✓ |
| `0x52F28A` | `0xE8` call | `e8 b1e3f6ff` → `call 0x49D640` | ✓ |
| `0x6B8559` | `0xE8` call | `e8 e296e3ff` → `call 0x4F1C40` | ✓ |
| `0x74785A` | short `jcc` | `0x7E` → `jle 0x7478AA` | ✓ |
| `0x747648` | short `jcc` | `0x7E` → `jle 0x7476AC` | ✓ |
| `0x6B8565` | short `jcc` | `0x74` → `je 0x6B857A` | ✓ |

`0x6B8300` and `0x52F28A` both resolve to separately published targets
(`0x7244F0`, `0x49D640`).

### 4.3 Resume-address tests — 18/20

Detour stubs record the address to jump back to. Decoding forward from the
enclosing function entry, each must be an exact instruction boundary:

`0x6B6016` ✓ `0x721615` ✓ `0x721645` ✓ `0x6B69BE` ✓ `0x6B601C` ✓ `0x5E6F2A` ✓
`0x7215DF` ✓ `0x72169D` ✓ `0x6D0521` ✓ `0x6D042A` ✓ `0x736E2E` ✓ `0x6F9170` ✓
`0x725C4B` ✓ `0x523BF6` ✓ `0x523BE0` ✓ `0x7279E5` ✓ `0x6CA0FA` ✓ `0x6C750A` ✓

The two that miss — **`0x74A315` and `0x74A31B`** — belong to
`cull::entities_stub`, and **that hook is commented out upstream and never
installed**. Its inline assembly expects `and [esp+0xC],esi; mov esi,[esp+0x4C]`
at `0x74A30D`; our binary has a loop tail there:

```
0074A30A  47             inc edi
0074A30B  83c408         add esp, 8
0074A30E  3b7c2410       cmp edi, dword ptr [esp+0x10]
0074A312  72c5           jb 0x74A2D9
0074A314  8b54242c       mov edx, dword ptr [esp+0x2c]
```

The linear decode is self-consistent from `0x74A2F0`, so this is not a
desynchronised disassembly — it is stale, disabled upstream data, in exactly the
same class as the commented-out control group in §2. **Every enabled patch site
matches; only disabled ones do not.**

### 4.4 Two-number agreements — 25 found

Across all 341 published constants that fall inside `.text`, 25 pairs are related
in our binary by a direct `call`/`jmp` where **both** endpoints are independently
published:

```
0x50F41D → 0x4C8890   0x51226D → 0x49B930   0x51B175 → 0x4C8890   0x52F28A → 0x49D640
0x64FEAD → 0x5C4BA0   0x67FA19 → 0x6BFDF0   0x6B8300 → 0x7244F0   0x6B833A → 0x70B210
0x6B8456 → 0x6ED220   0x6D0403 → 0x6CFD20   0x6D5908 → 0x6EB760   0x6EBE84 → 0x5A06C0
0x6EBEC9 → 0x6B82E0   0x7A18AA → 0x6D7B90   0x88A410 → 0x4C8890
0x4C8890+12 → 0x621530   0x523BD8+3 → 0x523BE0   0x5A272C+15 → 0x45BB20
0x5A272E+13 → 0x45BB20   0x6CA3E0+12 → 0x45BB20   0x6D0516+3 → 0x6CFB30
0x6EBE84+5 → 0x5A0720    0x829B42+11 → 0x67D4F0   0x867C5B+9 → 0x5DF2B0
```

Two of these are worth calling out because they close a loop:

* At `0x6B833A` our binary has `call 0x70B210`, and the *next* instruction starts at
  `0x6B833F`. Upstream independently publishes `0x6B833A` as the hook site,
  `0x70B210` as the function to call through, and `0x6B833F` as the resume address.
  Three separately published numbers, all correct simultaneously.
* At `0x6C8DEB` the 6-byte instruction ends at `0x6C8DF1` — the published resume
  address — and the instruction *at* `0x6C8DF1` is `mov edx, dword ptr [0x3B3708C]`,
  where `0x3B3708C` is the separately published `frontEndDataOut` pointer global.
  A code address and a data address confirming each other.

---

## 5. Verified struct layouts

### 5.1 `GfxCmdBufSourceState` — global at `0x40CA570`

`0x40CA570` is referenced 120 times as an absolute immediate in `.text`
(`mov esi, 0x40CA570`, `push 0x40CA570`), i.e. it is a global struct base.

Layout, with the evidence for each offset. "abs refs" counts `.text` instructions
referencing `0x40CA570 + offset` as an absolute address.

| offset | field | type | abs refs | evidence |
|---:|---|---|---:|---|
| `0x0000` | `matrices` | `GfxMatrix[32]` (0x800) | 120 | base |
| `0x0800` | union: `GfxCmdBufInput` / `{ byte gap[0xE60]; GfxViewParms }` | 0xFA0 | — | size forced by `pad` at `0x17A0`; internal placement **unverified**, see below |
| `0x17A0` | `pad` | `char[592]` | 0 | boundary implied |
| **`0x19F0`** | **`eyeOffset`** | **`float[4]`** | 11 | **direct — see below** |
| `0x1A00` | `shadowableLightForShadowLookupMatrix` | `u32` | 0 | |
| `0x1A04` | `objectPlacement` | ptr | 0 | |
| **`0x1A08`** | **`viewParms3D`** | **`GfxViewParms*`** | 11 | **direct — see below** |
| `0x1A0C` | `depthHackFlags` | `u32` | 0 | |
| `0x1A10` | `skinnedPlacement` | `GfxScaledPlacement` (0x20) | 1 | size forced by `cameraView` at `0x1A30` |
| `0x1A30` | `cameraView` | `int` | 0 | |
| `0x1A34` | `viewMode` | enum | 27 | written `mov [0x40CBFA4], ebx` |
| `0x1A38` | `sceneDef.time` | `int` | 18 | |
| `0x1A3C` | `sceneDef.floatTime` | `float` | 1 | |
| `0x1A40` | `sceneDef.viewOffset` | `float[3]` | 0 | |
| `0x1A4C` | `sceneViewport` | `GfxViewport` (0x10) | 15 | filled as two 8-byte stores at `0x40CBFBC`/`0x40CBFC4` |
| `0x1A5C` | `scissorViewport` | `GfxViewport` | 3 | |
| `0x1A6C` | `materialTime` | `float` | 0 | |
| `0x1A70` | `destructibleBurnAmount` | `float` | 0 | |
| `0x1A74` | `destructibleFadeAmount` | `float` | 0 | |
| `0x1A78` | `wetness` | `float` | 0 | |
| `0x1A7C` | `viewportBehavior` | enum | 48 | written by `R_SetRenderTargetSize` |
| `0x1A80` | `renderTargetWidth` | `int` | 69 | written by `R_SetRenderTargetSize` |
| `0x1A84` | `renderTargetHeight` | `int` | 69 | written by `R_SetRenderTargetSize` |
| `0x1A88` | `viewportIsDirty` | `bool` | 20 | `mov byte [0x40CBFF8], 1` |
| `0x1A89` | `scissorEnabled` | `bool` | 3 | |
| `0x1A8C` | `shadowableLightIndex` | `u32` | 0 | |

Total size `0x1A90`.

**`R_SetRenderTargetSize` confirms three offsets at once.** Our disassembly of
`0x7265E0`, which the map claims takes the struct in `ECX`:

```
007265E0  56                push esi
007265E1  8bf1              mov esi, ecx              ; <- struct pointer arrives in ECX, as published
...
00726620  898e7c1a0000      mov dword ptr [esi+0x1a7c], ecx    ; viewportBehavior
00726626  0fb790f4b15e04    movzx edx, word ptr [eax+0x45eb1f4]
0072662D  8996801a0000      mov dword ptr [esi+0x1a80], edx    ; renderTargetWidth
00726633  0fb780f6b15e04    movzx eax, word ptr [eax+0x45eb1f6]
0072663A  8986841a0000      mov dword ptr [esi+0x1a84], eax    ; renderTargetHeight
```

**`eyeOffset[4]` at `+0x19F0` is confirmed directly**, at `0x6D2E12`:

```
006D2E0F  0f57c0                    xorps xmm0, xmm0
006D2E12  f30f110560bf0c04          movss dword ptr [0x40CBF60], xmm0   ; eyeOffset[0] = 0
006D2E1A  f30f110564bf0c04          movss dword ptr [0x40CBF64], xmm0   ; eyeOffset[1] = 0
006D2E22  f30f110568bf0c04          movss dword ptr [0x40CBF68], xmm0   ; eyeOffset[2] = 0
006D2E2A  f30f10053c62b400          movss xmm0, dword ptr [0xB4623C]    ; = 1.0f
006D2E45  f30f11056cbf0c04          movss dword ptr [0x40CBF6C], xmm0   ; eyeOffset[3] = 1.0
```

`0x40CBF60 = 0x40CA570 + 0x19F0`. Four consecutive floats, zero/zero/zero/one —
a homogeneous offset vector uploaded as a shader constant. **This is the field the
VR work needs, and it is confirmed both in address and in semantics.** The same
basic block also writes `sceneViewport` (`0x40CBFBC`), `viewMode` (`0x40CBFA4`) and
`viewportIsDirty` (`0x40CBFF8`) — five published fields in ~50 bytes of code.

### 5.2 `GfxViewParms` — size `0x140`

| offset | field | type |
|---:|---|---|
| `0x000` | `viewMatrix` | `float[4][4]` |
| `0x040` | `projectionMatrix` | `float[4][4]` |
| `0x080` | `viewProjectionMatrix` | `float[4][4]` |
| `0x0C0` | `inverseViewProjectionMatrix` | `float[4][4]` |
| `0x100` | `origin` | `float[4]` |
| `0x110` | `axis` | `float[3][3]` |
| `0x134` | `depthHackNearClip` | `float` |
| `0x138` | `zNear` | `float` |
| `0x13C` | `zFar` | `float` |
| | **sizeof** | **`0x140`** |

Confirmed by taking all 8 sites that load the `viewParms3D` pointer
(`mov eax, dword ptr [0x40CBF78]`) and histogramming every displacement
subsequently used off `EAX`:

```
+0x004,+0x008                              viewMatrix row 0
+0x080 +0x084 +0x088 +0x08C                viewProjectionMatrix row 0   (5,5,4,5 uses)
+0x090 +0x094 +0x098 +0x09C                                      row 1   (5,5,4,5)
+0x0A0 +0x0A4 +0x0A8 +0x0AC                                      row 2   (5,5,4,5)
+0x0B0 +0x0B4 +0x0B8 +0x0BC                                      row 3   (3,3,3,3)
+0x100 +0x104 +0x108                       origin[0..2]
+0x110 +0x114 +0x118                       axis[0][0..2]
(nothing at or beyond +0x140)
```

All sixteen elements of the matrix at `+0x80` are used, and the code at `0x722B93`
is a textbook homogeneous point transform — multiply by rows 0–2, add row 3:

```
00722B93  a178bf0c04            mov eax, dword ptr [0x40cbf78]     ; viewParms3D
00722B98  f30f1098a0000000      movss xmm3, dword ptr [eax+0xa0]   ; m[2][0]
00722BA0  f30f109090000000      movss xmm2, dword ptr [eax+0x90]   ; m[1][0]
00722BB8  f30f59d1              mulss xmm2, xmm1
00722BC4  f30f59de              mulss xmm3, xmm6
00722BC8  f30f58da              addss xmm3, xmm2
00722BCC  f30f109080000000      movss xmm2, dword ptr [eax+0x80]   ; m[0][0]
00722BD4  f30f59d0              mulss xmm2, xmm0
00722BD8  f30f58da              addss xmm3, xmm2
00722BE4  f30f5898b0000000      addss xmm3, dword ptr [eax+0xb0]   ; + m[3][0]
```

The three-matrix stride to `+0x80` pins `viewMatrix` at `0` and `projectionMatrix`
at `0x40`. `origin` at `+0x100` and `axis` at `+0x110` are confirmed by direct use.
Nothing beyond `+0x13C` is ever touched through this pointer, consistent with
`sizeof == 0x140`; and `0x140` is independently forced by arithmetic, since the
union at `0x800` must end exactly at the verified `pad` offset `0x17A0`
(`0x800 + 0xE60 + 0x140 = 0x17A0`).

**Unverified, flag it:** the union's *second* interpretation places an embedded
`GfxViewParms` at struct offset `+0x1660` (absolute `0x40CBBD0`). We could not
confirm that. The three absolute references to `0x40CBBD0` are plain dword stores
next to a store to `0x40CBA5C`, which look like scalar fields of the union's *other*
member (`GfxCmdBufInput`), and a nearby three-float vector subtraction at
`0x40CBD00/04/08` does not line up with `axis[2][2]`/`depthHackNearClip`/`zNear`.
Because it is a union, both readings can be simultaneously true of the same bytes,
so this is not a contradiction — but **do not assume `viewParms3D` points at
`0x40CBBD0`. Dereference the pointer at `0x40CBF78` at runtime.**

### 5.3 Supporting structures

```
GfxMatrix        { float m[4][4]; }                                    // 0x40
GfxViewport      { int x, y, width, height; }                          // 0x10
GfxSceneDef      { int time; float floatTime; float viewOffset[3]; }   // 0x14
GfxCodeMatrices  { GfxMatrix matrix[32]; }                             // 0x800
GfxCullViewInfo  { GfxViewParms viewParms;      // 0x000
                   GfxViewport sceneViewport;   // 0x140
                   GfxViewport displayViewport; // 0x150
                   GfxViewport scissorViewport; }  // 0x160, sizeof 0x170
```

`GfxViewInfo` is `__declspec(align(16))` and begins with a union of
`GfxCullViewInfo` and an identical anonymous struct, i.e. `GfxViewInfo+0x000` is a
`GfxViewParms` and `GfxViewInfo+0x170` is a `GfxSceneDef`. Everything past that
point in `GfxViewInfo` depends on a long chain of effect structures we have **not**
verified — treat only the first `0x184` bytes as trustworthy.

`GfxCmdBufState` (global at `0x457EE00`, 92 absolute references): the only offset
we verified is `renderTargetId` at **`+0x1128`**, from `R_SetRenderTarget`'s
`cmp bl, byte ptr [esi+0x1128]`.

### 5.4 Global data addresses

All land in `.data` and all are referenced from `.text`.

| address | identity | exact refs | how it is used |
|---|---|---:|---|
| `0x3963440` | `DxGlobals dx` | 2 | struct base; see §6 |
| `0x3966180` | `r_global_permanent_t rgp` | 13 | `mov ecx,[ecx*4+0x3966180]` — array base |
| `0x40CA570` | `GfxCmdBufSourceState` | 120 | `mov esi, 0x40CA570` |
| `0x457EE00` | `GfxCmdBufState` | 92 | |
| `0x3E58E30` | `Ui3dTextureWindow[]` | 2 (68 within +0x200) | |
| `0x41706E0` | `GfxBuffers` | 1 (129 within +0x200) | |
| `0x2430014` | `cmd_function_s**` | 23 | |
| `0x460C0B0` | `GfxWorldDraw*` (pointer) | 49 | |
| `0x3B3708C` | `GfxBackEndData*` frontEndDataOut | 358 | `mov ecx,[0x3B3708C]` — see §4.4 |
| `0x3957100` | `DpvsGlob` | 6 (159 within +0x200) | |
| `0x4643FD8` | `GfxBackEndData**` SMP | 6 | |
| `0xC72280` | `field_t g_consoleField` | 24 | |
| `0xC48A48` / `0xC48A08` | console autocomplete state | 15 / 13 | `cmp dword [0xC48A48],0` |
| `0x2910160` | flags word | 46 | `test byte [0x2910160], 1` |
| `0x3956508` | Sys TLS slot index | 127 | used by `Sys_GetValue` **and** `Sys_IsMainThread` |

---

## 6. Independent confirmation from our own prior analysis

The strongest single piece of evidence in this report involves no shared provenance
at all. Separately, and without reference to the published map, we derived from
NVAPI call sites: `g_stereoActive` at `0x396346B`, `g_stereoHandle` at `0x396346C`,
`R_IsStereoActive` at `0x6B8B20`.

The published map says `DxGlobals` is at `0x3963440`. Laying its field list over
that base:

| offset | field | absolute | abs refs |
|---:|---|---|---:|
| `+0x00` | `hinst` | `0x3963440` | 2 |
| `+0x04` | `d3d9` (`IDirect3D9*`) | `0x3963444` | 28 |
| `+0x08` | `device` (`IDirect3DDevice9*`) | `0x3963448` | 143 |
| `+0x0C` | `adapterIndex` | `0x396344C` | 16 |
| `+0x10` | `vendorId` | `0x3963450` | 6 |
| `+0x28` | `supportsSceneNullRenderTarget` | `0x3963468` | 5 |
| `+0x29` | `supportsIntZ` | `0x3963469` | 15 |
| `+0x2A` | `nvInitialized` | `0x396346A` | 7 |
| **`+0x2B`** | **`nvStereoActivated`** | **`0x396346B`** | 7 |
| **`+0x2C`** | **`nvStereoHandle`** | **`0x396346C`** | 8 |
| `+0x30` | `nvDepthBufferHandle` | `0x3963470` | 5 |
| `+0x34` | `nvFloatZBufferHandle` | `0x3963474` | 5 |

`nvStereoActivated` at `+0x2B` **is** our `g_stereoActive` at `0x396346B`.
`nvStereoHandle` at `+0x2C` **is** our `g_stereoHandle` at `0x396346C`. And our
`R_IsStereoActive` is, in full:

```
006B8B20  a06b349603     mov al, byte ptr [0x396346b]
006B8B25  c3             ret
006B8B26  cc             int3
```

— a one-instruction accessor for `dx->nvStereoActivated`. Two address maps produced
by different people, from different tools, on different copies of the game, agree to
the byte on a global base address *and* a structure field offset *and* a function
entry. The `device` pointer at `+0x08` having 143 references (by far the busiest
field, as you would expect of `IDirect3DDevice9*`) is a further sanity check, and it
is the same `0x3963448` we saw loaded at `0x6D0408` in §4.

---

## 7. Why CEG does not shift this binary

Two structural facts, both measured here.

**The file is Authenticode-signed by Valve, and the signature covers this exact
byte sequence.** The signature's `SpcIndirectDataContent` digest is

```
A4F2B65A67533CC5C473F33F066FC4C4A144C5596F9B764820E4823DD88FB532
```

and the Authenticode SHA-256 we compute over the file (excluding only the checksum
field, the security data-directory entry, and the certificate blob itself) is
`a4f2b65a67533cc5c473f33f066fc4c4a144c5596f9b764820e4823dd88fb532` — identical.
The signing chain is `Valve Corp.` → `DigiCert Trusted G4 Code Signing RSA4096
SHA384 2021 CA1` → `DigiCert Trusted Root G4`, i.e. Valve signed the CEG output,
using a certificate issued 2021-04-29. There is no data trailing the certificate
blob (0 bytes), so essentially the whole file is covered.

For a *per-user* `.text` to exist, Valve would have to produce a fresh RSA signature
from its code-signing key for every user. That is an inference about Valve's
operations rather than a measurement, and it is the one link in this chain we cannot
close directly from the file — but combined with everything in §3.3 it is
overwhelming.

**CEG does not encrypt the code.** `.text` entropy is 6.73 overall and no 64 KiB
block exceeds 7.0; the entire section disassembles cleanly; the import table is
fully populated (16 DLLs, 334 symbols). CEG here is an *additive* transformation —
it appends check functions (the `0x9A2xxx` tables, right at the end of `.text`) and
replaces some direct calls with `call <deobfuscator>; jmp eax` thunks. It does not
relocate, compress, or re-lay-out the original Treyarch code.

Together: the original code keeps its build-time addresses, and the CEG additions
sit at fixed addresses too.

---

## 8. Corrections to the brief

**"thousands of surviving `C:\projects_pc\cod\codsrc\...\*.cpp` source-path and
assert strings" is wrong.** There are **145**:

```
$ strings -a -n 6 BlackOps.exe | grep -c 'codsrc'
145
$ strings -a -n 6 BlackOps.exe | grep -o 'codsrc[^ ]*' | sort -u | wc -l
145
```

and nearly all of them are DemonWare or crypto-library paths (`bdNet`, `bdSocket`,
`bdGetHostByName`, `universal/physicalmemory.cpp`, `universal/com_memory.cpp`).
Treyarch's own asserts are compiled out of the shipping build. **There is no
function-to-source attribution available for renderer code**, so "corroborate via a
nearby assert or source-path string" is not a usable method on this binary.
No conclusion in this document relies on it. The corroboration routes actually used
were: dvar/material name strings at call sites, argument-shape agreement with the
claimed signature, structure-offset agreement, call-graph structure, and the
independent cross-check in §6.

The PDB path `C:\projects_pc\cod\codsrc\src\obj\t5\CoDSteam_CEG_bin\BlackOps.pdb`
is worth noting on its own: the shipping build's own object directory is named
`CoDSteam_CEG_bin`, i.e. CEG was part of Treyarch's build, not a post-hoc rewrite.

---

## 9. Confidence statement, and what not to trust

**The core finding — that published addresses land on our binary with zero
systematic offset — is as close to certain as static analysis gets.** It rests on
43 independent tests all passing at δ = 0 and nothing else scoring above 14/43;
19 exact interior instruction-length matches; 25 pairs of independently published
constants confirming each other through call displacements; 80/80 CEG functions
landing on entries with category-matching prologues; and a byte-exact agreement
with a symbol set we derived ourselves from an unrelated starting point (§6).
It would take a conspiracy of coincidences to fake this. **Build on it.**

Everything below is weaker and is flagged so it does not get rounded up:

1. **`R_Set3D` @ `0x7244C0` is a thunk, not a function body.** It is a real entry
   with zero callers, containing `call 0x4ED550; jmp 0x6D4350`. The name is
   unconfirmed. Commented out upstream. Re-derive before using.
2. **Six symbol identities are prologue-and-shape only**, with no semantic
   corroboration: `Dvar_HasLatchedValue` `0x61BE00`, `Dvar_MakeLatchedCurrent`
   `0x61CFC0`, `Vec2UnpackTexCoords` `0x5AC8E0`,
   `R_AddCellSurfacesAndCullGroupsInFrustumDelayed` `0x6B32C0`, `R_AddDebugString`
   `0x6FCC50`, `Cbuf_AddText` `0x49B930`. The *address is a real function* in every
   case; the *name* is taken on trust. Verify by behaviour before depending on any
   of them.
3. **The embedded `GfxViewParms` inside the `GfxCmdBufSourceState` union
   (offset `+0x1660`, absolute `0x40CBBD0`) is unverified** — see §5.2. Always
   dereference the `viewParms3D` pointer at `0x40CBF78`; never hardcode
   `0x40CBBD0`.
4. **`GfxViewInfo` beyond its first `0x184` bytes is unverified.** The published
   definition continues through a long chain of post-effect structures we did not
   check. `GfxCmdBufState` is unverified except `renderTargetId` at `+0x1128`.
5. **Anything commented out upstream should be assumed stale.** The pattern is
   perfectly consistent: 11 of 12 commented-out symbol addresses are not function
   starts at all, and the only two failing interior tests in the whole report
   (`0x74A315`, `0x74A31B`) belong to a hook that is disabled upstream.
6. **This is verified for our copy only** — MD5 `2b179a5741…`, SHA-256
   `cc9aee4fd1…`. If Steam ever re-issues the executable, re-run the sweep in §2
   before trusting any of it. The reproduction is cheap: the four test classes need
   only `pefile` and `capstone`.
7. **The "Valve would not sign per user" step in §7 is an inference, not a
   measurement.** It is the only such step in the report. The empirical results in
   §3.3 and §4 do not depend on it.

## 10. Licence note

`xoxor4d/t5-rtx` has no `LICENSE` file; the GitHub API confirms `"license": null`.
All rights are reserved by its author. It was used here strictly as a published
statement of measurable facts about a third-party binary — memory addresses and
structure offsets, every one of which is re-derivable from `BlackOps.exe` alone,
and many of which this document does re-derive. No source, no header, and no
verbatim text from that project has been copied into this repository, and none
should be. If we want these declarations in code, we write them ourselves from the
tables above.
