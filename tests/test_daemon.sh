#!/usr/bin/env bash
# Tests of the daemon protocol: run -d, list, logs, logs -f, attach, kill, net-from
source "$(dirname "$0")/lib.sh"
check_env
echo "== daemon tests"

export MINIRT_HOME="$(mktemp -d /tmp/minirt-test.XXXXXX)"
"$RT" daemon 2>"$MINIRT_HOME/daemon.err" &
DAEMON=$!
cleanup() { kill "$DAEMON" 2>/dev/null; wait "$DAEMON" 2>/dev/null; rm -rf "$MINIRT_HOME"; }
trap cleanup EXIT

wait_for 5 test -S "$MINIRT_HOME/daemon.sock" || { bad "daemon starts"; finish; exit 1; }
ok "daemon starts and listens on a unix socket"

pid_of()    { "$RT" list | awk -v n="$1" '$2==n {print $3}'; }
status_of() { "$RT" list | awk -v n="$1" '$2==n {print $4}'; }
is_exited() { [[ "$(status_of "$1")" == exited* ]]; }
strip_cr()  { tr -d '\r'; }

# --- run -d + list
id=$("$RT" run -d --name web "$ROOTFS" sh -c 'echo started; sleep 1000')
[ -n "$id" ] && ok "run -d prints a container id ($id)" || bad "run -d prints a container id"
assert_eq "list shows the container as running" "running" "$(status_of web)"
assert_contains "list shows the command" "sleep 1000" "$("$RT" list)"
host_pid=$(pid_of web)
[ -d "/proc/$host_pid" ] && ok "list shows a real host pid ($host_pid)" || bad "list shows a real host pid"

# --- name must be unique
"$RT" run -d --name web "$ROOTFS" true 2>/dev/null
[ $? -ne 0 ] && ok "duplicate name is rejected" || bad "duplicate name is rejected"

# --- logs
wait_for 5 bash -c "\"$RT\" logs web | grep -q started"
assert_contains "logs shows the container output" "started" "$("$RT" logs web | strip_cr)"
assert_contains "logs also works with an id prefix" "started" "$("$RT" logs "${id:0:4}" | strip_cr)"

# --- logs -f follows until the container exits
"$RT" run -d --name ticker "$ROOTFS" sh -c 'for i in 1 2 3; do echo tick$i; sleep 0.5; done' >/dev/null
out=$(timeout 15 "$RT" logs -f ticker | strip_cr)
assert_contains "logs -f receives live output" "tick3" "$out"
assert_eq "logs -f ends when the container exits" "0" "$?"

# --- exit code is stored
"$RT" run -d --name quick "$ROOTFS" sh -c 'exit 3' >/dev/null
wait_for 5 is_exited quick
assert_eq "exit code is recorded in list" "exited(3)" "$(status_of quick)"

# --- attach: interactive shell through the daemon
"$RT" run -d --name shell "$ROOTFS" sh >/dev/null
out=$(printf 'echo attached-$((20+22))\nexit\n' | timeout 10 "$RT" attach shell 2>&1 | strip_cr)
assert_contains "attach sends input and shows output (PTY)" "attached-42" "$out"
wait_for 5 is_exited shell
assert_eq "container exits after 'exit' typed in attach" "exited(0)" "$(status_of shell)"
"$RT" attach shell 2>/dev/null
[ $? -ne 0 ] && ok "attach to a stopped container fails" || bad "attach to a stopped container fails"

# --- kill: graceful (SIGTERM, then SIGKILL after timeout)
"$RT" kill -t 1 web >/dev/null
wait_for 6 is_exited web
assert_eq "kill stops the container (killed by SIGKILL = 137)" "exited(137)" "$(status_of web)"

# --- kill by host pid and --force
"$RT" run -d --name victim "$ROOTFS" sleep 1000 >/dev/null
vpid=$(pid_of victim)
"$RT" kill --force "$vpid" >/dev/null
wait_for 5 is_exited victim
assert_eq "kill by pid with --force" "exited(137)" "$(status_of victim)"

# --- kill with an explicit signal: a shell trap exits cleanly on SIGTERM
"$RT" run -d --name polite "$ROOTFS" sh -c 'trap "exit 0" TERM; while true; do sleep 0.2; done' >/dev/null
sleep 0.5
"$RT" kill polite >/dev/null
wait_for 6 is_exited polite
assert_eq "graceful stop: program handles SIGTERM itself" "exited(0)" "$(status_of polite)"

# --- shared network between containers (--net-from)
"$RT" run -d --name solo "$ROOTFS" sleep 1000 >/dev/null
"$RT" run -d --name peer --net-from solo "$ROOTFS" sleep 1000 >/dev/null
sleep 0.5
a=$(readlink "/proc/$(pid_of solo)/ns/net"); b=$(readlink "/proc/$(pid_of peer)/ns/net")
assert_eq "--net-from: containers share one network namespace" "$a" "$b"
c=$(readlink "/proc/$(pid_of solo)/ns/pid"); d=$(readlink "/proc/$(pid_of peer)/ns/pid")
[ "$c" != "$d" ] && ok "--net-from: PID namespaces stay separate" || bad "--net-from: PID namespaces stay separate"
out=$("$RT" run -d --net-from nosuch "$ROOTFS" true 2>&1)
assert_contains "--net-from unknown container is an error" "no such container" "$out"

# --- overlay in daemon mode
if "$RT" run --overlay "$ROOTFS" true 2>/dev/null; then
  "$RT" run -d --name ov --overlay "$ROOTFS" sh -c 'touch /x && echo overlay-ok' >/dev/null
  wait_for 5 bash -c "\"$RT\" logs ov | grep -q overlay-ok"
  assert_contains "run -d --overlay" "overlay-ok" "$("$RT" logs ov | strip_cr)"
fi

# --- rm
"$RT" rm quick >/dev/null
assert_not_contains "rm removes an exited container" "quick" "$("$RT" list)"
"$RT" rm solo 2>/dev/null
[ $? -ne 0 ] && ok "rm refuses a running container" || bad "rm refuses a running container"

# --- daemon shutdown kills the remaining containers
solo_pid=$(pid_of solo)
kill "$DAEMON"; wait "$DAEMON" 2>/dev/null
sleep 0.3
[ ! -d "/proc/$solo_pid" ] && ok "stopping the daemon leaves no orphan containers" || bad "orphan container left"
[ ! -S "$MINIRT_HOME/daemon.sock" ] && ok "socket removed on shutdown" || bad "socket left behind"

finish
