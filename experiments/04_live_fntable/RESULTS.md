# Experiment 4 — a live OpenVR `FnTable` from a 32-bit mingw DLL

**Question.** Can a 32-bit mingw-built DLL running inside a Proton prefix get a
**non-NULL** function table out of
`VR_GetGenericInterface("FnTable:IVRCompositor_029", &err)` and successfully
call `__stdcall` methods through it, against a real VR runtime?

**This is the gating experiment for the whole Linux/Proton plan.** Every earlier
round got a NULL table plus a plausible `VRInitError_Init_NotInitialized`. That
is explicitly **not** a pass; a returned error code from a NULL table proves
nothing about the ABI.

**Answer: YES — with two Proton defects worked around.** Reproduce with
`./run.sh`. No VR hardware is involved.

---

## Verdict

| Link in the chain | State |
|---|---|
| Monado with a Simulated HMD, headless | **works, already running on this box** |
| Plain Linux OpenXR app → Monado | **works** (instance, system, session, `xrBeginSession`) |
| xrizer (64-bit) → OpenXR → Monado | **works** |
| Proton 32-bit PE `vrclient.dll` → xrizer, **classic WoW64** | **BLOCKED** — needs a 32-bit xrizer that does not exist |
| Proton 32-bit PE `vrclient.dll` → xrizer, **new-WoW64** | **works after 2 local Proton patches** |
| 32-bit mingw DLL → live `IVRCompositor_029` FnTable | **PASS** |
| Calling `__stdcall` methods through the table | **PASS** |
| `WaitGetPoses` returning a valid HMD pose | **PASS** |
| `IVRSystem_023`: render target size, projection, eye transform | **PASS** |
| Same probe as an `.asi` under the repo's own `dist/dinput8.dll` | **PASS** |
| Compositor `Submit` of a real texture | **not attempted** — needs a D3D9/DXVK device |

---

## The winning configuration

```
Proton                  10.0-4b, PROTON_USE_WOW64=1  (new-WoW64)
                        + tools/patch-proton-wow64-vrclient.py   (see below)
OpenVR runtime          xrizer be664bb, from the WiVRn flatpak, staged so that
                        $RUNTIME/bin/vrclient.so -> linux64/vrclient.so
OpenXR runtime          Monado 21.0.0 (Ubuntu monado-service), Simulated HMD
Compiler                i686-w64-mingw32-gcc 13-win32
Interfaces              IVRCompositor_029, IVRSystem_023
Env                     VR_OVERRIDE=<xrizer dir>, SteamGameId=<anything>
```

`SteamGameId` is **required**, and its absence is a silent failure. Proton's
`steam.exe` only calls `vrclient_init_registry()` when `game_process` is true,
and that flag is set solely by the presence of `SteamGameId`
(`steam_helper/steam.c`). Without it, `HKCU\Software\Wine\VR` is never created
and the game process reports:

```
err:vrclient:get_vulkan_extensions_from_registry Could not create key, status 0x2.
trace:vrclient:load_vrclient Error getting extensions from registry.
```

---

## The passing run

`out/vrlive.log`, verbatim:

```
=== EXPERIMENT 4: live OpenVR FnTable from a 32-bit mingw DLL ===
built with GCC 13-win32, pointer size 4, pid 340
--- step: LoadLibraryA(openvr_api.dll)
openvr_api.dll @ 7b9e0000
VR_IsHmdPresent() = 1
--- step: VR_InitInternal2(&err, VRApplication_Scene, "")
token = 00000001, err = 0 ((null))
--- step: VR_GetGenericInterface("FnTable:IVRCompositor_029", &err)
returned ptr = 0008b470, err = 0 ((null))
PASS-1: non-NULL IVRCompositor_029 FnTable
  SetTrackingSpace      = 001f0000
  GetTrackingSpace      = 001f0010
  WaitGetPoses          = 001f0020
  Submit                = 001f0060
--- step: comp->GetTrackingSpace()
GetTrackingSpace() = 1
--- step: comp->SetTrackingSpace(Standing) then GetTrackingSpace()
after Set(Standing): GetTrackingSpace() = 1
after Set(Seated):   GetTrackingSpace() = 0
PASS-2: round-tripped state through two __stdcall FnTable methods
--- step: more nullary compositor methods (stack discipline check)
GetFrameTimeRemaining()        = 0.000000
IsFullscreen()                 = 1
CanRenderScene()               = 1
ShouldAppRenderWithLowResources() = 0
--- step: VR_GetGenericInterface("FnTable:IVRSystem_026", &err)  [expected NULL]
IVRSystem_026 ptr = 00000000, err = 0 ((null))
--- step: VR_GetGenericInterface("FnTable:IVRSystem_023", &err)
IVRSystem_023 ptr = 0008b550, err = 0 ((null))
--- step: sys->GetRecommendedRenderTargetSize(&w,&h)
recommended render target = 896 x 1007
PASS-3: real per-eye data came back from the runtime through the table
--- step: sys->GetProjectionRaw(Eye_Left, ...)
projection raw L=-0.916331 R=0.916331 T=-0.986818 B=0.986818
IsTrackedDeviceConnected(0) = 1
GetTrackedDeviceClass(0)    = 1
--- step: comp->WaitGetPoses(render[64], 64, game[64], 64)
WaitGetPoses -> 0
PASS-4: WaitGetPoses returned None
  hmd pose valid=1 connected=1
  hmd m[0][3]=-0.028856 m[1][3]=1.620666 m[2][3]=0.000000
--- step: ABI: sys->GetProjectionMatrix (64-byte struct return, hidden pointer)
proj [0][0]=1.091309 [1][1]=1.013358 [2][2]=-1.001001 [2][3]=-0.100100
survived the 64-byte struct return
--- step: ABI: sys->GetEyeToHeadTransform (48-byte struct return)
eye->head translation = (-0.031500, 0.000000, -0.000000)
survived the 48-byte struct return
--- step: VR_ShutdownInternal()
=== EXPERIMENT 4 END: PASS ===
```

Why each line is evidence and not coincidence:

* **PASS-1** is the milestone the task named: a non-NULL `FnTable:IVRCompositor_029`
  with `err == VRInitError_None`. Confirmed independently by Proton's own trace,
  `create_winIVRCompositor_IVRCompositor_029_FnTable -> 0008A838, vtable 0008B990, thunks 001F0000`.
* **PASS-2** proves the `__stdcall` calls really executed, rather than returning
  junk that happens to look sane: `SetTrackingSpace(Standing)` then
  `GetTrackingSpace()` returns 1, `SetTrackingSpace(Seated)` then 0. State
  round-tripped through the runtime. Four further nullary calls after it prove
  the callee-cleans-up stack discipline holds over a sequence.
* **PASS-3** is data only a real runtime could produce: `896 x 1007` is Monado's
  Simulated HMD per-eye render target, and the projection frustum is symmetric
  and plausible.
* **PASS-4** is the whole chain end to end: `WaitGetPoses` returned
  `VRCompositorError_None` with `bPoseIsValid=1`, `bDeviceIsConnected=1`, and a
  head position 1.62 m up — Monado's simulated standing eye height. Tracking data
  crossed 32-bit PE → WoW64 → Proton vrclient → xrizer → OpenXR → Monado and back.

---

## The two Proton defects, and how they were found

Both are in `vrclient`, both only bite a 32-bit PE, and `tools/patch-proton-wow64-vrclient.py`
works around both in a hard-linked *copy* of Proton. The Steam install is never
touched.

### Defect 1 — no unixlib for the 32-bit PE in new-WoW64

Wine resolves a PE builtin's unix library by base name: PE `foo.dll` →
`<unix-arch>/foo.so`. Proton ships

```
files/lib/wine/i386-unix/vrclient.so         (32-bit ELF, for classic WoW64)
files/lib/wine/x86_64-unix/vrclient_x64.so   (64-bit ELF)
```

There is **no `x86_64-unix/vrclient.so`**. `vrclient` is the only Proton module
whose PE name differs between bitnesses (`vrclient.dll` vs `vrclient_x64.dll`),
so it is the only one that hits this. In new-WoW64 the unix side is 64-bit,
`__wine_init_unix_call()` finds nothing, and the first unix call faults:

```
0150:trace:vrclient:load_vrclient got openvr runtime path: .../xrizer/bin/vrclient.so
0150:warn:vrclient:load_vrclient vrclient_init failed, status 0xc0000005
0150:err:msvcrt:_wassert (L"!status",L"../src-vrclient/vrclient_main.c",269)
```

