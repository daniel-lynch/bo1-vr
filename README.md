# bo1-vr

VR mod scaffolding for **Call of Duty: Black Ops (2010)** running under Proton on Linux.

This repository is the toolchain foundation. It contains a working 32-bit
`dinput8.dll` ASI loader and four verification experiments. **It hooks nothing in
the game yet** — but the VR chain itself is no longer hypothetical: a 32-bit
mingw DLL running under Proton now gets a **live OpenVR function table** from
xrizer/Monado and calls through it, with no VR hardware attached. See
`experiments/04_live_fntable/`.

---

## Status

| Piece | State |
|---|---|
| 32-bit `dinput8.dll` ASI loader | builds, self-verifies, **runs under Wine** |
| MinHook (vendored, MinGW makefile path) | builds, links, `MH_Initialize` returns `MH_OK` |
| Exp. 0 — loader smoke test | passes; found one real bug in our VEH |
| Exp. 1 — OpenVR C API from a mingw DLL | passes up to the point a VR runtime is required |
| Exp. 2 — C++ exceptions across a DLL boundary | **answered, with a surprise** |
| Exp. 3 — winedbg on a 32-bit target | **answered — works far better than expected** |
| Exp. 4 — **live OpenVR FnTable from 32-bit under Proton** | **PASS** — non-NULL table, methods called, `WaitGetPoses` returns a valid HMD pose |

Full commands and raw output live in `experiments/*/RESULTS.md`.
`experiments/04_live_fntable/run.sh` reproduces the gating result end to end.

---

## Building

```sh
make toolchain-check     # verifies i686-w64-mingw32-gcc is present
make                     # -> dist/dinput8.dll, then runs `make verify`
make install BO1_DIR=/path/to/Call\ of\ Duty\ Black\ Ops
```

### Toolchain requirement

```sh
sudo apt install gcc-mingw-w64-i686 g++-mingw-w64-i686 binutils-mingw-w64-i686
```

On Ubuntu 24.04 that installs GCC 13 for `i686-w64-mingw32`, configured
`--disable-sjlj-exceptions --with-dwarf2` (verified with `g++ -v`), i.e. the
DWARF-2 exception model. That configuration detail matters — see Decision 6.

**Two threading models are packaged, and `update-alternatives` picks one.**
Ubuntu ships both `i686-w64-mingw32-gcc-win32` (`Thread model: win32`) and
`i686-w64-mingw32-gcc-posix` (`Thread model: posix`); the unsuffixed
`i686-w64-mingw32-gcc` is an alternatives symlink and **win32 has the higher
priority**, so a default install compiles with `gcc version 13-win32`. Check
which you have — the banner printed by `dist/dinput8.dll` reports it:

```sh
i686-w64-mingw32-gcc -v 2>&1 | grep -E 'Thread model|gcc version'
```

Everything in this repository has been measured under **both** models and every
behavioural result is identical. The threading model changes exactly two
observable things, both cosmetic-to-us:

* the shared C++ runtime's own dependency list — 13-posix's `libgcc_s_dw2-1.dll`
  and `libstdc++-6.dll` import `libwinpthread-1.dll`, 13-win32's do not
  (Decision 7);
* how many prebuilt-CRT DWARF-5 CUs land in the output — 35 under 13-posix,
  29 under 13-win32, because winpthreads is not linked in (Decision 4).

### Installing

Copy `dist/dinput8.dll` next to `BlackOps.exe`. Drop any `.asi` plugins in the
same directory. **No `WINEDLLOVERRIDES` is required** — see Decision 2.

---

## Configuration decisions, and why

These were settled deliberately. Each one has a failure mode that is expensive to
rediscover. Do not change one without reading its reason.

### 1. Build system: plain Makefile, not CMake

CMake pays for itself with multiple configurations, platforms, or discovered
dependencies. This project has exactly one target triple forever
(`i686-w64-mingw32`) and will never build for x86-64, Linux, or MSVC.

It also actively hurts here. MinHook must **not** be built through its
`CMakeLists.txt` (Decision 5), but the natural CMake idiom for a vendored
dependency — `add_subdirectory(third_party/minhook)` — is exactly that broken
path. A Makefile makes the correct call the obvious one.

