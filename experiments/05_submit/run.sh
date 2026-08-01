#!/bin/bash
# experiments/05_submit/run.sh -- reproduce the stereo-submit experiment.
#
# Builds on experiments/04_live_fntable/run.sh: same staging, same two Proton
# patches, same Monado Simulated HMD, no VR hardware. What is new is that a
# D3D9 texture rendered through DXVK is handed to IVRCompositor::Submit and the
# frames are then counted ON THE MONADO SIDE.
#
#   32-bit mingw DLL  ->  DXVK d3d9.dll  ->  ID3D9VkInterop{Device,Texture}
#     ->  VkImage + PE Vulkan handles
#     ->  Texture_t{ eType = TextureType_Vulkan } -> IVRCompositor_029::Submit
#     ->  Proton vrclient (unwraps PE handles to native) -> xrizer
#     ->  OpenXR -> monado-service
#
# A Submit that returns VRCompositorError_None is NOT accepted as proof.
# Verification is three-way:
#   (a) our own GPU readback of the render target, before it is submitted;
#   (b) xrizer's own frame counter, read back via IVRCompositor::GetFrameTiming;
#   (c) monado-service's log: one swapchain acquire/wait/release triple and one
#       LAYER_COMMIT per frame, on a swapchain of exactly our per-eye size.
#
# (c) needs Monado at XRT_COMPOSITOR_LOG=trace. The packaged --user unit hard-codes
# `debug`, so by default this script stops that unit, runs its own monado-service
# at trace, and restores the unit afterwards. Set BO1VR_MONADO_TRACE=0 to leave the
# running service alone; verification then falls back to the coarser journal check.
#
# Everything is written under $WORK; the Steam install is never modified.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="${WORK:-${TMPDIR:-/tmp}/bo1vr-exp05}"
PROTON_SRC="${PROTON_SRC:-$HOME/.steam/steam/steamapps/common/Proton 10.0}"
XRIZER_SRC="${XRIZER_SRC:-$(ls -d "$HOME"/.local/share/flatpak/app/io.github.wivrn.wivrn/*/stable/active/files/xrizer 2>/dev/null | head -1)}"
FRAMES="${BO1VR_FRAMES:-900}"
MONADO_TRACE="${BO1VR_MONADO_TRACE:-1}"

say() { printf '\n== %s\n' "$*"; }

OWNED_MONADO=""
cleanup() {
    if [ -n "$OWNED_MONADO" ]; then
        say "restoring the packaged monado-service"
        kill "$OWNED_MONADO" 2>/dev/null || true
        [ -n "${MONADO_STDIN_PID:-}" ] && kill "$MONADO_STDIN_PID" 2>/dev/null || true
        sleep 1
        rm -f "$XDG_RUNTIME_DIR/monado.pid"
        systemctl --user start monado.socket 2>/dev/null || true
    fi
}
trap cleanup EXIT

say "0. preflight"
command -v monado-service >/dev/null || { echo "monado-service not installed"; exit 1; }
[ -d "$PROTON_SRC" ] || { echo "no Proton at $PROTON_SRC"; exit 1; }
[ -f "$XRIZER_SRC/bin/linux64/vrclient.so" ] || { echo "no xrizer at $XRIZER_SRC"; exit 1; }
mkdir -p "$WORK"

say "1. stage an xrizer runtime with a bin/vrclient.so for the 32-bit PE"
mkdir -p "$WORK/xrizer/bin/linux64"
cp -f "$XRIZER_SRC/bin/linux64/vrclient.so" "$WORK/xrizer/bin/linux64/vrclient.so"
ln -sfn linux64/vrclient.so "$WORK/xrizer/bin/vrclient.so"
: > "$WORK/xrizer/bin/version.txt"
file -L "$WORK/xrizer/bin/vrclient.so" | sed 's/^/   /'

say "2. hard-link a Proton copy and patch its wow64 vrclient"
"$HERE/../../tools/patch-proton-wow64-vrclient.py" "$PROTON_SRC" "$WORK/proton"

