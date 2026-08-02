#!/bin/bash
# experiments/08_launch/gdb-launch.sh -- start BlackOps.exe UNDER winedbg --gdb
# and break on the two fatal-exit stubs found statically in the binary:
#
#   0x00454570  call 0x8F0AA0 / MessageBoxA / call 0x5F3290 / ExitProcess(0)
#   0x00523050  call 0x5F3290 / push 0x8000DEAD / ExitProcess
#
# 0x5F3290 is the Steam-DRM IPC routine (it names the semaphores
# "STEAM_DRM_IPC", "STEAM_DIPC_CONSUME", "SREAM_DIPC_PRODUCE").
#
# Launching under the debugger rather than attaching (Exp. 7's approach) is
# deliberate: the game only lives 4-13 s, which is not a reliable attach window.
# The gotchas from Exp. 7 still apply -- "set breakpoint pending on", and
# "handle SIGSEGV nostop noprint pass" issued AFTER the breakpoints.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="${WORK:-/mnt/games/tmp/bo1vr-exp08}"
GAME="${CASE_GAME:-$WORK/game}"
PROTON="${CASE_PROTON:-$HOME/.steam/steam/steamapps/common/Proton 10.0}"
PFX="${CASE_PFX:-$WORK/pfx-Proton_10.0}"

CMDS=$(cat <<'EOF'
set pagination off
set confirm off
set breakpoint pending on
echo \n===== breakpoints =====\n
break *0x00454591
break *0x0052305a
break *0x005deb0d
break *0x00651f26
break *0x0096ce31
break *0x005b3e27
break *0x0096c0c1
break *0x0097e8bb
break *0x0050a759
break *0x0050a82b
handle SIGSEGV nostop noprint pass
handle SIGABRT nostop noprint pass
handle SIGILL  nostop noprint pass
handle SIGFPE  nostop noprint pass
echo \n===== continue =====\n
continue
echo \n===== STOPPED: where =====\n
info registers eip esp
bt
x/16wx $esp
echo \n===== done =====\n
detach
quit
EOF
)

cd "$GAME" || exit 1
# Sys_CheckImproperQuit (0x004F1930) reads a 4-byte pid marker from
#   drive_c/users/steamuser/AppData/Local/Activision/CoD/__BlackOps
# and, if it is there, puts up a MODAL "Run In Safe Mode?" MessageBox before
# anything else happens. Every killed run leaves that marker behind, so from the
# second run onward the game blocks on a dialog nobody can click, and answering
# it Cancel makes WinMain return 0 -> a clean exit(0) that looks exactly like
# the failure being investigated. Remove it so each run starts clean.
rm -f "$PFX/pfx/drive_c/users/steamuser/AppData/Local/Activision/CoD/__BlackOps"
export STEAM_COMPAT_CLIENT_INSTALL_PATH="$HOME/.steam/steam"
export STEAM_COMPAT_DATA_PATH="$PFX"
export SteamGameId=42700 SteamAppId=42700
unset PROTON_LOG PROTON_USE_WOW64
export WINEDEBUG="${CASE_DEBUG:-+debugstr}"

printf '%s\n' "$CMDS" | timeout -k 10 "${CASE_SECS:-180}" \
  "$PROTON/proton" runinprefix winedbg --gdb "$GAME/BlackOps.exe" 2>&1
echo "== winedbg exited $?"
