#!/usr/bin/env bash
# Tests of the container core (no daemon): `runtime run ...`
source "$(dirname "$0")/lib.sh"
check_env
echo "== container tests"

out=$("$RT" run "$ROOTFS" echo hello)
assert_eq "runs a command and returns its output" "hello" "$out"

out=$("$RT" run "$ROOTFS" sh -c 'echo $$')
assert_eq "process is PID 1 in its PID namespace" "1" "$out"

out=$("$RT" run "$ROOTFS" sh -c 'cat /proc/1/cmdline | tr "\0" " "')
assert_contains "/proc is mounted and shows only the container" "sh -c" "$out"

"$RT" run "$ROOTFS" sh -c 'exit 42'; assert_eq "exit code is propagated" "42" "$?"
"$RT" run "$ROOTFS" true;             assert_eq "exit code 0" "0" "$?"
"$RT" run "$ROOTFS" /nonexistent 2>/dev/null; assert_eq "missing binary gives 127" "127" "$?"

out=$("$RT" run "$ROOTFS" id -u)
assert_eq "rootless: current user is mapped to uid 0" "0" "$out"

host_name=$(hostname)
out=$("$RT" run --hostname boxy "$ROOTFS" hostname)
assert_eq "UTS namespace: own hostname" "boxy" "$out"
assert_eq "host hostname is untouched" "$host_name" "$(hostname)"

out=$("$RT" run "$ROOTFS" sh -c 'ls /.oldroot 2>&1; ls /home/claude 2>&1' | tr '\n' ' ')
assert_contains "old root is not reachable (pivot_root)" "No such file" "$out"

out=$("$RT" run "$ROOTFS" sh -c 'echo data > /dev/null && echo ok')
assert_eq "/dev/null works" "ok" "$out"

out=$("$RT" run "$ROOTFS" sh -c 'ip -o link | wc -l')
assert_eq "network namespace: only loopback exists" "1" "$out"
out=$("$RT" run "$ROOTFS" sh -c 'ip link show lo')
assert_contains "loopback is up" "UP" "$out"

out=$(echo "from-stdin" | "$RT" run "$ROOTFS" cat)
assert_eq "stdin is forwarded" "from-stdin" "$out"

out=$("$RT" run -e FOO=bar "$ROOTFS" sh -c 'echo $FOO')
assert_eq "environment variables (-e)" "bar" "$out"

# interactive PTY mode: input goes through a pseudo terminal
out=$("$RT" run -t "$ROOTFS" sh -c '[ -t 0 ] && [ -t 1 ] && echo is-a-tty' </dev/null | tr -d '\r')
assert_eq "-t gives the process a terminal (isatty)" "is-a-tty" "$out"
out=$(printf 'echo $((6*7))\nexit\n' | "$RT" run -t "$ROOTFS" sh 2>&1 | tr -d '\r')
assert_contains "interactive shell works through the PTY" "42" "$out"
printf 'exit 7\n' | "$RT" run -t "$ROOTFS" sh >/dev/null 2>&1
assert_eq "exit code through PTY mode" "7" "$?"

# overlayfs: writes go to the upper layer, the rootfs stays pristine
if "$RT" run --overlay "$ROOTFS" true 2>/dev/null; then
  out=$("$RT" run --overlay "$ROOTFS" sh -c 'touch /overlay-marker && ls /overlay-marker')
  assert_eq "overlay: container can write to its root" "/overlay-marker" "$out"
  [ ! -e "$ROOTFS/overlay-marker" ] && ok "overlay: rootfs is not modified" || bad "overlay: rootfs was modified"
else
  echo "  skip - overlayfs not available (needs Linux >= 5.11)"
fi

# two containers must not share a network namespace by default
"$RT" run "$ROOTFS" sleep 2 & pid_a=$!
sleep 0.5
a_net=$(readlink /proc/$(pgrep -P $pid_a | head -1)/ns/net 2>/dev/null)
[ -n "$a_net" ] && [ "$a_net" != "$(readlink /proc/$$/ns/net)" ] && ok "container has its own net namespace" \
  || bad "container has its own net namespace" "$a_net"
wait $pid_a

finish
