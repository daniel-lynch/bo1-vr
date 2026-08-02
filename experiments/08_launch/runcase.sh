#!/bin/bash
# experiments/08_launch/runcase.sh -- launch BlackOps.exe once, under one
# named configuration, and capture everything it says before it dies.
#
# One case = one Proton build x one wow64 mode x one set of WINEDEBUG channels
# x one set of extra environment. Every case gets its own prefix, keyed on the
# Proton build, so that a 8.0 prefix is never handed to 11.0.
#
#   ./runcase.sh <tag>
#
# Configured entirely through the environment (see run.sh for the matrix):
#   CASE_PROTON     path to a Proton build          (default: Proton 10.0)
#   CASE_WOW64      1 = PROTON_USE_WOW64=1, 0 = classic wow64   (default 0)
#   CASE_DEBUG      WINEDEBUG value                 (default +debugstr)
#   CASE_ENV        extra "K=V K=V" applied verbatim
#   CASE_SECS       hard timeout in seconds         (default 90)
#   CASE_ARGS       extra BlackOps.exe command line
#   CASE_PFXTAG     override the prefix key
#   CASE_VERB       proton verb: "run" (default) or "waitforexitandrun"
#                   (waitforexitandrun is the verb Steam itself uses)
#   CASE_GAME       game mirror to launch           (default $WORK/game)
#
# The Steam install is never written to: $WORK/game is a mirror whose bulk
# directories are symlinks and whose writable files are private copies.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="${WORK:-/mnt/games/tmp/bo1vr-exp08}"
TAG="${1:?usage: runcase.sh <tag>}"

PROTON="${CASE_PROTON:-$HOME/.steam/steam/steamapps/common/Proton 10.0}"
WOW64="${CASE_WOW64:-0}"
SECS="${CASE_SECS:-90}"
GAME="${CASE_GAME:-$WORK/game}"
PFXTAG="${CASE_PFXTAG:-$(basename "$PROTON" | tr ' ()' '___')}"
PFX="$WORK/pfx-$PFXTAG"

OUT="$HERE/out/$TAG"
rm -rf "$OUT"; mkdir -p "$OUT"

{
  echo "tag      : $TAG"
  echo "proton   : $PROTON  ($(cat "$PROTON/version" 2>/dev/null))"
  echo "wow64    : $WOW64"
  echo "prefix   : $PFX"
  echo "game     : $GAME"
  echo "debug    : ${CASE_DEBUG:-+debugstr}"
  echo "extraenv : ${CASE_ENV:-<none>}"
  echo "verb     : ${CASE_VERB:-run}"
  echo "args     : ${CASE_ARGS:-<none>}"
  echo "timeout  : ${SECS}s"
} | tee "$OUT/case.txt"

mkdir -p "$PFX"
export STEAM_COMPAT_CLIENT_INSTALL_PATH="$HOME/.steam/steam"
export STEAM_COMPAT_DATA_PATH="$PFX"
export SteamGameId=42700
export SteamAppId=42700
export WINEDEBUG="${CASE_DEBUG:-+debugstr}"
unset PROTON_LOG
if [ "$WOW64" = "1" ]; then export PROTON_USE_WOW64=1; else unset PROTON_USE_WOW64; fi
# shellcheck disable=SC2163
for kv in ${CASE_ENV:-}; do export "$kv"; done

cd "$GAME" || exit 1
start=$(date +%s.%N)
# shellcheck disable=SC2086
timeout -k 10 "$SECS" "$PROTON/proton" "${CASE_VERB:-run}" "$GAME/BlackOps.exe" ${CASE_ARGS:-} \
    > "$OUT/console.txt" 2>&1
rc=$?
end=$(date +%s.%N)
elapsed=$(echo "$end - $start" | bc)

printf 'exit %s after %.1fs\n' "$rc" "$elapsed" | tee -a "$OUT/case.txt"
echo "$rc" > "$OUT/rc.txt"
printf '%.1f\n' "$elapsed" > "$OUT/elapsed.txt"

# The game's own Com_Printf output arrives via OutputDebugStringA, so strip the
# +debugstr wrapper to get a readable game console.
sed -n 's/.*OutputDebugStringA "\(.*\)"$/\1/p' "$OUT/console.txt" \
  | sed 's/\\n$//' > "$OUT/gameconsole.txt"

echo "--- last 25 lines of the game console"
tail -25 "$OUT/gameconsole.txt"
echo "--- last 25 lines of the raw console"
tail -25 "$OUT/console.txt"
