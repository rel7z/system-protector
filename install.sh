#!/usr/bin/env bash
# Thin wrapper: build + symlink system-protector onto PATH, then open the menu.
#   sudo ./install.sh          (systemd, recommended)
#   ./install.sh               (per-user cron fallback, no root)
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
"$DIR/system-protector" self-install
echo
echo "Now set up a program with:  system-protector add   (or just: system-protector)"
