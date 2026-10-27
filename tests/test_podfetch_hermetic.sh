#!/bin/sh
# Hermetic, dependency-free test for podfetch.sh's OWN logic: no network, no
# expat, no real podindex -- both are replaced by tiny stubs on $PATH / via
# $PODINDEX, so this runs anywhere `sh` runs (matches the project rule that
# pure script/logic gets a dep-free host test; hardware/network glue stays
# HITL -- see tests/test_bt_lock.sh for the same style applied to the mkdir
# mutex). Complements, and does NOT replace, tests/test_podfetch.sh, which is
# the network test against the real feeds and real podindex.
#
# Covers, at minimum:
#   (a) refresh writes the expected index from a stubbed podindex
#   (b) a failing podindex leaves the previous good index byte-identical,
#       AND yields a non-zero script exit, AND an `error` status line
#   (c) lock exclusion leaves a running instance's status file untouched
#   (d) wanted.txt entry removal on success, INCLUDING the last-line-with-
#       no-trailing-newline case (the `read` loop must still process it)
#   (e) a traversal name and a quote-containing name are both refused, with
#       no files created outside the feed directory
#   (f) a wanted.txt entry absent from index.tsv sets non-zero exit and the
#       loop continues to the next entry
#   (g) wget "succeeding" (rc 0) without ever creating the target file must
#       not be treated as a successful download: the mv must be checked,
#       the wanted.txt entry must survive for a retry, and rc must be non-0
#   (h) a lock whose recorded pid is GONE is stale and gets taken over, so one
#       killed run cannot wedge every later Refresh/Download for the boot
#   (i) a run killed mid-download must not leave the status file claiming
#       progress: cleanup writes `error ... interrupted` and frees the lock
#   (j) the episode COUNT in the status line is computed the way the read loop
#       iterates (a final line with no newline included), never "3/2"
#   (k) an orphaned .part from an interrupted run is removed at the start of
#       the next download run
#   (l) refresh honours the df floor BEFORE writing, leaving the old index
#       byte-identical (the full-card route that used to destroy it)
#   (m) podindex's skipped/dropped counts are surfaced on the SUCCESS path --
#       on stdout and in the final status line -- and no index.err is left
#       behind on the card
#   (n) /run/wifi-want is set to 1 before wifi_up.sh and 0 before wifi_down.sh,
#       so neither script's last-intent guard silently no-ops us
#   (o) a feed name that sanitises to all dots is skipped, and nothing is
#       written into the podcast ROOT itself
set -u
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
SCRIPT="${PODFETCH_SCRIPT:-}"
if [ -z "$SCRIPT" ]; then
	# PODFETCH_SCRIPT overrides the location so this suite can be pointed at a
	# PREVIOUS revision of the script -- that is how each new check below was
	# shown to fail before its fix rather than merely passing after it.
	SCRIPT="$SCRIPT_DIR/../../erosq-br/package/erosq-board/files/usr/bin/podfetch.sh"
	[ -f "$SCRIPT" ] || SCRIPT="$SCRIPT_DIR/../erosq-br/package/erosq-board/files/usr/bin/podfetch.sh"
fi

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
FAKEBIN="$T/fakebin"
mkdir -p "$FAKEBIN"
FAIL=0

# --- stub podindex: cats a canned fixture, or fails on demand --------------
cat >"$FAKEBIN/podindex" <<'EOF'
#!/bin/sh
cat >/dev/null
if [ "${FAKE_PODINDEX_FAIL:-0}" = 1 ]; then
	echo "fake podindex: forced failure" >&2
	exit 1
fi
# The real podindex reports refused enclosures on stderr while still exiting 0.
[ -n "${FAKE_PODINDEX_STDERR:-}" ] && echo "$FAKE_PODINDEX_STDERR" >&2
cat "$FAKE_PODINDEX_FIXTURE"
EOF
chmod +x "$FAKEBIN/podindex"

