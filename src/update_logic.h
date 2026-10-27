/* Pure A/B update decision logic — dependency-free, host-testable.
 * Used by the UI update screen (ui_update.c) and mirrored by the shell
 * commit script (S99abcommit). See docs .../2026-07-12-firmware-update-swupdate-design.md. */
#ifndef EROSQ_UPDATE_LOGIC_H
#define EROSQ_UPDATE_LOGIC_H
#include <string.h>

/* The slot to install INTO = the one not currently committed/running. */
static inline const char *ul_inactive_slot(const char *boot_slot) {
	return (boot_slot && boot_slot[0] == 'b') ? "a" : "b";
}

/* Which slot is running, derived from the kernel cmdline (asymmetric layout:
 * slot B roots on rootfs_b, slot A on plain rootfs). */
static inline const char *ul_running_slot(const char *cmdline) {
	return (cmdline && strstr(cmdline, "rootfs_b")) ? "b" : "a";
}

/* Bundle-level "install if different": the update is gated by swupdate's -R
 * (--no-reinstalling) against the running firmware version, not per-component.
 * A bundle whose version differs (up OR down) installs; identical is rejected.
 * This helper documents that comparison and is used by the host tests. */
static inline int ul_ver_differs(const char *installed, const char *bundle) {
	if (!installed || !bundle) return 1;
	return strcmp(installed, bundle) != 0;
}

/* Extract the value of `key` from /etc/sw-versions-style text: lines of
 * "<name><ws><version>". Matches only at a line start with `key` followed by
 * whitespace (so "rootfs" never matches a "rootfs_b" line). Writes the value
 * (trimmed of trailing CR/LF) to out and returns 1 on hit; returns 0 and leaves
 * out empty otherwise. Dependency-free (no fscanf/strtok state). */
static inline int ul_sw_version(const char *contents, const char *key,
                                char *out, size_t outlen) {
	if (out && outlen) out[0] = 0;
	if (!contents || !key || !out || !outlen) return 0;
	size_t klen = strlen(key);
	for (const char *p = contents; p && *p; ) {
		if (strncmp(p, key, klen) == 0 && (p[klen] == ' ' || p[klen] == '\t')) {
			const char *v = p + klen;
			while (*v == ' ' || *v == '\t') v++;
			size_t i = 0;
			while (v[i] && v[i] != '\n' && v[i] != '\r' && i + 1 < outlen) {
				out[i] = v[i]; i++;
			}
			out[i] = 0;
			return i > 0;
		}
		const char *nl = strchr(p, '\n');
		p = nl ? nl + 1 : NULL;
	}
	return 0;
}

/* swupdate rejects a same-version bundle (our -R gate) in the parser with the
 * line "No reinstalling allowed: ...". Detect it so the UI reports "up to date"
 * instead of a scary failure. The reject happens BEFORE any volume/bootenv is
 * touched, so there is nothing to undo. */
static inline int ul_reinstall_rejected(const char *out) {
	return out && strstr(out, "No reinstalling allowed") != NULL;
}

/* A firmware version string is safe to interpolate into a shell command line iff
 * it contains only the characters git-describe / the kernel release produce
 * (alnum . - _ +). Guards the -R argument in ui_update.c's popen() line. */
static inline int ul_ver_shell_safe(const char *v) {
	if (!v || !v[0]) return 0;
	for (const char *p = v; *p; p++) {
		char c = *p;
		int ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
		         (c >= 'A' && c <= 'Z') ||
		         c == '.' || c == '-' || c == '_' || c == '+';
		if (!ok) return 0;
	}
	return 1;
}

/* Which of several *.swu on the card to offer: the newest by mtime. Bundle
 * versions are git-describe composites (787bd0e+6.18.53-g83...), not orderable,
 * so mtime is the only "newest" there is. Equal mtimes -> the larger name, so
 * the pick is deterministic (not readdir order) and date-stamped names win
 * newest-last. 1 iff candidate (mt, name) beats the current best; best == NULL
 * means none yet. */
static inline int ul_swu_better(long long mt, const char *name,
                                long long best_mt, const char *best) {
	if (!best) return 1;
	if (mt != best_mt) return mt > best_mt;
	return strcmp(name, best) > 0;
}

/* Bundle version from the first `len` bytes of a .swu. The cpio's first member
 * is sw-description (swupdate requires it), and its first "version" key is the
 * top-level bundle version the -R gate compares (tools-erosq/sw-description.in).
 * Byte scan, not strstr: the cpio header pads the member name with NULs.
 * 1 + out filled on success; 0 + out emptied if absent/truncated/too long. */
static inline int ul_swu_version(const char *buf, size_t len, char *out, size_t outlen) {
	static const char nm[] = "sw-description", key[] = "\"version\"";
	size_t i = 0, n;
	if (outlen) out[0] = 0;
	if (!buf || !out || !outlen) return 0;
	for (; i + sizeof nm - 1 <= len; i++) if (!memcmp(buf + i, nm, sizeof nm - 1)) break;
	if (i + sizeof nm - 1 > len) return 0;
	for (; i + sizeof key - 1 <= len; i++) if (!memcmp(buf + i, key, sizeof key - 1)) break;
	if (i + sizeof key - 1 > len) return 0;
	for (i += sizeof key - 1; i < len && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == ':'); i++) {}
	if (i >= len || buf[i++] != '"') return 0;
	for (n = 0; i + n < len && buf[i + n] != '"'; n++) {}
	if (i + n >= len || n == 0 || n >= outlen) return 0;
	memcpy(out, buf + i, n);
	out[n] = 0;
	return 1;
}

/* Commit a try-boot only when an update is pending (upgrade_available==1). */
static inline int ul_should_commit(int upgrade_available) {
	return upgrade_available == 1;
}

#endif
