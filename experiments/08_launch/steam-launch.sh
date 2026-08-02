#!/bin/bash
# Exp 8 follow-up: the one hypothesis exp 8 could not test -- launch through the
# Steam client itself, so the CEG/Steam-DRM ownership handshake sees the real
# reaper + Steam-Linux-Runtime container wrapper, and the exe runs out of the
# install where it may write config.cfg / .STEAMSTART.
#
# Bounded: hard-kills at WATCH seconds no matter what, so a fullscreen grab
# cannot outlive the test.
#
# RUN THIS YOURSELF -- it is deliberately not agent-runnable. It starts a
# fullscreen game on the desktop and calls pkill, which the agent permission
# classifier blocks (correctly). Back up players/ first; unlike every other
# case in this experiment, this one lets the game write into the install.
#
#     ./steam-launch.sh            # 75 s bounded run, output under out/steamlaunch/
#     WATCH=180 ./steam-launch.sh  # longer, if it reaches the menu
#
# READING THE RESULT. Steam does not start the game until ~8 s after the verb;
# out/steamlaunch/steam_applog.txt is authoritative, NOT the pid ticks. If its
# "Adding process ... 42700" and "Removing process ... 42700" share a timestamp
# and no window ever appears, that reproduces the exp 8 failure and means
# Steam's reaper/container wrapper is NOT the missing piece -- go to the
# STEAM_DRM_IPC semaphore hook (RESULTS.md §8). If the game survives to the
# kill with a Black Ops window in out/steamlaunch/windows_t*.txt, the CEG
# ownership handshake needs the real client and the whole mod must launch
# through Steam from here on.
set -u
OUT="$(dirname "$0")/out/steamlaunch"
GAME="/mnt/games/steam/steamapps/common/Call of Duty Black Ops"
PFX="/mnt/games/steam/steamapps/compatdata/42700/pfx"
WATCH=${WATCH:-75}
mkdir -p "$OUT"

# The improper-quit marker: Sys_CheckImproperQuit (0x004F1930) leaves a 4-byte
# pid file, and from run 2 onward the game blocks on a modal "Run In Safe Mode?"
# box whose Cancel makes WinMain return 0 -- an exit(0) indistinguishable from
# the bug we are hunting. Clear it first (exp 8 §6).
MARK="$PFX/drive_c/users/steamuser/AppData/Local/Activision/CoD/__BlackOps"
if [ -e "$MARK" ]; then echo "removing improper-quit marker" | tee -a "$OUT/log.txt"; rm -f "$MARK"; fi

# Record the install's state so we can prove afterwards what the game wrote.
find "$GAME" -maxdepth 2 -type f -newermt "2026-01-01" > "$OUT/install_before.txt" 2>/dev/null
md5sum "$GAME/players/config.cfg" > "$OUT/cfg_before.txt" 2>/dev/null

echo "=== launching via Steam at $(date -Is) ===" | tee -a "$OUT/log.txt"
setsid steam -applaunch 42700 >"$OUT/steam_stdout.txt" 2>&1 &
STEAMPID=$!

# NEVER BREAK EARLY, AND NEVER KILL BEFORE THE WINDOW ENDS. Run 1 of this
# script did both and invalidated its own result. Steam's launch scaffolding
# (reaper, pressure-vessel) carries the exe path in argv, so it MATCHES
# `pgrep -f BlackOps.exe` seconds before the game itself exists. Those
# scaffolding pids appeared at t=2..4, vanished at t=5, and the old loop read
# that as "the game died" -- broke out, and ran pkill -f, sleep 3, pkill -9.
# Steam chdir'd and started the real game at t=8s. The second pkill landed on
# it. Steam's own console-linux.txt is what exposed this:
#     20:23:41  (pkill)
#     20:23:44  chdir ".../Call of Duty Black Ops"; Adding process ... 42700
#     20:23:44  Removing process ... 42700
# So: watch the WHOLE window, treat the pid list as an observation and never as
# a stop condition, and kill exactly once at the end.
STEAMLOG="$HOME/.local/share/Steam/logs/console-linux.txt"
wc -l < "$STEAMLOG" > "$OUT/steamlog_start_line.txt" 2>/dev/null || echo 0 > "$OUT/steamlog_start_line.txt"

ALIVE=0
for i in $(seq 1 "$WATCH"); do
  read -r -t 1 _ </dev/zero   # 1s tick without calling sleep
  PIDS=$(pgrep -f "BlackOps.exe" 2>/dev/null | tr '\n' ' ')
  if [ -n "$PIDS" ]; then
    ALIVE=$((ALIVE + 1))
    echo "t=${i}s  pids: $PIDS" >> "$OUT/log.txt"
  else
    echo "t=${i}s  -" >> "$OUT/log.txt"
  fi
  # Snapshot the screen and the window list regardless of what pgrep says --
  # the screenshot is the ground truth for "did it reach the menu", and it costs
  # nothing to take one when the game is not there.
  case "$i" in
    15|30|45|60|70)
      xwd -root -silent > "$OUT/screen_t${i}.xwd" 2>/dev/null && echo "t=${i}s  screenshot" >> "$OUT/log.txt"
      wmctrl -l >> "$OUT/windows_t${i}.txt" 2>&1
      ;;
  esac
done

echo "=== killing at $(date -Is) (watched the full ${WATCH}s; game present for ${ALIVE}s of it) ===" | tee -a "$OUT/log.txt"
pkill -f "BlackOps.exe" 2>/dev/null
read -r -t 3 _ </dev/zero
pkill -9 -f "BlackOps.exe" 2>/dev/null

# Steam's own view of the app's lifetime -- authoritative, and immune to the
# argv-matching confusion above.
START_LINE=$(cat "$OUT/steamlog_start_line.txt")
tail -n "+$START_LINE" "$STEAMLOG" 2>/dev/null \
  | grep -aE "42700|Call of Duty Black Ops" > "$OUT/steam_applog.txt"
echo "=== Steam's record of the app (Adding/Removing process = its lifetime) ===" | tee -a "$OUT/log.txt"
cat "$OUT/steam_applog.txt" | tee -a "$OUT/log.txt"

find "$GAME" -maxdepth 2 -type f -newermt "2026-01-01" > "$OUT/install_after.txt" 2>/dev/null
md5sum "$GAME/players/config.cfg" > "$OUT/cfg_after.txt" 2>/dev/null
echo "=== install files touched since 2026-01-01 (after) ===" | tee -a "$OUT/log.txt"
diff "$OUT/install_before.txt" "$OUT/install_after.txt" | tee -a "$OUT/log.txt"
echo "=== config.cfg md5 ===" | tee -a "$OUT/log.txt"
cat "$OUT/cfg_before.txt" "$OUT/cfg_after.txt" | tee -a "$OUT/log.txt"
echo "=== saw process: $SAWPROC, last alive tick: ${BEST}s ===" | tee -a "$OUT/log.txt"
