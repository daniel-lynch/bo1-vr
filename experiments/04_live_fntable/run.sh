#!/bin/bash
# experiments/04_live_fntable/run.sh -- reproduce the gating experiment.
#
# Brings up the whole chain with NO VR HARDWARE and runs out/host.exe:
#
#   32-bit mingw DLL (vrlive.dll)
#     -> Valve openvr_api.dll (32-bit PE)
#     -> C:\vrclient\bin\vrclient.dll        (Proton's 32-bit PE bridge)
#     -> x86_64-unix/vrclient.so             (Proton's unixlib, via new-WoW64)
#     -> $XRIZER/bin/vrclient.so             (xrizer, 64-bit)
#     -> OpenXR loader -> monado-service      (Simulated HMD)
#
# Everything is written under $WORK; the Steam install is never modified.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="${WORK:-${TMPDIR:-/tmp}/bo1vr-exp04}"
PROTON_SRC="${PROTON_SRC:-$HOME/.steam/steam/steamapps/common/Proton 10.0}"
# Where xrizer's 64-bit vrclient.so lives. Default: the WiVRn flatpak.
XRIZER_SRC="${XRIZER_SRC:-$(ls -d "$HOME"/.local/share/flatpak/app/io.github.wivrn.wivrn/*/stable/active/files/xrizer 2>/dev/null | head -1)}"

say() { printf '\n== %s\n' "$*"; }

say "0. preflight"
command -v monado-service >/dev/null || { echo "monado-service not installed"; exit 1; }
[ -d "$PROTON_SRC" ] || { echo "no Proton at $PROTON_SRC"; exit 1; }

# Monado must be up with a device. The Ubuntu package is socket-activated as a
# --user unit; if it is not running, start it in the background. Monado's legacy
# prober falls back to the Simulated HMD when no hardware is probed, which is
# exactly what we want. SIMULATED_ENABLE=1 forces it even if hardware appears.
if ! pgrep -x monado-service >/dev/null; then
  say "0b. starting monado-service (simulated HMD)"
  systemctl --user start monado.socket 2>/dev/null || {
    SIMULATED_ENABLE=1 XRT_COMPOSITOR_FORCE_XCB=1 monado-service >"$WORK/monado.log" 2>&1 &
    sleep 3
  }
fi
pgrep -x monado-service >/dev/null || { echo "monado-service is not running"; exit 1; }
echo "monado-service pid $(pgrep -x monado-service)"

mkdir -p "$WORK"

say "1. stage an xrizer runtime with a bin/vrclient.so for the 32-bit PE"
# Proton's 32-bit PE vrclient.dll appends "/bin/vrclient.so" to the runtime path
# (the 64-bit PE appends "/bin/linux64/vrclient.so"). The #if that chooses is on
# the *PE* architecture, so it still says /bin/vrclient.so under new-WoW64 --
# where the unix side is 64-bit and therefore wants a 64-bit .so there.
[ -f "$XRIZER_SRC/bin/linux64/vrclient.so" ] || { echo "no xrizer at $XRIZER_SRC"; exit 1; }
mkdir -p "$WORK/xrizer/bin/linux64"
cp -f "$XRIZER_SRC/bin/linux64/vrclient.so" "$WORK/xrizer/bin/linux64/vrclient.so"
ln -sfn linux64/vrclient.so "$WORK/xrizer/bin/vrclient.so"
: > "$WORK/xrizer/bin/version.txt"
file -L "$WORK/xrizer/bin/vrclient.so" | sed 's/^/   /'

say "2. hard-link a Proton copy and patch its wow64 vrclient"
"$HERE/../../tools/patch-proton-wow64-vrclient.py" "$PROTON_SRC" "$WORK/proton"

say "3. build"
make -C "$HERE" >/dev/null
ls -la "$HERE/out"/*.dll "$HERE/out"/*.exe | sed 's/^/   /'

say "4. run under Proton (new-WoW64)"
cd "$HERE/out"
rm -f vrlive.log
export STEAM_COMPAT_CLIENT_INSTALL_PATH="$HOME/.steam/steam"
export STEAM_COMPAT_DATA_PATH="$WORK/pfx"
export PROTON_USE_WOW64=1
export VR_OVERRIDE="$WORK/xrizer"       # -> Proton exports PROTON_VR_RUNTIME
export SteamGameId=42700                # -> Proton's steam.exe calls vrclient_init_registry()
export BO1VR_LOG=vrlive.log
export WINEDEBUG="${WINEDEBUG:-+vrclient}"
unset PROTON_LOG                        # PROTON_LOG=1 enables +seh and writes GB/min
mkdir -p "$STEAM_COMPAT_DATA_PATH"
set +e
"$WORK/proton/proton" run "$HERE/out/host.exe" > console.txt 2>&1
rc=$?
set -e

say "5. result (out/vrlive.log; wine trace in out/console.txt)"
cat vrlive.log
echo
if grep -q "EXPERIMENT 4 END: PASS" vrlive.log; then
  echo "RESULT: PASS"
else
  echo "RESULT: FAIL (proton run exited $rc)"; exit 1
fi
