#!/bin/bash
# experiments/08_launch/run.sh -- why does BlackOps.exe exit during startup?
#
# Exp. 7 established that the game comes up, reaches renderer init and starts
# loading the frontend zone, and then exits ~10 s in -- WITH OR WITHOUT our
# loader. This experiment takes that failure apart.
#
# Nothing in the Steam install is written to. $WORK/game is a mirror whose bulk
# directories (main/, zone/, Redist/, Soundtrack/) are symlinks to the read-only
# originals and whose writable files are private copies. Proton is used
# UNPATCHED and straight out of the Steam library -- launch diagnosis does not
# need the vrclient patch, and using stock builds is the point (it isolates
# whether our Proton changes contribute: they do not).
#
#   ./run.sh                run the whole matrix
#   ./run.sh stage          just (re)build the mirror
#   ./run.sh matrix         Proton / wow64 sweep         (cases A-D)
#   ./run.sh relay          relay-trace the exit path    (case E)
#   ./run.sh steamlike      Steam launch verb + Steam env (case F)
#   ./run.sh freshcfg       no players/config.cfg        (case G)
#   ./run.sh fileopen       trace CreateFile* to the end (case H)
#   ./run.sh gdb            launch under winedbg --gdb with exit breakpoints
#
# Everything lands in out/<case>/{case.txt,console.txt,gameconsole.txt}.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export WORK="${WORK:-/mnt/games/tmp/bo1vr-exp08}"
GAME_SRC="${GAME_SRC:-/mnt/games/steam/steamapps/common/Call of Duty Black Ops}"
STEAMAPPS="${STEAMAPPS:-$HOME/.steam/steam/steamapps/common}"

say() { printf '\n== %s\n' "$*"; }

stage() {
  say "stage a mirror of the game directory (the install is never written to)"
  local G="$WORK/game"
  mkdir -p "$WORK"; rm -rf "$G"; mkdir -p "$G"
  local e b
  for e in "$GAME_SRC"/*; do
    b="$(basename "$e")"
    case "$b" in
      main|zone|Redist|Soundtrack) ln -sfn "$e" "$G/$b" ;;
      *)                           cp -a  "$e" "$G/$b" ;;
    esac
  done
  cmp -s "$GAME_SRC/BlackOps.exe" "$G/BlackOps.exe" \
    && echo "   BlackOps.exe copied byte-identically" \
    || { echo "   mirror copy differs -- abort"; exit 1; }
  echo "   mirror: $G"
}

# Sys_CheckImproperQuit (0x004F1930) leaves a 4-byte pid marker behind; if it is
# present at startup the game puts up a MODAL "Run In Safe Mode?" MessageBox
# before it does anything else, and Cancel on that dialog makes WinMain return 0
# -- an exit(0) indistinguishable from the failure under investigation. Every
# killed run leaves the marker, so clear it before every launch.
clear_quit_marker() {
  find "$WORK" -path '*/AppData/Local/Activision/CoD/__BlackOps' -delete 2>/dev/null
}

matrix() {
  say "matrix: is it the Proton build, or the wow64 mode?"
  clear_quit_marker
  CASE_PROTON="$STEAMAPPS/Proton 10.0"          CASE_WOW64=1 CASE_DEBUG="+debugstr,+seh" \
    CASE_SECS=120 "$HERE/runcase.sh" A_p10_wow64
  clear_quit_marker
  CASE_PROTON="$STEAMAPPS/Proton 10.0"          CASE_WOW64=0 CASE_DEBUG="+debugstr" \
    CASE_SECS=90  "$HERE/runcase.sh" B_p10_classic
  clear_quit_marker
  CASE_PROTON="$STEAMAPPS/Proton - Experimental" CASE_WOW64=0 CASE_DEBUG="+debugstr" \
    CASE_SECS=90  "$HERE/runcase.sh" C_pexp_default
  clear_quit_marker
  CASE_PROTON="$STEAMAPPS/Proton 8.0"           CASE_WOW64=0 CASE_DEBUG="+debugstr" \
    CASE_SECS=90  "$HERE/runcase.sh" D_p8
}

relay() {
  say "relay: who calls ExitProcess / MessageBox?"
  # Wine only honours a relay filter from the registry, so it has to be written
  # into the prefix first. RelayInclude keeps the log to something readable.
  local P="$STEAMAPPS/Proton 10.0" PFX="$WORK/pfx-Proton_10.0"
  STEAM_COMPAT_CLIENT_INSTALL_PATH="$HOME/.steam/steam" STEAM_COMPAT_DATA_PATH="$PFX" \
  SteamGameId=42700 SteamAppId=42700 WINEDEBUG=-all \
    "$P/proton" run reg.exe add 'HKCU\Software\Wine\Debug' /v RelayInclude /t REG_SZ /d \
    'kernel32.ExitProcess;kernel32.TerminateProcess;ntdll.NtTerminateProcess;ntdll.RtlExitUserProcess;ntdll.RtlExitUserThread;user32.MessageBoxA;user32.MessageBoxW;kernel32.UnhandledExceptionFilter;kernel32.SetUnhandledExceptionFilter' /f >/dev/null 2>&1
  grep -q RelayInclude "$PFX/pfx/user.reg" && echo "   RelayInclude written"
  clear_quit_marker
  CASE_PROTON="$P" CASE_WOW64=0 CASE_DEBUG="+debugstr,+relay,+seh" CASE_SECS=180 \
    "$HERE/runcase.sh" E_relay_exit
}

