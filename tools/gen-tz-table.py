#!/usr/bin/env python3
"""Generate src/ui_clock_tz.h -- the timezone table for the status-bar clock.

Run on the build host (needs tzdata installed), commit the result. The device
build never sees tzdata: a TZif v2+ file's footer IS the POSIX TZ string for
future dates, and musl parses that string directly, DST rules included.

    ./tools/gen-tz-table.py            # write src/ui_clock_tz.h
    ./tools/gen-tz-table.py --check    # exit 1 if the committed file is stale

Why generated rather than typed: the rules include a 30-minute DST shift
(Lord Howe) and a 2:45am transition (Chatham), and printed tables go stale --
Casio's own is stamped "January 2022" and still has Tehran on summer time,
which Iran abolished that year. Nobody should hand-maintain this.
"""
import argparse
import os
import sys

# Casio GA-B2100 (module 5691) City Table, verbatim order from the operation
# guide qw5691_EN.pdf p25: 38 cities / 38 time zones plus UTC, one city per
# zone, ascending from -12 to +14. Plus Santiago -- see NOTES below.
#
# The `casio_utc` column is a CROSS-CHECK ONLY and is never emitted: the offset
# the device uses comes from the tzdata rule. It exists so a stale Casio table
# or a bad IANA mapping fails the build instead of shipping a wrong clock.
#
# NOTES
#   SCL  Not on the GA-B2100. Deliberate addition: Casio lists one city per UTC
#        offset, which assumes cities sharing an offset share a DST rule.
#        Santiago and Halifax are both -4 standard but their DST runs opposite
#        halves of the year, so Halifax is an hour wrong for Chile for about
#        half of it. Code and name are still Casio's (their longer World Time
#        tables carry SCL).
#   BAR  Baker Island is uninhabited and has no IANA zone; Etc/GMT+12 is the
#        fixed -12 offset it represents. (Etc/GMT+N is deliberately
#        sign-inverted in tzdata: GMT+12 means UTC-12.)
#
# fmt: off
CITIES = [
    # code   display name           IANA zone                casio_utc
    ("BAR", "Baker Island",        "Etc/GMT+12",             -12),
    ("PPG", "Pago Pago",           "Pacific/Pago_Pago",      -11),
    ("HNL", "Honolulu",            "Pacific/Honolulu",       -10),
    ("NHV", "Marquesas Islands",   "Pacific/Marquesas",     -9.5),
    ("ANC", "Anchorage",           "America/Anchorage",       -9),
    ("LAX", "Los Angeles",         "America/Los_Angeles",     -8),
    ("DEN", "Denver",              "America/Denver",          -7),
    ("CHI", "Chicago",             "America/Chicago",         -6),
    ("NYC", "New York",            "America/New_York",        -5),
    ("YHZ", "Halifax",             "America/Halifax",         -4),
    ("SCL", "Santiago",            "America/Santiago",        -4),
    ("YYT", "St. John's",          "America/St_Johns",      -3.5),
    ("RIO", "Rio de Janeiro",      "America/Sao_Paulo",       -3),
    ("FEN", "Fernando de Noronha", "America/Noronha",         -2),
    ("RAI", "Praia",               "Atlantic/Cape_Verde",     -1),
    ("UTC", "UTC",                 "UTC",                      0),
    ("LON", "London",              "Europe/London",            0),
    ("PAR", "Paris",               "Europe/Paris",             1),
    ("ATH", "Athens",              "Europe/Athens",            2),
    ("JED", "Jeddah",              "Asia/Riyadh",              3),
    ("THR", "Tehran",              "Asia/Tehran",            3.5),
    ("DXB", "Dubai",               "Asia/Dubai",               4),
    ("KBL", "Kabul",               "Asia/Kabul",             4.5),
    ("KHI", "Karachi",             "Asia/Karachi",             5),
    ("DEL", "Delhi",               "Asia/Kolkata",           5.5),
    ("KTM", "Kathmandu",           "Asia/Kathmandu",        5.75),
    ("DAC", "Dhaka",               "Asia/Dhaka",               6),
    ("RGN", "Yangon",              "Asia/Yangon",            6.5),
    ("BKK", "Bangkok",             "Asia/Bangkok",             7),
    ("HKG", "Hong Kong",           "Asia/Hong_Kong",           8),
    ("EUC", "Eucla",               "Australia/Eucla",       8.75),
    ("TYO", "Tokyo",               "Asia/Tokyo",               9),
    ("ADL", "Adelaide",            "Australia/Adelaide",     9.5),
    ("SYD", "Sydney",              "Australia/Sydney",        10),
    ("LDH", "Lord Howe Island",    "Australia/Lord_Howe",   10.5),
    ("NOU", "Noumea",              "Pacific/Noumea",          11),
    ("WLG", "Wellington",          "Pacific/Auckland",        12),
    ("CHT", "Chatham Islands",     "Pacific/Chatham",      12.75),
    ("TBU", "Nuku'alofa",          "Pacific/Tongatapu",       13),
    ("CXI", "Kiritimati",          "Pacific/Kiritimati",      14),
]
# fmt: on

ZDIR = "/usr/share/zoneinfo/"
OUT = "src/ui_clock_tz.h"
FALLBACK_CODE = "UTC"   # ui_clock.c resolves an unknown persisted code to this


