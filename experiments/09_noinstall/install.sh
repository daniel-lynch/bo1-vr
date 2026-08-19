#!/bin/bash
# Install (or remove) the loader WITHOUT touching the game install.
#
#   ./install.sh            install into the BO1 Proton prefix
#   ./install.sh remove     put the prefix back exactly as it was
#   PFXDIR=... ./install.sh use a different prefix (the exp-9 test prefix)
#
# WHAT THIS CHANGES, AND WHAT IT DELIBERATELY DOES NOT
# ---------------------------------------------------
# Everything below lives in steamapps/compatdata/42700 -- the Wine prefix, which
# Proton owns and rebuilds from scratch if deleted. NOTHING is written into
# "steamapps/common/Call of Duty Black Ops". That is the point: RESULTS.md §10
# of Exp. 8 established the game only runs when the Steam client launches it, so
# the exe must stay exactly as Steam installed it.
#
#   1. drive_c/windows/syswow64/winmm.dll
#      Proton ships this as a SYMLINK to its builtin. We rename that symlink to
#      winmm_real.dll and drop our shim in its place. The shim probes
#      winmm_real.dll first (winmm_shim.c resolve_real_winmm), so the real one is
#      still what the game's eleven imports reach.
#
#   2. drive_c/windows/syswow64/bo1vr_loader.dll
#      The ASI loader, under a name no real system DLL uses. We do NOT shadow
#      dinput8.dll here -- a system directory is shared with everything else in
#      the prefix and booby-trapping a real DLL name is how you break unrelated
#      software months later.
#
#   3. user.reg: [Software\Wine\AppDefaults\BlackOps.exe\DllOverrides]
#      "winmm"="native,builtin", scoped to BlackOps.exe alone.
#      This is why no WINEDLLOVERRIDES is needed, which is why no Steam launch
#      option is needed, which is why this works when Steam starts the game
#      and we never get to set an environment variable.
#
# WHY NOT the alternatives, each measured in RESULTS.md:
#   * WINEPATH pointing at a mod directory  -- does not work; Wine did not
#     resolve the native winmm from the Windows PATH (§2, control run).
#   * AppInit_DLLs                          -- not implemented in this Wine.
#   * Proton user_settings.py               -- lives in the Proton directory and
#     would apply to EVERY game using Proton Experimental.
#   * Steam launch options                  -- works, but needs a Steam config
#     write plus a client restart, and the client is often mid-session.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"

# Find the BO1 Proton prefix (app 42700). Checked in order: $PFXDIR from the
# environment, the default Steam library, then every library listed in
# libraryfolders.vdf (which is how a game on a second disk is found).
if [ -z "${PFXDIR:-}" ]; then
  for lib in "$HOME/.local/share/Steam" \
             $(grep -oP '"path"\s+"\K[^"]+' \
               "$HOME/.local/share/Steam/steamapps/libraryfolders.vdf" 2>/dev/null); do
    if [ -d "$lib/steamapps/compatdata/42700/pfx" ]; then
      PFXDIR="$lib/steamapps/compatdata/42700/pfx"; break
    fi
  done
fi
[ -n "${PFXDIR:-}" ] || {
  echo "could not find the BO1 Proton prefix (steamapps/compatdata/42700/pfx)." >&2
  echo "run the game once under Proton, or set PFXDIR=/path/to/that/pfx" >&2
  exit 1
}
# steamapps/compatdata/42700/pfx -> steamapps/common/... in the same library.
GAMEDIR="${PFXDIR%/compatdata/*}/common/Call of Duty Black Ops"
SW="$PFXDIR/drive_c/windows/syswow64"
REG="$PFXDIR/user.reg"
SRC="$HERE/../07_ingame/out"
MODE="${1:-install}"

[ -d "$SW" ] || { echo "no such prefix: $SW" >&2; exit 1; }

