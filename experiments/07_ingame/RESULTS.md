# Experiment 7 — the ASI loader inside the live `BlackOps.exe`

**Question.** Can the repository's `dinput8.dll` ASI loader come up inside the
*real* Call of Duty: Black Ops under Proton, with CEG and the documented
anti-debug check handled, and can a debugger attach and hit a breakpoint in our
own code inside that process?

**Answer: yes — and CEG and the anti-debug check needed no patching whatsoever.**
The expensive part of this experiment turned out not to be CEG at all. It was
that **`BlackOps.exe` does not import `dinput8.dll`**, so the loader was never
being loaded in the first place.

Reproduce with `./run.sh`. Nothing in the Steam install is written to.

---

## Verdict

| Link in the chain | State |
|---|---|
| `BlackOps.exe` imports `dinput8.dll` | **FALSE** — it does not, and never loads it at runtime either |
| `winmm.dll` shim → `dist/dinput8.dll` → `bo1probe.asi` | **PASS** — all three in the live game |
| Game reaches renderer init, `Server: frontend`, frontend zone load | **PASS** |
| **CEG needed patching** | **NO** — nothing patched, game runs |
| `.text` in memory identical to `.text` on disk | **PASS** — 0 differing bytes in 0x5A1A10 |
| **Anti-debug at `0x4C06E0` needed patching** | **NO** — it is not an anti-debug check |
| Game keeps running with `PEB->BeingDebugged = 1` | **PASS** |
| Published addresses valid in the live process | **PASS** — 6/6 byte anchors |
| `Dvar_FindVar` called for real, values verified | **PASS** — 12/12, cross-checked three ways |
| `Dvar_Hash` reimplementation matches the game's own | **PASS** — 3/3 |
| Live `IDirect3DDevice9` located through the address map | **PASS** — `dx->device` non-NULL |
| `winedbg --gdb` breakpoint in our own code, new-WoW64 | **PASS** — hit, with source, args and backtrace |
| Game reaches the main menu | **NO** — exits during frontend zone load, **and so does the unmodified game** (§8) |

---

## 1. The finding that mattered: `dinput8.dll` is never loaded

README Decision 2 says naming the loader `dinput8.dll` makes installation "copy
the file", because Proton lists that name prefer-native. That is true about
Proton and **false about this game**. `BlackOps.exe`'s import directory, read
with `pefile`, is:

```
steam_api.dll  WINMM.dll  WSOCK32.dll  binkw32.dll  d3d9.dll  d3dx9_43.dll
DSOUND.dll  KERNEL32.dll  USER32.dll  GDI32.dll  ADVAPI32.dll  SHELL32.dll
ole32.dll  XINPUT1_3.dll  PSAPI.DLL  WS2_32.dll
```

No `dinput8`. The delay-import directory is empty, and the only DLL-name strings
anywhere in the 8 MB file are `DBGHELP.DLL`, `nvapi.dll`, `ddraw.dll` and the
PunkBuster set — so there is no runtime `LoadLibrary("dinput8.dll")` either:

```sh
$ strings -a BlackOps.exe | grep -ioE '^[a-z0-9_]+\.dll$' | sort -u
ADVAPI32.dll  binkw32.dll  d3d9.dll  d3dx9_43.dll  DBGHELP.DLL  ddraw.dll
DSOUND.dll  gdi32.dll  kernel32.dll  NTDLL.DLL  nvapi.dll  ole32.dll
pbag.dll  pbcl.dll  pbclnew.dll  pbclold.dll  pbsv.dll  pbsvnew.dll  pbsvold.dll
PSAPI.DLL  SHELL32.dll  steam_api.dll  user32.dll  USER32.dll  WINMM.dll
WS2_32.dll  WSOCK32.dll  XINPUT1_3.dll
```

**A `dinput8.dll` next to `BlackOps.exe` is simply never opened.** Every earlier
experiment used a stand-in host that called `LoadLibraryA("./dinput8.dll")`
explicitly, which is exactly the step the real game does not perform.

### What was done about it

`winmm_shim.c`, a ~40-line `winmm.dll` that forwards the eleven exports the game
imports and `LoadLibrary`s the repository's **unmodified** `dist/dinput8.dll`.
Everything the README says about the loader still holds; only the way it gets
loaded changed.

Why winmm and not something else:

* `XINPUT1_3.dll` (ordinals 2, 3, 4) and `DSOUND.dll` (ordinals 11, 6) are
  imported **by ordinal**, so a proxy must pin ordinals as well as names.