steamlike() {
  say "F: Proton's own Steam launch verb (waitforexitandrun) + Steam's environment"
  local G="$WORK/game"
  clear_quit_marker
  CASE_PROTON="$STEAMAPPS/Proton - Experimental" CASE_WOW64=0 CASE_VERB=waitforexitandrun \
  CASE_DEBUG="+debugstr" CASE_SECS=150 \
  CASE_ENV="SteamClientLaunch=1 SteamEnv=1 SteamOverlayGameId=42700 STEAM_COMPAT_APP_ID=42700 STEAM_COMPAT_INSTALL_PATH=$G STEAM_COMPAT_LIBRARY_PATHS=/mnt/games/steam STEAM_COMPAT_MOUNTS=/mnt/games/steam STEAM_COMPAT_SHADER_PATH=/mnt/games/steam/steamapps/shadercache/42700" \
    "$HERE/runcase.sh" F_steamverb
}

freshcfg() {
  say "G: no players/config.cfg at all, plus the game's own file log"
  local G="$WORK/game"
  rm -f "$G/players/config.cfg"
  clear_quit_marker
  CASE_PROTON="$STEAMAPPS/Proton 10.0" CASE_WOW64=0 CASE_DEBUG="+debugstr" CASE_SECS=150 \
    CASE_ARGS='+set logfile 2 +set developer 1' "$HERE/runcase.sh" G_freshcfg_log
  # put the shipped config back so the mirror matches the install again
  cp -a "$GAME_SRC/players/config.cfg" "$G/players/config.cfg"
}

fileopen() {
  say "H: which file is the last one the game touches?"
  local P="$STEAMAPPS/Proton 10.0" PFX="$WORK/pfx-Proton_10.0"
  STEAM_COMPAT_CLIENT_INSTALL_PATH="$HOME/.steam/steam" STEAM_COMPAT_DATA_PATH="$PFX" \
  SteamGameId=42700 SteamAppId=42700 WINEDEBUG=-all \
    "$P/proton" run reg.exe add 'HKCU\Software\Wine\Debug' /v RelayInclude /t REG_SZ /d \
    'kernel32.CreateFileA;kernel32.CreateFileW;kernel32.ExitProcess;user32.MessageBoxA' /f >/dev/null 2>&1
  clear_quit_marker
  CASE_PROTON="$P" CASE_WOW64=0 CASE_DEBUG="+debugstr,+relay" CASE_SECS=180 \
    "$HERE/runcase.sh" H_fileopen
  echo "   last CreateFile calls before the exit:"
  grep -a 'Call KERNEL32.CreateFile' "$HERE/out/H_fileopen/console.txt" | tail -8 | sed 's/^/     /'
  grep -a 'ExitProcess' "$HERE/out/H_fileopen/console.txt" | grep -av load_list | sed 's/^/     /'
}

gdb_case() {
  say "gdb: launch under winedbg --gdb with breakpoints on every exit path"
  clear_quit_marker
  CASE_SECS=300 "$HERE/gdb-launch.sh" > "$HERE/out/gdb-launch.txt" 2>&1
  echo "   -> out/gdb-launch.txt"
}

summary() {
  say "summary"
  printf '   %-16s %-6s %-8s %s\n' CASE EXIT ELAPSED "LAST GAME CONSOLE LINE"
  local d
  for d in "$HERE"/out/*/; do
    [ -f "$d/rc.txt" ] || continue
    printf '   %-16s %-6s %-8s %s\n' "$(basename "$d")" \
      "$(cat "$d/rc.txt")" "$(cat "$d/elapsed.txt")s" \
      "$(tail -1 "$d/gameconsole.txt" 2>/dev/null)"
  done
}

case "${1:-all}" in
  stage)     stage ;;
  matrix)    matrix; summary ;;
  relay)     relay ;;
  steamlike) steamlike ;;
  freshcfg)  freshcfg ;;
  fileopen)  fileopen ;;
  gdb)       gdb_case ;;
  all)       stage; matrix; relay; steamlike; freshcfg; fileopen; gdb_case; summary ;;
  *) echo "usage: $0 [stage|matrix|relay|steamlike|freshcfg|fileopen|gdb|all]"; exit 2 ;;
esac
