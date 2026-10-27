#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui_mpd.h"
int main(int argc, char **argv)
{
    assert(argc == 3);   /* jpeg path, expected bytes ("0" = expect failure) */
    FILE *f = fopen(argv[1], "rb"); assert(f);
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *ref = malloc(n); assert(fread(ref, 1, n, f) == (size_t)n); fclose(f);
    unsigned char *got = NULL;
    int r = mpd_readpicture("a/b \"quoted\".flac", &got, 2 * 1024 * 1024);
    if (atoi(argv[2]) == 0) { assert(r <= 0 && got == NULL); puts("readpicture: none OK"); return 0; }
    assert(r == n); assert(memcmp(got, ref, n) == 0);
    /* cap smaller than the picture -> refused, no partial buffer */
    unsigned char *g2 = NULL;
    assert(mpd_readpicture("x", &g2, 100) <= 0 && g2 == NULL);
    free(got); free(ref); printf("readpicture: %d bytes OK\n", r); return 0;
}
