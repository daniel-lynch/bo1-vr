#!/bin/bash
# experiments/06_visual/run.sh -- photograph what the compositor actually got.
#
# Experiment 5 proved the frames arrive. It could not show what they look like,
# so an eye swap, a vertical flip or a colour-space error would all have passed
# it. This run closes that: it submits a per-eye test pattern with an
# unambiguous up/down and left/right, then SCREENSHOTS MONADO'S OWN COMPOSITOR
# OUTPUT and reads the answer out of the pixels.
#
#   32-bit mingw DLL -> DXVK d3d9 -> ID3D9VkInterop{Device,Texture}
#     -> Texture_t{ eType = TextureType_Vulkan } -> IVRCompositor_029::Submit
#     -> Proton vrclient -> xrizer -> OpenXR -> monado-service
#     -> XRT_WINDOW_PEEK window  ->  xwd(1)  ->  out/*.png  ->  analyse.py
#
# The observation channel needed two environment defects worked around, both in
# a Vulkan layer built here and loaded ONLY by monado-service -- see
# vkxlibsurface.c for the measurements behind each. Monado itself is unmodified,
# and nothing is installed system-wide.
#
# FIVE RUNS, because one picture on its own proves less than it looks. A
# photograph that looks right is worth nothing unless a deliberately wrong input
# produces a visibly wrong photograph, so three of the five exist only to make
# the instrument prove it responds:
#
#   null     the answer. Normal submit, both eyes side by side.
#   swap     CONTROL: eye 0's texture is submitted as Eye_Right and vice versa.
#            The two halves MUST swap. If they do not, the picture is not of what
#            we submitted and nothing else here means anything.
#   dflip    CONTROL: the pattern is drawn upside down IN OUR OWN D3D9 TEXTURE.
#            The picture MUST come out upside down. This is what proves the
#            vertical axis is genuinely being observed.
#   left1    XRT_WINDOW_PEEK=left -- Monado shows the LEFT eye alone, so "left
#            content is in the left eye" stops resting on an assumption about
#            which half of the side-by-side view is which.
#   vbounds  FINDING, not a control: Submit() with VRTextureBounds_t{0,1,1,0}
#            instead of pBounds=NULL. The picture does NOT change -- xrizer
#            ignores pBounds entirely. Kept in the default set because that is a
#            constraint a real mod has to know about, and because it was
#            originally written as the control and failed to be one.
#
# Everything is written under $WORK; the Steam install is never modified. The
# packaged monado.socket is stopped for the duration and restored by an EXIT
# trap, including on failure.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="${WORK:-${TMPDIR:-/tmp}/bo1vr-exp06}"
PROTON_SRC="${PROTON_SRC:-$HOME/.steam/steam/steamapps/common/Proton 10.0}"
XRIZER_SRC="${XRIZER_SRC:-$(ls -d "$HOME"/.local/share/flatpak/app/io.github.wivrn.wivrn/*/stable/active/files/xrizer 2>/dev/null | head -1)}"
FRAMES="${BO1VR_FRAMES:-2400}"
DISP="${BO1VR_DISPLAY:-${DISPLAY:-:1}}"
MODES="${BO1VR_MODES:-null swap dflip left1 vbounds}"
RAISE="${BO1VR_RAISE:-0}"
SHOTS="${BO1VR_SHOTS:-3}"

say() { printf '\n== %s\n' "$*"; }

MONADO_LOG="$WORK/monado.log"
OWNED_MONADO=""

# monado-service does not always go away on the first SIGTERM, and a survivor is
# not harmless: a second instance races for $XDG_RUNTIME_DIR/monado_comp_ipc and
# the client can end up talking to the one WITHOUT the peek window. Measured --
# an earlier run of this script ended up with three of them. So insist.
stop_all_monado() {
    local i
    systemctl --user stop monado.service monado.socket 2>/dev/null || true
    for i in 1 2 3 4 5; do
        pgrep -x monado-service >/dev/null || return 0
        pkill -x monado-service 2>/dev/null || true
        sleep 1
    done
    pkill -9 -x monado-service 2>/dev/null || true
    sleep 2
    if pgrep -x monado-service >/dev/null; then
        echo "could not stop the running monado-service (pids: $(pgrep -x monado-service | tr '\n' ' '))"
        return 1
    fi
    return 0
}