* `d3d9.dll` is only three exports and is tempting, but it puts us in the
  rendering path before anything is proven and it shadows the module DXVK/WineD3D
  provides. That is a BAC-281 decision, not a bootstrap decision.
* `binkw32.dll` and `steam_api.dll` live in the game install, which we do not touch.
* `WINMM.dll` is imported **by name**, all eleven, is a static import so it loads
  before the entry point, and does nothing that can break rendering.

Each export is a one-instruction indirect tail jump emitted as inline asm rather
than a hand-transcribed prototype — arguments and the callee's own `ret N` are
untouched, which makes the thunk arity-proof. Getting a `__stdcall` arity wrong
is an immediate stack-corrupting crash, and eleven `mmsystem.h` prototypes is
eleven chances to get one wrong.

**It needs `WINEDLLOVERRIDES="winmm=n,b"`.** Exp. 0 Finding 2: a Wine builtin
silently beats a native DLL in the application directory unless the name is
prefer-native. `dinput8` is on Proton's list; `winmm` is not. And it must be
`n,b`, never bare `n`, or the shim's own `LoadLibrary` of the real winmm fails
with error 126 (Exp. 0 Finding 1).

---

## 2. It is in

`out/console.txt`, via `WINEDEBUG=+debugstr` (see §7 for why that channel):

```
0150:warn:debugstr:OutputDebugStringA "[winmm-shim] attach, pid=332\n"
0150:warn:debugstr:OutputDebugStringA "[winmm-shim] real winmm at 7B9D0000, 11/11 exports resolved\n"
0150:warn:debugstr:OutputDebugStringA "[bo1-vr] =====================================================\n"
0150:warn:debugstr:OutputDebugStringA "[bo1-vr] bo1-vr ASI loader (dinput8.dll proxy)\n"
0150:warn:debugstr:OutputDebugStringA "[bo1-vr]   built Aug  1 2026 16:40:47 with GCC 13-win32\n"
0150:warn:debugstr:OutputDebugStringA "[bo1-vr]   module: Z:\\tmp\\bo1vr-exp07\\game\\dinput8.dll\n"
0150:warn:debugstr:OutputDebugStringA "[bo1-vr]   pid=332\n"
0150:warn:debugstr:OutputDebugStringA "[bo1-vr] VEH: handler installed\n"
0150:warn:debugstr:OutputDebugStringA "[bo1-vr] MinHook: initialised (MH_OK)\n"
0150:warn:debugstr:OutputDebugStringA "[bo1-vr] asi: scanning Z:\\tmp\\bo1vr-exp07\\game\\*.asi\n"
0150:warn:debugstr:OutputDebugStringA "[bo1-vr] asi: loaded bo1probe.asi at 7B240000\n"
0150:warn:debugstr:OutputDebugStringA "[bo1-vr] asi: 1 plugin(s) loaded\n"
0150:warn:debugstr:OutputDebugStringA "[bo1-vr] loader: init complete\n"
```

and from inside the process, `out/bo1probe.log`:

```
--- 1. identity
  pid                 = 332 (0x14c)
  exe                 = Z:\tmp\bo1vr-exp07\game\BlackOps.exe
  GetModuleHandle(0)  = 00400000   (expected 00400000, no ASLR)
  this .asi           = Z:\tmp\bo1vr-exp07\game\bo1probe.asi
  dinput8.dll module  = 79C20000
  winmm.dll module    = 7AC70000
  d3d9.dll module     = 7A770000
  steam_api.dll       = 3B400000
  .text protection    = 0x20 (initial 0x80), state 0x1000, size 0x5a2000
```

`MinHook: initialised (MH_OK)` is worth noting on its own — the vendored MinGW
build initialises inside the real game, not just in a toy host.

The game itself runs. From its own logging, in order: asset loading,
`------ Server Initialization ------`, `Server: frontend`, `----- R_Init -----`,
`DirectX reports 4096 MB of video memory`, then `Adding fastfile 'frontend' to
queue` and `Loading fastfile 'en_frontend'`. Its own `version` dvar, read out of
the live process (§5), is

```
Call of Duty Singleplayer - Ship COD_T5_S SP build 7.0.152 CL(966072)
CODPCAB-V64 CEG Thu Sep 01 16:06:17 2011 win-x86
```

---

## 3. CEG: nothing needed patching

**This is the headline for the scene's Windows-oriented advice: on Proton it
does not apply.** Nukem9/LinkerMod ships a `Patch_CEG()` for the native Windows
case. Nothing equivalent was written here, nothing was patched, and the game
runs. Three independent observations:

