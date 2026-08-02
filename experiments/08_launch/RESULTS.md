# Experiment 8 — why `BlackOps.exe` exits during startup

**Question.** Exp. 7 left the game exiting ~10 s in, during `Loading fastfile
'en_frontend'`, with or without our loader. Why, and can it be got to the main
menu and into a map?

**Answer to "why": it is not a crash. The game deliberately calls
`kernel32!ExitProcess(0)` on its main thread, at a fixed point in the frontend
zone load, with no exception, no error message and no dialog — and the return
address on the stack when it does is `0x8000DEAD`, a constant that occurs
exactly once in the 8 MB executable and in no Wine module: as the operand of
`push $0x8000DEAD` inside a four-instruction stub whose only other act is to
call the routine that opens the game's `STEAM_DRM_IPC` semaphores.**

**Answer to "can it be got to the menu": NO. It still does not reach the main
menu, and no map has been loaded.** Everything below is measured; §8 is explicit
about what is inference and what the next step should be.

Reproduce with `./run.sh`. Nothing in the Steam install is written to. Verified
at the end: `find "/mnt/games/steam/steamapps/common/Call of Duty Black Ops"
-maxdepth 1 -newermt 2026-01-01` returns **only the directory itself** (whose
mtime was already 2026-08-01 19:00 before this experiment started); every file
inside — `BlackOps.exe` (`md5 2b179a57416680b60462c5af05552ea2`),
`players/config.cfg`, both shipped `*.STEAMSTART` — still carries its May 2025
timestamp.

---

## Verdict

| Claim | State |
|---|---|
| The failure reproduces with a **stock** mirror, **stock** Proton, no mod | **YES** — 8/8 configurations |
| It is a **crash** (unhandled exception / access violation) | **NO** — `+seh` shows every exception handled; process exit code 0 |
| It is a `Com_Error` / `Sys_Error` with a message we were not capturing | **NO** — the only `MessageBoxA` in the whole run is the safe-mode dialog (§6) |
| It is caused by the **Proton build** | **NO** — 8.0, 10.0 classic-WoW64, 10.0 new-WoW64 and Experimental 11.0 all identical |
| It is caused by **our Proton patches** | **NO** — every case here used unpatched Proton straight from the Steam library |
| It is caused by a **missing / corrupt asset or language pack** | **NO** — `en_frontend.ff` and `frontend.ff` are present, well-formed and readable (§5.3) |
| It needs Proton's **Steam launch verb** or Steam's environment | **NO** — `waitforexitandrun` + full `Steam*`/`STEAM_COMPAT_*` env fails identically |
| It is caused by a **stale `players/config.cfg`** | **NO** — deleting it entirely changes nothing |
| The game **opens** `zone/Common/frontend.ff` before dying | **NO** — it never opens it (§4) |
| The exit is **deliberate**, from the game's own code | **YES** — `ExitProcess(0)`, `ret=0x8000DEAD` (§3) |
| **Game reaches the main menu** | **NO** |
| **Game loads a map** | **NO** |

---

## 1. The failure, reproduced eight ways

`run.sh matrix` stages a **stock** mirror — no `winmm.dll` shim, no
`dinput8.dll`, no `.asi` — and launches it under four Proton configurations.
`out/<case>/gameconsole.txt` is the game's own `Com_Printf` output, recovered by
stripping the `+debugstr` wrapper off `OutputDebugStringA`.