cleanup() {
    if [ -n "$OWNED_MONADO" ]; then
        say "restoring the packaged monado-service"
        stop_all_monado || true
        rm -f "$XDG_RUNTIME_DIR/monado.pid"
        systemctl --user start monado.socket 2>/dev/null || true
    fi
}
trap cleanup EXIT

# ---------------------------------------------------------------- preflight --
say "0. preflight"
command -v monado-service >/dev/null || { echo "monado-service not installed"; exit 1; }
command -v xwininfo >/dev/null || { echo "need xwininfo (x11-utils)"; exit 1; }
command -v xwd     >/dev/null || { echo "need xwd (x11-apps)"; exit 1; }
command -v convert >/dev/null || { echo "need convert (imagemagick)"; exit 1; }
python3 -c 'import PIL' 2>/dev/null || { echo "need python3-pil"; exit 1; }
[ -d "$PROTON_SRC" ] || { echo "no Proton at $PROTON_SRC"; exit 1; }
[ -f "$XRIZER_SRC/bin/linux64/vrclient.so" ] || { echo "no xrizer at $XRIZER_SRC"; exit 1; }
DISPLAY="$DISP" xwininfo -root >/dev/null 2>&1 || { echo "no X server on DISPLAY=$DISP"; exit 1; }
mkdir -p "$WORK"
echo "   DISPLAY=$DISP, work dir $WORK"

say "1. stage an xrizer runtime with a bin/vrclient.so for the 32-bit PE"
mkdir -p "$WORK/xrizer/bin/linux64"
cp -f "$XRIZER_SRC/bin/linux64/vrclient.so" "$WORK/xrizer/bin/linux64/vrclient.so"
ln -sfn linux64/vrclient.so "$WORK/xrizer/bin/vrclient.so"
: > "$WORK/xrizer/bin/version.txt"

say "2. hard-link a Proton copy and patch its wow64 vrclient (Exp. 4's two defects)"
"$HERE/../../tools/patch-proton-wow64-vrclient.py" "$PROTON_SRC" "$WORK/proton"

