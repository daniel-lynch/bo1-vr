# Experiment 5 — a D3D9/DXVK texture reaching the compositor

**Question.** From a 32-bit process under Proton, can a texture rendered through
a **D3D9** device reach `IVRCompositor::Submit` and actually arrive at the
compositor — for both eyes, against Monado's Simulated HMD, no VR hardware?

Experiment 4 proved *tracking*. This is the experiment that decides whether
stereo **rendering** is feasible.

**Answer: YES.** Reproduce with `./run.sh`. 600 frames × 2 eyes submitted,
`VRCompositorError_None` every time, and `monado-service`'s own trace log shows
exactly 600 `ACQUIRE_IMAGE` / `WAIT_IMAGE` / `RELEASE_IMAGE` triples on a client
swapchain of exactly our per-eye size.

**But not the way the task premise assumed**, and the difference matters — see
"The premise was wrong" below. `openvr_api_dxvk.dll` is not an interop shim, and
Proton's DirectX→Vulkan bridge does not cover D3D9. The app has to do the
D3D9→Vulkan interop itself.

---

## Verdict

| Link in the chain | State |
|---|---|
| D3D9 device on DXVK in a 32-bit **new-WoW64** process | **PASS** |
| `IDirect3D9` → `ID3D9VkInteropInterface` | **PASS** |
| `IDirect3DDevice9` → `ID3D9VkInteropDevice` (VkInstance/PhysDev/Device/Queue) | **PASS** |
| `IDirect3DTexture9` → `ID3D9VkInteropTexture::GetVulkanImageInfo` (VkImage) | **PASS** |
| Render target really contains what we drew (GPU readback) | **PASS** |
| `Submit(eType = TextureType_Vulkan)` per eye | **PASS**, 1200/1200 |
| Proton vrclient unwrapping 32-bit PE Vulkan handles to native | **PASS** |
| xrizer creating an OpenXR session on the app's VkDevice | **PASS** |
| **Frames observed arriving at `monado-service`** | **PASS** — 600/600 |
| `Submit(eType = TextureType_DirectX)` with a D3D9 surface | **KILLS THE PROCESS** — measured, see below |
| `openvr_api_dxvk.dll` as an interop path | **does not exist** — it is a plain `openvr_api.dll` |
| Same probe as an `.asi` under the repo's own `dist/dinput8.dll` | **PASS** |
| Real HMD, real game | still not attempted |

---

## The premise was wrong, and the correction is the useful part

The working assumption going in was: *"`IVRCompositor::Submit` does not accept a
D3D9 texture directly — that is precisely why `openvr_api_dxvk.dll` exists:
DXVK's D3D9 backs surfaces with `VkImage`s, which can be submitted as Vulkan
textures."* Half right. Checked against the code:

### `openvr_api_dxvk.dll` performs no interop whatsoever

It is a plain build of Valve's `openvr_api.dll` under a different filename.
Measured on the shipped binary:

```
$ i686-w64-mingw32-objdump -p .../dxvk/i386-windows/openvr_api_dxvk.dll
        DLL Name: KERNEL32.dll
        DLL Name: SHELL32.dll
   [ 5] VR_GetGenericInterface      [11] VR_InitInternal
   [ 6] VR_GetInitToken             [12] VR_InitInternal2
   ... exactly openvr_api.dll's 18 exports, and nothing else

$ strings -a .../openvr_api_dxvk.dll | grep -iE 'dxvk|vulkan|d3d9|d3d11|interop|VkImage'
VRInitError_Compositor_D3D11HardwareRequired
VRInitError_Compositor_D3D11RendererInitializationFailed      # error-enum names only
$ strings -a .../openvr_api_dxvk.dll | grep -c openvr_api_dxvk
0
$ strings -a .../openvr_api_dxvk.dll | grep -c '^openvr_api\.dll$'
1                                                             # it calls itself openvr_api.dll
```

DXVK's own source says what it is for (`src/dxvk/dxvk_openvr.cpp`, v2.6.2):

```cpp
HMODULE VrInstance::loadLibrary() {
    // Use openvr_api.dll only if already loaded in the process ...
    if (!::GetModuleHandleEx(0, "openvr_api.dll", &handle))
      handle = ::LoadLibrary("openvr_api_dxvk.dll");
    ...
}
```