### 2. The loader is named `dinput8.dll`

Proton 10 and 11 already list `dinput8.dll` as prefer-native in their builtin/native
policy, precisely because mod and ASI loaders have hijacked that name for years.
Naming ourselves `dinput8.dll` means installation is "copy the file" with zero
configuration.

If a DLL override is ever needed for some *other* module, spell it `name=n,b` and
**never** bare `name=n`. With a bare `n`, Wine returns `DLL_NOT_FOUND` at the
moment the proxy tries to `LoadLibrary` the real system DLL, because the override
also applies to that second load. The `,b` fallback lets the builtin satisfy it.

We do not use `.def`-file export forwarding, because the forward target would be
our own module name. `src/proxy.c` resolves the five real exports by hand from
`%SystemRoot%\system32\dinput8.dll` (which, in a 32-bit process under WoW64,
`GetSystemDirectoryA` already resolves to `syswow64`).

### 3. Exports come from a `.def` file

`dinput8.def` lists the five names undecorated. The game imports
`DirectInput8Create`, not `_DirectInput8Create@20`; if mingw's stdcall decoration
leaks into the export table the process dies at load with an unhelpful message.
`-Wl,--kill-at` plus the `.def` gives exactly the right names, and `make verify`
fails the build if that ever regresses.

The five exports were confirmed against Proton 10.0-4b's own 32-bit
`files/lib/wine/i386-windows/dinput8.dll` — `DirectInput8Create`,
`DllCanUnloadNow`, `DllGetClassObject`, `DllRegisterServer`,
`DllUnregisterServer`, ordinal base 1, and nothing else.

### 4. `-gdwarf-4`, and never strip

Wine's dbghelp hard-clamps `max_supported_dwarf_version` to 4. GCC 11+ and Clang
14+ default to DWARF 5 — the compiler in use here (GCC 13) does emit DWARF 5 by
default. Get this wrong and Wine does not warn or degrade; it silently finds no
symbols at all.

**Caveat found while testing (see Exp. 3):** `-gdwarf-4` only governs the CUs
*we* compile. The distro's prebuilt mingw-w64 runtime — `mingw-w64-crt`
(`crtdll.c`, `tlssup.c`, `pseudo-reloc.c`, …), `winpthreads`, `libgcc` — ships
precompiled as DWARF 5 and gets linked into every binary. `dist/dinput8.dll`
therefore contains DWARF-5 CUs that no compiler flag of ours can change: **29
under GCC 13-win32, 35 under GCC 13-posix** (`make verify` prints the count).
The difference is winpthreads — the posix build links `cond.c`, `mutex.c`,
`rwlock.c`, `sched.c`, `spinlock.c`, `thread.c` and winpthreads' `misc.c`, the
win32 build links none of them and picks up `tlsmthread.c` instead.

This turns out **not** to matter under either model: symbol and line resolution
for our own code works fine, both in winedbg and in gdb. Re-verified directly
against the shipped artifact under 13-win32 — `break asi_load_all` resolves to
`src/asi_loader.c:17` in native winedbg and gives a full backtrace through
`DllMain@12` → `LdrLoadDll@16` in `--gdb` mode, with all 29 DWARF-5 CRT CUs
present. `make verify` identifies our CUs positively (by path) and only *reports*
the toolchain ones.

MinHook's own MinGW makefile passes `-s` in its `LDFLAGS`; the top-level Makefile
overrides `CFLAGS` when invoking it so the static library keeps its debug info.

### 5. MinHook via `build/MinGW/Makefile`, never its CMake

MinHook is vendored at `third_party/minhook` (upstream commit `d94c64d`).

Build it with:

```
make -C third_party/minhook -f build/MinGW/Makefile libMinHook.a CROSS_PREFIX=i686-w64-mingw32-
```

MinHook's CMake selects `hde32.c` vs `hde64.c` from `CMAKE_SIZEOF_VOID_P`, which
under a cross compiler reflects the **host**, so it picks the 64-bit length
disassembler for our 32-bit build. That links cleanly and then mis-decodes every
function prologue at runtime.