say "3. build (32-bit probe + the native Vulkan layer that unblocks the peek window)"
make -C "$HERE" >/dev/null
cp -f "$PROTON_SRC/files/lib/wine/dxvk/i386-windows/openvr_api_dxvk.dll" "$HERE/out/openvr_api_dxvk.dll"
ls -la "$HERE/out"/*.dll "$HERE/out"/*.exe "$HERE/out"/*.so | sed 's/^/   /'

# ------------------------------------------------------------------- monado --
# $1 = XRT_WINDOW_PEEK value
start_monado() {
    OWNED_MONADO=1
    stop_all_monado || exit 1
    rm -f "$XDG_RUNTIME_DIR/monado.pid"
    # monado-service epoll()s stdin to notice its terminal going away, and
    # epoll_ctl fails on /dev/null -- give it a pipe that never closes (Exp. 5).
    tail -f /dev/null | \
      DISPLAY="$DISP" \
      VK_LAYER_PATH="$HERE/out" VK_INSTANCE_LAYERS=VK_LAYER_BO1VR_xlib_surface \
      XRT_COMPOSITOR_LOG=trace XRT_WINDOW_PEEK="$1" XRT_COMPOSITOR_FORCE_XCB=1 \
      SIMULATED_ENABLE=1 IPC_LOG=info \
      monado-service >> "$MONADO_LOG" 2>&1 &
    disown
    sleep 5
    local pids; pids="$(pgrep -x monado-service | tr '\n' ' ')"
    [ -n "$pids" ] || { echo "monado-service did not come up; see $MONADO_LOG"; exit 1; }
    [ "$(echo $pids | wc -w)" = 1 ] || { echo "more than one monado-service: $pids"; exit 1; }
    echo "   monado-service pid $pids XRT_WINDOW_PEEK=$1"
}

# The peek window is an SDL window owned by monado-service. It is NOT called
# "Monado" -- Monado titles it after the HMD ("Simulated HMD" here), and both it
# and its window-manager frame carry that title, so match on WM_CLASS instead.
# Experiment 5 searched for a window called "Monado" and concluded none existed.
#
# It is also not a stable window. Monado destroys the peek window and builds a
# new one when the compositor swapchain is resized, which happens while a
# session settles, so an id read a second ago can already be a Bad Drawable.
# Hence: return only an id that still answers, and re-resolve before every
# capture attempt.
peek_id() {
    local id wh
    for id in $(DISPLAY="$DISP" xwininfo -root -tree 2>/dev/null \
                | grep '("monado-service" "monado-service")' \
                | grep -oE '0x[0-9a-f]+'); do
        wh="$(DISPLAY=$DISP xwininfo -id "$id" 2>/dev/null \
              | awk '/Width:/{w=$2} /Height:/{h=$2} END{print w"x"h}')"
        case "$wh" in ""|x|1x1|5x5) continue ;; esac
        echo "$id"
        return 0
    done
    return 1
}

# Capture the peek window into PNG $1. Pass "frame" as $2 to photograph the
# window-manager frame (title bar included) instead of the client area.
#
# xwd, NOT ImageMagick's `import`. `import` calls XGrabServer for the duration
# of the capture, and while the server is grabbed monado-service cannot present
# to its X11 swapchain -- so the compositor stalls, the app blocks in
# WaitGetPoses, and import waits for a window that can never repaint. That is a
# three-way deadlock that takes the whole desktop with it; it happened here, and
# `X rc=124` on every subsequent xwininfo is what it looks like. xwd only ever
# calls XGetImage (nm -D confirms: XGrabPointer/XUngrabPointer and nothing
# else), and the .xwd -> .png conversion afterwards never touches X at all.
#
# The result is also REFUSED unless its dimensions are the window's: a capture
# tool that quietly returns something other than the window it was asked for
# produces a screenshot that analyses as "UNREADABLE" rather than as "the
# capture failed", which is precisely the kind of plausible-looking wrong answer
# this experiment exists to catch.
grab() {
    local out="$1" extra="${2:-}" id wh got i
    for i in 1 2 3 4 5 6 7 8; do
        id="$(peek_id)" || { sleep 1; continue; }
        # "frame" -> photograph the window-manager frame instead, so the picture
        # carries a title bar and is self-evidently a window on a desktop.
        # xwd's own -frame flag does not do this for an explicit -id.
        if [ "$extra" = frame ]; then
            id="$(DISPLAY=$DISP xwininfo -id "$id" 2>/dev/null | awk '/Parent window id:/{print $4}')"
            [ -n "$id" ] || { sleep 1; continue; }
        fi
        wh="$(DISPLAY=$DISP xwininfo -id "$id" 2>/dev/null \
              | awk '/Width:/{w=$2} /Height:/{h=$2} END{print w"x"h}')"
        if [ -n "$wh" ] && [ "$wh" != x ] \
           && timeout 20 xwd -display "$DISP" -silent -id "$id" > "$WORK/grab.xwd" 2>/dev/null \
           && convert "$WORK/grab.xwd" "$out" 2>/dev/null; then
            got="$(python3 -c "from PIL import Image; import sys
try:
    print('%dx%d' % Image.open(sys.argv[1]).size)
except Exception:
    print('none')" "$out" 2>/dev/null)"
            [ -n "$extra" ] && return 0          # framed shot: any size is fine
            [ "$got" = "$wh" ] && return 0
        fi
        [ "$RAISE" = "1" ] && wmctrl -i -a "$id" 2>/dev/null || true
        sleep 1
    done
    echo "   grab: gave up, window is ${wh:-gone} but the capture came out ${got:-none}"
    rm -f "$out"
    return 1
}

# ---------------------------------------------------------------- one round --
# $1 = mode label, $2 = env var to set in the probe ("" for the plain run)
run_once() {
    local mode="$1" var="$2" id="" i rc=0
    say "5.$mode  run under Proton (new-WoW64), $FRAMES frames${var:+, $var}"

    cd "$HERE/out"
    rm -f "visual_$mode.log" "console_$mode.txt" "peek_${mode}"_*.png
    export STEAM_COMPAT_CLIENT_INSTALL_PATH="$HOME/.steam/steam"
    export STEAM_COMPAT_DATA_PATH="$WORK/pfx"
    export PROTON_USE_WOW64=1
    export VR_OVERRIDE="$WORK/xrizer"   # -> Proton exports PROTON_VR_RUNTIME
    export SteamGameId=42700            # -> Proton's steam.exe calls vrclient_init_registry()
    export BO1VR_LOG="visual_$mode.log"
    export BO1VR_FRAMES="$FRAMES"
    export WINEDEBUG="${WINEDEBUG:-+vrclient}"
    unset PROTON_LOG BO1VR_FLIP_V BO1VR_SWAP_EYES BO1VR_DRAW_FLIP
    [ -n "$var" ] && export "$var=1"
    mkdir -p "$STEAM_COMPAT_DATA_PATH"

    set +e
    "$WORK/proton/proton" run "$HERE/out/host.exe" > "console_$mode.txt" 2>&1 &
    local app=$!

    for i in $(seq 1 120); do
        id="$(peek_id)"
        [ -n "$id" ] && break
        kill -0 $app 2>/dev/null || break
        sleep 1
    done
    if [ -n "$id" ]; then
        echo "   peek window: $id  $(DISPLAY=$DISP xwininfo -id "$id" 2>/dev/null | awk '/Width:/{w=$2} /Height:/{h=$2} END{print w"x"h}')"
        [ "$RAISE" = "1" ] && wmctrl -i -a "$id" 2>/dev/null
        sleep 10       # let the session settle: the peek window is torn down and
                       # rebuilt once as the compositor sizes its swapchain
        for i in $(seq 1 "$SHOTS"); do
            grab "peek_${mode}_$i.png"
            sleep 2
        done
        # the same thing with its window-manager frame, purely so the committed
        # artefact is obviously a screenshot of a real window on a real desktop
        grab "peek_${mode}_framed.png" frame || true
    else
        echo "   FAILED to find the peek window"
    fi

    wait $app; rc=$?
    set -e
    echo "   proton run exited $rc"
    return 0
}

# $1 = mode, $2 = extra analyse.py args
analyse() {
    local mode="$1" extra="$2" rw rh shot i
    rw="$(grep -oE 'render target [0-9]+ x [0-9]+' "$HERE/out/visual_$mode.log" | awk '{print $3}')"
    rh="$(grep -oE 'render target [0-9]+ x [0-9]+' "$HERE/out/visual_$mode.log" | awk '{print $5}')"
    : "${rw:=896}" ; : "${rh:=1007}"
    shot=""
    for i in $(seq "$SHOTS" -1 1); do
        [ -f "$HERE/out/peek_${mode}_$i.png" ] && { shot="$HERE/out/peek_${mode}_$i.png"; break; }
    done
    say "6.$mode  what the RUNTIME received ($(basename "${shot:-<none>}"), source ${rw}x${rh})"
    if [ -z "$shot" ]; then echo "   no screenshot captured"; return 2; fi
    python3 "$HERE/analyse.py" "$shot" --rw "$rw" --rh "$rh" $extra \
        --json "$HERE/out/analysis_$mode.json" | tee "$HERE/out/analysis_$mode.txt"
    return "${PIPESTATUS[0]}"
}

# -------------------------------------------------------------------- go -----
: > "$MONADO_LOG"
declare -A VERDICT
for mode in $MODES; do
    case "$mode" in
        null)    say "4.$mode  monado-service: Simulated HMD, XRT_WINDOW_PEEK=both"
                 start_monado both; run_once null    "" ;;
        swap)    say "4.$mode  monado-service: Simulated HMD, XRT_WINDOW_PEEK=both"
                 start_monado both; run_once swap    BO1VR_SWAP_EYES ;;
        dflip)   say "4.$mode  monado-service: Simulated HMD, XRT_WINDOW_PEEK=both"
                 start_monado both; run_once dflip   BO1VR_DRAW_FLIP ;;
        vbounds) say "4.$mode  monado-service: Simulated HMD, XRT_WINDOW_PEEK=both"
                 start_monado both; run_once vbounds BO1VR_FLIP_V ;;
        left1)   say "4.$mode  monado-service: Simulated HMD, XRT_WINDOW_PEEK=left"
                 start_monado left; run_once left1   "" ;;
        *) echo "unknown mode $mode"; exit 1 ;;
    esac
    set +e
    case "$mode" in
        left1) analyse "$mode" "--single left" ;;
        *)     analyse "$mode" "" ;;
    esac
    VERDICT[$mode]=$?
    set -e
done

say "7. what MONADO saw (per-frame, from its own trace log)"
acq=$(grep -c 'swapchain_acquire_image' "$MONADO_LOG" || true)
rel=$(grep -c 'swapchain_release_image' "$MONADO_LOG" || true)
com=$(grep -c 'LAYER_COMMIT at'          "$MONADO_LOG" || true)
grep -m2 'comp_swapchain_create_init' "$MONADO_LOG" | sed 's/^/   /' || true
echo "   swapchain_acquire_image : $acq"
echo "   swapchain_release_image : $rel"
echo "   LAYER_COMMIT            : $com"
echo "   frames we submitted     : $((FRAMES * $(echo $MODES | wc -w)))"

# --------------------------------------------------------------- the verdict -
say "8. keep the evidence (out/ is gitignored; images/ is not)"
mkdir -p "$HERE/images"
for mode in $MODES; do
    for f in "$HERE/out/peek_${mode}"_*.png "$HERE/out/analysis_$mode.txt"; do
        [ -f "$f" ] && cp -f "$f" "$HERE/images/"
    done
done
ls -la "$HERE/images" | sed 's/^/   /'

say "9. result"
all_ok=1
for mode in $MODES; do
    app_ok=0
    grep -q "EXPERIMENT 6 END: PASS" "$HERE/out/visual_$mode.log" 2>/dev/null && app_ok=1
    [ $app_ok = 1 ] || all_ok=0
    # "reads clean" vs "reads different" is a description, not a judgement: two
    # of these runs are controls and MUST read different. The ok_if lines below
    # are where each mode is held to what it was supposed to do.
    printf '   %-7s app %s   picture %s\n' "$mode" \
        "$([ $app_ok = 1 ] && echo PASS || echo FAIL)" \
        "$(case ${VERDICT[$mode]} in
             0) echo 'reads exactly as drawn';;
             1) echo 'reads different from what was drawn';;
             *) echo 'NO IMAGE CAPTURED';;
           esac)"
done

ok_if() {  # $1 = condition already evaluated as 0/1, $2.. = what it means
    local c="$1"; shift
    if [ "$c" = 0 ]; then echo "   OK   $*"; else echo "   BAD  $*"; all_ok=0; fi
}
saw() { grep -q "$2" "$HERE/out/analysis_$1.txt" 2>/dev/null && echo 0 || echo 1; }

case "$MODES" in *null*)
    ok_if "$([ "${VERDICT[null]:-9}" = 0 ] && echo 0 || echo 1)" \
        "THE ANSWER: eye order correct, up is up, no mirroring, colour clean" ;;
esac
case "$MODES" in *swap*)
    # the control has to fail, and fail in the exact way we asked it to
    ok_if "$(saw swap 'EYE ORDER        : SWAPPED')" \
        "CONTROL: submitting eye 0's texture as Eye_Right swaps the two halves of
        the picture -- so the screenshot really is of what we submit"
    ok_if "$(saw swap 'ORIENTATION left : CORRECT')" \
        "CONTROL: ...and swapping the eyes changes nothing else" ;;
esac
case "$MODES" in *dflip*)
    ok_if "$(saw dflip 'VERTICALLY FLIPPED')" \
        "CONTROL: drawing the pattern upside down in our own texture produces an
        upside-down picture -- so the vertical axis is genuinely observed, and
        'up is up' in the null run is a measurement and not a coincidence" ;;
esac
case "$MODES" in *left1*)
    ok_if "$([ "${VERDICT[left1]:-9}" = 0 ] && echo 0 || echo 1)" \
        "with only the LEFT eye on screen, the left eye holds the left eye's
        content -- eye order confirmed without assuming which half of the
        side-by-side view is which" ;;
esac
case "$MODES" in *vbounds*)
    # NOT a pass/fail: a finding. Report which way it went either way.
    if [ "${VERDICT[vbounds]:-9}" = 0 ]; then
        echo "   NOTE Submit(pBounds = VRTextureBounds_t{0,1,1,0}) produced an IDENTICAL"
        echo "        picture to pBounds = NULL: xrizer IGNORES pBounds. A mod cannot use"
        echo "        texture bounds to flip, crop or pack eyes into one render target."
    else
        echo "   NOTE Submit(pBounds = VRTextureBounds_t{0,1,1,0}) changed the picture --"
        echo "        xrizer honours pBounds on this build. Re-read RESULTS.md, which"
        echo "        records the opposite."
    fi ;;
esac
if [ $all_ok = 1 ]; then echo "RESULT: PASS"; else echo "RESULT: FAIL"; exit 1; fi
