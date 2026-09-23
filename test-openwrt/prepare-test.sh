#!/bin/sh
set -eu
D=/tmp/parental-control-v10
mkdir -p "$D/backup"
cp -p /usr/sbin/parental-control "$D/backup/parental-control.original"
cp -p /usr/sbin/parental-control-web "$D/backup/parental-control-web.original"
cp -a /etc/parental-control "$D/backup/etc-parental-control" 2>/dev/null || true
cp -a /var/lib/parental-control "$D/backup/var-lib-parental-control" 2>/dev/null || true
printf '%s\n' "Backup created in $D/backup"
printf '%s\n' "Copy your reconstructed test binaries into $D as parental-control and parental-control-web."
printf '%s\n' "Do not overwrite /usr/sbin/* for this test."
