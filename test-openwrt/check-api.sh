#!/bin/sh
set -u
BASE=http://127.0.0.1:5000
for p in /api/health /api/status /api/devices /api/state /api/discovered /api/history; do
  echo "===== $p ====="
  wget -qO- "$BASE$p" || echo "request failed"
  echo
done
