#!/bin/sh
set -eu
D=/tmp/parental-control-v10
[ -f "$D/web.pid" ] && kill "$(cat "$D/web.pid")" 2>/dev/null || true
[ -f "$D/daemon.pid" ] && kill "$(cat "$D/daemon.pid")" 2>/dev/null || true
sleep 1
rm -f /var/run/parental-control.sock
/etc/init.d/parental-control start
/etc/init.d/parental-control-web start
echo "Original OpenWrt services restarted. Original /usr/sbin binaries were never replaced."
