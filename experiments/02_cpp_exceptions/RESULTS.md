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
--target=i686-w64-mingw32 --disable-sjlj-exceptions --with-dwarf2
gcc version 13-posix (GCC)
```

DWARF-2 unwinding. Not SEH, not SJLJ — as expected for 32-bit mingw.

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
| exit code | **3** | 0 | 0 (dies at D) |

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
for an MSVC-built game frame:

```
$ i686-w64-mingw32-gcc -m32 -O1 -fno-asynchronous-unwind-tables -fno-unwind-tables -c cframe.c
$ i686-w64-mingw32-objdump -h out-noeh/cframe.o | grep eh_frame   # (no output)
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