— a private copy so DXVK can reach `VR_GetGenericInterface` without clashing
with the game's own `openvr_api.dll`. All it does with it is call
`GetVulkanInstanceExtensionsRequired` / `GetVulkanDeviceExtensionsRequired`
before creating its `VkInstance`/`VkDevice`. It never touches a texture.

Under Proton it is usually not even loaded: `VrInstance::initInstanceExtensions()`
opens `HKCU\Software\Wine\VR` first and only falls back to `getCompositor()` if
that key is missing —

```cpp
if (!m_vr_key && !m_compositor)
  m_compositor = this->getCompositor();
```

— and Proton's `steam.exe` populates that key (which is why `SteamGameId` is
mandatory; Exp. 4). The extension lists come out of the registry.

### The real D3D9 interop path is `ID3D9VkInterop{Device,Texture}`

`dxvk/src/d3d9/d3d9_interfaces.h`. `d3d9_dxvk.h` in this directory is the C
transcription. `ID3D9VkInteropTexture::GetVulkanImageInfo()` hands out the
backing `VkImage` plus a full `VkImageCreateInfo`;
`ID3D9VkInteropDevice::GetVulkanHandles()` / `GetSubmissionQueue()` hand out the
instance, physical device, device and queue. That is everything
`VRVulkanTextureData_t` needs.

### Proton's DirectX→Vulkan bridge is D3D11/D3D12 only

Proton *does* translate a DirectX texture for you, but only through DXGI.
`vrclient_x64/vrcompositor_manual.c` (proton_10.0):

```c
static const w_Texture_t *load_compositor_texture( uint32_t eye, const w_Texture_t *texture, ... )
{
    switch (texture->eType)
    {
    case TextureType_DirectX:   return load_compositor_texture_dxvk( ... );
    case TextureType_DirectX12: return load_compositor_texture_d3d12( ... );
    default: return texture;
    }
}
```

and `load_compositor_texture_dxvk()` opens with

```c
if (FAILED(texture_iface->lpVtbl->QueryInterface( texture_iface, &IID_IDXGIVkInteropSurface,
                                                  (void **)&state->dxvk_surface )))
{
    WARN( "Invalid D3D11 texture %p.\n", texture );
    return texture;                 /* forwarded untranslated */
}
```

`IDXGIVkInteropSurface` is DXVK's **DXGI/D3D11** interop IID. A DXVK D3D9
surface does not implement it. `strings` on the shipped 32-bit `vrclient.dll`
confirms which interop IIDs it carries — `IID_IDXGIVkInteropDevice`,
`IID_IDXGIVkInteropSurface`, `IID_ID3D12DXVKInteropDevice{,2}` — and **no
`ID3D9VkInterop*` of any kind**.

So `TextureType_DirectX` from D3D9 is a dead end at *both* ends of the bridge.
**Measured** (`BO1VR_TRY_DIRECTX=1 ./run.sh`, verbatim from `out/dxconsole.txt`):

```
0158:trace:vrclient:winIVRCompositor_IVRCompositor_029_Submit _this 0008A020, eEye 0,
      pTexture 0063FC70 (eType 0), pBounds 00000000, nSubmitFlags 0
0158:trace:vrclient:load_compositor_texture_dxvk texture = 0063FC70
0158:warn:vrclient:load_compositor_texture_dxvk Invalid D3D11 texture 0063FC70.
[ERROR xrizer ThreadId(1)] panicked at src/graphics_backends.rs:131:22:
Unsupported texture type: DirectX
[ERROR xrizer ThreadId(1)] Backtrace:
   9: _Z20IVRCompositor_SubmitI33u_IVRCompositor_IVRCompositor_029...
  10: __wine_unix_call_dispatcher
0158:err:seh:call_seh_handlers invalid frame 00000001000FDD10 (0000000100102000-00000001001FFD20)
```

The process dies — the same Exp. 4 failure mode where a Rust panic unwinds into
a frame Wine cannot dispatch. xrizer's `SupportedBackend::new()` ends in
`other => panic!("Unsupported texture type: {other:?}")`, and
`is_texture_type_supported()` accepts only `Vulkan` and `OpenGL`.

**Consequence for the mod: a D3D9 game on this stack must do the D3D9→Vulkan
interop itself and submit `TextureType_Vulkan`.** There is no shim that will do
it for you, and getting it wrong is a process kill rather than an error code.

---

## What the app did

