#!/bin/bash
# Make the OpenVR runtime reachable from a 32-bit PE that Steam launches.
#
#   ./setup-runtime.sh          install
#   ./setup-runtime.sh remove   put openvrpaths.vrpath back
#
# THE PROBLEM. Proton resolves the OpenVR runtime from VR_OVERRIDE if set,
# otherwise from runtime[0] in ~/.config/openvr/openvrpaths.vrpath (proton
# lines 344-348). We cannot set VR_OVERRIDE -- Steam starts the game and there
# is no environment to set (Exp. 8 §10, Exp. 9). So the config file is the only
# lever, and it already points at WiVRn's xrizer.
#
# But that xrizer ships ONLY bin/linux64/vrclient.so, and a 32-bit PE needs
# bin/vrclient.so. Exp. 5 solved this by staging a copy with the extra symlink
# and pointing VR_OVERRIDE at it; here the same staged directory has to be what
# openvrpaths.vrpath names.
#
# WHAT IS STAGED. Symlinks, never copies, and via flatpak's "active" path -- so
# a WiVRn update is picked up automatically instead of silently pinning an old
# vrclient.so:
#
#   <stage>/bin/linux64/vrclient.so -> <flatpak active>/files/xrizer/bin/linux64/vrclient.so
#   <stage>/bin/vrclient.so         -> linux64/vrclient.so
#
# The staged tree is therefore a strict SUPERSET of what openvrpaths pointed at
# before: 64-bit apps resolve exactly the same file they did, and 32-bit ones
# now resolve too. The previous file is backed up next to itself regardless.
set -u
STAGE="${STAGE:-$HOME/.local/share/bo1vr-xrizer}"
VRPATH="${XDG_CONFIG_HOME:-$HOME/.config}/openvr/openvrpaths.vrpath"
XRIZER="${XRIZER:-$HOME/.local/share/flatpak/app/io.github.wivrn.wivrn/x86_64/stable/active/files/xrizer}"
MODE="${1:-install}"

case "$MODE" in
install)
  [ -f "$XRIZER/bin/linux64/vrclient.so" ] || {
      echo "no xrizer at $XRIZER/bin/linux64/vrclient.so" >&2; exit 1; }

  mkdir -p "$STAGE/bin/linux64"
  ln -sfn "$XRIZER/bin/linux64/vrclient.so" "$STAGE/bin/linux64/vrclient.so"
  ln -sfn linux64/vrclient.so               "$STAGE/bin/vrclient.so"
  [ -e "$XRIZER/bin/version.txt" ] && cp -f "$XRIZER/bin/version.txt" "$STAGE/bin/version.txt" \
      || : > "$STAGE/bin/version.txt"
  echo "staged runtime at $STAGE"
  ls -l "$STAGE/bin" "$STAGE/bin/linux64" | sed 's/^/  /'

  if [ -f "$VRPATH" ] && [ ! -f "$VRPATH.bo1vr-backup" ]; then
      cp -a "$VRPATH" "$VRPATH.bo1vr-backup"
      echo "backed up $VRPATH -> $VRPATH.bo1vr-backup"
  fi
  mkdir -p "$(dirname "$VRPATH")"
  python3 - "$VRPATH" "$STAGE" <<'PY'
import json, sys, os
p, stage = sys.argv[1], sys.argv[2]
d = {"runtime": [], "version": 1}
if os.path.exists(p):
    try: d = json.load(open(p))
    except Exception: pass
d.setdefault("version", 1)
rt = [r for r in d.get("runtime", []) if r != stage]
d["runtime"] = [stage] + rt          # ours first, the old one kept behind it
json.dump(d, open(p, "w"))
print("openvrpaths.vrpath runtime order now:")
for r in d["runtime"]: print("   ", r)
PY
  ;;
remove)
  if [ -f "$VRPATH.bo1vr-backup" ]; then
      mv -f "$VRPATH.bo1vr-backup" "$VRPATH"
      echo "restored $VRPATH"
  else
      echo "no backup to restore" >&2
  fi
  rm -rf "$STAGE"
  echo "removed $STAGE"
  ;;
*) echo "usage: $0 [install|remove]" >&2; exit 2 ;;
esac
