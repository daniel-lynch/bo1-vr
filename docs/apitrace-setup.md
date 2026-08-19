# apitrace on this game, without touching the Steam install

> Lab notes from the dev machine — the paths below are where things were
> installed there; adjust to your own.

Installed 2026-08-03. Both builds live in `/home/dlynch/dev/reference/apitrace`
(outside this repo — they are third-party binaries, not ours to vendor):

* `apitrace-14.0-win32/` — the **32-bit** build. Required: `BlackOps.exe` is
  PE32 i386, so the 64-bit wrapper cannot load into it.
* `apitrace-14.0-Linux/bin/apitrace` — the host-side reader, for
  `apitrace dump` / `apitrace diff` on whatever the wrapper produces. Verified
  running (`apitrace 14.0`). No `apt install` was needed and none was done —
  `sudo` wants a password here.

The win32 wrapper is staged at
`…/compatdata/42700/pfx/drive_c/bo1vr/apitrace_d3d9.dll`, i.e. in the **prefix**,
beside the plugins.

## Why it cannot simply be dropped in as `d3d9.dll`

apitrace's D3D9 wrapper works by *being* `d3d9.dll` and loading the real one
underneath. Confirmed from its imports: it calls `GetSystemDirectoryA`, appends
`d3d9.dll`, and `LoadLibraryA`s that.

That gives exactly two placements, and both are ruled out:

| placement | result |
|---|---|
| `drive_c/windows/syswow64/d3d9.dll` | it would find **itself** via `GetSystemDirectory` — infinite recursion. And that path is DXVK's own `d3d9.dll`; shadowing it breaks every other app in the prefix. |
| next to `BlackOps.exe` | works, and is apitrace's documented usage — but that is inside the Steam install, which this project does not modify (Exp. 8). |

Wine's per-app `DllOverrides` registry key does not help: it selects *which
implementation* of a name to load, not *where else* to look. Exp. 9 §2 already
measured that a native override is not resolved from a directory outside the
install.

## The route that does work

**We already own `Direct3DCreate9`.** `gameframe.asi` hooks it (`my_create9`),
and it is loaded by the winmm shim before `d3d9.dll` is ever touched. So the
wrapper does not need to be found by the loader at all — we can call it
ourselves:

1. `LoadLibraryA("C:\\bo1vr\\apitrace_d3d9.dll")` inside `my_create9`.
2. `GetProcAddress(h, "Direct3DCreate9")` — the wrapper exports it.
3. Call **that** instead of the real one, and return its `IDirect3D9`.

Every subsequent call the game makes goes through apitrace's wrapped
interfaces, and apitrace's own `GetSystemDirectory` lookup finds DXVK's real
`d3d9.dll` — no recursion, because the system copy is DXVK's and ours is a
differently-named file in the prefix.

The trace destination is set with `APITRACE_TRACE_FILE`, which the plugin can
`SetEnvironmentVariableA` before step 1 — the same trick already used for
`DXVK_DEBUG=hang` (there is no launch environment under a Steam launch).

### Why this is worth the trouble

Trigger 1 (§12) is our mid-scene MSAA `StretchRect` making DXVK call
`endCurrentPass(true)` and tear down the game's live render pass. Every claim
in that chain is currently read out of DXVK's *source*, not observed. A D3D9
call stream would show the actual ordering around the freeze, and `apitrace
diff` between a frozen and a surviving run would name the divergence directly.

### Status

Staged and designed, **not yet wired** — it needs the `my_create9` change plus
one game launch to validate, and untested instruments are how several runs in
this project were wasted. Queued with the `r_allow_intz` trial, which needs
launches anyway.

### The cheaper alternative, if this proves fiddly

DXVK is configured entirely from the prefix and from environment variables the
plugin can set before `d3d9.dll` loads, so a **Vulkan capture layer** is
reachable with no wrapper at all. It sits one level below D3D9, but that is
arguably the better level here: `vkCmdEndRenderPass` arriving mid-scene *is*
trigger 1's mechanism, so the thing we want to observe is directly visible
there.