`vrsubmit.c`, in the order it runs. The Vulkan hand-off mirrors Proton's own
D3D11 sequence step for step (`load_compositor_texture_dxvk`):

```
transition texture  layout -> VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
FlushRenderingCommands()
LockSubmissionQueue()
Submit( eye, &Texture_t{ handle = &VRVulkanTextureData_t, eType = Vulkan } )
ReleaseSubmissionQueue()
transition texture  TRANSFER_SRC_OPTIMAL -> layout
```

`TRANSFER_SRC_OPTIMAL` is not a guess: xrizer's
`graphics_backends/vulkan.rs: copy_texture_to_swapchain()` issues
`cmd_copy_image` / `cmd_blit_image` / `cmd_resolve_image` with
`vk::ImageLayout::TRANSFER_SRC_OPTIMAL` as the source layout.

Left eye is cleared red, right eye green, plus a white band that walks down the
image each frame, so a wrong-eye or frozen-frame bug is visible rather than
inferred.

---

## The passing run

`./run.sh` with `BO1VR_FRAMES=600`. `out/vrsubmit.log`, verbatim:

```
=== EXPERIMENT 5: D3D9/DXVK texture -> IVRCompositor::Submit ===
built with GCC 13-win32, pointer size 4, pid 336, frames=600
--- step: 1. LoadLibraryA(openvr_api.dll) + VR_InitInternal2
  openvr_api.dll         @7b550000  Z:\home\dlynch\dev\bo1-vr\experiments\05_submit\out\openvr_api.dll
                         size=631960 bytes, PE machine=0x014c (i386)
VR_InitInternal2 -> token=00000001 err=0 ((null))
IVRCompositor_029 FnTable = 0008ba70 (err 0)
IVRSystem_023 FnTable    = 0008bb50 (err 0)
PASS-1: OpenVR up; per-eye render target 896 x 1007
--- step: 2. probe openvr_api_dxvk.dll (README Correction B)
  2a. LoadLibraryA("C:\windows\system32\openvr_api_dxvk.dll")
    system copy          NOT LOADED (err=193)
  -> the swapped-directory bug is PRESENT: the system copy a 32-bit process resolves is
     the x86_64 build (LoadLibrary err=193, 193 = BAD_EXE_FORMAT)
  2b. LoadLibraryA("openvr_api_dxvk.dll")  [bare name: app dir first]
    effective            @7a6f0000  Z:\home\dlynch\dev\bo1-vr\experiments\05_submit\out\openvr_api_dxvk.dll
                         size=631960 bytes, PE machine=0x014c (i386)
  -> loadable 32-bit build, VR_GetGenericInterface present: workaround confirmed
--- step: 3. LoadLibraryA(d3d9.dll) + Direct3DCreate9
  d3d9.dll               @7a2e0000  C:\windows\system32\d3d9.dll
                         size=4173838 bytes, PE machine=0x014c (i386)
Direct3DCreate9 -> 001dfe08
--- step: 3b. QueryInterface(IDirect3D9 -> ID3D9VkInteropInterface)
QI ID3D9VkInteropInterface -> hr=0x00000000 ptr=001dfed8
PASS-2: d3d9.dll is DXVK; VkInstance = 00091a98
--- step: 3c. CreateDevice
hwnd = 000100d4
CreateDevice -> hr=0x00000000 dev=00cbc780
PASS-3: live D3D9 device on DXVK inside a 32-bit new-WoW64 process
--- step: 3d. QueryInterface(IDirect3DDevice9 -> ID3D9VkInteropDevice)
QI ID3D9VkInteropDevice -> hr=0x00000000 ptr=00ceb09c
  VkInstance       = 00091a98  (matches IDirect3D9's: yes)
  VkPhysicalDevice = 00091ab0
  VkDevice         = 00090cd8
  VkQueue          = 00090d00  queueIndex=0 queueFamilyIndex=0
PASS-4: Vulkan handles obtained from the D3D9 device
--- step: 4. CreateTexture(RENDERTARGET) per eye + GetVulkanImageInfo
  eye 0: GetVulkanImageInfo hr=0x00000000 VkImage=0x0000555572b7dc30 layout=5
    extent=896x1007x1 mips=1 layers=1 samples=1 format=44 (B8G8R8A8_UNORM) usage=0x00080017 tiling=0
  eye 1: GetVulkanImageInfo hr=0x00000000 VkImage=0x0000555572b8d7e0 layout=5
    extent=896x1007x1 mips=1 layers=1 samples=1 format=44 (B8G8R8A8_UNORM) usage=0x00080017 tiling=0
PASS-5: a real VkImage behind each eye's D3D9 render target, extents agree
--- step: 5. frame loop: WaitGetPoses -> render -> Submit(L) -> Submit(R) -> PostPresentHandoff
  readback eye0/LEFT (want red 0xdc1414 + white): background(8,8)=0xdc1414  marker(448,62)=0xffffff
frame 0 eye 0: Submit -> 0 (None)
  readback eye1/RIGHT (want green 0x14c828 + white): background(8,8)=0x14c828  marker(448,62)=0xffffff
PASS-5b: both render targets read back exactly the colours we drew
frame 0 eye 1: Submit -> 0 (None)
PASS-6: first stereo pair submitted with VRCompositorError_None
  ... frame 0 done (2 successful submits so far)
  ... frame 500 done (1002 successful submits so far)
submitted 600 frames, 1200 successful eye submits
--- step: 5b. comp->GetFrameTiming() -- ask the runtime how many frames it ended
runtime-side m_nFrameIndex = 600 (we called PostPresentHandoff 600 times)
runtime-side m_flSystemTimeInSeconds = 10.354104
PASS-7: the runtime's own frame counter advanced to 600
--- step: 6. teardown: VR_ShutdownInternal first, then D3D9
  VR_ShutdownInternal returned
  D3D9 released
=== EXPERIMENT 5 END: PASS ===
```

