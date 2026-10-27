#include <assert.h>
#include <string.h>
#include "ui_launch.h"
int main(void)
{
    const char *t = "# comment\nBars Demo\t/usr/bin/drmtest\tUtilities\n\nNES\t/usr/bin/infones\tGames\n";
    ui_app_t a[8];
    int n = ui_manifest_parse(t, a, 8);
    assert(n == 2);
    assert(strcmp(a[0].name, "Bars Demo") == 0);
    assert(strcmp(a[0].exec, "/usr/bin/drmtest") == 0);
    assert(strcmp(a[0].cat,  "Utilities") == 0);
    assert(strcmp(a[1].cat,  "Games") == 0);

    /* last entry has no trailing newline -> still parsed */
    int n2 = ui_manifest_parse("App\t/bin/app\tCat", a, 8);
    assert(n2 == 1 && strcmp(a[0].name, "App") == 0 && strcmp(a[0].cat, "Cat") == 0);

    /* a line missing the second tab (only 2 fields) is skipped */
    int n3 = ui_manifest_parse("Broken\t/bin/x\nGood\t/bin/y\tCat\n", a, 8);
    assert(n3 == 1 && strcmp(a[0].name, "Good") == 0);

    /* max cap: two valid rows, room for one -> returns 1 */
    int n4 = ui_manifest_parse("A\t/bin/a\tC\nB\t/bin/b\tC\n", a, 1);
    assert(n4 == 1 && strcmp(a[0].name, "A") == 0);

    /* over-long name is truncated to fit name[32] (31 chars + NUL) */
    int n5 = ui_manifest_parse(
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789\t/bin/z\tC\n", a, 8);
    assert(n5 == 1 && strlen(a[0].name) == 31);
    return 0;
}
