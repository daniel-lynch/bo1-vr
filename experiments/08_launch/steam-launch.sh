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
# READING THE RESULT. "process GONE" before ~15 s with no window reproduces the
# exp 8 failure and means Steam's reaper/container wrapper is NOT the missing
# piece -- go to the STEAM_DRM_IPC semaphore hook (RESULTS.md §8). Surviving to
# the 75 s kill with a Black Ops window means the CEG ownership handshake needs
# the real client, and the whole mod must launch through Steam from here on.
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

BEST=0; SAWPROC=0
for i in $(seq 1 "$WATCH"); do
  read -r -t 1 _ </dev/zero   # 1s tick without calling sleep
  PIDS=$(pgrep -f "BlackOps.exe" 2>/dev/null | tr '\n' ' ')
  if [ -n "$PIDS" ]; then
    SAWPROC=1; BEST=$i
    echo "t=${i}s  pids: $PIDS" >> "$OUT/log.txt"
  else
    [ "$SAWPROC" = 1 ] && { echo "t=${i}s  process GONE (lived ~$((BEST))s of watch)" | tee -a "$OUT/log.txt"; break; }
  fi
  # Snapshot the screen and the window list a few times while it is alive.
  if [ "$i" = 20 ] || [ "$i" = 45 ] || [ "$i" = 70 ]; then
    xwd -root -silent > "$OUT/screen_t${i}.xwd" 2>/dev/null && echo "t=${i}s  screenshot" >> "$OUT/log.txt"
    { xdotool search --name . getwindowname %@ 2>/dev/null | grep -iE "black|call of duty" ; } >> "$OUT/windows_t${i}.txt" 2>&1
    wmctrl -l >> "$OUT/windows_t${i}.txt" 2>&1
  fi
done

echo "=== killing at $(date -Is) ===" | tee -a "$OUT/log.txt"
pkill -f "BlackOps.exe" 2>/dev/null
read -r -t 3 _ </dev/zero
pkill -9 -f "BlackOps.exe" 2>/dev/null

find "$GAME" -maxdepth 2 -type f -newermt "2026-01-01" > "$OUT/install_after.txt" 2>/dev/null
md5sum "$GAME/players/config.cfg" > "$OUT/cfg_after.txt" 2>/dev/null
echo "=== install files touched since 2026-01-01 (after) ===" | tee -a "$OUT/log.txt"
diff "$OUT/install_before.txt" "$OUT/install_after.txt" | tee -a "$OUT/log.txt"
echo "=== config.cfg md5 ===" | tee -a "$OUT/log.txt"
cat "$OUT/cfg_before.txt" "$OUT/cfg_after.txt" | tee -a "$OUT/log.txt"
echo "=== saw process: $SAWPROC, last alive tick: ${BEST}s ===" | tee -a "$OUT/log.txt"