| case | Proton | mode | verb | exit | lifetime | last line the game printed |
|---|---|---|---|---|---|---|
| `A_p10_wow64`   | 10.0 (`proton-10.0-4b`) | `PROTON_USE_WOW64=1` | run | 0 | 4.2 s | `used 0.50 MB memory in DB alloc` |
| `B_p10_classic` | 10.0 | classic WoW64 | run | 0 | 13.2 s | `used 0.50 MB memory in DB alloc` |
| `C_pexp_default`| Experimental (`experimental-11.0-20260724c`) | default | run | 0 | 5.0 s | `used 0.50 MB memory in DB alloc` |
| `D_p8`          | 8.0 (`proton-8.0-5d`) | classic WoW64 | run | 0 | 11.2 s | `used 0.50 MB memory in DB alloc` |
| `E_relay_exit`  | 10.0 | classic | run + `+relay` | 0 | 33.8 s | `used 0.50 MB memory in DB alloc` |
| `F_steamverb`   | Experimental | default | **`waitforexitandrun`** + Steam env | 0 | 3.7 s | `used 0.50 MB memory in DB alloc` |
| `G_freshcfg_log`| 10.0 | classic | run, **no `config.cfg`**, `+set logfile 2 +set developer 1` | 0 | 3.4 s | `used 0.50 MB memory in DB alloc` |
| `H_fileopen`    | 10.0 | classic | run + `+relay` on `CreateFile*` | 0 | — | `used 0.50 MB memory in DB alloc` |

Every single run stops on the same line. The tail is always:

```
Adding fastfile 'patch_ui' to queue
Adding fastfile 'en_frontend' to queue
Adding fastfile 'frontend' to queue
WARNING: Could not find zone 'Z:\...\game\zone\english\patch_ui.ff'
Loading fastfile 'en_frontend'
used 0.50 MB memory in DB alloc
```

**The lifetime varies from 3.4 s to 33.8 s but the stopping point does not.**
`E_relay_exit` is the load-bearing one: `+relay` slows the process by roughly an
order of magnitude, and it still died at exactly the same line. **Whatever ends
the process is a function of how far the load has progressed, not of wall-clock
time** — which rules out a plain timeout or watchdog.

`Could not find zone 'patch_ui.ff'` is a red herring: the shipped install has
`zone/Common/patch_ui_mp.ff` but no SP `patch_ui.ff` at all, so the warning is
what this build always prints. The game logs it and carries on.

---

## 2. It is not a crash

`A_p10_wow64` ran with `WINEDEBUG=+debugstr,+seh`. The complete exception census
for the whole process lifetime:

```
932  code=40010006   DBG_PRINTEXCEPTION_C          (OutputDebugString itself)
 26  code=406d1388   EXCEPTION_WINE_NAME_THREAD    (thread naming)
 10  code=e06d7363   EXCEPTION_WINE_CXX_EXCEPTION  (C++ throw)
  2  code=6ba        RPC_S_SERVER_UNAVAILABLE      (services.exe, AFTER the exit)
  1  code=6be        (ditto)
```

**No `c0000005`, no `c000001d`, no `80000003`, nothing unhandled.** The ten C++
exceptions are all raised at `0x9A219E` and all caught by a handler at
`0x009A2200` — i.e. inside the CEG stub region; they are part of CEG's normal
operation, not a fault (see §6b, which is where that matters). The last of them
is ~850 lines of log *before* the end, and the game keeps printing afterwards.

`wine: Unhandled exception` never appears. Under `winedbg --gdb` the process
ends with `exit process (0)` and `[Inferior 1 (Remote target) exited normally]`.

---

## 3. The exit path: `ExitProcess(0)`, return address `0x8000DEAD`

### 3.1 Relay

`out/E_relay_exit/` was run with `WINEDEBUG=+debugstr,+relay,+seh` and
`HKCU\Software\Wine\Debug` `RelayInclude` narrowed to the process-exit and
message-box entry points (`run.sh relay` writes the key). The game's main thread
is `0154`, and it does this:

```
0154:Call KERNEL32.ExitProcess(00000000) ret=8000dead
```

`out/H_fileopen/` reproduces it independently with a different relay filter and
a different thread id:

```
014c:Call KERNEL32.ExitProcess(00000000) ret=8000dead
```

Everything after that is normal teardown — `steam.exe`
(`TerminateProcess ... ret=1400083b9`) and Wine's own helper processes.

### 3.2 The same thing under a debugger, with the stack

`out/gdb-exitwho.txt`, breakpoint on `kernel32!ExitProcess@4`, taken on the game's
main thread:

