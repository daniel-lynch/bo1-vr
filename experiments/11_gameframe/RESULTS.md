# Experiment 11 — game frames on the compositor, and the one thing Steam has to be told

**Question.** Exp. 10 got hold of the game's swap chain. Can its frames reach
the headset?

**Answer: yes. Confirmed in the real Steam-launched game**, with the launch
option of §2/§2b set:

```
[gameframe] OpenVR up: compositor=143B20E8 system=047D2608 per-eye 896x1007
[gameframe] vk: instance=047D0A88 phys=047D0AE0 device=047CF970 queue=047CF9B0 family=0
[gameframe] eye 0: VkImage=0x00007951d0dcd790 layout=1 896x1007 fmt=44
[gameframe] PIPE LIVE: game frames -> compositor
[gameframe] source backbuffer 2560x1440 fmt=21 MULTISAMPLE=4 pool=0 -> per-eye 896x1007
[gameframe] frame 2: 4 successful eye submits
```
```
DEBUG [comp_swapchain_create_init] CREATE 0x730e50008f10 896x1007 VK_FORMAT_B8G8R8A8_SRGB (50)
```

That is **the game's own 4x multisampled 2560x1440 back buffer**, resolved and
delivered to monado, from inside `BlackOps.exe`, with nothing written to the
game install. The section below is the bench work that got there.

```
[gameframe] OpenVR up: compositor=00676008 system=001195A0 per-eye 896x1007
[gameframe] vk: instance=00669228 phys=00669240 device=001168C8 queue=00116908 family=0
[gameframe] eye 0: VkImage=0x0000555580f4d8b0 layout=1 896x1007 fmt=44
[gameframe] eye 1: VkImage=0x0000555580f4e420 layout=1 896x1007 fmt=44
[gameframe] PIPE LIVE: game frames -> compositor
[gameframe] source backbuffer 2560x1440 fmt=21 MULTISAMPLE=4 pool=0 -> per-eye 896x1007
[gameframe] frame 2: 4 successful eye submits
```

and on the far side of the chain, monado's own log:

```
DEBUG [compositor_begin_session] BEGIN_SESSION
DEBUG [comp_swapchain_create_init] CREATE 0x730e50008530 896x1007 VK_FORMAT_B8G8R8A8_SRGB (50)
```

---

## 1. What is proven, and on what

Proven on `fakegame.exe`, a stand-in that reproduces what Exp. 10 **measured**
about the real game rather than what a D3D9 tutorial would do: 2560x1440,
`A8R8G8B8`, **4x multisampled**, presenting through
`IDirect3DSwapChain9::Present` and not `Device::Present`. The plugin's own log
confirms the source it resolved from was `MULTISAMPLE=4`, so the `StretchRect`
resolve really ran and was not silently a plain blit — which matters, because
under `proton run` the host's stderr is swallowed and there is no other channel
to ask.

Still unproven against the real game, because of §2: that the game's *contents*
arrive intact and at its own frame rate.

## 2. THE LAUNCH OPTION — the one thing that cannot be done from the prefix

`PROTON_USE_WOW64=1` is mandatory, and it is an environment variable, which is
precisely what a Steam launch gives us no way to set (Exp. 8 §10, Exp. 9).

Measured A/B — same host, same runtime, same prefix, one variable:

| `PROTON_USE_WOW64` | `VR_InitInternal2` |
|---|---|
| `1` | `err=0`, compositor FnTable live, per-eye 896x1007 |
| unset (Steam's default) | `err=105 VRInitError_Init_InterfaceNotFound` |

`err=105` is **exactly** what the plugin reported from inside the real
Steam-launched game, so this is the game's failure reproduced on the bench, not
an analogy.

The cause is Decision 9 restated from the other end: under classic WoW64 a
32-bit PE needs an **i386** `vrclient.so`, xrizer ships only `linux64`, and no
32-bit xrizer exists. Under new-WoW64 the 32-bit PE runs in a 64-bit host
process and the `linux64` build is the correct one.

### 2b. …and `PROTON_USE_WOW64=1` alone is not enough: the container

With the launch option set, the game's own environment was verified live from
`/proc/<pid>/environ`: `PROTON_USE_WOW64=1`, `WINEARCH=wow64`, and
`PROTON_VR_RUNTIME=/home/dlynch/.local/share/bo1vr-xrizer` all reach
`BlackOps.exe`. **And it still failed with err=105.**

The bench and the real launch differ in one more way: the game runs inside the
**Steam Linux Runtime pressure-vessel container**, whose `/usr` is the
container's, not the host's. `err=105 InterfaceNotFound` looks identical
whether the OpenVR runtime is missing, the OpenXR manifest is missing, or
monado's socket is unreachable, so it had to be measured.

It could not be measured from outside — `bwrap` refuses to set up a uid map
under an unprivileged shell, so the container cannot be entered to look. But the
plugin is *already inside it*, and Wine maps the container root at `Z:`. Asking
from there is the only vantage point that works, and it answered exactly:

| Path, as seen by the game | |
|---|---|
| `/usr/share/openxr/1/openxr_monado.json` | **NO** — container's `/usr` |
| `/run/host/usr/share/openxr/1/openxr_monado.json` | yes — the host's, remapped |
| `/usr/lib/x86_64-linux-gnu/libopenxr_monado.so` | **NO** |
| `/run/host/usr/lib/x86_64-linux-gnu/libopenxr_monado.so` | yes |
| `/run/user/1000/monado_comp_ipc` | **NO** — *the blocker* |
| `~/.local/share/bo1vr-xrizer/bin/vrclient.so` | yes — `$HOME` is mounted |

Two separate problems, and the second is fatal on its own: **monado's IPC socket
is not exposed into the container**, so even a correctly located OpenXR manifest
would have nothing to connect to.

Both are fixed with environment, and `PRESSURE_VESSEL_FILESYSTEMS_RW` /
`_RO` are confirmed present in this pressure-vessel build (`strings` on
`pressure-vessel-wrap`).

**The complete launch option — Steam → Black Ops → Properties → Launch Options:**

```
PROTON_USE_WOW64=1 PRESSURE_VESSEL_FILESYSTEMS_RW=/run/user/1000/monado_comp_ipc XR_RUNTIME_JSON=/run/host/usr/share/openxr/1/openxr_monado.json %command%
```

* `PROTON_USE_WOW64=1` — new-WoW64, §2.
* `PRESSURE_VESSEL_FILESYSTEMS_RW=...monado_comp_ipc` — share monado's socket
  into the container.
* `XR_RUNTIME_JSON=/run/host/...` — the host's manifest at the path the
  container sees it. Its `library_path` is *relative*
  (`../../../lib/x86_64-linux-gnu/libopenxr_monado.so`), so it resolves to
  `/run/host/usr/lib/...`, which the table above confirms exists.

Everything else the mod needs lives in the prefix and installs itself. This is
one line, plainly visible, removed by clearing the field — still a far smaller
footprint than writing into the game install.

**Deliberately NOT done:** creating `~/.config/openxr/1/active_runtime.json`.
It would be seen inside the container (`$HOME` is mounted) and would save one
variable, but any absolute `library_path` correct for the container
(`/run/host/...`) is wrong on the host, and it is a user-global file that other
VR applications read. A launch option is scoped to this game.

## 3. The OpenVR runtime, without an environment variable

Proton takes the runtime from `VR_OVERRIDE` if set, else from `runtime[0]` of
`~/.config/openvr/openvrpaths.vrpath` (proton lines 344-348). We cannot set
`VR_OVERRIDE`, so the config file is the lever — and it already pointed at
WiVRn's xrizer.

But that xrizer ships only `bin/linux64/vrclient.so` and a 32-bit PE needs
`bin/vrclient.so`. `setup-runtime.sh` stages a directory that adds it, using
**symlinks through flatpak's `active` path** so a WiVRn update is picked up
instead of silently pinning an old `vrclient.so`:

```
<stage>/bin/linux64/vrclient.so -> <flatpak active>/files/xrizer/bin/linux64/vrclient.so
<stage>/bin/vrclient.so         -> linux64/vrclient.so
```

The staged tree is a strict **superset** of what was there before — 64-bit apps
resolve the identical file — and it is prepended to `runtime`, leaving the
original entry behind it. The previous file is backed up to
`openvrpaths.vrpath.bo1vr-backup`; `./setup-runtime.sh remove` restores it.

That this worked is measured, not assumed: after a launch, Proton had staged
`drive_c/vrclient/bin/vrclient.dll` **and** `vrclient_x64.dll` into the game's
prefix, which it only does when it has resolved a VR runtime.

## 4. Three things the code does because of earlier measurements

* **Hooks `IDirect3DSwapChain9::Present` (slot 3), not `Device::Present`**
  (slot 17). Exp. 10 hooked slot 17 successfully and counted zero frames.
* **`StretchRect` from the multisampled back buffer into a single-sampled
  target.** That call *is* the MSAA resolve, and it does the
  2560x1440 -> 896x1007 downscale in the same pass. A multisampled surface
  cannot be submitted or read with `GetRenderTargetData` at all.
* **One render target per eye, `pBounds` always NULL.** xrizer ignores
  `pBounds` (Exp. 6), so packing both eyes into one texture and slicing would
  silently send the whole thing to both.

The real Present is called **last**, and nothing before it changes the render
target, viewport or any device state, so the game's own picture still reaches
the monitor unaltered.

`Reset` is hooked because the eye targets are `D3DPOOL_DEFAULT` and must be
released before a `Reset` or it fails outright; they are rebuilt afterwards.

`WaitGetPoses` is called per frame. That is deliberate and has a consequence
worth stating plainly: **the game's frame rate becomes the headset's.**

## 5. Deliberately mono

Both eyes get the same picture. This experiment proves the pipe, not stereo;
per-eye content needs the camera hook (BAC-281). Anyone reading a flat image in
the headset as a bug should read this paragraph first.

## 6. Files

```
gameframe.c        the plugin: hooks, interop, resolve, submit
fakegame.c         stand-in for BlackOps.exe with the game's measured surface shape
setup-runtime.sh   stage a 32-bit-capable xrizer; install/remove
Makefile           `make`, `make install` (-> C:\bo1vr in the prefix)
```

`openvr_api.dll` is staged into `C:\bo1vr` and loaded from there by absolute
path: the bare name would find the game install (untouched) or the system
directories, where only Proton's `openvr_api_dxvk.dll` lives — a different DLL
that does no interop at all.