The MinGW makefile instead globs `src/*.c src/hde/*.c` and compiles both. That is
correct because each file self-guards — `hde32.c` opens with
`#if defined(_M_IX86) || defined(__i386__)` and `hde64.c` with the `_M_X64`
equivalent — so under `i686-w64-mingw32` the 64-bit one compiles to an empty
object. The preprocessor decides, not the build system. (Verified: the build
compiles `hde32.o` and `hde64.o`, and only the former contributes code.)

Note the makefile lives in `build/MinGW/` but its paths are relative to the
MinHook root, so it must be invoked with `-f` from the root.

### 6. Never let an exception escape a hook callback

32-bit mingw has no SEH: `__try`/`__except` do not compile, and this toolchain
uses DWARF-2 unwinding, which walks `.eh_frame` and cannot unwind through a frame
without CFI — which is every MSVC-built frame in `BlackOps.exe`.

**This was tested and confirmed** (Exp. 2, case D): throwing through a single C
frame compiled `-fno-asynchronous-unwind-tables -fno-unwind-tables` calls
`std::terminate` and kills the process. Throwing through the *same* frame compiled
normally works, because GCC emits `.eh_frame` for C by default.

Use `AddVectoredExceptionHandler` instead — `src/veh.c`. It is an OS-level
callback, not a compiler construct, so it is ABI-neutral across mingw and MSVC
frames. Ours logs and always returns `EXCEPTION_CONTINUE_SEARCH`: it is a tracer,
not a handler. Swallowing faults would mask real game crashes.

**A VEH that logs is a feedback loop waiting to happen.** `OutputDebugStringA`
raises `DBG_PRINTEXCEPTION_C` (`0x40010006`), so a handler that logs every
exception will log its own log line, forever. This actually happened on the first
run (Exp. 0, finding 3). `src/veh.c` now filters informational-severity codes
before touching the logger and keeps a per-thread reentrancy guard. Do not remove
either.

### 7. C++ plugins must link libstdc++/libgcc *dynamically*

Discovered in Exp. 2 and **not** in the original research notes.
Re-measured under GCC 13-win32 and GCC 13-posix; the result is the same in both.

With `-static-libgcc -static-libstdc++ -static`, an exception thrown in a DLL and
caught in another module calls `std::terminate`, because each module gets its own
unwinder and its own copies of the `type_info` objects. With the shared
`libgcc_s_dw2-1.dll` + `libstdc++-6.dll`, cross-module throw/catch and
cross-module RTTI both work correctly.

**Which DLLs to ship depends on the threading model — measure, do not guess.**
The full transitive import closure of a C++ plugin built shared, from
`i686-w64-mingw32-objdump -p`:

| | GCC 13-win32 | GCC 13-posix |
|---|---|---|
| plugin `.dll` / host `.exe` imports | `KERNEL32.dll`, `msvcrt.dll`, `libgcc_s_dw2-1.dll`, `libstdc++-6.dll` | same |
| `libstdc++-6.dll` imports | `KERNEL32.dll`, `msvcrt.dll`, `libgcc_s_dw2-1.dll` | + `libwinpthread-1.dll` |
| `libgcc_s_dw2-1.dll` imports | `KERNEL32.dll`, `msvcrt.dll` | + `libwinpthread-1.dll` |
| **must ship beside `BlackOps.exe`** | **2 DLLs** — `libgcc_s_dw2-1.dll`, `libstdc++-6.dll` | **3 DLLs** — plus `libwinpthread-1.dll` |

`KERNEL32.dll` and `msvcrt.dll` come from the prefix. Confirmed by deletion under
13-win32: removing `libwinpthread-1.dll` from the output directory changes
nothing (all four exception cases still pass, exit 0), while removing
`libgcc_s_dw2-1.dll` kills the process before `main` with

```
err:module:import_dll Library libgcc_s_dw2-1.dll (which is needed by ...\host2.exe) not found
err:module:loader_init Importing dlls for ...\host2.exe failed, status c0000135
```

`experiments/02_cpp_exceptions/Makefile` therefore derives the list by walking
the actual import tables rather than hardcoding names, so it stays correct under
either compiler.

