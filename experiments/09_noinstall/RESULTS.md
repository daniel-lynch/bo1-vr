# Experiment 9 — loading our code into the game without touching the install

**Question.** Exp. 8 §10 established that BO1 only runs when the Steam client
launches it. Steam starts the process itself, so we never get to set an
environment variable, and the exe must stay byte-identical to what Steam
installed. Can the Exp. 7 loader still get in?

**Answer: yes, with nothing written into the game install and no Steam launch
option.** Proven inside the real Steam-launched `BlackOps.exe`:

```
[winmm-shim] attach, pid=332
[winmm-shim] real winmm at 79D50000, 11/11 exports resolved
[winmm-shim] loaded ASI loader C:\windows\system32\bo1vr_loader.dll at 79CB0000
```

`find "steamapps/common/Call of Duty Black Ops" -newermt <before>` returns
**nothing**. The game ran 50 s and was ended by the harness, not by CEG: the
DRM `ExitProcess` stub of Exp. 8 §3 is not triggered by our presence.

Install and remove with `./install.sh` / `./install.sh remove`.

---

## 1. What changes, and where

Everything is inside `steamapps/compatdata/42700` — the Wine prefix, which
Proton owns and rebuilds from scratch if deleted.

| Change | Why |
|---|---|
| `syswow64/winmm.dll` — Proton's builtin symlink renamed to `winmm_real.dll`, our shim put in its place | The shim must be the `winmm.dll` that Wine's search order finds. The game's eleven imports still reach the real one, because `resolve_real_winmm` probes `winmm_real.dll` first. |
| `syswow64/bo1vr_loader.dll` — the ASI loader | A name no real system DLL uses. We do **not** shadow `dinput8.dll` here: a system directory is shared with everything else in the prefix, and booby-trapping a real DLL name is how you break unrelated software months later. |
| `user.reg`: `[Software\Wine\AppDefaults\BlackOps.exe\DllOverrides]` `"winmm"="native,builtin"` | Scoped to `BlackOps.exe` alone. **This is the load-bearing part** — it is why no `WINEDLLOVERRIDES` is needed, which is why no Steam launch option is needed, which is why this works when Steam starts the game and we cannot set an environment variable. |

Wine rewrote our key with its own timestamp during the run (`1785460000` ->
`863405750`), which is independent confirmation that it read it rather than
ignoring it.

## 2. The four alternatives, and why each is worse

| Approach | Verdict |
|---|---|
| `WINEPATH` -> a mod directory outside the install | **Does not work.** Measured: host exe importing `timeGetTime`, our shim in a separate directory, `WINEDLLOVERRIDES=winmm=n,b`, `WINEPATH` set — no shim, Wine used the builtin. Wine did not consult the Windows PATH for the native override. The identical run with the DLL copied beside the exe loaded it (11/11 exports), so the override worked and only the *location* failed. |
| `AppInit_DLLs` | **Not available.** No `AppInit` string in this Proton's `user32.dll` in any encoding, 32- or 64-bit. |
| Proton `user_settings.py` | Lives in the Proton directory (`g_proton.path("user_settings.py")`), so it would apply to **every** game using Proton Experimental. |
| Steam launch options (`WINEDLLOVERRIDES="winmm=n,b" %command%`) | Works, and is the conventional answer, but needs a Steam config write plus a client restart — and the client is usually mid-session. Kept as the documented fallback. |

## 3. Two changes to `winmm_shim.c`, both backward compatible

1. **`resolve_real_winmm` probes `<sysdir>\winmm_real.dll` first**, then falls
   back to `<sysdir>\winmm.dll`. In the Exp. 7 placement (beside the exe)
   `winmm_real.dll` does not exist and behaviour is unchanged. In this
   placement, reading `<sysdir>\winmm.dll` would be *us*, and the first
   forwarded call would recurse until the stack died.
2. **The loader is looked up as `bo1vr_loader.dll` first**, then `dinput8.dll`.
   Same reason as the table above.

Which placement is in use is now a property of the prefix, not of the binary.

## 4. A third change: the shim logs to a file

Under a real Steam launch neither of the shim's existing channels can be read —
there is no shell to inherit stderr, and `WINEDEBUG=+debugstr` cannot be set
without the launch option this experiment exists to avoid. `shimlog` now also
appends to `%TEMP%\bo1vr_shim.log`.

**Trap:** `GetTempPathA` in this prefix yields
`drive_c/users/steamuser/AppData/Local/Temp`, **not**
`drive_c/users/steamuser/Temp`. The first successful run was briefly written
off as a failure because only the latter was checked.

## 5. Known fragility

Proton rebuilds the prefix when `CURRENT_PREFIX_VERSION` changes (currently
`11.0-100`), which will restore the builtin `winmm.dll` symlink and drop the
registry key. That is a re-run of `install.sh`, not a redesign — and it is
strictly preferable to the alternative, because a wiped prefix is Proton doing
its job whereas a modified game install is a file Steam may or may not notice.

`install.sh remove` restores the symlink and deletes the key.

## 6. Files

```
install.sh    install/remove; refuses to run if syswow64/winmm.dll is not the
              symlink it expects, so it cannot clobber a real file
out/run1/     the Steam launch that proved it
```

The shim and loader are built by `make -C ../07_ingame` and are **not** copied
into this directory; there is exactly one build of each.
