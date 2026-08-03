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