The loader itself is pure C and links `-static` deliberately, so it has no runtime
DLL dependencies at all — `dist/dinput8.dll` imports only `KERNEL32.dll` and
`msvcrt.dll` (`make verify` asserts no libgcc dependency). Any C++ `.asi` plugin
that throws across a module boundary must ship the runtime DLLs beside
`BlackOps.exe` — or, preferably, catch everything internally and never throw
across a boundary at all, which is required anyway by Decision 6.

### 8. OpenVR: use the C API (`openvr_capi.h` + `FnTable:`), not the C++ vtable

Confirmed correct, but for a partly different reason than assumed — read this
before relying on it.

`OPENVR_FNTABLE_CALLTYPE` is `__stdcall` on Win32, and the free entry points
(`VR_GetGenericInterface` etc.) are `__cdecl` and exported undecorated. That
combination is unambiguous from mingw, needs no vtable layout assumptions, no
C++ name mangling, and no `__thiscall` emulation. Valve maintains this API
specifically for non-C++ language bindings, so it is the stable contract.

**However**, the specific justification that the C API "dissolves" the 8-byte
struct-return problem in `GetHiddenAreaMesh` does not hold up as stated. See the
"Corrections" section below.

Practical note: `openvr_capi.h` cannot be included from C as-is. For `_WIN32`
without `OPENVR_API_EXPORTS`/`OPENVR_API_NODLL`, `S_API` expands to
`extern "C" __declspec(dllimport)`, which is not valid C. Define
`OPENVR_API_NODLL` and resolve everything with `GetProcAddress`.

### 9. Run under Proton's **new-WoW64** (`PROTON_USE_WOW64=1`), not classic WoW64

This is the decision the whole Linux plan rests on, and it was settled by
measurement in Exp. 4. Read `experiments/04_live_fntable/RESULTS.md` before
changing it.

Proton's 32-bit PE `vrclient.dll` loads the native OpenVR runtime from a path
that is fixed at compile time by the **PE** architecture
(`vrclient_x64/vrclient_main.c`): a 32-bit game always wants
`$PROTON_VR_RUNTIME/bin/vrclient.so`, where the 64-bit build wants
`bin/linux64/vrclient.so`.

* **Classic WoW64** puts a 32-bit ELF unix side under the process, so that
  `bin/vrclient.so` must be a **32-bit** native library. xrizer ships only
  `bin/linux64/vrclient.so`, and so does OpenComposite. Supplying one would mean
  building a 32-bit xrizer, a 32-bit OpenXR loader, **and a 32-bit OpenXR
  runtime** — and there is no i386 Monado, nor any prospect of one without
  building it from source against a full i386 dev sysroot. This path is a dead
  end today.
* **New-WoW64** keeps the unix side 64-bit, so the *same* 64-bit xrizer, OpenXR
  loader and Monado that already work serve the 32-bit game. Stage the runtime
  so `$RUNTIME/bin/vrclient.so` is a symlink to `linux64/vrclient.so` and it
  resolves correctly.

New-WoW64 needs two Proton defects worked around, both applied by
`tools/patch-proton-wow64-vrclient.py` to a hard-linked *copy* of Proton:
Proton ships no `x86_64-unix/vrclient.so` for the 32-bit PE to bind to, and
`struct wow64_vrclient_init_params` leaves `HMODULE winevulkan` unwrapped so
every field after it is 4 bytes out of step. Both are proven from the shipped
binary's own disassembly. Report them upstream rather than carrying the patch
forever.

Two consequences to remember:

* **`SteamGameId` must be set** or nothing works, silently. Proton's `steam.exe`
  only calls `vrclient_init_registry()` when it thinks it is launching a game,
  and that is decided purely by `SteamGameId` being present.
* **An OpenVR method xrizer has not implemented kills the process.** xrizer
  `todo!()`s panic in Rust and the unwind lands in a frame Wine cannot dispatch.
  `GetCurrentSceneFocusProcess` and `GetLastFrameRenderer` are two such methods.
  Check coverage before calling anything new.

### 10. Target `IVRSystem_023`, not the `IVRSystem_026` in our own header

`third_party/openvr` is OpenVR master. Neither Proton 10.0-4b's `vrclient` nor
xrizer knows any `IVRSystem` past `_023`, so `"FnTable:IVRSystem_026"` returns
**NULL** — measured. Worse, master's table is not a prefix of `_023`'s: master
inserts `ComputeDistortionSet` at slot 4, so calling an `_023` table through
master's struct would silently invoke the wrong slot from
`GetEyeToHeadTransform` onwards. `experiments/04_live_fntable/ivrsystem_023.h`
carries the correct layout, lifted from OpenVR v2.12.14.

