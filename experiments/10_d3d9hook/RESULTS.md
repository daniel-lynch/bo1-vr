# Experiment 10 — the game's own D3D9 device, and its frames

**Question.** Exp. 5 put a texture of *our* making in front of the compositor.
Can we reach the one the **game** draws?

**Yes.** Inside the real Steam-launched `BlackOps.exe`, with nothing written to
the game install:

```
[d3d9hook] armed: d3d9.dll=7A440000 Direct3DCreate9=7A45E7A0
[d3d9hook] Direct3DCreate9(32) -> 001E0798
[d3d9hook] IDirect3D9 vtable 7AA64590  CreateDevice=7A505E20
[d3d9hook] CreateDevice hr=0x00000000 adapter=2 type=1 flags=0x40 hwnd=0001009A
[d3d9hook]   requested 2560x1440 fmt=A8R8G8B8 windowed=0 backbuffers=1
[d3d9hook] device 0A353240 vtable 7AA636D0  Reset=7A4DBEF0 Present=7A4DC5D0 EndScene=7A4DC990
[d3d9hook] swapchain 0A4065A8 vtable 7AA646AC  Present=7A50EB00
[d3d9hook] backbuffer 2560x1440 fmt=A8R8G8B8(21) mult=4 usage=0x1 pool=0
[d3d9hook] SwapChain::Present #3000 sc=0A4065A8 wnd=00000000
```

**3,000 frames in ~50 s (~60 fps), steady.** Build with `make`, install with
`make install` (drops into `C:\bo1vr` inside the prefix — Exp. 9).

---

## 1. The finding that matters: the game does not call `IDirect3DDevice9::Present`

Run 1 hooked `Device::Present` (vtable slot 17) **successfully** — the log says
`device hooks live` — and the counter never moved in 50 s, while the device had
been created 2560x1440 fullscreen and the game was on screen. Hooking the
obvious entry point and getting silence is not evidence the hook failed; here it
was evidence about the *caller*.

The game presents through **`IDirect3DSwapChain9::Present`, vtable slot 3**
(IUnknown's three, then `Present`). `Device::Present` is in truth a convenience
wrapper around exactly that, so a caller that skips the wrapper is invisible to
a hook on it. `EndScene` (slot 42) was hooked at the same time purely as a
control — if frames were being drawn at all it would fire regardless of which
present path was right — and it did.

**Anything downstream must hook the swap chain, not the device.**

## 2. What the back buffer actually is

| Property | Value | Consequence |
|---|---|---|
| Size | 2560x1440 | Matches `config.cfg`'s `r_mode`. Per-eye targets do **not** have to be this size. |
| Format | `D3DFMT_A8R8G8B8` (21) | Straightforward for interop; no 10-bit or float surprise. |
| `MultiSampleType` | **4** | **4x MSAA.** A multisampled surface cannot be handed to the compositor or copied with `GetRenderTargetData`; it needs a `StretchRect` resolve into a plain render target first. This is the single most important constraint the experiment turned up. |
| `Usage` | `0x1` (`RENDERTARGET`) | As expected. |
| `Pool` | `0` (`D3DPOOL_DEFAULT`) | Destroyed by `Reset`; hence the `Reset` hook, so the lifetime of anything we allocate is visible from the start rather than a surprise at the first alt-tab. |

## 3. Two other things worth recording

* **`Direct3DCreate9` is called twice** (`001E0798`, then `001EAC60`). Only the
  first is used to hook `CreateDevice`, which is correct and sufficient: the
  hook is on the *vtable slot*, which both objects share, so the second
  instance is covered without hooking anything twice.
* **`adapter=2`.** The game does not pick adapter 0. On this three-monitor
  machine that is the third adapter, and it is a plausible reason a whole-root
  X11 screenshot did not obviously contain the game window.
* `wnd` is `NULL` in every `Present` call — the swap chain uses its implicit
  window, so the HWND to correlate with is the one from `CreateDevice`
  (`0001009A`), not the present argument.

## 4. How the hooks are placed, and why not the usual way

The usual recipe creates a throwaway D3D9 device to learn the vtable layout.
That needs an HWND, and at `DLL_PROCESS_ATTACH` there is no window — the game
has not run a line of its own code. So the chain starts at the one thing that
exists before any object does, the exported `Direct3DCreate9`, and each
subsequent hook is installed the first moment an object of that type exists:

```
Direct3DCreate9  ->  IDirect3D9 vtable[16] = CreateDevice
CreateDevice     ->  IDirect3DDevice9 vtable[17]=Present, [16]=Reset, [42]=EndScene
                 ->  GetSwapChain(0) -> IDirect3DSwapChain9 vtable[3] = Present
```

`d3d9.dll` is a static import of `BlackOps.exe`, so it is already mapped when
any `DllMain` runs; `GetModuleHandleA` suffices and no nested `LoadLibrary` is
needed. The work is done on the calling thread rather than a spawned one — the
device is created from the game's main thread long after our `DllMain` returns,
so there is no race to lose, and spawning a thread from `DllMain` to call back
into the loader lock is the deadlock `src/dllmain.c` warns about.

**No `__try`/`__except`** — README Decision 6: 32-bit mingw has no SEH and this
toolchain's DWARF-2 unwinder cannot walk the CFI-less MSVC frames of
`BlackOps.exe` anyway. The loader's `AddVectoredExceptionHandler` is the
project-wide answer, and the defence in the callbacks is to touch nothing that
can fault: every pointer used is one D3D9 handed us, null-checked before use.

## 5. Next

The remaining half of BAC-280 is to get that back buffer to the compositor:
`StretchRect` the MSAA back buffer into a plain `D3DPOOL_DEFAULT` render target,
then feed it through the Exp. 5 D3D9->Vulkan interop and `Submit` it. Exp. 6's
constraint applies — xrizer ignores `pBounds`, so one render target **per eye**,
never one texture with bounds.
