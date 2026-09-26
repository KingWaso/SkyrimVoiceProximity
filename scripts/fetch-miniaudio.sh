#!/usr/bin/env bash
# Fetches the single-header miniaudio.h into deps/miniaudio (or a path you
# pass as $1). Works in MSYS2 MinGW64 shell, Git Bash, WSL, or plain Linux --
# needs curl, which MSYS2/Git Bash both ship with.
set -euo pipefail

DEST_DIR="${1:-deps/miniaudio}"
URL_PRIMARY="https://raw.githubusercontent.com/mackron/miniaudio/master/miniaudio.h"
URL_FALLBACK="https://raw.githubusercontent.com/mackron/miniaudio/main/miniaudio.h"

mkdir -p "$DEST_DIR"

echo "Fetching miniaudio.h into $DEST_DIR ..."
if curl -fsSL -o "$DEST_DIR/miniaudio.h" "$URL_PRIMARY"; then
    echo "Downloaded from 'master' branch."
elif curl -fsSL -o "$DEST_DIR/miniaudio.h" "$URL_FALLBACK"; then
    echo "Downloaded from 'main' branch."
else
    echo "Failed to download miniaudio.h from either branch -- check your network / the repo URL." >&2
    exit 1
fi

# miniaudio.h is a large single-file header (several hundred KB). If it came
# back tiny, something went wrong (rate limit, redirect to an HTML page, etc).
SIZE=$(wc -c < "$DEST_DIR/miniaudio.h" | tr -d ' ')
if [ "$SIZE" -lt 100000 ]; then
    echo "Warning: downloaded file is only $SIZE bytes -- that's too small to be the real header. Check $DEST_DIR/miniaudio.h manually." >&2
    exit 1
fi

echo "Done: $DEST_DIR/miniaudio.h ($SIZE bytes)"
