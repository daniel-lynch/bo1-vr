# Experiment 2 — C++ exceptions across a DLL boundary, GNU ABI, x86, under Wine

**Question.** There is a report that with an **MSVC-ABI** build, any thrown
exception crashes the game while a debugger is attached, though the same build
runs fine undebugged. How does a **mingw/GNU-ABI** build behave?

**Verdict: the debugger is irrelevant to the GNU ABI build.** The reported
MSVC-ABI failure does **not** reproduce. Behaviour is byte-for-byte identical with
and without a debugger attached (`IsDebuggerPresent()` confirmed `1`).

Two *other* things determine success, and one of them was not in the original
research.

---

## Toolchain

```
$ i686-w64-mingw32-g++ -v
--target=i686-w64-mingw32 --enable-threads=win32 --program-suffix=-win32
--disable-sjlj-exceptions --with-dwarf2
Thread model: win32
gcc version 13-win32 (GCC)
```

DWARF-2 unwinding. Not SEH, not SJLJ — as expected for 32-bit mingw.

> **Re-run history.** This experiment was first measured under **GCC 13-posix**
> and later re-run in full under **GCC 13-win32** (the `update-alternatives`
> default on this machine) in a fresh scratch `WINEARCH=win32` prefix on
> wine-11.0. **Every pass/terminate result below is identical under both
> threading models.** The one thing that changed is which runtime DLLs have to
> be shipped — see "Runtime DLL closure" below.

## Cases

| | |
|---|---|
| A | throw and catch entirely inside the DLL |
| B | throw `std::runtime_error` in DLL, catch in EXE |
| C | throw a DLL-private type, catch as `std::exception&` in EXE (cross-module RTTI) |
| D | EXE callback throws; unwinds through a plain C frame inside the DLL |

Three link variants:

| variant | libgcc/libstdc++ | C frame |
|---|---|---|
| `out-static` | `-static-libgcc -static-libstdc++ -static` | with `.eh_frame` |
| `out-shared` | shared DLLs | with `.eh_frame` |
| `out-noeh`   | shared DLLs | `-fno-asynchronous-unwind-tables -fno-unwind-tables` |

## Results

```
$ WINEPREFIX=... WINEARCH=win32 wine host2.exe      # in each variant dir
```

| case | static | shared | shared, C frame w/o unwind info |
|---|---|---|---|
| A throw/catch inside DLL | PASS | PASS | PASS |
| B cross-module catch | **terminate** | PASS | PASS |
| C cross-module RTTI | (not reached) | PASS | PASS |
| D through a C frame | (not reached) | PASS | **terminate** |
| exit code | **3** | 0 | **3** (dies at D) |

*(The `noeh` exit code was previously recorded as 0 in this table. It is 3 —
`std::terminate` gives the same exit code whichever case triggers it. Corrected
after re-measurement.)*

### Static variant, case B

```
[B] throw std::runtime_error in DLL, catch in EXE
  [dll] about to throw std::runtime_error
terminate called after throwing an instance of 'std::runtime_error'
  what():  thrown from throwlib.dll
=== EXIT=3 ===
```

### Shared variant — all four pass

```
[A] ... returned 1 -> SURVIVED
[B] caught std::runtime_error: thrown from throwlib.dll -> CROSS-MODULE CATCH WORKS
[C] caught as std::exception&: custom type from throwlib.dll -> CROSS-MODULE RTTI WORKS
[D] caught: from exe callback -> UNWOUND THROUGH THE C FRAME
=== EXPERIMENT 2 END (process survived all cases) ===
```

### The C frame matters — case D isolated

Case D initially passed, which was suspicious. Checking why:

```
$ i686-w64-mingw32-objdump -h out-shared/cframe.o | grep eh_frame
 10 .eh_frame     00000030  ...
```

GCC emits `.eh_frame` for plain C on i686 mingw **by default**, so case D was only
proving that unwinding works through a *GCC-compiled* C frame — not through an
MSVC frame, which is the situation that actually matters.

