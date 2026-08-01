#!/bin/bash
# gdb-attach.sh WORK GAMEDIR -- attach winedbg --gdb to the already-running
# BlackOps.exe and break in OUR code inside it.
#
# Attaching to a live process rather than launching under the debugger is
# deliberate, and answers two questions in one run:
#   * whether a breakpoint in our own .asi resolves and fires inside the real
#     game (Exp. 3 proved this for a toy target under CLASSIC WoW64; this is the
#     first time it is tried under NEW-WoW64, which Exp. 3 explicitly left
#     untested and flagged as the risky case);
#   * whether BO1 notices. The probe's heartbeat logs the moment
#     IsDebuggerPresent flips, so the game's reaction -- or lack of one -- is
#     recorded from inside the process.
#
# The Windows pid comes out of the probe's own log; winedbg takes a Windows pid,
# not a unix one, so it cannot be taken from pgrep.
set -uo pipefail

WORK="${1:?usage: gdb-attach.sh WORK GAMEDIR}"
GAME="${2:?usage: gdb-attach.sh WORK GAMEDIR}"

WPID_HEX="$(grep -oE 'pid *= *[0-9]+ \(0x[0-9a-f]+\)' "$GAME/bo1probe.log" 2>/dev/null \
            | head -1 | grep -oE '0x[0-9a-f]+')"
WPID_DEC="$(grep -oE 'pid *= *[0-9]+' "$GAME/bo1probe.log" 2>/dev/null | head -1 | grep -oE '[0-9]+$')"
[ -n "${WPID_DEC:-}" ] || { echo "gdb-attach: no pid in $GAME/bo1probe.log"; exit 1; }
echo "== attaching winedbg --gdb to Windows pid $WPID_DEC ($WPID_HEX)"

# Exp. 3's one gotcha: gdb (unlike native winedbg) does NOT defer a breakpoint
# on a symbol from a not-yet-known module, and every symbol we care about lives
# in a dynamically loaded DLL. Without "set breakpoint pending on" the whole
# session silently does nothing.
#
# THE SECOND GOTCHA, found here and not in Exp. 3 (which never attached to a
# running process): under new-WoW64 winedbg's attach leaves the break-in thread
# sitting at a garbage EIP, 0xfff4fbd0. MEASURED to be a Wine artifact and not
# BO1 anti-debug, by attaching identically to a CEG-free host of our own -- it
# stops at the very same 0xfff4fbd0. If that thread is ever resumed (by
# `continue` or, worse, by `finish` on its corrupt frame) it faults immediately,
# nothing handles the AV, and the whole process dies with 0xC0000005.
#
# "nopass" is what makes an attach survivable: gdb neither stops for the signal
# nor delivers it to the inferior, so the bogus thread spins harmlessly instead
# of taking the process down with it, and every other thread keeps running.
CMDS=$(cat <<'EOF'
set pagination off
set confirm off
set breakpoint pending on
echo \n===== sharedlibrary =====\n
info sharedlibrary
echo \n===== break in our own code =====\n
break bo1probe_breakpoint_target
handle SIGSEGV nostop noprint nopass
continue
echo \n===== HIT: backtrace =====\n
bt
echo \n===== args / locals / globals =====\n
info args
info locals
print g_bo1probe_heartbeat
print g_bo1probe_last
echo \n===== registers =====\n
info registers eip esp ebp
echo \n===== second hit proves it is periodic, not a one-off =====\n
continue
bt
info args
echo \n===== detach, leave the game running =====\n
delete
detach
quit
EOF
)

cd "$GAME"
export STEAM_COMPAT_CLIENT_INSTALL_PATH="$HOME/.steam/steam"
export STEAM_COMPAT_DATA_PATH="$WORK/pfx"
export PROTON_USE_WOW64=1
export SteamGameId=42700 SteamAppId=42700
unset PROTON_LOG WINEDEBUG

printf '%s\n' "$CMDS" | timeout -k 10 120 "$WORK/proton/proton" runinprefix \
    winedbg --gdb "$WPID_DEC" 2>&1
echo "== winedbg exited $?"
