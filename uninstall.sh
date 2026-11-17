#!/usr/bin/env bash
# Thin wrapper around: system-protector uninstall
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
exec "$DIR/system-protector" uninstall
