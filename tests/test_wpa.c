#include <assert.h>
#include <string.h>
#include "ui_net.h"
int main(void)
{
    const char *r =
      "bssid / frequency / signal level / flags / ssid\n"
      "00:11:22:33:44:55\t2412\t-42\t[WPA2-PSK-CCMP][ESS]\tHomeNet\n"
      "66:77:88:99:aa:bb\t2437\t-70\t[ESS]\tCafeOpen\n";
    ui_wifi_ap_t ap[8];
    int n = wpa_parse_scan(r, ap, 8);
    assert(n == 2);
    assert(strcmp(ap[0].ssid, "HomeNet") == 0 && ap[0].secured == 1);
    assert(ap[0].signal == -42);
    assert(strcmp(ap[1].ssid, "CafeOpen") == 0 && ap[1].secured == 0);

    /* WEP flags also count as secured (code checks both "WPA" and "WEP") */
    const char *wep =
      "hdr\n11:22:33:44:55:66\t2412\t-50\t[WEP][ESS]\tOldRouter\n";
    assert(wpa_parse_scan(wep, ap, 8) == 1 && ap[0].secured == 1);

    /* header-only (no data rows) -> 0; input with no newline at all -> 0 */
    assert(wpa_parse_scan("bssid / ... / ssid\n", ap, 8) == 0);
    assert(wpa_parse_scan("no-newline-anywhere", ap, 8) == 0);

    /* rows with fewer than 5 fields are skipped; a blank SSID field is dropped */
    const char *bad =
      "hdr\ntoo\tfew\tfields\n"                      /* only 4 fields -> skip */
      "aa:bb:cc:dd:ee:ff\t2412\t-40\t[ESS]\t\n"       /* empty ssid -> dropped */
      "aa:bb:cc:dd:ee:00\t2412\t-40\t[ESS]\tReal\n";  /* valid */
    assert(wpa_parse_scan(bad, ap, 8) == 1 && strcmp(ap[0].ssid, "Real") == 0);

    /* SSID longer than 32 bytes is truncated to 32 (buffer safety) */
    const char *longssid =
      "hdr\naa:bb:cc:dd:ee:11\t2412\t-40\t[ESS]\t"
      "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789\n";        /* 36 chars */
    assert(wpa_parse_scan(longssid, ap, 8) == 1);
    assert(strlen(ap[0].ssid) == 32);

    /* max cap: two valid rows, room for one -> returns 1 */
    const char *two =
      "hdr\naa:bb:cc:dd:ee:22\t2412\t-40\t[ESS]\tOne\n"
      "aa:bb:cc:dd:ee:33\t2412\t-40\t[ESS]\tTwo\n";
    assert(wpa_parse_scan(two, ap, 1) == 1);

    /* security path: ssid hex-encoding + passphrase validation */
    char hex[67]; to_hex("HomeNet", hex);
    assert(strcmp(hex, "486f6d654e6574") == 0);
    assert(psk_ok("password"));                 /* 8 chars, ok */
    assert(!psk_ok("short"));                    /* < 8 */
    assert(!psk_ok("has\"quote_in_it_here"));    /* embedded quote -> reject */
    assert(!psk_ok("back\\slash_in_it_too"));    /* embedded backslash -> reject */

    /* length boundaries: 63 printable ok, 64 rejected */
    char p63[64], p64[65];
    memset(p63, 'a', 63); p63[63] = 0;
    memset(p64, 'a', 64); p64[64] = 0;
    assert(psk_ok(p63));
    assert(!psk_ok(p64));

    /* signal words: thresholds are inclusive at -60 / -72 */
    assert(!strcmp(wifi_strength(-40), "Strong"));
    assert(!strcmp(wifi_strength(-60), "Strong"));
    assert(!strcmp(wifi_strength(-61), "Good"));
    assert(!strcmp(wifi_strength(-72), "Good"));
    assert(!strcmp(wifi_strength(-73), "Weak"));

    /* non-printable bytes rejected: below 0x20 (tab) and above 0x7e (DEL) */
    assert(!psk_ok("has\ttab_in_it_here"));      /* 0x09 control */
    assert(!psk_ok("has\x7f_del_in_it_here"));   /* 0x7f above printable */
    assert(psk_ok("with spaces work ok"));       /* 0x20 space is printable */

    /* printf_encode unescaping: SSIDs arrive ESCAPED on the ctrl interface.
     * \xHH -> raw byte (so to_hex on the result targets the real SSID) */
    char un[33];
    const char *utf8 = "Caf\\xc3\\xa9";               /* "Café" escaped */
    assert(wpa_unescape(utf8, utf8 + strlen(utf8), un, 32) == 5);
    assert(memcmp(un, "Caf\xc3\xa9", 6) == 0);        /* raw UTF-8 + NUL */
    /* named escapes + escaped backslash/quote */
    const char *named = "a\\tb\\nc\\\\d\\\"e";
    assert(wpa_unescape(named, named + strlen(named), un, 32) == 9);
    assert(memcmp(un, "a\tb\nc\\d\"e", 10) == 0);
    /* \e (ESC), single-digit hex, trailing lone backslash, unknown escape */
    const char *odd = "\\e\\x5\\q\\";
    (void)wpa_unescape(odd, odd + strlen(odd), un, 32);
    assert(un[0] == 0x1b && un[1] == 0x05 && un[2] == 'q' && un[3] == '\\' && un[4] == 0);
    /* cap enforced: decoded output truncates at cap bytes */
    const char *longesc = "\\x41\\x42\\x43\\x44";     /* "ABCD" */
    assert(wpa_unescape(longesc, longesc + strlen(longesc), un, 2) == 2);
    assert(strcmp(un, "AB") == 0);

    /* end-to-end: an escaped-UTF-8 SSID in SCAN_RESULTS parses to raw bytes */
    const char *esc =
      "hdr\naa:bb:cc:dd:ee:44\t2412\t-40\t[WPA2-PSK-CCMP][ESS]\tCaf\\xc3\\xa9\n";
    assert(wpa_parse_scan(esc, ap, 8) == 1);
    assert(strcmp(ap[0].ssid, "Caf\xc3\xa9") == 0 && ap[0].secured == 1);
    return 0;
}