Rebuilt that one object with unwind tables suppressed, which is a far better proxy
for an MSVC-built game frame. This is now the `noeh` target in the Makefile
(originally it was built by hand, which made it unreproducible); the rule asserts
the section is really gone rather than trusting the flags:

```
$ make noeh
i686-w64-mingw32-gcc -m32 -O1 -gdwarf-4 -Wall -Wno-format-zero-length \
    -fno-asynchronous-unwind-tables -fno-unwind-tables -c -o out-noeh/cframe.o cframe.c
  OK: out-noeh/cframe.o has no .eh_frame
```

Result:

```
[D] EXE callback throws; unwinds through a plain C frame inside the DLL
  [exe] callback throwing std::runtime_error
terminate called after throwing an instance of 'std::runtime_error'
  what():  from exe callback
```

**Confirmed.** An exception that must unwind through a frame lacking CFI calls
`std::terminate`. This is exactly the hook-callback situation — the frame above a
hook is MSVC-built game code with no GCC unwind info — and it validates the
"never let an exception escape a hook callback" rule as a hard requirement rather
than a stylistic one.

## Runtime DLL closure — this DID change with the threading model

The original notes said a shared C++ plugin must ship **three** DLLs. Under
**GCC 13-win32 it needs only two.** `libwinpthread-1.dll` is not in the closure
at all, because the win32-threading libgcc and libstdc++ do not use winpthreads.

```
$ i686-w64-mingw32-objdump -p out-shared/host2.exe out-shared/throwlib.dll | grep "DLL Name:" | sort -u
	DLL Name: KERNEL32.dll
	DLL Name: libgcc_s_dw2-1.dll
	DLL Name: libstdc++-6.dll
	DLL Name: msvcrt.dll

$ i686-w64-mingw32-objdump -p out-shared/libstdc++-6.dll | grep "DLL Name:" | sort -u
	DLL Name: KERNEL32.dll
	DLL Name: libgcc_s_dw2-1.dll
	DLL Name: msvcrt.dll

$ i686-w64-mingw32-objdump -p out-shared/libgcc_s_dw2-1.dll | grep "DLL Name:" | sort -u
	DLL Name: KERNEL32.dll
	DLL Name: msvcrt.dll
```

Side by side with the 13-posix runtime on the same machine:

```
$ i686-w64-mingw32-objdump -p /usr/lib/gcc/i686-w64-mingw32/13-posix/libstdc++-6.dll | grep "DLL Name:"
	DLL Name: KERNEL32.dll
	DLL Name: libgcc_s_dw2-1.dll
	DLL Name: libwinpthread-1.dll        <- present under posix
	DLL Name: msvcrt.dll

$ i686-w64-mingw32-objdump -p /usr/lib/gcc/i686-w64-mingw32/13-win32/libstdc++-6.dll | grep "DLL Name:"
	DLL Name: KERNEL32.dll
	DLL Name: libgcc_s_dw2-1.dll
	DLL Name: msvcrt.dll                 <- no winpthread under win32
```

| | GCC 13-win32 | GCC 13-posix |
|---|---|---|
| ship beside the game | `libgcc_s_dw2-1.dll`, `libstdc++-6.dll` | those two **plus** `libwinpthread-1.dll` |

Confirmed by deletion, not just by reading import tables. Removing
`libwinpthread-1.dll` from `out-shared/` under 13-win32 changes nothing — all
four cases still pass, exit 0. Removing `libgcc_s_dw2-1.dll` kills the process
before `main`:

```
$ WINEDEBUG=err+module wine host2.exe
0024:err:module:import_dll Library libgcc_s_dw2-1.dll (which is needed by L"...\out-shared\host2.exe") not found
0024:err:module:import_dll Library libgcc_s_dw2-1.dll (which is needed by L"...\out-shared\libstdc++-6.dll") not found
0024:err:module:import_dll Library libstdc++-6.dll (which is needed by L"...\out-shared\host2.exe") not found
0024:err:module:loader_init Importing dlls for L"...\out-shared\host2.exe" failed, status c0000135
```