### And what Monado saw — this is the actual pass condition

`Submit` returning `None` is **not** accepted as proof; a plausible-looking
success has already fooled this project once. `run.sh` runs `monado-service` at
`XRT_COMPOSITOR_LOG=trace` and counts what arrives:

```
== 7. what MONADO saw
   client swapchain(s) monado created for us:
     DEBUG [comp_swapchain_create_init] CREATE 0x7e9ac00086d0 896x1007 VK_FORMAT_B8G8R8A8_SRGB (50)
   swapchain_acquire_image : 600
   swapchain_wait_image    : 600
   swapchain_release_image : 600
   LAYER_COMMIT            : 627
   frames we submitted     : 600
```

Per frame, in `$WORK/monado.log`:

```
TRACE [swapchain_acquire_image] ACQUIRE_IMAGE
TRACE [swapchain_wait_image] WAIT_IMAGE
TRACE [swapchain_release_image] RELEASE_IMAGE
TRACE [compositor_mark_frame] MARK_FRAME 0
TRACE [compositor_begin_frame] BEGIN_FRAME
TRACE [compositor_layer_commit] LAYER_COMMIT at 239310351.424ms
TRACE [compositor_layer_commit] LAYER_COMMIT finished drawing at 239310351.652ms
TRACE [compositor_predict_frame] PREDICT_FRAME
```

Why this is evidence and not coincidence:

* The counts are **exact**, in a process we did not write and do not control:
  600 acquires, 600 waits, 600 releases for 600 submitted frames. A no-op
  `Submit` produces zero of them.
* The swapchain Monado created for the client is **896×1007** — precisely the
  `GetRecommendedRenderTargetSize()` the app asked `IVRSystem_023` for and
  precisely the extent `GetVulkanImageInfo` reported for the D3D9 texture. The
  size travelled from Monado, through xrizer and Proton to the app, back through
  the D3D9 texture, and back to Monado.
* `LAYER_COMMIT` is Monado's server side of `xrEndFrame`. 627 ≈ 600 + the idle
  frames around session start/stop.
* Independently, xrizer's own counter (`GetFrameTiming().m_nFrameIndex`, bumped
  once per `end_frame()` that reaches `xrEndFrame`) read **600**.
* Independently again, the GPU readback shows the submitted render targets
  really held `0xdc1414` (left) and `0x14c828` (right) with the white marker —
  so the frames Monado consumed are the frames we drew, not empty images.

Three independent counters, from three different processes, agreeing on 600.

Confirmed on the Proton side too, once per eye per frame:

```
0158:trace:vrclient:winIVRCompositor_IVRCompositor_029_Submit _this 00089FE8, eEye 0,
      pTexture 0063FCFC (eType 2), pBounds 00000000, nSubmitFlags 0
```

`eType 2` = `TextureType_Vulkan`.

---

## Other measured facts

