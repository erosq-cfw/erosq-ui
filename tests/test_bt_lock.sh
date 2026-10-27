#!/bin/sh
# Self-check for the mkdir mutex used by bt_up.sh / bt_audio_up.sh / bt_down.sh
# to serialize BT power transitions (fast UI off->on race). Asserts (1) two
# concurrent critical sections never interleave, and (2) a waiter that times
# out FAILS without entering the critical section and without removing the
# holder's lock (the old pattern barged in and its trap rmdir'd a lock it never
# created, unlocking a third caller). No framework; run directly.
set -u
TMP=$(mktemp -d)
LOCK="$TMP/bt-power.lock"
LOG="$TMP/log"
: > "$LOG"

# Same acquire/release as the shipped scripts (short sleep + small cap here to
# keep the test fast; the scripts use `sleep 1` and a cap of 30).
worker() {
	LOCKED=0
	trap '[ $LOCKED -eq 1 ] && rmdir "$LOCK" 2>/dev/null' EXIT
	trap 'exit 1' INT TERM
	n=0
	while ! mkdir "$LOCK" 2>/dev/null; do
		sleep 0.05; n=$((n+1))
		[ $n -ge ${2:-200} ] && { echo "timeout $1" >> "$LOG"; exit 1; }
	done
	LOCKED=1
	echo "enter $1" >> "$LOG"
	sleep 0.2                      # hold the critical section
	echo "exit $1"  >> "$LOG"
}

# --- case 1: three contenders serialize, no overlap -------------------------
( worker A ) & ( worker B ) & ( worker C ) &
wait

# Correct serialization => every "enter X" is immediately followed by "exit X".
# A race would produce "enter X\nenter Y" (two enters back to back).
prev=""
while read -r ev who; do
	if [ "$ev" = enter ]; then
		[ -z "$prev" ] || { echo "FAIL: $prev then enter $who (overlap)"; cat "$LOG"; rm -rf "$TMP"; exit 1; }
		prev="$who"
	else
		[ "$prev" = "$who" ] || { echo "FAIL: exit $who without matching enter"; cat "$LOG"; rm -rf "$TMP"; exit 1; }
		prev=""
	fi
done < "$LOG"

lines=$(wc -l < "$LOG")
[ "$lines" -eq 6 ] || { echo "FAIL: expected 6 events, got $lines"; cat "$LOG"; rm -rf "$TMP"; exit 1; }

# --- case 2: wait-timeout must fail closed, not steal the holder's lock -----
: > "$LOG"
mkdir "$LOCK"                      # a (wedged) holder owns the lock
( worker T 3 )                     # tiny cap -> times out fast
rc=$?
[ "$rc" -ne 0 ] || { echo "FAIL: timed-out waiter exited 0"; rm -rf "$TMP"; exit 1; }
grep -q "^timeout T$" "$LOG" || { echo "FAIL: waiter entered the critical section on timeout"; cat "$LOG"; rm -rf "$TMP"; exit 1; }
[ -d "$LOCK" ] || { echo "FAIL: timed-out waiter removed the holder's lock"; rm -rf "$TMP"; exit 1; }

rm -rf "$TMP"
echo "PASS: 3 workers serialized, no overlap; timeout fails closed without lock steal"