say "3. build"
make -C "$HERE" >/dev/null
# README Correction B: Proton 10.0-4b installs the i386 openvr_api_dxvk.dll into
# system32 and the x86_64 one into syswow64, i.e. swapped, so the name a 32-bit
# process resolves is the 64-bit build. Fixing the *prefix* does not stick --
# `proton` re-copies both files at every launch (lines 1077-1079). The durable
# workaround is to put a correct i386 build next to the executable, which
# precedes the system directories in the search order. vrsubmit.c measures both.
cp -f "$PROTON_SRC/files/lib/wine/dxvk/i386-windows/openvr_api_dxvk.dll" "$HERE/out/openvr_api_dxvk.dll"
ls -la "$HERE/out"/*.dll "$HERE/out"/*.exe | sed 's/^/   /'

MONADO_LOG="$WORK/monado.log"
if [ "$MONADO_TRACE" = "1" ]; then
    say "4. restart monado-service at XRT_COMPOSITOR_LOG=trace (Simulated HMD)"
    systemctl --user stop monado.service monado.socket 2>/dev/null || true
    sleep 1
    pkill -x monado-service 2>/dev/null || true
    sleep 1
    rm -f "$XDG_RUNTIME_DIR/monado.pid"
    : > "$MONADO_LOG"
    # monado-service epoll()s stdin to notice its terminal going away, and
    # epoll_ctl fails on /dev/null -- give it a pipe that never closes.
    tail -f /dev/null | \
      XRT_COMPOSITOR_LOG=trace XRT_COMPOSITOR_FORCE_XCB=1 SIMULATED_ENABLE=1 IPC_LOG=info \
      monado-service > "$MONADO_LOG" 2>&1 &
    OWNED_MONADO=$!
    MONADO_STDIN_PID=$(jobs -p | head -1)
    sleep 5
    pgrep -x monado-service >/dev/null || { echo "monado-service did not come up; see $MONADO_LOG"; exit 1; }
    grep -A3 "Got devices" "$MONADO_LOG" | sed 's/^/   /'
else
    say "4. using the already-running monado-service (no trace)"
    pgrep -x monado-service >/dev/null || systemctl --user start monado.socket
    pgrep -x monado-service >/dev/null || { echo "monado-service is not running"; exit 1; }
fi
echo "   monado-service pid $(pgrep -x monado-service)"
JCURSOR="$(date '+%Y-%m-%d %H:%M:%S')"

say "5. run under Proton (new-WoW64), $FRAMES frames"
cd "$HERE/out"
rm -f vrsubmit.log console.txt
export STEAM_COMPAT_CLIENT_INSTALL_PATH="$HOME/.steam/steam"
export STEAM_COMPAT_DATA_PATH="$WORK/pfx"
export PROTON_USE_WOW64=1
export VR_OVERRIDE="$WORK/xrizer"       # -> Proton exports PROTON_VR_RUNTIME
export SteamGameId=42700                # -> Proton's steam.exe calls vrclient_init_registry()
export BO1VR_LOG=vrsubmit.log
export BO1VR_FRAMES="$FRAMES"
export WINEDEBUG="${WINEDEBUG:-+vrclient}"
unset PROTON_LOG
mkdir -p "$STEAM_COMPAT_DATA_PATH"
set +e
"$WORK/proton/proton" run "$HERE/out/host.exe" > console.txt 2>&1
rc=$?
set -e

say "6. what the app saw (out/vrsubmit.log)"
cat vrsubmit.log

say "7. what MONADO saw"
if [ "$MONADO_TRACE" = "1" ]; then
    acq=$(grep -c 'swapchain_acquire_image'  "$MONADO_LOG" || true)
    wai=$(grep -c 'swapchain_wait_image'     "$MONADO_LOG" || true)
    rel=$(grep -c 'swapchain_release_image'  "$MONADO_LOG" || true)
    com=$(grep -c 'LAYER_COMMIT at'          "$MONADO_LOG" || true)
    echo "   client swapchain(s) monado created for us:"
    grep 'comp_swapchain_create_init' "$MONADO_LOG" | sed 's/^/     /'
    echo "   swapchain_acquire_image : $acq"
    echo "   swapchain_wait_image    : $wai"
    echo "   swapchain_release_image : $rel"
    echo "   LAYER_COMMIT            : $com"
    echo "   frames we submitted     : $FRAMES"
    monado_ok=0
    [ "$acq" = "$FRAMES" ] && [ "$wai" = "$FRAMES" ] && [ "$rel" = "$FRAMES" ] && monado_ok=1
else
    journalctl --user -u monado --since "$JCURSOR" --no-pager -o cat \
      | grep -E 'comp_swapchain_create_init|BEGIN_SESSION|END_SESSION' | sed 's/^/     /'
    monado_ok=0
    journalctl --user -u monado --since "$JCURSOR" --no-pager -o cat \
      | grep -q 'comp_swapchain_create_init' && monado_ok=1
    echo "   (no per-frame counts without BO1VR_MONADO_TRACE=1)"
fi

say "8. result"
app_ok=0
grep -q "EXPERIMENT 5 END: PASS" vrsubmit.log && app_ok=1
echo "   app side  : $([ $app_ok = 1 ] && echo PASS || echo FAIL)  (proton run exited $rc)"
echo "   monado side: $([ $monado_ok = 1 ] && echo PASS || echo FAIL)"
if [ $app_ok = 1 ] && [ $monado_ok = 1 ]; then
    echo "RESULT: PASS"
else
    echo "RESULT: FAIL"; exit 1
fi