`x86_64-unix/vrclient_x64.so` does export `__wine_unix_call_wow64_funcs`, so the
wow64 half was built — only the name it must be found under is missing.

**Fix:** `ln -s vrclient_x64.so x86_64-unix/vrclient.so`.

### Defect 2 — `wow64_vrclient_init_params` is 4 bytes out of step

With the symlink in place the call reached the unixlib and failed differently:

```
0158:trace:vrclient:load_vrclient got openvr runtime path: .../xrizer/bin/vrclient.so
0158:err:vrclient:vrclient_init unable to load HmdSystemFactory
```

`nm -D` shows xrizer's `vrclient.so` **does** export `HmdSystemFactory`, and a
plain native `dlopen`+`dlsym` of the same file resolves it. `LD_DEBUG=libs`
settled it: in the 32-bit process xrizer's `.so` **is never loaded at all** —
the only `vrclient` in the map is Proton's own. So `dlopen()` was called with
`NULL`, which returns a handle to the main program, and the subsequent
`dlsym` naturally fails.

Cause, in `vrclient_x64/unixlib.h` (structs are `#pragma pack(1)`):

```c
struct wow64_vrclient_init_params
{
    int8_t _ret;
    HMODULE winevulkan;                      /* <-- not wrapped in W32_PTR */
    W32_PTR(char *unix_path, unix_path, char *);
};
```

`HMODULE` is 8 bytes when the unixlib is compiled for x86_64 but the 32-bit PE
writes 4, so every field after it sits 4 bytes too far along. The shipped binary
confirms it exactly — compare the two thunks in `x86_64-unix/vrclient_x64.so`:

```
vrclient_init        14cf18:  48 8b 7f 09    mov 0x9(%rdi),%rdi   ; unix_path, 8-byte
wow64_vrclient_init  14d058:  8b 7f 09       mov 0x9(%rdi),%edi   ; unix_path, 4-byte, STILL at +9
                     14d0af:  48 8b 7b 01    mov 0x1(%rbx),%rdi   ; winevulkan, 8-byte
```

The 32-bit PE puts `unix_path` at **+5**, not +9.

**Fix** (three byte-level edits, applied by the tool):

```
14d058: 8b 7f 09    -> 8b 7f 05      mov 0x5(%rdi),%edi
14d0af: 48 8b 7b 01 -> 8b 7b 01 90   mov 0x1(%rbx),%edi ; nop   (zero-extends)
14d0fd: 44 8b 43 09 -> 44 8b 43 05   the matching TRACE
```

After these, `vrclient_init` dlopens xrizer correctly and the chain comes up.

---

## Why classic WoW64 cannot work today

Run the same thing with `PROTON_USE_WOW64=0` and it fails cleanly rather than
faulting:

```
0150:trace:vrclient:load_vrclient got openvr runtime path: .../xrizer/bin/vrclient.so
0150:trace:vrclient:vrclient_init unable to load .../xrizer/bin/vrclient.so
```

In classic WoW64 the unix side of a 32-bit Windows process is a 32-bit ELF
process, so `i386-unix/vrclient.so` is used and it must `dlopen` a **32-bit**
native runtime. The path it uses is fixed at compile time by the *PE*
architecture (`vrclient_x64/vrclient_main.c`):

```c
#if defined(__x86_64__) && !defined(__arm64ec__)
    static const char append_path[] = "/bin/linux64/vrclient.so";
#elif defined(__arm64ec__)
    static const char append_path[] = "/bin/linuxarm64/vrclient.so";
#else
    static const char append_path[] = "/bin/vrclient.so";
#endif
```

so a 32-bit game always wants `$PROTON_VR_RUNTIME/bin/vrclient.so`, 32-bit.
**xrizer ships only `bin/linux64/vrclient.so`**, and so does the OpenComposite
build in the same flatpak. Making classic WoW64 work would require, in order:

1. a 32-bit build of xrizer (`rustup target add i686-unknown-linux-gnu` plus a
   32-bit link sysroot — `libc6-dev-i386` is **not** installed here, though the
   i386 dpkg architecture *is* enabled);
2. a **32-bit OpenXR loader** (buildable from OpenXR-SDK, xrizer statically
   links its loader today — `ldd` shows no `libopenxr_loader`);
