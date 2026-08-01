#!/bin/bash
# experiments/07_ingame/run.sh -- get the ASI loader into the LIVE BlackOps.exe.
#
#   winmm.dll (shim, imported by the game)
#     -> dist/dinput8.dll                (the repository's ASI loader, unmodified)
#     -> bo1probe.asi                    (this experiment)
#
# THE STEAM INSTALL IS NEVER WRITTEN TO. The game is launched from a mirror
# directory under $WORK built out of symlinks to the read-only bulk (main/,
# zone/, Redist/, Soundtrack/) and private copies of everything the game or
# Steam CEG might write to (BlackOps.exe, players/, *.STEAMSTART, the small
# DLLs). Our three DLLs go in the mirror, never next to the real executable.
# Proton is likewise hard-link copied and only the copy is patched (Exp. 4).
#
#   ./run.sh                    normal run; quits itself after BO1VR_QUIT_AFTER_S
#   BO1VR_GDB=1 ./run.sh        additionally attach winedbg --gdb and break in
#                               our own code inside the live process
#   BO1VR_QUIT_AFTER_S=0 ./run.sh   leave the game running until the timeout
#   WORK=/some/dir ./run.sh
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="${WORK:-${TMPDIR:-/tmp}/bo1vr-exp07}"
GAME_SRC="${GAME_SRC:-/mnt/games/steam/steamapps/common/Call of Duty Black Ops}"
PROTON_SRC="${PROTON_SRC:-$HOME/.steam/steam/steamapps/common/Proton 10.0}"
RUN_SECS="${BO1VR_RUN_SECS:-240}"
QUIT_AFTER_S="${BO1VR_QUIT_AFTER_S:-90}"
WAIT_MS="${BO1VR_WAIT_MS:-180000}"

say() { printf '\n== %s\n' "$*"; }

say "0. preflight"
[ -f "$GAME_SRC/BlackOps.exe" ] || { echo "no BlackOps.exe at $GAME_SRC"; exit 1; }
[ -d "$PROTON_SRC" ]           || { echo "no Proton at $PROTON_SRC"; exit 1; }
pgrep -x steam >/dev/null || pgrep -f 'ubuntu12_32/steam' >/dev/null || {
  echo "WARNING: the Steam client does not look like it is running."
  echo "         BlackOps.exe is a CEG title and statically imports steam_api.dll;"
  echo "         without a Steam session it will exit during init."
}
printf '   game   %s\n   md5    %s\n' "$GAME_SRC/BlackOps.exe" \
  "$(md5sum "$GAME_SRC/BlackOps.exe" | cut -d' ' -f1)"
printf '   DISPLAY=%s\n' "${DISPLAY:-<unset>}"

say "1. build"
make -C "$HERE" >/dev/null || exit 1
ls -la "$HERE/out"/ | sed 's/^/   /'

say "2. hard-link a Proton copy and patch its wow64 vrclient (Exp. 4)"
"$HERE/../../tools/patch-proton-wow64-vrclient.py" "$PROTON_SRC" "$WORK/proton" \
  | sed 's/^/   /'

say "3. stage a mirror of the game directory (the install is never written to)"
GAME="$WORK/game"
rm -rf "$GAME"; mkdir -p "$GAME"
for e in "$GAME_SRC"/*; do
  b="$(basename "$e")"
  case "$b" in
    main|zone|Redist|Soundtrack)  ln -sfn "$e" "$GAME/$b" ;;   # bulk, read-only
    *)                            cp -a "$e" "$GAME/$b" ;;     # may be written
  esac
done
cmp -s "$GAME_SRC/BlackOps.exe" "$GAME/BlackOps.exe" \
  && echo "   BlackOps.exe copied byte-identically" \
  || { echo "   mirror copy of BlackOps.exe differs -- abort"; exit 1; }
cp -f "$HERE/out/winmm.dll" "$HERE/out/dinput8.dll" "$HERE/out/bo1probe.asi" "$GAME/"
du -sh --exclude=main --exclude=zone --exclude=Redist --exclude=Soundtrack "$GAME" \
  | sed 's/^/   mirror (excluding symlinked bulk): /'

say "4. force a small window and a magic com_maxfps into the MIRROR's config"
# 47 fps is the tell-tale: it appears nowhere else on this machine, so reading
# it back through Dvar_FindVar in the live process cannot be a coincidence.
CFG="$GAME/players/config.cfg"
chmod u+w "$CFG"
sed -i -e 's/^seta r_fullscreen .*/seta r_fullscreen "0"/' \
       -e 's/^seta r_mode .*/seta r_mode "1024x768"/' \
       -e 's/^seta com_maxfps .*/seta com_maxfps "47"/' "$CFG"
grep -E '^seta (r_fullscreen|r_mode|com_maxfps|r_monitor) ' "$CFG" | sed 's/^/   /'