### 3.1 The game runs unpatched

No byte of `BlackOps.exe` was modified — the mirror copy is verified
byte-identical with `cmp` before every launch — and no CEG function was hooked,
stubbed or NOP'd. The process reaches renderer init and frontend zone loading.

### 3.2 `.text` in memory is byte-identical to `.text` on disk

The probe reads `BlackOps.exe` back off disk from inside the running process,
parses its own PE headers, and compares each section against the mapped image:

```
--- 2. in-memory image vs BlackOps.exe on disk
  read 8101944 bytes from Z:\tmp\bo1vr-exp07\game\BlackOps.exe
  file ImageBase=0x400000  SizeOfImage=0x4277000  sections=7  DllCharacteristics=0x8000
  IAT (the positive control) = 0x9a3000 .. 0x9a3578
  .text   VA 0x401000 len 0x5a1a10 : 0 differing byte(s) in 0 range(s)
  .rdata  VA 0x9a3000 len 0x1c86ba : 1331 differing byte(s) in 582 range(s)
            of which 1331 are inside the IAT (expected: the loader wrote them)
            0x009a3000 len 48  disk[a4 b6 76 00 8c b6 76 00 7a b6 76 00]  mem[c0 fa b7 7b 60 c1 b7 7b 40 ee b7 7b]
```

**Zero differing bytes across all 0x5A1A10 bytes of `.text`.** CEG does not
decrypt, unpack, relocate or hot-patch code at runtime. It is exactly the
additive, static transformation `docs/address-map.md` §7 inferred from the file
alone — now confirmed dynamically.

The `.rdata` result is the positive control that makes the `.text` result mean
something. 1331 bytes differ, **every one of them inside the IAT**, which the
Windows loader must have written. A comparison method that reported "no
differences" everywhere would be broken; this one demonstrably detects real
runtime writes and finds none in `.text`.

### 3.3 The CEG stub tables are present and untouched

The probe checks bytes at the addresses `docs/address-map.md` §3.3 identified,
in the live image:

```
    0x009a23b0 OK   [55 8b ec 68 00 29 9a 00]  CEG stub table 0x9A23B0[0]
    0x009a2980 OK   [55 8b ec 68 00 28 9a 00]  CEG stub table 0x9A2980[0]
```

They are there, they are unmodified, and the game runs anyway.

### Why CEG is a non-event here

Two static facts explain it, both re-derived for this experiment:

* Steam is running, the app is owned and installed, and Proton has shipped Steam
  CEG support since 6.3-8. `steamclient.dll`, `lsteamclient.dll` and `Steam.dll`
  are all in the live process's module list (§6).
* The CEG check bodies do not have the failure path the name suggests.
  `0x9A2080`, `0x9A2100`, `0x9A2180` — the ones upstream labels "HWBP detection"
  — disassemble to `GetModuleHandleExA` → `GetModuleFileNameA` → `CreateFileA` →
  `GetFileTime` → `InterlockedIncrement`. They take a **census**: they count, and
  there is no branch that exits or corrupts on failure.

---

## 4. The anti-debug check at `0x004C06E0` is not an anti-debug check

`journal.lunar.sh/2023/gsctool.html` documents `0x004C06E0` as a timing-based
anti-debug check. Disassembled in our copy it is this:

```
004c06e0  cmpb   $0x0, 0x4(%esp)          ; arg1 == 0 ?
004c06e5  jne    0x4c073d                 ; no -> ret, do nothing at all
004c06e7  push   %esi
004c06e8  mov    0xc(%esp), %esi          ; arg2
004c06ee  jg     0x4c0704
   ...    a1 d4 65 9b 00                  ; eax = [0x9B65D4]
   ...    8d 88 d4 65 9b 00               ; ecx = 0x9B65D4 + eax
   ...    89 4a fc                        ; *(edx-4) = ecx     <- writes a stack slot
0044c071d call   *0x9a31ac                ; KERNEL32!GetTickCount
004c0723  add    $0xffffffff, %esi        ; esi = arg2 - 1
004c0728  div    %esi                     ; edx = GetTickCount() % (arg2-1)
004c0736  mov    0x10(%esp,%edx,4), %edx  ; pick one of the caller's args AT RANDOM
004c073a  mov    %ecx, -0x4(%edx)         ; ...and write the result through it
```

