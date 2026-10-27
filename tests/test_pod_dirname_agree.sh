#!/bin/sh
# The feed-DIRECTORY-name rule is implemented TWICE -- once in C
# (pod_dir_name() in src/pod_name.h, used by up_parse_feeds()) and once in shell
# (podfetch.sh). The script creates the directory; the UI reads it back. A
# one-byte disagreement is silent and permanent: the feed row reads
# "(not indexed)" forever and every wanted.txt the UI writes lands in a
# directory the script never opens.
#
# This test is the only thing that stops them drifting apart again. It does NOT
# re-implement either rule: it runs the REAL podfetch.sh (with stubbed
# wget/podindex) and the REAL up_parse_feeds() over the SAME podcasts.txt, then
# diffs the resulting directory names byte for byte -- including a name that is
# exactly 63 bytes, one that is exactly 64, one whose 64th byte pushes a space
# to the truncation boundary, an all-dots name, an empty name, and non-ASCII.
set -u
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
UI_DIR=$(cd "$SCRIPT_DIR/.." && pwd)
SCRIPT="${PODFETCH_SCRIPT:-}"
if [ -z "$SCRIPT" ]; then
	SCRIPT="$UI_DIR/../erosq-br/package/erosq-board/files/usr/bin/podfetch.sh"
fi
[ -f "$SCRIPT" ] || { echo "SKIP: podfetch.sh not found at $SCRIPT"; exit 0; }

CC=${CC:-cc}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
FAKEBIN="$T/fakebin"
mkdir -p "$FAKEBIN" "$T/pod"

# --- stubs: no network, no expat -------------------------------------------
cat >"$FAKEBIN/podindex" <<'EOF'
#!/bin/sh
cat >/dev/null
printf 'a.mp3\t2026-08-01\tOne\thttps://ex.invalid/a.mp3\n'
EOF
cat >"$FAKEBIN/wget" <<'EOF'
#!/bin/sh
echo "<rss/>"
exit 0
EOF
chmod +x "$FAKEBIN/podindex" "$FAKEBIN/wget"

# --- the corpus ------------------------------------------------------------
# At most UP_FEEDS_MAX (16) VALID feeds, or the C side would stop early for a
# reason that has nothing to do with the name rule.
A63=$(printf 'A%.0s' $(seq 1 63))
B64=$(printf 'B%.0s' $(seq 1 64))
C62=$(printf 'C%.0s' $(seq 1 62))
{
	printf '# a comment line\n'
	printf '\n'
	printf 'Spaced Show |https://ex.invalid/1\n'          # trailing space in name
	printf 'Trail   |https://ex.invalid/2\n'              # several trailing spaces
	printf '  Lead|https://ex.invalid/3\n'                # leading whitespace
	printf 'Tabbed\tShow|https://ex.invalid/4\n'          # tab dropped
	printf 'Sla/sh|https://ex.invalid/5\n'                # '/' dropped
	printf 'Quote"Show|https://ex.invalid/6\n'            # '"' dropped
	printf 'Unicode \303\234\303\257 Show|https://ex.invalid/7\n'
	printf '%s|https://ex.invalid/8\n' "$A63"             # exactly 63 bytes
	printf '%s|https://ex.invalid/9\n' "$B64"             # exactly 64 -> 63
	printf '%s x|https://ex.invalid/10\n' "$C62"          # 64 with a space at 63
	printf '..dots..|https://ex.invalid/11\n'             # dots, but not ALL dots
	printf '..|https://ex.invalid/12\n'                   # all dots: must be SKIPPED
	printf '.|https://ex.invalid/13\n'                    # all dots: must be SKIPPED
	printf '   |https://ex.invalid/14\n'                  # empty name: must be SKIPPED
	printf 'CR Show|https://ex.invalid/15\r\n'            # CRLF line ending
	printf 'https://bare.example.invalid/feed\n'          # bare URL -> host name
	printf 'https://bare2.example.invalid/feed   \n'      # bare URL, trailing spaces
	printf 'https://bare3.example.invalid   \n'           # no path: the trailing
	                                                      # spaces reach the NAME
} >"$T/feeds.txt"

# --- side 1: the real podfetch.sh ------------------------------------------
env PATH="$FAKEBIN:$PATH" \
    PODCAST_ROOT="$T/pod" PODCAST_FEEDS="$T/feeds.txt" PODINDEX="$FAKEBIN/podindex" \
    PODFETCH_STATUS="$T/status" PODFETCH_LOCK="$T/lock" PODFETCH_NO_WIFI=1 \
    PODFETCH_MIN_FREE_KB=1 \
    sh "$SCRIPT" refresh >"$T/script.log" 2>&1

