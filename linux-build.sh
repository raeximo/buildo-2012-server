#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
GAME_SRC=${BUILDO_SRC:-"$ROOT/../OldAssBuildoWinFrom2012"}
RUN_DIR=${BUILDO_RUN:-"$ROOT/run"}

if [[ ! -f "$GAME_SRC/Buildo.exe" || ! -f "$GAME_SRC/game/item_definitions.txt" ]]; then
  echo "Game files not found: $GAME_SRC" >&2
  echo "Set BUILDO_SRC to the OldAssBuildoWinFrom2012 directory." >&2
  exit 1
fi

EXPECTED=fbec5a3c16af17c44c4eb54d43fc048df9f18fc9c3f0bc6fcef4ea8a7a933945
ACTUAL=$(sha256sum "$GAME_SRC/Buildo.exe" | cut -d' ' -f1)
if [[ "$ACTUAL" != "$EXPECTED" ]]; then
  echo "Buildo.exe SHA-256 mismatch: $ACTUAL" >&2
  exit 1
fi

command -v c++ >/dev/null || { echo "c++ not found" >&2; exit 1; }
[[ -f /usr/include/enet/enet.h ]] || c++ -E -x c++ - <<< '#include <enet/enet.h>' >/dev/null 2>&1 || {
  echo "ENet development files not found. Install the ENet development package." >&2
  exit 1
}

mkdir -p "$RUN_DIR"
cp -a "$GAME_SRC/." "$RUN_DIR/"
python3 "$ROOT/make_items.py" "$GAME_SRC/game/item_definitions.txt" "$ROOT/items.dat"
mkdir -p "$RUN_DIR/cache"
cp "$ROOT/items.dat" "$RUN_DIR/cache/items.dat"
BUILDO_SRC="$GAME_SRC" BUILDO_OUT="$RUN_DIR" python3 "$ROOT/patch_client.py"
c++ -O2 -std=c++11 -o "$ROOT/gs" "$ROOT/server.cpp" -lenet
printf 'server|127.0.0.1\nport|17091\ntype|1\nRTENDMARKERBS1001' > "$ROOT/response.txt"

echo "Build complete: $RUN_DIR/Buildo-local.exe"
