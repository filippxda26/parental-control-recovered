#!/bin/sh
set -eu
D=/tmp/parental-control-v10

[ -f "$D/backup/.prepared" ] || {
  echo "Missing completed backup. Run $D/prepare-test.sh (or repository test-openwrt/prepare-test.sh) first."
  exit 1
}
[ -x "$D/parental-control" ] || { echo "Missing $D/parental-control"; exit 1; }
[ -x "$D/parental-control-web" ] || { echo "Missing $D/parental-control-web"; exit 1; }

if [ -f "$D/web.pid" ] && kill -0 "$(cat "$D/web.pid")" 2>/dev/null; then
  echo "Test web process is already running."
  exit 1
fi
if [ -f "$D/daemon.pid" ] && kill -0 "$(cat "$D/daemon.pid")" 2>/dev/null; then
  echo "Test daemon process is already running."
  exit 1
fi
rm -f "$D/web.pid" "$D/daemon.pid"

/etc/init.d/parental-control-web stop || true
/etc/init.d/parental-control stop || true
sleep 1

"$D/parental-control" >"$D/daemon.log" 2>&1 &
echo $! >"$D/daemon.pid"
sleep 1
if ! kill -0 "$(cat "$D/daemon.pid")" 2>/dev/null; then
  echo "Test daemon failed to start:"
  cat "$D/daemon.log" 2>/dev/null || true
  /etc/init.d/parental-control start || true
  /etc/init.d/parental-control-web start || true
  exit 1
fi

"$D/parental-control-web" 127.0.0.1 5000 >"$D/web.log" 2>&1 &
echo $! >"$D/web.pid"
sleep 1
if ! kill -0 "$(cat "$D/web.pid")" 2>/dev/null; then
  echo "Test web service failed to start:"
  cat "$D/web.log" 2>/dev/null || true
  kill "$(cat "$D/daemon.pid")" 2>/dev/null || true
  rm -f "$D/web.pid" "$D/daemon.pid"
  /etc/init.d/parental-control start || true
  /etc/init.d/parental-control-web start || true
  exit 1
fi

echo "Test services started. Web is bound to 127.0.0.1:5000 only."
echo "Run: $D/check-api.sh"
