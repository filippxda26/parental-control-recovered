#!/bin/sh
set -eu
D=/tmp/parental-control-v10
[ -x "$D/parental-control" ] || { echo "Missing $D/parental-control"; exit 1; }
[ -x "$D/parental-control-web" ] || { echo "Missing $D/parental-control-web"; exit 1; }
/etc/init.d/parental-control-web stop || true
/etc/init.d/parental-control stop || true
sleep 1
"$D/parental-control" >"$D/daemon.log" 2>&1 & echo $! >"$D/daemon.pid"
sleep 1
"$D/parental-control-web" 127.0.0.1 5000 >"$D/web.log" 2>&1 & echo $! >"$D/web.pid"
sleep 1
echo "v10 test services started. Web is bound to 127.0.0.1:5000 only."
echo "Check: wget -O- http://127.0.0.1:5000/api/health"
