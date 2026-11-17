#!/usr/bin/env bash
#
# update.sh — safely update system-protector from GitHub.
#
# It is written so a bad update can't take your protected programs down:
#   1. pull latest (fast-forward only; refuses if it would need a merge)
#   2. build the new watchdog to a TEMP file and smoke-test it
#   3. only if that works, atomically swap it in (old binary kept as .bak)
#   4. refresh the PATH symlinks + systemd unit
#   5. restart each running instance so it picks up the new watchdog
#
# If any step before the swap fails, nothing changes and your programs keep
# running on the old binary.
#
#   sudo ./update.sh      (systemd setups)
#   ./update.sh           (per-user / cron setups)
#
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR"

CONFDIR="/etc/system-protector"
URUN="$HOME/.local/share/system-protector"
CC="$(command -v cc || command -v gcc || true)"

B=""; G=""; Y=""; R=""; N=""
if [ -t 1 ]; then B=$'\033[1m'; G=$'\033[32m'; Y=$'\033[33m'; R=$'\033[31m'; N=$'\033[0m'; fi
ok()   { printf '%s>>%s %s\n' "$G" "$N" "$*"; }
warn() { printf '%s!!%s %s\n' "$Y" "$N" "$*"; }
die()  { printf '%s** %s%s\n' "$R" "$*" "$N" >&2; exit 1; }

[ -n "$CC" ] || die "No C compiler found (install build-essential)."
[ -d .git ] || die "Not a git checkout. Re-clone from GitHub to use update.sh."

# ---- 1. pull latest, fast-forward only -----------------------------------
ok "Checking for updates..."
if ! git diff --quiet || ! git diff --cached --quiet; then
  die "You have local uncommitted changes. Commit/stash them first, then re-run."
fi

git fetch --quiet origin || die "git fetch failed (network/auth?)."
BRANCH="$(git rev-parse --abbrev-ref HEAD)"
LOCAL="$(git rev-parse @)"
REMOTE="$(git rev-parse "@{u}" 2>/dev/null || echo "")"
[ -n "$REMOTE" ] || die "No upstream set for branch '$BRANCH'."

if [ "$LOCAL" = "$REMOTE" ]; then
  ok "Already up to date. Rebuilding anyway to be safe."
else
  ok "Updating $BRANCH: ${LOCAL:0:7} -> ${REMOTE:0:7}"
  git merge --ff-only "@{u}" || die "Can't fast-forward (history diverged). Resolve manually."
fi

# ---- 2. build to a temp file + smoke test --------------------------------
ok "Building new watchdog (test build)..."
if ! "$CC" -O2 -Wall -Wextra -o protector.new protector.c; then
  rm -f protector.new
  die "Build failed. Nothing changed — your programs are still running the old binary."
fi
if ! ./protector.new --help >/dev/null 2>&1; then
  rm -f protector.new
  die "New binary failed its smoke test. Aborted; old binary left in place."
fi
ok "New binary builds and runs."

# ---- 3. atomic swap, keep a backup ---------------------------------------
if [ -f protector ]; then cp -f protector protector.bak; fi
mv -f protector.new protector
ok "Swapped in new binary (previous kept as protector.bak)."

# ---- 4. refresh symlinks / unit ------------------------------------------
if [ -x ./system-protector ]; then
  ./system-protector self-install || warn "self-install step reported a problem."
fi

# ---- 5. restart running instances so they use the new watchdog -----------
restarted=0
if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ] && [ -d "$CONFDIR" ]; then
  for c in "$CONFDIR"/*.conf; do
    [ -e "$c" ] || continue
    name="$(basename "$c" .conf)"
    [ "$name" = "example" ] && continue
    if systemctl is-active --quiet "system-protector@$name"; then
      ok "Restarting system-protector@$name"
      systemctl restart "system-protector@$name" && restarted=$((restarted+1)) \
        || warn "Failed to restart $name (rollback: cp protector.bak protector && systemctl restart system-protector@$name)"
    fi
  done
fi

# per-user cron instances: stop the old watchdog, let the guard relaunch it
if [ -d "$URUN" ]; then
  for g in "$URUN"/guard-*.sh; do
    [ -e "$g" ] || continue
    name="$(basename "$g" .sh)"; name="${name#guard-}"
    pf="$URUN/$name.pid"
    if [ -f "$pf" ] && kill -0 "$(cat "$pf" 2>/dev/null)" 2>/dev/null; then
      ok "Cycling cron instance '$name' onto new binary"
      kill "$(cat "$pf")" 2>/dev/null || true
      sleep 1
      "$g" || warn "Could not relaunch '$name'; cron will within a minute."
      restarted=$((restarted+1))
    fi
  done
fi

ok "${B}Update complete.${N} Restarted $restarted running instance(s)."
echo "Roll back if needed:  cp protector.bak protector && ./update.sh is not required — just restart instances."
