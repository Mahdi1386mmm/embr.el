#!/bin/bash
set -euo pipefail

DATA_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/better-eww"
BROWSERS_DIR="${XDG_CACHE_HOME:-$HOME/.cache}/ms-playwright"

echo "This will delete:"
echo "  Data dir (venv + profile):  $DATA_DIR"
echo ""

read -rp "Proceed? [y/N] " confirm
if [[ "$confirm" != [yY] ]]; then
    echo "Aborted."
    exit 0
fi

if [ -d "$DATA_DIR" ]; then
    rm -rf "$DATA_DIR"
    echo "Deleted $DATA_DIR"
else
    echo "$DATA_DIR not found, skipping."
fi

echo ""
read -rp "Also delete Playwright's shared browser cache ($BROWSERS_DIR)? [y/N] " confirm2
if [[ "$confirm2" == [yY] ]] && [ -d "$BROWSERS_DIR" ]; then
    rm -rf "$BROWSERS_DIR"
    echo "Deleted $BROWSERS_DIR"
fi

echo ""
echo "Uninstall complete. Remove the Emacs package with your package manager."
