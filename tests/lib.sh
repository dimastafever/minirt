#!/usr/bin/env bash
# Tiny test helpers. Override paths with RT=... ROOTFS=...
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RT="${RT:-$ROOT/build/runtime}"
ROOTFS="${ROOTFS:-$ROOT/rootfs/alpine}"
PASS=0
FAIL=0

ok()  { echo "  ok   - $1"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL - $1"; [ -n "${2:-}" ] && echo "         $2"; FAIL=$((FAIL + 1)); }

assert_eq()       { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1" "expected '$2', got '$3'"; fi; }
assert_contains() { case "$3" in *"$2"*) ok "$1" ;; *) bad "$1" "'$2' not found in: $3" ;; esac; }
assert_not_contains() { case "$3" in *"$2"*) bad "$1" "'$2' unexpectedly found in: $3" ;; *) ok "$1" ;; esac; }

# wait_for SECONDS cmd...   (polls every 0.1 s)
wait_for() {
  local t=$1; shift
  for _ in $(seq $((t * 10))); do "$@" >/dev/null 2>&1 && return 0; sleep 0.1; done
  return 1
}

check_env() {
  [ -x "$RT" ] || { echo "build first: make"; exit 2; }
  [ -d "$ROOTFS/bin" ] || { echo "SKIP: no rootfs at $ROOTFS (run scripts/get_rootfs.sh)"; exit 0; }
  if ! "$RT" run "$ROOTFS" true 2>/tmp/minirt-env-check.log; then
    echo "SKIP: cannot start a rootless container here:"; cat /tmp/minirt-env-check.log; exit 0
  fi
}

finish() { echo "passed=$PASS failed=$FAIL"; [ "$FAIL" -eq 0 ]; }
