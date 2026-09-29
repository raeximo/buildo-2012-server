#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
cd "$ROOT"

[[ -x ./gs ]] || { echo "Run ./linux-build.sh first." >&2; exit 1; }
[[ -f ./response.txt && -f ./items.dat ]] || { echo "Generated files are missing; run ./linux-build.sh." >&2; exit 1; }

cleanup() {
  [[ -n "${HTTP_PID:-}" ]] && kill "$HTTP_PID" 2>/dev/null || true
  [[ -n "${GS_PID:-}" ]] && kill "$GS_PID" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

python3 ./raw_httpd.py >raw.log 2>&1 & HTTP_PID=$!
BUILDO_ITEMS="$ROOT/items.dat" BUILDO_WORLDS=$'START\nSECOND\nTHIRD' ./gs >gs.log 2>&1 & GS_PID=$!
sleep 1
kill -0 "$HTTP_PID" 2>/dev/null || { echo "HTTP server failed to start; see raw.log." >&2; exit 1; }
kill -0 "$GS_PID" 2>/dev/null || { echo "Game server failed to start; see gs.log." >&2; exit 1; }

echo "HTTP: 127.0.0.1:8081 (raw.log)"
echo "Game: UDP 17091 (gs.log)"
echo "Press Ctrl-C to stop."
wait