3. a **32-bit OpenXR runtime**. This is the wall. `libopenxr1-monado` is an
   `amd64`-only package, there is no i386 Monado, and Monado's client library
   talks to `monado-service` over an IPC protocol of C structs that is not
   designed to be bitness-agnostic. Nothing in the WiVRn flatpak is 32-bit
   either (`find` for `*i386*` returns nothing).

New-WoW64 avoids all three, because every native component stays 64-bit. **That
is the strategic finding: target `PROTON_USE_WOW64=1`, not classic WoW64.**

---

## Other measured facts

### `openvr_api_dxvk.dll` really is installed into swapped directories

Confirmed again on a freshly created Proton 10.0-4b prefix (README Correction B):

```
drive_c/windows/system32/openvr_api_dxvk.dll   631960 bytes  (the i386 build)
drive_c/windows/syswow64/openvr_api_dxvk.dll   836760 bytes  (the x86_64 build)
```

This experiment does not depend on it — it loads Valve's own 32-bit
`openvr_api.dll` from beside the exe — but a DXVK VR submit path will.
Proton Experimental's `proton` script routes both through `arch_pe_dir()` and
gets it right; Proton 10.0-4b's lines 1077–1079 do not.

### The maximum interface versions are `IVRSystem_023` / `IVRCompositor_029`

`third_party/openvr` is OpenVR master, whose `openvr_capi.h` declares
`IVRSystem_026`. Nothing in this chain knows it:

```
WARN xrizer::clientcore  app requested unknown interface "IVRSystem_026"
warn:vrclient:winIVRClientCore_IVRClientCore_003_GetGenericInterface Failed to create FnTable:IVRSystem_026.
```

`strings` on both Proton's `vrclient.dll` and xrizer's `vrclient.so` tops out at
`IVRSystem_023`. **And master's table is not a prefix of `_023`'s** — master
inserts `ComputeDistortionSet` at slot 4, so every slot from
`GetEyeToHeadTransform` on would be called through the wrong pointer. Hence
`ivrsystem_023.h`, lifted verbatim from OpenVR v2.12.14. `IVRCompositor_029` is
byte-identical between master and v2.12.14, so the compositor keeps using
`openvr_capi.h`.

### xrizer's unimplemented methods kill the process

`GetCurrentSceneFocusProcess()` is `todo!()` in xrizer. The Rust panic unwinds
into a frame Wine cannot dispatch and the process dies:

```
ERROR xrizer  panicked at src/compositor.rs:476:9: not yet implemented
   9: wow64_IVRCompositor_IVRCompositor_029_GetCurrentSceneFocusProcess
  10: __wine_unix_call_dispatcher
0158:err:seh:call_seh_handlers invalid frame 00000001000FF720 (0000000100102000-00000001001FFD20)
0158:err:seh:NtRaiseException Exception frame is not in stack limits => unable to dispatch exception.
```

`GetLastFrameRenderer()` is the same. `GetFrameTimeRemaining()` is a softer case
— it warns `[ONCE] GetFrameTimeRemaining unimplemented` and returns 0.
**Consequence for the mod: an unimplemented xrizer method is a hard process
kill, not a returned error.** Any OpenVR method the mod calls must be checked
against xrizer's coverage first.

### `GetHiddenAreaMesh` faults — but not for the reason the research predicted

README *Correction A* left open whether the 8-byte-struct return of
`GetHiddenAreaMesh` diverges between mingw and MSVC. Calling it here kills the
process, but the failure is **inside Proton's wow64 thunk**, before any
mingw/MSVC question arises:

```
0158:trace:vrclient:winIVRSystem_IVRSystem_023_GetHiddenAreaMesh 0008A1B8
0158:warn:vrclient:winIVRSystem_IVRSystem_023_GetHiddenAreaMesh
        IVRSystem_IVRSystem_023_GetHiddenAreaMesh failed, status 0xc0000005
0158:err:msvcrt:_wassert (L"!status",L"../src-vrclient/winIVRSystem.c",12973)
```

