#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "ui_update.h"
#include "update_logic.h"

static int has_swu_ext(const char *name)
{
    size_t n = strlen(name);
    return n > 4 && strcasecmp(name + n - 4, ".swu") == 0;
}

/* Reject anything but a plain, shell-safe basename: this string ends up inside
 * a popen() command line (see ui_update_run), and the SD card is untrusted
 * external media -- a filename crafted with shell metacharacters must never
 * reach system()/popen(). Mirrors the injection-guard precedent in
 * ui_net.c's psk_ok(). Dot is allowed only for the extension/version dots
 * a bundle name normally has. */
static int swu_name_safe(const char *name)
{
    if (name[0] == '.') return 0;   /* no hidden files / no leading "../" */
    for (const char *p = name; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!isalnum(c) && c != '.' && c != '-' && c != '_') return 0;
    }
    return 1;
}

int ui_update_find_swu(char *name, size_t namelen)
{
    DIR *d = opendir(UI_UPDATE_SD_DIR);
    if (!d) return 0;   /* no card / not mounted */
    struct dirent *e;
    char best[256];     /* d_name is at most 255 */
    long long best_mt = 0;
    int found = 0;
    /* Newest by mtime, not readdir's first: the card root collects old bundles
     * and an old one offered with Install focused is one press from overwriting
     * the fallback slot. readdir+stat of ~25 names is cheap; no bundle is opened. */
    while ((e = readdir(d))) {
        if (!has_swu_ext(e->d_name)) continue;
        if (!swu_name_safe(e->d_name)) continue;   /* skip unsafe names, keep scanning */
        if (strlen(e->d_name) >= namelen) continue; /* never offer a truncated name */
        struct stat st;
        if (fstatat(dirfd(d), e->d_name, &st, 0) != 0 || !S_ISREG(st.st_mode)) continue;
        if (ul_swu_better((long long)st.st_mtime, e->d_name, best_mt, found ? best : NULL)) {
            snprintf(best, sizeof best, "%s", e->d_name);
            best_mt = (long long)st.st_mtime;
            found = 1;
        }
    }
    closedir(d);
    if (found) snprintf(name, namelen, "%s", best);
    return found;
}

/* Which slot is currently running, per update_logic.h (ul_running_slot),
 * derived from the live kernel cmdline -- not reimplemented here. */
static void running_slot(char *out, size_t outlen)
{
    char cmdline[512] = "";
    int fd = open("/proc/cmdline", O_RDONLY);
    if (fd >= 0) {
        int n = read(fd, cmdline, sizeof cmdline - 1);
        close(fd);
        if (n > 0) cmdline[n] = 0;
    }
    snprintf(out, outlen, "%s", ul_running_slot(cmdline));
}

/* Positive install detection. swupdate logs "Installing image ... into volume"
 * ONLY when it actually writes a volume, so that line is the reliable "something
 * changed" signal used to gate the reboot. */
static int output_says_installed(const char *out)
{
    return strstr(out, "Installing image") != NULL;
}

/* Running firmware version, from the `firmware` line of /etc/sw-versions (the
 * single composite version the bundle is gated on). Empty out on any failure. */
static void read_firmware_version(char *out, size_t outlen)
{
    if (outlen) out[0] = 0;
    int fd = open("/etc/sw-versions", O_RDONLY);
    if (fd < 0) return;
    char buf[512];
    int n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    ul_sw_version(buf, "firmware", out, outlen);
}

int ui_update_swu_installed(const char *swu_name)
{
    if (!swu_name_safe(swu_name)) return 0;
    char path[320], buf[2048], bv[128], fw[128];
    snprintf(path, sizeof path, UI_UPDATE_SD_DIR "/%s", swu_name);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    ssize_t n = read(fd, buf, sizeof buf);   /* sw-description is the cpio's first member */
    close(fd);
    if (n <= 0 || !ul_swu_version(buf, (size_t)n, bv, sizeof bv)) return 0;
    read_firmware_version(fw, sizeof fw);
    return fw[0] && strcmp(fw, bv) == 0;
}

