#!/usr/bin/env bash
# Builds the widscan58 web UI from source and puts it in the app's assets.
#
#   scripts/fetch-webui.sh [git ref]          # default: main
#   FIRMWARE=pages scripts/fetch-webui.sh     # also bundle the firmware that
#                                             # the published web UI flashes
#
# Without bundled firmware the in-app flasher downloads it when needed.
# Prints the widscan58 commit it built. GPL-3.0-or-later.
set -euo pipefail

REF="${1:-main}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/widscan58"
OUT="$ROOT/app/src/main/assets/www"
PAGES="https://capnbry.github.io/widscan58"

if [ -d "$SRC/.git" ]; then
  git -C "$SRC" fetch --quiet origin "$REF"
  git -C "$SRC" checkout --quiet FETCH_HEAD
else
  git clone --quiet https://github.com/CapnBry/widscan58 "$SRC"
  git -C "$SRC" checkout --quiet "$REF"
fi

(cd "$SRC/web" && npm ci --no-audit --no-fund --loglevel=error && npm run build >/dev/null)

rm -rf "$OUT"
mkdir -p "$OUT"
cp -R "$SRC/web/dist/." "$OUT/"

if [ "${FIRMWARE:-}" = "pages" ]; then
  mkdir -p "$OUT/firmware"
  curl -fsSL "$PAGES/firmware/manifest.json" -o "$OUT/firmware/manifest.json"
  for part in $(jq -r '.builds[].parts[].path' "$OUT/firmware/manifest.json"); do
    mkdir -p "$OUT/firmware/$(dirname "$part")"
    curl -fsSL "$PAGES/firmware/$part" -o "$OUT/firmware/$part"
  done
fi

git -C "$SRC" rev-parse --short HEAD
