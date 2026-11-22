#!/usr/bin/env bash
# CLI tests: settings, validation, JSON status, start/stop of a stand-in renderer.
# Everything runs in a scratch HOME/XDG tree with systemd disabled and a fake
# renderer, so nothing reaches the real session, user manager or display.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
CLI="$HERE/../bin/letitsnow"
T=$(mktemp -d "${TMPDIR:-/tmp}/letitsnow-test.XXXXXX")
trap 'pkill -TERM -f "$T/fake/letitsnowd" 2>/dev/null; rm -rf "$T"' EXIT
mkdir -p "$T/home" "$T/run" "$T/config" "$T/state" "$T/data" "$T/cache" "$T/fake"; chmod 700 "$T/run"

# A renderer that behaves like the real one where the CLI can see it: it is
# called letitsnowd (the CLI signals nothing else) and holds
# $XDG_RUNTIME_DIR/letitsnow-<display>.lock with its pid in it until SIGTERM.
cat > "$T/fake/letitsnowd" <<'FAKE'
#!/usr/bin/bash
exec 9> "$XDG_RUNTIME_DIR/letitsnow-wayland-test.lock"
flock -w 1.5 9 || exit 0   # retries like letitsnowd: status probes take the lock for an instant
echo $$ >&9
trap 'exit 0' TERM
while :; do sleep 0.2; done
FAKE
chmod +x "$T/fake/letitsnowd"

run() {
	env -i PATH=/usr/bin:/bin HOME="$T/home" XDG_CONFIG_HOME="$T/config" XDG_STATE_HOME="$T/state" \
		XDG_DATA_HOME="$T/data" XDG_CACHE_HOME="$T/cache" XDG_RUNTIME_DIR="$T/run" \
		LETITSNOW_NO_SYSTEMD=1 LETITSNOWD="$T/fake/letitsnowd" "$CLI" "$@"
}

# The isolation itself, checked before anything else runs.
probe=$(env -i PATH=/usr/bin:/bin HOME="$T/home" XDG_CONFIG_HOME="$T/config" XDG_RUNTIME_DIR="$T/run" sh -c 'echo "$HOME|$XDG_CONFIG_HOME|$XDG_RUNTIME_DIR|${WAYLAND_DISPLAY:-}|${DBUS_SESSION_BUS_ADDRESS:-}"')
[ "$probe" = "$T/home|$T/config|$T/run||" ] || { echo "isolation probe failed: $probe"; exit 1; }

pass=0 fail=0
check() { # check NAME EXPECTED ACTUAL
	if [ "$2" = "$3" ]; then pass=$((pass + 1)); else fail=$((fail + 1)); printf 'FAIL %s\n  want: %s\n  got:  %s\n' "$1" "$2" "$3"; fi
}
fails() { # fails NAME CMD...: must exit non-zero
	local name=$1; shift
	if run "$@" > /dev/null 2>&1; then fail=$((fail + 1)); echo "FAIL $name: accepted"; else pass=$((pass + 1)); fi
}

check "defaults" '{"version":"'"$(cat "$HERE/../VERSION")"'","running":false,"service":"none","unit":false,"autostart":false,"speed":1,"density":1,"opacity":1,"layer":"desktop","fps":60}' "$(run status --json)"
run speed 1.50 > /dev/null
check "speed set and normalized" "1.5" "$(run speed)"
check "file written" "speed=1.5" "$(grep '^speed=' "$T/config/letitsnow/config")"
run set density=2 layer=above opacity=0.4 > /dev/null
check "set several" "2 top 0.4" "$(run density) $(run layer) $(run opacity)"
run layer background > /dev/null
check "layer alias" "desktop" "$(run layer)"
run layer dynamic > /dev/null
check "dynamic layer" "dynamic" "$(run layer)"
fails "speed too high" speed 9
fails "speed not a number" speed fast
fails "negative density" density -1
fails "opacity zero" opacity 0
fails "bad layer" layer sideways
fails "fps fraction" fps 59.5
fails "unknown key" set wind=3
fails "unknown command" snowball
check "rejected values leave the file alone" "1.5 2" "$(run speed) $(run density)"

printf 'speed = 3\ndensity=lots\n# comment\nlayer=top\nbogus=1\n' > "$T/config/letitsnow/config"
check "hand edits: valid kept, invalid default" "3 1 top" "$(run speed) $(run density) $(run layer)"

run reset > /dev/null
check "reset" "1 1 1 desktop 60" "$(run speed) $(run density) $(run opacity) $(run layer) $(run fps)"

out=$(run start --speed 2 --layer=top)
case $out in "letitsnow: snowing, speed 2, density 1, opacity 1, layer top") pass=$((pass + 1)) ;; *) fail=$((fail + 1)); echo "FAIL start: $out" ;; esac
check "running in status" "true direct" "$(run status --json | sed -E 's/.*"running":([a-z]+),"service":"([a-z]+)".*/\1 \2/')"
run start > /dev/null
check "start twice keeps one renderer" "1" "$(pgrep -fc "$T/fake/letitsnowd")"
run density 3 > /dev/null
check "change while running" "3" "$(run density)"
run stop > /dev/null
check "stopped" "false" "$(run status --json | sed -E 's/.*"running":([a-z]+).*/\1/')"
run toggle > /dev/null
check "toggle on" "true" "$(run status --json | sed -E 's/.*"running":([a-z]+).*/\1/')"
run toggle > /dev/null
check "toggle off" "false" "$(run status --json | sed -E 's/.*"running":([a-z]+).*/\1/')"
fails "enable without the unit" enable

# Hostile files where the CLI looks: a FIFO for the settings must not hang it,
# and a lock naming some other process must not get that process signaled.
rm -f "$T/config/letitsnow/config"; mkfifo "$T/config/letitsnow/config"
check "settings FIFO ignored" "1" "$(timeout 5 env -i PATH=/usr/bin:/bin HOME="$T/home" XDG_CONFIG_HOME="$T/config" XDG_RUNTIME_DIR="$T/run" LETITSNOW_NO_SYSTEMD=1 "$CLI" speed)"
rm -f "$T/config/letitsnow/config"
sleep 300 & victim=$!
( exec 9> "$T/run/letitsnow-planted.lock"; flock 9; echo $victim >&9; sleep 3 ) & holder=$!
sleep 0.3
run stop > /dev/null 2>&1
check "a planted lock's pid is not signaled" "alive" "$(kill -0 $victim 2>/dev/null && echo alive || echo killed)"
kill $victim $holder 2>/dev/null; wait $victim $holder 2>/dev/null

echo "cli: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
