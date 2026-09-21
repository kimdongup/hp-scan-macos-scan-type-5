#!/usr/bin/env bash
# Per-user local-only scanner service; no root privileges or LAN listener.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LABEL="io.github.kimdongup.hp-soapht-airscan"
SERVICE="gui/$(id -u)/$LABEL"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
DEST="$HOME/Library/Application Support/HP SOAPHT AirScan"
LOG_DIR="$HOME/Library/Logs/HP SOAPHT AirScan"
case "${1:-status}" in
 install)
  OUT_DIR="${OUT_DIR:-$ROOT/build/airscan}"
  test -x "$OUT_DIR/airscan-bridge"
  test -x "$OUT_DIR/hp-soapht-probe"
  mkdir -p "$DEST" "$LOG_DIR" "$(dirname "$PLIST")"
  launchctl bootout "$SERVICE" >/dev/null 2>&1 || true
  # bootout may return before native CancelJob cleanup finishes. Refuse to
  # replace binaries if the old service still has a listener.
  if lsof -nP -iTCP:"${AIRSCAN_PORT:-8089}" -sTCP:LISTEN >/dev/null 2>&1; then
   echo "Port is still in use; stop the active bridge and wait for scan cleanup." >&2; exit 1
  fi
  install -m 0755 "$OUT_DIR/airscan-bridge" "$OUT_DIR/hp-soapht-probe" "$DEST/"
  python3 - "$PLIST" "$DEST" "$LOG_DIR" <<'PY'
import os,plistlib,sys
plist,dest,logs=sys.argv[1:]
env={'PATH':'/opt/homebrew/bin:/usr/bin:/bin','AIRSCAN_PROBE':dest+'/hp-soapht-probe'}
for key in ('AIRSCAN_DEVICE','AIRSCAN_PORT','AIRSCAN_SCANIMAGE','AIRSCAN_DEBUG'):
 if os.environ.get(key):env[key]=os.environ[key]
data={'Label':'io.github.kimdongup.hp-soapht-airscan','ProgramArguments':[dest+'/airscan-bridge'],
 'RunAtLoad':True,'KeepAlive':True,'ThrottleInterval':15,'ExitTimeOut':400,
 'EnvironmentVariables':env,'StandardOutPath':logs+'/bridge.log','StandardErrorPath':logs+'/bridge.log'}
with open(plist,'wb') as f:plistlib.dump(data,f)
PY
  launchctl bootstrap "gui/$(id -u)" "$PLIST"
  echo "Installed local scanner service. Log: $LOG_DIR/bridge.log"
  ;;
 stop) launchctl bootout "$SERVICE" ;;
 start) launchctl bootstrap "gui/$(id -u)" "$PLIST" ;;
 status) launchctl print "$SERVICE" ;;
 uninstall)
  launchctl bootout "$SERVICE" >/dev/null 2>&1 || true
  if [[ -f "$PLIST" ]]; then rm "$PLIST"; fi
  echo "LaunchAgent removed. Binaries/logs retained in $DEST and $LOG_DIR."
  ;;
 *) echo "Usage: $0 install|start|stop|status|uninstall" >&2; exit 2 ;;
esac
