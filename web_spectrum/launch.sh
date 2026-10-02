#!/bin/bash
# FobosOne web spectrum backend dispatcher.
# Runs whichever backend `backend.conf` selects ("cpp" or "python"). Works for both the
# installed layout (bin/spectrumd, python/app.pyc) and the dev source tree (cpp/spectrumd,
# python/app.py). The app's Configure tab re-execs this script to switch backends.
cd "$(dirname "$(readlink -f "$0")")" || exit 1
export FOBOS_LAUNCHER="$(readlink -f "$0")"
PORT="${FOBOS_PORT:-8080}"
BE="$(tr -d '[:space:]' < backend.conf 2>/dev/null)"
[ -z "$BE" ] && BE=cpp
if [ "$BE" = "python" ]; then
  APP=python/app.pyc; [ -f "$APP" ] || APP=python/app.py
  echo "launch: python backend ($APP) on :$PORT"
  exec python3 "$APP" --port "$PORT"
else
  BIN=bin/spectrumd; [ -f "$BIN" ] || BIN=cpp/spectrumd
  echo "launch: cpp backend ($BIN) on :$PORT"
  exec "$BIN" --port "$PORT"
fi
