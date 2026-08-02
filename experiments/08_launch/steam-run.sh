#!/bin/bash
# Launch BO1 through the Steam client, watch it, screenshot it, stop it.
#
# WHY THROUGH STEAM. RESULTS.md §10: the CEG ownership handshake only completes
# when the running Steam client starts the game (reaper + SteamLinuxRuntime_4 +
# Proton). `proton run` against a mirror always hits the DRM ExitProcess stub of
# §3. So this is the only way to get a running game to hook.
#
#   ./steam-run.sh              launch, watch 60 s, screenshot, quit
#   WATCH=300 ./steam-run.sh    leave it up longer
#   NOKILL=1 ./steam-run.sh     leave it running afterwards
#
# Lessons from run 1 of steam-launch.sh, all load-bearing:
#   * Steam's scaffolding (reaper, pressure-vessel) carries the exe path in
#     argv, so `pgrep -f BlackOps.exe` matches ~6 s BEFORE the game exists.
#     Never treat the pid list as a stop condition; watch the whole window.
#   * Steam's console-linux.txt Adding/Removing lines for gameID 42700 are the
#     authoritative lifetime. Everything else is inference.
#   * xwd fails on this root window with BadColor (X_QueryColors). ffmpeg
#     x11grab works. NEVER use ImageMagick `import` -- it calls XGrabServer and
#     has deadlocked this desktop twice.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${OUT:-$HERE/out/steamrun}"
GAME="/mnt/games/steam/steamapps/common/Call of Duty Black Ops"
PFX="/mnt/games/steam/steamapps/compatdata/42700/pfx"
STEAMLOG="$HOME/.local/share/Steam/logs/console-linux.txt"
WATCH=${WATCH:-60}
NOKILL=${NOKILL:-0}
DISP=${DISPLAY:-:1}
mkdir -p "$OUT"
: > "$OUT/log.txt"
say() { echo "$*" | tee -a "$OUT/log.txt"; }

# Sys_CheckImproperQuit (0x004F1930) leaves a 4-byte pid marker; a killed run
# leaves it behind and the NEXT launch then blocks on a modal "Run In Safe
# Mode?" box whose Cancel makes WinMain return 0 -- an exit(0) that looks
# exactly like the DRM bug. Clear it before every launch.
MARK="$PFX/drive_c/users/steamuser/AppData/Local/Activision/CoD/__BlackOps"
[ -e "$MARK" ] && { say "clearing improper-quit marker"; rm -f "$MARK"; }

LOGLINE=$(wc -l < "$STEAMLOG" 2>/dev/null || echo 1)
md5sum "$GAME/players/config.cfg" > "$OUT/cfg_before.txt" 2>/dev/null

say "=== launching app 42700 via Steam at $(date -Is) ==="
setsid steam -applaunch 42700 >"$OUT/steam_stdout.txt" 2>&1 &

ALIVE=0
for i in $(seq 1 "$WATCH"); do
  read -r -t 1 _ </dev/zero          # 1 s tick without calling sleep
  if pgrep -f "BlackOps.exe" >/dev/null 2>&1; then ALIVE=$((ALIVE + 1)); fi
  case "$i" in
    20|35|50)
      ffmpeg -loglevel error -f x11grab -i "$DISP" -frames:v 1 -y \
             "$OUT/screen_t${i}.png" </dev/null >/dev/null 2>&1 \
        && say "t=${i}s  screenshot -> screen_t${i}.png"
      ;;
  esac
done

say "=== watched ${WATCH}s; a BlackOps.exe-matching process was up for ${ALIVE}s ==="
if [ "$NOKILL" = 1 ]; then
  say "NOKILL=1 -- leaving the game running"
else
  say "=== stopping at $(date -Is) ==="
  pkill -f "BlackOps.exe" 2>/dev/null
  read -r -t 5 _ </dev/zero
  pkill -9 -f "BlackOps.exe" 2>/dev/null
fi

# Steam's own record -- authoritative, immune to the argv-matching confusion.
tail -n "+$LOGLINE" "$STEAMLOG" 2>/dev/null \
  | grep -aE "gameID 42700|AppID 42700|Black Ops" > "$OUT/steam_applog.txt"
say "=== Steam's record of app 42700 this run ==="
grep -aE "Adding process|Removing process|chdir|stopped" "$OUT/steam_applog.txt" \
  | head -20 | tee -a "$OUT/log.txt"

md5sum "$GAME/players/config.cfg" > "$OUT/cfg_after.txt" 2>/dev/null
say "=== config.cfg (rewrite = the game reached a clean shutdown) ==="
say "  before: $(cut -d' ' -f1 < "$OUT/cfg_before.txt")"
say "  after:  $(cut -d' ' -f1 < "$OUT/cfg_after.txt")"