The unix call *returns* `STATUS_ACCESS_VIOLATION` and Proton's own generated
wrapper asserts; our frame is intact when it happens. Struct-return-by-value is
**not** broken in general here — `GetProjectionMatrix` (64-byte) and
`GetEyeToHeadTransform` (48-byte) both succeeded in the same run.
`GetHiddenAreaMesh` is the one OpenVR method returning by value an 8-byte struct
that *contains a pointer*, which is exactly the shape whose wow64 conversion is
mishandled. Set `BO1VR_ABI_HAM=1` to reproduce. **Treat the hidden-area mesh as
unavailable under new-WoW64 until Proton is fixed.**

---

## Reproduction

```sh
./run.sh                     # everything: stage, patch, build, run
make asi                     # the same probe as a real .asi plugin (below)
WORK=/some/dir ./run.sh      # choose the scratch directory
XRIZER_SRC=/path/to/xrizer ./run.sh
BO1VR_ABI_HAM=1 ./run.sh     # additionally reproduce the GetHiddenAreaMesh fault
```

`run.sh` never writes inside the Steam install: it hard-link copies Proton into
`$WORK/proton` and patches only that copy.

Artifacts after a run: `out/vrlive.log` (the experiment) and `out/console.txt`
(the `WINEDEBUG=+vrclient` trace plus xrizer's own logging).

### Standing Monado up by hand

On this machine `monado-service` is already running as a socket-activated
`--user` unit and its legacy prober had already fallen back to the Simulated
HMD, so nothing had to be configured:

```
$ journalctl --user -u monado | grep -A3 'Got devices'
        Got devices:
                0: Simulated HMD
                head: Simulated HMD
```

To force it from a cold start: `SIMULATED_ENABLE=1 XRT_COMPOSITOR_FORCE_XCB=1 monado-service`.

The runtime was verified independently of Wine first, with a plain Linux OpenXR
program using `XR_MND_headless` (kept out of tree; ~90 lines against
`libopenxr_loader`):

```
xrCreateInstance OK
runtime: Monado(XRT) by Collabora et al 'GIT-NOTFOUND'
system: Monado: Simulated HMD  (vendor 42)
xrCreateSession OK (headless)
xrCreateReferenceSpace OK
session state -> 1
session state -> 2
xrBeginSession OK
RESULT: PASS
```

---

## It also works in the real deployment shape

`make asi` builds the same translation unit as `out/asi/vrlive.asi` and stages it
beside the repository's own `dist/dinput8.dll`. `out/asi/asihost.exe` stands in
for `BlackOps.exe`: it does nothing but `LoadLibraryA("./dinput8.dll")` and wait.
The loader's `asi_load_all()` then finds and loads `vrlive.asi`, and the full
experiment runs and passes — `out/asi/vrlive.log` ends in
`=== EXPERIMENT 4 END: PASS ===` and `asihost.exe` exits 0.

The one thing the `.asi` must get right: **`asi_load_all()` runs inside
`DllMain(DLL_PROCESS_ATTACH)` of `dinput8.dll`, i.e. under the loader lock.** A
plugin that called `LoadLibraryA("openvr_api.dll")` and `VR_InitInternal2`
directly from its own `DllMain` would be doing a nested `LoadLibrary` under the
lock. `vrlive.asi` therefore does nothing but `CreateThread` and return; the
worker signals a named event when it is done. Any real VR plugin must follow the
same pattern.

Note that neither the loader's own `LOGI("asi: ...")` output nor the plugin's
`stderr` reached the launching shell under `proton run` — only the file log did.
That matches the README's Logging note: log to a file.

---

## Not proven

* **Compositor `Submit`.** No texture was submitted. That needs a D3D9 (or
  Vulkan) device inside the prefix and exercises `openvr_api_dxvk.dll`, whose
  install directories are swapped in Proton 10.0-4b. This is the next
  experiment, and it is the one that will decide whether stereo rendering is
  feasible, not just tracking.
* **Real hardware.** Everything here is against Monado's Simulated HMD. WiVRn
  was not used.
* **`BlackOps.exe` itself.** The game is not installed on this machine, so the
  host executable is a stand-in. The *loader path* is real, though — see the
  section above.
* **Whether Proton Experimental 11 needs the same patches.** It ships no
  `files/bin-wow64/`, so `PROTON_USE_WOW64=1` selects a different code path
  there and was not tested.
* **A 32-bit xrizer.** Not attempted, because item 3 of the classic-WoW64 list
  above (a 32-bit OpenXR runtime) has no path forward without building Monado
  for i386.