### `openvr_api_dxvk.dll`: fixing the prefix does not stick

README Correction B says Proton 10.0-4b installs the two builds into swapped
directories. It does — and swapping them back in the prefix is **useless**,
because `proton` re-copies both at *every* launch (lines 1077–1079):

```python
try_copy(g_proton.lib_dir + "wine/dxvk/x86_64-windows/openvr_api_dxvk.dll", "drive_c/windows/syswow64", ...)
try_copy(g_proton.lib_dir + "wine/dxvk/i386-windows/openvr_api_dxvk.dll",   "drive_c/windows/system32", ...)
```

Verified directly: the files were swapped back by hand, the experiment was run,
and afterwards `system32` again held the 631960-byte i386 build and `syswow64`
the 836760-byte x86_64 one.

The durable workaround is to put a correct i386 build **next to the executable**
— the application directory precedes the system directories in the search order.
`run.sh` stages it, and step 2 of the log confirms the fix took by the size and
PE machine type of the file that was actually loaded, not by assuming:

```
  2a  C:\windows\system32\openvr_api_dxvk.dll   -> NOT LOADED (err=193 BAD_EXE_FORMAT)
  2b  openvr_api_dxvk.dll  -> Z:\...\out\openvr_api_dxvk.dll  631960 bytes, machine 0x014c (i386)
```

Because Proton populates `HKCU\Software\Wine\VR`, DXVK never actually needs it
here — the bug is inert on this configuration. It would bite a prefix without
that registry key.

### Teardown order is not optional

xrizer's OpenXR session is created **on the app's own** `VkInstance`/`VkDevice`
— the ones DXVK owns. Releasing the D3D9 device before shutting the runtime down
destroys them underneath the runtime:

```
0158:warn:vrclient:winIVRClientCore_IVRClientCore_003_Cleanup
      IVRClientCore_IVRClientCore_003_Cleanup failed, status 0xc0000005
```

and the process then never exits (measured: killed at a 300 s timeout, the wine
session left behind). Calling `VR_ShutdownInternal()` first and releasing D3D9
afterwards exits cleanly with rc 0. **A real mod must observe the same order.**

### DXVK gives the render target the usage bits xrizer needs

`GetVulkanImageInfo` reports `usage=0x00080017` on a `D3DUSAGE_RENDERTARGET`
texture, i.e. `TRANSFER_SRC | TRANSFER_DST | SAMPLED | COLOR_ATTACHMENT`
(+ `0x00080000`). `TRANSFER_SRC` is the one that matters: xrizer copies out of
the image as a transfer source. No extra `D3D9VkExtImageDesc` /
`ID3D9VkInteropDevice::CreateImage` dance is needed for a plain render target.

### Handle widths behave exactly as the ABI says

In the 32-bit PE the dispatchable handles are 4 bytes (`VkDevice = 00090cd8`)
while `VkImage` is 8 (`0x0000555572b7dc30` — visibly a host-side pointer).
Proton's `unix_vrcompositor_manual.cpp` has a dedicated `w32_VRVulkanTextureData_t`
path and `unwrap_texture_vkdata()` converts each dispatchable handle with
`p_get_native_VkDevice()` and friends. It works: unlike
`wow64_vrclient_init_params` (Exp. 4, Defect 2), this structure's wow64
conversion is correct as shipped.

`vk_min.h` is hand-written rather than pulled from libvulkan-dev, so
`make vkcheck` compiles it alongside the system `<vulkan/vulkan_core.h>` and
`_Static_assert`s every size, offset and enumerant. Verified against Vulkan
header 1.3.275.

### It also works in the real deployment shape

`make asi` builds the same translation unit as `out/asi/vrsubmit.asi` beside the
repository's own `dist/dinput8.dll`; `out/asi/asihost.exe` stands in for
`BlackOps.exe`. The loader picks the plugin up, the worker thread creates the
D3D9 device and submits, and the run passes with `asihost.exe` exiting 0:

```
PASS-1 .. PASS-7
runtime-side m_nFrameIndex = 200
=== EXPERIMENT 5 END: PASS ===
```

Creating a D3D9 device from an ASI worker thread is fine. The Exp. 4 rule still
applies: `DllMain` must only `CreateThread` and return.

### Monado's XCB compositor window could not be captured