say "5. launch under Proton (new-WoW64)"
mkdir -p "$WORK/pfx"
export STEAM_COMPAT_CLIENT_INSTALL_PATH="$HOME/.steam/steam"
export STEAM_COMPAT_DATA_PATH="$WORK/pfx"
export PROTON_USE_WOW64=1                 # README Decision 9
export SteamGameId=42700                  # Exp. 4: silent failure without it
export SteamAppId=42700                   # steam_api.dll needs to know the app
# Exp. 0 Finding 2: a Wine builtin silently beats a native DLL in the app
# directory unless the name is prefer-native. dinput8 is on Proton's list;
# winmm is not, so it needs an override -- "n,b" and never bare "n"
# (Exp. 0 Finding 1), or our own LoadLibrary of the real winmm fails with 126.
export WINEDLLOVERRIDES="winmm=n,b${WINEDLLOVERRIDES:+;$WINEDLLOVERRIDES}"
export BO1VR_LOG="Z:${GAME//\//\\}\\bo1probe.log"
export BO1VR_WAIT_MS="$WAIT_MS"
export BO1VR_QUIT_AFTER_S="$QUIT_AFTER_S"
unset PROTON_LOG                          # Exp. 4: PROTON_LOG=1 writes GB/min
# MEASURED, and the reason this is "+debugstr" rather than nothing: under
# `proton run` the Windows process's stdio is swallowed outright -- a plain host
# exe's own printf never reaches the launching shell, so neither does
# src/log.c's fprintf(stderr). ~/steam-42700.log does not exist either; that
# redirect only happens when Steam itself launches the game. src/log.c also
# mirrors every line to OutputDebugStringA, and +debugstr is what makes that
# visible. It is the only channel that carries the loader's own banner.
export WINEDEBUG="${WINEDEBUG:-+debugstr}"

rm -f "$GAME/bo1probe.log" "$HERE/out/console.txt"
echo "   BO1VR_LOG        = $BO1VR_LOG"
echo "   WINEDLLOVERRIDES = $WINEDLLOVERRIDES"
echo "   timeout          = ${RUN_SECS}s, plugin self-quit after ${QUIT_AFTER_S}s"

cd "$GAME"
timeout -k 10 "$RUN_SECS" "$WORK/proton/proton" run "$GAME/BlackOps.exe" \
    > "$HERE/out/console.txt" 2>&1 &
PROTON_PID=$!

# Watch for the loader banner while it runs, and grab a screenshot + a debugger
# session once the probe says it is alive.
BANNER_AT=""
for i in $(seq 1 "$RUN_SECS"); do
  if [ -z "$BANNER_AT" ] && grep -q "bo1-vr ASI loader" "$HERE/out/console.txt" 2>/dev/null; then
    BANNER_AT="$i"
    echo "   [t+${i}s] ASI loader banner seen on the game's stderr"
  fi
  # Attach only once the renderer has a D3D9 device. The probe's early pass can
  # finish 2 s in, while the game is still spawning threads by the dozen, and a
  # winedbg attach into that window was measured to leave gdb never connecting
  # at all ("No shared libraries loaded at this time").
  if grep -q "EXPERIMENT 7 END" "$GAME/bo1probe.log" 2>/dev/null && [ -z "${SHOT_DONE:-}" ]; then
    SHOT_DONE=1
    if [ -n "${DISPLAY:-}" ] && command -v import >/dev/null; then
      import -window root "$HERE/out/screen.png" 2>/dev/null && \
        echo "   [t+${i}s] screenshot -> out/screen.png"
    fi
    if [ "${BO1VR_GDB:-0}" = "1" ]; then
      "$HERE/gdb-attach.sh" "$WORK" "$GAME" > "$HERE/out/gdb.txt" 2>&1
      echo "   [t+${i}s] winedbg --gdb session -> out/gdb.txt"
    fi
  fi
  kill -0 "$PROTON_PID" 2>/dev/null || break
  sleep 1
done
wait "$PROTON_PID"; rc=$?

say "6. result"
if [ -f "$GAME/bo1probe.log" ]; then
  cp -f "$GAME/bo1probe.log" "$HERE/out/bo1probe.log"
  cat "$HERE/out/bo1probe.log"
else
  echo "   no bo1probe.log -- the plugin never ran. Tail of the console:"
  tail -40 "$HERE/out/console.txt"
fi

say "7. verdict"
ok=0
grep -q "bo1-vr ASI loader" "$HERE/out/console.txt" && { echo "   PASS: dinput8.dll loader banner reached the game's stderr"; ok=$((ok+1)); }
grep -qE "EXPERIMENT 7 (FINAL|END): PASS" "$HERE/out/bo1probe.log" 2>/dev/null && { echo "   PASS: bo1probe.asi ran inside BlackOps.exe and every check passed"; ok=$((ok+1)); }
grep -q 'com_maxfps .*value=47' "$HERE/out/bo1probe.log" 2>/dev/null && { echo "   PASS: com_maxfps read back as 47 through Dvar_FindVar"; ok=$((ok+1)); }
echo "   proton run exited $rc; $ok/3 pass conditions met"
[ "$ok" -ge 2 ] || exit 1