# --- stub wget: recognizes only the two forms podfetch.sh actually uses ----
# `wget -q -T N -O - URL`      (refresh, stdout)
# `wget -q -T N -O FILE URL`   (download, to a file)
# FAKE_WGET_MODE=fail        -> exit 1, nothing written (download failure)
# FAKE_WGET_MODE=ok_no_file  -> exit 0 but never creates FILE (Finding 2 repro:
#                               a "successful" wget whose target never lands)
# FAKE_WGET_MODE=term      -> SIGTERM our parent (the podfetch shell) and fail,
#                             standing in for an OOM kill / killall mid-run
# FAKE_WGET_MODE=ok (default) -> normal success
# FAKE_WGET_LOG_STATUS=<file> -> append the status line as it stood when this
#                             download started, so the test can check the
#                             episode counter the script published
cat >"$FAKEBIN/wget" <<'EOF'
#!/bin/sh
mode="${FAKE_WGET_MODE:-ok}"
out=""
prev=""
for a in "$@"; do
	[ "$prev" = "-O" ] && out="$a"
	prev="$a"
done
if [ -n "${FAKE_WGET_LOG_STATUS:-}" ] && [ "$out" != "-" ]; then
	cat "$PODFETCH_STATUS" >>"$FAKE_WGET_LOG_STATUS" 2>/dev/null
fi
case "$mode" in
	fail) exit 1 ;;
	ok_no_file) exit 0 ;;
	term) kill -TERM "$PPID" 2>/dev/null; exit 1 ;;
esac
if [ "$out" = "-" ]; then
	echo "<rss>fake feed body, ignored by the stubbed podindex</rss>"
else
	echo "FAKE-AUDIO-CONTENT" >"$out"
fi
exit 0
EOF
chmod +x "$FAKEBIN/wget"

# --- stubs for the WiFi lifecycle seam (case n) -----------------------------
# Each records the value of /run/wifi-want AS IT STOOD when it was invoked --
# that is exactly what the real scripts' last-intent guard reads.
cat >"$FAKEBIN/wifi_up_stub.sh" <<'EOF'
#!/bin/sh
cat "$PODFETCH_WIFI_WANT" >"$FAKE_WANT_AT_UP" 2>/dev/null || : >"$FAKE_WANT_AT_UP"
exit 0
EOF
cat >"$FAKEBIN/wifi_down_stub.sh" <<'EOF'
#!/bin/sh
cat "$PODFETCH_WIFI_WANT" >"$FAKE_WANT_AT_DOWN" 2>/dev/null || : >"$FAKE_WANT_AT_DOWN"
exit 0
EOF
# wifi_is_up() probes these two; make "down, then addressed" deterministic.
cat >"$FAKEBIN/wpa_cli" <<'EOF'
#!/bin/sh
exit 1
EOF
cat >"$FAKEBIN/ip" <<'EOF'
#!/bin/sh
echo "    inet 192.0.2.5/24 scope global wlan0"
exit 0
EOF
chmod +x "$FAKEBIN/wifi_up_stub.sh" "$FAKEBIN/wifi_down_stub.sh" \
         "$FAKEBIN/wpa_cli" "$FAKEBIN/ip"

# Never a real network call: even the URL host is fake (fully replaced wget
# never resolves it), but keep it well-formed since podfetch validates the
# scheme before ever invoking wget.
cat >"$T/feeds.txt" <<'EOF'
Test Show|https://example.invalid/feed.xml
EOF

cat >"$T/fixture.tsv" <<'EOF'
ep1.mp3	2026-08-01	Episode One	https://example.invalid/ep1.mp3
ep2.mp3	2026-08-02	Episode Two	https://example.invalid/ep2.mp3
ep3.mp3	2026-08-03	Episode Three	https://example.invalid/ep3.mp3
ep4.mp3	2026-08-04	Episode Four	https://example.invalid/ep4.mp3
EOF