`IVRCompositor_029` is byte-identical between master and v2.12.14, so the
compositor can keep using `openvr_capi.h`.

---

## Corrections to the original research

Two items from the pre-work research did not survive contact with a compiler.
Both are recorded loudly because acting on the original version would waste time.

### A. The `GetHiddenAreaMesh` struct-return argument is not what it claimed

The premise was: mingw returns an 8-byte struct in `EAX:EDX` with `ret 4`, MSVC
uses a hidden pointer with `ret 8`, `IVRSystem::GetHiddenAreaMesh` has that shape,
and the C API's `__stdcall` function pointers dissolve the problem.

Disassembling mingw's actual code generation (`experiments/01_openvr_capi/RESULTS.md`)
shows mingw emits **identical** return-value handling for `__stdcall` and
`__thiscall` function pointers returning an 8-byte struct: result in `EAX:EDX`, no
hidden pointer, callee pops only the explicit stack arguments.

So whatever the MSVC-side truth is, switching from the C++ vtable to the C
`FnTable` does **not** change the return convention — the C API declares
`GetHiddenAreaMesh` as returning `struct HiddenAreaMesh_t` **by value** too. If a
divergence exists, it exists in both APIs equally.

What could not be checked: MSVC's side, because there is no MSVC toolchain on this
machine. The claim "MSVC uses a hidden pointer with `ret 8`" is **unverified here**.

The decision to use the C API still stands on its other merits (Decision 8).

**Update from Exp. 4 — measured against the real runtime.** By-value struct
returns through the `FnTable` are fine in general: `GetProjectionMatrix`
(64-byte) and `GetEyeToHeadTransform` (48-byte) both returned correct data from
a 32-bit mingw DLL. `GetHiddenAreaMesh` specifically **kills the process**, but
the fault is inside *Proton's own wow64 thunk* —
`IVRSystem_IVRSystem_023_GetHiddenAreaMesh failed, status 0xc0000005`, asserted
by Proton's generated wrapper with our frame still intact — and not a
mingw-vs-MSVC divergence. `GetHiddenAreaMesh` is the one OpenVR method returning
an 8-byte struct *containing a pointer* by value, which is exactly the shape
whose wow64 conversion Proton mishandles. Treat the hidden-area mesh as
**unavailable under new-WoW64** until Proton is fixed; the mingw/MSVC question
remains untested because it is never reached.

### B. Proton 10.0-4b installs `openvr_api_dxvk.dll` into the wrong directories

This is a genuine bug in the shipped `proton` script, and it sits directly on this
project's critical path.

`proton` lines 1077–1079 (Proton 10.0-4b):

```python
try_copy(g_proton.lib_dir + "wine/dxvk/x86_64-windows/openvr_api_dxvk.dll", "drive_c/windows/syswow64", ...)
try_copy(g_proton.lib_dir + "wine/dxvk/i386-windows/openvr_api_dxvk.dll",   "drive_c/windows/system32", ...)
```

`syswow64` holds 32-bit DLLs and `system32` holds 64-bit DLLs, so these are
swapped. The neighbouring DXVK copies at lines 1114–1116 get the same mapping
right, and Proton Experimental (11.x) is correct via `arch_pe_dir()`.

Verified on four real Proton 10.0-4b prefixes on this machine — every one has the
32-bit DLL in `system32` and the 64-bit DLL in `syswow64`.

Re-checked later across **all 23** prefixes on this machine that contain an
`openvr_api_dxvk.dll`. Five are swapped: the four Proton 10 ones above plus a
`GE-Proton10-15` prefix. Every Proton 7/8/9/11 prefix is correct. Note that some
prefixes whose `version` file now reads `10.1000-200` are *correct* — the
`version` stamp tracks the Proton currently running the prefix, while the DLL
copy happened at prefix creation, so **the stamp is not a reliable predictor.
Check the actual files** with `file drive_c/windows/syswow64/openvr_api_dxvk.dll`
before assuming either way.

