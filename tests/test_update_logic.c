/* Host unit test for pure A/B update logic. Build+run:
 *   cc -o /tmp/tul erosq-ui/tests/test_update_logic.c && /tmp/tul && echo PASS */
#include <assert.h>
#include <string.h>
#include "../src/update_logic.h"

int main(void) {
	/* inactive slot = the other one */
	assert(!strcmp(ul_inactive_slot("a"), "b"));
	assert(!strcmp(ul_inactive_slot("b"), "a"));
	assert(!strcmp(ul_inactive_slot(""), "b"));   /* default/committed-A */

	/* running slot from cmdline (asymmetric: rootfs_b => b, else a) */
	assert(!strcmp(ul_running_slot("...ubi.block=0,rootfs_b root=/dev/ubiblock0_4"), "b"));
	assert(!strcmp(ul_running_slot("...ubi.block=0,rootfs root=/dev/ubiblock0_1"), "a"));

	/* install-if-different: differ up, down, and skip same */
	assert(ul_ver_differs("1.0.0", "1.1.0"));   /* upgrade */
	assert(ul_ver_differs("1.1.0", "1.0.0"));   /* downgrade allowed */
	assert(!ul_ver_differs("1.0.0", "1.0.0"));  /* same -> skip */

	/* commit only while an update is pending */
	assert(ul_should_commit(1) == 1);
	assert(ul_should_commit(0) == 0);

	/* ul_sw_version: extract the firmware line from /etc/sw-versions text */
	{
		const char *sv = "firmware e255483+6.18.37-g9c28240d1e\n"
		                 "kernel 6.18.37-g9c28240d1e\n"
		                 "rootfs e255483\n";
		char v[128];
		assert(ul_sw_version(sv, "firmware", v, sizeof v) &&
		       !strcmp(v, "e255483+6.18.37-g9c28240d1e"));
		assert(ul_sw_version(sv, "kernel", v, sizeof v) &&
		       !strcmp(v, "6.18.37-g9c28240d1e"));
		assert(ul_sw_version(sv, "rootfs", v, sizeof v) && !strcmp(v, "e255483"));
		/* prefix must not match a longer name (rootfs vs rootfs_b) */
		const char *sv2 = "rootfs_b 999\nrootfs abc\n";
		assert(ul_sw_version(sv2, "rootfs", v, sizeof v) && !strcmp(v, "abc"));
		/* tab separator + trailing CR trimmed */
		char v2[128];
		assert(ul_sw_version("firmware\tX.Y\r\n", "firmware", v2, sizeof v2) &&
		       !strcmp(v2, "X.Y"));
		/* absent key -> 0, out emptied */
		assert(!ul_sw_version(sv, "nope", v, sizeof v) && v[0] == 0);
		/* no crash on NULLs */
		assert(!ul_sw_version(NULL, "firmware", v, sizeof v));
	}

	/* ul_reinstall_rejected: detect swupdate's -R parser reject */
	assert(ul_reinstall_rejected("... No reinstalling allowed: new version X == installed X"));
	assert(!ul_reinstall_rejected("Installing image erosq.itb into volume kernel_b"));
	assert(!ul_reinstall_rejected(""));
	assert(!ul_reinstall_rejected(NULL));

	/* ul_ver_shell_safe: accept real version strings, reject shell metachars */
	assert(ul_ver_shell_safe("e255483+6.18.37-g9c28240d1e"));
	assert(ul_ver_shell_safe("1.2.3"));
	assert(!ul_ver_shell_safe(""));            /* empty */
	assert(!ul_ver_shell_safe(NULL));
	assert(!ul_ver_shell_safe("a;rm -rf /"));  /* space + ; */
	assert(!ul_ver_shell_safe("a'b"));         /* quote */
	assert(!ul_ver_shell_safe("a$b"));         /* expansion */

	/* ul_swu_better: newest mtime wins regardless of scan order; ties -> larger name */
	{
		struct { long long mt; const char *n; } f[] = {
			{ 1754352000LL, "erosq-charge-20260805.swu" },   /* August, scanned first */
			{ 1760572800LL, "erosq-settings1b.swu" },        /* newest */
			{ 1757000000LL, "erosq-ntp.swu" },
		};
		const char *best = NULL; long long bmt = 0;
		for (int k = 0; k < 3; k++)
			if (ul_swu_better(f[k].mt, f[k].n, bmt, best)) { best = f[k].n; bmt = f[k].mt; }
		assert(!strcmp(best, "erosq-settings1b.swu"));
		/* reverse scan order -> same pick */
		best = NULL; bmt = 0;
		for (int k = 2; k >= 0; k--)
			if (ul_swu_better(f[k].mt, f[k].n, bmt, best)) { best = f[k].n; bmt = f[k].mt; }
		assert(!strcmp(best, "erosq-settings1b.swu"));
		assert(ul_swu_better(5, "a.swu", 0, NULL));          /* first candidate always taken */
		assert(ul_swu_better(-1, "a.swu", 0, NULL));         /* even a pre-1970 mtime */
		assert(!ul_swu_better(5, "b.swu", 6, "a.swu"));      /* older loses */
		assert(ul_swu_better(6, "b.swu", 6, "a.swu"));       /* tie -> larger name */
		assert(!ul_swu_better(6, "a.swu", 6, "b.swu"));
		assert(!ul_swu_better(6, "a.swu", 6, "a.swu"));      /* same entry is not "better" */
	}

	/* ul_swu_version: top-level version out of a raw cpio head (NUL-padded name) */
	{
		static const char head[] =
			"070702016C6826000081B4000003E8000003E8000000016AC15DAA000005BC0000000000000036"
			"00000000000000000000000F00017E0Fsw-description\0\0\0"
			"software =\n{\n    \"version\": \"a92121b+6.18.53-g175045e3d8+uijazz-74-g664096c\",\n"
			"    b: { images: ( { \"version\": \"6.18.53-g175045e3d8\" } ) }\n";
		char v[128];
		assert(ul_swu_version(head, sizeof head - 1, v, sizeof v) &&
		       !strcmp(v, "a92121b+6.18.53-g175045e3d8+uijazz-74-g664096c"));
		/* truncated before the closing quote -> 0, emptied */
		size_t cut = 0;   /* byte scan: strstr stops at the cpio NUL padding */
		while (memcmp(head + cut, "a92121b", 7)) cut++;
		cut += 5;
		assert(!ul_swu_version(head, cut, v, sizeof v) && v[0] == 0);
		/* out too small -> 0 (never a silently truncated version that could match) */
		char small[8];
		assert(!ul_swu_version(head, sizeof head - 1, small, sizeof small));
		/* "version" before sw-description (not a bundle head) -> 0 */
		static const char junk[] = "\"version\": \"x\" sw-description";
		assert(!ul_swu_version(junk, sizeof junk - 1, v, sizeof v));
		assert(!ul_swu_version("", 0, v, sizeof v));
		assert(!ul_swu_version(NULL, 10, v, sizeof v));
		/* no sw-description at all, even in a buffer too short to hold the name -> 0 */
		assert(!ul_swu_version("\"version\":\"x\"", 13, v, sizeof v));
		/* version exactly outlen long -> 0 (no room for the NUL; no 1-byte overflow) */
		static const char eight[] = "sw-description\"version\": \"abcdefgh\"";
		char o8[8], o9[9];
		assert(!ul_swu_version(eight, sizeof eight - 1, o8, sizeof o8));
		assert(ul_swu_version(eight, sizeof eight - 1, o9, sizeof o9) && !strcmp(o9, "abcdefgh"));
		/* unquoted value -> 0, not the text up to some later quote */
		static const char unq[] = "sw-description\"version\": 1.0, \"x\": \"y\"";
		assert(!ul_swu_version(unq, sizeof unq - 1, v, sizeof v));
		/* empty version string -> 0 */
		static const char empty[] = "sw-description\0\"version\": \"\"";
		assert(!ul_swu_version(empty, sizeof empty - 1, v, sizeof v));
	}

	return 0;
}
