# Experiment 1 — OpenVR C API from a mingw-built 32-bit DLL

**Question.** Can a mingw 32-bit DLL call
`VR_GetGenericInterface("FnTable:IVRCompositor_029", &err)` against the shipped
`openvr_api.dll` and get a sane answer rather than a crash?

**Verdict: PASS, up to the point where a live VR runtime is required.**
Everything structural works. The final link — a non-NULL `FnTable` — could not be
reached, for the reason recorded under "Where this stopped".

---

## Setup

Binary under test: `third_party/openvr/bin/win32/openvr_api.dll`, from Valve's
`openvr` repo at master. `IVRCompositor_Version = "IVRCompositor_029"`,
`IVRSystem_Version = "IVRSystem_026"`.

```
$ file third_party/openvr/bin/win32/openvr_api.dll
PE32 executable (DLL) (GUI) Intel 80386, for MS Windows, 5 sections

$ i686-w64-mingw32-objdump -p openvr_api.dll | grep "DLL Name:"
	DLL Name: KERNEL32.dll
	DLL Name: SHELL32.dll
```

Only KERNEL32 and SHELL32 — no MSVC runtime dependency, so it loads under Wine
with nothing extra. Exports are undecorated (`VR_GetGenericInterface`, not
`_VR_GetGenericInterface@8`).

## Run

```
$ make && cd out
$ WINEPREFIX=... WINEARCH=win32 wine host.exe
```

```
host: pid=32, pointer size=4

=== EXPERIMENT 1: OpenVR C API from a mingw 32-bit DLL ===
built with GCC 13-posix, target i686 (32-bit)

[1] LoadLibraryA("openvr_api.dll")
    loaded at 77af0000

[2] GetProcAddress for the C entry points
  VR_GetGenericInterface                        resolved (77af1a40)
  VR_InitInternal                               resolved (77af1c60)
  VR_ShutdownInternal                           resolved (77af1fe0)
  VR_IsHmdPresent                               resolved (77af1d20)
  VR_IsRuntimeInstalled                         resolved (77af1e10)
  VR_GetVRInitErrorAsSymbol                     resolved (77af1c00)
  VR_GetVRInitErrorAsEnglishDescription         resolved (77af1ba0)
  VR_IsInterfaceVersionValid                    resolved (77af1db0)
    0 missing

[3] environment probes
Unable to read VR Path Registry from C:\users\dlynch\AppData\Local\openvr\openvrpaths.vrpath
    VR_IsRuntimeInstalled() = 0
    VR_IsHmdPresent()       = 0
    VR_IsInterfaceVersionValid("IVRCompositor_029") = 0
    VR_IsInterfaceVersionValid("IVRSystem_026") = 0

[4] VR_GetGenericInterface("FnTable:IVRCompositor_029", &err)  [uninitialised]
    returned ptr = 00000000
    err          = 109 (VRInitError_Init_NotInitialized)
    -> survived the call without faulting

[5] VR_InitInternal(&err, VRApplication_Background), then retry
    VR_InitInternal token = 00000000
    err = 110 (VRInitError_Init_PathRegistryNotFound)
    desc: Installation path could not be located (110)
    retry FnTable:IVRCompositor_029 -> ptr=00000000 err=109 (VRInitError_Init_NotInitialized)
    FnTable NULL, as expected without a running compositor
    VR_ShutdownInternal() returned cleanly

=== EXPERIMENT 1 END (reached the end without crashing) ===
```

## What this proves

- The 32-bit `openvr_api.dll` loads under Wine from a mingw-built DLL.
- All eight C entry points resolve by undecorated name.
- The `FnTable:` string protocol is understood: the DLL parses the name and
  returns a *specific, meaningful* error (`109 VRInitError_Init_NotInitialized`)
  rather than a generic failure or a fault.
- **The error path is a returned code, not an access violation.** This is the
  result that matters for the no-HMD code path: we can probe for VR and degrade
  gracefully.
- `VR_ShutdownInternal()` after a failed init is safe.

## Where this stopped, and why

`VR_GetGenericInterface` never returned a non-NULL `FnTable`, so **the `__stdcall`
function-pointer table was never actually called.** That remains unproven.

Cause: no OpenVR runtime is reachable from the test prefix. `VR_InitInternal`
returned `110 VRInitError_Init_PathRegistryNotFound` because the plain Wine prefix
has no `AppData\Local\openvr\openvrpaths.vrpath`.

