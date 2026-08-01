# Experiment 3 — winedbg and `winedbg --gdb` against a 32-bit target

**Question.** Do real breakpoints and symbol resolution work for a 32-bit target
under new-WoW64, or is the realistic answer "give up and use stderr logging"?
This was flagged as the experiment most likely to disappoint.

**Verdict: it works, and it works well.** Source-level breakpoints, deferred
breakpoints in dynamically loaded DLLs, full backtraces across the DLL/EXE
boundary, named locals and parameters, single-stepping, `finish` with return
values, and faulting with an exact source line — all functional, in **both**
native winedbg and `winedbg --gdb`.

There is exactly one gotcha, and it is a one-line fix.

---

## Important scoping caveat, read this first

The premise of the question was new-WoW64. **The Wine and Proton builds on this
machine are classic WoW64, not new-WoW64.**

```
$ ls -d /opt/wine-stable/lib/wine/*/
i386-unix/  i386-windows/  x86_64-unix/  x86_64-windows/

$ ls -d ".../Proton 10.0/files/lib/wine/"*/
dxvk/  i386-unix/  i386-windows/  icu/  nvapi/  vkd3d-proton/  x86_64-unix/  x86_64-windows/
```

The presence of `i386-unix/` is the marker: 32-bit PE code is backed by 32-bit
Unix libraries. A new-WoW64 build has `i386-windows/` but **no** `i386-unix/`, and
runs 32-bit code entirely through 64-bit host thunks.

So the concern about winedbg hiding modules whose machine differs from the
debuggee's — which is a new-WoW64 problem — **did not apply to this
configuration, and remains untested.** The good news is that the configuration
that actually matters for this project, Proton 10.0-4b, is the classic one, where
everything below works.

Environment: wine-11.0 (winehq-stable), `WINEARCH=win32` prefix, target built by
`i686-w64-mingw32-gcc` 13 with `-O0 -gdwarf-4`, not stripped.

> `winedbg target.exe` fails with `Couldn't start process`. It needs a path
> separator: use `winedbg ./target.exe`. Also, Wine 11's winedbg no longer
> accepts `--no-start` or `--port`.

---

## Native winedbg — works fully

```
$ printf 'break main\ncont\nbt\ninfo locals\nbreak tgtlib_leaf\ncont\nbt\ninfo locals\ncont\nquit\n' \
    | winedbg ./target.exe
```

```
Breakpoint 1 at 0x00401549 main [.../target.c:28] in target
Stopped on breakpoint 1 at 0x00401549 main [.../target.c:28] in target

Backtrace:
=>0 0x00401549 main(argc=..., argv=...) [.../target.c:28] in target (0x0063ff48)
  1 0x7bb4fb58 in kernel32 (+0xfb58) (0x0063ff68)
  2 0x7bcde1f7 in ntdll (+0xe1f7) (0x0063ff80)
  3 0x7bd13b95 in ntdll (+0x43b95) (0x0063ffec)

No symbols found for tgtlib_leaf
Unable to add breakpoint, will check again when a new DLL is loaded
Breakpoint 2 at 0x78a414d0 tgtlib_leaf [.../tgtlib.c:12] in tgtlib
Stopped on breakpoint 2 at 0x78a414d0 tgtlib_leaf [.../tgtlib.c:12] in tgtlib

Backtrace:
=>0 0x78a414d0 tgtlib_leaf(a=..., b=0x2a) [.../tgtlib.c:12] in tgtlib (0x0063fe48)
  1 0x00401526 app_level_two+0x19(f=78A414EB, v=0x15) [.../target.c:19] in target (0x0063fe78)
  2 0x00401547 app_level_one+0x1f(f=78A414EB) [.../target.c:25] in target (0x0063fea8)
  3 0x00401650 main+0x107(...) [.../target.c:44] in target (0x0063fef8)
  4 0x004012de in target (+0x12de) (0x0063ff48)
  5 0x7bb4fb58 in kernel32 (+0xfb58) (0x0063ff68)
  ...
0x78a414d0 tgtlib_leaf: (0063fe48)
	int a=... (parameter [EBP+4])
	int b=0x2a (parameter [EBP+8])
	int product=0 (local [EBP+4294967288])
```

