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
PROTON_USE_WOW64=1 PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES=1 %command%
```

* `PROTON_USE_WOW64=1` — new-WoW64, §2. Nothing to do with OpenXR; without it
  the 32-bit `vrclient.so` cannot load and `VR_InitInternal2` returns `err=105`.
* `PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES=1` — **the supported way** to make
  the host's OpenXR runtime visible inside the container. pressure-vessel's own
  help: *"Import OpenXR 1 runtimes from the host system."*

### What this replaced, and why the replacement is better

The first version of this section prescribed two hand-rolled variables instead:

```
PRESSURE_VESSEL_FILESYSTEMS_RW=/run/user/1000/monado_comp_ipc
XR_RUNTIME_JSON=/run/host/usr/share/openxr/1/openxr_monado.json
```

They worked, and the reasoning behind them (§2b) was correct about *what* was
broken. But they were a workaround for a problem pressure-vessel already
solves, and both are **Monado-specific**:

* `XR_RUNTIME_JSON` hardcodes a path to Monado's manifest, so it would
  **override WiVRn** — the runtime the headset actually needs (BAC-282).
* `PRESSURE_VESSEL_FILESYSTEMS_RW` bind-mounts one named socket, which is
  Monado's and not WiVRn's.

The import flag imports whatever the *active* runtime is, so it keeps working
when the runtime changes. Credit where due: WiVRn's own documentation is what
pointed at it.

§2b's diagnostic table stands unchanged — it is still the measurement that
identified the container as the problem, and the in-process probe that produced
it is still the only way to see what the game sees.

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

---

## 7. The freeze: where it stands

Single-threading the OpenVR calls was a large, measurable improvement, not a
guess that failed:

| | frames before freeze | xrizer panic |
|---|---|---|
| WaitGetPoses on a pose thread | ~2 | yes, repeatedly |
| both on the render thread | **399** | **none** |

Still freezes eventually, and the watchdog is consistent about how:

```
WATCHDOG: no frame for 150 s, stuck at stage 0 (idle), pose ticks 399
```

**Stage 0 means our code is not running.** We complete a frame, return, and the
game's own render loop never calls Present again. The compositor is healthy and
xrizer did not crash. So we do not hang — we leave the game's renderer wedged.

### The lead, from `~/dev/re4vr-research-private/render-submit-sync-RE.md`

That document reverse-engineers the same class of bug in a VrApi→OpenXR shim:
the app's eye render has **not yet been `vkQueueSubmit`ed** when the shim
resolves and releases the swapchain image, so the compositor reads an unwritten
image. Its measured tell was that `vkQueueWaitIdle` had *no effect* — there was
nothing queued to drain — and it was load-gated, appearing only in dense scenes.

Ours is the mirror image: we submit on the queue DXVK owns, holding DXVK's
submission-queue lock, while the game's renderer wants that queue. Removing the
lock made it worse (we wedged in our own `StretchRect`), which says the lock is
load-bearing rather than incidental.

Its recommendation translates directly: **gate on having seen the game's own
submit** rather than assuming `FlushRenderingCommands` has drained it, keep the
same-queue ordering as the in-place mechanism (its Option C), and hold the
one-frame deferred release only as a fallback — it reports that option as
crash-prone in practice.

### Escape hatch

`novr.on` in `C:\bo1vr` disables submission entirely; the game is then stable
with every hook, the dual-view render and the per-eye camera still active.

### 7b. The submit race is CONFIRMED, and it is not the freeze

```
frame 2: 4 submits, ..., gate max 161978 spins, 0 timeouts
```

**161,978 spins.** Our `StretchRect` was demonstrably not complete when we handed
the texture to the compositor — `FlushRenderingCommands` was not draining it,
exactly as `render-submit-sync-RE.md` predicted for its own case. We were
submitting **unwritten textures on every frame**, which is almost certainly what
the flashing was. The gate closes it with zero timeouts.

**And the freeze is unaffected.** Same signature: stage 0, our code idle,
compositor healthy, no panic, the game's render loop stopping (343 frames this
run against 399 before).

So these are **two different faults**, and this run separated them. That is
worth more than a fix would have been, because every earlier attempt was
conflating them — which is why each one appeared to half-work.

It also revalidates an experiment that was previously spoiled. When `nolock.on`
was first tried, removing the queue lock made us wedge inside our own
`StretchRect` — but the copy was not complete at that point, so **that failure
was the race, not the lock**. With the gate guaranteeing completion, the nolock
run is now a clean measurement of the lock alone.

**Known cost to revisit:** 161,978 spins is a busy-wait burning a core for
milliseconds, which is bad for frametime and wants a yield. Left as-is so the
next run changes one variable rather than two.

### 7c. The queue lock is not the cause either

`nolock.on` with the gate in place — the clean measurement that was impossible
before — still froze: stage 0, 68 pose ticks, no panic, gate max 137,544 spins
with 0 timeouts.

So DXVK's submission queue lock is **not** what wedges the game's renderer. That
was the last specific hypothesis standing, and it is now dead. Restoring the
lock, since it is needed (removing it alone made things worse) and is not the
culprit.

**What is left, and the order to test it:**

| Switch | Question it answers |
|---|---|
| `nosubmit.on` | Does the freeze survive with **no `Submit` call at all**? If yes, nothing we hand the compositor matters and the fault is in the interop setup itself — creating the shared VkImages, holding `ID3D9VkInteropDevice`, or the queries — not in the handoff. If no, it is `Submit` and only `Submit`. |
| `notrans.on` | Whether the image layout transitions are implicated. |

`nosubmit.on` is the decisive one and is enabled next. Everything to date has
assumed the fault is in the frame handoff; that assumption has never actually
been tested, and this tests it in one run.

### 7d. It is not the frame handoff at all

`nosubmit.on` — every part of the frame work except the `Submit` call itself —
**still froze**: stage 0, 939 pose ticks, past frame 600.

**With no `IVRCompositor::Submit` at all, the game still wedges.** So the fault
is not in the handoff: not `Submit`, not the compositor, not xrizer. Every
hypothesis from the queue lock onward was aimed at the wrong half of the code,
and this one run retired all of them.

What still runs under `nosubmit=1`, i.e. what is left to blame:

1. `WaitGetPoses` on the render thread (still called — the ticks prove it)
2. the capture `StretchRect` into the eye textures
3. **the gate's busy-wait** — 187,641 iterations of `GetData(D3DGETDATA_FLUSH)`
   per eye, per frame, on the render thread. That hammers DXVK and burns
   milliseconds where frametime is known to be sensitive. It cannot be the
   original cause (the freeze predates it) but it is a strong candidate for
   making things worse, and it should not ship regardless.
4. the layout transitions and the queue lock
5. the interop setup itself (shared VkImages, `ID3D9VkInteropDevice`)

Next run has `nolock.on` + `notrans.on` + `nosubmit.on` all set, leaving only
capture, gate, `WaitGetPoses` and the interop setup. Whatever that does,
it halves the remainder again.

Reminder of the fixed point: `novr.on` (none of this active, every hook and the
dual-view render still running) is **stable**. The boundary is somewhere in the
list above.

## 8. Stop bisecting; make the freeze name itself

Four rounds of bisecting have each cost a playtest and returned one bit. They
have been worth it -- Submit, the frame handoff, texture reuse and the DXVK
queue lock are all dead as theories, killed by measurement rather than
argument -- but the method is now the bottleneck. `nosubmit.on` proved the
strongest form of the result: **the game froze with no `IVRCompositor::Submit`
call at all.** Whatever wedges the render loop, it is not the handoff.

The remaining suspects are capture/`StretchRect`, the gate's busy-wait,
`WaitGetPoses` on the render thread, and the interop setup. Two more rounds to
separate them, at one playtest each.

There is no reason to pay that. `g_stage` answers "where are WE when it
freezes", and has answered it four times running: **stage 0, idle, our code not
on the stack.** That is precisely why it can no longer help -- the question is
now about the GAME's threads, and the watchdog is a live thread sitting in the
same process with the authority to look.

`freeze_autopsy()` suspends every other thread in turn, reads its register
context and a slab of its stack, resumes it, and only then symbolises: module
name plus offset for the instruction pointer, and for every stack slot that
points into an executable page. Not a real unwind -- mingw's DWARF unwinder
cannot walk MSVC frames (Exp. 13) -- but the callers are all in there, and the
module names are the answer.

**Order is load-bearing.** Between `SuspendThread` and `ResumeThread` nothing is
called that could take a lock the suspended thread holds: no logging, no
`GetModuleFileNameA` (loader lock), no CRT. Only `ReadProcessMemory`, a syscall.
Getting this backwards would deadlock the process while diagnosing a deadlock,
and the failure would be indistinguishable from the bug.

### Proven on the bench, before a playtest was spent on it

Two instruments in this project were silent when they were finally needed -- the
slot watch behind a `c < 4096` filter, the crop line behind `g_fov_ok` -- and
both times the run was wasted. So `fakegame.exe` gained `BO1VR_STALL=<seconds>`,
which stops presenting mid-loop and looks exactly like the freeze from the
watchdog's side, and the watchdog now starts BEFORE the VR-init gate so it runs
even where VR is unavailable. Under wine, with `vr_init` failing:

```
AUTOPSY: 2 other threads; render thread is 36
AUTOPSY: tid 36 [RENDER] eip=7bcdd390 ntdll.dll+0xd390 esp=0064fde4
AUTOPSY:   [36] +032 00401acc fakegame.exe+0x1acc      <- the stalling caller
AUTOPSY: tid 312 eip=7bcde040 ntdll.dll+0xe040
AUTOPSY:   [312] +000 79b2c131 wined3d.dll+0x1ac131
```

It fires at 5 s and again at 20 s -- twice and no more, so what is genuinely
stuck can be told from what merely happened to be idle -- and the process ran on
to a clean exit afterwards, which is the evidence that the suspend/resume does
not itself hang anything.

### The next run has the switches OFF

All three bisect switches are removed. The autopsy does not need them, and the
real configuration is the one worth diagnosing: full pipeline, stereo, a picture
in the headset. If it freezes, the log names the module. If it does not, the
playtest was still a stereo validation rather than a black screen.

## 9. The freeze reproduced without a headset, and cornered

Two things made this session's progress possible, and neither was a fix.

**1. There is a second OpenXR runtime on this machine.** `monado.service` is
socket-activated and running with a **Simulated HMD**; `~/.config/openxr/1/` is
empty, so WiVRn is never the loader's default and `/etc/xdg/openxr/1/` resolves
to the system Monado. Whenever WiVRn's server is not up, sessions land there
automatically -- Steam's log has caught it (`Failed to connect to socket
/run/user/1000/wivrn/comp_ipc`). That is a hazard for headset runs, but it also
means **the whole pipeline can be exercised with no headset and no human**, and
a freeze trial now costs ~90 seconds instead of a playtest.

**2. The autopsy named the freeze.** Reproduced on the first unattended launch:

```
WATCHDOG: no frame for 3 s, stuck at stage 0, pose ticks 289
AUTOPSY: 31 other threads; render thread is 416
AUTOPSY: tid 416 [RENDER] eip=7bf1d410 ntdll.dll+0xd410   <- ZwDelayExecution
```

`ZwDelayExecution` is `Sleep`, at the same EIP **and the same ESP** 15 s apart.
Reading that thread's stack out of `/proc/<pid>/mem` while it was still frozen
gave 292 bytes -- a shallow stack, `BlackOps.exe+0xaae6b` calling Sleep beneath
the thread trampoline at `+0x42fe23`. **That is a worker idling in its
wait-for-work loop, not a render call chain.** The main thread (tid 348) is
blocked in `WaitForSingleObject` at `BlackOps.exe+0x347ab`. Every other game
thread is parked; every DXVK thread is in a condition-variable wait; Monado is
alive and still spinning (`Frame late by 252249ms`, no `END_SESSION`).

So the freeze is **the game's own main/render handshake deadlocking**, with the
compositor healthy. Not a hang inside anything we call.

### A bug in the instrument, which hid the answer

The first autopsy printed no stack at all for the render thread. It read a flat
1 KB from ESP, and `ReadProcessMemory` fails the WHOLE call if any part of the
range is unreadable -- a thread parked near the top of its stack has less than
that left before the guard page. It silently blanked precisely the threads the
autopsy exists to look at: the idle ones. Now clamped via `VirtualQuery`.

### The bisect, run unattended, four trials in twenty minutes

| switch | what it removes | result |
|---|---|---|
| `nosubmit` | `IVRCompositor::Submit` entirely | **FROZE** |
| `nowait` | `WaitGetPoses` on the render thread | **FROZE** (42 s, 121 ticks) |
| `nogate` | the 187k-spin event-query gate | **FROZE** (25 s, 307 ticks) |
| `nocap` | the capture `StretchRect`s | **FROZE** (25 s, 282 ticks) |
| `novk` | **the DXVK Vulkan interop device** | **SURVIVED 150 s, 0 watchdog fires** |

No single per-frame operation is necessary for the freeze. `novr.on` stops it
completely. The one thing every freezing configuration shares and every stable
one lacks is `interop_init` -- `QueryInterface(IID_ID3D9VkInteropDevice)`,
`GetVulkanHandles`, `GetSubmissionQueue`, and the four exported eye textures.

In the `novk` run OpenVR was fully up and the capture `StretchRect`s were still
being issued (failing harmlessly, `hr=0x8876086c`) -- so this is also an
independent second confirmation that neither OpenVR nor the capture is the
cause.

**Next split is inside `interop_init`:** the interop handles alone, versus the
exported render targets alone. That is one more unattended trial, not a
playtest.

## 10. The freeze, named: the GAME has an event-query spin loop, and we starve it

Bisecting inside `interop_init` (all unattended, ~90 s per trial):

| configuration | result |
|---|---|
| `notex` — interop device + Vulkan handles, **no eye targets** | SURVIVED |
| `noviq` — targets created, **no** `ID3D9VkInteropTexture` export | FROZE |
| `noq` — targets exported, **no** event queries | FROZE |
| `nocap`+`noviq`+`noq` — targets exist, **nothing touches them** | SURVIVED, 11 400 frames |
| `noviq`+`noq` — **capture only** (plain D3D9 StretchRect) | FROZE |

So the targets are not the problem; *touching* them is — and the D3D9 path
(capture) and the Vulkan path (transition/Submit) each do it independently.
That is where bisecting stops being informative, because there is no single
component left to remove.

The autopsy had already put the render thread at `BlackOps.exe+0xaae6b`, which
disassembles to a one-instruction `Sys_Sleep` wrapper. Its caller is the answer:

```
6ebb40:  ecx = [0x39660b4]                 ; ring size
         edx = [0x396a4cc]                 ; index
         idiv ecx                          ; edx = (index + size - 1) % size
         eax = [edx*4 + 0x3966134]         ; ring[i] -> IDirect3DQuery9*
         test eax,eax ; je done            ; empty slot -> done