# Include dot-prefixed directories: "..dots.." is a legitimate name under the
# shared rule, and a plain '*' glob would silently drop it from the comparison.
for d in "$T/pod"/* "$T/pod"/.*; do
	[ -d "$d" ] || continue
	b=${d##*/}
	[ "$b" = "." ] || [ "$b" = ".." ] && continue
	printf '%s\n' "$b"
done | sort >"$T/from_script.txt"

# --- side 2: the real up_parse_feeds() -------------------------------------
cat >"$T/print_names.c" <<'EOF'
/* Prints the feed-directory name erosq-ui would READ, one per line. */
#include "ui_podcast_logic.h"
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv)
{
    static char buf[65536];
    static up_feed_t feeds[UP_FEEDS_MAX];
    FILE *f = argc > 1 ? fopen(argv[1], "r") : NULL;
    size_t n;
    int nf, i;
    if (!f) { fprintf(stderr, "print_names: cannot open\n"); return 2; }
    n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    fclose(f);
    nf = up_parse_feeds(buf, feeds, UP_FEEDS_MAX);
    for (i = 0; i < nf; i++) printf("%s\n", feeds[i].name);
    return 0;
}
EOF
$CC -Wall -Wextra -O1 -I"$UI_DIR/src" -I"$UI_DIR" \
    "$T/print_names.c" "$UI_DIR/src/ui_podcast_logic.c" -o "$T/print_names" || {
	echo "FAIL: could not build the C side"; exit 1; }
"$T/print_names" "$T/feeds.txt" | sort >"$T/from_ui.txt"

# --- compare ---------------------------------------------------------------
FAIL=0
if cmp -s "$T/from_script.txt" "$T/from_ui.txt"; then
	echo "PASS (agree): $(wc -l <"$T/from_ui.txt" | tr -d ' ') directory names identical"
else
	echo "FAIL (agree): podfetch.sh and up_parse_feeds() disagree"
	echo "--- created by podfetch.sh (cat -A) ---"; cat -A "$T/from_script.txt"
	echo "--- expected by erosq-ui  (cat -A) ---"; cat -A "$T/from_ui.txt"
	FAIL=1
fi

# Spot-check the boundary cases by name, so a future change that makes BOTH
# sides wrong in the same way still gets caught.
expect_present() {
	if grep -qxF -- "$1" "$T/from_ui.txt" && grep -qxF -- "$1" "$T/from_script.txt"; then
		echo "PASS ($2)"
	else
		echo "FAIL ($2): '$1' missing from one or both sides"; FAIL=1
	fi
}
expect_absent() {
	if grep -qxF -- "$1" "$T/from_ui.txt" || grep -qxF -- "$1" "$T/from_script.txt"; then
		echo "FAIL ($2): '$1' should have been skipped"; FAIL=1
	else
		echo "PASS ($2)"
	fi
}
expect_present "Spaced Show"            "trailing space trimmed"
expect_present "$A63"                   "63 bytes kept whole"
expect_present "$(printf 'B%.0s' $(seq 1 63))" "64 bytes truncated to 63"
expect_present "$C62"                   "space at the 63-byte boundary trimmed"
expect_present "..dots.."               "dots but not all dots"
expect_present "bare.example.invalid"   "bare URL host"
expect_present "bare2.example.invalid"  "bare URL host, trailing spaces"
expect_present "bare3.example.invalid"  "bare URL, no path, trailing spaces"
expect_absent  ".."                     "all-dots name skipped"
expect_absent  "."                      "single-dot name skipped"
expect_absent  "feed14"                 "no invented fallback name"

# The traversal consequence of an all-dots name, checked directly: '..' as a
# directory name would put index.tsv in the PARENT of the podcast root.
if [ -e "$T/index.tsv" ]; then
	echo "FAIL (traversal): index.tsv escaped the podcast root"; FAIL=1
else
	echo "PASS (traversal)"
fi

echo
if [ "$FAIL" = 0 ]; then
	echo "test_pod_dirname_agree: ALL CHECKS PASSED"
else
	echo "test_pod_dirname_agree: SOME CHECKS FAILED"
fi
exit $FAIL
