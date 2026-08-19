# bo1-vr

A native VR mod for **Call of Duty: Black Ops (2010)**, built for **Linux** (Should work on Windows but is untested, see below) —
the game runs under Proton, and the mod feeds it straight into your headset
through xrizer and any OpenXR runtime (Monado, WiVRn, or SteamVR's OpenXR
runtime).

> Note from maintainer -- I started this project out of curiosity. Its nowhere near complete but its a start. Tested on Ubuntu 24.04 via WiVRn. I plan to continue working on this but have paused for now until I feel like picking up again. Feel free to use/contribute!

No game files are modified. The mod injects into the live, Steam-launched,
CEG-protected `BlackOps.exe` via a DLL shim in the Wine prefix, hooks the
engine from the inside, and does its own D3D9 → Vulkan hand-off to the
compositor through DXVK's interop interfaces.

**Status: working in-headset, not yet packaged for players.** Head-tracked
rendering of the real game is confirmed live on hardware. Motion-control
weapon aim is the current frontier — the injection points are located and a
reticle already draws at the weapon's true aim point, but controller aim is
not yet written into the engine. Single-player and zombies only: the
multiplayer game is a separate executable this project never touches, and
injecting DLLs into any online game is a ban risk regardless.

## What works today

Everything below is measured, not assumed — each item has a full write-up with
raw output in `experiments/*/RESULTS.md`.

- **Injection with zero writes to the game install** — a `winmm.dll` shim in
  the Proton prefix chain-loads the ASI loader and plugins; CEG needed no
  patching (exp 7, 8, 9)
- **The game's own frames in the headset** — the D3D9 back buffer resolved and
  submitted to the compositor per eye, via DXVK's `ID3D9VkInterop*` interfaces
  (exp 10, 11)
- **Stereo rendering** — two scene renders per frame, per-eye projection with
  the headset's own FOV (exp 11)
- **Head tracking** — position and orientation, hooked into the engine's
  `R_SetViewParms` with the OpenVR→CoD basis change proven not-mirrored by an
  offline math check (exp 12, 13, 14)
- **Groundwork for motion controls** — controller poses, and a reticle drawn
  at the weapon's true aim point (exp 11, 12; `docs/`)

In progress (feasibility proven or wired, but not yet playtested): motion-
controlled weapon aim, world scale, hiding the viewmodel arms (`noarms`), ADS
with motion controls, physical reload, weapon-bone drive, HUD on a world-space
quad. The `docs/` directory holds the findings for each.

## Requirements

- A legitimate Steam copy of Call of Duty: Black Ops (app `42700`) on Linux
- Proton 10+ — two of its WoW64 vrclient defects are worked around by
  `tools/patch-proton-wow64-vrclient.py` (applied to a *copy* of Proton)
- [xrizer](https://github.com/Supreeeme/xrizer) registered as the OpenVR
  runtime, on top of any OpenXR runtime (Monado, WiVRn, …)
- To build: the 32-bit MinGW cross toolchain —
  `sudo apt install gcc-mingw-w64-i686 g++-mingw-w64-i686 binutils-mingw-w64-i686`

The whole chain runs on Proton's **new WoW64** (64-bit unix side under the
32-bit game), which is what lets the stock 64-bit xrizer/Monado serve a 32-bit
title. The only per-game Steam configuration is one launch option:

```
PROTON_USE_WOW64=1 PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES=1 %command%
```

## Building and running

```sh
make toolchain-check     # verifies i686-w64-mingw32-gcc is present
make                     # -> dist/dinput8.dll, then runs `make verify`
```

That builds the loader only. There is no one-command player install yet — the
end-to-end setup is scripted per experiment, in this order:

1. `make` — the ASI loader, `dist/dinput8.dll`.
2. `make -C experiments/11_gameframe install` and
   `make -C experiments/13_camera install` — the VR plugins
   (`gameframe.asi`, `camera.asi`), copied to `drive_c/bo1vr` in the game's
   Proton prefix, where the loader scans (override `PLUGDIR=` if your Steam
   library isn't the default one).
3. `tools/patch-proton-wow64-vrclient.py` — apply the two wow64 vrclient
   fixes to a hard-linked copy of Proton, and run the game with that copy.
4. `experiments/09_noinstall/install.sh` — put the winmm shim and loader into
   the prefix (touches only the prefix, never the game directory).
5. `experiments/11_gameframe/setup-runtime.sh` — stage the xrizer runtime so
   a 32-bit PE can load it.
6. Set the launch option from Requirements and start the game from Steam —
   `experiments/11_gameframe/RESULTS.md` §2 is the measured story behind it.

Start at `experiments/07_ingame/` and read forward if you want the full path
into the running game.

## Windows

**Untested — nothing in this repo has ever been run on Windows.** That said,
nothing in the design is Linux-specific either, and most of the setup above is
Linux plumbing that simply doesn't exist on Windows: no Proton to patch, no
WoW64 concerns, no runtime staging, no launch option. The expected recipe:

- **SteamVR** as the OpenVR runtime, in place of xrizer/Monado.
- **DXVK's 32-bit `d3d9.dll` next to `BlackOps.exe`.** This is required, not
  optional: the mod submits frames through DXVK's `ID3D9VkInterop*`
  interfaces, which Microsoft's native D3D9 does not implement.
  [DXVK](https://github.com/doitsujin/dxvk) runs fine on Windows on GPUs with
  current Vulkan drivers.
- **The loader chain in the game directory**: the `winmm.dll` shim, the ASI
  loader, and the `.asi` plugins beside the exe. The shim already handles this
  placement — it resolves the real `winmm.dll` from the system directory when
  no `winmm_real.dll` is present (`experiments/07_ingame/winmm_shim.c`).
- **Building**: the same MinGW toolchain via MSYS2's mingw32 environment, or
  cross-compile from Linux/WSL exactly as above.

The honest unknowns, in this repo's spirit of not claiming what hasn't been
measured: whether SteamVR's compositor accepts these Vulkan submissions as
readily as xrizer does, and whether CEG on Windows tolerates the extra DLLs in
the game directory the way it demonstrably does under Proton (exp 7). If you
try it, an issue reporting the result — either way — would be genuinely
useful.

## Repository layout

```
src/                 the ASI loader (dinput8.dll): proxy, VEH, .asi scan, logging
experiments/         numbered, self-contained; each RESULTS.md is the evidence
  00–06                toolchain + VR-chain foundation (no game involved)
  07–14                the real-game phase: injection, frames, stereo, tracking
                       (12 and 14 are offline modules feeding it)
docs/                engine findings and plans (camera, input, weapons, HUD, …)
  toolchain-notes.md   the build/configuration decisions and their reasons
  address-map.md       verified BlackOps.exe addresses this work rests on
research/engine/     scripts + dumps from reverse-engineering the binary
tools/               headless Ghidra harness, Proton wow64 vrclient patcher
third_party/         vendored MinHook and OpenVR (see their licenses)
```

Two documents are load-bearing if you want to contribute:
[docs/toolchain-notes.md](docs/toolchain-notes.md) (why the build is configured
the way it is — several decisions have expensive-to-rediscover failure modes)
and [docs/camera-hook-plan.md](docs/camera-hook-plan.md) (how the engine side
fits together).

## Contributing

Issues and PRs welcome. A note on ids you'll see around the repo: `BAC-nnn`
(and the occasional `#nn`) refer to the private issue tracker this was built
against before publication — they're kept for provenance, and everything they
decided is written out in `docs/` and the RESULTS files.

The house rule this repo was built under: **claims get
measured**. Every experiment records what was actually observed, negative
results included, and the docs mark unverified statements as ASSUMED. Keeping
that discipline is what makes the address map and the findings trustworthy.

## Legal

This project contains no game code, assets, or copyrighted material from
Call of Duty: Black Ops. You must own the game. The research notes document
addresses, strings, and structures observed in the binary for interoperability
purposes. This project is not affiliated with or endorsed by Activision or
Treyarch.

## License

[MIT](LICENSE). Vendored third-party components keep their own licenses:
[MinHook](third_party/minhook/LICENSE.txt) (BSD-2),
[OpenVR](third_party/openvr/LICENSE) (BSD-3).
