#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DATA_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/embr"
VENV_DIR="$DATA_DIR/.venv"
TMP_VENV="$DATA_DIR/.venv.tmp"

mkdir -p "$DATA_DIR"

cleanup() {
    if [ $? -ne 0 ]; then
        echo "ERROR: Setup failed. Cleaning up..." >&2
        rm -rf "$TMP_VENV"
        if [ -d "$VENV_DIR.old" ]; then
            mv "$VENV_DIR.old" "$VENV_DIR"
            echo "Rolled back to previous venv." >&2
        fi
        exit 1
    fi
}
trap cleanup EXIT

# Build everything in a temp venv.
rm -rf "$TMP_VENV"
python3 -m venv "$TMP_VENV"
"$TMP_VENV/bin/pip" install "cloakbrowser[geoip]"
"$TMP_VENV/bin/python" -m cloakbrowser install

# Swap atomically.
if [ -d "$VENV_DIR" ]; then
    mv "$VENV_DIR" "$VENV_DIR.old"
fi
mv "$TMP_VENV" "$VENV_DIR"
rm -rf "$VENV_DIR.old"

# Download ad/tracker blocklist into the package dir (next to embr.py).
BLOCKLIST="$SCRIPT_DIR/blocklist.txt"
echo "Downloading ad blocklist..."
curl -sL "https://raw.githubusercontent.com/StevenBlack/hosts/master/hosts" \
    | grep "^0\.0\.0\.0 " \
    | awk '{print $2}' \
    | sort -u > "$BLOCKLIST.tmp"
mv "$BLOCKLIST.tmp" "$BLOCKLIST"
echo "Blocklist: $(wc -l < "$BLOCKLIST") domains"

# Compile embr-booster (C transport proxy) if a C compiler is available.
# Binary goes into DATA_DIR (alongside the venv), source stays in package dir.
# Source may be under libexec/ (repo) or flat (elpaca/straight build dir).
BOOSTER_SRC="$SCRIPT_DIR/libexec/embr-booster.c"
[ -f "$BOOSTER_SRC" ] || BOOSTER_SRC="$SCRIPT_DIR/embr-booster.c"
BOOSTER_BIN="$DATA_DIR/embr-booster"
if [ -f "$BOOSTER_SRC" ]; then
    CC="${CC:-cc}"
    if command -v "$CC" >/dev/null 2>&1; then
        echo "Compiling embr-booster..."
        "$CC" -O2 -std=c11 -Wall -o "$BOOSTER_BIN" "$BOOSTER_SRC" && \
            echo "embr-booster compiled to $BOOSTER_BIN" || \
            echo "WARNING: embr-booster compilation failed (optional, embr works without it)." >&2
    else
        echo "No C compiler found — skipping embr-booster (optional)."
    fi
fi

echo "Setup complete. CloakBrowser is ready."