The import at `0x9A31AC` is `KERNEL32!GetTickCount`, confirmed against the
import directory. So there **is** a timing call, and that is presumably where
the "timing-based" description came from — but `GetTickCount` is not used to
measure an interval. It is used modulo `(arg2-1)` to choose **which of the
caller's stack slots to write the result into**. This is CEG obfuscation: a
value delivered through a randomly-selected output slot so a static patcher
cannot predict where it lands. There is no comparison against a threshold, no
`IsDebuggerPresent`, no exit path.

It is also, structurally, exactly the function `docs/address-map.md` §3.3
categorised as *"ceg func called by multiple other funcs"* — the `cmp byte ptr
[esp+4], 0` clone with 37 and 33 callers. The address map and the published
write-up are describing the same function under two different names.

**The whole binary has no meaningful anti-debug surface.** Import-directory
search:

```
IsDebuggerPresent            thunk=009a32e4  callsites=2   0x96c088, 0x97e878
GetThreadContext             not imported
CheckRemoteDebuggerPresent   not imported
NtQueryInformationProcess    not imported
```

Both `IsDebuggerPresent` call sites sit immediately beside
`SetUnhandledExceptionFilter` / `UnhandledExceptionFilter` (`0x96C092`,
`0x96C09F`) — that is the MSVC CRT's `_invoke_watson` / unhandled-exception
path, present in every MSVC binary, not a protection.

### And it was tested, not just read

The probe watches its own debugger visibility and logs the moment it changes.
With `winedbg --gdb` attached to the live game:

```
  *** debugger state CHANGED at heartbeat 2 (t+10s): IsDebuggerPresent 0 -> 1, PEB->BeingDebugged=1
```

The process observed `PEB->BeingDebugged = 1` **and carried on** — the log
continues past that line with a full re-probe of every dvar. Nothing exited,
nothing corrupted itself.

**Conclusion for both questions: patch nothing. Neither CEG nor the `0x4C06E0`
check requires any intervention under Proton.**

---

## 5. Published addresses work in the live process

### 5.1 Byte anchors — 6/6

```
--- 1b. published-address anchors (in-memory bytes vs our disassembly)
    0x005ae810 OK   [8b 44 24 04 85 c0 74 1a]  Dvar_FindVar prologue  mov eax,[esp+4]; test eax,eax; je
    0x0068b370 OK   [53 8b 5c 24 08 85 db]     Dvar_Hash prologue     push ebx; mov ebx,[esp+8]
    0x006b8b20 OK   [a0 6b 34 96 03 c3]        R_IsStereoActive       mov al,[0x396346B]; ret
    0x004c06e0 OK   [80 7c 24 04 00 75 56]     0x4C06E0 (lunar.sh 'timing anti-debug'/CEG helper)
    0x009a23b0 OK   [55 8b ec 68 00 29 9a 00]  CEG stub table 0x9A23B0[0]
    0x009a2980 OK   [55 8b ec 68 00 28 9a 00]  CEG stub table 0x9A2980[0]
  6/6 anchors match
```

### 5.2 `Dvar_FindVar` called for real

The dvar structure offsets used here were derived from our own disassembly for
this experiment, not taken on trust. `Dvar_Create` at `0x862B20` is the source:

```
00862b65  mov  edx, [0x2691bf0]
00862b7e  imul esi, esi, 0x70          ; sizeof(dvar_s) == 0x70
00862b81  add  esi, 0x2621bf0          ; dvar pool base
00862b87  mov  [eax*4 + 0x261cbe8], esi ; dvar_s *table[]
00862b8f  mov  [0x261cbd4], eax         ; dvar count
00862b9f  mov  [esi + 0x10], eax        ; type      -> +0x10
00862bb9  mov  [esi], edi               ; name      -> +0x00
00862be8  mov  [esi + 0x18], eax        ; current   -> +0x18
00862beb  mov  [esi + 0x28], eax        ; latched   -> +0x28
00862bee  mov  [esi + 0x38], eax        ; reset     -> +0x38
```

and the hash-bucket walk inside `Dvar_FindVar`'s callee at `0x862280` gives the
remaining two:

```
008622ad  and  ecx, 0x3ff                    ; 1024 buckets
008622b3  mov  eax, [ecx*4 + 0x2620bf0]      ; bucket head array
008622c0  cmp  [eax + 8], esi                ; hash      -> +0x08
008622c5  mov  eax, [eax + 0x68]             ; next      -> +0x68
```

The live result — `run.sh` writes `com_maxfps "47"` into the **mirror's**
`players/config.cfg`, a value that appears nowhere else on this machine:

```
--- 9. dx->device became 0574a900 at t+4s: re-probing dvars
    com_maxfps    @02671120  name="com_maxfps"    hash=75fe5192==ours  type=5(int   ) flags=0x0001  value=47
    r_fullscreen  @0267fac0  name="r_fullscreen"  hash=b4577e49==ours  type=0(bool  ) flags=0x0021  value=false
    r_mode        @0267ff90  name="r_mode"        hash=18bfdf1b==ours  type=6(enum  ) flags=0x0021  value=1
    r_monitor     @02680000  name="r_monitor"     hash=4f6f6ffe==ours  type=5(int   ) flags=0x0021  value=0
    fs_game       @02674450  name="fs_game"       hash=c2a65097==ours  type=7(string) flags=0x011c  value="" (char* 03067c94)
    fs_basepath   @02674370  name="fs_basepath"   hash=c8efa5e5==ours  type=7(string) flags=0x0210  value="Z:\tmp\bo1vr-exp07\game"
    sv_cheats     @02670da0  name="sv_cheats"     hash=abb9b265==ours  type=0(bool  ) flags=0x0058  value=false
    r_gamma       @0267fb30  name="r_gamma"       hash=3047a9d9==ours  type=1(float ) flags=0x0001  value=0.900000
    version       @026846e0  name="version"       hash=73006c4b==ours  type=7(string) flags=0x0040  value="Call of Duty Singleplayer - Ship COD_T5_S SP build 7.0.152 CL(966072) CODPCAB-V64 CEG Thu Sep 01 16:06:17 2011 win-x86"
    cg_fov        @026889d0  name="cg_fov"        hash=f64b6f99==ours  type=1(float ) flags=0x0080  value=65.000000
    name          @02688500  name="name"          hash=7c9b0c46==ours  type=7(string) flags=0x0042  value="Unknown Soldier"
  12/12 dvars resolved AND fully cross-checked (name, hash, pool grid)
=== EXPERIMENT 7 LATE PASS: dvars 12/12, device 0574a900, dvar count 1970 ===
=== EXPERIMENT 7 FINAL: PASS ===
```

This is the **late** pass. The probe deliberately also runs an *early* pass the
moment the dvar count goes non-zero — which can be as early as two dvars — so
that log legitimately reads `1/12` a few lines above. Getting in that early is
the point: it proves the loader is up before the game is. The late pass, taken
once `dx->device` exists and `players/config.cfg` has therefore certainly been
applied, is the one whose numbers count.

Every line is checked three ways rather than merely printed:

* **`name` read back out of the struct equals the name we searched for.** A hash
  lookup that returned the wrong dvar, or a wrong `+0x00`, would show it.
* **`hash` in the struct equals our own reimplementation** of `Dvar_Hash`
  (`h = 0x1505; h = tolower(c) + h*33`), written from the disassembly of
  `0x68B370`. `==ours` on all twelve.
* **the returned pointer sits exactly on the 0x70 grid** of the pool at
  `0x2621BF0`, i.e. it is a real element of the game's dvar array.

Three further cross-checks:

```
--- 6. closing the loop: table -> name -> Dvar_FindVar -> same pointer?
  12/12 table entries round-tripped through Dvar_FindVar by name

--- 7. our Dvar_Hash reimplementation vs the game's own @0x0068b370
    Dvar_Hash("fs_game")       game=c2a65097 ours=c2a65097 ==
    Dvar_Hash("R_CUSTOMWIDTH") game=246e31f1 ours=246e31f1 ==
    Dvar_Hash("cg_fov")        game=f64b6f99 ours=f64b6f99 ==
  3/3 hashes agree
```

§6 walks the game's own `dvar_s *table[]`, pulls a name out of each entry, feeds
that name back through `Dvar_FindVar`, and requires the returned pointer to be
the entry it came from. §7 **calls** the game's `Dvar_Hash` and compares it with
ours — including a deliberately upper-case input, which only agrees if the
`tolower` in the loop was read correctly.

`com_maxfps == 47` is the load-bearing one. 47 came out of a config file this
experiment wrote, so reading it back out of the live process is not something a
plausible-looking coincidence can produce.

### 5.3 The renderer globals, i.e. BAC-281's target

```
--- 8. renderer globals (DxGlobals @0x3963440)
    dx->d3d9   (+0x04) = 001e92a0
    dx->device (+0x08) = 0574a900  <-- live IDirect3DDevice9, BAC-281's hook target
    dx->nvStereoActivated (+0x2B) = 0
    R_IsStereoActive() called @0x006b8b20 -> 0
    device vtable      = 7aaacb48 (plausible)
```