```
Thread 1 (Thread 256 "0100"):
#0  0x7bebebfb in ExitProcess@4 () from .../i386-windows/kernel32.dll

eax  0x9a30d8      ecx  0x9a30d8      esp 0x497e2d8      ebp 0x497e2d8

0x497e2d8:  0x0497eb00  0x8000dead  0x00000000  0x0497eaf8
0x497e2e8:  0x00000000  0x0497f000  0x8000dead  0x009a30d8
0x497e2f8:  0x00000000  0x00000000  0x00000000  0x00000000   <- and zero from here on
```

`ExitProcess@4+3` is immediately after `push ebp; mov ebp,esp` — which is why
`ebp == esp` — so the frame reads: `[esp]` saved `ebp` (a valid stack address),
`[esp+4]` **return address `0x8000DEAD`**, `[esp+8]` **`uExitCode` 0**. That
agrees with the relay line exactly, and with `exit process (0)`.

`eax == ecx == 0x009A30D8` is the address of the **`ExitProcess` IAT slot**, and
that same value is sitting on the stack four words up, next to a second copy of
`0x8000DEAD`.

### 3.3 What `0x8000DEAD` is

It is the game's own constant, and it is unique:

```
occurrences of 8000dead in the WHOLE 8MB BlackOps.exe: ['0x122456']    # = VA 0x00523056
files in Proton 10's i386-windows / i386-unix / x86_64 trees containing it: NONE
```

Confirmed live as well — gdb's `find /b 0x00400000, 0x04270000, 0x68,0xad,0xde,0x00,0x80`
over the mapped image returns **one** hit, `0x523055`, and the code there is
byte-identical to disk:

```
0x523050:  call   0x5f3290
0x523055:  push   $0x8000dead
0x52305a:  call   *0x9a30d8          ; KERNEL32.ExitProcess
0x523060:  int3
```

`0x005F3290` — the only thing that stub does before dying — is the routine that
builds a NULL DACL and creates three named semaphores:

```
0x9E0BF4  "STEAM_DIPC_CONSUME"
0xA01DF4  "SREAM_DIPC_PRODUCE"     (sic, typo is Treyarch's)
0x9CECC0  "STEAM_DRM_IPC"
```

`0x005F3290` has exactly **two** callers in the whole binary. The other one is
`0x00454570`, which is the same shape with a message attached:

```
00454570  call 0x8f0aa0                      ; a check
00454575  test al,al
00454577  jne  0x45458a                      ; passed -> skip the message
00454579  push 0x30 / push 0 / push 0xa20248 / push 0
00454584  call [0x9a33a8]                    ; USER32.MessageBoxA("This file contains strips")
0045458a  call 0x5f3290                      ; <- Steam DRM IPC
0045458f  push 0
00454591  call [0x9a30d8]                    ; ExitProcess(0)
00454597  int3
```

So the binary has a matched pair of **Steam-DRM self-destruct stubs**: one noisy
(`"This file contains strips"`, `ExitProcess(0)`) and one silent
(`ExitProcess(0x8000DEAD)`), both routed through the same `STEAM_DRM_IPC`
notification. `0x8000DEAD` is that mechanism's marker value, and it is what ends
up on the stack when the game dies.

### 3.4 What could not be pinned down

Breakpoints at `0x00523050`, `0x0052305a`, `0x005F3290` and `0x00454570` **do
not fire** (`out/gdb-stub.txt`, `out/gdb-launch4.txt`) — nor do breakpoints at
any of the five `call [ExitProcess]` sites, the three `TerminateProcess` sites,
or either of `WinMain`'s two `ret 0x10` paths. The breakpoint mechanism itself is
sound: the **control**, a breakpoint at `Dvar_FindVar` `0x005AE810`, hits
immediately (`Thread 1 "00fc" hit Breakpoint 1, 0x005ae810`).