def posix_tz(zone):
    """Return the POSIX TZ string from a TZif v2/v3 footer.

    Layout is 'TZif' + version + v1 block + v2 block + footer, where the footer
    is the POSIX string enclosed in newlines at EOF. Raises on anything
    unexpected -- a wrong table must fail the build, not ship.
    """
    path = ZDIR + zone
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"TZif":
        raise ValueError(f"{zone}: not a TZif file")
    if data[4:5] == b"\0":
        raise ValueError(f"{zone}: TZif v1 has no POSIX footer; need tzdata >= 2004")
    if not data.endswith(b"\n"):
        raise ValueError(f"{zone}: footer is not newline-terminated")
    footer = data[:-1].rsplit(b"\n", 1)[1].decode("ascii")
    if not footer:
        raise ValueError(f"{zone}: empty POSIX footer (zone has no future rule)")
    if '"' in footer or "\\" in footer:
        raise ValueError(f"{zone}: footer needs C escaping: {footer!r}")
    return footer


def tzdata_version():
    """Read the tzdata release from tzdata.zi, e.g. '2026a'. '?' if absent."""
    try:
        with open(ZDIR + "tzdata.zi") as f:
            for line in f:
                if line.startswith("# version"):
                    return line.split()[-1]
    except OSError:
        pass
    return "?"


def std_offset(zone):
    """Standard (non-DST) UTC offset in hours, for cross-checking Casio."""
    import datetime
    import zoneinfo

    tz = zoneinfo.ZoneInfo(zone)
    # Sample both solstices; the smaller |offset| relative to DST is standard.
    # dst() tells us directly which sample is standard time.
    for month in (1, 7):
        t = datetime.datetime(2026, month, 15, 12, 0, tzinfo=datetime.timezone.utc)
        local = t.astimezone(tz)
        if not local.dst():
            return local.utcoffset().total_seconds() / 3600.0
    raise ValueError(f"{zone}: never in standard time at either solstice")


def build():
    """Resolve the table, cross-checking every row. Raises on any problem."""
    rows, codes = [], set()
    for code, name, zone, casio in CITIES:
        if code in codes:
            raise ValueError(f"duplicate city code {code}")
        codes.add(code)
        tz = posix_tz(zone)
        got = std_offset(zone)
        if got != casio:
            raise ValueError(
                f"{code} ({zone}): Casio says UTC{casio:+g}, tzdata says "
                f"UTC{got:+g} -- fix the mapping or update the table"
            )
        rows.append((code, name, tz))
    if FALLBACK_CODE not in codes:
        raise ValueError(f"table has no {FALLBACK_CODE} row; it is the fallback")
    return rows


def render(rows):
    ver = tzdata_version()
    w_code = max(len(r[0]) for r in rows) + 3
    w_name = max(len(r[1]) for r in rows) + 3
    out = [
        "/* Timezone table for the status-bar clock. GENERATED -- DO NOT EDIT.",
        " *",
        " * Regenerate with tools/gen-tz-table.py (needs tzdata on the host) and",
        " * commit the result; the device build has no tzdata. Each rule string is",
        f" * the POSIX TZ footer of the zone's TZif file, from tzdata {ver}.",
        " *",
        " * City set: Casio GA-B2100 / module 5691 World Time table, plus Santiago.",
        " * See the generator for the per-city notes and the rationale.",
        " */",
        "#ifndef EROSQ_UI_CLOCK_TZ_H",
        "#define EROSQ_UI_CLOCK_TZ_H",
        "",
        "struct ui_clock_tz_entry {",
        "    const char *code;   /* Casio 3-letter code; this is what /data persists */",
        "    const char *name;   /* display name for the picker */",
        "    const char *tz;     /* POSIX TZ string, parsed by musl's tzset() */",
        "};",
        "",
        "/* Ordered by standard (winter) UTC offset, -12 to +14, as the watch orders",
        " * it. Order is part of the UI: do not sort by current offset, or rows would",
        " * move twice a year as DST flips. */",
        "static const struct ui_clock_tz_entry ui_clock_tz[] = {",
    ]
    for code, name, tz in rows:
        c = f'"{code}",'.ljust(w_code)
        n = f'"{name}",'.ljust(w_name)
        out.append(f'    {{ {c} {n} "{tz}" }},')
    out += [
        "};",
        "",
        "#define UI_CLOCK_TZ_COUNT ((int)(sizeof ui_clock_tz / sizeof ui_clock_tz[0]))",
        "",
        "#endif",
        "",
    ]
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true",
                    help="verify the committed header is up to date; write nothing")
    args = ap.parse_args()

    # Run from the repo root regardless of cwd.
    os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

    try:
        text = render(build())
    except (OSError, ValueError) as e:
        print(f"gen-tz-table: {e}", file=sys.stderr)
        return 1

    if args.check:
        try:
            with open(OUT) as f:
                cur = f.read()
        except OSError as e:
            print(f"gen-tz-table: {e}", file=sys.stderr)
            return 1
        if cur != text:
            print(f"gen-tz-table: {OUT} is stale; re-run without --check",
                  file=sys.stderr)
            return 1
        print(f"gen-tz-table: {OUT} up to date ({len(CITIES)} cities)")
        return 0

    with open(OUT, "w") as f:
        f.write(text)
    print(f"gen-tz-table: wrote {OUT} ({len(CITIES)} cities, tzdata {tzdata_version()})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