ui_update_result_t ui_update_run(const char *swu_name, char *msg, size_t msglen)
{
    /* Defense-in-depth: swu_name is interpolated into a popen() shell line. The
     * scan caller already passes only swu_name_safe() names, but re-validate at
     * this public entry point so the injection guard can't be bypassed by a
     * future caller. */
    if (!swu_name_safe(swu_name)) {
        snprintf(msg, msglen, "invalid update filename");
        return UI_UPD_FAILED;
    }

    /* Stop MPD first to free RAM for swupdate (this device's tiny 32MB is
     * tight); "mpdctl stop" is the codebase's established stop path, already
     * used the same way in ui_power.c's ui_power_off(). Ignore failure: no
     * MPD running is not an error here either. */
    if (system("mpdctl stop") == -1) { /* ignore */ }

    char slot[4];
    running_slot(slot, sizeof slot);

    /* Bundle-level version gate: hand swupdate our running firmware version as
     * -R (--no-reinstalling). An identical bundle is rejected in the parser
     * (nothing written, nothing armed) -> "Already up to date"; ANY different
     * version (up OR down) installs BOTH kernel+rootfs into the inactive slot,
     * keeping it coherent. If our version is unreadable/unsafe, omit -R: worst
     * case a redundant reinstall of the same firmware, which is harmless. */
    char fwver[128], rflag[160] = "";
    read_firmware_version(fwver, sizeof fwver);
    if (ul_ver_shell_safe(fwver))
        snprintf(rflag, sizeof rflag, "-R '%s' ", fwver);

    char cmd[640];
    /* swupdate extracts each image to TMPDIR before writing the UBI volume;
     * /tmp is a ~12MB tmpfs (too small for the rootfs image), so point it at the
     * SD card (HW-verified: without this, "not enough free space to extract").
     * -b '0 1 2' blacklists the raw SPI-NOR partitions (mtd0 spl / mtd1 uboot /
     * mtd2 uboot-env) from swupdate's startup UBI scan. Only mtd3 is a UBI
     * device; without this swupdate probes all four and logs three spurious
     * "cannot attach mtdN - maybe not a NAND or raw device" ERROR lines. The
     * layout is fixed by the kernel partition table (see /proc/mtd). */
    snprintf(cmd, sizeof cmd,
             "mkdir -p " UI_UPDATE_SD_DIR "/swu-tmp; "
             "TMPDIR=" UI_UPDATE_SD_DIR "/swu-tmp "
             "swupdate -i " UI_UPDATE_SD_DIR "/%s -k /etc/swupdate/public.pem "
             "-b '0 1 2' "
             "%s-e slotsel,%s 2>&1",
             swu_name, rflag, slot);

    char out[4096] = "";
    size_t used = 0;
    FILE *f = popen(cmd, "r");
    if (!f) {
        snprintf(msg, msglen, "could not start swupdate");
        return UI_UPD_FAILED;
    }
    size_t n;
    char buf[512];
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        if (used < sizeof out - 1) {
            size_t room = sizeof out - 1 - used;
            size_t cp = n < room ? n : room;
            memcpy(out + used, buf, cp);
            used += cp;
        }
        /* keep draining even once `out` is full, so popen's pipe never
         * blocks swupdate on a full buffer */
    }
    out[used] = 0;
    int status = pclose(f);
    int rc = (status >= 0 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;

    if (rc == 0 && output_says_installed(out)) {
        /* A volume was written; swupdate has armed boot_try/upgrade_available for
         * the inactive slot. Caller reboots into it; S99abcommit commits on health. */
        snprintf(msg, msglen, "Update installed");
        return UI_UPD_OK;
    }
    if (ul_reinstall_rejected(out)) {
        /* -R gate: the bundle's version equals ours, so swupdate rejected it in
         * the parser BEFORE touching any volume or bootenv — nothing to undo. */
        snprintf(msg, msglen, "Already up to date");
        return UI_UPD_ALREADY;
    }
    if (rc == 0) {
        /* rc==0 but no "Installing image": with bundle-level gating this should
         * not occur (a same-version bundle is the rc!=0 reject above; a differing
         * one always writes both images). Kept as defense-in-depth: if swupdate
         * ever applied the bootenv without writing, undo the arm and do NOT
         * reboot, so a stale try-boot can't silently switch+commit an unwritten
         * slot. */
        if (system("fw_setenv boot_try; fw_setenv upgrade_available 0; "
                   "fw_setenv bootcount 0") == -1) { /* ignore */ }
        snprintf(msg, msglen, "Already up to date");
        return UI_UPD_ALREADY;
    }
    /* Failure: surface the last non-empty line of swupdate's output as the
     * brief reason, falling back to the exit code if it printed nothing. */
    char *end = out + used;
    while (end > out && (end[-1] == '\n' || end[-1] == '\r')) end--;   /* trim trailing blank lines */
    *end = 0;
    char *line = end;
    while (line > out && line[-1] != '\n') line--;
    if (*line) snprintf(msg, msglen, "%s", line);
    else       snprintf(msg, msglen, "swupdate exited %d", rc);
    return UI_UPD_FAILED;
}

void ui_update_reboot(void)
{
    /* busybox installs the applet at /sbin/reboot (not /usr/sbin) -- same
     * lesson already paid for in ui_power.c's poweroff path. */
    if (system("/sbin/reboot") == -1) { /* ignore */ }
}