# A wineserver holding this prefix caches the registry and rewrites user.reg
# when it exits -- it would silently discard our override. Match on the
# process's own WINEPREFIX rather than a command-line pattern: a pgrep pattern
# containing the prefix path also matches THIS script's own shell, and killing
# that is how the first attempt at this died.
stop_wineserver() {
  local p
  for p in $(pgrep -x wineserver 2>/dev/null); do
    if tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null | grep -qx "WINEPREFIX=$PFXDIR"; then
      echo "  stopping wineserver $p holding this prefix"
      kill "$p" 2>/dev/null
    fi
  done
  read -r -t 3 _ </dev/zero || true
}

reg_set() {   # add the override if absent
  python3 - "$REG" <<'PY'
import io, sys
p = sys.argv[1]
s = io.open(p, encoding="utf-8").read()
key = "[Software\\\\Wine\\\\AppDefaults\\\\BlackOps.exe\\\\DllOverrides]"
if key in s:
    print("  registry override already present")
else:
    io.open(p, "w", encoding="utf-8").write(
        s.rstrip("\n") + "\n\n" + key + " 1785460000\n"
        "#time=1dc0000000000000\n\"winmm\"=\"native,builtin\"\n")
    print("  registry override added")
PY
}

reg_unset() {
  python3 - "$REG" <<'PY'
import io, re, sys
p = sys.argv[1]
s = io.open(p, encoding="utf-8").read()
key = "[Software\\\\Wine\\\\AppDefaults\\\\BlackOps.exe\\\\DllOverrides]"
i = s.find(key)
if i < 0:
    print("  registry override not present")
else:
    j = s.find("\n[", i)
    s = s[:i] + (s[j+1:] if j > 0 else "")
    io.open(p, "w", encoding="utf-8").write(s)
    print("  registry override removed")
PY
}

case "$MODE" in
install)
  [ -f "$SRC/winmm.dll" ] || { echo "build first: make -C $HERE/../07_ingame" >&2; exit 1; }
  stop_wineserver
  if [ -e "$SW/winmm_real.dll" ]; then
    echo "  winmm_real.dll already exists -- prefix already prepared"
  elif [ -L "$SW/winmm.dll" ]; then
    mv "$SW/winmm.dll" "$SW/winmm_real.dll"
    echo "  moved Proton's builtin symlink aside -> winmm_real.dll"
  else
    echo "  UNEXPECTED: $SW/winmm.dll is not a symlink; refusing" >&2; exit 1
  fi
  rm -f "$SW/winmm.dll"
  cp "$SRC/winmm.dll"   "$SW/winmm.dll"
  cp "$SRC/dinput8.dll" "$SW/bo1vr_loader.dll"
  echo "  installed shim + bo1vr_loader.dll into the prefix syswow64"
  reg_set
  # GetTempPathA in this prefix yields AppData\Local\Temp, NOT users\steamuser\
  # Temp. Reading the wrong one is how the first successful run got written off
  # as a failure.
  rm -f "$PFXDIR/drive_c/users/steamuser/AppData/Local/Temp/bo1vr_shim.log"
  # A killed run leaves this marker and the NEXT launch then blocks on a modal
  # "Run In Safe Mode?" box whose Cancel exits with code 0 -- indistinguishable
  # from a real failure (Exp. 8 §6a).
  rm -f "$PFXDIR/drive_c/users/steamuser/AppData/Local/Activision/CoD/__BlackOps"
  echo "installed. nothing in the game install was touched:"
  find "$GAMEDIR" -maxdepth 1 \
       -newermt "-10 minutes" 2>/dev/null | sed 's/^/  recently modified: /'
  ;;
remove)
  stop_wineserver
  rm -f "$SW/winmm.dll" "$SW/bo1vr_loader.dll"
  [ -e "$SW/winmm_real.dll" ] && mv "$SW/winmm_real.dll" "$SW/winmm.dll" \
    && echo "  restored Proton's builtin winmm symlink"
  reg_unset
  echo "removed."
  ;;
*)
  echo "usage: $0 [install|remove]" >&2; exit 2 ;;
esac