`R_IsStereoActive` is **called**, not just read, and its return agrees with the
byte at `dx->nvStereoActivated` that `docs/address-map.md` §6 says it returns.
The live `IDirect3DDevice9` is reachable through the published `DxGlobals` base.

---

## 6. `winedbg --gdb`, attached to the live game, breakpoint hit in our own code

**PASS.** `out/gdb-session.txt`, attached to the running `BlackOps.exe`
(Windows pid 0x150) with `winedbg --gdb <pid>`:

```
== attaching winedbg --gdb to Windows pid 336 (0x150)
WineDbg attached to pid 0150
Wine-gdb> Breakpoint 1 at 0x79bc150b: file bo1probe.c, line 458.
Wine-gdb> Continuing.
[Switching to Thread 648]

Thread 2 "0288" hit Breakpoint 1, bo1probe_breakpoint_target (
    tick=tick@entry=11, d3d9_device=0x0) at bo1probe.c:458
458	{

#0  bo1probe_breakpoint_target (tick=tick@entry=11, d3d9_device=0x0)
    at bo1probe.c:458
#1  0x79bc2a4c in worker (arg=0x0) at bo1probe.c:639
#2  0x7be9ec7c in InterlockedDecrement@4 ()
   from .../i386-windows/kernel32.dll
#3  0x7bf3cf93 in call_thread_func_wrapper () from .../i386-windows/ntdll.dll
#4  0x7bf70a42 in call_thread_func ()         from .../i386-windows/ntdll.dll

tick = 11
d3d9_device = 0x0
mixed = 11
$1 = 11          # g_bo1probe_heartbeat
$2 = 31          # g_bo1probe_last
eip            0x79bc150b     0x79bc150b <bo1probe_breakpoint_target>
```

Source file and line, named arguments with correct values, a named local, two
globals, and a backtrace crossing our `.asi` into Wine's own ntdll. `$2 = 31` is
the previous tick's `10*3+1`, so these are live values and not stale memory.

This is the first time the repository has debugged anything under **new-WoW64**;
Exp. 3's results were all obtained under classic WoW64 and it explicitly flagged
new-WoW64 as untested. It works. Exp. 3's one gotcha still applies —
`set breakpoint pending on` is mandatory, since every symbol we care about is in
a dynamically loaded DLL.

### Two new gotchas, neither of them the game's fault

**(a) The attach leaves a thread at a garbage EIP, and resuming it kills the
process.** After `winedbg --gdb <pid>` the break-in thread sits at `0xfff4fbd0`.
Let it run — with `continue`, or worse with `finish` on its corrupt frame — and:

```
Thread 1 "0230" received signal SIGSEGV, Segmentation fault.
0xfff4fbd0 in ?? ()
#0  0xfff4fbd0 in ?? ()
#1  0x7be9ec7c in InterlockedDecrement@4 () from kernel32.dll
#2  0x7bf3cf93 in call_thread_func_wrapper () from ntdll.dll
...
014c:0230: exit process (3221225477)          # 0xC0000005
```

**This is a Wine artifact, not BO1 anti-debug**, and that was established rather
than assumed: attaching identically to a **CEG-free host of our own** stops at
the very same `0xfff4fbd0`. The fix is one gdb line, and it is in
`gdb-attach.sh`:

```
handle SIGSEGV nostop noprint nopass
```

`nopass` is the operative word — gdb neither stops nor delivers the signal, so
the bogus thread spins harmlessly instead of taking the process down.

**(b) Ordering matters.** That `handle` line must come **after** the breakpoint
is set, not before `info sharedlibrary`. Issued too early it desynchronises
winedbg's gdb bootstrap and gdb never connects at all — measured, the whole
session then reports `No shared libraries loaded at this time` and a plain
`(gdb)` prompt instead of `Wine-gdb>`.

Also measured: attaching in the first ~5 s, while the game is still spawning
threads by the dozen, produces the same never-connected failure. `run.sh` waits
for the probe to announce itself first.

**gdb 15.1 bug, for completeness.** A *second* `continue` after the hit trips
`gdb/infrun.c:6706: internal-error: finish_step_over: Assertion
'ecs->event_thread->control.trap_expected' failed`. The first hit is completely
reliable; do not script a second resume.

---

## 7. The evidence channel: `~/steam-42700.log` does not exist under `proton run`

The brief suggested the loader banner reaching `~/steam-42700.log` as the cheap
signal. **That file is never created here**, and the reason generalises:

* `proton run` does not create it. That redirect only happens when *Steam
  itself* launches the game.
* Worse, under `proton run` the Windows process's stdio is swallowed outright.
  Measured with a trivial host exe: even its own `printf` never reaches the
  launching shell, so `src/log.c`'s `fprintf(stderr, ...)` cannot either. Exp. 4
  saw the same thing and worked around it with a file log.

`src/log.c` also mirrors every line to `OutputDebugStringA`, and **that** channel
survives:

```sh
export WINEDEBUG=+debugstr
...
0150:warn:debugstr:OutputDebugStringA "[bo1-vr] bo1-vr ASI loader (dinput8.dll proxy)\n"
```

`WINEDEBUG=+debugstr` is therefore the load-bearing setting for seeing the
loader's own banner, and `run.sh` sets it. The probe additionally writes its own
file log (`BO1VR_LOG`), which is the only channel that survives a debugger —
winedbg detaches the inferior's stdio in both modes (README, Logging).

---

## 8. How far the game actually gets, and the honest limitation

The game reaches, in its own words:

```
------ Server Initialization ------
Server: frontend
----- R_Init -----
Video memory for device: 4081 MB.
DirectX reports 4096 MB of video memory and 4037 MB of available texture memory.
Using picmip 0 on most textures, 0 on normal maps, and 0 on specular maps
Adding fastfile 'patch_ui' to queue
Adding fastfile 'en_frontend' to queue
Adding fastfile 'frontend' to queue
WARNING: Could not find zone 'Z:\tmp\bo1vr-exp07\game\zone\english\patch_ui.ff'
Loading fastfile 'en_frontend'
used 0.50 MB memory in DB alloc
```

and then exits, roughly 10–15 s in. **It never reaches the main menu.** No
screenshot of the game is claimed; `out/screen.png` is the desktop at the moment
the probe finished and does not show a game window.

### This is not caused by our loader

Controlled directly. A second mirror, `game-ctrl`, was staged with **no
`winmm.dll`, no `dinput8.dll`, no `.asi`** — stock game, same Proton, same
prefix, same config — and launched the same way. It stops at exactly the same
place:

```
=== CLEAN CONTROL (no shim/loader/asi) ===
Adding fastfile 'frontend' to queue
WARNING: Could not find zone 'Z:\tmp\bo1vr-exp07\game-ctrl\zone\english\patch_ui.ff'
Loading fastfile 'en_frontend'
used 0.50 MB memory in DB alloc
```

and then exits too. **The unmodified game gets no further than the instrumented
one.** Whatever ends the process is a property of this Proton/prefix/hardware
configuration, not of the ASI loader.

One candidate visible in the log, non-fatal on its own — the game survives it
and keeps loading for several seconds afterwards:

```
0150:err:virtual:allocate_virtual_memory out of memory for allocation, base (nil) size aa010000
```

0xAA010000 is 2.85 GB requested in a 32-bit process, immediately after
`--- Initializing Voice ---`. `BlackOps.exe` *is* `LARGE_ADDRESS_AWARE`
(`Characteristics=0x0123`), so it is entitled to ask, but 2.85 GB contiguous is
not available once DXVK, steamclient and 60-odd DLLs are mapped.

**Consequences for BAC-281.** Everything that experiment needs is reachable well
before this point — `dx->device` is live, the dvar system is fully populated
(1970 dvars), and `.text` is intact — but a camera hook that wants to run for
more than ~10 s, or wants an actual rendered frame on screen, will have to get
past this first. Diagnosing it is its own task -- done in
`experiments/08_launch/`, which traces it to a deliberate Steam-DRM
`ExitProcess(0)` rather than a fault -- and it is **not** a CEG,
anti-debug, or loader problem: it reproduces with a stock executable and no mod
loaded.

---

## 9. What CEG and anti-debug required, in one paragraph

**Nothing.** No CEG function was patched, hooked, stubbed or NOP'd; no anti-debug
check was bypassed; `BlackOps.exe` was not modified by a single byte, and the
mirror copy is `cmp`-verified against the original before every launch. The game
initialises its server, its renderer and its asset system with the ASI loader and
a plugin resident. `.text` in memory is byte-identical to `.text` on disk, so
there is nothing being decrypted that a patch could even be aimed at. The
function published as a timing anti-debug check at `0x004C06E0` calls
`GetTickCount`, but uses it to pick an output stack slot at random, not to
measure an interval — it has no exit path. The binary imports no
`GetThreadContext`, no `CheckRemoteDebuggerPresent`, no
`NtQueryInformationProcess`, and its only two `IsDebuggerPresent` call sites are
the MSVC CRT's unhandled-exception filter. Under a live debugger the process
observed `PEB->BeingDebugged = 1` and carried on regardless.

