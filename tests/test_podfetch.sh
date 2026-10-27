#!/bin/sh
# OPTIONAL NETWORK TEST -- exercises podfetch.sh against two REAL third-party
# podcast feeds. SKIPS (exit 0) when either the network or a working podindex
# is unavailable, which is the case in this project's own dev environment
# (no expat headers here) -- see tests/test_podfetch_hermetic.sh for the
# dependency-free test that actually covers podfetch's own logic (lock
# exclusion, wanted.txt handling, refusal of unsafe names, etc).
#
# Bounds, not exact counts: both shows keep publishing, so ">=" is the only
# stable oracle -- an exact "==508" would start failing the day either show
# drops a new episode. Precedent: tests/test_bt_lock.sh.
#
# Requires: network access and a podindex binary. Pass PODINDEX=/path or let it
# build one if host expat is available; skips (exit 0) if neither is possible.
set -e
cd "$(dirname "$0")/.."
SCRIPT=../erosq-br/package/erosq-board/files/usr/bin/podfetch.sh
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT

PI=${PODINDEX:-}
if [ -z "$PI" ]; then
	if cc -Wall -O2 -Isrc -I. src/podindex.c src/podindex_logic.c -lexpat -o "$T/podindex" 2>/dev/null; then
		PI="$T/podindex"
	else
		echo "SKIP: no expat on this host and no PODINDEX= given"; exit 0
	fi
fi

cat >"$T/feeds.txt" <<'EOF'
# comment ignored
Nothing Much Happens|https://feeds.megaphone.fm/SSM2868305742
Dungeons and Daddies|https://feeds.acast.com/public/shows/46e42ca7-4921-5f04-a297-1970e72fd86d
EOF

env PODCAST_ROOT="$T/pod" PODCAST_FEEDS="$T/feeds.txt" PODINDEX="$PI" \
    PODFETCH_STATUS="$T/status" PODFETCH_LOCK="$T/lock" PODFETCH_NO_WIFI=1 \
    sh "$SCRIPT" refresh

NMH=$(wc -l <"$T/pod/Nothing Much Happens/index.tsv" | tr -d ' ')
DAD=$(wc -l <"$T/pod/Dungeons and Daddies/index.tsv" | tr -d ' ')
echo "indexed: nmh=$NMH dad=$DAD"
# Lower bounds only -- both shows keep publishing, an exact count is not a
# stable oracle. The field-count and dedup checks below are the real guard
# against a silent parser regression.
[ "$NMH" -ge 500 ] || { echo "FAIL: nmh expected >=500 rows, got $NMH"; exit 1; }
[ "$DAD" -ge 200 ] || { echo "FAIL: dad expected >=200 rows, got $DAD"; exit 1; }

# Every row must have 4 fields -- proves no title desynchronised the TSV.
awk -F'\t' 'NF!=4 {print "FAIL: "FILENAME" line "NR" has "NF" fields"; bad=1}
            END {exit bad+0}' "$T/pod"/*/index.tsv

# Entity decoding and CDATA: expat's job, asserted on real data.
grep -q '&amp;' "$T/pod/Nothing Much Happens/index.tsv" && \
	{ echo "FAIL: undecoded entity in title column"; exit 1; }
grep -q 'CDATA' "$T/pod/Dungeons and Daddies/index.tsv" && \
	{ echo "FAIL: raw CDATA leaked into a title"; exit 1; }

# Filenames must be unique despite Acast reusing media.mp3 for every episode.
DUP=$(cut -f1 "$T/pod/Dungeons and Daddies/index.tsv" | sort | uniq -d | wc -l | tr -d ' ')
[ "$DUP" -eq 0 ] || { echo "FAIL: $DUP duplicate filenames"; exit 1; }

# A failed refresh must keep the previous index.
cp "$T/pod/Nothing Much Happens/index.tsv" "$T/good.tsv"
sed -i 's|megaphone.fm/SSM2868305742|megaphone.fm/DOES-NOT-EXIST|' "$T/feeds.txt"
env PODCAST_ROOT="$T/pod" PODCAST_FEEDS="$T/feeds.txt" PODINDEX="$PI" \
    PODFETCH_STATUS="$T/status" PODFETCH_LOCK="$T/lock" PODFETCH_NO_WIFI=1 \
    sh "$SCRIPT" refresh || true
cmp -s "$T/good.tsv" "$T/pod/Nothing Much Happens/index.tsv" || \
	{ echo "FAIL: failed refresh clobbered the good index"; exit 1; }

echo "podfetch smoke OK"
