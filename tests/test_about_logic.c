#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/about_logic.h"

int main(void) {
    char b[32];

    /* battery: unavailable node (-1) must not render as "0%  0.00V" */
    al_batt(b, sizeof b, -1, 0);    assert(strcmp(b, "--") == 0);
    al_batt(b, sizeof b, -1, 3900); assert(strcmp(b, "--") == 0);
    al_batt(b, sizeof b, 101, 3900); assert(strcmp(b, "--") == 0);
    al_batt(b, sizeof b, 85, 3920); assert(strcmp(b, "85%  3.92V") == 0);
    al_batt(b, sizeof b, 0, 3301);  assert(strcmp(b, "0%  3.30V") == 0);  /* real 0% is shown */
    al_batt(b, sizeof b, 100, 4205); assert(strcmp(b, "100%  4.20V") == 0);
    al_batt(b, sizeof b, 50, 0);    assert(strcmp(b, "50%") == 0);        /* voltage unreadable */
    al_batt(b, 4, 85, 3920);        assert(strlen(b) == 3);               /* truncates, no overrun */

    /* free space */
    al_free(b, sizeof b, 0);                          assert(strcmp(b, "0 MB") == 0);
    al_free(b, sizeof b, (1ULL << 20) * 512);         assert(strcmp(b, "512 MB") == 0);
    al_free(b, sizeof b, (1ULL << 20) * 1023);        assert(strcmp(b, "1023 MB") == 0);
    al_free(b, sizeof b, 1ULL << 30);                 assert(strcmp(b, "1.0 GB") == 0);
    al_free(b, sizeof b, (1ULL << 30) * 3 / 2);       assert(strcmp(b, "1.5 GB") == 0);
    al_free(b, sizeof b, (1ULL << 30) * 2 - 1);       assert(strcmp(b, "1.9 GB") == 0);  /* truncated */
    al_free(b, sizeof b, (1ULL << 30) * 237);         assert(strcmp(b, "237.0 GB") == 0);
    al_free(b, sizeof b, (1ULL << 40) * 2);           assert(strcmp(b, "2048.0 GB") == 0);

    /* brightness percent over the slider's actual PWM levels */
    assert(al_bl_pct(-5)  == 0);
    assert(al_bl_pct(0)   == 0);
    assert(al_bl_pct(39)  == 15);   /* slider floor */
    assert(al_bl_pct(128) == 50);
    assert(al_bl_pct(207) == 81);
    assert(al_bl_pct(254) == 100);
    assert(al_bl_pct(255) == 100);
    assert(al_bl_pct(999) == 100);

    printf("about_logic: ok\n");
    return 0;
}