xrizer *is* installed on this machine (via the WiVRn flatpak) and *is* registered
in the host's `~/.config/openvr/openvrpaths.vrpath`. It was not wired into the
test prefix because:

1. Proton — not plain Wine — is what performs that wiring, in
   `setup_openvr_paths()`. It rewrites the runtime path to `C:\vrclient\` and
   copies its own `vrclient.dll` / `vrclient_x64.dll` into the prefix. Reproducing
   that outside the Steam runtime container was out of scope here.
2. Even wired up, a non-NULL `IVRCompositor` requires a **running compositor**,
   which requires WiVRn to have an actual headset session. No headset is present.

**Unproven, therefore, and worth an early follow-up with hardware:**

- that a non-NULL `FnTable` is returned;
- that its `__stdcall` members are callable from mingw without stack corruption;
- that `Submit()` works from a 32-bit process through the
  OpenVR → xrizer → OpenXR → WiVRn chain.

## Additional finding: 32-bit runtime availability

xrizer ships **only** `bin/linux64/vrclient.so` — there is no `bin/win32/` and no
`bin/linux32/`:

```
$ find .../io.github.wivrn.wivrn/.../files/xrizer
.../xrizer/bin/version.txt
.../xrizer/bin/linux64/vrclient.so
```

Proton supplies the missing 32-bit half itself — it ships
`lib/wine/i386-windows/vrclient.dll` and `lib/wine/i386-unix/vrclient.so` and
points the prefix's vrpath at `C:\vrclient\`. So the 32-bit path is expected to
work *through Proton*, but note it depends on Proton's bridge, not on xrizer
providing a 32-bit build. This is untested here and is the single largest
remaining unknown in the runtime chain.

See also the Proton 10.0-4b `openvr_api_dxvk.dll` placement bug in the top-level
README ("Corrections", item B) — it is on this same path.

---

## Addendum: the 8-byte struct-return ABI claim

The research premise for choosing the C API was that
`IVRSystem::GetHiddenAreaMesh` returns an 8-byte struct from a virtual member
function, that mingw and MSVC disagree about how, and that the C API's `__stdcall`
free function pointers dissolve the problem.

Checked directly. `HiddenAreaMesh_t` is confirmed exactly 8 bytes on i386
(`const HmdVector2_t *` + `uint32_t`). Compiling calls through both a `__stdcall`
and a `__thiscall` function pointer returning it:

```
$ i686-w64-mingw32-gcc -m32 -O1 -c abi.c && i686-w64-mingw32-objdump -d -Mintel abi.o
00000000 <_call_stdcall>:
   0:	sub    esp,0x1c
   3:	mov    DWORD PTR [esp+0x4],0x2
   b:	mov    DWORD PTR [esp],0x1
  12:	call   DWORD PTR [esp+0x20]
  16:	sub    esp,0x8              <- compensating for callee's ret 8
  19:	mov    eax,edx              <- result read from EAX:EDX
  1b:	add    esp,0x1c
  1e:	ret

0000001f <_call_thiscall>:
  1f:	sub    esp,0x1c
  22:	mov    DWORD PTR [esp+0x4],0x2
  2a:	mov    DWORD PTR [esp],0x1
  31:	mov    ecx,DWORD PTR [esp+0x24]   <- `this` in ECX
  35:	call   DWORD PTR [esp+0x20]
  39:	sub    esp,0x8
  3c:	mov    eax,edx              <- identical return handling
  3e:	add    esp,0x1c
  41:	ret
```

**mingw generates identical return-value handling for both.** The struct comes
back in `EAX:EDX`; there is no hidden return pointer in either case; the callee is
expected to pop only the explicit stack arguments.

Consequences:

- Choosing the C API does **not** change the return convention. The C API declares
  `struct HiddenAreaMesh_t (OPENVR_FNTABLE_CALLTYPE *GetHiddenAreaMesh)(...)` —
  by value, exactly like the C++ one. If a divergence exists it exists in both.
- **What MSVC does could not be checked** — there is no MSVC toolchain on this
  machine. The claim "MSVC uses a hidden pointer with `ret 8`" is unverified.

The C API is still the right choice (no vtable layout assumptions, no name
mangling, unambiguous `__stdcall`, Valve's supported binding surface). But
`GetHiddenAreaMesh` specifically should be treated as an **open ABI risk** and
tested against a real runtime before use — not assumed safe because it is reached
through the C API.