So the transfer into `ExitProcess` is made from something other than the static
call site — and the observed frame (`ret=0x8000DEAD`, `uExitCode=0`, i.e. the two
values in the *opposite* order to what `push 0x8000dead; call ExitProcess` would
leave) says it is a hand-built tail jump of the form
`push 0; push 0x8000DEAD; jmp ExitProcess`, with `eax`/`ecx` still holding the
IAT slot address. That is CEG-stub shaped. **It is stated here as the strongest
reading of the evidence, not as something disassembled.** What *is* demonstrated
is that the process ends by voluntarily entering `ExitProcess(0)` carrying
`BlackOps.exe`'s own DRM marker value.

---

## 4. Exactly where it stops

`out/H_fileopen/` relays every `CreateFileA`/`CreateFileW`. In load order, the
last file activity of the process:

```
017c: CreateFileA "...\zone\english\patch_ui.ff"      <- misses, warned about, harmless
017c: CreateFileA "...\zone\english\en_frontend.ff"   <- opens, loads, "used 0.50 MB"
0190: CreateFileA "...\main\images\menu_mp_lobby_new.iwi"   } image streamer probing
0190: CreateFileA "...\main_shared\images\..."              } the three search paths
0190: CreateFileA "...\players\images\..."                  } for six loose textures
      ... cuba_intel, khe_sanh_intel, flashpoint_intel, vorkuta_intel, hue_city_intel ...
014c: Call KERNEL32.ExitProcess(00000000) ret=8000dead
```

**`zone\Common\frontend.ff` is never opened at all** — one `CreateFileA` mentions
`frontend.ff` in the entire run and it is the `en_` one. The game dies in the gap
between finishing the localised frontend zone and starting the real one.

(`main\video\treyarch.bik` *is* opened, through `binkw32.dll`, earlier in the run,
so the logo movie path works.)

---

## 5. What it is not

### 5.1 Not the Proton build, not the WoW64 mode, not our patches
Four Proton builds spanning 8.0 → Experimental 11.0, both WoW64 modes, all
**unpatched, straight out of the Steam library** — identical failure (§1). The
patched Proton copy and `PROTON_USE_WOW64=1` that the VR chain needs are
therefore not implicated in this at all.

### 5.2 Not the launch method
`F_steamverb` used Proton's **`waitforexitandrun`** verb — the verb Steam itself
invokes, not `run` — with `SteamClientLaunch=1`, `SteamEnv=1`,
`SteamOverlayGameId`, `STEAM_COMPAT_APP_ID`, `STEAM_COMPAT_INSTALL_PATH`,
`STEAM_COMPAT_LIBRARY_PATHS`, `STEAM_COMPAT_MOUNTS` and
`STEAM_COMPAT_SHADER_PATH` all set, on top of the `SteamGameId`/`SteamAppId`
Exp. 7 already had. Died at the same line in 3.7 s. The Steam client was running
throughout (it registers our process: `Adding process ... for gameID 42700`
appears in `~/.local/share/Steam/logs/console-linux.txt` for every launch).

