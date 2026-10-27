/* Timezone table for the status-bar clock. GENERATED -- DO NOT EDIT.
 *
 * Regenerate with tools/gen-tz-table.py (needs tzdata on the host) and
 * commit the result; the device build has no tzdata. Each rule string is
 * the POSIX TZ footer of the zone's TZif file, from tzdata 2026c.
 *
 * City set: Casio GA-B2100 / module 5691 World Time table, plus Santiago.
 * See the generator for the per-city notes and the rationale.
 */
#ifndef EROSQ_UI_CLOCK_TZ_H
#define EROSQ_UI_CLOCK_TZ_H

struct ui_clock_tz_entry {
    const char *code;   /* Casio 3-letter code; this is what /data persists */
    const char *name;   /* display name for the picker */
    const char *tz;     /* POSIX TZ string, parsed by musl's tzset() */
};

/* Ordered by standard (winter) UTC offset, -12 to +14, as the watch orders
 * it. Order is part of the UI: do not sort by current offset, or rows would
 * move twice a year as DST flips. */
static const struct ui_clock_tz_entry ui_clock_tz[] = {
    { "BAR", "Baker Island",        "<-12>12" },
    { "PPG", "Pago Pago",           "SST11" },
    { "HNL", "Honolulu",            "HST10" },
    { "NHV", "Marquesas Islands",   "<-0930>9:30" },
    { "ANC", "Anchorage",           "AKST9AKDT,M3.2.0,M11.1.0" },
    { "LAX", "Los Angeles",         "PST8PDT,M3.2.0,M11.1.0" },
    { "DEN", "Denver",              "MST7MDT,M3.2.0,M11.1.0" },
    { "CHI", "Chicago",             "CST6CDT,M3.2.0,M11.1.0" },
    { "NYC", "New York",            "EST5EDT,M3.2.0,M11.1.0" },
    { "YHZ", "Halifax",             "AST4ADT,M3.2.0,M11.1.0" },
    { "SCL", "Santiago",            "<-04>4<-03>,M9.1.6/24,M4.1.6/24" },
    { "YYT", "St. John's",          "NST3:30NDT,M3.2.0,M11.1.0" },
    { "RIO", "Rio de Janeiro",      "<-03>3" },
    { "FEN", "Fernando de Noronha", "<-02>2" },
    { "RAI", "Praia",               "<-01>1" },
    { "UTC", "UTC",                 "UTC0" },
    { "LON", "London",              "GMT0BST,M3.5.0/1,M10.5.0" },
    { "PAR", "Paris",               "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "ATH", "Athens",              "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "JED", "Jeddah",              "<+03>-3" },
    { "THR", "Tehran",              "<+0330>-3:30" },
    { "DXB", "Dubai",               "<+04>-4" },
    { "KBL", "Kabul",               "<+0430>-4:30" },
    { "KHI", "Karachi",             "PKT-5" },
    { "DEL", "Delhi",               "IST-5:30" },
    { "KTM", "Kathmandu",           "<+0545>-5:45" },
    { "DAC", "Dhaka",               "<+06>-6" },
    { "RGN", "Yangon",              "<+0630>-6:30" },
    { "BKK", "Bangkok",             "<+07>-7" },
    { "HKG", "Hong Kong",           "HKT-8" },
    { "EUC", "Eucla",               "<+0845>-8:45" },
    { "TYO", "Tokyo",               "JST-9" },
    { "ADL", "Adelaide",            "ACST-9:30ACDT,M10.1.0,M4.1.0/3" },
    { "SYD", "Sydney",              "AEST-10AEDT,M10.1.0,M4.1.0/3" },
    { "LDH", "Lord Howe Island",    "<+1030>-10:30<+11>-11,M10.1.0,M4.1.0" },
    { "NOU", "Noumea",              "<+11>-11" },
    { "WLG", "Wellington",          "NZST-12NZDT,M9.5.0,M4.1.0/3" },
    { "CHT", "Chatham Islands",     "<+1245>-12:45<+1345>,M9.5.0/2:45,M4.1.0/3:45" },
    { "TBU", "Nuku'alofa",          "<+13>-13" },
    { "CXI", "Kiritimati",          "<+14>-14" },
};

#define UI_CLOCK_TZ_COUNT ((int)(sizeof ui_clock_tz / sizeof ui_clock_tz[0]))

#endif