DIR="$T/pod/Test Show"

run() {  # run <refresh|download>   (honors FAKE_PODINDEX_FAIL / FAKE_WGET_MODE from env)
	env PATH="$FAKEBIN:$PATH" \
	    PODCAST_ROOT="$T/pod" PODCAST_FEEDS="$T/feeds.txt" PODINDEX="$FAKEBIN/podindex" \
	    FAKE_PODINDEX_FIXTURE="$T/fixture.tsv" \
	    FAKE_PODINDEX_FAIL="${FAKE_PODINDEX_FAIL:-0}" \
	    FAKE_PODINDEX_STDERR="${FAKE_PODINDEX_STDERR:-}" \
	    FAKE_WGET_MODE="${FAKE_WGET_MODE:-ok}" \
	    FAKE_WGET_LOG_STATUS="${FAKE_WGET_LOG_STATUS:-}" \
	    PODFETCH_MIN_FREE_KB="${PODFETCH_MIN_FREE_KB:-1}" \
	    PODFETCH_REFRESH_MIN_FREE_KB="${PODFETCH_REFRESH_MIN_FREE_KB:-1}" \
	    PODFETCH_STATUS="$T/status" PODFETCH_LOCK="$T/lock" PODFETCH_NO_WIFI=1 \
	    sh "$SCRIPT" "$@"
}

# Same, but with the WiFi lifecycle ENABLED and pointed at the stubs above.
run_wifi() {
	env PATH="$FAKEBIN:$PATH" \
	    PODCAST_ROOT="$T/pod" PODCAST_FEEDS="$T/feeds.txt" PODINDEX="$FAKEBIN/podindex" \
	    FAKE_PODINDEX_FIXTURE="$T/fixture.tsv" FAKE_PODINDEX_FAIL=0 \
	    FAKE_PODINDEX_STDERR="" FAKE_WGET_MODE=ok FAKE_WGET_LOG_STATUS="" \
	    PODFETCH_MIN_FREE_KB=1 PODFETCH_REFRESH_MIN_FREE_KB=1 \
	    PODFETCH_STATUS="$T/status" PODFETCH_LOCK="$T/lock" PODFETCH_NO_WIFI=0 \
	    PODFETCH_WIFI_UP="$FAKEBIN/wifi_up_stub.sh" \
	    PODFETCH_WIFI_DOWN="$FAKEBIN/wifi_down_stub.sh" \
	    PODFETCH_WIFI_WANT="$T/wifi-want" \
	    FAKE_WANT_AT_UP="$T/want-at-up" FAKE_WANT_AT_DOWN="$T/want-at-down" \
	    sh "$SCRIPT" "$@"
}

status_state() { cut -d' ' -f1 "$T/status" 2>/dev/null; }

ok()   { echo "PASS ($1)"; }
bad()  { echo "FAIL ($1): $2"; FAIL=1; }

# --- (a) refresh writes the expected index ----------------------------------
run refresh; RC=$?
N=$(wc -l <"$DIR/index.tsv" 2>/dev/null | tr -d ' ')
if [ "$RC" = 0 ] && [ "$N" = 4 ]; then ok a; else bad a "rc=$RC rows=$N"; fi

# --- (b) failing podindex: previous index untouched, rc!=0, status=error ---
cp "$DIR/index.tsv" "$T/good.tsv"
FAKE_PODINDEX_FAIL=1 run refresh; RC=$?
if [ "$RC" != 0 ] && cmp -s "$T/good.tsv" "$DIR/index.tsv" && [ "$(status_state)" = error ]; then
	ok b
else
	bad b "rc=$RC status=$(cat "$T/status" 2>/dev/null) cmp=$(cmp -s "$T/good.tsv" "$DIR/index.tsv"; echo $?)"
fi