Why it matters here: DXVK's VR submit path loads `openvr_api_dxvk.dll`, and in a
32-bit process that name resolves from `syswow64` — which under Proton 10.0-4b
contains the **64-bit** build, so the load fails.

Workarounds: swap the two files in the prefix after it is created, ship a correct
`openvr_api_dxvk.dll` next to `BlackOps.exe`, or use Proton Experimental.

**Re-confirmed in Exp. 4** on a freshly created Proton 10.0-4b prefix:
`system32` got the 631960-byte i386 build and `syswow64` the 836760-byte x86_64
build. Exp. 4 does not depend on it — it loads Valve's own 32-bit
`openvr_api.dll` from beside the exe — but a DXVK compositor submit will.

---

## Environment this was verified on

| | |
|---|---|
| OS | Ubuntu 24.04.4 LTS, kernel 6.8 |
| Cross compiler | **GCC 13-win32** (`Thread model: win32`), `i686-w64-mingw32`, `--disable-sjlj-exceptions --with-dwarf2` — the alternatives default |
| Also measured against | GCC 13-posix (`i686-w64-mingw32-gcc-posix`) — identical behaviour, different runtime DLL closure; see Decisions 4 and 7 |
| Wine used for experiments | wine-11.0 (winehq-stable, `/opt/wine-stable`; `/usr/bin/wine` is a symlink to it) |
| Wine prefix | fresh scratch `WINEARCH=win32` prefix, not `~/.wine` and not a Proton prefix |
| Proton installed | 10.0-4b (`proton-10.0-4b`) — meets the 10.0-2 minimum |
| OpenVR SDK | Valve `openvr` master (`IVRCompositor_029` / `IVRSystem_026`) — but see Decision 9: the chain caps at `IVRSystem_023` |
| VR runtime | xrizer `be664bb` via the WiVRn flatpak, registered in `~/.config/openvr/openvrpaths.vrpath` |
| OpenXR runtime | Monado 21.0.0 (`monado-service`), **Simulated HMD** — no VR hardware |

Experiments 0–3 ran on wine-11.0 in classic WoW64 (both wine-11.0 and Proton
10.0-4b ship a `lib/wine/i386-unix/`, so both *can* do classic WoW64); see
`experiments/03_winedbg/RESULTS.md`. Experiment 4 deliberately runs on Proton's
**new-WoW64** (`PROTON_USE_WOW64=1`, `files/bin-wow64/`) — see Decision 9, which
is the single most important configuration decision in this repository.

---

## Layout

```
Makefile                    top-level build (see Decision 1)
dinput8.def                 undecorated export names (Decision 3)
src/
  dllmain.c                 DllMain: banner, VEH, MinHook init, ASI scan
  proxy.c/.h                forwarding to the real dinput8.dll
  asi_loader.c/.h           loads *.asi beside the module
  veh.c/.h                  vectored exception handler (Decision 6)
  log.c/.h                  stderr logging -> ~/steam-42700.log
third_party/
  minhook/                  vendored, commit d94c64d
  openvr/                   headers + 32-bit openvr_api.dll
experiments/
  00_loader_smoke/          proves the loader loads, proxies and scans .asi
  01_openvr_capi/           OpenVR C API reachability + struct-return ABI
  02_cpp_exceptions/        exceptions across a DLL boundary, ±debugger
  03_winedbg/               winedbg and winedbg --gdb against 32-bit
  04_live_fntable/          THE GATING EXPERIMENT: a live FnTable, called
    run.sh                    one-command reproduction (stage, patch, build, run)
    ivrsystem_023.h           the FnTable layout the chain actually speaks
tools/
  patch-proton-wow64-vrclient.py   works around two Proton wow64 vrclient bugs
```

## Logging

Proton redirects the game's stderr into `~/steam-42700.log`, so
`fprintf(stderr, ...)` is a free, crash-surviving logging channel. `src/log.c`
writes there and also to `OutputDebugStringA` (visible with `WINEDEBUG=+debugstr`)
for cases where a launcher script swallows stderr.

Note that **winedbg detaches the inferior's stdio**, both in native and `--gdb`
mode — under a debugger, stderr does not reach the shell that launched winedbg.
Log to a file if you need output in that situation.