6ebb60:  ecx = [eax]                       ; vtable
         push 1                            ; D3DGETDATA_FLUSH
         push 4                            ; sizeof(DWORD)
         lea edx,[esp+0x14]; push edx      ; pData
         push eax                          ; this
         call [ecx+0x1c]                   ; slot 7 = IDirect3DQuery9::GetData
         test eax,eax ; je done            ; S_OK      -> done
         cmp  eax,0x88760868 ; je done     ; DEVICELOST-> done
6ebb7c:  push 1 ; call Sys_Sleep           ; S_FALSE   -> Sleep(1)
6ebb86:  jmp 6ebb40                        ;           -> and poll again, forever
```

**The game runs a ring of D3D9 EVENT queries and spins on
`GetData(D3DGETDATA_FLUSH)` with `Sleep(1)` between polls, leaving only on
`S_OK` or `D3DERR_DEVICELOST`.** The freeze is that query never retiring. The
main thread then blocks in `WaitForSingleObject` waiting for the render thread,
which is what the autopsy saw, and nothing in our submit path is on any stack
because our code has long since returned.

This explains every result above at once. It is not about which of our
operations runs; it is that ANY GPU work we issue against extra resources can
leave the game's own fence unretired. It also explains why the compositor is
always healthy, why `nosubmit` froze, and why `novr.on` is stable.

The irony is instructive: §7 added a gate that spins on
`GetData(D3DGETDATA_FLUSH)` because that is the correct D3D9 way to prove work
has landed. The game had the identical construct all along, one ring away, and
we starve it.

### What this does NOT yet establish

Which DXVK behaviour actually defers the submission that would retire the
game's query. Both a plain `StretchRect` and the interop transition path
trigger it, so the mechanism is below the D3D9 API surface and needs to be
found on the DXVK side, not by more bisecting here.

Addresses for the record: query ring base `0x3966134`, ring size `0x39660b4`,
index `0x396a4cc`, poll loop `0x6ebb40`, `Sys_Sleep` `0x4aae60`.

## 11. Deeper: it is not a VR bug at all

### The freeze reproduces with zero VR involvement

Two probes, both with `novr.on` -- no OpenVR, no interop, no eye textures, no
Submit -- adding exactly one thing: a single extra `D3DPOOL_DEFAULT` render
target and one `StretchRect` into it.

| probe | where the StretchRect runs | source | result |
|---|---|---|---|
| `probe.on` | at Present, between frames | swap-chain back buffer | survived 150 s |
| `probe2.on` | mid-scene, from the camera hook | `GetRenderTarget(0)` (the BOUND target) | **froze** (1 of 2 informative runs) |

So it is not "any extra GPU work" -- the Present-time resolve is harmless. And
the mid-scene one reproduces the deadlock **with no VR code running at all**,
which takes OpenVR, xrizer, Monado, Vulkan interop and the compositor out of
the picture entirely. The minimal reproducer is BO1 + DXVK + one mid-scene
`StretchRect` from the bound MSAA render target.

It is also **stochastic**: probe2 froze once and survived once, with positive
proof it fired in both. The full VR configuration, which does far more work per
frame, has frozen on every run -- 25-42 s, 250-350 frames.

**A trap worth recording:** probe2's FIRST run "survived", and it was a silent
no-op -- `g_probe_rt` is created at the first Present, and nothing logged
whether the StretchRect had ever been issued. The same class of error as the
slot watch and the crop line, for the third time in this project. The rerun
carries `probe2 LIVE: ... hr=0x00000000`, and only runs that print it count.

### What the game's fence looks like at the moment it hangs

```
FENCE: ring size=1 index=264 -> game is polling slot 0
FENCE:   slot 0 = 0d51f238 vtbl->d3d9.dll+0x624d40   <== POLLED
```

The "ring" is a **single** query, and `index` tracks the frame counter exactly
(264 against 263 frames). One event query, issued each frame and waited on the
next: a one-frame GPU-latency limiter. The vtable resolving into `d3d9.dll`
confirms the three addresses from §10 still mean what the disassembly said.

### What is NOT the cause

* **A device Reset.** None occurs -- the log has no Reset line at all.
* **A GPU or driver hang.** Monado is compositing on the same GPU throughout,
  in another process, at 60 Hz.
* **DXVK being blocked.** At the freeze every DXVK worker -- `dxvk-cs`,
  `dxvk-submit`, `dxvk-descriptor`, the shader threads -- is in `futex_wait`
  with nothing queued, `dxvk-queue` in `poll`, `dxvk-frame` in `nanosleep`.
  Nothing is stuck; nothing is pending. That is the shape of a lost wakeup or a
  dropped command, not a deadlock.
* **The single-threaded device, at least as missing locks.** The game asks for
  `flags=0x00000040` -- `HARDWARE_VERTEXPROCESSING` only, **not**
  `D3DCREATE_MULTITHREADED` -- which does permit the runtime to skip internal
  locking, and every D3D9 call we add is traffic its owner promised would not
  exist. `mt.on` ORs `D3DCREATE_MULTITHREADED` into `CreateDevice`. **It froze
  anyway**, at 28 s. Good hypothesis, cleanly refuted.

### A retraction

§11's earlier probe issued a fresh query from the watchdog thread and reported
that it never retired, concluding "the stall is below D3D9". That device is
single-threaded, so calling D3D9 from a second thread is undefined behaviour;
the measurement is not evidence and the conclusion does not stand.

### Where this leaves the fix

Present-time capture survived and mid-scene capture did not, so the practical
route is to stop resolving the bound render target mid-scene: capture at
Present from the back buffer -- which is exactly what `probe.on` does safely --
and obtain stereo by alternating eyes across frames rather than by capturing
twice within one. That trades the dual-view latency win for a configuration
with direct evidence of stability behind it, and it is testable unattended.

## 12. Two independent triggers, and the DXVK mechanism

### Rates, not anecdotes

Everything up to §11 rested on one or two runs, and the freeze is stochastic.
`freezeloop.sh` repeats a configuration and reports a rate; `freezerun.sh` now
marks a run INVALID if the plugin never logged `BISECT`, because two runs where
Steam simply refused to relaunch had been silently counted as passes.

| configuration | froze |
|---|---|
| baseline (everything on) | **3 / 3** (23, 24, 24 s) |
| `nocap`+`nogate` — AER at Present, no gate spin, Submit ON | **3 / 3** |
| `nocap`+`nogate`+`notrans` | **2 / 2** |
| `nocap`+`nogate`+`nolock` | 1 / 2 |
| `nocap`+`nogate`+**`nosubmit`** | **0 / 1** (8 400 frames) |
| `probe` — one StretchRect at Present, no VR | 0 / 1 |
| `probe2` — the same copy mid-scene, no VR | 1 / 2 |

**There are two independent sufficient triggers**, and each freezes on its own:

1. **The mid-scene resolve.** Reproducible with no VR code running at all.
2. **`IVRCompositor::Submit`.** With mid-scene capture gone, removing the layout
   transitions or the queue lock changes nothing, but removing `Submit` stops
   the freeze dead.

Removing both is the only configuration that survives -- and it is useless as a
product, because it puts nothing in the headset.

### What GetData actually returns

DXVK's `D3D9Query::GetData` can return `D3DERR_INVALIDCALL` (when
`vkGetEventStatus` reports neither SET nor RESET), and the game's loop exits
only on `S_OK`/`D3DERR_DEVICELOST` -- so that value would spin forever, a
different bug with a different fix. Hooking the game's own `GetData` settles it:

```
FENCE: GetData calls=21098  last hr=0x00000001  (S_FALSE=19937 S_OK=1161 other=0)
```

`S_FALSE`. The query is issued and simply never signals, so the failure is a
submitted command list that never retires -- not an error code the game can't
handle.

### The mechanism, from DXVK source (v3.0.2, matching Proton Experimental)

* `StretchRect` from a 4x MSAA source to a same-size 1x destination takes the
  `fastPath` and emits `ctx->resolveImage`.
* `resolveImageInline` -- the "fold the resolve into the live render pass"
  path -- requires `GpRenderPassSecondaryCmd`, set only when
  `perfHints().preferRenderPassOps`, which is `tilerMode`: Turnip, Qualcomm,
  MoltenVK, PanVK, ARM, V3DV, Imagination. **On NVIDIA it is never taken.**
* So every mid-scene capture instead calls `endCurrentPass(true)` --
  **tearing down the game's live render pass** -- and emits a dedicated resolve
  render pass. Present-time capture avoids all of this: the back buffer is not
  multisampled, so it is a plain `copyImage` with no live pass to tear down.

That is exactly the measured split between `probe` and `probe2`.

* `D3D9DeviceEx::End` latches a stall flag (`m_stallFlag |= ...`, never
  cleared), so after ~16-32 frames a one-frame-latency fence performs an
  unconditional `ExecuteFlush` on every Issue. **"Recorded but never submitted"
  is therefore not reachable** -- which rules out the lost-flush reading.

### A correction to §11

§11 read the thread dump as "every DXVK worker idle, nothing pending". That was
wrong about the important one. `dxvk-queue` is `DxvkSubmissionQueue::
finishCmdLists`; when idle it waits on a condition variable (a futex), and when
a submission is outstanding it calls `vkWaitSemaphores`, which on the NVIDIA
driver blocks in `poll()`. It was in `poll()`. **It was not idle -- it was
waiting on a submission that never completed**, which is the same conclusion the
`S_FALSE` measurement reaches independently.

`DXVK_DEBUG=hang` was enabled from inside the plugin (set before the first
`Direct3DCreate9`, since a Steam launch has no environment to set).
`VK_KHR_device_fault` and `VK_NV_device_diagnostic_checkpoints` both came up,
but no hang report was printed during a 90 s freeze.

---

## 13. Trigger 2, fixed: an unsynchronised `vkQueueSubmit` on DXVK's queue, issued from inside `WaitGetPoses`

`VkQueue` is an **externally synchronised** object. Two threads may not call
`vkQueueSubmit` on the same queue at the same time, and nothing in Vulkan
detects it if they do -- on this driver the observable result is a submission
that never retires, which is precisely the `S_FALSE` forever / `dxvk-queue`
parked in `vkWaitSemaphores` picture §12 measured.

We were doing exactly that, once per frame, and not in the code §12 was
bisecting.

### The path, read from source at the revisions actually installed

xrizer `be664bb`, Monado `21.0.0+git2905.e26a272c1`, DXVK `v3.0.2`.

1. `VulkanData::session_create_info` (xrizer `src/graphics_backends/vulkan.rs`)
   hands `xrCreateSession` the `device`, `queue_family_index` and a
   `queue_index` derived by matching `get_device_queue` against the queue in
   `VRVulkanTextureData_t`. **That queue is ours** --
   `ID3D9VkInteropDevice::GetSubmissionQueue`'s, i.e. DXVK's.

2. `Compositor::WaitGetPoses` (`src/compositor.rs`), in the **default**
   `EVRCompositorTimingMode::Implicit`, calls `self.PostPresentHandoff()`
   itself -> `FrameController::end_frame` -> `xrEndFrame`.

3. Monado `client_vk_compositor_layer_commit` -> `submit_fence` ->
   `vk_create_and_submit_fence_native` (`src/xrt/auxiliary/vk/vk_sync_objects.c`):

   ```c
   os_mutex_lock(&vk->queue_mutex);
   ret = vk->vkQueueSubmit(vk->queue, 0, NULL, fence);   /* our queue */
   os_mutex_unlock(&vk->queue_mutex);
   ```

   An empty submit whose only job is to signal an exportable fence, guarded by
   **Monado's own mutex and nothing else**.

4. Meanwhile DXVK's `dxvk-submit` thread submits under `m_mutexQueue`
   (`DxvkSubmissionQueue::submitCmdLists`), which is the mutex
   `LockSubmissionQueue` takes -- `D3D9VkInteropDevice::LockSubmissionQueue` ->
   `DxvkDevice::lockSubmission` -> `synchronize()` + `lockDeviceQueue()`.

Two mutexes, one queue. `submit_eye` already held DXVK's for the duration of
`Submit`, so xrizer's own copy `queue_submit` was safe. The fence submit is in
`WaitGetPoses`, which was never inside anything.

### Why this explains every row of §12's table, including the one that mattered

| §12 row | why |
|---|---|
| `nocap`+`nogate` froze 3/3 | Submit on -> real session bound to DXVK's queue -> a foreign `vkQueueSubmit` every frame |
| `+notrans` 2/2, `+nolock` 1/2 | both only touch **our** calls; the unsynchronised one was somewhere else entirely |
| `+nosubmit` 0/1 | with no `Submit`, xrizer never runs `initialize_real_session`, the frame controller stays `None`, `PostPresentHandoff` returns at "no frame controller", and **the foreign submit never happens at all** |

That last line is the whole answer to "why is `Submit` a trigger when
`nosubmit` still does the transitions, the flush and the lock". `Submit` is not
the trigger because of what it hands over. **It is the trigger because it is
what BINDS Monado's client compositor to DXVK's queue.** Everything after the
first `Submit` is per-frame consequence.

### The fix

Hold DXVK's submission lock across `WaitGetPoses`, the same way `submit_eye`
already holds it across `Submit`. Ten lines. `nowaitlock.on` restores the racy
order for an A/B, and `g_waitlocks` is printed in the frame line so a silent
no-op cannot masquerade as a pass -- three instruments in this project have
done exactly that.

| configuration | froze |
|---|---|
| `nocap`+`nogate`+**`nowaitlock`** (control: the racy order, today's build) | **3 / 3** (28, 25, 24 s) |
| `nocap`+`nogate` (**lock held across `WaitGetPoses`**) | **0 / 8** (150 s each) |
| full config (mid-scene resolve back on) + the fix | 1 / 3 (37 s) |

The control is not decoration. A day had passed and Monado had been up for 27 h;
re-measuring the old configuration against the new binary is what makes 0/8
mean something, and it came back at exactly the documented 3/3 in exactly the
documented 24-28 s window.

A ninth fix run came back INVALID -- Steam declined to relaunch and the plugin
never logged `BISECT`. It is counted neither way. That guard is the one §12
added after two such runs had been silently scored as passes, and it is still
earning its place.

Positive proof the passes were real work, from the last frame line of a
surviving run:

```
frame 7800: 15600 submits, 7800 ticks, 0 flat, 0 skipped, gate max 0 spins, 0 timeouts, 7800 waitlocks
```

7800 frames, **15600 successful `Submit`s** (two per frame, none rejected),
7800 locks taken. Not a run that quietly stopped submitting.

### The third row is the two-trigger model confirming itself

With the fix in and the mid-scene MSAA resolve back on, the full configuration
falls from 3/3 to 1/3 -- which is trigger 1's own rate, the same 1-in-2 to
1-in-3 that `probe2` showed in §11 with no VR code running at all. The 3/3
baseline was the two triggers **summed**. Removing one leaves the other, at its
own rate, which is what §12 predicted and is now measured rather than argued.

### Verified vs inferred

* **VERIFIED**: the source path in steps 1-4 (read at the installed revisions);
  that `LockSubmissionQueue` is the mutex `dxvk-submit` holds around
  `vkQueueSubmit`/`vkQueuePresentKHR`; that the lock is taken once per frame in
  the fixed build; the three rates above.
* **INFERRED**: that the concurrent `vkQueueSubmit` is what loses the
  submission. Nothing prints when two threads race an externally synchronised
  object -- there is no error code and no validation layer in this stack. The
  argument is that the spec forbids it, that it is the only unsynchronised
  queue access left in the frame, that serialising it takes the freeze from
  3/3 to 0/8, and that it accounts for `nosubmit`'s survival, which no previous
  theory did.

### The leading hypothesis going in was wrong, and so were the other candidates

The brief's ranked list was: DXVK waiting on an external/imported semaphore for
an exported eye image that never signals; too few eye images in the round
robin; xrizer/Monado never releasing the image. **All three are refuted.**
Monado's client compositor does not import a semaphore from us -- it *exports*
a fence it submits itself, and the swapchain images it copies into are its own.
DXVK's wait is on its own timeline semaphore for its own submission; that
submission is lost, not blocked. Doubling the eye images (`g_buf`) or chasing
image release would have changed nothing, because the image was never the
subject: **the queue was.**

### Cost, and what is left

The lock is now held across a call that blocks (`xrWaitFrame`), so DXVK cannot
submit while we wait for the runtime. That is the one thing to watch, and it is
not visible yet: 7800 frames per 150 s run, which includes launch and level
load, so ≥52 fps average with the lock taken every frame. If it ever does bite,
the principled version is `SetExplicitTimingMode(Explicit_ApplicationPerforms-`
`PostPresentHandoff)` -- xrizer honours it (`compositor.rs`, the
`timing_mode` check in `WaitGetPoses`) -- and then calling
`SubmitExplicitTimingData` / `PostPresentHandoff` ourselves, under the lock,
around the two `Submit`s. That moves the foreign submit inside a lock we hold
for microseconds instead of milliseconds, and `SubmitExplicitTimingData`
returning `None` instead of `RequestFailed` is a free proof that the mode
actually took.

**Trigger 1 is now the only thing standing between this and a playable build**,
at 1/3. §12 named its mechanism (`resolveImageInline` needs `tilerMode`, NVIDIA
never takes it, so every mid-scene MSAA `StretchRect` tears down the game's
live render pass) and §11 named the route around it: capture at Present from
the non-multisampled back buffer, which `probe.on` survived and which the
`nocap` configuration measured 0/8 above already uses.

## 14. A per-frame timing CSV, and the FPU bug that nearly made it useless

Every diagnostic in this experiment so far was built *after* a failure and cost
playtests before it could say anything: the watchdog, then the thread-suspend
autopsy, then `g_waitlocks` to prove the fix had run. The generic version of
that instrument is a per-stage millisecond record, because the stage that stops
advancing is the stage that hangs.

The column set is taken in spirit from Vice City VR's `vr_perf_openxr_*.csv`
(a reVC fork: x64, D3D12, none of our constraints), which carries
`xr_wait_frame_ms` / `xr_acquire_ms` / `xr_end_frame_ms` / `submit_ms` and
counts its own fallbacks and failures as first-class columns. Ours maps that
onto the stages this plugin actually has, written to
`%TEMP%\bo1vr_frames.csv`, on by default, disabled with `perf.off`.

The row is emitted **after** the real `Present` returns, so a frozen frame
writes no row at all and the last row plus `stage` names where the next one
died.

### Two defects the bench caught before a playtest could

**Flushing on a row count alone loses the rows that matter.** The first bench
run presented 60 frames, never reached the 64-row mark, and wrote a file
containing nothing but the header. Fixed by bounding staleness in *time*
(`PERF_MAX_STALE_MS` = 250 ms), which holds at any frame rate, plus a
`DLL_PROCESS_DETACH` flush for the tail of a clean exit.

**D3D9 sets the x87 to single precision, and it silently destroyed the clock.**
The first real-game capture quantised *every column* to exact multiples of
32 ms: whole frames fell below one tick, every per-stage column read `0.000`,
and the file looked perfectly well-formed. The clock was never the problem --
`QPF` is 10 MHz. The problem is that D3D9 leaves the x87 with a **24-bit
mantissa** for any device created without `D3DCREATE_FPU_PRESERVE`, and the
game does not pass it (`flags=0x40`, see the `CreateDevice` log line). Computing
`(double)counter * 1000.0 / freq` on an absolute counter of ~4.3e12 then has a
representable step of tens of milliseconds. Differencing against a baseline in
64-bit integers *first* removes the dependence entirely; at 43 ms elapsed the
step is ~3 ns. `perf_init` now logs `QPF` and the base counter so this is
answerable from the log rather than by re-deriving it.

This is the fourth instrument in this project to look like it worked while
reporting nothing. The tell is always the same: implausibly clean output.

### What it says about the shipped configuration

4902 frames, dual view live, `nocap.on`, 100 s unattended (`freezerun.sh`):

| stage | mean ms |
| --- | --- |
| whole hook | 13.665 |
| `WaitGetPoses` (incl. queue lock) | **13.335** |
| left: transitions + flush + lock | 0.243 |
| left / right `Submit` | 0.024 / 0.022 |
| right: transitions + flush + lock | 0.022 |
| resolve | 0.003 |
| real `Present` | 0.010 |
| gate spins | 0 |

Two things follow. **We are compositor-paced, not overhead-bound:** 97.6% of
the frame is the pose wait, and everything this plugin does costs ~0.33 ms.
**The first eye pays for the queue drain:** left `trans` is 11x right, which is
`LockSubmissionQueue` draining pending submissions once per frame -- the second
eye finds nothing left to drain. That asymmetry is not a bug, but it is the
number to watch if the §13 lock is ever revisited.

The event-query gate never spun once (0 spins across 4902 frames) on the dual
view path, which is consistent with §12: the gate exists for the resolve path,
and dual view does not use it.

## 15. `r_smp_backend 0` refuted — it makes the freeze *more* likely

§14's lead from `xoxor4d/t5-rtx` was that the freeze spin loop at `0x6EBB40` is
the frame fence of the engine's SMP (separate render thread) backend, so
turning that backend off might remove trigger 1 at the root and let us drop
`nocap.on` — which is what currently forces our capture to Present and blocks
proper per-eye capture (#30).

**It does the opposite.** Behind `smpoff.on`, with everything else at the
shipped configuration:

| configuration | froze |
| --- | --- |
| `nocap.on` only (control) | **0 / 3** (100 s, 100 s, 90 s) |
| `nocap.on` + `smpoff.on`, per-frame override | 1 / 1 — froze at 49 s |
| `nocap.on` + `smpoff.on`, registration override | 1 / 1 — froze at 41 s |

Two for two against zero for three is not a large sample, but the direction is
unambiguous and there is no version of this result that says "adopt it".

### What was actually established

The lever is not in doubt — it demonstrably worked, twice, two different ways:

* `Dvar_RegisterBool("r_smp_backend", 1) -> forcing 0 at registration`, and the
  dvar then read back `current=0 latched=0`;
* `smp 1/0` in the frame line — state live, and the engine never once wrote the
  value back.

So the game really did run with its SMP backend disabled, and froze sooner.

### Facts confirmed on the way, each verified against our own binary

* `dvar_s.current` is at **`+0x18`** — not merely because t5-rtx says so, but
  because the game's own code does `cmp BYTE PTR [eax+0x18], 0` at `0x6D5815`.
* `Dvar_RegisterBool` = **`0x45BB20`** — hooked successfully and it intercepted
  the real registration. Args are cdecl `(name, value, flags, desc)`, read off
  the push order at `0x6CAD4D..0x6CAD60`.
* `Dvar_FindVar` = **`0x5AE810`** — returned a dvar whose `name` field read back
  as exactly `"r_smp_backend"`.
* `r_smp_backend` registers with a default of **1**, its `dvar_s*` lives at
  **`0x3B1FB70`**, and it is read in exactly **one** place: the function at
  `0x6D5810`, which branches to a `Sys_IsMainThread` (`0x5A48F0`) path when the
  flag is clear.

### An honest limit on what this tested

Both trials ran with `nocap.on` still present, which suppresses trigger 1. So
the freezes seen here are **not** trigger 1 — they are a new failure mode that
disabling SMP introduces. Whether `r_smp_backend 0` would also have suppressed
trigger 1 is now moot: a lever that freezes the shipped configuration cannot be
the route to removing `nocap.on`, so it was not worth a further run to find out.

### A wrong inference, corrected

The first version of `smp_backend_off()` counted frames where the engine had
set the value back to 1 and claimed a count stuck at 1 proved the dvar was no
longer read. That is wrong — reading a value does not modify it, so the counter
cannot distinguish inert from live. It was settled statically instead (one
write site, one read site, above). The comment in the source now says so.

The lever stays in the tree, off by default. It is a working, verified
instrument, and a negative result that cost three runs is worth keeping so it
is not re-derived.

## 16. First headset playtest: "pretty zoomed, and pulsing" — two symptoms, one cause

The first real playtest under the shipped `nocap.on` config reported no working
head tracking, a heavily magnified image, and a visible pulse. All three come
from the same place, and none of them were head tracking being broken.

**The evidence.** `bo1vr_frames.csv` (17568 frames, no freeze) carried
`dual=0` with the `eye` column flipping `0,1,0,1` every row. `bo1vr_camera.log`
showed the head basis being composed and applied — `HEAD ORIENTATION LIVE`,
`*** ORIENTATION APPLIED`, a real offset (`offset returned 1.8198`) — so the
camera side was working. But of twelve consecutive `R_SetViewParms` calls only
ONE carried an eye at all, and it carried `eye=1`; the rest were `eye=-1` at the
game's own `tanHalfFov 0.849 0.478` rather than the widened `1.889 1.151`.

**The cause.** `nocap.on` makes `bo1vr_capture_eye` return at its first check,
so nothing ever fills the eye textures, `g_captured` never reaches 2, and
`g_dual` never goes live. Nothing else switched off with it:

* `camera.asi` still rendered the scene twice, for two eyes nobody captured —
  the second render simply overwrote the first;
* `gameframe.asi`, still seeing `g_dual == 0`, still ran the alternate-eye
  fallback and called `bo1vr_camera_set_eye` every frame at Present.

Two eye-selection schemes writing one flag, over a back buffer holding whichever
camera happened to draw last. The eye offset flipped sideways every frame — the
**pulse** — and whenever the surviving frame was one of the game's own views,
the Present-time crop kept the middle 1309 of 2560 columns on the assumption it
had been rendered 1.96x wide when it had not: a ~2.2x magnification, the
**zoom**. Head tracking was applied to renders that were then discarded.

**The fix**, both sides made to agree via a new `bo1vr_capture_enabled()` export:

* when capture is off, `camera.asi` renders ONCE with `CAM_EYE_CENTRE` — head
  orientation, headset FOV, no eye offset;
* `gameframe.asi` resolves that one frame into BOTH eyes and stops alternating.

Also fixed on the way: `CAM_EYE_CENTRE` was exempt from the FOV override as well
as the eye offset. The offset exemption is right (mono has no parallax); the FOV
one was not, and it meant the centre path — the configuration documented for
testing head tracking on a flat monitor — was permanently zoomed.

Result is mono, which is the honest shape for a configuration that cannot
capture per-eye. Stereo comes back with #32, not before. **This is a code fix
verified only by build and by reading the traces; the playtest that confirms it
has not run yet.**

### 16.1 The mono fix was right and still did nothing — the eye bracket wraps the wrong function

Second playtest with §16 installed: pulsing gone (the `eye` column is pinned and
`wait_poses_ms` is back to a compositor-paced 8.9-9.3 ms of an 11.1 ms frame),
but head tracking was now *completely* dead and the zoom barely moved.

The new log says why: **every sampled `R_SetViewParms` call read `eye=-1`.** Not
one carried `CAM_EYE_CENTRE`, so `hk_body` never ran, so neither the head basis
nor the headset FOV was ever written.

`objdump` names five call sites for `R_SetViewParms` (`0x6C7F80`):

| site | in |
|---|---|
| `0x6C8C5F` | `R_RenderScene` `0x6C8C40` — the one we hook, and **conditional** |
| `0x6C8D96` | `R_RenderSceneInternal` `0x6C8CD0` — the scene view |
| `0x6C8E73` | `R_RenderSceneInternal` |
| `0x6C8F5B` | `R_RenderSceneInternal` |
| `0x6CEF64` | elsewhere |

and Ghidra shows the one site we do wrap is gated:

```c
void R_RenderScene(int refdef) {
    if (*(char *)(DAT_03b1fd24 + 0x14) != '\0') { FUN_004682d0(...); R_SetViewParms(); }
    ...
}
```

`R_RenderScene` (`0x6C8C40`) never calls `R_RenderSceneInternal` (`0x6C8CD0`) —
they are separate functions with separate callers. So bracketing
`o_R_RenderScene` sets the eye across a window that usually contains no
`R_SetViewParms` call at all.

**Which means the bracket was never what made head tracking work.** The
alternate-eye fallback set the eye at Present and *left it set*, so it was still
set when the real scene view was built later in the frame. §16 removed that
fallback and with it the only thing that was actually delivering the pose. The
alternation was the pulse; the persistence was the mechanism.

Fix: in mono mode set `CAM_EYE_CENTRE` and leave it set. Same breadth the old
fallback already had, minus the alternation, plus the correct FOV.

**A second lesson, about the instrument.** The call dump sampled calls 1000-1011
and then went silent forever, which put every sample in the first seconds of the
process — the main menu, whose background is two static cameras (origins
`243.500 478.300 105.800` and `52.033 457.121 51.783`, unchanged across every
sample in three separate runs). Two playtest reports about *gameplay* were
analysed off *menu* frames. Now sampling four consecutive calls every 3600.
