# Experiment 0 — loader smoke test

Not one of the three assigned experiments. Added because the scaffold is only
worth anything if it actually loads, and running it immediately found a bug and
confirmed two of the established facts by direct observation.

`smoke.exe` loads `dinput8.dll`, calls `DirectInput8Create` through it, and
`dummy.asi` proves the plugin scan works.

## Result — the loader works end to end

```
$ make run
cd out && WINEDLLOVERRIDES="dinput8=n,b" wine smoke.exe
```

```
smoke: loading ./dinput8.dll
[bo1-vr] =====================================================
[bo1-vr] bo1-vr ASI loader (dinput8.dll proxy)
[bo1-vr]   built Aug  1 2026 16:02:43 with GCC 13-posix
[bo1-vr]   module: Z:\...\smoke\dinput8.dll
[bo1-vr]   pid=32
[bo1-vr] =====================================================
[bo1-vr] VEH: handler installed
[bo1-vr] MinHook: initialised (MH_OK)
[bo1-vr] asi: scanning Z:\...\smoke\*.asi
[dummy.asi] DllMain PROCESS_ATTACH, base=77b30000
[bo1-vr] asi: loaded dummy.asi at 77b30000
[bo1-vr] asi: 1 plugin(s) loaded
[bo1-vr] loader: init complete
smoke: DirectInput8Create export = 77b81a80
[bo1-vr] proxy: real dinput8 at 77a20000 (DirectInput8Create=77a21880)
smoke: DirectInput8Create -> hr=0x00000000 iface=00248f08
```

Confirms: stderr logging, VEH installation, **MinHook initialising `MH_OK`** (so
the vendored MinGW-makefile build genuinely works, not just links), `.asi`
discovery and loading, lazy resolution of the real `dinput8.dll`, and successful
forwarding of `DirectInput8Create` (`S_OK` with a live interface pointer).

---

## Finding 1 — bare `n` fails exactly as predicted

```
$ make run-bare-n
cd out && WINEDLLOVERRIDES="dinput8=n" wine smoke.exe
```

```
[bo1-vr][error] proxy: LoadLibraryA("C:\windows\system32\dinput8.dll") failed, err=126
smoke: DirectInput8Create -> hr=0x80004005 iface=00000000
```

Error 126 is `ERROR_MOD_NOT_FOUND`. The override applies to the proxy's *own*
`LoadLibrary` of the system DLL, so with no builtin fallback the real dinput8 can
never be found and `DirectInput8Create` returns `E_FAIL` — i.e. game input dies,
with an error that points at the wrong thing. `n,b` works. **Established fact
confirmed by direct observation.**

## Finding 2 — under plain Wine the override is mandatory

The very first run, with no `WINEDLLOVERRIDES` at all, printed no banner and
`DirectInput8Create` succeeded — because Wine silently loaded its **builtin**
dinput8 instead of ours, despite ours sitting in the application directory and
being loaded by explicit relative path.

This is precisely why Proton 10/11 list `dinput8.dll` as prefer-native, and it
confirms Decision 2 from the opposite direction: under Proton no configuration is
needed, but any testing done under plain Wine **must** set
`WINEDLLOVERRIDES="dinput8=n,b"` or it will silently test nothing.

## Finding 3 — a real bug in our own VEH, caught on first run

The first successful load flooded stderr without limit:

```
[bo1-vr] VEH: handler installed
[bo1-vr][error] VEH: exception 0x40010006 at 7b6188f7
[bo1-vr][error] VEH: exception 0x40010006 at 7b6188f7
...  (unbounded)
```

`0x40010006` is `DBG_PRINTEXCEPTION_C`, raised by `OutputDebugStringA` — which
`src/log.c` calls on every log line. So: log → `OutputDebugStringA` → exception →
VEH → log → forever. The handler fed itself.

Fixed in `src/veh.c` by returning `EXCEPTION_CONTINUE_SEARCH` for
`DBG_PRINTEXCEPTION_C` / `DBG_PRINTEXCEPTION_WIDE_C` and for any code with
informational severity (`(code & 0xC0000000) == 0x40000000`) **before** touching
the logger, plus a per-thread reentrancy guard for everything else.

Worth recording because it generalises: a VEH that logs is a loop waiting to
happen, and any logging that itself raises an exception will find it. This would
have been considerably harder to diagnose inside the game.