# --- (c) lock exclusion: a live instance's status file is left untouched ---
# The lock must name a LIVE owner, or it is (correctly) treated as stale and
# taken over -- see (h). $$ is this test process, which is certainly alive.
echo "sentinel: a run is actively updating this" >"$T/status"
mkdir "$T/lock"; echo $$ >"$T/lock/pid"
run refresh 2>"$T/c.err"; RC=$?
rm -rf "$T/lock"
if [ "$RC" != 0 ] && grep -q "already running" "$T/c.err" \
   && [ "$(cat "$T/status")" = "sentinel: a run is actively updating this" ]; then
	ok c
else
	bad c "rc=$RC status=$(cat "$T/status") stderr=$(cat "$T/c.err")"
fi

# --- (d) wanted.txt removal on success, INCLUDING no trailing newline ------
# ep2.mp3's line has NO trailing newline -- this is exactly Finding 1's repro:
# a bare `while read -r x; do ... done` drops the last unterminated line.
printf 'ep1.mp3\nep2.mp3' >"$DIR/wanted.txt"
run download; RC=$?
if [ "$RC" = 0 ] && [ -f "$DIR/ep1.mp3" ] && [ -f "$DIR/ep2.mp3" ] && [ ! -s "$DIR/wanted.txt" ]; then
	ok d
else
	bad d "rc=$RC ep1=$([ -f "$DIR/ep1.mp3" ] && echo y || echo n) ep2=$([ -f "$DIR/ep2.mp3" ] && echo y || echo n) wanted='$(cat "$DIR/wanted.txt" 2>/dev/null)'"
fi

# --- (e) traversal + quote-containing names refused, no stray files --------
printf '../etc/passwd\nbad"name.mp3\n' >"$DIR/wanted.txt"
run download >"$T/e.log" 2>&1; RC=$?
if [ -e "$T/pod/etc/passwd" ] || find "$T" -iname '*passwd*' 2>/dev/null | grep -q passwd; then
	bad e "traversal file was created"
elif find "$T/pod" -name '*name.mp3' 2>/dev/null | grep -q .; then
	bad e "quote-named file was created"
elif grep -q "refusing name '../etc/passwd'" "$T/e.log" && grep -q "refusing name" "$T/e.log"; then
	ok e
else
	bad e "refusal messages missing: $(cat "$T/e.log")"
fi

# --- (f) entry absent from index.tsv: rc!=0, loop continues ----------------
printf 'not-in-index.mp3\nep3.mp3\n' >"$DIR/wanted.txt"
run download >"$T/f.log" 2>&1; RC=$?
if [ "$RC" != 0 ] && grep -q "'not-in-index.mp3' not in index" "$T/f.log" && [ -f "$DIR/ep3.mp3" ]; then
	ok f
else
	bad f "rc=$RC ep3=$([ -f "$DIR/ep3.mp3" ] && echo y || echo n) log=$(cat "$T/f.log")"
fi

# --- (g) wget rc=0 but target never created: mv must be checked ------------
# Finding 2 repro: a wget that reports success without producing its output
# file must not be logged as "got $want" / have its wanted.txt line dropped.
printf 'ep4.mp3\n' >"$DIR/wanted.txt"
FAKE_WGET_MODE=ok_no_file run download >"$T/g.log" 2>&1; RC=$?
if [ "$RC" != 0 ] && [ ! -f "$DIR/ep4.mp3" ] && grep -qx 'ep4.mp3' "$DIR/wanted.txt" \
   && ! grep -q "got ep4.mp3" "$T/g.log"; then
	ok g
else
	bad g "rc=$RC ep4_exists=$([ -f "$DIR/ep4.mp3" ] && echo y || echo n) wanted='$(cat "$DIR/wanted.txt")' log=$(cat "$T/g.log")"
fi


