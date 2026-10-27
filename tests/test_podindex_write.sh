#!/bin/sh
# podindex must exit NON-ZERO when its output could not be written.
#
# This is the whole-branch review's Critical finding. Every row goes through
# puts() into a buffered stdout, so on a full SD card the writes fail inside
# stdio and main() used to return 0 anyway -- podfetch.sh then ran
# `mv index.tsv.new index.tsv` and replaced a good 127 KB index with an empty
# file, for every feed, until space was freed. The spec guarantees the opposite:
# a failed refresh leaves the previous index intact.
#
# /dev/full is the exact reproduction: writes fail with ENOSPC, everything else
# behaves normally. Needs real expat, so this test SKIPS (exit 0) rather than
# failing where no expat is available -- the pure parsing rules are covered
# dependency-free by tests/test_podindex.c.
set -u
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
UI_DIR=$(cd "$SCRIPT_DIR/.." && pwd)
CC=${CC:-cc}

# expat, in preference order: pkg-config, then the Buildroot HOST tree (which
# builds host-expat for its own tooling), then the system default include path.
EXPAT_CFLAGS=""
EXPAT_LIBS="-lexpat"
EXPAT_RPATH=""
BR_HOST=${BR_HOST:-../buildroot/output/host}
if pkg-config --exists expat 2>/dev/null; then
	EXPAT_CFLAGS=$(pkg-config --cflags expat)
	EXPAT_LIBS=$(pkg-config --libs expat)
elif [ -f "$BR_HOST/include/expat.h" ]; then
	EXPAT_CFLAGS="-I$BR_HOST/include"
	EXPAT_LIBS="-L$BR_HOST/lib -lexpat"
	EXPAT_RPATH="-Wl,-rpath,$BR_HOST/lib"
elif [ ! -f /usr/include/expat.h ]; then
	echo "SKIP: no expat headers (tried pkg-config and $BR_HOST/include)"
	exit 0
fi

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
FAIL=0

# shellcheck disable=SC2086
$CC -Wall -Wextra -O1 -I"$UI_DIR/src" -I"$UI_DIR" $EXPAT_CFLAGS \
    "$UI_DIR/src/podindex.c" "$UI_DIR/src/podindex_logic.c" \
    -o "$T/podindex" $EXPAT_LIBS $EXPAT_RPATH || {
	echo "SKIP: could not build podindex against expat"; exit 0; }

cat >"$T/feed.xml" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<rss version="2.0"><channel>
<title>Channel Title Must Not Become A Row</title>
<item><title>Intro &amp; Chapter 1</title><pubDate>Tue, 11 Aug 2026 16:00:00 GMT</pubDate>
<enclosure url="https://cdn.example.invalid/abc123/media.mp3?tok=1" type="audio/mpeg" length="1"/></item>
<item><title><![CDATA[Second one]]></title><pubDate>11 Aug 2026 16:00:00 GMT</pubDate>
<enclosure url="https://cdn.example.invalid/def456/media.mp3" type="audio/mpeg" length="1"/></item>
</channel></rss>
EOF

# --- (a) the normal path still succeeds ------------------------------------
if "$T/podindex" <"$T/feed.xml" >"$T/out.tsv" 2>"$T/out.err" \
   && [ "$(wc -l <"$T/out.tsv" | tr -d ' ')" = 2 ]; then
	echo "PASS (a): 2 rows, exit 0"
else
	echo "FAIL (a): rc=$? rows=$(wc -l <"$T/out.tsv" | tr -d ' ') err=$(cat "$T/out.err")"
	FAIL=1
fi

# --- (b) a failed write must be reported ----------------------------------
if [ ! -w /dev/full ]; then
	echo "SKIP (b): no writable /dev/full on this host"
else
	"$T/podindex" <"$T/feed.xml" >/dev/full 2>"$T/full.err"; RC=$?
	if [ "$RC" != 0 ] && grep -q "write failed" "$T/full.err"; then
		echo "PASS (b): exit $RC on a full device"
	else
		echo "FAIL (b): exit $RC (expected non-zero), err=$(cat "$T/full.err")"
		FAIL=1
	fi
fi

# --- (c) an empty feed is still an error, as before ------------------------
printf '<rss version="2.0"><channel></channel></rss>' >"$T/empty.xml"
"$T/podindex" <"$T/empty.xml" >"$T/empty.tsv" 2>/dev/null; RC=$?
if [ "$RC" != 0 ]; then
	echo "PASS (c): empty feed exits $RC"
else
	echo "FAIL (c): empty feed exited 0"
	FAIL=1
fi

echo
if [ "$FAIL" = 0 ]; then
	echo "test_podindex_write: ALL CHECKS PASSED"
else
	echo "test_podindex_write: SOME CHECKS FAILED"
fi
exit $FAIL
