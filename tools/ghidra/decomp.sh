#!/bin/bash
# Decompile BO1 functions by address, against the project setup.sh built.
#
#   ./decomp.sh 0x726650                 R_SetRenderTarget
#   ./decomp.sh 0x6C7F80 0x6C8CD0        several at once
#
# Addresses are the game's own VAs (image base 0x400000) -- the same numbers
# used throughout this repo's notes and sources, no adjustment.
#
# -noanalysis is what makes this fast: the project was analysed once by
# setup.sh, and re-opening it read-only takes seconds rather than re-running
# the whole auto-analysis for every question.
#
# Ghidra's headless output is extremely chatty, so DecompileAt.java prefixes
# every line it emits with BO1DECOMP: and this filters on that. Anything else
# on stdout is Ghidra's own logging and is dropped; pass -v to see all of it
# when something has gone wrong.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
GHIDRA="${GHIDRA:?set GHIDRA to your Ghidra install dir (e.g. ~/tools/ghidra_12.1.2_PUBLIC)}"
PROJDIR="${PROJDIR:-$HOME/dev/reference/bo1-ghidra}"
PROJ="${PROJ:-bo1}"

VERBOSE=0
if [ "${1:-}" = "-v" ]; then VERBOSE=1; shift; fi
[ $# -gt 0 ] || { echo "usage: $(basename "$0") [-v] <addr> [addr...]" >&2; exit 2; }
[ -e "$PROJDIR/$PROJ.gpr" ] || { echo "no project at $PROJDIR/$PROJ.gpr -- run ./setup.sh first" >&2; exit 1; }

OUT=$("$GHIDRA/support/analyzeHeadless" "$PROJDIR" "$PROJ" \
        -process 'BlackOps.exe' -noanalysis \
        -scriptPath "$HERE" \
        -postScript DecompileAt.java "$@" 2>&1)
RC=$?

if [ "$VERBOSE" = 1 ]; then printf '%s\n' "$OUT"; exit $RC; fi

# Strip Ghidra's log prefix, our own marker, and the trailing "(GhidraScript)"
# that headless appends to every println -- without the last one the output is
# not valid C and cannot be pasted anywhere useful.
printf '%s\n' "$OUT" \
  | sed -n 's/.*BO1DECOMP: \{0,1\}//p' \
  | sed 's/[[:space:]]*(GhidraScript)[[:space:]]*$//'
if ! printf '%s\n' "$OUT" | grep -q 'BO1DECOMP'; then
  echo "(no script output -- rerun with -v to see Ghidra's log)" >&2
  exit 1
fi