### 5.3 Not a missing or damaged asset
```
en_frontend.ff  429536     4957666675313030 d9010000     # "IWffu100", version 0x1d9
frontend.ff     40995296   4957666675313030 d9010000
patch.ff        1530048    4957666675313030 d9010000
en_patch.ff     8704       4957666675313030 d9010000
```
All present, correct magic and version, and `frontend.ff` reads end-to-end in
0.06 s. `zone/` on disk is `Common/` and `English/` while the game asks for
`zone\english\` — Wine's case-insensitive lookup resolves that correctly, which
is proven by `en_frontend.ff` actually being found and loaded. The install is
complete (`appmanifest_42700.acf`, `StateFlags 4`, all seven depots installed).

### 5.4 Not the config
`G_freshcfg_log` deleted `players/config.cfg` outright — so no
`r_mode "2560x1440"`, no `r_displayRefresh "144 Hz"`, no stale
`sd_xa2_device_guid`, no `sys_configSum`. The game came up on its own defaults
(`Texture detail is set automatically` instead of `Picmip is set manually`) and
died at the same line in 3.4 s. Exp. 7's windowed 1024x768 run behaved the same
way, so it is not fullscreen, resolution or refresh rate either.

### 5.5 Not the display or a gamepad
`DISPLAY=:1` is a real X session on the NVIDIA RTX 3080 Ti (`glxinfo -B`:
`direct rendering: Yes`, `NVIDIA 595.84`). The game creates its window
(`WM_NCCREATE`/`WM_CREATE`/`WM_SIZE` on its own `WndProc` at `0x00554AA0`), gets
`WM_ACTIVATEAPP wp=1`, `WM_SETFOCUS` and `WM_PAINT`, and paints. **Its window
never receives `WM_CLOSE`, `WM_QUIT`, `WM_DESTROY` or `WM_ENDSESSION`** — the
exit does not come from the window at all.

---

## 6. Two hazards that will waste the next agent's time

### (a) The "Run In Safe Mode?" dialog poisons every run after the first

`Sys_CheckImproperQuit` at `0x004F1930` reads a 4-byte pid marker from

```
<prefix>/drive_c/users/steamuser/AppData/Local/Activision/CoD/__BlackOps
```

and, if it is there, puts up a **modal** `MessageBoxA` before anything else
happens:

```
0154:Call user32.MessageBoxA(00000000,"It appears that Call of Duty: BlackOps did not quit
     properly the last time it ran.\nDo you want to run the game in safe mode?...",
     "Run In Safe Mode?",00000033) ret=004f19e5