# --- (h) a lock whose owner is gone is stale and gets taken over ------------
# Without this, one OOM-killed run wedges every later Refresh and Download for
# the rest of the boot ($LOCK is tmpfs) with no message anywhere the user looks.
( exit 0 ) & DEADPID=$!
wait "$DEADPID" 2>/dev/null
mkdir "$T/lock"; echo "$DEADPID" >"$T/lock/pid"
run refresh >"$T/h.log" 2>&1; RC=$?
if [ "$RC" = 0 ] && [ ! -d "$T/lock" ] && grep -q "taking over stale lock" "$T/h.log"; then
	ok h
else
	bad h "rc=$RC lock_left=$([ -d "$T/lock" ] && echo y || echo n) log=$(cat "$T/h.log")"
fi

rm -rf "$T/lock"   # isolate: one failing case must not cascade
# --- (i) killed mid-download: status must stop claiming progress ------------
printf 'ep1.mp3\n' >"$DIR/wanted.txt"
rm -f "$DIR/ep1.mp3"
FAKE_WGET_MODE=term run download >"$T/i.log" 2>&1; RC=$?
if [ "$RC" != 0 ] && [ "$(status_state)" = error ] \
   && grep -q interrupted "$T/status" && [ ! -d "$T/lock" ]; then
	ok i
else
	bad i "rc=$RC status='$(cat "$T/status")' lock_left=$([ -d "$T/lock" ] && echo y || echo n)"
fi

rm -rf "$T/lock"   # isolate: one failing case must not cascade
# --- (j) the status episode counter matches how the loop iterates -----------
# ep2.mp3's line has no trailing newline, so `wc -l` says 1 while the loop
# processes 2 -- the status row used to read "2/1".
rm -f "$DIR/ep1.mp3" "$DIR/ep2.mp3" "$T/statlog"
printf 'ep1.mp3\nep2.mp3' >"$DIR/wanted.txt"
FAKE_WGET_LOG_STATUS="$T/statlog" run download >"$T/j.log" 2>&1; RC=$?
if [ "$RC" = 0 ] && grep -q '^download 1/1 1/2 ' "$T/statlog" \
   && grep -q '^download 1/1 2/2 ' "$T/statlog"; then
	ok j
else
	bad j "rc=$RC statlog='$(cat "$T/statlog" 2>/dev/null)'"
fi

rm -rf "$T/lock"   # isolate: one failing case must not cascade
# --- (k) orphaned .part files are cleaned at the start of a download run ----
# An un-ticked episode's partial would otherwise linger forever: up to 70 MB,
# hidden by the leading dot, invisible everywhere in the UI.
: >"$DIR/.orphan.mp3.part"
printf 'ep3.mp3\n' >"$DIR/wanted.txt"
rm -f "$DIR/ep3.mp3"
run download >"$T/k.log" 2>&1; RC=$?
if [ "$RC" = 0 ] && [ ! -e "$DIR/.orphan.mp3.part" ] && [ -f "$DIR/ep3.mp3" ]; then
	ok k
else
	bad k "rc=$RC part_left=$([ -e "$DIR/.orphan.mp3.part" ] && echo y || echo n) log=$(cat "$T/k.log")"
fi

rm -rf "$T/lock"   # isolate: one failing case must not cascade
# --- (l) refresh honours its OWN df floor BEFORE it writes -----------------
# A full card was the expected route into the index-destroying bug, and refresh
# had no floor at all. An impossible floor (on refresh's own, much smaller,
# floor -- see REFRESH_MIN_FREE_KB in podfetch.sh) stands in for a full card.
cp "$DIR/index.tsv" "$T/good2.tsv"
PODFETCH_REFRESH_MIN_FREE_KB=999999999999 run refresh >"$T/l.log" 2>&1; RC=$?
if [ "$RC" != 0 ] && cmp -s "$T/good2.tsv" "$DIR/index.tsv" \
   && grep -q "card full" "$T/status" && [ ! -e "$DIR/index.tsv.new" ]; then
	ok l
else
	bad l "rc=$RC status='$(cat "$T/status")' cmp=$(cmp -s "$T/good2.tsv" "$DIR/index.tsv"; echo $?)"
