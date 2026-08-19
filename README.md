# bo1-vr

A native VR mod for **Call of Duty: Black Ops (2010)**, built for **Linux** —
the game runs under Proton, and the mod feeds it straight into your headset
through xrizer and any OpenXR runtime (Monado, WiVRn, SteamVR via
OpenComposite-style paths).

No game files are modified. The mod injects into the live, Steam-launched,
CEG-protected `BlackOps.exe` via a DLL shim in the Wine prefix, hooks the
engine from the inside, and does its own D3D9 → Vulkan hand-off to the
compositor through DXVK's interop interfaces.

**Status: working in-headset, not yet packaged for players.** Head-tracked
stereo rendering of the real game is confirmed live on hardware. Motion-control
weapon aim is the current frontier — the injection points are located and a
controller-aimed reticle already draws, but aim is not yet written into the
engine.

## What works today

Everything below is measured, not assumed — each item has a full write-up with
raw output in `experiments/*/RESULTS.md`.

- **Injection with zero writes to the game install** — a `winmm.dll` shim in
  the Proton prefix chain-loads the ASI loader and plugins; CEG needed no
  patching (exp 7, 8, 9)
- **The game's own frames in the headset** — the D3D9 back buffer resolved and
  submitted to the compositor per eye, via DXVK's `ID3D9VkInterop*` interfaces
  (exp 10, 11)
- **True stereo** — two scene renders per frame, per-eye projection with the
  headset's own FOV, world scale (exp 11)
- **Head tracking** — position and orientation, hooked into the engine's
  `R_SetViewParms` with the OpenVR→CoD basis change proven not-mirrored by an
  offline math check (exp 12, 13, 14)
- **Groundwork for motion controls** — controller poses, a reticle drawn where
  the weapon points, viewmodel arms hidden while keeping the weapon
  (exp 12, 13; `docs/`)

In progress (feasibility proven, not yet wired): motion-controlled weapon aim,
ADS with motion controls, physical reload, weapon-bone drive, HUD on a
world-space quad. The `docs/` directory holds the findings for each.

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

There is no one-command player install yet. The current end-to-end setup —
staging the OpenVR runtime, installing the prefix-side shim, and launching —
is scripted per experiment:

- `experiments/09_noinstall/install.sh` — install/remove the loader in the
  game's Proton prefix (touches only the prefix, never the game directory)
- `experiments/11_gameframe/setup-runtime.sh` — stage the xrizer runtime so a
  32-bit PE can load it
- `experiments/11_gameframe/RESULTS.md` §2 — the launch-option story, measured

Start at `experiments/07_ingame/` and read forward if you want the full path
into the running game.

## Repository layout

```
src/                 the ASI loader (dinput8.dll): proxy, VEH, .asi scan, logging
experiments/         numbered, self-contained; each RESULTS.md is the evidence
  00–06                toolchain + VR-chain foundation (no game involved)
  07–14                inside the real game: injection, frames, stereo, tracking
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

Issues and PRs welcome. The house rule this repo was built under: **claims get
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