```

Every killed run leaves that marker behind, so from the second launch onward the
game blocks on a dialog nobody clicks. Worse, the answer matters: `WinMain` at
`0x0050A730` does

```
0050a7ff  call 0x4f1930          ; the dialog
0050a804  test eax,eax
0050a806  je   0x50a82b          ; -> call 0x45c4c0; xor eax,eax; ret 0x10   (WinMain returns 0)
```

so a *cancelled* dialog makes `WinMain` return 0 — **a clean `exit(0)` that is
indistinguishable from the failure under investigation**. `run.sh` and
`gdb-launch.sh` both delete the marker before every launch. Do not skip that.

### (b) `handle SIGABRT nostop noprint nopass` in gdb kills the game

CEG throws C++ exceptions (`0xE06D7363`) as part of its normal control flow and
catches them in its own handler at `0x009A2200`. winedbg's gdb proxy maps that
unknown code to `SIGABRT`, and `nopass` makes winedbg continue the debug event
with `DBG_CONTINUE` — the game's handler never runs, CEG's control flow breaks,
and the process exits(0) right after `Sys_Init`, ~400 lines earlier than the real
failure. `out/gdb-launch3.txt` is that false trail; `out/gdb-launch4.txt` is the
same script with `pass` and it reproduces the genuine failure under the debugger.

**Exp. 7's `handle SIGSEGV nostop noprint nopass` is correct for *attaching*** (it
neutralises the bogus break-in thread) **and wrong for *launching*.** When
launching under `winedbg --gdb`, pass every signal through.

---

## 7. Recorded but not yet implicated: a 2.66 GiB allocation failure

Present in A, B, C and E (Proton 8 does not log the channel), on the **main
thread**, immediately after `--- Initializing Voice ---`:

```
0154:err:virtual:allocate_virtual_memory out of memory for allocation, base (nil) size aa010000
```

0xAA010000 = 2,852,126,720 bytes in a 32-bit `LARGE_ADDRESS_AWARE` process.
Exp. 7 saw the same. It is **not** immediately fatal — the game prints ~600 more
console lines and reaches renderer init afterwards — but it is the only sign of
distress in the log and it happens before the point of death. Whether the
frontend zone allocation later fails because of the resulting fragmentation is
untested; `frontend.ff` is never opened, so if it is the cause it acts before the
file is even touched, which does not obviously fit.

Also recorded, in case it matters later: the game reports `8 logical CPUs
reported / 8 physical CPUs detected` on a 24-thread i9-12900K, and `System memory
is 1024 MB (capped at 1 GB)`.

---

## 8. Honest statement of where this leaves things

**The game does not reach the main menu, and no map has been loaded.** That is
unchanged from Exp. 7. What is new is that the failure now has a shape:

* it is a **voluntary, silent `ExitProcess(0)`** from the game's main thread, not
  a crash, not a `Com_Error`, and not a window close;
* it happens at a **fixed point in the asset load** — after
  `zone/English/en_frontend.ff` is loaded and before `zone/Common/frontend.ff` is
  opened — and that point is invariant under a 10x change in execution speed;
* the only identifying mark it carries, `0x8000DEAD`, belongs to
  `BlackOps.exe`'s **Steam-DRM/CEG self-destruct stub** and to nothing else in
  the process;
* it is independent of Proton version, WoW64 mode, launch verb, Steam
  environment, config file, display mode and our mod.

This does *not* contradict Exp. 7's finding that CEG needs no patching — CEG
plainly works for the first several seconds — but it does contradict the reading
that CEG is a non-event. The correct statement now is: **CEG requires no
intervention to get the game running, and something CEG-mediated ends it a few
seconds later.** Exp. 7's §3.2 result (`.text` in memory byte-identical to disk)
remains true and is not in tension with this; nothing needs to be decrypted for a
stub that was always there to be jumped to.

### The one test that has not been run, and should be first

**Launch the game through the Steam client itself** — `steam -applaunch 42700`,
or the Play button — and see whether it reaches the menu. This is the only
hypothesis on the list that could not be tested here, because it necessarily
runs the executable out of `/mnt/games/steam/steamapps/common/Call of Duty Black
Ops` and lets it write `players/config.cfg` and `BlackOps.exe.<pid>.STEAMSTART`
into the install, which this experiment was instructed not to touch. Everything
else about Steam was replicated (`waitforexitandrun`, the `Steam*` and
`STEAM_COMPAT_*` environment, a live Steam client, `steamclient.dll`/`steam.dll`
resident) and made no difference — but Proton's `steam.exe` shim and Steam's own
`reaper`/Steam-Linux-Runtime-4.0 container wrapper were not, and CEG's ownership
handshake is exactly the sort of thing that could care.

There is strong circumstantial reason to think a working configuration exists on
this machine: `players/config.cfg` in the install is a real played-in config
(`sys_gpu "NVIDIA GeForce RTX 3080 Ti"`, `r_mode "2560x1440"`,
`sensitivity "2.69159"`, `sv_lastSaveGame "save\flashpoint-13.svg"`),
`players/save/` holds seven real save files dated 2–3 May 2025, and Steam records
`"Playtime" "1937"` minutes for app 42700. Whatever that configuration was, it is
not `proton run` from a mirror.

If a Steam launch also fails, the next lead is the `0x005F3290` /
`STEAM_DRM_IPC` handshake itself: hook `CreateSemaphoreA`/`OpenSemaphoreA` and
`ReleaseSemaphore` from the `winmm` shim of Exp. 7, log the DRM IPC traffic, and
find out which side stops answering. The loader is already proven to get into
the process early enough (Exp. 7 §2), and a hook on `ExitProcess` placed there
would also finally capture a real call stack for the exit, which the static
breakpoints in §3.4 could not.

---

## 9. Files

```
run.sh          stage the mirror; drive the case matrix; write the relay filter
runcase.sh      one launch = one Proton x one WoW64 mode x one WINEDEBUG x one verb
gdb-launch.sh   launch under winedbg --gdb with breakpoints on every exit path
out/<case>/     case.txt, rc.txt, elapsed.txt, console.txt (raw), gameconsole.txt
out/gdb-*.txt   the debugger sessions, including the two false trails of §6b
```

`WORK` defaults to `/mnt/games/tmp/bo1vr-exp08` rather than `/tmp` — the root
filesystem has 2.0 GB free and a Proton prefix does not fit.