(exit 53, no program output at all — the failure happens in the PE loader.)

The Makefile's `.runtime` rule no longer hardcodes the three names. It reads the
import tables of the built binaries and follows them transitively, so it copies
two DLLs under 13-win32 and three under 13-posix without needing to know which
compiler ran.

## With a debugger attached

Run under both `winedbg` and `winedbg --gdb`. `IsDebuggerPresent()` returns `1` in
both the EXE and the DLL, confirming the debugger really is attached.

```
$ printf 'set pagination off\nset confirm off\ncontinue\nquit\n' | winedbg --gdb ./host2.exe
```

Shared variant, debugger attached:

```
IsDebuggerPresent(): exe=1 dll=1
[A] returned 1 -> SURVIVED
[B] caught std::runtime_error: thrown from throwlib.dll -> CROSS-MODULE CATCH WORKS
[C] caught as std::exception&: custom type from throwlib.dll -> CROSS-MODULE RTTI WORKS
[D] caught: from exe callback -> UNWOUND THROUGH THE C FRAME
=== EXPERIMENT 2 END (process survived all cases) ===
```

Static variant, debugger attached: `exit process (3)`, failing at case B — the
same failure and the same exit code as undebugged.

| | no debugger | native winedbg | winedbg --gdb |
|---|---|---|---|
| shared | all pass, exit 0 | all pass, exit 0 | all pass, exit 0 |
| static | fails at B, exit 3 | fails at B | fails at B, exit 3 |
| noeh   | fails at D, exit 3 | fails at D | fails at D, exit 3 |

All nine cells re-measured under GCC 13-win32; all nine match the original
13-posix measurement.

**The debugger changes nothing.** The MSVC-ABI report does not carry over to the
GNU ABI.

> Practical note: winedbg detaches the inferior's stdio in both modes, so the
> program's stderr does not reach the launching shell. `host2.cpp` therefore also
> logs to a file (`$EXP2_LOG`, default `exp2.log`); the transcripts above come
> from that file.

---

## Conclusions

1. **The debugger-attached crash does not affect mingw/GNU-ABI builds.** No
   special handling needed for debugging sessions.
2. **C++ plugins must link libstdc++/libgcc dynamically** if they throw across a
   module boundary. Static linking gives each module its own unwinder and its own
   `type_info` copies, and cross-module catch degrades to `std::terminate`. This
   was not in the original research and would have been a confusing bug.
   The loader itself is pure C and stays `-static` deliberately — no runtime DLL
   dependencies.
   **Do not hardcode the DLL list**: it is two DLLs under GCC 13-win32 and three
   under GCC 13-posix. Derive it from `objdump -p`.
3. **An exception must never escape a hook callback** — confirmed by direct test,
   not merely by reasoning. Wrap every callback body in `try { } catch (...) { }`,
   or compile plugins `-fno-exceptions`, and use
   `AddVectoredExceptionHandler` (`src/veh.c`) for fault *observation*.

## Not tested

- Interaction with a genuine MSVC-built frame. The `-fno-unwind-tables` C frame is
  a close proxy — no CFI, same unwinder outcome — but it is still GCC-compiled
  code, and real MSVC frames additionally carry SEH scope tables that the DWARF-2
  unwinder ignores. The proxy fails in the expected direction, so this is a
  conservative result, but it is a proxy.
- Behaviour under Proton's Wine specifically. These ran on wine-11.0
  (winehq-stable). Proton 10.0-4b is wine-10.0; nothing here depends on a version
  difference, but it was not re-run there.
- `_CxxThrowException` / MSVC-ABI exceptions raised by the game itself passing
  through our frames.