Native winedbg **defers breakpoints automatically** ("will check again when a new
DLL is loaded") and resolves them when the DLL loads. Backtraces cross the
DLL → EXE boundary with file and line, and Wine's own `kernel32`/`ntdll` frames
appear as addresses (no debug info shipped) without truncating the trace.

---

## `winedbg --gdb` — works, with one gotcha

gdb 15.1 (`x86_64-linux-gnu` build) drives the i386 target fine.

### The gotcha

A breakpoint set on a symbol in a not-yet-loaded DLL is simply rejected, and
unlike native winedbg, gdb does **not** defer it by default:

```
Wine-gdb> break tgtlib_leaf
Function "tgtlib_leaf" not defined.
Wine-gdb> continue
...
Wine-gdb> bt
No stack.
```

That single behaviour is what makes gdb mode look broken on first contact — the
loader and every `.asi` are dynamically loaded, so *every* breakpoint that matters
falls into this case.

### The fix

```
set breakpoint pending on
```

With that, everything works:

```
$ printf 'set pagination off\nset confirm off\nset breakpoint pending on\nbreak tgtlib_leaf\ncontinue\nbt\ninfo args\nnext\nnext\ninfo locals\nprint product\nfinish\ncontinue\nquit\n' \
    | winedbg --gdb ./target.exe
```

```
Function "tgtlib_leaf" not defined.
Breakpoint 1 (tgtlib_leaf) pending.
Continuing.
0140:0144: loads DLL Z:\...\tgtlib.dll @78C50000 (64512<883>)

Breakpoint 1, tgtlib_leaf (a=142, b=3) at tgtlib.c:13
13	    int product = a * b;

#0  tgtlib_leaf (a=142, b=3) at tgtlib.c:13
#1  0x78c5150d in tgtlib_middle (n=42) at tgtlib.c:21
#2  0x00401526 in app_level_two (f=0x78c514eb <tgtlib_middle>, v=21) at target.c:18
#3  0x00401547 in app_level_one (f=0x78c514eb <tgtlib_middle>) at target.c:24
#4  0x00401650 in main (argc=1, argv=0x982a98) at target.c:44

a = 142
b = 3
14	    volatile int sink = product;
15	    return sink;
product = 426
sink = 426
$1 = 426
Run till exit from #0  tgtlib_leaf (a=142, b=3) at tgtlib.c:15
tgtlib_middle (n=42) at tgtlib.c:22
Value returned is $2 = 426
```

Correct argument values, a full five-frame backtrace crossing DLL → EXE with
file:line, named locals with correct values, `next`, and `finish` with the return
value. Nothing is truncated at a module boundary.

### DLL symbols are loaded automatically

The `warning: Could not load shared library symbols for 3 libraries` that gdb
prints at startup refers to Wine's own builtins, not to our modules:

```
Wine-gdb> info sharedlibrary
From        To          Syms Read   Shared Object Library
0xf5fb8000  0xf607c6d8  Yes (*)     /opt/wine-stable/lib/wine/i386-unix/ntdll.so
0x00401000  0x00411444  Yes         .../out/target.exe
0x7bcd1000  0x7bd882a4  Yes         .../system32/ntdll.dll
0x7bb41000  0x7bba658c  Yes         .../system32/kernel32.dll
0x7b5d1000  0x7b872f80  Yes         .../system32/kernelbase.dll
0x7b201000  0x7b2b1004  Yes         .../system32/msvcrt.dll
0x78a81000  0x78a8b1e0  Yes         .../out/tgtlib.dll
(*): Shared library is missing debugging information.
```

`tgtlib.dll` — dynamically loaded via `LoadLibrary`, and relocated from its
preferred base `0x64940000` to `0x78a80000` — shows `Syms Read: Yes`. No
`add-symbol-file` is needed and no manual base-address arithmetic is required.

### Faults

```
$ ... | winedbg --gdb ./target.exe crash
```

```
Program received signal SIGSEGV, Segmentation fault.
0x78a8151f in tgtlib_crash_now () at tgtlib.c:29
29	    *p = 0x41414141;
#0  0x78a8151f in tgtlib_crash_now () at tgtlib.c:29
#1  0x004016e8 in main (argc=2, argv=0x982ab0) at target.c:51
eip            0x78a8151f          0x78a8151f <tgtlib_crash_now+16>
```

Exact faulting source line and a backtrace out of the DLL into the EXE.

---

## DWARF version — the flag works, and the CRT does not matter

`dist/dinput8.dll` contains a mix of DWARF versions:

```
$ i686-w64-mingw32-objdump --dwarf=info dist/dinput8.dll | grep -E "Compilation Unit|Version:"
  Compilation Unit @ offset 0:       Version: 5   <- crtdll.c   (Ubuntu's prebuilt CRT)
  Compilation Unit @ offset 0x15aa:  Version: 4   <- src/asi_loader.c
  Compilation Unit @ offset 0x1e8b:  Version: 4   <- src/dllmain.c
  Compilation Unit @ offset 0x25d6:  Version: 4   <- src/log.c
  Compilation Unit @ offset 0x2b70:  Version: 4   <- src/proxy.c
  Compilation Unit @ offset 0x3989:  Version: 4   <- src/veh.c
  Compilation Unit @ offset 0x456d:  Version: 4   <- minhook src/hook.c
  ...
  Compilation Unit @ offset 0x9522:  Version: 5   <- gccmain.c   (prebuilt CRT)
  Compilation Unit @ offset 0x9bbf:  Version: 5   <- natstart.c  (prebuilt CRT)
```

Every DWARF-5 CU comes from `/usr/src/mingw-w64-11.0.1-3build1/mingw-w64-crt/...`
— Ubuntu's precompiled CRT objects. `-gdwarf-4` governs only the CUs we compile,
and no flag can change the shipped ones short of rebuilding mingw-w64-crt.

**This does not break debugging.** Wine's dbghelp skips CUs above its supported
version individually rather than abandoning the module, and every experiment above
resolved our own symbols and line numbers correctly with those DWARF-5 CRT CUs
present. `make verify` enforces DWARF ≤ 4 for first-party and MinHook CUs and only
*reports* the CRT ones.

The underlying rule still stands and still matters: had our own CUs been DWARF 5,
Wine would have found no symbols at all, silently.

---

## Conclusions

1. **Use a debugger. It works.** The pessimistic expectation is disproven for
   Proton 10.0-4b's WoW64 configuration.
2. **`set breakpoint pending on` is mandatory** in `--gdb` mode. Put it in a
   `.gdbinit`. Native winedbg defers automatically and needs nothing.
3. Native winedbg is the lower-friction option for quick work — no gdb startup
   noise, automatic deferral, and readable backtraces. gdb mode is better for
   stepping, expression evaluation and `finish`.
4. **stderr logging is still worth keeping**, not as a substitute but because
   winedbg detaches the inferior's stdio in both modes, so a debugged run loses
   the log unless it also goes to a file.

## Not tested / still unknown

- **New-WoW64.** Neither Wine build here uses it. If Proton later drops
  `i386-unix/`, the module-hiding concern becomes live again and this experiment
  must be re-run.
- **Under Proton itself.** These ran on system wine-11.0. Proton 10.0-4b is
  wine-10.0 and is classic WoW64 like the tested build, but attaching winedbg to a
  process inside the Steam runtime container was not attempted.
- **Against `BlackOps.exe`.** The game is not installed on this machine (see the
  report). Debugging a CEG-protected, packed MSVC binary with no symbols is a
  materially different problem from debugging our own DWARF-annotated modules —
  everything above concerns *our* code, which is what we need to step through.