**So Nukem9/LinkerMod's `Patch_CEG()` is solving a problem that does not exist on
this stack.** Its premise is a native Windows process with no Steam client
mediation; ours is a Proton process where `steamclient.dll`, `lsteamclient.dll`
and `Steam.dll` are all mapped and Steam is running. Treat the scene's
CEG/anti-debug advice as inapplicable to us unless something concrete
contradicts this, and re-measure with `bo1probe.asi` (section 2 of its log) if
the executable is ever re-issued.

---

## 10. Reproduction

```sh
./run.sh                          # stage, build, launch, verify
BO1VR_GDB=1 ./run.sh              # + attach winedbg --gdb and break in our code
BO1VR_QUIT_AFTER_S=0 ./run.sh     # do not self-quit; run to the timeout
BO1VR_RUN_SECS=280 ./run.sh
WORK=/some/dir ./run.sh
```

`run.sh` **never writes inside the Steam install.** It builds a mirror of the
game directory under `$WORK/game` from symlinks to the read-only bulk (`main/`,
`zone/`, `Redist/`, `Soundtrack/`) and private copies of everything the game or
Steam CEG may write (`BlackOps.exe`, `players/`, the `*.STEAMSTART` files, the
small DLLs), then puts our three DLLs in the mirror. Proton is hard-link copied
and only the copy is patched, as in Exp. 4. The copy of `BlackOps.exe` is
`cmp`-verified byte-identical before each launch, and the run aborts if it is not.

Artifacts: `out/bo1probe.log` (the probe, from inside the process),
`out/console.txt` (`+debugstr`: the loader banner and the game's own logging),
`out/gdb-session.txt` (the debugger session), `out/run.txt`, `out/screen.png`.

Note that the four results above come from **different runs** — the game's short
lifetime (§8) means a run that spends its budget on a debugger session does not
also produce a fully populated dvar pass. `out/bo1probe.log` is from a clean run
without the debugger; `out/gdb-session.txt` is from a run with it.

---

## Environment this was verified on

| | |
|---|---|
| OS | Ubuntu 24.04.4 LTS, kernel 6.8 |
| Game | `BlackOps.exe` 8,101,944 bytes, md5 `2b179a57416680b60462c5af05552ea2`, Steam appid 42700, installed at `/mnt/games/steam/...` |
| Game build (its own `version` dvar) | `COD_T5_S SP build 7.0.152 CL(966072) CODPCAB-V64 CEG Thu Sep 01 16:06:17 2011 win-x86` |
| Proton | 10.0-4b, `PROTON_USE_WOW64=1`, + `tools/patch-proton-wow64-vrclient.py` |
| Steam client | running (required: CEG title, static `steam_api.dll` import) |
| D3D9 | DXVK (Fossilize shader-cache messages in the log confirm it) |
| Compiler | `i686-w64-mingw32-gcc` 13-win32, `-gdwarf-4`, unstripped |
| Debugger | `winedbg --gdb` + gdb 15.1 (`Ubuntu 15.1-1ubuntu1~24.04.1`) |
| Display | real Xorg 21.1.11 on `:1` |

---

## Not proven

* **The main menu, and anything rendered.** The game exits during frontend zone
  load (§8), with or without our loader. No game window was captured.
* **Why it exits.** Characterised and shown not to be ours; not diagnosed here.
  **Since answered by `experiments/08_launch/`**, which corroborates the
  control above (stock mirror, no mod, 8/8 configurations) and finds it is not
  a crash at all: the game calls `ExitProcess(0)` deliberately from a stub that
  pushes `0x8000DEAD` and opens the `STEAM_DRM_IPC` semaphores. Consistent with
  what is measured here -- exit code 0, no exception, no message. It still does
  not reach the main menu.
* **Long-lived hooks.** Nothing here installs a MinHook detour on a game
  function. `MH_Initialize` returns `MH_OK` inside the game, which is as far as
  this experiment takes it.
* **`winmm.dll` as the permanent bootstrap.** It works and is the least invasive
  choice today, but BAC-281 needs D3D9 hooks anyway, so a `d3d9.dll` proxy may
  end up being the natural home. The loader itself does not care.
* **A second `continue` under gdb**, which trips a gdb 15.1 internal assertion
  (§6).
* **Any of this via a Steam-launched game.** Everything used `proton run`.
