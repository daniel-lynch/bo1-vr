# BO1 client input path — usercmd / button state (task #46)

Target: `BlackOps.exe` (retail Steam, 32-bit PE, image base `0x400000`).
All addresses are **virtual addresses** (runtime, no ASLR relocation applied — the module is
non-ASLR in practice; verify `GetModuleHandle("BlackOps.exe") == 0x400000` at runtime before
using absolute addresses).

Derived **solely** from disassembly (objdump) and Ghidra decompilation of this binary.
No external decompilation project, PDB, or source dump was consulted.

Every claim below is tagged **[V] VERIFIED** (read directly out of this binary's code) or
**[I] INFERRED** (a naming/semantic guess that the code does not prove).

---

## 0. TL;DR for the mod

* **Best injection point: the two engine-provided "pending buttons" globals.**
  * `0x02911DE0` — OR'd into `usercmd->buttons`  (dword)
  * `0x02911DE4` — OR'd into `usercmd->buttons2` (dword)
  * `CL_FinishMove` (`0x00881540`) does `cmd->buttons |= *0x2911DE0; cmd->buttons2 |= *0x2911DE4;`
    then **zeroes both**. This runs at the very end of every command build, *after* every other
    button source, and *after* the code paths that mask ADS/sprint off. Writing these every frame
    is a pure-OR merge that cannot be stomped by the normal input code. **[V]**
* **Fire** = `buttons |= 0x80000000` **[V]**
* **ADS / aim** = `buttons |= 0x00100000` **[V]**
* **Melee** = `buttons |= 0x20000000` **[V]**
* **Use / interact** = `buttons |= 0x10000000` **[V]**
* **Reload** = `buttons |= 0x08000000` **[V]**
* **Sprint** = `buttons |= 0x40000000` **[V]**
* **Jump** = `buttons |= 0x00200000` **[V]**
* A **kbutton table does exist** and is directly writable — see §5.

---

## 1. Command-registration table (how the names were resolved)

`Cmd_AddCommand`-style registrar: **`0x00661400`**, `__cdecl (const char *name, void (*fn)(void), cmd_function_t *node)`.
Nodes are 0x18 bytes apart in `.data`. **[V]** (name string / fn / node triple is literal in the
push sequence)

The client input commands are all registered from one function whose body spans
`0x004BB4C0 – 0x004BBC80`. That function is the **client input init** (`CL_InitInput` equivalent). **[I]** (name)

Full list extracted (name → handler):

```
+moveup 0x87FF40   -moveup 0x87FFA0    +movedown 0x87FFC0  -movedown 0x87FFD0
+left 0x87FFE0     -left 0x87FFF0      +right 0x880000     -right 0x880010
+forward 0x880020  -forward 0x880030   +back 0x880040      -back 0x880050
+lookup 0x880060   -lookup 0x880070    +lookdown 0x880080  -lookdown 0x880090
+strafe 0x880110   -strafe 0x880120    +moveleft 0x8800A0  -moveleft 0x8800B0
+moveright 0x8800C0 -moveright 0x8800D0 +speed 0x8800E0    -speed 0x880100
+attack 0x880130   -attack 0x880240    +melee 0x880530     -melee 0x880540
+holdbreath 0x880290 -holdbreath 0x8802A0 +melee_breath 0x8802B0 -melee_breath 0x8802D0
+frag 0x8802F0     -frag 0x8803C0      +smoke 0x8803D0     -smoke 0x8803E0
+breath_sprint 0x8804F0 -breath_sprint 0x880510 +activate 0x880550 -activate 0x8805A0
+reload 0x880600   -reload 0x880650    +usereload 0x8806B0 -usereload 0x880700
+leanleft 0x880760 -leanleft 0x880770  +leanright 0x880780 -leanright 0x880790
+prone 0x8807A0    -prone 0x8807B0     +stance 0x8807C0    -stance 0x880850
+mlook 0x87FD50    -mlook 0x881030     toggleads 0x8808D0  leaveads 0x880920
+throw 0x880940    -throw 0x880950     +speed_throw 0x880A20 -speed_throw 0x880A50
+toggleads_throw 0x880960 -toggleads_throw 0x8809C0
+gas 0x8803F0      -gas 0x880400       +reverse 0x880410   -reverse 0x880420
+handbrake 0x880430 -handbrake 0x880440 +switchseat 0x880450 -switchseat 0x880460
+vehicleattack 0x880470 -vehicleattack 0x880480
+vehicleattacksecond 0x880490 -vehicleattacksecond 0x8804A0
+vehiclemoveup 0x8804B0 -vehiclemoveup 0x8804C0
+vehiclemovedown 0x8804D0 -vehiclemovedown 0x8804E0
lowerstance 0x880A70 raisestance 0x880AC0 togglecrouch 0x880B00 toggleprone 0x880B40
goprone 0x880BB0   gocrouch 0x880BE0   +gostand 0x880C10   -gostand 0x880C60
+specNext 0x880CA0 -specNext 0x880CB0  +specPrev 0x880CC0  -specPrev 0x880CD0
+toggleSpec 0x880CE0 -toggleSpec 0x880CF0 toggleView 0x880D00
+talk 0x880D20     -talk 0x880D30      +sprint 0x880C80    -sprint 0x880C90
```
**[V]**

---

## 2. `usercmd_t` — layout and lifetime

### 2.1 Size and ring buffer **[V]**

`CL_CreateNewCommands` (**`0x008823E0`**) — decompiled:

```c
if (8 < DAT_02910164) {                       // clientState gate
  DAT_02993850 = DAT_02993850 + 1;            // cmdNumber++
  uVar1 = DAT_02993850 & 0x7f;                // 128-entry ring
  puVar2 = CL_CreateCmd(param_1);             // 0x8822D0
  <copy 13 dwords to local>
  <copy 13 dwords to (&DAT_02991e50 + uVar1 * 0x34)>
}
```

* `usercmd_t` size = **0x34 (52) bytes** (13 dwords, and `memset(cmd,0,0x34)` in `CL_CreateCmd`).
* Command ring buffer = **`0x02991E50`, 128 entries × 0x34**.
* Command counter (`cl.cmdNumber`) = **`0x02993850`**.
* Consistency check: `0x02991E50 + 128*0x34 = 0x02993850` — the counter sits exactly at the end
  of the array. Strong confirmation of both size and count.

Callers of `CL_CreateNewCommands` are two tail-jump thunks: `0x0049F320` (runs when
`*0x2910164 == 0xA`) and `0x005E5380` (runs when `*0x2910164 >= 6 && != 0xA`). **[V]**

### 2.2 Field offsets **[V] unless marked**

| Off | Size | Content | Evidence |
|---|---|---|---|
| `+0x00` | int | server time (clamped to `*0x2910290 + 5000`) | `0x881583` |
| `+0x04` | uint | **buttons** | every `orl ...,(%edi)` with `edi = cmd+4` |
| `+0x08` | uint | **buttons2** | `orl ...,0x4(%edi)` / `0x8(%esi)` |
| `+0x0C` | uint | angles[0] — 16-bit packed | `0x881588` loop |
| `+0x10` | uint | angles[1] — 16-bit packed | same |
| `+0x14` | uint | angles[2] — 16-bit packed | same |
| `+0x18` | byte | weapon/index-ish (from `*0x2911D7C`; also overwritten by the override latch) **[I] name** | `0x881540` |
| `+0x19` | byte | from `*0x2911D80` (latch overrides with `*0x2911E3C`) **[I] name** | `0x881547..` |
| `+0x1A` | byte | from `*0x2911D84` **[I] name** | `0x88154A` |
| `+0x1B` | s8 | move axis A (right/strafe) **[I] which axis** | `0x882093`, `0x88111x` |
| `+0x1C` | s8 | move axis B (forward) **[I] which axis** | `0x882077`, `0x88112x` |
| `+0x1E` | s8 | look axis (pitch, gamepad) **[I]** | `0x8820B2` |
| `+0x1F` | s8 | look axis (yaw, gamepad) **[I]** | `0x8820E0` |
| `+0x20` | s16 | `*0x2911D88 * 182.04` (an extra packed angle) | `0x8815C6` |
| `+0x22` | s16 | `*0x2911D8C * 182.04` | `0x8815DA` |
| `+0x24` | float | written in the gamepad path | `0x8822B5` |
| `+0x28` | byte | written in the gamepad path | `0x8822BA` |
| `+0x29..0x33` | | not observed being written | — |

Angle packing (`CL_FinishMove`, `0x00881588`):

```c
pfVar3 = (float *)0x02911DA8;              // cl.viewangles[3]
puVar4 = &cmd->angles[0];                  // cmd+0x0C
do { *puVar4 = (int)((pfVar3[0x1e] + *pfVar3) * 182.0444f) & 0xffff; ... }
   while (pfVar3 < 0x2911DB4);
```
`pfVar3[0x1e]` = `*pfVar3 + 0x78` → `0x02911E20 / 0x02911E24 / 0x02911E28`.

**Important corroboration of the project's earlier negative result:**
`0x02911E20 / E24 / E28` are the per-frame **angle deltas**, added on top of the base angles.
The **absolute view angles are at `0x02911DA8 / 0x02911DAC / 0x02911DB0`**, written by
`0x00448BB0` (a `CL_SetViewAngles`-shaped setter, `(int localClient, const vec3_t angles, float, float, float)`)
which is called from cgame at `0x00563354`. That setter also writes `0x02911D88 / D8C / D90`. **[V]**
Angle index order (pitch/yaw/roll) is **[I]** — not proven here, but index 0 feeds the same slot
the mouse-pitch delta feeds, consistent with `angles[0] = pitch`.

---

## 3. `CL_CreateCmd` — `0x008822D0`

Signature (MSVC register-ish): `usercmd_t *cmd` in **EAX**, `int localClientNum` on the stack.
Returns `cmd` in EAX. **[V]** (from the call site at `0x00882404`)

Decompiled:

```c
void CL_CreateCmd(int localClient)   // cmd in EAX
{
  cmd->buttons  = 0;                              // +4
  cmd->buttons2 = 0;                              // +8
  if (FUN_005791d0()) FUN_004b3bd0(localClient);
  savedPitchDelta = DAT_02911e20;
  FUN_00880d40(localClient);
  memset(cmd, 0, 0x34);
  if ((!FUN_00509780(localClient,8)) || (!FUN_00881650(localClient))) {
      CL_CmdButtons (cmd /*EAX*/, localClient);   // 0x00881240
      FUN_00881070 (localClient /*EAX*/);         // stance + ADS + sprint  (cmd in ESI)
      FUN_00881930 (localClient);                 // mouse look (cmd in EDI)
      i = FUN_004f3c70(localClient);
      if (i == -1 || !FUN_005f8d90(i)) FUN_006036b0(localClient, &cmd->buttons);
      else                             FUN_00881f10(localClient);   // gamepad path
      <clamp DAT_02911e20 drift vs savedPitchDelta>
  }
  CL_FinishMove(cmd /*ECX, __fastcall*/);         // 0x00881540
}
```
**[V]**. Function names `CL_CreateCmd` / `CL_CmdButtons` / `CL_FinishMove` are **[I]** (idTech-family
naming applied to verified behaviour).

Note the early-out: if `FUN_00509780(lc,8) && FUN_00881650(lc)` the whole button build is skipped —
**but `CL_FinishMove` still runs**, so the `0x2911DE0/DE4` injection channel still applies. **[V]**

---

## 4. `CL_FinishMove` — `0x00881540` — THE INJECTION POINT

```c
void __fastcall CL_FinishMove(int *cmd)     // ECX = cmd
{
  *(byte*)(cmd+6)        = DAT_02911d7c;     // cmd+0x18
  *(byte*)((int)cmd+0x1a)= DAT_02911d84;
  *(byte*)((int)cmd+0x19)= DAT_02911d80;
  cmd[0] = clamp(DAT_02911ce4, DAT_02910290+5000);
  <pack 3 angles from viewangles + deltas>
  *(short*)(cmd+8)         = (short)(DAT_02911d88 * 182.0444f);
  *(short*)((int)cmd+0x22) = (short)(DAT_02911d8c * 182.0444f);

  cmd[1] |= _DAT_02911de0;      /* <<<<<< cmd->buttons  |= pendingButtons  */
  cmd[2] |= _DAT_02911de4;      /* <<<<<< cmd->buttons2 |= pendingButtons2 */
  _DAT_02911de0 = 0;
  _DAT_02911de4 = 0;

  if (_DAT_02911e2c != 0) {     /* full-override latch */
    cmd[1] = _DAT_02911e30;     /* buttons  = ... (assignment, not OR) */
    cmd[2] = _DAT_02911e34;     /* buttons2 = ... */
    *(byte*)(cmd+6)         = DAT_02911e38;
    *(int*)(DAT_02ff5354 + 0xa9be8) = _DAT_02911e38;
    *(byte*)((int)cmd+0x19) = DAT_02911e3c;
    _DAT_02911e2c = 0;
  }
}
```
**[V]** (Ghidra output, matches the raw disassembly instruction-for-instruction.)

### 4.1 Channel A — pending-buttons OR (RECOMMENDED)

* `uint32 *pendingButtons  = (uint32*)0x02911DE0;`
* `uint32 *pendingButtons2 = (uint32*)0x02911DE4;`
* Set every frame; the engine ORs them into the next command and clears them. **[V]**
* The engine's own writer is `FUN_00536E70(int localClient, uint *pair)`:
  `*0x2911DE0 |= pair[0]; *0x2911DE4 |= pair[1];` — called from cgame at `0x00563361` with
  `pair = cgameGlob(0x02FF5354) + 0xCC820`, which cgame then zeroes. So this is the engine's
  *intended* "cgame asks for a button press" channel. **[V]**
* Caveats:
  * Only cleared when a command is actually built. If the client builds zero commands for a
    frame (paused, loading), the value persists and applies on the next command. Set it fresh
    each frame rather than accumulating.
  * `CL_CreateNewCommands` is gated on `*0x2910164 > 8`; `CL_CreateCmd`/`CL_FinishMove` are not
    reached at all outside that state, so no leakage into menus. **[V]**

### 4.2 Channel B — full override latch

`FUN_00453880(int localClient, uint buttonsPair[2], u32 a, u32 b)` sets
`0x2911E2C=1, 0x2911E30=pair[0], 0x2911E34=pair[1], 0x2911E38=a, 0x2911E3C=b`,
gated on `((&DAT_0291015C)[localClient*4] >> 1) & 1`. **[V]**
This **replaces** `buttons`/`buttons2` wholesale — worse for a mod that wants to merge with real
input. Only use if you want total control of the button word.

### 4.3 Channel C — write the kbutton table (see §5)

---

## 5. The kbutton table (the "queryable table" question — YES, it exists)

### 5.1 Location and stride **[V]**

* Array base: **`0x028D9088`**
* Per-local-client stride: **`0x3F0`** (`imul $0x3f0, localClient` appears in every accessor:
  `0x4CBE8E`, `0x5D9414`, `0x68B348`, `0x88124C`, `0x8820F8`)
* Per-entry stride: **`0x18` (24 bytes)** (`lea (ecx,ecx,2),ecx; ...(%eax,%ecx,8)` = `24*index`)
* Address of entry `k` for local client `c`:
  `0x028D9088 + c*0x3F0 + k*0x18`
* 42 entries per client (`0x3F0 / 0x18`); indices 0..41 observed.

### 5.2 `kbutton_t` layout (24 bytes) **[V]** from `IN_KeyDown 0x0087FD60` / `IN_KeyUp 0x0087FE30`

| Off | Type | Meaning |
|---|---|---|
| `+0x00` | int | `down[0]` — key id holding this button (0 = none) |
| `+0x04` | int | `down[1]` — second key id |
| `+0x08` | int | `downtime` (ms) |
| `+0x0C` | int | `msec` — accumulated held time |
| `+0x10` | u8  | **`active` / held** — this is the flag every reader tests |
| `+0x11` | u8  | **`wasPressed`** — set on key-down, **cleared by `CL_CmdButtons` after it is read** |
| `+0x14` | float | analog value (from the optional 4th argument of the `+cmd`) |

`IN_KeyDown` prints `"Three keys down for a button!\n"` when both key slots are occupied — a literal
string in this binary, which is what confirms the two-key `down[]` array. **[V]**

Helper accessors:
* `FUN_0068B340(int localClient, int kbuttonIndex) -> bool` = `down[0]!=0 || wasPressed!=0` **[V]**
* `FUN_004CBE80(int localClient, uint *buttonsWord, int kbuttonIndex, uint bitIndex, int bitCount)`
  — the analog-button packer; also clears `wasPressed`. **[V]**
* `FUN_005D9410(int localClient)` = `memset(&kbuttons[localClient], 0, 0x3F0)` — clear all keys.
  Called from `0x498CF0`, `0x498D67`, `0x5B0E41`. **[V]**

### 5.3 Bit-index convention **[V]**

`FUN_004CBE80` computes the mask as `0x80000000u >> (bitIndex & 0x1f)`, i.e. **bit indices are
numbered from the MSB**. Bit 0 = `0x80000000`, bit 31 = `0x00000001`. The whole button word makes
sense read this way (see §6).

### 5.4 kbutton index → command

| k | address (client 0) | `down` byte | commands |
|---|---|---|---|
| 0 | 0x028D9088 | 0x028D9098 | +left / -left |
| 1 | 0x028D90A0 | 0x028D90B0 | +right |
| 2 | 0x028D90B8 | 0x028D90C8 | +forward |
| 3 | 0x028D90D0 | 0x028D90E0 | +back |
| 4 | 0x028D90E8 | 0x028D90F8 | +lookup |
| 5 | 0x028D9100 | 0x028D9110 | +lookdown |
| 6 | 0x028D9118 | 0x028D9128 | +moveleft |
| 7 | 0x028D9130 | 0x028D9140 | +moveright |
| 8 | 0x028D9148 | 0x028D9158 | +strafe |
| **9** | **0x028D9160** | **0x028D9170** | **+speed, +speed_throw — the ADS hold key** |
| **10** | **0x028D9178** | **0x028D9188** | **JUMP** (set by +moveup / +gostand when stance == stand) |
| 11 | 0x028D9190 | 0x028D91A0 | +movedown (crouch key) |
| 12 | 0x028D91A8 | 0x028D91B8 | +moveup / +gostand (raw) |
| 14 | 0x028D91D8 | 0x028D91E8 | +toggleSpec |
| 15 | 0x028D91F0 | 0x028D9200 | +specNext |
| 16 | 0x028D9208 | 0x028D9218 | +specPrev |
| **17** | **0x028D9220** | **0x028D9230** | **+attack** |
| 18 | 0x028D9238 | 0x028D9248 | +holdbreath, +breath_sprint, +melee_breath |
| 19 | 0x028D9250 | 0x028D9260 | +frag |
| 20 | 0x028D9268 | 0x028D9278 | +smoke |
| **21** | **0x028D9280** | **0x028D9290** | **+melee, +melee_breath** |
| **22** | **0x028D9298** | **0x028D92A8** | **+activate (use/interact)** |
| **23** | **0x028D92B0** | **0x028D92C0** | **+reload** |
| 24 | 0x028D92C8 | 0x028D92D8 | +usereload |
| 25 | 0x028D92E0 | 0x028D92F0 | +leanleft |
| 26 | 0x028D92F8 | 0x028D9308 | +leanright |
| 27 | 0x028D9310 | 0x028D9320 | +prone |
| 28 | 0x028D9328 | 0x028D9338 | *(no writer in the binary — dead entry, maps to the CROUCH bit)* |
| 29 | 0x028D9340 | 0x028D9350 | +throw, +toggleads_throw, +speed_throw |
| **30** | **0x028D9358** | **0x028D9368** | **+sprint, +breath_sprint** |
| 32 | 0x028D9388 | 0x028D9398 | +gas (analog) |
| 33 | 0x028D93A0 | 0x028D93B0 | +reverse |
| 34 | 0x028D93B8 | 0x028D93C8 | +handbrake |
| 35 | 0x028D93D0 | 0x028D93E0 | +switchseat |
| 36 | 0x028D93E8 | 0x028D93F8 | +vehicleattack |
| 37 | 0x028D9400 | 0x028D9410 | +vehicleattacksecond |
| 38 | 0x028D9418 | 0x028D9428 | +vehiclemoveup |
| 39 | 0x028D9430 | 0x028D9440 | +vehiclemovedown |
| 40 | 0x028D9448 | 0x028D9458 | set by **+attack double-click** (`0x8801C2`), released by -attack |
| 41 | 0x028D9460 | 0x028D9470 | +talk |

Indices 13, 31 have no observed users. **[V]** for every row above (each derived from the literal
`mov $0x28d9XXX,%esi; call IN_KeyDown` in the named handler).

`down` byte column = entry base + 0x10. `wasPressed` = entry base + 0x11.

---

## 6. Button bit layout

### 6.1 `usercmd->buttons` (+0x04)

Sources: `CL_CmdButtons` (`0x00881240`), `FUN_00881070` (stance/ADS/sprint), `FUN_00881F10` (gamepad).

| Mask | MSB bit# | Meaning | Source | Conf |
|---|---|---|---|---|
| `0x80000000` | 0 | **ATTACK / fire** | kbutton 17 (+attack); also set by gamepad trigger `>= dvar(0x28D9064)` | **[V]** |
| `0x40000000` | 1 | **SPRINT** | kbutton 30 (+sprint / +breath_sprint), gated on `!kbutton[3].down` (can't sprint while pressing +back) | **[V]** |
| `0x20000000` | 2 | **MELEE** | kbutton 21 (+melee) | **[V]** |
| `0x10000000` | 3 | **USE / activate** | kbutton 22 (+activate) | **[V]** |
| `0x08000000` | 4 | **RELOAD** | kbutton 23 (+reload) | **[V]** |
| `0x04000000` | 5 | usereload (context "use or reload") | kbutton 24 | **[V]** |
| `0x02000000` | 6 | lean left | kbutton 25 | **[V]** |
| `0x01000000` | 7 | lean right | kbutton 26 | **[V]** |
| `0x00800000` | 8 | **PRONE** | kbutton 27 (+prone) and `stance == 2/3` in `FUN_881070` | **[V]** |
| `0x00400000` | 9 | **CROUCH** | kbutton 11 (+movedown) and `stance == 1` in `FUN_881070`; also dead kbutton 28 | **[V]** |
| `0x00200000` | 10 | **JUMP / move up** | kbutton 10; also kbutton 12 when `*0x29102A8 ∈ {2,3,4}` | **[V]** |
| `0x00100000` | 11 | **ADS (aim down sights)** | `kbutton[9].down XOR (adsToggle *0x2910280)`; set in both `FUN_881070` and `FUN_881F10` | **[V]** |
| `0x00080000` | 12 | "stance key held / stance changed" — set whenever an explicit stance key is down, cleared otherwise | `FUN_881070` | **[I]** name |
| `0x00040000` | 13 | **HOLD BREATH** | kbutton 18 (+holdbreath / +breath_sprint) | **[V]** |
| `0x00020000` | 14 | **FRAG grenade** | kbutton 19 (+frag) | **[V]** |
| `0x00010000` | 15 | **SMOKE / special grenade** | kbutton 20 (+smoke) | **[V]** |
| `0x00001000`\|`0x00000800`\|`0x00000400` | 19–21 | vehicle **gas**, 3-bit analog value | `FUN_004CBE80(lc,&buttons,32,0x13,3)` | **[V]** |
| `0x00000200` | 22 | vehicle reverse | kbutton 33 | **[V]** |
| `0x00000100` | 23 | vehicle handbrake | kbutton 34 | **[V]** |
| `0x00000080` | 24 | **THROW / offhand** (grenade throw button) | kbutton 29 (+throw / +toggleads_throw / +speed_throw) | **[V]** for the wiring, **[I]** for the "offhand" name |
| `0x00000008` | 28 | switch seat | kbutton 35 | **[V]** |
| `0x00000004` | 29 | set when `*0x2910160 != 0 && cl_bypassMouseInput(0x28D9084)->current == 0 && FUN_004369D0(lc) != 0xD` | `0x8814D7` | **[V]** wiring, meaning **[I]** — plausibly "mouse/look input is live" |

Masks not observed being written: `0x00008000..0x00002000`, `0x00000040..0x00000010`, `0x00000002`, `0x00000001`.

### 6.2 `usercmd->buttons2` (+0x08)

| Mask | MSB bit# | Meaning | Source | Conf |
|---|---|---|---|---|
| `0x20000000` | 2 | vehicle attack | kbutton 36 | **[V]** |
| `0x10000000` | 3 | vehicle attack (secondary) | kbutton 37 | **[V]** |
| `0x08000000` | 4 | vehicle move up | kbutton 38 | **[V]** |
| `0x04000000` | 5 | vehicle move down | kbutton 39 | **[V]** |
| `0x02000000` | 6 | attack **double-click** | kbutton 40, set by `+attack` when two presses land inside the window computed at `0x880160–0x8801C0` | **[V]** wiring, **[I]** meaning |
| `0x00080000` | 12 | set when stance state `*0x2911D70 == 3` | `FUN_881070` | **[V]** wiring, **[I]** meaning |
| `0x00002000` | 18 | set when the +activate/+reload/+usereload double-tap counter `*0x02B10B58 == 2`; counter then reset | `FUN_881070` | **[V]** wiring, **[I]** meaning |

The gamepad path additionally does `buttons |= 0x80000000; buttons2 |= 0x30000000;` when the stick
magnitude exceeds a threshold while in a vehicle (`0x88225F`). **[V]**

---

## 7. ADS specifics (task explicitly asked)

* ADS is **not** a raw kbutton→bit mapping. It is computed:

  ```c
  /* FUN_00881070 @ 0x8810E4, and identically FUN_00881F10 @ 0x882105 */
  if ((bool)kbuttons[lc][9].down == (adsToggle == 0))
        cmd->buttons |=  0x00100000;
  else  cmd->buttons &= ~0x00100000;      /* only FUN_881070 clears */
  ```
  i.e. **ADS = held(+speed) XOR adsToggle**. **[V]**
* `adsToggle` = **`0x02910280`** (a byte).
  * `toggleads` (`0x8808D0`) and `+toggleads_throw` (`0x880960`) flip it.
  * `leaveads` (`0x880920`), `+speed` (`0x8800E0`) and `+speed_throw` (`0x880A20`) clear it.
  * A debounce byte at `0x028D9489` and a float at `0x028D9354` guard the toggle. **[V]**
* The string `"adsbuttonpressed"` exists at VA `0x00A1C8D0`, but **no code in `.text` takes its
  address**. It is therefore almost certainly a **script/notify string** consumed by GSC, not a C
  identifier. It does not name the mechanism above. **[V]** (exhaustive scan for the constant
  `0x00A1C8D0` in the disassembly returned zero hits).
* Because `FUN_881070` *clears* `0x00100000` when the condition is false, you **cannot** force ADS by
  writing the kbutton alone if a later path re-evaluates it — but you can, because `FUN_881070` runs
  before `CL_FinishMove`. Setting kbutton 9's `down` byte to 1 does work (`FUN_881070` reads it). The
  `0x2911DE0` channel works too and is immune to the clear.

---

## 8. Recommended mod implementation

### 8.1 Primary: pending-buttons OR (per frame, before the client builds its command)

```c
#define BO1_PENDING_BUTTONS   ((volatile uint32_t*)0x02911DE0)
#define BO1_PENDING_BUTTONS2  ((volatile uint32_t*)0x02911DE4)

#define BTN_ATTACK     0x80000000u
#define BTN_SPRINT     0x40000000u
#define BTN_MELEE      0x20000000u
#define BTN_USE        0x10000000u
#define BTN_RELOAD     0x08000000u
#define BTN_USERELOAD  0x04000000u
#define BTN_PRONE      0x00800000u
#define BTN_CROUCH     0x00400000u
#define BTN_JUMP       0x00200000u
#define BTN_ADS        0x00100000u
#define BTN_HOLDBREATH 0x00040000u
#define BTN_FRAG       0x00020000u
#define BTN_SMOKE      0x00010000u
#define BTN_THROW      0x00000080u

/* call once per VR frame */
*BO1_PENDING_BUTTONS |= vrButtons;   /* engine ORs into cmd->buttons then zeroes */
```

Why this is the cleanest:
* Pure OR — never fights the keyboard/mouse/gamepad code. **[V]**
* Applied last in `CL_FinishMove`, after the paths that mask ADS/sprint off. **[V]**
* Self-clearing, so a crashed/paused mod cannot latch a button on forever (beyond one command). **[V]**
* No code patching, no hook, no trampoline — a plain memory write. Works from an injected DLL
  thread or from a per-frame hook you already own.

### 8.2 Secondary / when you need "held" semantics that other code can *see*

Write the kbutton `down` byte. Other engine and cgame code queries kbuttons (`FUN_0068B340`,
`FUN_004CBE80`) — e.g. the ADS XOR and the sprint gate read `down` directly, so ADS/sprint
behaviour that depends on *state* rather than the raw bit will only be consistent if you drive the
kbutton.

```c
/* held: */    *(uint8_t*)(0x028D9088 + lc*0x3F0 + k*0x18 + 0x10) = 1;
/* one-shot: */*(uint8_t*)(0x028D9088 + lc*0x3F0 + k*0x18 + 0x11) = 1;  /* auto-cleared */
```
Do **not** touch `+0x00`/`+0x04` (key ids) — leave those to `IN_KeyDown`/`IN_KeyUp`, otherwise a real
key release can zero your state or the engine will log `"Three keys down for a button!"`.

For fire/reload/use/melee/jump/sprint, §8.1 is enough. For **ADS**, driving kbutton 9's `down` byte
is arguably more faithful (it feeds the XOR and therefore the cgame-visible ADS state) — **[I]**,
not verified against runtime behaviour.

### 8.3 Hook point if you want a code hook instead of a poll

Hook the head of `CL_FinishMove` (`0x00881540`, `__fastcall`, `ECX = usercmd_t*`) or the tail of
`CL_CreateCmd` (`0x008822D0`). Either gives you the exact `usercmd_t` about to be queued into
`0x02991E50`. **[V]** on signature and role; hooking either has not been tested.

---

## 9. Open / unresolved

* **Not established:** which of `cmd+0x1B/0x1C/0x1E/0x1F` is forwardmove vs rightmove vs the two
  gamepad look axes. The gamepad path (`FUN_881F10`) and the keyboard path (`FUN_881070`) both write
  `+0x1B` and `+0x1C`, so those two are the movement axes and `+0x1E/+0x1F` are gamepad-look-only —
  but the exact forward/right assignment is a guess.
* **Not established:** the meaning of `buttons` bit `0x00000004` and `buttons2` bits `0x00080000`
  and `0x00002000` beyond the code that sets them.
* **Not established:** angle index → pitch/yaw/roll ordering for `0x02911DA8..DB0`.
* **Not established:** what kbutton indices 13 and 31 are for (no writers found).
* **Not established:** whether writing `0x2911DE0` from another thread races the client thread.
  It is a non-atomic `or` in `CL_FinishMove`; a torn interleave would at worst drop one frame's
  press. Prefer writing from the game thread if you already have a per-frame hook.
* **Not verified at runtime.** Everything here is static analysis. Before shipping, log
  `*(uint32_t*)(cmd+4)` from a `CL_FinishMove` hook while pressing each key and confirm the mask
  table — this project has been burnt by confident static naming before, and §6 rows marked **[I]**
  are exactly where that risk lives.

---

## 10. Address quick-reference

| Address | What | Conf |
|---|---|---|
| `0x00661400` | Cmd_AddCommand(name, fn, node) | **[V]** |
| `0x004BB4C0` | client input command registration (CL_InitInput) | **[V]** span, **[I]** name |
| `0x0087FD60` | IN_KeyDown (kbutton in ESI) | **[V]** |
| `0x0087FE30` | IN_KeyUp (kbutton in ESI) | **[V]** |
| `0x0068B340` | kbutton is-down query (localClient, index) | **[V]** |
| `0x004CBE80` | analog kbutton → bitfield packer | **[V]** |
| `0x005D9410` | clear all kbuttons for a local client | **[V]** |
| `0x00881240` | CL_CmdButtons (cmd in EAX, localClient on stack) | **[V]** |
| `0x00881070` | stance + ADS + sprint + move axes (localClient in EAX, cmd in ESI) | **[V]** |
| `0x00881930` | mouse look (already known to the project) | **[V]** |
| `0x00881F10` | gamepad command build | **[V]** |
| `0x00881540` | **CL_FinishMove — injection point** (`__fastcall`, cmd in ECX) | **[V]** |
| `0x008822D0` | CL_CreateCmd (cmd in EAX, localClient on stack) | **[V]** |
| `0x008823E0` | CL_CreateNewCommands | **[V]** |
| `0x00536E70` | cgame → pending-buttons OR helper | **[V]** |
| `0x00453880` | cgame → full button override latch | **[V]** |
| `0x00448BB0` | CL_SetViewAngles (writes 0x2911DA8/DAC/DB0) | **[V]** |
| `0x028D9088` | kbutton array base (stride 0x3F0/client, 0x18/entry) | **[V]** |
| `0x02911DE0` | **pendingButtons (OR'd into cmd->buttons)** | **[V]** |
| `0x02911DE4` | **pendingButtons2 (OR'd into cmd->buttons2)** | **[V]** |
| `0x02911E2C` | override-latch flag | **[V]** |
| `0x02911E30/E34/E38/E3C` | override-latch payload | **[V]** |
| `0x02911DA8/DAC/DB0` | **cl.viewangles[3] (absolute)** | **[V]** |
| `0x02911E20/E24/E28` | per-frame angle deltas (NOT absolute angles) | **[V]** |
| `0x02910280` | ADS toggle latch (byte) | **[V]** |
| `0x02911D70` | stance state (0 stand / 1 crouch / 2 prone / 3 ?) | **[V]** |
| `0x02991E50` | usercmd ring buffer, 128 × 0x34 | **[V]** |
| `0x02993850` | cmdNumber | **[V]** |
| `0x02910164` | client connection state (gates ≥6, ≥9, ==0xA) | **[V]** wiring, **[I]** name |