Monado logs `Deferred target backend X11(XCB) Windowed selected!` and creates a
`VkSurfaceKHR` with a 3-image swapchain, but no corresponding window ever
appeared under `xwininfo -root -children` / `-tree` on `DISPLAY=:1` during a
run, at any sampling point. So there is **no screenshot** in this experiment;
the visual channel was not available and is not claimed. The per-frame trace
counts above are the evidence, and they are stronger than a screenshot anyway
because they are exact.

---

## Reproduction

```sh
./run.sh                       # everything: stage, patch, build, run, verify
BO1VR_FRAMES=1800 ./run.sh     # longer run
BO1VR_MONADO_TRACE=0 ./run.sh  # do not touch the packaged monado-service
                               #   (falls back to the coarser journal check)
BO1VR_TRY_DIRECTX=1 ./run.sh   # the control: submit as TextureType_DirectX.
                               #   EXPECTED TO KILL THE PROCESS.
make asi                       # the same probe as a real .asi plugin
make vkcheck                   # re-validate vk_min.h against libvulkan-dev
WORK=/some/dir ./run.sh
```

`run.sh` never writes inside the Steam install: it hard-link copies Proton into
`$WORK/proton` and patches only that copy (Exp. 4's two defects).

Per-frame Monado evidence needs `XRT_COMPOSITOR_LOG=trace`, and the packaged
`monado.service` hard-codes `debug`, so by default `run.sh` stops that unit,
runs its own `monado-service` at trace, and restores the unit on exit (including
on failure — there is an `EXIT` trap). Two gotchas found doing that:

* a stale `$XDG_RUNTIME_DIR/monado.pid` makes a hand-started `monado-service`
  fail with `Failed to init ipc main loop!`;
* `monado-service` `epoll`s **stdin** to notice its terminal disappearing, and
  `epoll_ctl` fails on `/dev/null`, so it must be given a pipe —
  `tail -f /dev/null | monado-service`.

Artifacts after a run: `out/vrsubmit.log`, `out/console.txt` (the
`WINEDEBUG=+vrclient` trace plus xrizer's logging) and `$WORK/monado.log`.

---

## Environment this was verified on

| | |
|---|---|
| OS | Ubuntu 24.04.4 LTS, kernel 6.8 |
| GPU | NVIDIA RTX 3080 Ti, proprietary driver 595.84 (Vulkan 1.4) |
| Proton | 10.0-4b, `PROTON_USE_WOW64=1`, + `tools/patch-proton-wow64-vrclient.py` |
| DXVK | v2.6.2-23-g3cb664e1260926e (the `d3d9.dll` Proton 10.0-4b installs) |
| OpenVR runtime | xrizer `be664bb`, from the WiVRn flatpak |
| OpenXR runtime | Monado 21.0.0 (`21.0.0+git2905.e26a272c1~dfsg1-2build2`), Simulated HMD |
| Interfaces | `IVRCompositor_029`, `IVRSystem_023` |
| Compiler | `i686-w64-mingw32-gcc` 13-win32, `-gdwarf-4`, unstripped |
| Vulkan headers (vkcheck only) | 1.3.275 |

---

## Not proven

* **What the frames look like.** Content is proven at the source (GPU readback
  of the exact submitted surfaces) and delivery is proven at the destination
  (Monado's own per-frame counts), but nothing displays the composited result,
  so a colour-space or eye-swap error inside xrizer's blit would not be caught
  here. Monado's XCB window was never enumerable on X (above).
* **Texture bounds / orientation.** `Submit` is called with `pBounds = NULL`,
  i.e. the full 0..1 range. D3D9 is top-down and OpenVR's convention is not, so
  a vertical flip is plausible and untested.
* **Depth submission**, `Submit_TextureWithPose`, and array textures. Proton's
  `Submit_TextureWithDepth` path explicitly does not unwrap the depth vkdata.
* **Sustained/real frame rates.** 600 frames ran in 10.4 s (≈58 fps) against a
  simulated HMD with two trivially cleared 896×1007 targets. Monado logged
  ~10 % of frames as late even at that load.
* **Reusing the game's own D3D9 device.** This experiment creates its own. A mod
  will have to hook `BlackOps.exe`'s device and share the submission queue with
  it, which is where `LockSubmissionQueue`/`LockDevice` start to matter.
* **Real hardware**, and **`BlackOps.exe`** itself — as in Exp. 4.
* **Proton Experimental 11.** Not tested; it ships no `files/bin-wow64/`.