fi

rm -rf "$T/lock"   # isolate: one failing case must not cascade
# --- (m) skipped-episode counts surface on the SUCCESS path ----------------
# They used to go to <dir>/index.err, which was cat'd only on failure, so a
# successful refresh dropped episodes in complete silence.
rm -f "$DIR/index.err"
FAKE_PODINDEX_STDERR="podindex: 2 items skipped (no title, or unusable enclosure URL)" \
	run refresh >"$T/m.log" 2>&1; RC=$?
if [ "$RC" = 0 ] && grep -q "2 items skipped" "$T/m.log" \
   && grep -q '2 skipped' "$T/status" && [ ! -e "$DIR/index.err" ]; then
	ok m
else
	bad m "rc=$RC status='$(cat "$T/status")' err_left=$([ -e "$DIR/index.err" ] && echo y || echo n) log=$(cat "$T/m.log")"
fi

rm -rf "$T/lock"   # isolate: one failing case must not cascade
# --- (n) /run/wifi-want records OUR intent around wifi_up/wifi_down --------
# wifi_up.sh exits 0 when wifi-want is "0" and wifi_down.sh exits 0 when it is
# "1": without writing it, a stale Settings toggle either stopped WiFi coming up
# or left the chip powered after the fetch.
rm -f "$T/want-at-up" "$T/want-at-down"
echo 0 >"$T/wifi-want"            # user turned WiFi off earlier this boot
run_wifi refresh >"$T/n.log" 2>&1; RC=$?
if [ "$RC" = 0 ] && [ "$(cat "$T/want-at-up" 2>/dev/null)" = 1 ] \
   && [ "$(cat "$T/want-at-down" 2>/dev/null)" = 0 ] \
   && [ "$(cat "$T/wifi-want")" = 0 ]; then
	ok n
else
	bad n "rc=$RC at_up='$(cat "$T/want-at-up" 2>/dev/null)' at_down='$(cat "$T/want-at-down" 2>/dev/null)' log=$(cat "$T/n.log")"
fi

rm -rf "$T/lock"   # isolate: one failing case must not cascade
# --- (o) an all-dots feed name is skipped, nothing lands in the ROOT -------
# '.' and '-' are whitelisted, so ".." survived the sanitiser on both sides and
# a hand-written "..|https://..." line wrote index.tsv straight into /media/sd.
cat >"$T/feeds2.txt" <<'EOF'
..|https://example.invalid/feed.xml
EOF
rm -f "$T/pod/index.tsv"
env PATH="$FAKEBIN:$PATH" PODCAST_ROOT="$T/pod" PODCAST_FEEDS="$T/feeds2.txt" \
    PODINDEX="$FAKEBIN/podindex" FAKE_PODINDEX_FIXTURE="$T/fixture.tsv" \
    FAKE_PODINDEX_FAIL=0 FAKE_PODINDEX_STDERR="" FAKE_WGET_MODE=ok \
    FAKE_WGET_LOG_STATUS="" PODFETCH_MIN_FREE_KB=1 PODFETCH_REFRESH_MIN_FREE_KB=1 \
    PODFETCH_STATUS="$T/status" PODFETCH_LOCK="$T/lock" PODFETCH_NO_WIFI=1 \
    sh "$SCRIPT" refresh >"$T/o.log" 2>&1
if [ ! -e "$T/pod/index.tsv" ] && [ ! -e "$T/index.tsv" ] \
   && grep -q "unusable name" "$T/o.log"; then
	ok o
else
	bad o "index_in_root=$([ -e "$T/pod/index.tsv" ] && echo y || echo n) log=$(cat "$T/o.log")"
fi

echo
if [ "$FAIL" = 0 ]; then
	echo "test_podfetch_hermetic: ALL CHECKS PASSED"
else
	echo "test_podfetch_hermetic: SOME CHECKS FAILED"
fi
exit $FAIL
