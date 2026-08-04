#!/bin/bash
# One-time: import BlackOps.exe into a headless Ghidra project and analyse it.
#
#   ./setup.sh              import + full auto-analysis (slow: tens of minutes)
#   ./setup.sh -f           delete an existing project and start over
#
# Ghidra is ALREADY ON THIS MACHINE, installed for re4vr-port. It is not
# re-downloaded and not vendored here; only the path is recorded, so a Ghidra
# update over there is picked up automatically.
#
# THE PROJECT LIVES OUTSIDE THIS REPO. A Ghidra project for an 8 MB PE is
# hundreds of megabytes of database, and it is derived data -- regenerable from
# the exe by re-running this script. Nothing about it belongs in git.
#
# NOTE ON CEG. Steam's DRM encrypts individual functions in the on-disk image,
# so a handful of bodies will decompile as garbage. That is expected and it is
# not a reason to distrust the rest: everything this project has verified so far
# (R_SetViewParms at 0x6C7F80, the dvar registrations, the fence loop at
# 0x6EBB40) reads correctly straight off the file.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
GHIDRA="${GHIDRA:-$HOME/dev/re4vr-port/tools/ghidra_12.1.2_PUBLIC}"
PROJDIR="${PROJDIR:-$HOME/dev/reference/bo1-ghidra}"
PROJ="${PROJ:-bo1}"
EXE="${EXE:-/mnt/games/steam/steamapps/common/Call of Duty Black Ops/BlackOps.exe}"

[ -x "$GHIDRA/support/analyzeHeadless" ] || { echo "no analyzeHeadless at $GHIDRA" >&2; exit 1; }
[ -f "$EXE" ] || { echo "no BlackOps.exe at $EXE" >&2; exit 1; }

if [ "${1:-}" = "-f" ]; then rm -rf "$PROJDIR/$PROJ.rep" "$PROJDIR/$PROJ.gpr"; fi
if [ -e "$PROJDIR/$PROJ.gpr" ]; then
  echo "project already exists at $PROJDIR/$PROJ.gpr -- use -f to rebuild" >&2
  exit 0
fi
mkdir -p "$PROJDIR"

echo "=== importing $(basename "$EXE") ($(stat -c%s "$EXE") bytes), md5 $(md5sum "$EXE" | cut -d' ' -f1)"
echo "=== this takes tens of minutes; log -> $PROJDIR/import.log"
"$GHIDRA/support/analyzeHeadless" "$PROJDIR" "$PROJ" \
    -import "$EXE" \
    -scriptPath "$HERE" \
    -log "$PROJDIR/import.log" 2>&1 | tail -20
echo "=== done. Query it with:  ./decomp.sh 0x726650"
